#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "playback_preferences.hpp"
#include "vlc_config.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string_view>

namespace {

// libvlc_media_stats_t in VLC 3.0.
struct MediaStats {
    int read_bytes;
    float input_bitrate;
    int demux_read_bytes;
    float demux_bitrate;
    int demux_corrupted, demux_discontinuity;
    int decoded_video, decoded_audio, displayed_pictures, lost_pictures, played_audio, lost_audio;
    int sent_packets, sent_bytes;
    float send_bitrate;
};

extern "C" {
__declspec(dllimport) int libvlc_media_get_stats(Media *, MediaStats *);
__declspec(dllimport) int vout_RegisterSubpictureChannel(void *);
__declspec(dllimport) void vout_FlushSubpictureChannel(void *, int);
__declspec(dllimport) void vout_OSDText(void *, int, int, std::int64_t, const char *);
}

using namespace playback_preferences;
constexpr std::int64_t playing_state = 2, paused_state = 3, ticks_per_ms = 1000;
constexpr ULONGLONG rewind_step_ms = 220;

struct Reverse {
    InputRef input{nullptr, vlc_object_release};
    bool native = false, resume = false;
    ULONGLONG last_step = 0;
};

std::mutex controls;
Reverse reverse;
ULONGLONG sleep_until = 0;
int sleep_minutes = 0;

struct StatisticsOverlay {
    std::unique_ptr<void, decltype(&vlc_object_release)> output{nullptr, vlc_object_release};
    int channel = -1;
    ULONGLONG next_refresh = 0;

    void clear() {
        if (output && channel >= 0)
            vout_FlushSubpictureChannel(output.get(), channel);
        output.reset();
        channel = -1;
        next_refresh = 0;
    }
};

StatisticsOverlay statistics;

VlcValue variable(void *object, const char *key) {
    VlcValue result{};
    if (object)
        var_Get(object, key, &result);
    return result;
}

bool can_reverse(Input *input) {
    if (!active_playback(input))
        return false;
    return variable(input, "can-rewind").boolean ||
           (variable(input, "can-seek").boolean && variable(input, "can-pause").boolean);
}

float rate(VlcPlayer player, Input *input) {
    const auto live = variable(input ? static_cast<void *>(input) : player.playlist, "rate").real;
    const auto saved = config_GetFloat(player.vlc, "rate");
    const auto value = live != 0 ? live : saved;
    return std::isfinite(value) && value != 0 ? std::abs(value) : 1.0f;
}

void set_input_rate(Input *input, float speed) {
    VlcValue value{};
    value.real = speed;
    var_Set(input, "rate", value);
}

void stop_reverse(VlcPlayer player, Input *current, bool resume) {
    if (reverse.input && reverse.input.get() == current) {
        if (reverse.native)
            set_input_rate(current, rate(player, current));
        if (resume && reverse.resume && variable(current, "state").integer == paused_state)
            var_Set(current, "state", VlcValue{playing_state});
    }
    reverse = {};
}

void refresh_reverse(VlcPlayer player, Input *input, ULONGLONG now) {
    if (!reverse.input)
        return;
    if (reverse.input.get() != input || !active_playback(input)) {
        stop_reverse(player, input, false);
        return;
    }
    if (reverse.native) {
        if (variable(input, "rate").real > 0)
            stop_reverse(player, input, false);
        return;
    }
    const auto elapsed = now - reverse.last_step;
    if (elapsed < rewind_step_ms)
        return;
    if (variable(input, "state").integer != paused_state) {
        stop_reverse(player, input, false);
        return;
    }

    const auto time = variable(input, "time").integer;
    const auto step = std::llround(std::min<ULONGLONG>(elapsed, 500) * ticks_per_ms * rate(player, input));
    const auto target = time > step ? time - step : 0;
    var_Set(input, "time", VlcValue{target});
    reverse.last_step = now;
    if (target == 0)
        stop_reverse(player, input, false);
}

int visualizer(VlcPlayer player) {
    const auto module = vlc_config::take_string(config_GetPsz(player.vlc, "audio-visual"));
    const auto mode = module == "visual" ? vlc_config::take_string(config_GetPsz(player.vlc, "effect-list")) : module;
    if (mode.empty() || mode == "none")
        return 0;
    for (std::size_t i = 1; i < std::size(visualizers); ++i) {
        if (_stricmp(mode.c_str(), visualizers[i].mode) == 0)
            return static_cast<int>(i);
    }
    return -1;
}

void refresh_statistics(VlcPlayer player, Input *input, LibVlcRef &metadata, ULONGLONG now) {
    if (!input || !switch_enabled(player, statistics_key)) {
        statistics.clear();
        return;
    }
    if (now < statistics.next_refresh)
        return;
    statistics.next_refresh = now + 750;
    const VideoOutputs outputs(input);
    void *output = outputs.count ? outputs.items[0] : nullptr;
    if (output != statistics.output.get()) {
        statistics.clear();
        statistics.next_refresh = now + 750;
        if (output) {
            statistics.output.reset(vlc_object_hold(output));
            statistics.channel = vout_RegisterSubpictureChannel(output);
        }
    }
    if (!output || statistics.channel < 0)
        return;

    if (!metadata)
        metadata.reset(new_metadata_library());
    if (!metadata)
        return;
    const MediaRef media(libvlc_media_new_from_input_item(metadata.get(), input_GetItem(input)), libvlc_media_release);
    MediaStats stats{};
    if (!media || !libvlc_media_get_stats(media.get(), &stats))
        return;
    const double bitrate = std::isfinite(stats.demux_bitrate) ? stats.demux_bitrate * 8000.0 : 0.0;
    char text[256];
    std::snprintf(text, sizeof(text), "Playback statistics\nBitrate: %.0f kb/s\n"
                  "Displayed frames: %d   Dropped: %d\nAudio buffers: %d   Dropped: %d",
                  bitrate, stats.displayed_pictures, stats.lost_pictures, stats.played_audio, stats.lost_audio);
    constexpr int top_left = 5; // SUBPICTURE_ALIGN_LEFT | SUBPICTURE_ALIGN_TOP.
    vout_OSDText(output, statistics.channel, top_left, 1500000, text);
}

}

