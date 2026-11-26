#include "preferences_presets.hpp"

#include <charconv>
#include <cmath>
#include <cstddef>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

#include "audio_preferences.hpp"
#include "subtitle_preferences.hpp"
#include "track_defaults.hpp"
#include "video_preferences.hpp"
#include "vlc_config.hpp"

namespace preferences_presets {
namespace {

// VLC 3.0.24 Win64 module_config_t, through the original option value.
struct NativeOption {
    std::byte prefix[0x10];
    const char *name;
    std::byte labels_and_current_value[0x18];
    VlcValue original;
};
static_assert(offsetof(NativeOption, original) == 0x30);

extern "C" {
__declspec(dllimport) const NativeOption *config_FindConfig(const char *);
__declspec(dllimport) int config_GetType(const char *);
}

enum ConfigType { boolean = 0x20, integer = 0x30, string = 0x40, real = 0x50 };
using Values = std::map<std::string, std::string, std::less<>>;

struct Timing {
    double subtitle_delay = 0, subtitle_speed = 1, subtitle_duration = 0, audio_delay = 0;
};

struct Snapshot {
    Values options;
    std::string audio_filters;
    video_preferences::PresetFilters video_filters;
    Timing timing;
};

using Presets = std::map<std::string, Snapshot>;

const std::vector<const char *> &option_keys() {
    static const auto keys = [] {
        std::vector<const char *> result{
            track_defaults::subtitle_key, track_defaults::audio_key, "sub-delay", "sub-fps", "subsdec-formatted",
            "freetype-font", "freetype-color", "freetype-outline-color", "sub-margin", "freetype-fontsize",
            "freetype-rel-fontsize", "freetype-outline-thickness", "freetype-background-opacity",
        };
        audio_preferences::append_preset_keys(result);
        video_preferences::append_preset_keys(result);
        return result;
    }();
    return keys;
}

template<class Number>
std::string number_string(Number value) {
    char text[64];
    const auto converted = std::to_chars(text, text + sizeof(text), value);
    return {text, converted.ptr};
}

template<class Number>
bool parse_number(std::string_view text, Number &value) {
    const char *end = text.data() + text.size();
    const auto parsed = std::from_chars(text.data(), end, value);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}

Values read_options(void *vlc, bool defaults) {
    Values result;
    for (const auto key : option_keys()) {
        const auto option = defaults ? config_FindConfig(key) : nullptr;
        if (defaults && (!option || !option->name || std::string_view(option->name) != key))
            continue;
        switch (config_GetType(key)) {
        case boolean:
        case integer:
            result.emplace(key, number_string(option ? option->original.integer : config_GetInt(vlc, key)));
            break;
        case real:
            result.emplace(key, number_string(option ? option->original.real : config_GetFloat(vlc, key)));
            break;
        case string:
            result.emplace(key, option ? (option->original.string ? option->original.string : "") :
                                         vlc_config::take_string(config_GetPsz(vlc, key)));
            break;
        }
    }
    return result;
}

void write_options(void *vlc, const Values &values) {
    for (const auto key : option_keys()) {
        const auto entry = values.find(key);
        if (entry == values.end())
            continue;
        switch (config_GetType(key)) {
        case boolean:
        case integer: {
            std::int64_t value = 0;
            if (parse_number(entry->second, value))
                config_PutInt(vlc, key, value);
            break;
        }
        case real: {
            float value = 0;
            if (parse_number(entry->second, value) && std::isfinite(value))
                config_PutFloat(vlc, key, value);
            break;
        }
        case string:
            config_PutPsz(vlc, key, entry->second.c_str());
            break;
        }
    }
}

// Encode individual values so quotes, path separators and multiline text fit in vlcrc's single line.
std::string encode(std::string_view text) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(text.size() * 2);
    for (const unsigned char byte : text) {
        result += digits[byte >> 4];
        result += digits[byte & 15];
    }
    return result;
}

bool read_value(std::istream &data, std::string &value) {
    std::string text;
    if (!(data >> std::quoted(text)) || text.size() % 2)
        return false;
    value.clear();
    value.reserve(text.size() / 2);
    for (std::size_t i = 0; i < text.size(); i += 2) {
        unsigned int byte = 0;
        const auto end = text.data() + i + 2;
        const auto parsed = std::from_chars(end - 2, end, byte, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != end || byte == 0)
            return false;
        value += static_cast<char>(byte);
    }
    return true;
}

bool read_snapshot(std::istream &data, Snapshot &snapshot) {
    auto &timing = snapshot.timing;
    std::size_t count = 0;
    if (!(data >> timing.subtitle_delay >> timing.subtitle_speed >> timing.subtitle_duration >> timing.audio_delay) ||
        !read_value(data, snapshot.audio_filters) || !read_value(data, snapshot.video_filters.video) ||
        !read_value(data, snapshot.video_filters.overlays) || !(data >> count))
        return false;
    for (std::size_t i = 0; i < count; ++i) {
        std::string key, value;
        if (!(data >> key) || !read_value(data, value))
            return false;
        snapshot.options.emplace(std::move(key), std::move(value));
    }
    return true;
}

Presets read_presets(std::string_view stored) {
    std::istringstream data{std::string(stored)};
    data.imbue(std::locale::classic());
    Presets result;
    int version = 0;
    if (!(data >> version) || version != 1)
        return result;
    std::string name;
    while (data >> std::quoted(name)) {
        Snapshot snapshot;
        if (!read_snapshot(data, snapshot))
            break;
        result.emplace(name, std::move(snapshot));
    }
    return result;
}

std::string serialize(const Presets &presets) {
    std::ostringstream data;
    data.imbue(std::locale::classic());
    data << 1 << std::setprecision(std::numeric_limits<double>::max_digits10);
    for (const auto &[name, snapshot] : presets) {
        const auto &timing = snapshot.timing;
        data << ' ' << std::quoted(name) << ' ' << timing.subtitle_delay << ' ' << timing.subtitle_speed << ' '
             << timing.subtitle_duration << ' ' << timing.audio_delay << ' '
             << std::quoted(encode(snapshot.audio_filters)) << ' ' << std::quoted(encode(snapshot.video_filters.video)) << ' '
             << std::quoted(encode(snapshot.video_filters.overlays)) << ' ' << snapshot.options.size();
        for (const auto &[key, value] : snapshot.options)
            data << ' ' << key << ' ' << std::quoted(encode(value));
    }
    return data.str();
}

std::string saved_settings(void *vlc) {
    return vlc_config::take_string(config_GetPsz(vlc, settings_key));
}

bool save_catalog(void *vlc, const std::string &catalog, const Presets &presets, const std::string &previous_settings) {
    const auto previous_catalog = vlc_config::take_string(config_GetPsz(vlc, names_key));
    config_PutPsz(vlc, names_key, catalog.c_str());
    config_PutPsz(vlc, settings_key, serialize(presets).c_str());
    if (vlc_config::save(vlc))
        return true;
    config_PutPsz(vlc, names_key, previous_catalog.c_str());
    config_PutPsz(vlc, settings_key, previous_settings.c_str());
    return false;
}

std::string read_active_name(void *vlc) {
    std::istringstream data(vlc_config::take_string(config_GetPsz(vlc, names_key)));
    std::size_t selected = 0;
    std::string name;
    if (!(data >> selected) || !selected)
        return name;
    for (std::size_t i = 0; i < selected; ++i)
        if (!(data >> std::quoted(name)))
            return {};
    return name;
}

Timing read_timing(VlcPlayer player) {
    const auto subtitles = subtitle_preferences::timing(player);
    return {subtitles.delay, subtitles.speed, subtitles.duration, audio_preferences::delay(player).value_or(0)};
}

Snapshot capture(VlcPlayer player, Timing timing) {
    return {read_options(player.vlc, false), audio_preferences::preset_filters(player),
            video_preferences::preset_filters(player), timing};
}

void apply_options(VlcPlayer player, const Snapshot &snapshot) {
    const bool was_formatted = config_GetInt(player.vlc, "subsdec-formatted") != 0;
    write_options(player.vlc, snapshot.options);
    video_preferences::apply_preset(player, snapshot.video_filters);
    audio_preferences::apply_preset(player, snapshot.audio_filters);
    const bool formatted = config_GetInt(player.vlc, "subsdec-formatted") != 0;
    subtitle_preferences::apply_preset_appearance(player, formatted != was_formatted);
}

struct PlaybackTiming {
    bool loaded = false;
    std::string name;
    std::optional<Timing> values;
    InputRef input{nullptr, vlc_object_release};
    bool delay_applied = false, speed_applied = false, duration_applied = false, audio_applied = false;

