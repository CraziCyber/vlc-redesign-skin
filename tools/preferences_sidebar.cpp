#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define UNICODE
#include "preferences_sidebar.hpp"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <cwchar>
#include <iterator>
#include <memory>
#include <utility>

#include "audio_preferences_view.hpp"
#include "context_menu_assets.hpp"
#include "playback_preferences_view.hpp"
#include "player_window.hpp"
#include "preferences_layout.hpp"
#include "preferences_widgets.hpp"
#include "preset_footer_icons.hpp"
#include "subtitle_preferences_view.hpp"
#include "video_preferences_view.hpp"

namespace preferences_sidebar {
namespace {

using namespace preferences_layout;
using namespace preferences_widgets;
constexpr UINT_PTR owner_subclass = 1;
constexpr UINT_PTR popup_subclass = 1;
const LPARAM dismiss_position = MAKELPARAM(context_menu::command_x,
                                          static_cast<WORD>(context_menu::SkinCommand::dismiss_preferences));

struct Page {
    HWND (*create)(HWND, HWND, const RECT &, VlcPlayer, bool);
    void (*set_visible)(HWND, bool);
    bool (*handle_input)(HWND, const MSG &);
    bool (*modal_open)(HWND);
    bool has_presets;
};

constexpr Page pages[] = {
    {subtitle_preferences_view::create, subtitle_preferences_view::set_visible,
     subtitle_preferences_view::handle_input, subtitle_preferences_view::modal_open, true},
    {audio_preferences_view::create, audio_preferences_view::set_visible,
     audio_preferences_view::handle_input, nullptr, true},
    {video_preferences_view::create, video_preferences_view::set_visible,
     video_preferences_view::handle_input, video_preferences_view::modal_open, true},
    {playback_preferences_view::create, playback_preferences_view::set_visible,
     playback_preferences_view::handle_input, nullptr, false},
};

class PresetFooter {
public:
    static constexpr int height = 118;

    PresetFooter(const Colors &colors, HINSTANCE module) : colors_(colors) {
        icons_.load(module, preset_footer_icons::resource, preset_footer_icons::size, preset_footer_icons::count);
    }

    ~PresetFooter() { DeleteObject(font_); }

    static HWND create(HWND parent, RECT area, bool light_theme) {
        HINSTANCE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(&procedure), &module))
            return nullptr;
        WNDCLASSW cls{};
        cls.hInstance = module;
        cls.lpfnWndProc = procedure;
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        cls.lpszClassName = L"VlcRedesignPresetFooter";
        if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return nullptr;

        auto footer = std::make_unique<PresetFooter>(light_theme ? light : dark, module);
        const LONG width = area.right - area.left;
        const auto window = CreateWindowExW(WS_EX_COMPOSITED, cls.lpszClassName, nullptr,
                                            WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                                            area.left, area.top, width, area.bottom - area.top,
                                            parent, nullptr, module, nullptr);
        if (!window)
            return nullptr;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(footer.release()));
        constexpr DWORD style = WS_CHILD | WS_VISIBLE | WS_DISABLED | WS_CLIPSIBLINGS | BS_OWNERDRAW;
        constexpr int inset = 16;
        if (!CreateWindowExW(0, L"BUTTON", L"Default Preset", style, inset, 21, width - 2 * inset, 44,
                             window, reinterpret_cast<HMENU>(preset_button), module, nullptr) ||
            !CreateWindowExW(0, L"BUTTON", L"Open Advanced Settings", style, inset, 77, width - 2 * inset, 32,
                             window, reinterpret_cast<HMENU>(advanced_button), module, nullptr)) {
            DestroyWindow(window);
            return nullptr;
        }
        return window;
    }

