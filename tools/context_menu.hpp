#pragma once

#include <windows.h>

#include "context_menu_assets.hpp"

namespace context_menu {

struct Player {
    HWND window;
    HWND surface;
    void *vlc;
    bool playing;
    bool has_input;
    bool recording;
    bool ui_hidden;
    void (*run)(HWND window, HWND surface, const char *, SkinCommand);
};

void load_artwork(HINSTANCE module);
void unload_artwork();
bool show(const Player &player, POINT location, bool keyboard);
bool handle_message(const MSG &message);
bool is_open();
bool is_navigation_key(WPARAM key);
bool handle_key(WPARAM key, bool navigate);
bool hide_ui_shortcut_available(void *vlc);

}
