#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace subtitle_drop {
using CanAttach = bool (*)();
using Attach = bool (*)(const std::vector<std::wstring> &);

UINT install_message();
void request(HWND window);
void install(HWND window, CanAttach can_attach, Attach attach);
}