    void attach(Input *next) {
        input.reset(next ? static_cast<Input *>(vlc_object_hold(next)) : nullptr);
        delay_applied = speed_applied = duration_applied = audio_applied = false;
    }

    bool use_input(Input *next) {
        if (!active_playback(next))
            next = nullptr;
        if (next != input.get())
            attach(next);
        return next != nullptr;
    }

    void capture_legacy_timing(VlcPlayer player) {
        // Older releases only saved the preset names.
        values = read_timing(player);
        delay_applied = speed_applied = duration_applied = audio_applied = true;
    }
};

std::mutex mutex;
PlaybackTiming playback;

void load_playback(void *vlc) {
    if (playback.loaded)
        return;
    playback.loaded = true;
    playback.name = read_active_name(vlc);
    if (!playback.name.empty()) {
        const auto presets = read_presets(saved_settings(vlc));
        const auto found = presets.find(playback.name);
        if (found != presets.end())
            playback.values = found->second.timing;
    }
}

template<class Set>
void apply_or_record(double live, double &saved, bool &applied, Set set) {
    if (applied)
        saved = live;
    else
        applied = live == saved || set(saved);
}

void update_timing(VlcPlayer player, Input *input) {
    if (!playback.use_input(input))
        return;
    if (!playback.values) {
        playback.capture_legacy_timing(player);
        return;
    }
    auto &values = *playback.values;
    const auto subtitles = subtitle_preferences::timing(player);
    using Field = subtitle_preferences::Timing;
    if (subtitles.active) {
        apply_or_record(subtitles.delay, values.subtitle_delay, playback.delay_applied,
            [&](double saved) { return subtitle_preferences::set_timing(player, Field::delay, saved); });
        apply_or_record(subtitles.speed, values.subtitle_speed, playback.speed_applied,
            [&](double saved) { return subtitle_preferences::set_timing(player, Field::speed, saved); });
    }
    if (subtitles.video)
        apply_or_record(subtitles.duration, values.subtitle_duration, playback.duration_applied,
            [&](double saved) { return subtitle_preferences::set_timing(player, Field::duration, saved); });
    if (const auto audio = audio_preferences::delay(player))
        apply_or_record(*audio, values.audio_delay, playback.audio_applied,
            [&](double saved) { return audio_preferences::set_delay(player, saved); });
}

// Keep sidebar edits even when playback ends before the next refresh.
Timing *edited_timing(VlcPlayer player) {
    load_playback(player.vlc);
    if (playback.name.empty())
        return nullptr;
    const InputRef input(playlist_CurrentInput(player.playlist), vlc_object_release);
    if (!playback.use_input(input.get()))
        return nullptr;
    if (!playback.values)
        playback.capture_legacy_timing(player);
    return &*playback.values;
}

bool apply_selection(VlcPlayer player, const std::string &previous, const std::string &selected,
                     const std::string &catalog, bool keep_previous) {
    const InputRef input(playlist_CurrentInput(player.playlist), vlc_object_release);
    if (!previous.empty())
        update_timing(player, input.get());
    const auto timing = !previous.empty() && playback.values ? *playback.values : read_timing(player);
    const auto before = capture(player, timing);
    const auto old_settings = saved_settings(player.vlc);
    auto presets = read_presets(old_settings);
    if (!previous.empty()) {
        if (keep_previous)
            presets[previous] = before;
        else
            presets.erase(previous);
    }

    const auto found = presets.find(selected);
    const bool restore = selected.empty() || found != presets.end();
    auto next = selected.empty() ? Snapshot{read_options(player.vlc, true), {}, {}, {}} :
                restore ? found->second : before;
    if (!selected.empty()) {
        if (restore) {
            auto defaults = read_options(player.vlc, true);
            next.options.merge(defaults);
        }
        presets[selected] = next;
    }
    if (restore)
        apply_options(player, next);
    if (!save_catalog(player.vlc, catalog, presets, old_settings)) {
        if (restore)
            apply_options(player, before);
        return false;
    }

    playback.name = selected;
    playback.values = next.timing;
    playback.attach(nullptr);
    update_timing(player, input.get());
    if (selected.empty())
        playback.attach(nullptr);
    return true;
}

}

