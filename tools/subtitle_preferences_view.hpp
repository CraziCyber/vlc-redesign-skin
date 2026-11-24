#pragma once

#include <windows.h>

#include "subtitle_preferences.hpp"

namespace subtitle_preferences_view {

HWND create(HWND parent, HWND video_owner, const RECT &bounds, subtitle_preferences::Player player, bool light);
void set_visible(HWND window, bool visible);
bool handle_input(HWND window, const MSG &message);
bool modal_open(HWND window);

}
