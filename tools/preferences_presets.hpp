#pragma once

#include <string>

#include "vlc_input.hpp"

namespace subtitle_preferences { enum class Timing; }

namespace preferences_presets {

inline constexpr char names_key[] = "redesign-preset-names";
inline constexpr char settings_key[] = "redesign-preset-settings";

// An empty name represents Default Preset.
bool select(VlcPlayer player, const std::string &previous, const std::string &selected, const std::string &catalog);
bool rename(VlcPlayer player, const std::string &previous, const std::string &renamed, const std::string &catalog);
bool remove(VlcPlayer player, const std::string &name, const std::string &catalog);
void set_audio_delay(VlcPlayer player, double seconds);
void set_subtitle_timing(VlcPlayer player, subtitle_preferences::Timing field, double value);
void refresh(VlcPlayer player, Input *input);
void release(VlcPlayer player);

}
