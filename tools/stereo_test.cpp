#include "audio_preferences.hpp"
#include "vlc_config.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>

using audio_preferences::TestPlayer;

extern "C" {
__declspec(dllimport) Media *libvlc_media_new_callbacks(LibVlc *,
    int (*)(void *, void **, std::uint64_t *), std::ptrdiff_t (*)(void *, unsigned char *, std::size_t),
    int (*)(void *, std::uint64_t), void (*)(void *), void *);
__declspec(dllimport) TestPlayer *libvlc_media_player_new_from_media(Media *);
__declspec(dllimport) int libvlc_media_player_play(TestPlayer *);
__declspec(dllimport) void libvlc_media_player_stop(TestPlayer *);
__declspec(dllimport) int libvlc_media_player_get_state(TestPlayer *);
__declspec(dllimport) void libvlc_media_player_release(TestPlayer *);
__declspec(dllimport) void libvlc_audio_output_device_set(TestPlayer *, const char *, const char *);
__declspec(dllimport) char *aout_DeviceGet(AudioOutput *);
}

namespace audio_preferences {
namespace {

constexpr int rate = 48000, tone_length = rate / 3, gap = rate / 7, frames = tone_length * 2 + gap;

struct StereoWave {
    char riff[4]{'R', 'I', 'F', 'F'};
    std::uint32_t riff_size = 36 + frames * 4;
    char wave[4]{'W', 'A', 'V', 'E'};
    char format[4]{'f', 'm', 't', ' '};
    std::uint32_t format_size = 16;
    std::uint16_t pcm = 1, channels = 2;
    std::uint32_t frequency = rate, byte_rate = rate * 4;
    std::uint16_t alignment = 4, bits = 16;
    char data[4]{'d', 'a', 't', 'a'};
    std::uint32_t data_size = frames * 4;
    std::int16_t samples[frames][2]{};

    StereoWave() {
        constexpr int fade = rate / 50;
        for (int i = 0; i < tone_length; ++i) {
            const auto envelope = std::min({fade, i, tone_length - 1 - i}) / double(fade);
            constexpr double tau = 6.283185307179586;
            samples[i][0] = std::lround(3300 * envelope * std::sin(tau * 440 * i / rate));
            samples[tone_length + gap + i][1] = std::lround(3300 * envelope * std::sin(tau * 660 * i / rate));
        }
    }
};
static_assert(offsetof(StereoWave, samples) == 44);

int open_wave(void *, void **reader, std::uint64_t *size) {
    *reader = new (std::nothrow) std::size_t(0);
    *size = sizeof(StereoWave);
    return *reader ? 0 : -1;
}

std::ptrdiff_t read_wave(void *reader, unsigned char *buffer, std::size_t length) {
    static const StereoWave wave;
    auto &offset = *static_cast<std::size_t *>(reader);
    const auto count = std::min(length, sizeof(wave) - offset);
    std::memcpy(buffer, reinterpret_cast<const unsigned char *>(&wave) + offset, count);
    offset += count;
    return count;
}

int seek_wave(void *reader, std::uint64_t offset) {
    if (offset > sizeof(StereoWave))
        return -1;
    *static_cast<std::size_t *>(reader) = offset;
    return 0;
}

void close_wave(void *reader) {
    delete static_cast<std::size_t *>(reader);
}

}

StereoTest::~StereoTest() { stop(); }

bool StereoTest::play(VlcPlayer player, int mode) {
    stop();
    VlcValue output_name{};
    constexpr int string_var = 0x40;  // VLC_VAR_STRING
    const auto output = var_Inherit(player.playlist, "aout", string_var, &output_name) == 0
        ? vlc_config::take_string(output_name.string) : "";
    const auto aout = "--aout=" + (output.empty() ? "any" : output);
    const auto stereo = "--stereo-mode=" + std::to_string(mode);
    const char *arguments[] = {"--ignore-config", "--no-media-library", "--no-video", "--intf=dummy",
                              aout.c_str(), stereo.c_str()};
    library_.reset(libvlc_new(std::size(arguments), arguments));
    if (!library_)
        return false;
    const MediaRef media(libvlc_media_new_callbacks(library_.get(), open_wave, read_wave, seek_wave, close_wave, nullptr),
                         libvlc_media_release);
    if (media)
        player_ = libvlc_media_player_new_from_media(media.get());
    if (player_) {
        const AudioOutputRef active_output(playlist_GetAout(player.playlist), vlc_object_release);
        const auto device = active_output ? vlc_config::take_string(aout_DeviceGet(active_output.get())) :
            vlc_config::take_string(config_GetPsz(player.vlc, "mmdevice-audio-device"));
        if (!device.empty())
            libvlc_audio_output_device_set(player_, nullptr, device.c_str());
        if (libvlc_media_player_play(player_) == 0)
            return true;
    }
    stop();
    return false;
}

void StereoTest::stop() {
    if (player_) {
        libvlc_media_player_stop(player_);
        libvlc_media_player_release(player_);
        player_ = nullptr;
    }
    library_.reset();
}

void StereoTest::release_finished() {
    constexpr int ended = 6;  // libvlc_Ended
    if (player_ && libvlc_media_player_get_state(player_) >= ended)
        stop();
}

}
