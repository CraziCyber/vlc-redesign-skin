#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define UNICODE
#include "playback_preferences_view.hpp"
#include "playback_preferences.hpp"
#include "preferences_widgets.hpp"
#include "vlc_config.hpp"

#include <memory>
#include <utility>

namespace playback_preferences_view {
namespace {

using namespace preferences_widgets;
using namespace playback_preferences;
constexpr wchar_t class_name[] = L"VlcRedesignPlaybackPreferences";
constexpr int inset = 22, option_height = 34, content_height = 444;
constexpr UINT_PTR refresh_timer = 1;

enum Control { timeline = 401, statistics, reverse, sleep, visualizer, speed, slider, options };
constexpr const wchar_t *labels[] = {
    L"Show Subtitle Timeline on Seekbar", L"Show Statistics", L"Reverse",
    L"Sleep Timer", L"Audio Visualizer", L"Speed", L"Playback speed",
};

std::wstring countdown(int seconds) {
    if (!seconds)
        return L"Off";
    wchar_t value[40];
    if (seconds >= 3600)
        std::swprintf(value, std::size(value), L"%d:%02d:%02d remaining", seconds / 3600, seconds / 60 % 60, seconds % 60);
    else
        std::swprintf(value, std::size(value), L"%d:%02d remaining", seconds / 60, seconds % 60);
    return value;
}

class View {
public:
    View(VlcPlayer player, bool light_theme) : player_(player), colors_(light_theme ? light : dark) {}
    ~View() {
        DeleteObject(font_);
        DeleteObject(small_);
    }

    static View *get(HWND window) {
        return reinterpret_cast<View *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    }

    static HWND open(HWND parent, RECT area, VlcPlayer player, bool light_theme) {
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(&procedure), &module))
            return nullptr;
        WNDCLASSW cls{};
        cls.hInstance = module;
        cls.lpfnWndProc = procedure;
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        cls.lpszClassName = class_name;
        if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return nullptr;

        auto view = std::make_unique<View>(player, light_theme);
        const auto window = CreateWindowExW(WS_EX_CONTROLPARENT | WS_EX_COMPOSITED, class_name, L"Playback Preferences",
            WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, area.left, area.top,
            area.right - area.left, area.bottom - area.top, parent, nullptr, module, view.get());
        if (window)
            view.release()->window_owned_ = true;
        return window;
    }

    void visible(bool show) {
        if (show) {
            refresh();
            SetTimer(window_, refresh_timer, 250, nullptr);
        } else {
            KillTimer(window_, refresh_timer);
            save_speed();
            close_dropdown(false);
            if (IsChild(window_, GetFocus()) || GetFocus() == window_)
                SetFocus(GetParent(window_));
        }
        ShowWindow(window_, show ? SW_SHOWNOACTIVATE : SW_HIDE);
    }

    bool input(const MSG &message) {
        if (!IsWindowVisible(window_))
            return false;
        if (dropdown_ && dropdown_input(message))
            return true;
        if (wheel_over(window_, message)) {
            wheel(GET_WHEEL_DELTA_WPARAM(message.wParam));
            return true;
        }
        if (message.message != WM_KEYDOWN || (message.hwnd != window_ && !IsChild(window_, message.hwnd)))
            return false;
        if (message.wParam == VK_TAB) {
            const auto current = IsChild(window_, GetFocus()) ? GetFocus() : nullptr;
            if (const auto next = GetNextDlgTabItem(window_, current, GetKeyState(VK_SHIFT) < 0)) {
                reveal(next);
                SetFocus(next);
            }
            return true;
        }
        if (GetDlgCtrlID(message.hwnd) == speed) {
            if (message.wParam == 'A' && GetKeyState(VK_CONTROL) < 0) {
                SendMessageW(message.hwnd, EM_SETSEL, 0, -1);
                return true;
            }
            if (message.wParam == VK_RETURN || message.wParam == VK_ESCAPE) {
                if (message.wParam == VK_ESCAPE)
                    SetWindowTextW(message.hwnd, original_speed_.c_str());
                SetFocus(window_);
                return true;
            }
        } else if (message.wParam == VK_RETURN && message.hwnd != window_ && GetDlgCtrlID(message.hwnd) != slider) {
            SendMessageW(message.hwnd, BM_CLICK, 0, 0);
            return true;
        }
        return false;
    }

private:
    VlcPlayer player_;
    const Colors &colors_;
    HWND window_ = nullptr;
    HFONT font_ = nullptr, small_ = nullptr;
    Scroll scroll_{content_height};
    Settings current_{};
    std::optional<Control> dropdown_;
    std::wstring original_speed_;
    int option_wheel_ = 0;
    bool window_owned_ = false, speed_dirty_ = false, save_failed_ = false;

