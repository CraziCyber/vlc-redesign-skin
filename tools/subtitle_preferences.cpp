#include "subtitle_preferences.hpp"
#include "vlc_config.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <string_view>

namespace subtitle_preferences {
namespace {

constexpr int bool_var = 0x20, float_var = 0x50, inherited_string = 0x8040;
constexpr double ticks_per_second = 1000000;
constexpr char filter_key[] = "sub-filter", duration_key[] = "subsdelay-factor", duration_filter[] = "subsdelay";
using vlc_config::append_filter;
using vlc_config::split_filter;

InputRef current_input(Player player) {
    InputRef input(player.playlist ? playlist_CurrentInput(player.playlist) : nullptr, vlc_object_release);
    if (!active_playback(input.get()))
        input.reset();
    return input;
}

std::string filter_chain(void *object) {
    VlcValue value{};
    return var_Get(object, filter_key, &value) == 0 ? vlc_config::take_string(value.string) : "";
}

void update_filter(void *object, std::string_view name, std::string_view module) {
    if (!var_Type(object, filter_key) && var_Create(object, filter_key, inherited_string) != 0)
        return;
    const auto previous = filter_chain(object);
    auto chain = split_filter(previous, name);
    append_filter(chain.before, module);
    append_filter(chain.before, chain.after);
    if (chain.before != previous) {
        VlcValue value{};
        value.string = chain.before.data();
        var_Set(object, filter_key, value);
    }
}

float read_duration(Player player) {
    VlcValue value{};
    return var_Get(player.playlist, duration_key, &value) == 0 ? value.real : config_GetFloat(player.vlc, duration_key);
}

void apply_duration(void *object, float factor, std::string_view filter) {
    if (!var_Type(object, duration_key))
        var_Create(object, duration_key, float_var);
    VlcValue value{};
    value.real = factor;
    var_Set(object, duration_key, value);

    update_filter(object, duration_filter, filter);
}

void apply_duration(Playlist *playlist, Input *input, float factor, std::string_view filter) {
    apply_duration(playlist, factor, filter);
    const VideoOutputs outputs(input);
    for (std::size_t i = 0; i < outputs.count; ++i)
        apply_duration(outputs.items[i], factor, filter);
}

struct DurationOverride {
    InputRef media{nullptr, vlc_object_release};
    std::string original_filter;
    float original_factor = 0;
};

std::mutex filter_mutex;
std::atomic_bool duration_modified{false};
DurationOverride duration_override;

void restore_duration(Player player, Input *input) {
    apply_duration(player.playlist, input, duration_override.original_factor, duration_override.original_filter);
    duration_override = {};
    duration_modified.store(false);
}

bool set_duration(Player player, InputRef input, double value) {
    const VideoOutputs outputs(input.get());
    if (!outputs.count)
        return false;

    const std::lock_guard<std::mutex> lock(filter_mutex);
    if (duration_override.media && duration_override.media.get() != input.get())
        restore_duration(player, input.get());
    if (!duration_override.media) {
        duration_override = {std::move(input), split_filter(filter_chain(player.playlist), duration_filter).selected,
                             read_duration(player)};
        duration_modified.store(true);
    }
    apply_duration(player.playlist, duration_override.media.get(), static_cast<float>(value),
                   value > 0 ? duration_filter : "");
    return true;
}

void enable_style_filter(Player player, Input *input) {
    const std::lock_guard<std::mutex> lock(filter_mutex);
    update_filter(player.playlist, text_style_filter, text_style_filter);
    const VideoOutputs outputs(input);
    for (std::size_t i = 0; i < outputs.count; ++i)
        update_filter(outputs.items[i], text_style_filter, text_style_filter);
}

int option(Player player, const char *key) {
    return static_cast<int>(config_GetInt(player.vlc, key));
}

void restart_subtitles(Input *input) {
    VlcValue track{};
    if (input && var_Get(input, "spu-es", &track) == 0 && track.integer >= 0)
        input_Control(input, input_restart_es, static_cast<int>(track.integer));
}

bool set_delay(Input *input, double seconds) {
    VlcValue delay{};
    delay.integer = std::llround(std::clamp(seconds, -600.0, 600.0) * ticks_per_second);
    return var_Set(input, "spu-delay", delay) == 0;
}

}

Synchronization timing(Player player) {
    Synchronization result;
    result.duration_in_seconds = option(player, "subsdelay-mode") == 0;
    const auto input = current_input(player);
    if (!input)
        return result;

    result.active = true;
    result.video = VideoOutputs(input.get()).count > 0;
    VlcValue value{};
    if (var_Get(input.get(), "spu-delay", &value) == 0)
        result.delay = value.integer / ticks_per_second;
    if (var_Get(input.get(), "sub-fps", &value) == 0 && value.real > 0)
        result.speed = value.real;
    if (result.video && !split_filter(filter_chain(player.playlist), duration_filter).selected.empty())
        result.duration = read_duration(player);
    return result;
}

bool set_timing(Player player, Timing field, double number) {
    auto input = current_input(player);
    if (!input || !std::isfinite(number))
        return false;
    VlcValue value{};
    switch (field) {
    case Timing::delay:
        return set_delay(input.get(), number);
    case Timing::speed:
        // 1 is the sidebar's normal speed. VLC stores automatic FPS as 0.
        value.real = number <= 1 ? 0.0f : static_cast<float>(std::min(number, 100.0));
        return var_Set(input.get(), "sub-fps", value) == 0;
    case Timing::duration:
        return set_duration(player, std::move(input), std::clamp(number, 0.0, 20.0));
    }
    return false;
}

bool adjust_delay(Input *input, int milliseconds) {
    VlcValue delay{};
    return active_playback(input) && var_Get(input, "spu-delay", &delay) == 0 &&
        set_delay(input, delay.integer / ticks_per_second + milliseconds / 1000.0);
}

Appearance appearance(Player player) {
    return {option(player, "subsdec-formatted") != 0, vlc_config::take_string(config_GetPsz(player.vlc, "freetype-font")),
            static_cast<std::uint32_t>(option(player, "freetype-color")),
            static_cast<std::uint32_t>(option(player, "freetype-outline-color")), option(player, "sub-margin"),
            std::clamp(option(player, "freetype-fontsize"), 0, 4096), option(player, "freetype-rel-fontsize"),
            std::clamp(option(player, "freetype-outline-thickness"), 0, 50),
            std::clamp(option(player, "freetype-background-opacity"), 0, 255)};
}

StyleResult set_style(Player player, Style field, const Appearance &style) {
    const auto input = current_input(player);
    bool reload = false;
    switch (field) {
    case Style::format: {
        config_PutInt(player.vlc, "subsdec-formatted", style.formatted);
        keep_embedded_style(style.formatted);
        enable_style_filter(player, input.get());
        restart_subtitles(input.get());
        break;
    }
    case Style::font:
        config_PutPsz(player.vlc, "freetype-font", style.font.c_str());
        reload = true;
        break;
    case Style::text_color:
        config_PutInt(player.vlc, "freetype-color", style.color);
        break;
    case Style::outline_color:
        config_PutInt(player.vlc, "freetype-outline-color", style.outline_color);
        reload = true;
        break;
    case Style::offset: {
        config_PutInt(player.vlc, "sub-margin", style.offset);
        const VideoOutputs outputs(input.get());
        for (std::size_t i = 0; i < outputs.count; ++i)
            var_Set(outputs.items[i], "sub-margin", VlcValue{style.offset});
        break;
    }
    case Style::size:
        config_PutInt(player.vlc, "freetype-fontsize", style.font_pixels);
        config_PutInt(player.vlc, "freetype-rel-fontsize", style.font_divisor);
        set_live_font_size(style.font_pixels, style.font_divisor);
        enable_style_filter(player, input.get());
        break;
    case Style::thickness:
        config_PutInt(player.vlc, "freetype-outline-thickness", style.outline_percent);
        break;
    case Style::opacity:
        config_PutInt(player.vlc, "freetype-background-opacity", style.background_alpha);
        break;
    }
    return {vlc_config::save(player.vlc), reload && bool(input)};
}

void use_saved_style(Player player) {
    keep_embedded_style(option(player, "subsdec-formatted") != 0);
    const int pixels = std::clamp(option(player, "freetype-fontsize"), 0, 4096);
    const int divisor = option(player, "freetype-rel-fontsize");
    if (pixels > 0 || divisor > 0)
        set_live_font_size(pixels, divisor);
    const auto input = current_input(player);
    enable_style_filter(player, input.get());
}

void reset_duration_on_media_change(Player player) {
    if (!duration_modified.load())
        return;
    const std::lock_guard<std::mutex> lock(filter_mutex);
    const auto input = current_input(player);
    if (duration_override.media && duration_override.media.get() != input.get())
        restore_duration(player, input.get());
}

void release_media() {
    const std::lock_guard<std::mutex> lock(filter_mutex);
    duration_override = {};
    duration_modified.store(false);
    set_live_font_size(-1, 0);
}

}
