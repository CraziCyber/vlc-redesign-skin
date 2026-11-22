#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "player_window.hpp"

#include <commctrl.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <memory>
#include <string>
#include <string_view>

extern "C" {
__declspec(dllimport) char *config_GetPsz(void *, const char *);
__declspec(dllimport) void config_PutPsz(void *, const char *, const char *);
__declspec(dllimport) void libvlc_free(void *);
}

namespace player_window {
namespace {

// Default player dimensions and layout minimums in build.py.
constexpr LONG default_width = 1280, default_height = 720;
constexpr LONG player_min_width = 860, playlist_min_width = 1040, min_height = 420;
constexpr UINT_PTR layout_subclass = 1;

LONG edge(LONG origin, LONG size) {
    return static_cast<LONG>(std::clamp(static_cast<long long>(origin) + size,
                                      static_cast<long long>(std::numeric_limits<LONG>::min()),
                                      static_cast<long long>(std::numeric_limits<LONG>::max())));
}

struct LayoutChange {
    LONG width, height;
    LONG layout_width, layout_height;
};

LRESULT CALLBACK keep_window_size(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
    if (message == WM_WINDOWPOSCHANGING) {
        auto &position = *reinterpret_cast<WINDOWPOS *>(lp);
        if (!(position.flags & SWP_NOSIZE)) {
            auto &change = *reinterpret_cast<LayoutChange *>(data);
            change.layout_width = position.cx;
            change.layout_height = position.cy;
            position.cx = change.width;
            position.cy = change.height;
        }
    } else if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(window, keep_window_size, id);
    }
    return DefSubclassProc(window, message, wp, lp);
}

void resize_skin_layout(HWND window, const LayoutChange &change) {
    // Drive the skin's own corner grip so its video rectangle and anchors resize too.
    constexpr int grip_inset = 4;
    constexpr LONG limit = std::numeric_limits<short>::max();
    if ((change.layout_width == change.width && change.layout_height == change.height) ||
        change.layout_width < grip_inset || change.layout_height < grip_inset ||
        std::max({change.layout_width, change.layout_height, change.width, change.height}) > limit)
        return;
    const LPARAM corner = MAKELPARAM(change.layout_width - grip_inset, change.layout_height - grip_inset);
    const LPARAM target = MAKELPARAM(change.width - grip_inset, change.height - grip_inset);
    SendMessageW(window, WM_MOUSEMOVE, 0, corner);
    SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, corner);
    SendMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, target);
    SendMessageW(window, WM_LBUTTONUP, 0, target);
}

}

void restore_startup_bounds(void *vlc) {
    constexpr const char *option = "skins2-config";
    constexpr std::string_view prefix = "[\"main\" \"";
    std::unique_ptr<char, decltype(&libvlc_free)> saved(config_GetPsz(vlc, option), libvlc_free);
    if (!saved)
        return;
    std::string config(saved.get());
    const auto entry = config.find(prefix);
    if (entry == std::string::npos)
        return;
    const auto layout_end = config.find('"', entry + prefix.size());
    if (layout_end == std::string::npos)
        return;
    const std::string_view layout(config.data() + entry + prefix.size(), layout_end - entry - prefix.size());
    const bool sidebar = layout == "dark_side" || layout == "light_side";
    if (!sidebar && layout != "dark_full" && layout != "light_full")
        return;

    const auto end = config.find(']', layout_end + 1);
    if (end == std::string::npos)
        return;
    const char *p = config.data() + layout_end + 1;
    const char *last = config.data() + end;
    std::array<LONG, 5> fields{};
    for (auto &field : fields) {
        if (p == last || *p != ' ')
            return;
        while (p != last && *p == ' ')
            ++p;
        const auto number = std::from_chars(p, last, field);
        if (number.ec != std::errc{})
            return;
        p = number.ptr;
    }
    const auto [x, y, width, height, visible] = fields;
    if (p != last || visible != 1 || width <= 0 || height <= 0)
        return;

    const RECT bounds{x, y, edge(x, width), edge(y, height)};
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (!GetMonitorInfoW(MonitorFromRect(&bounds, MONITOR_DEFAULTTONEAREST), &monitor))
        return;
    const RECT &work = monitor.rcWork;
    if (bounds.left > work.left || bounds.top > work.top || bounds.right < work.right || bounds.bottom < work.bottom)
        return;

    // Skins2 stores a maximized player as ordinary bounds, so it can reopen over the whole display.
    const LONG work_width = work.right - work.left, work_height = work.bottom - work.top;
    const LONG normal_width = std::clamp(work_width * 4 / 5, sidebar ? playlist_min_width : player_min_width,
                                         default_width);
    const LONG normal_height = std::clamp(work_height * 4 / 5, min_height, default_height);
    const LONG left = work.left + std::max(0L, (work_width - normal_width) / 2);
    const LONG top = work.top + std::max(0L, (work_height - normal_height) / 2);
    config.replace(layout_end + 1, end - layout_end - 1,
                   " " + std::to_string(left) + " " + std::to_string(top) + " " +
                   std::to_string(normal_width) + " " + std::to_string(normal_height) + " 1");
    config_PutPsz(vlc, option, config.c_str());
}

void toggle_playlist(HWND window, LPARAM position, bool release_only) {
    RECT before{};
    if (!GetClientRect(window, &before))
        return;
    LayoutChange change{std::max(before.right, playlist_min_width), before.bottom, before.right, before.bottom};
    const bool keep_size = before.bottom >= min_height &&
        SetWindowSubclass(window, keep_window_size, layout_subclass, reinterpret_cast<DWORD_PTR>(&change));
    if (!release_only) {
        SendMessageW(window, WM_MOUSEMOVE, 0, position);
        SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, position);
    }
    SendMessageW(window, WM_LBUTTONUP, 0, position);
    if (keep_size) {
        RemoveWindowSubclass(window, keep_window_size, layout_subclass);
    } else {
        RECT after{};
        if (!GetClientRect(window, &after))
            return;
        change.layout_width = after.right;
        change.layout_height = after.bottom;
    }

    // The mini player intentionally switches to the regular player.
    if (before.bottom >= min_height) {
        resize_skin_layout(window, change);
        InvalidateRect(window, nullptr, FALSE);
    }
    POINT mouse{};
    if (GetCursorPos(&mouse) && ScreenToClient(window, &mouse))
        SendMessageW(window, WM_MOUSEMOVE, 0, MAKELPARAM(mouse.x, mouse.y));
}

}
