#pragma once

#include <algorithm>
#include <cstdint>
#include <string>

#include "vlc_input.hpp"

namespace subtitle_preferences {

using Player = VlcPlayer;

struct Synchronization {
    bool active = false;
    bool video = false;
    bool duration_in_seconds = false;
    double delay = 0, speed = 1, duration = 0;
};

struct Appearance {
    bool formatted;
    std::string font;
    std::uint32_t color, outline_color;
    int offset, font_pixels, font_divisor, outline_percent, background_alpha;
};

enum class Timing { delay, speed, duration };
enum class Style { format, font, text_color, outline_color, offset, size, thickness, opacity };

struct StyleResult {
    bool saved;
    bool reopen_media;
};

Synchronization timing(Player player);
bool set_timing(Player player, Timing field, double value);
bool adjust_delay(Input *input, int milliseconds);
Appearance appearance(Player player);
StyleResult set_style(Player player, Style field, const Appearance &style);
void use_saved_style(Player player);
void reset_duration_on_media_change(Player player);
void release_media();

inline constexpr char text_style_filter[] = "redesigntextstyle";
inline constexpr char speed_filter[] = "redesignsubtitlespeed";
inline constexpr int default_relative_divisor = 16;
inline int relative_font_pixels(int short_side, int divisor) {
    return std::clamp(short_side / (divisor > 0 ? divisor : default_relative_divisor), 1, 4096);
}
void keep_embedded_style(bool enabled);
void set_live_font_size(int pixels, int divisor);
int open_style_filter(void *object);
int open_speed_filter(void *object);
void close_speed_filter(void *object);

}
