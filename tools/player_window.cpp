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
constexpr std::string_view main_entry = "[\"main\" \"";

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

void click_layout(HWND window, LPARAM position, bool release_only, const RECT &before, LONG target_width) {
    LayoutChange change{target_width, before.bottom, before.right, before.bottom};
    const bool preserve_size = target_width > 0;
    const bool keep_size = preserve_size &&
        SetWindowSubclass(window, keep_window_size, layout_subclass, reinterpret_cast<DWORD_PTR>(&change));
    if (!release_only) {
        SendMessageW(window, WM_MOUSEMOVE, 0, position);
        SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, position);
    }
    SendMessageW(window, WM_LBUTTONUP, 0, position);
    if (keep_size) {
        RemoveWindowSubclass(window, keep_window_size, layout_subclass);
    } else if (preserve_size) {
        RECT after{};
        if (!GetClientRect(window, &after))
            return;
        change.layout_width = after.right;
        change.layout_height = after.bottom;
    }

    if (preserve_size) {
        resize_skin_layout(window, change);
        InvalidateRect(window, nullptr, FALSE);
    }
    POINT mouse{};
    if (GetCursorPos(&mouse) && ScreenToClient(window, &mouse))
        SendMessageW(window, WM_MOUSEMOVE, 0, MAKELPARAM(mouse.x, mouse.y));
}

void dismiss_saved_preferences(std::string &config) {
    const auto panel = config.find("[\"preferenceswin\" ");
    if (panel != std::string::npos) {
        const auto end = config.find(']', panel);
        if (end != std::string::npos)
            config.erase(panel, end - panel + 1);
    }
    const auto main = config.find(main_entry);
    if (main == std::string::npos)
        return;
    constexpr std::string_view suffix = "_preferences";
    const auto begin = main + main_entry.size();
    const auto end = config.find('"', begin);
    if (end != std::string::npos && end - begin > suffix.size() &&
        config.compare(end - suffix.size(), suffix.size(), suffix) == 0)
        config.erase(end - suffix.size(), suffix.size());
}

}

void restore_startup_bounds(void *vlc) {
    constexpr const char *option = "skins2-config";
    std::unique_ptr<char, decltype(&libvlc_free)> saved(config_GetPsz(vlc, option), libvlc_free);
    if (!saved)
        return;
    std::string config(saved.get());
    dismiss_saved_preferences(config);
    if (config != saved.get())
        config_PutPsz(vlc, option, config.c_str());
    const auto entry = config.find(main_entry);
    if (entry == std::string::npos)
        return;
    const auto layout_start = entry + main_entry.size();
    const auto layout_end = config.find('"', layout_start);
    if (layout_end == std::string::npos)
        return;
    const std::string_view layout(config.data() + layout_start, layout_end - layout_start);
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
    // The mini player intentionally switches to the regular player.
    if (GetClientRect(window, &before))
        click_layout(window, position, release_only, before,
                     before.bottom >= min_height ? std::max(before.right, playlist_min_width) : 0);
}

void press_layout_button(HWND window, LPARAM position, bool release_only) {
    RECT before{};
    if (GetClientRect(window, &before))
        click_layout(window, position, release_only, before, before.right);
}

void resize_layout(HWND window, LONG width, LONG height) {
    RECT before{};
    if (GetClientRect(window, &before))
        resize_skin_layout(window, {width, height, before.right, before.bottom});
}

}
