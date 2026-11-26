#pragma once

#include <windows.h>

namespace player_window {

void restore_startup_bounds(void *vlc);
void toggle_playlist(HWND window, LPARAM position, bool release_only);
void press_layout_button(HWND window, LPARAM position, bool release_only);
void resize_layout(HWND window, LONG width, LONG height);

}
