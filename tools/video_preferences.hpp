#pragma once

#include <array>
#include <initializer_list>
#include <iterator>
#include <optional>

#include "vlc_input.hpp"

namespace video_preferences {

enum class Flip { none, horizontal, vertical };

struct Transform {
    Flip flip = Flip::none;
    double degrees = 0;
};

enum class Field { zoom, hdr, resolution, aspect, crop };

struct Choice {
    const wchar_t *label;
    const char *text = nullptr;
    float number = 0;
};

enum class Effect { adjust, sharpen, deband, grain };

struct EssentialSlider {
    const wchar_t *label;
    Effect effect;
    const char *key, *option;
    int minimum, maximum, units, initial, divisions;
};

inline constexpr EssentialSlider gamma_slider{L"Gamma", Effect::adjust, "gamma", "gamma", 1, 1000, 100, 100, 10};

inline constexpr EssentialSlider essential_sliders[] = {
    {L"Hue", Effect::adjust, "hue", "hue", -1800, 1800, 10, 0, 0},
    {L"Brightness", Effect::adjust, "brightness", "brightness", 0, 200, 100, 100, 0},
    {L"Contrast", Effect::adjust, "contrast", "contrast", 0, 200, 100, 100, 0},
    {L"Saturation", Effect::adjust, "saturation", "saturation", 0, 300, 100, 100, 3},
    gamma_slider,
    {L"Sharpen Sigma", Effect::sharpen, "sharpen-sigma", "sigma", 0, 200, 100, 0, 4},
    // The position below VLC's minimum radius (4) disables the filter.
    {L"Banding Removal Radius", Effect::deband, "gradfun-radius", "radius", 3, 32, 1, 16, 7},
    {L"Film Grain Variance", Effect::grain, "grain-variance", "variance", 0, 1000, 100, 200, 10},
};
using EssentialPositions = std::array<int, std::size(essential_sliders)>;

Transform transform(VlcPlayer player);
bool set_transform(VlcPlayer player, Transform value);
std::initializer_list<Choice> choices(Field field);
std::optional<int> selected(VlcPlayer player, Field field);
bool set_choice(VlcPlayer player, Field field, int index);
EssentialPositions essential_positions(VlcPlayer player);
void set_essential_position(VlcPlayer player, const EssentialSlider &slider, int position);

inline constexpr char software_video_filter[] = "redesignsoftwarevideo";
int open_software_video_filter(void *object);

}
