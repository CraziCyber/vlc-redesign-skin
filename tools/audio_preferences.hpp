#pragma once

#include <array>
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

struct CompressorParameter {
    const char *key;
    const wchar_t *label, *suffix;
    float minimum, maximum, initial;
};
inline constexpr CompressorParameter compressor_parameters[] = {
    {"compressor-rms-peak", L"RMS / Peak", L"", 0.0f, 1.0f, 0.2f},
    {"compressor-attack", L"Attack", L" ms", 1.5f, 400.0f, 25.0f},
    {"compressor-release", L"Release", L" ms", 2.0f, 800.0f, 100.0f},
    {"compressor-threshold", L"Threshold", L" dB", -30.0f, 0.0f, -11.0f},
    {"compressor-ratio", L"Ratio", L" : 1", 1.0f, 20.0f, 4.0f},
    {"compressor-knee", L"Knee Radius", L" dB", 1.0f, 10.0f, 5.0f},
    {"compressor-makeup-gain", L"Makeup Gain", L" dB", 0.0f, 24.0f, 7.0f},
};
using CompressorValues = std::array<float, sizeof(compressor_parameters) / sizeof(CompressorParameter)>;

struct Settings {
    std::optional<double> delay;
    int stereo_mode;
    std::string equalizer;
    EqualizerGains gains;
    bool two_pass, compressor, spatializer, widener;
    float pitch;
    CompressorValues compressor_values;
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
void set_compressor_value(VlcPlayer player, const CompressorParameter &parameter, float value);
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
