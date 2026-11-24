#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "subtitle_preferences.hpp"
#include "vlc_input.hpp"

namespace subtitle_defaults {

inline constexpr char config_key[] = "redesign-subtitle-defaults";

struct Preset {
    int track = 0;
    std::string language;

    bool operator==(const Preset &other) const { return track == other.track && language == other.language; }
};

Preset load(subtitle_preferences::Player player);
bool save(subtitle_preferences::Player player, const Preset &preset);
std::optional<int> parse_track(std::string_view value);
std::optional<std::string> parse_language(std::string_view value);
std::string_view language_name(const std::string &code);

class AutoSelect {
public:
    void refresh(subtitle_preferences::Player player, LibVlcRef &library);

private:
    Preset preset_;
    InputRef input_{nullptr, vlc_object_release};
    bool applied_ = false;
};

}
