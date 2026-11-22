#pragma once

#include <windows.h>

namespace hidden_controls {

void toggle(HWND player);
bool hidden(HWND player);
HWND player_for_overlay(HWND window);
void playlist_layout_changed(HWND player);

}