bool select(VlcPlayer player, const std::string &previous, const std::string &selected, const std::string &catalog) {
    const std::lock_guard lock(mutex);
    load_playback(player.vlc);
    return apply_selection(player, previous, selected, catalog, true);
}

bool rename(VlcPlayer player, const std::string &previous, const std::string &renamed, const std::string &catalog) {
    const std::lock_guard lock(mutex);
    load_playback(player.vlc);
    const auto old_settings = saved_settings(player.vlc);
    auto presets = read_presets(old_settings);
    const bool active = playback.name == previous;
    if (active) {
        const InputRef input(playlist_CurrentInput(player.playlist), vlc_object_release);
        update_timing(player, input.get());
        const auto timing = playback.values ? *playback.values : read_timing(player);
        presets.erase(previous);
        presets[renamed] = capture(player, timing);
    } else if (auto saved = presets.extract(previous)) {
        presets.insert_or_assign(renamed, std::move(saved.mapped()));
    }
    if (!save_catalog(player.vlc, catalog, presets, old_settings))
        return false;
    if (active)
        playback.name = renamed;
    return true;
}

bool remove(VlcPlayer player, const std::string &name, const std::string &catalog) {
    const std::lock_guard lock(mutex);
    load_playback(player.vlc);
    if (playback.name == name)
        return apply_selection(player, name, {}, catalog, false);
    const auto old_settings = saved_settings(player.vlc);
    auto presets = read_presets(old_settings);
    presets.erase(name);
    return save_catalog(player.vlc, catalog, presets, old_settings);
}

