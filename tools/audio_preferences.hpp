#pragma once

#include <array>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "vlc_input.hpp"

namespace audio_preferences {

struct TestPlayer;

enum class Filter { compressor, spatializer, widener };

// Preamp followed by the ten frequency bands in VLC order.
using EqualizerGains = std::array<float, 11>;
inline constexpr int equalizer_min_db = -20, equalizer_max_db = 20;

struct FilterParameter {
    const char *key;
    const wchar_t *label, *suffix;
    float minimum, maximum, initial;
    int steps_per_unit = 10;
    bool reload = false;
};
inline constexpr FilterParameter compressor_parameters[] = {
    {"compressor-rms-peak", L"RMS / Peak", L"", 0.0f, 1.0f, 0.2f},
    {"compressor-attack", L"Attack", L" ms", 1.5f, 400.0f, 25.0f},
    {"compressor-release", L"Release", L" ms", 2.0f, 800.0f, 100.0f},
    {"compressor-threshold", L"Threshold", L" dB", -30.0f, 0.0f, -11.0f},
    {"compressor-ratio", L"Ratio", L" : 1", 1.0f, 20.0f, 4.0f},
    {"compressor-knee", L"Knee Radius", L" dB", 1.0f, 10.0f, 5.0f},
    {"compressor-makeup-gain", L"Makeup Gain", L" dB", 0.0f, 24.0f, 7.0f},
};
inline constexpr FilterParameter spatializer_parameters[] = {
    {"spatializer-roomsize", L"Size", L"", 0.0f, 1.1f, 0.85f},
    {"spatializer-width", L"Width", L"", 0.0f, 1.0f, 1.0f},
    {"spatializer-wet", L"Wet", L"", 0.0f, 1.0f, 0.4f},
    {"spatializer-dry", L"Dry", L"", 0.0f, 1.0f, 0.5f},
    {"spatializer-damp", L"Damp", L"", 0.0f, 1.0f, 0.5f},
};
inline constexpr FilterParameter widener_parameters[] = {
    {"stereowiden-delay", L"Delay Time", L" ms", 1.0f, 100.0f, 20.0f, 1, true},
    {"stereowiden-feedback", L"Feedback Gain", L" %", 0.0f, 0.9f, 0.3f},
    {"stereowiden-crossfeed", L"Crossfeed", L" %", 0.0f, 0.8f, 0.3f, 10, true},
    {"stereowiden-dry-mix", L"Dry Mix", L" %", 0.0f, 1.0f, 0.8f},
};
using CompressorValues = std::array<float, std::size(compressor_parameters)>;
using SpatializerValues = std::array<float, std::size(spatializer_parameters)>;
using WidenerValues = std::array<float, std::size(widener_parameters)>;

struct Settings {
    std::optional<double> delay;
    int stereo_mode;
    std::string equalizer;
    EqualizerGains gains;
    bool two_pass, compressor, spatializer, widener;
    float pitch;
    CompressorValues compressor_values;
    SpatializerValues spatializer_values;
    WidenerValues widener_values;
};

struct Preset {
    std::string key, label;
    EqualizerGains gains;
};

Settings read(VlcPlayer player, const std::vector<Preset> &presets);
std::vector<Preset> equalizer_presets(VlcPlayer player);
void set_stereo_mode(VlcPlayer player, int mode);
void set_delay(VlcPlayer player, double seconds);
void set_equalizer(VlcPlayer player, const Preset *preset);
void set_equalizer_gains(VlcPlayer player, const EqualizerGains &gains);
void set_two_pass(VlcPlayer player, bool enabled);
void set_filter(VlcPlayer player, Filter filter, bool enabled);
void set_filter_value(VlcPlayer player, const FilterParameter &parameter, float value);
void reload_widener(VlcPlayer player);
void set_pitch(VlcPlayer player, float semitones);

class StereoTest {
public:
    StereoTest() = default;
    ~StereoTest();
    StereoTest(const StereoTest &) = delete;
    StereoTest &operator=(const StereoTest &) = delete;
    bool play(VlcPlayer player, int mode);
    void stop();
    void release_finished();

private:
    LibVlcRef library_{nullptr, libvlc_release};
    TestPlayer *player_ = nullptr;
};

}
