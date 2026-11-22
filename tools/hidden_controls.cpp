#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define UNICODE
#include "hidden_controls.hpp"

#include <commctrl.h>
#include <dwmapi.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <utility>

namespace hidden_controls {
namespace {

constexpr wchar_t overlay_class[] = L"VlcRedesignControlsOverlay";
constexpr UINT_PTR animation_timer = 1, owner_subclass = 1;
constexpr DWORD frame_interval = 15, hidden_interval = 80, duration_ms = 340;

// The player and fullscreen controller are laid out in build.py.
constexpr LONG top_height = 40, bottom_height = 54, side_margin = 10, sidebar_width = 427;
constexpr LONG mini_width = 391, mini_height = 339, mini_stage_bottom = 219, mini_margin = 8;
constexpr LONG controller_width = 1000, controller_height = 56;

RECT client_on_screen(HWND window) {
    RECT rect{};
    if (GetClientRect(window, &rect)) {
        MapWindowPoints(window, nullptr, reinterpret_cast<POINT *>(&rect), 2);
        return rect;
    }
    return {};
}

bool skin_window(HWND window) {
    wchar_t name[32];
    return GetClassNameW(window, name, 32) && std::wcscmp(name, L"SkinWindowClass") == 0;
}

BOOL CALLBACK find_fullscreen_bar(HWND window, LPARAM data) {
    RECT rect{};
    if (skin_window(window) && GetClientRect(window, &rect) &&
        rect.right == controller_width && rect.bottom == controller_height) {
        *reinterpret_cast<HWND *>(data) = window;
        return FALSE;
    }
    return TRUE;
}

bool has_sidebar(HWND player) {
    // With a video playing, use the skin's video host to distinguish the two layouts.
    for (HWND child = nullptr; (child = FindWindowExW(player, child, L"SkinWindowClass", nullptr));) {
        RECT rect = client_on_screen(child);
        if (IsWindowVisible(child) && rect.right > rect.left) {
            MapWindowPoints(nullptr, player, reinterpret_cast<POINT *>(&rect), 2);
            return rect.left == sidebar_width;
        }
    }
    // The upper gutter is above the artwork and title in the audio-only layout.
    const HDC dc = GetDC(player);
    if (!dc)
        return false;
    const COLORREF color = GetPixel(dc, sidebar_width - 7, top_height + 12);
    ReleaseDC(player, dc);
    return color == RGB(15, 15, 15) || color == RGB(194, 193, 193);
}

class Snapshot {
public:
    Snapshot() = default;
    Snapshot(const Snapshot &) = delete;
    Snapshot &operator=(const Snapshot &) = delete;
    ~Snapshot() { clear(); }

    bool capture(HWND source, LONG width, LONG height) {
        const HDC dc = GetDC(source);
        if (!dc)
            return false;
        const HDC copy = CreateCompatibleDC(dc);
        const HBITMAP bitmap = copy ? CreateCompatibleBitmap(dc, width, height) : nullptr;
        const HGDIOBJ previous = bitmap ? SelectObject(copy, bitmap) : nullptr;
        const bool copied = previous && previous != HGDI_ERROR &&
            BitBlt(copy, 0, 0, width, height, dc, 0, 0, SRCCOPY);
        ReleaseDC(source, dc);
        if (copied) {
            clear();
            dc_ = copy;
            bitmap_ = bitmap;
            previous_ = previous;
            return true;
        }
        if (previous && previous != HGDI_ERROR)
            SelectObject(copy, previous);
        if (bitmap)
            DeleteObject(bitmap);
        if (copy)
            DeleteDC(copy);
        return false;
    }

    void clear() {
        if (!dc_)
            return;
        SelectObject(dc_, previous_);
        DeleteObject(bitmap_);
        DeleteDC(dc_);
        dc_ = nullptr;
    }

    void slide(HDC target, RECT band, LONG x, LONG y) const {
        if (!dc_)
            return;
        RECT moved = band, visible{};
        OffsetRect(&moved, x, y);
        if (IntersectRect(&visible, &band, &moved))
            BitBlt(target, visible.left, visible.top, visible.right - visible.left,
                   visible.bottom - visible.top, dc_, visible.left - x, visible.top - y, SRCCOPY);
    }

private:
    HDC dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ previous_ = nullptr;
};

class Overlay {
public:
    HWND player = nullptr, window = nullptr, fullscreen_overlay = nullptr;
    bool hiding = false;

    void toggle(HWND owner) {
        const bool starting = player != owner;
        if (starting) {
            dismiss();
            if (!register_window())
                return;
            player = owner;
            sidebar_ = has_sidebar(player);
            if (!SetWindowSubclass(player, player_proc, owner_subclass, reinterpret_cast<DWORD_PTR>(this))) {
                player = nullptr;
                return;
            }
            window = create_window(player);
            if (!window) {
                dismiss();
                return;
            }
            sync_player();
            if (!window)
                return;
            ShowWindow(window, SW_SHOWNOACTIVATE);
        } else {
            advance();
            if (hiding)
                player_image_.capture(player, frame_.right - frame_.left, frame_.bottom - frame_.top);
        }

        hiding = !hiding;
        start_ = progress_;
        started_at_ = GetTickCount64();
        sync_fullscreen();
        InvalidateRect(window, nullptr, FALSE);
        if (!SetTimer(window, animation_timer, frame_interval, nullptr))
            finish();
    }

