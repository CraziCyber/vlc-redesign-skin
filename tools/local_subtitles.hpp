#pragma once

#include <windows.h>

#include <atomic>
#include <string>
#include <string_view>
#include <vector>

namespace local_subtitles {
bool is_srt_file(std::wstring_view path);
std::vector<std::vector<std::wstring>> find_matches(const std::vector<std::wstring> &media,
    const std::atomic_bool &cancelled);
HRESULT browse(HWND owner, std::wstring_view media_path, std::wstring &selected);
}