void set_audio_delay(VlcPlayer player, double seconds) {
    const std::lock_guard lock(mutex);
    if (!audio_preferences::set_delay(player, seconds))
        return;
    if (auto saved = edited_timing(player)) {
        if (const auto live = audio_preferences::delay(player)) {
            saved->audio_delay = *live;
            playback.audio_applied = true;
        }
    }
}

void set_subtitle_timing(VlcPlayer player, subtitle_preferences::Timing field, double value) {
    const std::lock_guard lock(mutex);
    if (!subtitle_preferences::set_timing(player, field, value))
        return;
    const auto saved = edited_timing(player);
    if (!saved)
        return;
    const auto live = subtitle_preferences::timing(player);
    switch (field) {
    case subtitle_preferences::Timing::delay:
        saved->subtitle_delay = live.delay;
        playback.delay_applied = true;
        break;
    case subtitle_preferences::Timing::speed:
        saved->subtitle_speed = live.speed;
        playback.speed_applied = true;
        break;
    case subtitle_preferences::Timing::duration:
        saved->subtitle_duration = live.duration;
        playback.duration_applied = true;
        break;
    }
}

void refresh(VlcPlayer player, Input *input) {
    const std::unique_lock lock(mutex, std::try_to_lock);
    if (!lock)
        return;
    load_playback(player.vlc);
    if (!playback.name.empty())
        update_timing(player, input);
}

void release(VlcPlayer player) {
    const std::lock_guard lock(mutex);
    if (!playback.name.empty()) {
        const InputRef input(playlist_CurrentInput(player.playlist), vlc_object_release);
        update_timing(player, input.get());
    }
    if (!playback.name.empty() && playback.values) {
        const auto previous = saved_settings(player.vlc);
        auto presets = read_presets(previous);
        presets[playback.name] = capture(player, *playback.values);
        config_PutPsz(player.vlc, settings_key, serialize(presets).c_str());
        if (!vlc_config::save(player.vlc))
            config_PutPsz(player.vlc, settings_key, previous.c_str());
    }
    playback = {};
}

}
