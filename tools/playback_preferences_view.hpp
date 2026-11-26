#pragma once

#include <windows.h>

#include "vlc_input.hpp"

namespace playback_preferences_view {

HWND create(HWND parent, HWND video_owner, const RECT &bounds, VlcPlayer player, bool light);
void set_visible(HWND window, bool visible);
bool handle_input(HWND window, const MSG &message);

}
