#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define UNICODE
#include "video_preferences_view.hpp"
#include "preferences_widgets.hpp"
#include "video_preferences.hpp"
#include "video_renderers.hpp"
#include "vlc_config.hpp"

#include <utility>

namespace video_preferences_view {
namespace {

using namespace preferences_widgets;
using namespace video_preferences;
constexpr wchar_t class_name[] = L"VlcRedesignVideoPreferences";
constexpr int inset = 20, label_inset = 24, rotate_width = 142, dropdown_width = 220;
constexpr int flip_top = 103, flip_height = 44, rotate_top = 173, rotate_height = 36;
constexpr int dropdown_top = 235, dropdown_height = 48, row_stride = 68, option_height = 33;
constexpr const wchar_t *filter_names[] = {L"Essential", L"Crop", L"Colors", L"Geometry", L"Overlay", L"Advanced"};
constexpr int filter_count = std::size(filter_names);
constexpr int filters_top = 645, first_filter_top = filters_top + 81;
constexpr int filter_stride = 125, filter_height = 48;
constexpr int preset_divider = first_filter_top + (filter_count - 1) * filter_stride + filter_height + 12;
constexpr int preset_top = preset_divider + 21, preset_height = 42;
constexpr int content_height = preset_top + preset_height + 13;
constexpr int essential_inset = 16, slider_height = 30, slider_start = 87, slider_spacing = 76;
constexpr int slider_count = std::size(essential_sliders);
constexpr int essential_height = slider_start + (slider_count - 1) * slider_spacing + 47;
constexpr UINT_PTR render_refresh = 1, essential_preview = 2;

enum Control {
    no_flip = 301, horizontal, vertical, rotate, zoom, hdr, resolution, aspect, crop, render, options_list,
    first_filter, default_preset = first_filter + filter_count, first_essential_slider
};
constexpr const wchar_t *flip_names[] = {L"None", L"Horizontal", L"Vertical"};
constexpr const wchar_t *dropdown_names[] = {L"Zoom", L"HDR", L"Resolution", L"Aspect Ratio", L"Crop", L"Render"};
constexpr Field field(Control id) { return static_cast<Field>(id - zoom); }
static_assert(field(crop) == Field::crop);

constexpr bool is_essential_slider(int id) {
    return id >= first_essential_slider && id < first_essential_slider + slider_count;
}

class View {
public:
    View(VlcPlayer player, bool light_theme) : player_(player), colors_(light_theme ? light : dark), detail_colors_(colors_) {
        detail_colors_.panel = colors_.button;
        if (!light_theme)
            detail_colors_.track = RGB(9, 9, 9);
    }

    ~View() {
        DeleteObject(regular_);
        DeleteObject(heading_);
    }

    static View *get(HWND window) {
        return reinterpret_cast<View *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    }

    static HWND open(HWND parent, const RECT &bounds, VlcPlayer player, bool light_theme) {
        HINSTANCE module = nullptr;
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
        const HWND window = CreateWindowExW(WS_EX_CONTROLPARENT | WS_EX_COMPOSITED, class_name, L"Video Preferences",
                                            WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                                            bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
                                            parent, nullptr, module, view.get());
        if (window)
            view.release()->window_owned_ = true;
        return window;
    }

