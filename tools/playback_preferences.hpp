#pragma once

#include "vlc_input.hpp"

namespace playback_preferences {

inline constexpr char timeline_key[] = "redesign-subtitle-timeline";
inline constexpr char statistics_key[] = "redesign-playback-statistics";
inline constexpr float min_speed = 0.25f, max_speed = 2.0f;

struct TimerChoice { const wchar_t *label; int minutes; };
inline constexpr TimerChoice timers[] = {
    {L"Off", 0}, {L"1 minute", 1}, {L"15 minutes", 15}, {L"30 minutes", 30},
    {L"45 minutes", 45}, {L"1 hour", 60}, {L"1 hour 30 minutes", 90}, {L"2 hours", 120},
};

struct VisualizerChoice { const wchar_t *label; const char *mode; };
inline constexpr VisualizerChoice visualizers[] = {
    {L"Disabled", "none"}, {L"Spectrum", "spectrum"}, {L"Oscilloscope", "scope"},
    {L"Spectrometer", "spectrometer"}, {L"VU Meter", "vuMeter"},
};

struct Settings {
    bool timeline, statistics, reverse, can_reverse;
    float speed;
    int remaining_seconds, timer_minutes, visualizer;
};

Settings settings(VlcPlayer player);
bool set_switch(VlcPlayer player, const char *key, bool enabled);
bool switch_enabled(VlcPlayer player, const char *key);
bool set_speed(VlcPlayer player, float speed, bool save = true);
bool set_visualizer(VlcPlayer player, int index);
void set_timer(int minutes);
void set_reverse(VlcPlayer player, bool enabled);
void refresh(VlcPlayer player, Input *input, LibVlcRef &metadata);
void release();

}
