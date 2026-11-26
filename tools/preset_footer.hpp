#pragma once

#include <windows.h>

#include "vlc_input.hpp"

namespace preset_footer {

inline constexpr int height = 118;

HWND create(HWND parent, RECT area, LONG content_top, VlcPlayer player, bool light_theme, void (*refresh_page)());
void close(HWND footer);
bool handle_input(HWND footer, const MSG &message);
bool owns_input_target(HWND footer, HWND target);

}