    HWND control(int id) const { return GetDlgItem(window_, id); }
    RECT bounds() const {
        RECT area{};
        GetClientRect(window_, &area);
        return area;
    }

    RECT box(Control id) const {
        const LONG right = bounds().right - inset;
        const int y = -scroll_.offset();
        if (id <= reverse) {
            const int top = y + 20 + (id - timeline) * 52;
            return {right - 68, top, right, top + 32};
        }
        if (id == sleep || id == visualizer) {
            const int top = y + 176 + (id - sleep) * 68;
            return {right - 220, top, right, top + 48};
        }
        if (id == speed)
            return {right - 142, y + 317, right, y + 351};
        return {inset - 7, y + 367, right + 7, y + 397};
    }

    bool initialize() {
        INITCOMMONCONTROLSEX common{sizeof(common), ICC_BAR_CLASSES};
        InitCommonControlsEx(&common);
        font_ = make_font(15);
        small_ = make_font(12);
        const auto module = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(window_, GWLP_HINSTANCE));
        for (int id = timeline; id <= options; ++id) {
            const bool list = id == options;
            const auto type = list ? L"LISTBOX" : id == speed ? L"EDIT" : id == slider ? TRACKBAR_CLASSW : L"BUTTON";
            const DWORD style = list ? LBS_HASSTRINGS | LBS_OWNERDRAWFIXED | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT :
                id == speed ? ES_AUTOHSCROLL : id == slider ? TBS_NOTICKS | TBS_FIXEDLENGTH | TBS_TRANSPARENTBKGND : BS_OWNERDRAW;
            const auto child = CreateWindowExW(0, type, list || id == speed ? L"" : labels[id - timeline],
                WS_CHILD | WS_CLIPSIBLINGS | WS_TABSTOP | (list ? 0 : WS_VISIBLE) | style, 0, 0, 0, 0, window_,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), module, nullptr);
            if (!child)
                return false;
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font_), FALSE);
        }
        SendMessageW(control(speed), EM_SETLIMITTEXT, 16, 0);
        SendMessageW(control(options), LB_SETITEMHEIGHT, 0, option_height);
        const auto scale = control(slider);
        SendMessageW(scale, TBM_SETRANGEMIN, FALSE, 25);
        SendMessageW(scale, TBM_SETRANGEMAX, FALSE, 200);
        SendMessageW(scale, TBM_SETLINESIZE, 0, 1);
        SendMessageW(scale, TBM_SETPAGESIZE, 0, 25);
        SendMessageW(scale, TBM_SETTHUMBLENGTH, 27, 0);
        layout();
        refresh();
        SetTimer(window_, refresh_timer, 250, nullptr);
        return true;
    }

    void layout() {
        for (int id = timeline; id <= slider; ++id) {
            auto area = box(static_cast<Control>(id));
            if (id == speed) {
                area.left += 12;
                area.right -= 8;
                area.top += 7;
                area.bottom -= 5;
            }
            SetWindowPos(control(id), nullptr, area.left, area.top, area.right - area.left, area.bottom - area.top,
                         SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS | SWP_NOREDRAW);
        }
        RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_NOERASE);
    }

    void reveal(HWND child) {
        if (scroll_.show(window_, child, bounds()))
            layout();
    }

    void wheel(int delta) {
        if (scroll_.wheel(delta, bounds()))
            layout();
    }

    void refresh() {
        const auto next = settings(player_);
        if (current_.timeline != next.timeline)
            InvalidateRect(control(timeline), nullptr, FALSE);
        if (current_.statistics != next.statistics)
            InvalidateRect(control(statistics), nullptr, FALSE);
        if (IsWindowEnabled(control(reverse)) != (next.can_reverse || next.reverse))
            EnableWindow(control(reverse), next.can_reverse || next.reverse);
        if (current_.reverse != next.reverse || current_.can_reverse != next.can_reverse)
            InvalidateRect(control(reverse), nullptr, FALSE);
        if (GetCapture() != control(slider)) {
            const int position = std::clamp<int>(std::lround(next.speed * 100), 25, 200);
            if (SendMessageW(control(slider), TBM_GETPOS, 0, 0) != position)
                SendMessageW(control(slider), TBM_SETPOS, TRUE, position);
        }
        current_ = next;
        set_unfocused_text(control(speed), number_text(current_.speed, 2, L"x"));
        const auto timer_text = countdown(current_.remaining_seconds);
        const auto visualizer_text = current_.visualizer >= 0 ? visualizers[current_.visualizer].label : L"Custom";
        if (window_text(control(sleep)) != timer_text)
            SetWindowTextW(control(sleep), timer_text.c_str());
        if (window_text(control(visualizer)) != visualizer_text)
            SetWindowTextW(control(visualizer), visualizer_text);
    }

    void save_speed() {
        if (std::exchange(speed_dirty_, false))
            save_failed_ = !vlc_config::save(player_.vlc);
    }

    void slide_speed(WORD event) {
        const auto value = static_cast<float>(SendMessageW(control(slider), TBM_GETPOS, 0, 0)) / 100;
        if (value != current_.speed) {
            set_speed(player_, value, false);
            current_.speed = value;
            set_unfocused_text(control(speed), number_text(value, 2, L"x"));
            speed_dirty_ = true;
        }
        if (event != TB_THUMBTRACK)
            save_speed();
    }

    bool is_on(Control id) const {
        return id == timeline ? current_.timeline : id == statistics ? current_.statistics : current_.reverse;
    }

    void draw(HDC dc) const {
        const auto area = bounds();
        fill(dc, area, colors_.panel);
        for (int id = timeline; id <= speed; ++id) {
            const auto field = box(static_cast<Control>(id));
            text(dc, labels[id - timeline], {inset, field.top, field.left - 8, field.bottom}, font_, colors_.dim);
        }
        round_rect(dc, box(speed), 6, colors_.track);
        const auto scale = box(slider);
        const LONG left = inset + 11, right = area.right - inset - 11;
        for (int i = 0; i <= 7; ++i) {
            const auto value = number_text((i + 1) / 4.0, 2, L"");
            const LONG x = left + (right - left) * i / 7;
            text(dc, value.c_str(), {x - 18, scale.bottom, x + 18, scale.bottom + 25}, small_, colors_.dim, DT_CENTER);
        }
        if (save_failed_)
            text(dc, L"VLC could not save this setting.", {inset, scale.bottom + 25, area.right - inset, scale.bottom + 45},
                 small_, colors_.text);
        if (scroll_.limit(area))
            round_rect(dc, scroll_.thumb(area), 2, colors_.knob);
    }

    void draw_item(const DRAWITEMSTRUCT &item) const {
        auto area = item.rcItem;
        const auto dc = item.hDC;
        const auto id = static_cast<Control>(item.CtlID);
        const bool selected = (item.itemState & ODS_SELECTED) != 0;
        if (id == options) {
            fill(dc, area, selected ? colors_.fill : colors_.button);
            if (item.itemID != UINT(-1) && dropdown_) {
                const auto label = *dropdown_ == sleep ? timers[item.itemID].label : visualizers[item.itemID].label;
                area.left += 15;
                area.right -= 8;
                text(dc, label, area, font_, selected ? colors_.accent : colors_.text);
            }
        } else {
            fill(dc, area, colors_.panel);
            if (id <= reverse) {
                draw_toggle(dc, area, is_on(id), colors_, small_);
            } else {
                round_rect(dc, area, 6, colors_.track);
                InflateRect(&area, -2, -2);
                round_rect(dc, area, 4, selected ? colors_.hover : colors_.button);
                const auto display = window_text(item.hwndItem);
                const auto color = id == sleep && current_.remaining_seconds ? colors_.accent : colors_.text;
                arrow(dc, area.right - 24, (area.top + area.bottom) / 2, Arrow::down, color);
                text(dc, display.c_str(), {area.left + 15, area.top, area.right - 38, area.bottom}, font_, color);
            }
        }
    }

    void open_dropdown(Control id) {
        close_dropdown(false);
        reveal(control(id));
        dropdown_ = id;
        option_wheel_ = 0;
        const auto list = control(options);
        SendMessageW(list, WM_SETREDRAW, FALSE, 0);
        SendMessageW(list, LB_RESETCONTENT, 0, 0);
        int selected = current_.visualizer;
        if (id == sleep) {
            for (std::size_t i = 0; i < std::size(timers); ++i) {
                SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(timers[i].label));
                if (timers[i].minutes == current_.timer_minutes)
                    selected = static_cast<int>(i);
            }
        } else {
            for (const auto &choice : visualizers)
                SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(choice.label));
        }
        const auto anchor = box(id);
        const LONG below = bounds().bottom - anchor.bottom, above = anchor.top;
        const LONG desired = (id == sleep ? std::size(timers) : std::size(visualizers)) * option_height;
        const bool down = below >= desired || below >= above;
        const LONG height = std::max<LONG>(option_height, std::min(desired, ((down ? below : above) / option_height) * option_height));
        const LONG top = std::max(0L, down ? anchor.bottom : anchor.top - height);
        SetWindowPos(list, HWND_TOP, anchor.left, top, anchor.right - anchor.left, height, SWP_NOACTIVATE | SWP_NOREDRAW);
        SendMessageW(list, LB_SETCURSEL, selected, 0);
        SendMessageW(list, WM_SETREDRAW, TRUE, 0);
        ShowWindow(list, SW_SHOWNOACTIVATE);
        InvalidateRect(list, nullptr, FALSE);
        SetFocus(list);
    }

    void close_dropdown(bool return_focus) {
        const auto id = std::exchange(dropdown_, std::nullopt);
        if (!id)
            return;
        const auto list = control(options);
        if (GetCapture() == list)
            ReleaseCapture();
        ShowWindow(list, SW_HIDE);
        if (return_focus)
            SetFocus(control(*id));
    }

    void choose_option(int index) {
        if (!dropdown_ || index < 0 || index >= SendMessageW(control(options), LB_GETCOUNT, 0, 0))
            return;
        const auto id = *dropdown_;
        close_dropdown(true);
        if (id == sleep)
            set_timer(timers[index].minutes);
        else
            save_failed_ = !set_visualizer(player_, index);
        refresh();
    }

    bool dropdown_input(const MSG &message) {
        const auto list = control(options), anchor = control(*dropdown_);
        if (message.message == WM_LBUTTONDOWN || message.message == WM_LBUTTONDBLCLK) {
            if (message.hwnd == anchor) {
                close_dropdown(true);
                return true;
            }
            if (message.hwnd != list)
                close_dropdown(false);
        } else if (message.message == WM_LBUTTONUP && message.hwnd == list) {
            const auto item = SendMessageW(list, LB_ITEMFROMPOINT, 0, message.lParam);
            if (HIWORD(item) == 0) {
                choose_option(LOWORD(item));
                return true;
            }
        } else if (message.message == WM_KEYDOWN && message.hwnd == list) {
            if (message.wParam == VK_RETURN) {
                choose_option(SendMessageW(list, LB_GETCURSEL, 0, 0));
                return true;
            }
            if (message.wParam == VK_ESCAPE || message.wParam == VK_TAB) {
                close_dropdown(true);
                return message.wParam == VK_ESCAPE;
            }
        } else if (message.message == WM_MOUSEWHEEL) {
            RECT area{};
            GetWindowRect(list, &area);
            if (message.hwnd == list || PtInRect(&area, {GET_X_LPARAM(message.lParam), GET_Y_LPARAM(message.lParam)})) {
                option_wheel_ += GET_WHEEL_DELTA_WPARAM(message.wParam);
                const int top = SendMessageW(list, LB_GETTOPINDEX, 0, 0);
                const int count = SendMessageW(list, LB_GETCOUNT, 0, 0);
                const int visible = std::max(1L, (area.bottom - area.top) / option_height);
                const int next = std::clamp(top - 2 * (option_wheel_ / WHEEL_DELTA), 0, std::max(0, count - visible));
                option_wheel_ %= WHEEL_DELTA;
                if (next != top)
                    SendMessageW(list, LB_SETTOPINDEX, next, 0);
                return true;
            }
            close_dropdown(false);
        }
        return false;
    }

    void command(Control id, WORD event) {
        if (id == speed) {
            const auto edit = control(speed);
            if (event == EN_SETFOCUS) {
                original_speed_ = window_text(edit);
                SendMessageW(edit, EM_SETSEL, 0, -1);
                reveal(edit);
            } else if (event == EN_KILLFOCUS) {
                const auto value = window_text(edit);
                if (value != original_speed_) {
                    if (const auto rate = parse_number(value, L"x"); rate && *rate >= min_speed && *rate <= max_speed)
                        save_failed_ = !set_speed(player_, static_cast<float>(*rate));
                }
                refresh();
            }
        } else if (id == options) {
            if (event == LBN_KILLFOCUS)
                close_dropdown(false);
        } else if (event == BN_CLICKED) {
            if (id == sleep || id == visualizer)
                open_dropdown(id);
            else if (id == reverse)
                set_reverse(player_, !current_.reverse);
            else if (id == timeline || id == statistics)
                save_failed_ = !set_switch(player_, id == timeline ? timeline_key : statistics_key, !is_on(id));
            refresh();
        }
        InvalidateRect(window_, nullptr, FALSE);
    }

    LRESULT message(UINT message, WPARAM wp, LPARAM lp) {
        switch (message) {
        case WM_CREATE: return initialize() ? 0 : -1;
        case WM_SIZE:
            close_dropdown(false);
            scroll_.move(scroll_.offset(), bounds());
            layout();
            return 0;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: paint(window_, bounds(), [this](HDC dc) { draw(dc); }); return 0;
        case WM_PRINTCLIENT: draw(reinterpret_cast<HDC>(wp)); return 0;
        case WM_TIMER: if (wp == refresh_timer) refresh(); return 0;
        case WM_MOUSEWHEEL: wheel(GET_WHEEL_DELTA_WPARAM(wp)); return 0;
        case WM_LBUTTONDOWN: {
            const POINT pointer{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            const auto field = box(speed);
            if (PtInRect(&field, pointer))
                SetFocus(control(speed));
            else {
                SetFocus(window_);
                if (scroll_.start_drag(window_, bounds(), pointer.x, pointer.y))
                    layout();
            }
            return 0;
        }
        case WM_MOUSEMOVE: if (scroll_.drag(bounds(), GET_Y_LPARAM(lp))) layout(); return 0;
        case WM_LBUTTONUP: scroll_.end_drag(); return 0;
        case WM_CAPTURECHANGED: scroll_.capture_lost(); return 0;
        case WM_COMMAND: command(static_cast<Control>(LOWORD(wp)), HIWORD(wp)); return 0;
        case WM_DRAWITEM: draw_item(*reinterpret_cast<const DRAWITEMSTRUCT *>(lp)); return TRUE;
        case WM_HSCROLL: if (reinterpret_cast<HWND>(lp) == control(slider)) slide_speed(LOWORD(wp)); return 0;
        case WM_NOTIFY: {
            const auto note = reinterpret_cast<NMHDR *>(lp);
            if (note->idFrom == slider && note->code == NM_CUSTOMDRAW) {
                const bool dark_theme = &colors_ == &dark;
                const SliderTicks ticks{7, dark_theme ? RGB(39, 36, 33) : colors_.edge,
                                         dark_theme ? RGB(81, 54, 24) : RGB(172, 144, 115)};
                return draw_slider(*reinterpret_cast<NMCUSTOMDRAW *>(lp), colors_, false,
                                   dark_theme ? RGB(171, 99, 20) : colors_.accent, ticks);
            }
            if (note->idFrom == slider && note->code == NM_RELEASEDCAPTURE) {
                save_speed();
                return 0;
            }
            break;
        }
        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLOREDIT: {
            const auto dc = reinterpret_cast<HDC>(wp);
            const auto color = message == WM_CTLCOLORLISTBOX ? colors_.button : colors_.track;
            SetTextColor(dc, colors_.text);
            SetBkColor(dc, color);
            SetDCBrushColor(dc, color);
            return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
        }
        case WM_DESTROY:
            KillTimer(window_, refresh_timer);
            save_speed();
            break;
        default:
            break;
        }
        return DefWindowProcW(window_, message, wp, lp);
    }

    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wp, LPARAM lp) {
        if (message == WM_NCCREATE) {
            const auto view = static_cast<View *>(reinterpret_cast<CREATESTRUCTW *>(lp)->lpCreateParams);
            view->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(view));
        }
        const auto view = get(window);
        if (!view)
            return DefWindowProcW(window, message, wp, lp);
        const auto result = view->message(message, wp, lp);
        if (message == WM_NCDESTROY && view->window_owned_)
            delete view;
        return result;
    }
};

}

HWND create(HWND parent, HWND, const RECT &bounds, VlcPlayer player, bool light_theme) {
    return View::open(parent, bounds, player, light_theme);
}

void set_visible(HWND window, bool visible) {
    if (const auto view = View::get(window))
        view->visible(visible);
}

bool handle_input(HWND window, const MSG &message) {
    const auto view = View::get(window);
    return view && view->input(message);
}

}