    void visible(bool show) {
        if (show) {
            refresh();
        } else {
            save_essential();
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
            const auto focus = IsChild(window_, GetFocus()) ? GetFocus() : nullptr;
            if (const auto next = GetNextDlgTabItem(window_, focus, GetKeyState(VK_SHIFT) < 0)) {
                show_control(next);
                SetFocus(next);
            }
            return true;
        }
        if (message.hwnd == control(rotate)) {
            if (message.wParam == 'A' && GetKeyState(VK_CONTROL) < 0) {
                SendMessageW(message.hwnd, EM_SETSEL, 0, -1);
                return true;
            }
            if (message.wParam == VK_RETURN || message.wParam == VK_ESCAPE) {
                if (message.wParam == VK_ESCAPE)
                    SetWindowTextW(message.hwnd, original_angle_.c_str());
                SetFocus(window_);
                return true;
            }
        } else if (message.wParam == VK_RETURN && !is_essential_slider(GetDlgCtrlID(message.hwnd)) &&
                   message.hwnd != window_) {
            SendMessageW(message.hwnd, BM_CLICK, 0, 0);
            return true;
        }
        return false;
    }

private:
    HWND window_ = nullptr;
    VlcPlayer player_;
    const Colors &colors_;
    Colors detail_colors_;
    HFONT regular_ = nullptr, heading_ = nullptr;
    Scroll scroll_{content_height};
    Transform transform_;
    video_renderers::Discovery renderers_;
    std::vector<video_renderers::Output> outputs_;
    std::optional<Control> dropdown_;
    std::wstring original_angle_;
    EssentialPositions essential_{};
    std::optional<int> pending_slider_;
    bool essential_expanded_ = false, essential_dirty_ = false;
    bool window_owned_ = false, save_failed_ = false;
    int option_wheel_ = 0;

    HWND control(int id) const { return GetDlgItem(window_, id); }

    RECT bounds() const {
        RECT area{};
        GetClientRect(window_, &area);
        return area;
    }

    bool initialize() {
        INITCOMMONCONTROLSEX common{sizeof(common), ICC_BAR_CLASSES};
        InitCommonControlsEx(&common);
        regular_ = make_font(15);
        heading_ = make_font(18, FW_SEMIBOLD);
        const auto module = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(window_, GWLP_HINSTANCE));
        for (int id = no_flip; id <= default_preset; ++id) {
            const bool edit = id == rotate, list = id == options_list;
            const bool pending = id > first_filter;
            const DWORD style = edit ? ES_AUTOHSCROLL : list ?
                LBS_HASSTRINGS | LBS_OWNERDRAWFIXED | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT : BS_OWNERDRAW;
            const wchar_t *label = L"";
            if (id <= vertical)
                label = flip_names[id - no_flip];
            else if (id >= first_filter)
                label = id == default_preset ? L"Default Preset" : L"Expand Settings";
            const DWORD visibility = list ? 0 : WS_VISIBLE | (pending ? WS_DISABLED : WS_TABSTOP);
            const auto child = CreateWindowExW(0, edit ? L"EDIT" : list ? L"LISTBOX" : L"BUTTON", label,
                                               WS_CHILD | WS_CLIPSIBLINGS | visibility | style,
                                               0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                               module, nullptr);
            if (!child)
                return false;
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(regular_), FALSE);
            if (id == first_filter && !create_essential_sliders(module))
                return false;
        }
        SendMessageW(control(rotate), EM_SETLIMITTEXT, 24, 0);
        SendMessageW(control(options_list), LB_SETITEMHEIGHT, 0, option_height);
        refresh();
        return true;
    }

