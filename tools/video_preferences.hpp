#pragma once

#include <array>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <optional>
#include <string>

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

inline constexpr Choice gradient_modes[] = {{L"Gradient", "gradient"}, {L"Edge", "edge"}, {L"Hough", "hough"}};
inline constexpr int hough_mode = 2, threshold_limit = 100, sepia_limit = 255;

enum class ColorEffect { extraction, threshold, negate, posterize, gradient, sepia, count };
enum class ColorValue { extract_rgb, threshold_rgb, saturation, similarity, posterize_level, gradient_mode,
                        gradient_color, cartoon, sepia_intensity, count };

struct ColorSettings {
    bool &enabled(ColorEffect effect) { return active[static_cast<int>(effect)]; }
    bool enabled(ColorEffect effect) const { return active[static_cast<int>(effect)]; }
    int &value(ColorValue option) { return values[static_cast<int>(option)]; }
    int value(ColorValue option) const { return values[static_cast<int>(option)]; }

    std::array<bool, static_cast<int>(ColorEffect::count)> active{};
    std::array<int, static_cast<int>(ColorValue::count)> values{};
};

enum class VideoEffect { interactive_zoom, mirror, text_overlay, logo_overlay, denoiser, antiflicker, motion_blur, count };
enum class EffectValue {
    mirror_split, mirror_direction, text_position, text_size, logo_position, logo_opacity,
    spatial_luma, spatial_chroma, temporal_luma, temporal_chroma, flicker_window, flicker_softening, blur_strength, count
};

struct EffectOption {
    VideoEffect effect;
    const char *key, *name;
    int minimum, maximum;
    bool real = false;
};

struct EffectSettings {
    bool &enabled(VideoEffect effect) { return active[static_cast<int>(effect)]; }
    bool enabled(VideoEffect effect) const { return active[static_cast<int>(effect)]; }
    double &value(EffectValue option) { return values[static_cast<int>(option)]; }
    double value(EffectValue option) const { return values[static_cast<int>(option)]; }

    std::array<bool, static_cast<int>(VideoEffect::count)> active{};
    std::array<double, static_cast<int>(EffectValue::count)> values{};
    std::string text, logo;
};

enum CropEdge { top_edge, bottom_edge, left_edge, right_edge, crop_edge_count };
using CropMargins = std::array<int, crop_edge_count>;
inline constexpr int max_crop_margin = std::numeric_limits<int>::max() / 2;

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
CropMargins crop_margins(VlcPlayer player);
bool set_crop_margins(VlcPlayer player, const CropMargins &margins);
EssentialPositions essential_positions(VlcPlayer player);
void set_essential_position(VlcPlayer player, const EssentialSlider &slider, int position);
ColorSettings color_settings(VlcPlayer player);
void apply_color_settings(VlcPlayer player, ColorEffect effect, const ColorSettings &settings);
const EffectOption &effect_option(EffectValue id);
EffectSettings effect_settings(VlcPlayer player);
void apply_effect_settings(VlcPlayer player, VideoEffect effect, const EffectSettings &settings);

inline constexpr char software_video_filter[] = "redesignsoftwarevideo";
int open_software_video_filter(void *object);

}
