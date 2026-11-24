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

struct Settings {
    std::optional<double> delay;
    int stereo_mode;
    std::string equalizer;
    EqualizerGains gains;
    bool two_pass, compressor, spatializer, widener;
    float pitch;
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