private:
    static constexpr INT_PTR preset_button = 1, advanced_button = 2;
    const Colors &colors_;
    HFONT font_ = make_font(16);
    IconStrip icons_;

    void draw_background(HDC dc, RECT area) const {
        fill(dc, area, colors_.panel);
        fill(dc, {area.left, area.top, area.right, area.top + 1}, colors_.edge);
    }

    void draw_button(const DRAWITEMSTRUCT &item) const {
        const auto area = item.rcItem;
        const auto dc = item.hDC;
        const auto label = window_text(item.hwndItem);
        constexpr int size = preset_footer_icons::size;
        const LONG center = (area.top + area.bottom) / 2;
        fill(dc, area, colors_.panel);
        if (item.CtlID == preset_button) {
            round_rect(dc, area, 6, colors_.track);
            icons_.draw(dc, area.right - 29 - size / 2, center - size / 2, preset_footer_icons::chevron);
            text(dc, label.c_str(), {area.left + 17, area.top, area.right - 48, area.bottom}, font_, colors_.text);
        } else {
            const auto old = SelectObject(dc, font_);
            SIZE label_size{};
            GetTextExtentPoint32W(dc, label.c_str(), static_cast<int>(label.size()), &label_size);
            SelectObject(dc, old);
            constexpr int gap = 5;
            const LONG x = (area.left + area.right - size - gap - label_size.cx) / 2;
            icons_.draw(dc, x, center - size / 2, preset_footer_icons::settings);
            text(dc, label.c_str(), {x + size + gap, area.top, area.right, area.bottom}, font_, colors_.dim);
        }
    }

    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wp, LPARAM lp) {
        const auto footer = reinterpret_cast<PresetFooter *>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (footer) {
            switch (message) {
            case WM_ERASEBKGND: return 1;
            case WM_PAINT:
            case WM_PRINTCLIENT: {
                RECT area{};
                GetClientRect(window, &area);
                if (message == WM_PAINT)
                    paint(window, area, [&](HDC dc) { footer->draw_background(dc, area); });
                else
                    footer->draw_background(reinterpret_cast<HDC>(wp), area);
                return 0;
            }
            case WM_DRAWITEM:
                footer->draw_button(*reinterpret_cast<const DRAWITEMSTRUCT *>(lp));
                return TRUE;
            case WM_NCDESTROY:
                SetWindowLongPtrW(window, GWLP_USERDATA, 0);
                delete footer;
                break;
            }
        }
        return DefWindowProcW(window, message, wp, lp);
    }
};

struct Sidebar {
    HWND owner = nullptr;
    HWND toolbar = nullptr;
    HWND popup = nullptr;
    VlcPlayer player{};
    HWND views[std::size(pages)]{};
    HWND footer = nullptr;
    bool moved_during_click = false;
    int tab = 1;
    int pressed_tab = 0;
};

thread_local Sidebar sidebar;
thread_local HWND pressed_gear = nullptr;
std::atomic<HWND> active_owner{nullptr};

const Page *current_page() {
    return sidebar.tab > 0 && static_cast<std::size_t>(sidebar.tab) <= std::size(pages) ? &pages[sidebar.tab - 1] : nullptr;
}

HWND current_view() {
    return current_page() ? sidebar.views[sidebar.tab - 1] : nullptr;
}

bool modal_open() {
    const auto page = current_page();
    return page && page->modal_open && page->modal_open(current_view());
}

UINT dismiss_message() {
    static const UINT id = RegisterWindowMessageW(L"VlcRedesign.DismissPreferencesSidebar");
    return id;
}

LRESULT CALLBACK follow_owner(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR);
LRESULT CALLBACK follow_popup(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR);

void stop_tracking() {
    active_owner.store(nullptr);
    const auto previous = std::exchange(sidebar, {});
    if (previous.owner)
        RemoveWindowSubclass(previous.owner, follow_owner, owner_subclass);
    if (previous.popup)
        RemoveWindowSubclass(previous.popup, follow_popup, popup_subclass);
    if (previous.footer)
        DestroyWindow(previous.footer);
    for (std::size_t i = 0; i < std::size(pages); ++i) {
        if (const auto view = previous.views[i]; view && IsWindow(view)) {
            pages[i].set_visible(view, false);
            DestroyWindow(view);
        }
    }
}

void dismiss() {
    const HWND target = sidebar.popup ? sidebar.popup : sidebar.owner;
    stop_tracking();
    if (target)
        player_window::press_layout_button(target, dismiss_position, false);
}

HWND panel_parent() {
    return sidebar.popup ? sidebar.popup : sidebar.owner;
}

POINT panel_origin(const RECT &client) {
    return sidebar.popup ? POINT{} : POINT{client.right - margin - width, top};
}