    bool create_essential_sliders(HINSTANCE module) {
        for (int i = 0; i < slider_count; ++i) {
            const auto &slider = essential_sliders[i];
            const HWND child = CreateWindowExW(0, TRACKBAR_CLASSW, slider.label,
                                               WS_CHILD | WS_CLIPSIBLINGS | WS_TABSTOP | TBS_NOTICKS | TBS_FIXEDLENGTH |
                                                   TBS_TRANSPARENTBKGND,
                                               0, 0, 0, 0, window_,
                                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(first_essential_slider + i)),
                                               module, nullptr);
            if (!child)
                return false;
            SendMessageW(child, TBM_SETRANGEMIN, FALSE, slider.minimum);
            SendMessageW(child, TBM_SETRANGEMAX, FALSE, slider.maximum);
            SendMessageW(child, TBM_SETTHUMBLENGTH, 27, 0);
            SendMessageW(child, TBM_SETLINESIZE, 0, 1);
            SendMessageW(child, TBM_SETPAGESIZE, 0, (slider.maximum - slider.minimum) / 10);
        }
        return true;
    }

    int essential_extra_height() const {
        return essential_expanded_ ? essential_height - filter_height : 0;
    }

    RECT box(Control id, LONG width) const {
        const LONG right = width - inset;
        const int y = -scroll_.offset();
        if (id <= vertical) {
            const int index = id - no_flip;
            const LONG group_width = right - inset;
            return {inset + group_width * index / 3, y + flip_top,
                    inset + group_width * (index + 1) / 3, y + flip_top + flip_height};
        }
        if (id == rotate)
            return {right - rotate_width, y + rotate_top, right, y + rotate_top + rotate_height};
        if (is_essential_slider(id)) {
            const int top = y + first_filter_top + slider_start + (id - first_essential_slider) * slider_spacing;
            const int slider_inset = inset + essential_inset - 7;
            return {slider_inset, top, width - slider_inset, top + slider_height};
        }
        if (id == default_preset)
            return {inset, y + preset_top + essential_extra_height(), right,
                    y + preset_top + essential_extra_height() + preset_height};
        if (id >= first_filter) {
            const int top = y + first_filter_top + (id - first_filter) * filter_stride +
                            (id == first_filter ? 0 : essential_extra_height());
            return {inset, top, right, top + filter_height};
        }
        const int top = y + dropdown_top + (id - zoom) * row_stride;
        return {right - dropdown_width, top, right, top + dropdown_height};
    }

    void layout() {
        const auto width = bounds().right;
        const int end = essential_expanded_ ? first_essential_slider + slider_count : default_preset + 1;
        for (int id = no_flip; id < end; ++id) {
            if (id == options_list)
                continue;
            auto area = box(static_cast<Control>(id), width);
            if (id == rotate) {
                area.left += 13;
                area.right -= 8;
                area.top += 8;
                area.bottom -= 7;
            }
            SetWindowPos(control(id), nullptr, area.left, area.top, area.right - area.left, area.bottom - area.top,
                         SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS | SWP_NOREDRAW);
        }
        RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_NOERASE);
    }

    void show_control(HWND child) {
        if (scroll_.show(window_, child, bounds()))
            layout();
    }

    void wheel(int delta) {
        if (scroll_.wheel(delta, bounds()))
            layout();
    }

    void refresh() {
        transform_ = transform(player_);
        set_unfocused_text(control(rotate), number_text(transform_.degrees, 1, L" deg"));
        for (int id = zoom; id < render; ++id) {
            const auto key = field(static_cast<Control>(id));
            const auto choice = selected(player_, key);
            SetWindowTextW(control(id), choice ? choices(key).begin()[*choice].label : L"Custom");
        }
        SetWindowTextW(control(render), wstring(video_renderers::selected(player_).name).c_str());
        if (essential_expanded_)
            refresh_essential();
        RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_NOERASE);
    }

    void refresh_essential() {
        if (pending_slider_)
            return;
        essential_ = essential_positions(player_);
        for (int i = 0; i < slider_count; ++i)
            SendMessageW(control(first_essential_slider + i), TBM_SETPOS, TRUE, essential_[i]);
    }

    void toggle_essential() {
        close_dropdown(false);
        if (essential_expanded_) {
            save_essential();
            if (is_essential_slider(GetDlgCtrlID(GetFocus())))
                SetFocus(control(first_filter));
        }
        essential_expanded_ = !essential_expanded_;
        SetWindowTextW(control(first_filter), essential_expanded_ ? L"Collapse Settings" : L"Expand Settings");
        scroll_.set_content_height(content_height + essential_extra_height());
        scroll_.move(scroll_.offset(), bounds());
        layout();
        if (essential_expanded_)
            refresh_essential();
        for (int i = 0; i < slider_count; ++i)
            ShowWindow(control(first_essential_slider + i), essential_expanded_ ? SW_SHOWNOACTIVATE : SW_HIDE);
        if (essential_expanded_)
            show_control(control(first_essential_slider));
    }

    void apply_essential() {
        KillTimer(window_, essential_preview);
        const auto index = std::exchange(pending_slider_, std::nullopt);
        if (!index)
            return;
        const int position = SendMessageW(control(first_essential_slider + *index), TBM_GETPOS, 0, 0);
        if (position != essential_[*index]) {
            set_essential_position(player_, essential_sliders[*index], position);
            essential_[*index] = position;
            essential_dirty_ = true;
        }
    }

    void save_essential() {
        apply_essential();
        if (std::exchange(essential_dirty_, false)) {
            save_failed_ = !vlc_config::save(player_.vlc);
            InvalidateRect(window_, nullptr, FALSE);
        }
    }

    void move_essential_slider(int id, WORD event) {
        const int index = id - first_essential_slider;
        if (pending_slider_ && *pending_slider_ != index)
            apply_essential();
        if (event == TB_THUMBTRACK) {
            // Coalesce drag events because VLC rebuilds the video filter chain when its options change.
            if (!pending_slider_)
                SetTimer(window_, essential_preview, 80, nullptr);
            pending_slider_ = index;
        } else {
            pending_slider_ = index;
            save_essential();
        }
    }

    void draw(HDC dc) const {
        const auto area = bounds();
        fill(dc, area, colors_.panel);
        const LONG top = -scroll_.offset(), right = area.right - label_inset;
        text(dc, L"Defaults", {label_inset, top + 13, right, top + 48}, heading_, colors_.text);
        text(dc, L"Flip", {label_inset, top + 57, right, top + 94}, regular_, colors_.dim);
        const auto edit = box(rotate, area.right);
        text(dc, L"Rotate", {label_inset, edit.top, edit.left - 8, edit.bottom}, regular_, colors_.dim);
        round_rect(dc, edit, 6, colors_.edge);
        round_rect(dc, {edit.left + 2, edit.top + 2, edit.right - 2, edit.bottom - 2}, 4, colors_.track);
        for (int id = zoom; id <= render; ++id) {
            const auto field = box(static_cast<Control>(id), area.right);
            text(dc, dropdown_names[id - zoom], {label_inset, field.top, field.left - 8, field.bottom},
                 regular_, colors_.dim);
        }
        if (save_failed_)
            text(dc, L"VLC could not save this setting.", {label_inset, top + 624, right, top + 644},
                 regular_, colors_.text);
        const auto divider = [&](LONG y) { fill(dc, {0, y, area.right, y + 1}, colors_.edge); };
        divider(top + filters_top - 1);
        text(dc, L"Filters", {label_inset, top + filters_top, right, top + filters_top + 32}, heading_, colors_.text);
        for (int i = 0; i < filter_count; ++i) {
            const auto button = box(static_cast<Control>(first_filter + i), area.right);
            text(dc, filter_names[i], {label_inset, button.top - 46, right, button.top - 9}, regular_, colors_.dim);
            if (i + 1 < filter_count)
                divider(button.bottom + 19 + (i == 0 ? essential_extra_height() : 0));
        }
        if (essential_expanded_) {
            const auto header = box(first_filter, area.right);
            round_rect(dc, {header.left, header.top, header.right, header.top + essential_height}, 6, detail_colors_.panel);
            for (int i = 0; i < slider_count; ++i) {
                const auto slider = box(static_cast<Control>(first_essential_slider + i), area.right);
                text(dc, essential_sliders[i].label, {header.left + essential_inset, slider.top - 33,
                     header.right - essential_inset, slider.top - 7}, regular_, colors_.dim);
            }
        }
        divider(top + preset_divider + essential_extra_height());
        if (scroll_.limit(area))
            round_rect(dc, scroll_.thumb(area), 2, colors_.knob);
    }

    void draw_item(const DRAWITEMSTRUCT &item) const {
        auto box = item.rcItem;
        const auto dc = item.hDC;
        const bool pressed = (item.itemState & ODS_SELECTED) != 0;
        if (item.CtlID == options_list) {
            fill(dc, box, pressed ? colors_.fill : colors_.button);
            if (item.itemID != UINT(-1)) {
                const auto length = SendMessageW(item.hwndItem, LB_GETTEXTLEN, item.itemID, 0);
                if (length >= 0) {
                    std::wstring label(length + 1, L'\0');
                    SendMessageW(item.hwndItem, LB_GETTEXT, item.itemID, reinterpret_cast<LPARAM>(label.data()));
                    box.left += 15;
                    box.right -= 8;
                    text(dc, label.c_str(), box, regular_, pressed ? colors_.accent : colors_.text);
                }
            }
            return;
        }
        const bool flip = item.CtlID <= vertical;
        const bool selected_flip = flip && static_cast<int>(transform_.flip) == static_cast<int>(item.CtlID) - no_flip;
        const auto label = window_text(item.hwndItem);
        fill(dc, box, colors_.panel);
        const bool expanded = item.CtlID == first_filter && essential_expanded_;
        round_rect(dc, box, 6, expanded ? detail_colors_.panel : colors_.track);
        if (expanded)
            fill(dc, {box.left, box.top + 6, box.right, box.bottom}, detail_colors_.panel);
        if (item.CtlID >= first_filter) {
            const bool preset = item.CtlID == default_preset;
            const auto color = preset ? colors_.text : colors_.dim;
            const int arrow_inset = preset ? 28 : 23;
            const auto direction = preset ? Arrow::up : expanded ? Arrow::down : Arrow::right;
            arrow(dc, box.right - arrow_inset, (box.top + box.bottom) / 2, direction, color);
            text(dc, label.c_str(), {box.left + 16, box.top, box.right - 38, box.bottom}, regular_, color);
            return;
        }
        InflateRect(&box, -2, -2);
        round_rect(dc, box, 4, selected_flip ? colors_.fill : pressed ? colors_.hover : colors_.button);
        if (flip) {
            text(dc, label.c_str(), box, regular_, selected_flip ? colors_.accent : colors_.dim, DT_CENTER);
        } else {
            arrow(dc, box.right - 23, (box.top + box.bottom) / 2, Arrow::down, colors_.text);
            text(dc, label.c_str(), {box.left + 15, box.top, box.right - 38, box.bottom}, regular_, colors_.text);
        }
    }

    void close_dropdown(bool restore_focus) {
        const auto previous = std::exchange(dropdown_, std::nullopt);
        if (!previous)
            return;
        KillTimer(window_, render_refresh);
        const auto list = control(options_list);
        if (GetCapture() == list)
            ReleaseCapture();
        ShowWindow(list, SW_HIDE);
        if (restore_focus)
            SetFocus(control(*previous));
        outputs_.clear();
    }

    void populate_dropdown(const std::vector<std::wstring> &labels, int selected, int keep_top = 0) {
        const auto list = control(options_list);
        SendMessageW(list, WM_SETREDRAW, FALSE, 0);
        SendMessageW(list, LB_RESETCONTENT, 0, 0);
        for (const auto &label : labels)
            SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));

        const auto area = bounds();
        const auto anchor = box(*dropdown_, area.right);
        const int below = area.bottom - anchor.bottom, above = anchor.top;
        const int desired = std::min<int>(labels.size(), 7) * option_height;
        const bool down = below >= desired || below >= above;
        const int height = std::min(desired, std::max(0, (down ? below : above) / option_height) * option_height);
        const int visible = height > 0 ? height : std::min<int>(option_height, area.bottom);
        const int top = std::clamp<int>(down ? anchor.bottom : anchor.top - visible, 0, area.bottom - visible);
        SetWindowPos(list, HWND_TOP, anchor.left, top, anchor.right - anchor.left, visible,
                     SWP_NOACTIVATE | SWP_NOREDRAW);
        if (keep_top)
            SendMessageW(list, LB_SETTOPINDEX, keep_top, 0);
        SendMessageW(list, LB_SETCURSEL, selected, 0);
        SendMessageW(list, WM_SETREDRAW, TRUE, 0);
        ShowWindow(list, SW_SHOWNOACTIVATE);
        RedrawWindow(list, nullptr, nullptr, RDW_INVALIDATE | RDW_NOERASE);
    }

    void update_renderers(bool opening) {
        auto outputs = renderers_.outputs();
        const auto same = [](const auto &a, const auto &b) { return a.id == b.id && a.name == b.name; };
        if (!opening && std::equal(outputs.begin(), outputs.end(), outputs_.begin(), outputs_.end(), same)) {
            outputs_ = std::move(outputs);
            return;
        }
        const auto list = control(options_list);
        const auto focused = SendMessageW(list, LB_GETCURSEL, 0, 0);
        const auto active = !opening && focused >= 0 && static_cast<std::size_t>(focused) < outputs_.size() ?
            outputs_[focused].id : video_renderers::selected(player_).id;
        int selected = -1;
        std::vector<std::wstring> labels;
        for (const auto &output : outputs) {
            if (output.id == active)
                selected = static_cast<int>(labels.size());
            labels.push_back(wstring(output.name));
        }
        outputs_ = std::move(outputs);
        populate_dropdown(labels, selected, opening ? 0 : SendMessageW(list, LB_GETTOPINDEX, 0, 0));
    }

    void open_dropdown(Control id) {
        close_dropdown(false);
        show_control(control(id));
        dropdown_ = id;
        option_wheel_ = 0;
        if (id == render) {
            renderers_.start();
            update_renderers(true);
            SetTimer(window_, render_refresh, 300, nullptr);
        } else {
            std::vector<std::wstring> labels;
            for (const auto &choice : choices(field(id)))
                labels.emplace_back(choice.label);
            populate_dropdown(labels, selected(player_, field(id)).value_or(-1));
        }
        SetFocus(control(options_list));
    }

    void select_option(int index) {
        if (!dropdown_ || index < 0 || index >= SendMessageW(control(options_list), LB_GETCOUNT, 0, 0))
            return;
        const auto id = *dropdown_;
        const auto output = id == render ? outputs_[index] : video_renderers::Output{};
        close_dropdown(true);
        save_failed_ = !(id == render ? video_renderers::select(player_, output) : set_choice(player_, field(id), index));
        refresh();
    }

    bool dropdown_input(const MSG &message) {
        const auto list = control(options_list), button = control(*dropdown_);
        if (message.message == WM_LBUTTONDOWN || message.message == WM_LBUTTONDBLCLK) {
            if (message.hwnd == button) {
                close_dropdown(true);
                return true;
            }
            if (message.hwnd != list)
                close_dropdown(false);
        } else if (message.hwnd == list && message.message == WM_LBUTTONUP) {
            const auto item = SendMessageW(list, LB_ITEMFROMPOINT, 0, message.lParam);
            if (HIWORD(item) == 0) {
                select_option(LOWORD(item));
                return true;
            }
        } else if (message.hwnd == list && message.message == WM_KEYDOWN) {
            if (message.wParam == VK_RETURN) {
                select_option(static_cast<int>(SendMessageW(list, LB_GETCURSEL, 0, 0)));
                return true;
            }
            if (message.wParam == VK_ESCAPE || message.wParam == VK_TAB) {
                close_dropdown(true);
                return message.wParam == VK_ESCAPE;
            }
        } else if (message.message == WM_MOUSEWHEEL) {
            RECT menu{};
            GetWindowRect(list, &menu);
            if (message.hwnd == list || PtInRect(&menu, {GET_X_LPARAM(message.lParam), GET_Y_LPARAM(message.lParam)})) {
                option_wheel_ += GET_WHEEL_DELTA_WPARAM(message.wParam);
                const int current = SendMessageW(list, LB_GETTOPINDEX, 0, 0);
                const int count = SendMessageW(list, LB_GETCOUNT, 0, 0);
                const int visible = std::max(1L, (menu.bottom - menu.top) / option_height);
                const int next = std::clamp(current - 2 * (option_wheel_ / WHEEL_DELTA), 0, std::max(0, count - visible));
                option_wheel_ %= WHEEL_DELTA;
                if (next != current)
                    SendMessageW(list, LB_SETTOPINDEX, next, 0);
                return true;
            }
            close_dropdown(false);
        }
        return false;
    }

    void command(Control id, int event) {
        if (id == options_list && event == LBN_KILLFOCUS) {
            close_dropdown(false);
        } else if (id == rotate) {
            if (event == EN_SETFOCUS) {
                original_angle_ = window_text(control(rotate));
                SendMessageW(control(rotate), EM_SETSEL, 0, -1);
                show_control(control(rotate));
            } else if (event == EN_KILLFOCUS) {
                const auto value = window_text(control(rotate));
                if (const auto degrees = parse_number(value, L" deg"); degrees && value != original_angle_) {
                    transform_.degrees = *degrees;
                    save_failed_ = !set_transform(player_, transform_);
                }
                refresh();
            }
        } else if (event == BN_CLICKED && id >= no_flip && id <= vertical) {
            const auto flip = static_cast<Flip>(id - no_flip);
            if (flip != transform_.flip) {
                transform_.flip = flip;
                save_failed_ = !set_transform(player_, transform_);
                refresh();
            }
        } else if (event == BN_CLICKED && id >= zoom && id <= render) {
            open_dropdown(id);
        } else if (event == BN_CLICKED && id == first_filter) {
            toggle_essential();
        }
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
        case WM_TIMER:
            if (wp == essential_preview)
                apply_essential();
            else if (wp == render_refresh && dropdown_ == render && GetCapture() != control(options_list))
                update_renderers(false);
            return 0;
        case WM_MOUSEWHEEL: wheel(GET_WHEEL_DELTA_WPARAM(wp)); return 0;
        case WM_LBUTTONDOWN:
            SetFocus(window_);
            if (scroll_.start_drag(window_, bounds(), GET_X_LPARAM(lp), GET_Y_LPARAM(lp)))
                layout();
            return 0;
        case WM_MOUSEMOVE:
            if (scroll_.drag(bounds(), GET_Y_LPARAM(lp)))
                layout();
            return 0;
        case WM_LBUTTONUP: scroll_.end_drag(); return 0;
        case WM_CAPTURECHANGED: scroll_.capture_lost(); return 0;
        case WM_COMMAND: command(static_cast<Control>(LOWORD(wp)), HIWORD(wp)); return 0;
        case WM_DRAWITEM: draw_item(*reinterpret_cast<const DRAWITEMSTRUCT *>(lp)); return TRUE;
        case WM_HSCROLL: {
            const int id = GetDlgCtrlID(reinterpret_cast<HWND>(lp));
            if (is_essential_slider(id))
                move_essential_slider(id, LOWORD(wp));
            return 0;
        }
        case WM_NOTIFY: {
            const auto note = reinterpret_cast<NMHDR *>(lp);
            const int id = static_cast<int>(note->idFrom);
            if (is_essential_slider(id)) {
                if (note->code == NM_RELEASEDCAPTURE) {
                    save_essential();
                } else if (note->code == NM_CUSTOMDRAW) {
                    const bool dark_theme = &colors_ == &dark;
                    const SliderTicks ticks{essential_sliders[id - first_essential_slider].divisions,
                                            dark_theme ? RGB(37, 32, 26) : colors_.edge,
                                            dark_theme ? RGB(76, 52, 22) : RGB(172, 144, 115)};
                    return draw_slider(*reinterpret_cast<NMCUSTOMDRAW *>(lp), detail_colors_, false,
                                       dark_theme ? RGB(171, 99, 20) : colors_.accent, ticks);
                }
            }
            break;
        }
        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLOREDIT: {
            const auto dc = reinterpret_cast<HDC>(wp);
            const auto background = message == WM_CTLCOLORLISTBOX ? colors_.button : colors_.track;
            SetTextColor(dc, colors_.text);
            SetBkColor(dc, background);
            SetDCBrushColor(dc, background);
            return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
        }
        case WM_DESTROY: save_essential(); KillTimer(window_, render_refresh); break;
        default: break;
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

HWND create(HWND parent, HWND, const RECT &bounds, VlcPlayer player, bool light) {
    return View::open(parent, bounds, player, light);
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
