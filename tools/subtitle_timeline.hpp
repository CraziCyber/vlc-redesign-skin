#pragma once

#include <windows.h>
#include <string_view>

#include "vlc_input.hpp"

namespace subtitle_timeline {

// The panel worker reads cues. The Skins2 thread owns the timeline controls.
bool refresh(VlcPlayer player, Input *input, LibVlcRef &metadata, std::string_view loaded_subtitle);
void release();
UINT update_message();
void update_window(HWND window, bool show_timeline, bool fullscreen_toolbar);

}
