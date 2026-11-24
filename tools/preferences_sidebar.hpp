#pragma once

#include <windows.h>
#include "subtitle_preferences.hpp"

namespace preferences_sidebar {

bool handle_gear(const MSG &message, subtitle_preferences::Player player, HWND fullscreen_surface);
bool handle_message(const MSG &message);
bool handle_video_event(const MSG &message);
bool owns_input_target(HWND window);

}