void place_view() {
    const auto page = current_page();
    if (!page)
        return;
    const HWND parent = panel_parent();
    RECT client{};
    if (!GetClientRect(parent, &client))
        return;
    const auto origin = panel_origin(client);
    RECT view{origin.x + body_left, origin.y + header + 1, origin.x + width - body_right,
              client.bottom - (sidebar.popup ? 0 : bottom) - body_bottom - corner};
    if (view.bottom <= view.top)
        return;
    const bool show_footer = page->has_presets && view.bottom - view.top > PresetFooter::height;
    const RECT footer{view.left, view.bottom - PresetFooter::height, view.right, view.bottom};
    auto &window = sidebar.views[sidebar.tab - 1];
    bool light_theme = false;
    if (!window || (show_footer && !sidebar.footer)) {
        UpdateWindow(parent);
        const HDC dc = GetDC(parent);
        light_theme = dc && GetPixel(dc, view.left + 2, view.top + 2) == light.panel;
        if (dc)
            ReleaseDC(parent, dc);
    }
    if (show_footer && !sidebar.footer)
        sidebar.footer = PresetFooter::create(parent, footer, light_theme);
    if (sidebar.footer) {
        if (show_footer) {
            view.bottom = footer.top;
            SetWindowPos(sidebar.footer, nullptr, footer.left, footer.top, footer.right - footer.left,
                         footer.bottom - footer.top, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        } else {
            ShowWindow(sidebar.footer, SW_HIDE);
        }
    }
    if (window) {
        SetWindowPos(window, nullptr, view.left, view.top, view.right - view.left, view.bottom - view.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        if (!IsWindowVisible(window))
            page->set_visible(window, true);
    } else {
        window = page->create(parent, sidebar.owner, view, sidebar.player, light_theme);
    }
}

int tab_at(const MSG &message) {
    RECT client{};
    if (message.hwnd != panel_parent() || !GetClientRect(message.hwnd, &client))
        return 0;
    const auto origin = panel_origin(client);
    const int x = GET_X_LPARAM(message.lParam) - origin.x - body_left;
    const int y = GET_Y_LPARAM(message.lParam) - origin.y;
    constexpr int tab_width = (width - body_left - body_right) / tabs;
    if (x < 0 || x >= tab_width * tabs || y < tab_top || y >= header - 2)
        return 0;
    return tabs - x / tab_width;
}

void place_popup() {
    RECT stage{}, bar{};
    if (!GetWindowRect(sidebar.owner, &stage) || !GetWindowRect(sidebar.toolbar, &bar))
        return;
    const LONG panel_left = stage.right - margin - width, panel_top = stage.top + top;
    LONG panel_bottom = stage.bottom - bottom;
    if (bar.right > panel_left && bar.top >= stage.bottom - 2 * bottom)
        panel_bottom = std::min(panel_bottom, bar.top - 8);
    player_window::resize_layout(sidebar.popup, width, panel_bottom - panel_top);
    SetWindowPos(sidebar.popup, HWND_TOPMOST, panel_left, panel_top, 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    InvalidateRect(sidebar.popup, nullptr, FALSE);
    place_view();
}

LRESULT CALLBACK follow_popup(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (message == WM_NCDESTROY && sidebar.popup == window)
        stop_tracking();
    const auto result = DefSubclassProc(window, message, wp, lp);
    // Skins2 can finish resizing the popup after the gear handler has created
    // the viewport. Use the resulting window size to fit the controls.
    if (message == WM_WINDOWPOSCHANGED && sidebar.popup == window &&
        !(reinterpret_cast<const WINDOWPOS *>(lp)->flags & SWP_NOSIZE))
        place_view();
    return result;
}

LRESULT CALLBACK follow_owner(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (message == WM_NCDESTROY) {
        if (sidebar.popup)
            dismiss();
        else
            stop_tracking();
    } else if (!modal_open() &&
               ((message == WM_ACTIVATEAPP && !wp) || (message == WM_WINDOWPOSCHANGED &&
               (reinterpret_cast<const WINDOWPOS *>(lp)->flags & SWP_HIDEWINDOW)))) {
        PostMessageW(window, dismiss_message(), 0, 0);
    }
    const auto result = DefSubclassProc(window, message, wp, lp);
    if (message == WM_WINDOWPOSCHANGED && sidebar.owner == window && IsWindowVisible(window)) {
        constexpr UINT unchanged = SWP_NOMOVE | SWP_NOSIZE;
        if ((reinterpret_cast<const WINDOWPOS *>(lp)->flags & unchanged) != unchanged) {
            if (sidebar.popup)
                place_popup();
            else {
                sidebar.moved_during_click = true;
                place_view();
            }
        }
    }
    return result;
}

HWND visible_popup() {
    HWND found = nullptr;
    EnumThreadWindows(GetCurrentThreadId(), [](HWND window, LPARAM data) -> BOOL {
        wchar_t name[32];
        RECT bounds{};
        if (!IsWindowVisible(window) || !GetClientRect(window, &bounds) || bounds.right != width ||
            !GetClassNameW(window, name, 32) || std::wcscmp(name, L"SkinWindowClass") != 0)
            return TRUE;
        *reinterpret_cast<HWND *>(data) = window;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&found));
    return found;
}

void track(HWND toolbar, HWND owner, HWND popup, subtitle_preferences::Player player) {
    if (SetWindowSubclass(owner, follow_owner, owner_subclass, 0)) {
        sidebar = {owner, toolbar, popup, player};
        active_owner.store(owner);
        if (popup) {
            SetWindowSubclass(popup, follow_popup, popup_subclass, 0);
            place_popup();
        } else
            place_view();
    }
}

bool click_released(UINT message) {
    return message == WM_LBUTTONUP || message == WM_RBUTTONUP || message == WM_MBUTTONUP;
}

bool escape_pressed(const MSG &message) {
    return message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE;
}

bool inside_sidebar(const MSG &message) {
    if (owns_input_target(message.hwnd))
        return true;
    if (sidebar.popup)
        return message.hwnd == sidebar.popup || IsChild(sidebar.popup, message.hwnd);
    RECT client{};
    if (message.hwnd != sidebar.owner || !GetClientRect(sidebar.owner, &client))
        return false;
    const RECT panel{client.right - margin - width, top, client.right - margin, client.bottom - bottom};
    return PtInRect(&panel, {GET_X_LPARAM(message.lParam), GET_Y_LPARAM(message.lParam)});
}

}

bool handle_gear(const MSG &message, subtitle_preferences::Player player, HWND fullscreen_surface) {
    const bool over_gear = player.vlc != nullptr;
    if (message.message == WM_LBUTTONDOWN || message.message == WM_LBUTTONDBLCLK) {
        pressed_gear = over_gear ? message.hwnd : nullptr;
        return false;
    }
    if (message.message != WM_LBUTTONUP)
        return false;
    const HWND pressed = std::exchange(pressed_gear, nullptr);
    if (!over_gear || pressed != message.hwnd)
        return false;

    const HWND toolbar = message.hwnd;
    const bool closing = sidebar.toolbar == toolbar;
    if (closing)
        stop_tracking();
    else if (sidebar.owner)
        dismiss();

    if (fullscreen_surface) {
        SendMessageW(toolbar, WM_LBUTTONUP, message.wParam, message.lParam);
        if (const HWND popup = visible_popup())
            track(toolbar, fullscreen_surface, popup, player);
    } else {
        player_window::press_layout_button(toolbar, message.lParam, true);
        if (!closing)
            track(toolbar, toolbar, nullptr, player);
    }
    return true;
}

bool handle_message(const MSG &message) {
    if (!sidebar.owner || modal_open())
        return false;
    if (const auto page = current_page(); page && page->handle_input(current_view(), message))
        return true;
    if (message.message == WM_MOUSEWHEEL && sidebar.footer && IsWindowVisible(sidebar.footer)) {
        RECT footer{};
        const POINT point{GET_X_LPARAM(message.lParam), GET_Y_LPARAM(message.lParam)};
        if (const auto view = current_view(); view && GetWindowRect(sidebar.footer, &footer) && PtInRect(&footer, point)) {
            SendMessageW(view, WM_MOUSEWHEEL, message.wParam, message.lParam);
            return true;
        }
    }
    if ((message.message == dismiss_message() && message.hwnd == sidebar.owner) || escape_pressed(message)) {
        dismiss();
        return true;
    }
    if (message.message == WM_LBUTTONDOWN || message.message == WM_LBUTTONDBLCLK) {
        sidebar.moved_during_click = false;
        sidebar.pressed_tab = tab_at(message);
    }
    if (!click_released(message.message))
        return false;
    if (message.message == WM_LBUTTONUP && message.hwnd == sidebar.owner && sidebar.moved_during_click)
        return false;
    if (message.message == WM_LBUTTONUP) {
        const int pressed = std::exchange(sidebar.pressed_tab, 0);
        if (pressed && pressed == tab_at(message)) {
            if (sidebar.tab != pressed) {
                if (const auto page = current_page())
                    page->set_visible(current_view(), false);
                sidebar.tab = pressed;
            }
            if (sidebar.popup)
                player_window::press_layout_button(sidebar.popup, message.lParam, true);
            else
                SendMessageW(message.hwnd, WM_LBUTTONUP, message.wParam, message.lParam);
            place_view();
            return true;
        }
    }
    if (!inside_sidebar(message)) {
        // Let another control handle the click before restoring the regular player layout.
        PostMessageW(sidebar.owner, dismiss_message(), 0, 0);
    }
    return false;
}

bool owns_input_target(HWND window) {
    const auto view = current_view();
    return (view && (window == view || IsChild(view, window) || modal_open())) ||
           (sidebar.footer && (window == sidebar.footer || IsChild(sidebar.footer, window)));
}

bool handle_video_event(const MSG &message) {
    const bool escape = escape_pressed(message);
    if (!escape && !click_released(message.message))
        return false;
    const HWND owner = active_owner.load();
    if (!owner || GetAncestor(message.hwnd, GA_ROOT) != owner)
        return false;
    const bool queued = PostMessageW(owner, dismiss_message(), 0, 0);
    return escape && queued;
}

}
