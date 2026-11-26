#include "audio_preferences.hpp"
#include "vlc_config.hpp"

#include <algorithm>
#include <cmath>
#include <locale>
#include <sstream>
#include <string_view>

struct Equalizer;

extern "C" {
__declspec(dllimport) void playlist_EnableAudioFilter(Playlist *, const char *, bool);
__declspec(dllimport) std::ptrdiff_t config_GetPszChoices(void *, const char *, char ***, char ***);
__declspec(dllimport) Equalizer *libvlc_audio_equalizer_new_from_preset(unsigned);
__declspec(dllimport) void libvlc_audio_equalizer_release(Equalizer *);
__declspec(dllimport) float libvlc_audio_equalizer_get_preamp(Equalizer *);
__declspec(dllimport) unsigned libvlc_audio_equalizer_get_band_count();
__declspec(dllimport) float libvlc_audio_equalizer_get_amp_at_index(Equalizer *, unsigned);
}

namespace audio_preferences {
namespace {

constexpr char filter_key[] = "audio-filter", stereo_key[] = "stereo-mode", delay_key[] = "audio-delay";
constexpr char equalizer[] = "equalizer", preset_key[] = "equalizer-preset", bands_key[] = "equalizer-bands";
constexpr char preamp_key[] = "equalizer-preamp", two_pass_key[] = "equalizer-2pass";
constexpr char pitch_key[] = "pitch-shift", pitch_filter[] = "scaletempo_pitch";
constexpr char widener_filter[] = "stereo_widen";
constexpr const char *filters[] = {"compressor", "spatializer", widener_filter};
constexpr double ticks_per_second = 1000000;

InputRef audio_input(VlcPlayer player) {
    InputRef input(playlist_CurrentInput(player.playlist), vlc_object_release);
    VlcValue track{};
    if (!active_playback(input.get()) || var_Get(input.get(), "audio-es", &track) != 0 || track.integer < 0)
        input.reset();
    return input;
}

std::string string_option(VlcPlayer player, AudioOutput *output, const char *key) {
    VlcValue value{};
    if (output && var_Get(output, key, &value) == 0)
        return vlc_config::take_string(value.string);
    return vlc_config::take_string(config_GetPsz(player.vlc, key));
}

std::size_t filter_position(std::string_view chain, std::string_view target) {
    std::size_t position = 0;
    while (!chain.empty()) {
        const auto separator = chain.find(':');
        if (chain.substr(0, separator) == target)
            return position;
        if (separator == chain.npos)
            break;
        const auto advance = separator + 1;
        position += advance;
        chain.remove_prefix(advance);
    }
    return chain.npos;
}

bool contains_filter(std::string_view chain, std::string_view target) {
    return filter_position(chain, target) != chain.npos;
}

bool preset_filter(std::string_view name, std::string_view) {
    return name == equalizer || name == pitch_filter ||
           std::find(std::begin(filters), std::end(filters), name) != std::end(filters);
}

void enable_filter(VlcPlayer player, const char *filter, bool enabled) {
    playlist_EnableAudioFilter(player.playlist, filter, enabled);
    const AudioOutputRef output(playlist_GetAout(player.playlist), vlc_object_release);
    if (output) {
        const auto chain = string_option(player, output.get(), filter_key);
        config_PutPsz(player.vlc, filter_key, chain.c_str());
    }
}

void set_string(AudioOutput *output, const char *key, std::string value) {
    if (!output)
        return;
    VlcValue data{};
    data.string = value.data();
    var_Set(output, key, data);
}

void set_float(AudioOutput *output, const char *key, float value) {
    if (!output)
        return;
    VlcValue data{};
    data.real = value;
    var_Set(output, key, data);
}

float gain(float value, float fallback) {
    return std::isfinite(value) ? std::clamp(value, float(equalizer_min_db), float(equalizer_max_db)) : fallback;
}

float filter_value(const FilterParameter &parameter, float value) {
    return std::isfinite(value) ? std::clamp(value, parameter.minimum, parameter.maximum) : parameter.initial;
}

template<std::size_t N>
std::array<float, N> read_filter_values(VlcPlayer player, AudioOutput *output, const FilterParameter (&parameters)[N]) {
    std::array<float, N> values{};
    for (std::size_t i = 0; i < values.size(); ++i) {
        const auto &parameter = parameters[i];
        VlcValue live{};
        const float value = !parameter.reload && output && var_Get(output, parameter.key, &live) == 0
            ? live.real : config_GetFloat(player.vlc, parameter.key);
        values[i] = filter_value(parameter, value);
    }
    return values;
}

void write_gains(VlcPlayer player, AudioOutput *output, const EqualizerGains &gains) {
    std::ostringstream bands;
    bands.imbue(std::locale::classic());
    for (std::size_t i = 1; i < gains.size(); ++i) {
        if (i > 1)
            bands << ' ';
        bands << gains[i];
    }
    const auto values = bands.str();
    config_PutPsz(player.vlc, bands_key, values.c_str());
    config_PutFloat(player.vlc, preamp_key, gains[0]);
    set_string(output, bands_key, values);
    set_float(output, preamp_key, gains[0]);
}

}

Settings read(VlcPlayer player, const std::vector<Preset> &presets) {
    VlcValue pitch{}, two_pass{};
    const AudioOutputRef output(playlist_GetAout(player.playlist), vlc_object_release);
    const auto chain = string_option(player, output.get(), filter_key);
    const auto key = string_option(player, output.get(), preset_key);
    const auto preset = std::find_if(presets.begin(), presets.end(), [&](const Preset &entry) { return entry.key == key; });
    auto gains = preset == presets.end() ? EqualizerGains{12.0f} : preset->gains;
    VlcValue preamp{};
    gains[0] = gain(output && var_Get(output.get(), preamp_key, &preamp) == 0
                   ? preamp.real : config_GetFloat(player.vlc, preamp_key), gains[0]);
    std::istringstream bands(string_option(player, output.get(), bands_key));
    bands.imbue(std::locale::classic());
    for (std::size_t i = 1; i < gains.size(); ++i) {
        float value = 0;
        if (!(bands >> value))
            break;
        gains[i] = gain(value, gains[i]);
    }
    std::string selection;
    if (contains_filter(chain, equalizer)) {
        const bool matches = preset != presets.end() &&
            std::equal(gains.begin(), gains.end(), preset->gains.begin(),
                       [](float a, float b) { return std::abs(a - b) < 0.005f; });
        selection = matches ? key : "custom";
    }
    return {
        delay(player),
        static_cast<int>(config_GetInt(player.vlc, stereo_key)),
        std::move(selection),
        gains,
        output && var_Get(output.get(), two_pass_key, &two_pass) == 0
            ? two_pass.boolean : config_GetInt(player.vlc, two_pass_key) != 0,
        contains_filter(chain, filters[0]), contains_filter(chain, filters[1]), contains_filter(chain, filters[2]),
        contains_filter(chain, pitch_filter)
            ? (output && var_Get(output.get(), pitch_key, &pitch) == 0 ? pitch.real : config_GetFloat(player.vlc, pitch_key))
            : 0.0f,
        read_filter_values(player, output.get(), compressor_parameters),
        read_filter_values(player, output.get(), spatializer_parameters),
        read_filter_values(player, output.get(), widener_parameters),
    };
}

std::optional<double> delay(VlcPlayer player) {
    const auto input = audio_input(player);
    VlcValue value{};
    if (input && var_Get(input.get(), delay_key, &value) == 0)
        return value.integer / ticks_per_second;
    return std::nullopt;
}

std::vector<Preset> equalizer_presets(VlcPlayer player) {
    char **values = nullptr, **labels = nullptr;
    const auto count = config_GetPszChoices(player.vlc, preset_key, &values, &labels);
    std::vector<Preset> presets;
    unsigned index = 0;
    for (std::ptrdiff_t i = 0; i < count; ++i) {
        const auto value = vlc_config::take_string(values[i]);
        const auto label = vlc_config::take_string(labels[i]);
        if (value.empty())
            continue;
        const std::unique_ptr<Equalizer, decltype(&libvlc_audio_equalizer_release)>
            settings(libvlc_audio_equalizer_new_from_preset(index++), libvlc_audio_equalizer_release);
        if (!settings)
            continue;
        EqualizerGains gains{};
        gains[0] = libvlc_audio_equalizer_get_preamp(settings.get());
        const auto count = std::min<std::size_t>(libvlc_audio_equalizer_get_band_count(), gains.size() - 1);
        for (unsigned band = 0; band < count; ++band)
            gains[band + 1] = libvlc_audio_equalizer_get_amp_at_index(settings.get(), band);
        presets.push_back({value, label.empty() ? value : label, gains});
    }
    libvlc_free(values);
    libvlc_free(labels);
    return presets;
}

void set_stereo_mode(VlcPlayer player, int mode) {
    config_PutInt(player.vlc, stereo_key, mode);
    const AudioOutputRef output(playlist_GetAout(player.playlist), vlc_object_release);
    if (output)
        var_Set(output.get(), stereo_key, VlcValue{mode});
}

bool set_delay(VlcPlayer player, double seconds) {
    const auto input = audio_input(player);
    if (!input || !std::isfinite(seconds))
        return false;
    const auto ticks = std::llround(std::clamp(seconds, -10.0, 10.0) * ticks_per_second);
    return var_Set(input.get(), delay_key, VlcValue{ticks}) == 0;
}

void set_equalizer(VlcPlayer player, const Preset *preset) {
    if (preset) {
        config_PutPsz(player.vlc, preset_key, preset->key.c_str());
        const AudioOutputRef output(playlist_GetAout(player.playlist), vlc_object_release);
        set_string(output.get(), preset_key, preset->key);
        write_gains(player, output.get(), preset->gains);
    }
    enable_filter(player, equalizer, preset != nullptr);
}

void set_equalizer_gains(VlcPlayer player, const EqualizerGains &gains) {
    const AudioOutputRef output(playlist_GetAout(player.playlist), vlc_object_release);
    write_gains(player, output.get(), gains);
    if (!contains_filter(string_option(player, output.get(), filter_key), equalizer))
        enable_filter(player, equalizer, true);
}

void set_two_pass(VlcPlayer player, bool enabled) {
    config_PutInt(player.vlc, two_pass_key, enabled);
    const AudioOutputRef output(playlist_GetAout(player.playlist), vlc_object_release);
    if (output) {
        VlcValue value{};
        value.boolean = enabled;
        var_Set(output.get(), two_pass_key, value);
    }
}

void set_filter(VlcPlayer player, Filter filter, bool enabled) {
    enable_filter(player, filters[static_cast<unsigned>(filter)], enabled);
}

void set_filter_value(VlcPlayer player, const FilterParameter &parameter, float value) {
    const auto bounded = filter_value(parameter, value);
    config_PutFloat(player.vlc, parameter.key, bounded);
    if (parameter.reload)
        return;
    const AudioOutputRef output(playlist_GetAout(player.playlist), vlc_object_release);
    set_float(output.get(), parameter.key, bounded);
}

void reload_widener(VlcPlayer player) {
    const AudioOutputRef output(playlist_GetAout(player.playlist), vlc_object_release);
    if (!output)
        return;
    const auto chain = string_option(player, output.get(), filter_key);
    const auto position = filter_position(chain, widener_filter);
    if (position == chain.npos)
        return;

    // Reload the filter on the audio thread. VLC's delay callback resizes its live buffer,
    // and its crossfeed callback updates feedback. Restore the original filter order.
    auto temporary = chain;
    temporary.erase(position == 0 ? 0 : position - 1, std::size(widener_filter));
    set_string(output.get(), filter_key, std::move(temporary));
    set_string(output.get(), filter_key, chain);
}

void set_pitch(VlcPlayer player, float semitones) {
    const auto value = std::clamp(semitones, -12.0f, 12.0f);
    config_PutFloat(player.vlc, pitch_key, value);
    const AudioOutputRef output(playlist_GetAout(player.playlist), vlc_object_release);
    set_float(output.get(), pitch_key, value);
    const auto chain = string_option(player, output.get(), filter_key);
    if (contains_filter(chain, pitch_filter) != (value != 0))
        enable_filter(player, pitch_filter, value != 0);
}

void append_preset_keys(std::vector<const char *> &keys) {
    keys.insert(keys.end(), {stereo_key, preset_key, bands_key, preamp_key, two_pass_key, pitch_key, "audio-desync"});
    for (const auto &parameter : compressor_parameters)
        keys.push_back(parameter.key);
    for (const auto &parameter : spatializer_parameters)
        keys.push_back(parameter.key);
    for (const auto &parameter : widener_parameters)
        keys.push_back(parameter.key);
}

std::string preset_filters(VlcPlayer player) {
    const AudioOutputRef output(playlist_GetAout(player.playlist), vlc_object_release);
    return vlc_config::split_filters(string_option(player, output.get(), filter_key), preset_filter).selected;
}

void apply_preset(VlcPlayer player, std::string_view selected) {
    const AudioOutputRef output(playlist_GetAout(player.playlist), vlc_object_release);
    const auto configured = string_option(player, nullptr, filter_key);
    const auto chain = vlc_config::replace_filters(configured, selected, preset_filter);
    if (chain != configured)
        config_PutPsz(player.vlc, filter_key, chain.c_str());

    if (player.playlist && var_Type(player.playlist, filter_key)) {
        VlcValue value{};
        if (var_Get(player.playlist, filter_key, &value) == 0) {
            const auto before = vlc_config::take_string(value.string);
            auto next = vlc_config::replace_filters(before, selected, preset_filter);
            if (next != before) {
                value.string = next.data();
                var_Set(player.playlist, filter_key, value);
            }
        }
    }
    if (!output)
        return;

    const auto live_string = [&](const char *key) {
        const auto configured = string_option(player, nullptr, key);
        if (string_option(player, output.get(), key) != configured)
            set_string(output.get(), key, configured);
    };
    const auto live_float = [&](const char *key) {
        VlcValue value{};
        const float configured = config_GetFloat(player.vlc, key);
        if (var_Get(output.get(), key, &value) != 0 || value.real != configured)
            set_float(output.get(), key, configured);
    };
    VlcValue mode{};
    const auto configured_mode = config_GetInt(player.vlc, stereo_key);
    if (var_Get(output.get(), stereo_key, &mode) == 0 && mode.integer != configured_mode)
        var_Set(output.get(), stereo_key, VlcValue{configured_mode});
    live_string(preset_key);
    live_string(bands_key);
    live_float(preamp_key);
    live_float(pitch_key);
    VlcValue pass{};
    pass.boolean = config_GetInt(player.vlc, two_pass_key) != 0;
    var_Set(output.get(), two_pass_key, pass);
    for (const auto &parameter : compressor_parameters)
        live_float(parameter.key);
    for (const auto &parameter : spatializer_parameters)
        live_float(parameter.key);
    for (const auto &parameter : widener_parameters)
        if (!parameter.reload)
            live_float(parameter.key);

    const auto previous = string_option(player, output.get(), filter_key);
    auto next = vlc_config::replace_filters(previous, selected, preset_filter);
    if (next != previous)
        set_string(output.get(), filter_key, std::move(next));
    else if (contains_filter(previous, widener_filter))
        reload_widener(player);
}

}