    void playlist_layout_changed() {
        sidebar_ = !sidebar_;
        sync_player(true);
    }

private:
    RECT frame_{}, stage_{}, fullscreen_frame_{};
    Snapshot player_image_, fullscreen_image_;
    HWND fullscreen_bar_ = nullptr;
    HINSTANCE module_ = nullptr;
    ULONGLONG started_at_ = 0;
    double start_ = 0, progress_ = 0;
    bool sidebar_ = false;

    bool register_window() {
        if (module_)
            return true;
        HINSTANCE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(&window_proc), &module))
            return false;
        WNDCLASSW cls{};
        cls.lpfnWndProc = window_proc;
        cls.hInstance = module;
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        cls.lpszClassName = overlay_class;
        if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return false;
        module_ = module;
        return true;
    }

    HWND create_window(HWND owner) {
        return CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, overlay_class, L"",
                               WS_POPUP, 0, 0, 0, 0, owner, nullptr, module_, this);
    }

    static LRESULT CALLBACK player_proc(HWND owner, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
        auto &overlay = *reinterpret_cast<Overlay *>(data);
        if (msg == WM_NCDESTROY)
            overlay.dismiss();
        const auto result = DefSubclassProc(owner, msg, wp, lp);
        if (msg == WM_WINDOWPOSCHANGED && overlay.player == owner)
            overlay.sync_player();
        return result;
    }

    static LRESULT CALLBACK window_proc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_NCCREATE) {
            const auto *create = reinterpret_cast<const CREATESTRUCTW *>(lp);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
        auto *overlay = reinterpret_cast<Overlay *>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (!overlay)
            return DefWindowProcW(window, msg, wp, lp);
        switch (msg) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            const HDC dc = BeginPaint(window, &paint);
            overlay->paint(window, dc);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_TIMER:
            if (wp == animation_timer)
                overlay->tick();
            return 0;
        case WM_NCDESTROY:
            if (window == overlay->window)
                overlay->window = nullptr;
            if (window == overlay->fullscreen_overlay)
                overlay->fullscreen_overlay = nullptr;
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            break;
        }
        return DefWindowProcW(window, msg, wp, lp);
    }

    void sync_player(bool force = false) {
        if (!window || IsIconic(player))
            return;
        const RECT rect = client_on_screen(player);
        if (!force && EqualRect(&rect, &frame_))
            return;
        const LONG width = rect.right - rect.left, height = rect.bottom - rect.top;
        if (width <= 0 || height <= 0)
            return;
        const bool resize = width != frame_.right - frame_.left || height != frame_.bottom - frame_.top;
        frame_ = rect;
        if (resize || force) {
            const bool mini = width == mini_width && height == mini_height;
            stage_ = mini ? RECT{mini_margin, mini_margin, width - mini_margin, mini_stage_bottom}
                          : RECT{sidebar_ ? sidebar_width : side_margin, top_height,
                                 width - side_margin, height - bottom_height};
            const int corner = mini ? 12 : 16;
            const HRGN cover = CreateRectRgn(0, 0, width, height);
            const HRGN stage = CreateRoundRectRgn(stage_.left, stage_.top, stage_.right, stage_.bottom, corner, corner);
            if (!cover || !stage || CombineRgn(cover, cover, stage, RGN_DIFF) == ERROR ||
                !SetWindowRgn(window, cover, FALSE)) {
                if (cover)
                    DeleteObject(cover);
                if (stage)
                    DeleteObject(stage);
                dismiss();
                return;
            }
            DeleteObject(stage);
            if (progress_ < 1) {
                player_image_.clear();
                player_image_.capture(player, width, height);
            }
        }
        SetWindowPos(window, nullptr, rect.left, rect.top, width, height,
                     SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER);
        InvalidateRect(window, nullptr, FALSE);
    }

    void paint(HWND target, HDC dc) const {
        RECT bounds{};
        GetClientRect(target, &bounds);
        if (target == fullscreen_overlay) {
            fullscreen_image_.slide(dc, bounds, 0, offset(bounds.bottom));
            return;
        }
        FillRect(dc, &bounds, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        if (progress_ >= 1)
            return;
        player_image_.slide(dc, {0, 0, bounds.right, stage_.top}, 0, -offset(stage_.top));
        player_image_.slide(dc, {0, stage_.bottom, bounds.right, bounds.bottom}, 0,
                            offset(bounds.bottom - stage_.bottom));
        player_image_.slide(dc, {0, stage_.top, stage_.left, stage_.bottom}, -offset(stage_.left), 0);
        player_image_.slide(dc, {stage_.right, stage_.top, bounds.right, stage_.bottom},
                            offset(bounds.right - stage_.right), 0);
    }

    LONG offset(LONG pixels) const {
        return static_cast<LONG>(std::lround(pixels * progress_));
    }

    void advance() {
        if (!started_at_)
            return;
        const double end = hiding ? 1 : 0;
        const double duration = std::max(70.0, duration_ms * std::abs(end - start_));
        const double t = std::min(1.0, (GetTickCount64() - started_at_) / duration);
        progress_ = start_ + (end - start_) * t * t * (3 - 2 * t);
    }

    void tick() {
        if (started_at_) {
            advance();
            if (progress_ == (hiding ? 1.0 : 0.0)) {
                finish();
                return;
            }
            InvalidateRect(window, nullptr, FALSE);
        }
        sync_fullscreen();
    }

    void finish() {
        started_at_ = 0;
        if (!hiding) {
            dismiss();
            return;
        }
        progress_ = 1;
        clear_fullscreen_overlay();
        player_image_.clear();
        InvalidateRect(window, nullptr, FALSE);
        SetTimer(window, animation_timer, hidden_interval, nullptr);
    }

    void sync_fullscreen() {
        HWND bar = nullptr;
        EnumThreadWindows(GetCurrentThreadId(), find_fullscreen_bar, reinterpret_cast<LPARAM>(&bar));
        if (bar != fullscreen_bar_) {
            clear_fullscreen_overlay();
            uncloak_fullscreen();
            const BOOL cloak = TRUE;
            if (!bar || !hiding)
                return;
            if (started_at_ && IsWindowVisible(bar))
                capture_fullscreen(bar);
            if (FAILED(DwmSetWindowAttribute(bar, DWMWA_CLOAK, &cloak, sizeof(cloak)))) {
                clear_fullscreen_overlay();
                return;
            }
            fullscreen_bar_ = bar;
        } else if (bar && !hiding && !fullscreen_overlay && started_at_ && IsWindowVisible(bar)) {
            capture_fullscreen(bar);
        }

        if (!fullscreen_overlay)
            return;
        if (!IsWindowVisible(bar) || !started_at_) {
            clear_fullscreen_overlay();
            return;
        }
        const LONG width = fullscreen_frame_.right - fullscreen_frame_.left;
        const LONG height = fullscreen_frame_.bottom - fullscreen_frame_.top;
        const HRGN region = CreateRectRgn(0, offset(height), width, height);
        if (region && !SetWindowRgn(fullscreen_overlay, region, TRUE))
            DeleteObject(region);
        InvalidateRect(fullscreen_overlay, nullptr, FALSE);
    }

    void capture_fullscreen(HWND bar) {
        const HWND owner = GetWindow(bar, GW_OWNER);
        if (!owner)
            return;
        fullscreen_frame_ = client_on_screen(bar);
        const LONG width = fullscreen_frame_.right - fullscreen_frame_.left;
        const LONG height = fullscreen_frame_.bottom - fullscreen_frame_.top;
        if (width <= 0 || height <= 0 || !fullscreen_image_.capture(bar, width, height))
            return;
        fullscreen_overlay = create_window(owner);
        if (!fullscreen_overlay) {
            fullscreen_image_.clear();
            return;
        }
        SetWindowPos(fullscreen_overlay, HWND_TOP, fullscreen_frame_.left, fullscreen_frame_.top, width, height,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
    }

    void uncloak_fullscreen() {
        const BOOL cloak = FALSE;
        if (fullscreen_bar_ && IsWindow(fullscreen_bar_))
            DwmSetWindowAttribute(fullscreen_bar_, DWMWA_CLOAK, &cloak, sizeof(cloak));
        fullscreen_bar_ = nullptr;
    }

    void clear_fullscreen_overlay() {
        if (fullscreen_overlay)
            DestroyWindow(fullscreen_overlay);
        fullscreen_image_.clear();
    }

    void dismiss() {
        if (const HWND owner = std::exchange(player, nullptr))
            RemoveWindowSubclass(owner, player_proc, owner_subclass);
        uncloak_fullscreen();
        clear_fullscreen_overlay();
        if (window)
            DestroyWindow(window);
        player_image_.clear();
        frame_ = {};
        started_at_ = 0;
        progress_ = 0;
        hiding = false;
    }
};

thread_local Overlay overlay;

}

void toggle(HWND player) {
    if (player)
        overlay.toggle(player);
}

bool hidden(HWND player) {
    return overlay.player == player && overlay.hiding;
}

HWND player_for_overlay(HWND window) {
    return window && (window == overlay.window || window == overlay.fullscreen_overlay) ? overlay.player : nullptr;
}

void playlist_layout_changed(HWND player) {
    if (overlay.player == player)
        overlay.playlist_layout_changed();
}

}
