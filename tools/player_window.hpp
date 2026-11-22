#pragma once

#include <windows.h>

namespace player_window {

void restore_startup_bounds(void *vlc);
void toggle_playlist(HWND window, LPARAM position, bool release_only);

}