namespace playback_preferences {

bool switch_enabled(VlcPlayer player, const char *key) {
    return vlc_config::take_string(config_GetPsz(player.vlc, key)) == "1";
}

bool set_switch(VlcPlayer player, const char *key, bool enabled) {
    config_PutPsz(player.vlc, key, enabled ? "1" : "");
    return vlc_config::save(player.vlc);
}

Settings settings(VlcPlayer player) {
    const InputRef input(playlist_CurrentInput(player.playlist), vlc_object_release);
    const std::lock_guard<std::mutex> lock(controls);
    const auto now = GetTickCount64();
    return {switch_enabled(player, timeline_key), switch_enabled(player, statistics_key),
            reverse.input && reverse.input.get() == input.get(), can_reverse(input.get()), rate(player, input.get()),
            sleep_until > now ? static_cast<int>((sleep_until - now + 999) / 1000) : 0, sleep_minutes, visualizer(player)};
}

bool set_speed(VlcPlayer player, float speed, bool save) {
    if (!std::isfinite(speed) || speed < min_speed || speed > max_speed)
        return false;
    const std::lock_guard<std::mutex> lock(controls);
    config_PutFloat(player.vlc, "rate", speed);
    VlcValue value{};
    value.real = speed;
    var_Set(player.playlist, "rate", value);
    if (reverse.input && reverse.native)
        set_input_rate(reverse.input.get(), -speed);
    return !save || vlc_config::save(player.vlc);
}

bool set_visualizer(VlcPlayer player, int index) {
    if (index < 0 || index >= static_cast<int>(std::size(visualizers)))
        return false;
    const char *mode = visualizers[index].mode;
    if (index != 0)
        config_PutPsz(player.vlc, "effect-list", mode);
    config_PutPsz(player.vlc, "audio-visual", index == 0 ? "none" : "visual");
    const AudioOutputRef audio(playlist_GetAout(player.playlist), vlc_object_release);
    if (audio) {
        VlcValue value{};
        value.string = const_cast<char *>(mode);
        var_Set(audio.get(), "visual", value);
    }
    return vlc_config::save(player.vlc);
}

void set_timer(int minutes) {
    const std::lock_guard<std::mutex> lock(controls);
    sleep_minutes = minutes;
    sleep_until = minutes ? GetTickCount64() + 60000ULL * minutes : 0;
}

void set_reverse(VlcPlayer player, bool enabled) {
    InputRef input(playlist_CurrentInput(player.playlist), vlc_object_release);
    const std::lock_guard<std::mutex> lock(controls);
    if (!enabled) {
        stop_reverse(player, input.get(), true);
    } else if (can_reverse(input.get())) {
        const bool native = variable(input.get(), "can-rewind").boolean;
        const bool playing = variable(input.get(), "state").integer == playing_state;
        if (native)
            set_input_rate(input.get(), -rate(player, input.get()));
        else if (playing)
            var_Set(input.get(), "state", VlcValue{paused_state});
        reverse = {std::move(input), native, !native && playing, GetTickCount64()};
    }
}

void refresh(VlcPlayer player, Input *input, LibVlcRef &metadata) {
    const auto now = GetTickCount64();
    {
        const std::lock_guard<std::mutex> lock(controls);
        if (sleep_until && sleep_until <= now) {
            sleep_until = 0;
            sleep_minutes = 0;
            stop_reverse(player, input, false);
            if (input && variable(input, "state").integer == playing_state)
                var_Set(input, "state", VlcValue{paused_state});
        } else {
            refresh_reverse(player, input, now);
        }
    }
    refresh_statistics(player, input, metadata, now);
}

void release() {
    const std::lock_guard<std::mutex> lock(controls);
    reverse = {};
    sleep_until = 0;
    sleep_minutes = 0;
    statistics.clear();
}

}
