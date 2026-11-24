#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "vlc_input.hpp"

namespace track_defaults {

enum class Kind { subtitles, audio };

inline constexpr char subtitle_key[] = "redesign-subtitle-defaults";
inline constexpr char audio_key[] = "redesign-audio-defaults";

struct Preset {
    int track = 0;
    std::string language;

    bool operator==(const Preset &other) const { return track == other.track && language == other.language; }
};

Preset load(VlcPlayer player, Kind kind);
bool save(VlcPlayer player, Kind kind, const Preset &preset);
std::optional<int> parse_track(std::string_view value);
std::optional<std::string> parse_language(std::string_view value);
std::string_view language_name(const std::string &code);

class AutoSelect {
public:
    explicit AutoSelect(Kind kind) : kind_(kind) {}
    void refresh(VlcPlayer player, LibVlcRef &library);

private:
    Kind kind_;
    Preset preset_;
    InputRef input_{nullptr, vlc_object_release};
    bool applied_ = false;
};

}
