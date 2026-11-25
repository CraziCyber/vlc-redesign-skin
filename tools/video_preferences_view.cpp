#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define UNICODE
#include "video_preferences_view.hpp"
#include "crop_link_icons.hpp"
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
constexpr int detail_inset = 16, slider_height = 30, slider_start = 87, slider_spacing = 76;
constexpr int slider_count = std::size(essential_sliders);
constexpr int essential_height = slider_start + (slider_count - 1) * slider_spacing + 47;
constexpr int crop_field_top = 54, crop_field_height = 33, crop_row_spacing = 49;
constexpr int crop_height = crop_field_top + crop_row_spacing + crop_field_height + detail_inset;
constexpr int colors_height = 755, color_toggle_width = 68, color_field_width = 116, color_row_height = 32;
constexpr UINT_PTR render_refresh = 1, slider_preview = 2;

enum Control {
    no_flip = 301, horizontal, vertical, rotate, zoom, hdr, resolution, aspect, crop, render, options_list,
    first_filter, essential_filter = first_filter, crop_filter, colors_filter, default_preset = first_filter + filter_count,
    first_essential_slider, first_crop_edge = first_essential_slider + slider_count,
    crop_top = first_crop_edge, crop_bottom, crop_left, crop_right, link_top_bottom, link_left_right,
    extract_hex, extract_swatch, extract_toggle, threshold_hex, threshold_swatch, threshold_toggle,
    threshold_saturation, threshold_similarity, negate_toggle, posterize_toggle, gradient_toggle,
    gradient_mode, gradient_color, gradient_cartoon, sepia_slider
};
constexpr const wchar_t *flip_names[] = {L"None", L"Horizontal", L"Vertical"};
constexpr const wchar_t *dropdown_names[] = {L"Zoom", L"HDR", L"Resolution", L"Aspect Ratio", L"Crop", L"Render"};
constexpr const wchar_t *crop_names[] = {L"Top", L"Bottom", L"Left", L"Right"};
constexpr CropEdge paired_edges[] = {bottom_edge, top_edge, right_edge, left_edge};
static_assert(std::size(crop_names) == crop_edge_count && std::size(paired_edges) == crop_edge_count);
constexpr Field field(Control id) { return static_cast<Field>(id - zoom); }
static_assert(field(crop) == Field::crop);

struct FilterSection {
    Control header, first, last;
    int height;
};
constexpr FilterSection sections[] = {
    {essential_filter, first_essential_slider, static_cast<Control>(first_essential_slider + slider_count - 1), essential_height},
    {crop_filter, crop_top, link_left_right, crop_height},
    {colors_filter, extract_hex, sepia_slider, colors_height},
};
constexpr bool expandable(int id) { return id >= first_filter && id < first_filter + int(std::size(sections)); }

constexpr bool is_essential_slider(int id) {
    return id >= first_essential_slider && id < first_essential_slider + slider_count;
}

constexpr bool is_crop_edge(int id) { return id >= crop_top && id <= crop_right; }
constexpr bool is_crop_link(int id) { return id == link_top_bottom || id == link_left_right; }

enum class ColorControl { hex, swatch, toggle, slider, mode };
struct ColorRow {
    Control id;
    int top;
    ColorControl type;
    const wchar_t *label;
    ColorEffect effect;
    ColorValue value = ColorValue::count;
};
constexpr ColorRow color_rows[] = {
    {extract_hex,          54, ColorControl::hex,    L"", ColorEffect::extraction, ColorValue::extract_rgb},
    {extract_swatch,       54, ColorControl::swatch, L"Choose extraction color", ColorEffect::extraction, ColorValue::extract_rgb},
    {extract_toggle,       54, ColorControl::toggle, L"Color Extraction", ColorEffect::extraction},
    {threshold_hex,      126, ColorControl::hex,    L"", ColorEffect::threshold, ColorValue::threshold_rgb},
    {threshold_swatch,   126, ColorControl::swatch, L"Choose threshold color", ColorEffect::threshold, ColorValue::threshold_rgb},
    {threshold_toggle,   126, ColorControl::toggle, L"Color Threshold", ColorEffect::threshold},
    {threshold_saturation, 203, ColorControl::slider, L"Saturation", ColorEffect::threshold, ColorValue::saturation},
    {threshold_similarity, 279, ColorControl::slider, L"Similarity", ColorEffect::threshold, ColorValue::similarity},
    {negate_toggle,      346, ColorControl::toggle, L"Negate Colors", ColorEffect::negate},
    {posterize_toggle,   398, ColorControl::toggle, L"Posterize", ColorEffect::posterize},
    {gradient_toggle,    455, ColorControl::toggle, L"Gradient", ColorEffect::gradient},
    {gradient_mode,      502, ColorControl::mode,   L"Gradient", ColorEffect::gradient, ColorValue::gradient_mode},
    {gradient_color,     565, ColorControl::toggle, L"Color", ColorEffect::gradient, ColorValue::gradient_color},
    {gradient_cartoon,   613, ColorControl::toggle, L"Cartoon", ColorEffect::gradient, ColorValue::cartoon},
    {sepia_slider,       708, ColorControl::slider, L"Sepia Intensity", ColorEffect::sepia, ColorValue::sepia_intensity},
};
static_assert(std::size(color_rows) == sepia_slider - extract_hex + 1);

constexpr bool is_color_control(int id) { return id >= extract_hex && id <= sepia_slider; }
constexpr const ColorRow &color_row(int id) { return color_rows[id - extract_hex]; }
constexpr bool color_type(int id, ColorControl type) { return is_color_control(id) && color_row(id).type == type; }
constexpr bool is_slider(int id) { return is_essential_slider(id) || color_type(id, ColorControl::slider); }
constexpr bool is_edit(int id) { return id == rotate || is_crop_edge(id) || color_type(id, ColorControl::hex); }

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
        DeleteObject(small_);
    }

    bool has_modal() const { return modal_; }

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
            save_sliders();
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
        const int id = GetDlgCtrlID(message.hwnd);
        if (is_edit(id)) {
            if (message.wParam == 'A' && GetKeyState(VK_CONTROL) < 0) {
                SendMessageW(message.hwnd, EM_SETSEL, 0, -1);
                return true;
            }
            if (message.wParam == VK_RETURN || message.wParam == VK_ESCAPE) {
                if (message.wParam == VK_ESCAPE)
                    SetWindowTextW(message.hwnd, original_edit_.c_str());
                SetFocus(window_);
                return true;
            }
        } else if (message.wParam == VK_RETURN && !is_slider(id) && message.hwnd != window_) {
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
    HFONT regular_ = nullptr, heading_ = nullptr, small_ = nullptr;
    Scroll scroll_{content_height};
    Transform transform_;
    video_renderers::Discovery renderers_;
    std::vector<video_renderers::Output> outputs_;
    std::optional<Control> dropdown_;
    std::wstring original_edit_;
    EssentialPositions essential_{};
    std::optional<Control> pending_slider_;
    CropMargins margins_{};
    IconStrip crop_icons_;
    std::array<bool, std::size(sections)> expanded_{};
    std::array<bool, 2> crop_linked_{};
    ColorSettings color_settings_;
    std::array<COLORREF, 16> custom_colors_{};
    bool slider_dirty_ = false;
    bool window_owned_ = false, save_failed_ = false, modal_ = false;
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
        small_ = make_font(12);
        const auto module = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(window_, GWLP_HINSTANCE));
        crop_icons_.load(module, crop_link_icons::resource, crop_link_icons::size, crop_link_icons::count);
        for (int id = no_flip; id <= default_preset; ++id) {
            const bool edit = id == rotate, list = id == options_list;
            const bool pending = id >= first_filter && !expandable(id);
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
            if (id == essential_filter && !create_essential_sliders(module))
                return false;
            if (id == crop_filter && !create_crop_fields(module))
                return false;
            if (id == colors_filter && !create_color_controls(module))
                return false;
        }
        SendMessageW(control(rotate), EM_SETLIMITTEXT, 24, 0);
        SendMessageW(control(options_list), LB_SETITEMHEIGHT, 0, option_height);
        refresh();
        return true;
    }

    bool create_color_controls(HINSTANCE module) {
        for (const auto &row : color_rows) {
            const bool edit = row.type == ColorControl::hex, slider = row.type == ColorControl::slider;
            const DWORD style = edit ? ES_AUTOHSCROLL :
                                slider ? TBS_NOTICKS | TBS_FIXEDLENGTH | TBS_TRANSPARENTBKGND : BS_OWNERDRAW;
            const auto child = CreateWindowExW(0, edit ? L"EDIT" : slider ? TRACKBAR_CLASSW : L"BUTTON", row.label,
                                               WS_CHILD | WS_CLIPSIBLINGS | WS_TABSTOP | style, 0, 0, 0, 0, window_,
                                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(row.id)), module, nullptr);
            if (!child)
                return false;
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(regular_), FALSE);
            if (edit)
                SendMessageW(child, EM_SETLIMITTEXT, 14, 0);
            if (slider)
                setup_slider(child, 0, row.id == sepia_slider ? sepia_limit : threshold_limit);
        }
        return true;
    }

    bool create_crop_fields(HINSTANCE module) {
        for (const auto id : {crop_top, crop_bottom, link_top_bottom, crop_left, crop_right, link_left_right}) {
            const bool edit = is_crop_edge(id);
            const auto child = CreateWindowExW(0, edit ? L"EDIT" : L"BUTTON", L"",
                                               WS_CHILD | WS_CLIPSIBLINGS | WS_TABSTOP |
                                                   (edit ? ES_AUTOHSCROLL : BS_OWNERDRAW),
                                               0, 0, 0, 0, window_,
                                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), module, nullptr);
            if (!child)
                return false;
            if (edit) {
                SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(regular_), FALSE);
                SendMessageW(child, EM_SETLIMITTEXT, 16, 0);
            } else {
                update_link(id);
            }
        }
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
            setup_slider(child, slider.minimum, slider.maximum);
        }
        return true;
    }

    void setup_slider(HWND child, int minimum, int maximum) const {
        SendMessageW(child, TBM_SETRANGEMIN, FALSE, minimum);
        SendMessageW(child, TBM_SETRANGEMAX, FALSE, maximum);
        SendMessageW(child, TBM_SETTHUMBLENGTH, 27, 0);
        SendMessageW(child, TBM_SETLINESIZE, 0, 1);
        SendMessageW(child, TBM_SETPAGESIZE, 0, (maximum - minimum) / 10);
    }

    bool expanded(Control id) const {
        return expandable(id) && expanded_[id - first_filter];
    }

    int section_extra_height(Control id) const {
        return expanded(id) ? sections[id - first_filter].height - filter_height : 0;
    }

    int extra_height_before(Control id) const {
        int height = 0;
        for (const auto &section : sections) {
            if (section.header < id)
                height += section_extra_height(section.header);
        }
        return height;
    }

    RECT crop_box(Control id, LONG width) const {
        const auto header = box(crop_filter, width);
        const LONG left = header.left + detail_inset, right = header.right - detail_inset;
        constexpr int link_width = 28, link_gap = 10, field_gap = 8;
        const int row = is_crop_link(id) ? id - link_top_bottom : (id - crop_top) / 2;
        const LONG top = header.top + crop_field_top + row * crop_row_spacing;
        const LONG bottom = top + crop_field_height;
        const LONG link_left = right - link_width;
        if (is_crop_link(id))
            return {link_left, top, right, bottom};
        const LONG fields_right = link_left - link_gap;
        const LONG middle = left + (fields_right - left - field_gap) / 2;
        return (id == crop_top || id == crop_left) ? RECT{left, top, middle, bottom} :
                                                    RECT{middle + field_gap, top, fields_right, bottom};
    }

    RECT color_box(const ColorRow &row, LONG width) const {
        const auto header = box(colors_filter, width);
        const LONG left = header.left + detail_inset, right = header.right - detail_inset;
        const LONG top = header.top + row.top, bottom = top + color_row_height;
        constexpr int field_gap = 8, swatch_size = 22;
        const LONG field_right = right - color_toggle_width - field_gap;
        switch (row.type) {
        case ColorControl::slider: return {left - 7, top, right + 7, top + slider_height};
        case ColorControl::mode: return {left, top, right, top + dropdown_height};
        case ColorControl::toggle: return {right - color_toggle_width, top, right, bottom};
        case ColorControl::hex: return {field_right - color_field_width, top, field_right, bottom};
        case ColorControl::swatch: return {field_right - 5 - swatch_size, top + 5, field_right - 5, top + 5 + swatch_size};
        }
        return {};
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
            const int slider_inset = inset + detail_inset - 7;
            return {slider_inset, top, width - slider_inset, top + slider_height};
        }
        if (is_crop_edge(id) || is_crop_link(id))
            return crop_box(id, width);
        if (is_color_control(id))
            return color_box(color_row(id), width);
        if (id == default_preset) {
            const int top = y + preset_top + extra_height_before(id);
            return {inset, top, right, top + preset_height};
        }
        if (id >= first_filter) {
            const int top = y + first_filter_top + (id - first_filter) * filter_stride + extra_height_before(id);
            return {inset, top, right, top + filter_height};
        }
        const int top = y + dropdown_top + (id - zoom) * row_stride;
        return {right - dropdown_width, top, right, top + dropdown_height};
    }

    void layout() {
        const auto width = bounds().right;
        for (int id = no_flip; id <= sepia_slider; ++id) {
            if (id == options_list || (is_essential_slider(id) && !expanded(essential_filter)) ||
                ((is_crop_edge(id) || is_crop_link(id)) && !expanded(crop_filter)) ||
                (is_color_control(id) && !expanded(colors_filter)))
                continue;
            auto area = box(static_cast<Control>(id), width);
            if (id == rotate) {
                area.left += 13;
                area.right -= 8;
                area.top += 8;
                area.bottom -= 7;
            } else if (is_crop_edge(id)) {
                area.left += 72;
                area.right -= 8;
                area.top += 7;
                area.bottom -= 6;
            } else if (color_type(id, ColorControl::hex)) {
                area.left += 12;
                area.right -= 31;
                area.top += 7;
                area.bottom -= 6;
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
        if (expanded(essential_filter))
            refresh_essential();
        if (expanded(crop_filter))
            refresh_crop();
        if (expanded(colors_filter))
            refresh_colors();
        RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_NOERASE);
    }

    void refresh_essential() {
        if (pending_slider_ && is_essential_slider(*pending_slider_))
            return;
        essential_ = essential_positions(player_);
        for (int i = 0; i < slider_count; ++i)
            SendMessageW(control(first_essential_slider + i), TBM_SETPOS, TRUE, essential_[i]);
    }

    void refresh_crop() {
        margins_ = crop_margins(player_);
        for (int i = 0; i < crop_edge_count; ++i)
            set_unfocused_text(control(first_crop_edge + i), std::to_wstring(margins_[i]) + L" px");
    }

    void refresh_colors() {
        if (pending_slider_ && is_color_control(*pending_slider_))
            return;
        color_settings_ = color_settings(player_);
        const bool threshold = color_settings_.enabled(ColorEffect::threshold);
        const bool gradient_options = color_settings_.enabled(ColorEffect::gradient) &&
                                      color_settings_.value(ColorValue::gradient_mode) != hough_mode;
        for (const auto &row : color_rows) {
            const auto child = control(row.id);
            if (row.type == ColorControl::hex) {
                set_unfocused_text(child, hex_text(color_settings_.value(row.value)));
            } else if (row.type == ColorControl::slider) {
                if (row.effect == ColorEffect::threshold)
                    EnableWindow(child, threshold);
                SendMessageW(child, TBM_SETPOS, TRUE, color_settings_.value(row.value));
            } else if (row.type == ColorControl::mode) {
                SetWindowTextW(child, gradient_modes[color_settings_.value(row.value)].label);
            } else if (row.id == gradient_color || row.id == gradient_cartoon) {
                EnableWindow(child, gradient_options);
            }
        }
    }

    bool change_color_value(const ColorRow &row, int value) {
        if (color_settings_.value(row.value) == value)
            return false;
        color_settings_.value(row.value) = value;
        if (row.effect == ColorEffect::sepia)
            color_settings_.enabled(row.effect) = value != 0;
        apply_color_settings(player_, row.effect, color_settings_);
        return true;
    }

    void set_color_value(const ColorRow &row, int value) {
        if (change_color_value(row, value))
            save_failed_ = !vlc_config::save(player_.vlc);
    }

    void set_rgb(const ColorRow &row, std::uint32_t rgb) {
        const auto minimum = row.effect == ColorEffect::extraction ? 1U : 0U;
        set_color_value(row, std::max(rgb, minimum));
    }

    void choose_color(const ColorRow &row) {
        modal_ = true;
        const auto picked = choose_rgb(GetAncestor(window_, GA_ROOT), custom_colors_, color_settings_.value(row.value));
        modal_ = false;
        if (picked) {
            set_rgb(row, *picked);
            refresh();
        }
    }

    bool color_toggle_on(const ColorRow &row) const {
        if (row.value == ColorValue::count)
            return color_settings_.enabled(row.effect);
        return IsWindowEnabled(control(row.id)) && color_settings_.value(row.value) != 0;
    }

    void toggle_color(const ColorRow &row) {
        if (row.value == ColorValue::count) {
            auto &enabled = color_settings_.enabled(row.effect);
            enabled = !enabled;
            apply_color_settings(player_, row.effect, color_settings_);
            save_failed_ = !vlc_config::save(player_.vlc);
        } else {
            set_color_value(row, !color_toggle_on(row));
        }
        refresh();
    }

    void update_link(Control id) {
        const bool linked = crop_linked_[id - link_top_bottom];
        const wchar_t *name = id == link_top_bottom ?
            (linked ? L"Unlink Top and Bottom" : L"Link Top and Bottom") :
            (linked ? L"Unlink Left and Right" : L"Link Left and Right");
        SetWindowTextW(control(id), name);
        InvalidateRect(control(id), nullptr, FALSE);
    }

    void toggle_section(Control id) {
        close_dropdown(false);
        const auto &section = sections[id - first_filter];
        const bool show = !expanded(id);
        if (!show) {
            save_sliders();
            const auto focus = GetFocus();
            const int focused_id = GetDlgCtrlID(focus);
            if (IsChild(window_, focus) && focused_id >= section.first && focused_id <= section.last)
                SetFocus(control(id));
        }
        expanded_[id - first_filter] = show;
        SetWindowTextW(control(id), show ? L"Collapse Settings" : L"Expand Settings");
        scroll_.set_content_height(content_height + extra_height_before(default_preset));
        scroll_.move(scroll_.offset(), bounds());
        layout();
        if (show) {
            if (id == essential_filter)
                refresh_essential();
            else if (id == crop_filter)
                refresh_crop();
            else
                refresh_colors();
        }
        for (int child = section.first; child <= section.last; ++child)
            ShowWindow(control(child), show ? SW_SHOWNOACTIVATE : SW_HIDE);
        if (show)
            show_control(control(id == crop_filter ? crop_left : section.first));
    }

    void apply_slider() {
        KillTimer(window_, slider_preview);
        const auto id = std::exchange(pending_slider_, std::nullopt);
        if (!id)
            return;
        const int position = SendMessageW(control(*id), TBM_GETPOS, 0, 0);
        if (is_essential_slider(*id)) {
            const auto index = *id - first_essential_slider;
            if (position != essential_[index]) {
                set_essential_position(player_, essential_sliders[index], position);
                essential_[index] = position;
                slider_dirty_ = true;
            }
        } else if (IsWindowEnabled(control(*id))) {
            slider_dirty_ |= change_color_value(color_row(*id), position);
        }
    }

    void save_sliders() {
        apply_slider();
        if (std::exchange(slider_dirty_, false)) {
            save_failed_ = !vlc_config::save(player_.vlc);
            InvalidateRect(window_, nullptr, FALSE);
        }
    }

    void move_slider(Control id, WORD event) {
        if (pending_slider_ && *pending_slider_ != id)
            apply_slider();
        if (event == TB_THUMBTRACK) {
            // Coalesce drag events because VLC rebuilds the video filter chain when its options change.
            if (!pending_slider_)
                SetTimer(window_, slider_preview, 80, nullptr);
            pending_slider_ = id;
        } else {
            pending_slider_ = id;
            save_sliders();
        }
    }

    void draw_colors(HDC dc, LONG width) const {
        const auto header = box(colors_filter, width);
        const LONG left = header.left + detail_inset, right = header.right - detail_inset;
        for (const int y : {106, 327, 388, 440, 660}) {
            const LONG top = header.top + y;
            fill(dc, {header.left, top, header.right, top + 1}, &colors_ == &dark ? RGB(32, 32, 32) : colors_.edge);
        }
        for (const auto &row : color_rows) {
            const auto box = color_box(row, width);
            if (row.type == ColorControl::hex) {
                round_rect(dc, box, 6, detail_colors_.track);
            } else if (row.type == ColorControl::toggle) {
                LONG label_end = box.left - 8;
                if (row.id == extract_toggle || row.id == threshold_toggle)
                    label_end -= color_field_width + 8;
                text(dc, row.label, {left, box.top, label_end, box.bottom}, regular_, colors_.dim);
            } else if (row.type == ColorControl::slider) {
                text(dc, row.label, {left, box.top - 33, right, box.top - 7}, regular_, colors_.dim);
            }
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
            const auto id = static_cast<Control>(first_filter + i);
            const auto button = box(id, area.right);
            text(dc, filter_names[i], {label_inset, button.top - 46, right, button.top - 9}, regular_, colors_.dim);
            if (i + 1 < filter_count)
                divider(button.bottom + 19 + section_extra_height(id));
        }
        for (const auto &section : sections) {
            if (expanded(section.header)) {
                auto card = box(section.header, area.right);
                card.bottom = card.top + section.height;
                round_rect(dc, card, 6, detail_colors_.panel);
            }
        }
        if (expanded(essential_filter)) {
            const auto header = box(essential_filter, area.right);
            for (int i = 0; i < slider_count; ++i) {
                const auto slider = box(static_cast<Control>(first_essential_slider + i), area.right);
                text(dc, essential_sliders[i].label, {header.left + detail_inset, slider.top - 33,
                     header.right - detail_inset, slider.top - 7}, regular_, colors_.dim);
            }
        }
        if (expanded(crop_filter)) {
            for (int i = 0; i < crop_edge_count; ++i) {
                const auto edit = crop_box(static_cast<Control>(first_crop_edge + i), area.right);
                round_rect(dc, edit, 6, detail_colors_.track);
                text(dc, crop_names[i], {edit.left + 12, edit.top, edit.left + 66, edit.bottom}, regular_, colors_.dim);
            }
        }
        if (expanded(colors_filter))
            draw_colors(dc, area.right);
        divider(top + preset_divider + extra_height_before(default_preset));
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
        const auto id = static_cast<Control>(item.CtlID);
        if (is_crop_link(id)) {
            fill(dc, box, detail_colors_.panel);
            constexpr int size = crop_link_icons::size;
            crop_icons_.draw(dc, (box.left + box.right - size) / 2, (box.top + box.bottom - size) / 2,
                             crop_linked_[id - link_top_bottom] ? 1 : 0);
            return;
        }
        if (color_type(id, ColorControl::toggle)) {
            fill(dc, box, detail_colors_.panel);
            draw_toggle(dc, box, color_toggle_on(color_row(id)), detail_colors_, small_);
            return;
        }
        if (color_type(id, ColorControl::swatch)) {
            fill(dc, box, detail_colors_.track);
            round_rect(dc, box, 3, colors_.knob);
            InflateRect(&box, -1, -1);
            round_rect(dc, box, 2, colorref(color_settings_.value(color_row(id).value)));
            return;
        }
        const bool flip = item.CtlID <= vertical;
        const bool selected_flip = flip && static_cast<int>(transform_.flip) == static_cast<int>(item.CtlID) - no_flip;
        const auto label = window_text(item.hwndItem);
        fill(dc, box, id == gradient_mode ? detail_colors_.panel : colors_.panel);
        const bool open = expanded(id);
        round_rect(dc, box, 6, open ? detail_colors_.panel : id == gradient_mode ? detail_colors_.track : colors_.track);
        if (open)
            fill(dc, {box.left, box.top + 6, box.right, box.bottom}, detail_colors_.panel);
        if (id >= first_filter && id <= default_preset) {
            const bool preset = item.CtlID == default_preset;
            const auto color = preset ? colors_.text : colors_.dim;
            const int arrow_inset = preset ? 28 : 23;
            const auto direction = preset ? Arrow::up : open ? Arrow::down : Arrow::right;
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
        } else if (id == gradient_mode) {
            std::vector<std::wstring> labels;
            for (const auto &mode : gradient_modes)
                labels.emplace_back(mode.label);
            populate_dropdown(labels, color_settings_.value(ColorValue::gradient_mode));
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
        if (id == gradient_mode)
            set_color_value(color_row(id), index);
        else
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

    void commit_edit(Control id, const std::wstring &value) {
        if (id == rotate) {
            if (const auto degrees = parse_number(value, L" deg")) {
                transform_.degrees = *degrees;
                save_failed_ = !set_transform(player_, transform_);
            }
        } else if (color_type(id, ColorControl::hex)) {
            if (const auto rgb = parse_color(value))
                set_rgb(color_row(id), *rgb);
        } else if (const auto pixels = parse_number(value, L" px");
                   pixels && *pixels >= 0 && *pixels <= max_crop_margin && std::floor(*pixels) == *pixels) {
            const int edge = id - first_crop_edge;
            auto next = margins_;
            next[edge] = static_cast<int>(*pixels);
            if (crop_linked_[edge / 2])
                next[paired_edges[edge]] = next[edge];
            if (next != margins_)
                save_failed_ = !set_crop_margins(player_, next);
        }
    }

    void command(Control id, int event) {
        if (id == options_list && event == LBN_KILLFOCUS) {
            close_dropdown(false);
        } else if (is_edit(id)) {
            if (event == EN_SETFOCUS) {
                original_edit_ = window_text(control(id));
                SendMessageW(control(id), EM_SETSEL, 0, -1);
                show_control(control(id));
            } else if (event == EN_KILLFOCUS) {
                const auto value = window_text(control(id));
                if (value != original_edit_)
                    commit_edit(id, value);
                refresh();
            }
        } else if (event == BN_CLICKED && id >= no_flip && id <= vertical) {
            const auto flip = static_cast<Flip>(id - no_flip);
            if (flip != transform_.flip) {
                transform_.flip = flip;
                save_failed_ = !set_transform(player_, transform_);
                refresh();
            }
        } else if (event == BN_CLICKED && ((id >= zoom && id <= render) || id == gradient_mode)) {
            open_dropdown(id);
        } else if (event == BN_CLICKED && expandable(id)) {
            toggle_section(id);
        } else if (event == BN_CLICKED && is_crop_link(id)) {
            auto &linked = crop_linked_[id - link_top_bottom];
            linked = !linked;
            update_link(id);
        } else if (event == BN_CLICKED && color_type(id, ColorControl::swatch)) {
            choose_color(color_row(id));
        } else if (event == BN_CLICKED && color_type(id, ColorControl::toggle) && IsWindowEnabled(control(id))) {
            toggle_color(color_row(id));
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
            if (wp == slider_preview)
                apply_slider();
            else if (wp == render_refresh && dropdown_ == render && GetCapture() != control(options_list))
                update_renderers(false);
            return 0;
        case WM_MOUSEWHEEL: wheel(GET_WHEEL_DELTA_WPARAM(wp)); return 0;
        case WM_LBUTTONDOWN: {
            const POINT pointer{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (expanded(crop_filter)) {
                const LONG width = bounds().right;
                for (int id = crop_top; id <= crop_right; ++id) {
                    const auto box = crop_box(static_cast<Control>(id), width);
                    if (PtInRect(&box, pointer)) {
                        SetFocus(control(id));
                        return 0;
                    }
                }
            }
            SetFocus(window_);
            if (scroll_.start_drag(window_, bounds(), pointer.x, pointer.y))
                layout();
            return 0;
        }
        case WM_MOUSEMOVE:
            if (scroll_.drag(bounds(), GET_Y_LPARAM(lp)))
                layout();
            return 0;
        case WM_LBUTTONUP: scroll_.end_drag(); return 0;
        case WM_CAPTURECHANGED: scroll_.capture_lost(); return 0;
        case WM_COMMAND: command(static_cast<Control>(LOWORD(wp)), HIWORD(wp)); return 0;
        case WM_DRAWITEM: draw_item(*reinterpret_cast<const DRAWITEMSTRUCT *>(lp)); return TRUE;
        case WM_HSCROLL: {
            const auto id = static_cast<Control>(GetDlgCtrlID(reinterpret_cast<HWND>(lp)));
            if (is_slider(id))
                move_slider(id, LOWORD(wp));
            return 0;
        }
        case WM_NOTIFY: {
            const auto note = reinterpret_cast<NMHDR *>(lp);
            const int id = static_cast<int>(note->idFrom);
            if (is_slider(id)) {
                if (note->code == NM_RELEASEDCAPTURE) {
                    save_sliders();
                } else if (note->code == NM_CUSTOMDRAW) {
                    const bool dark_theme = &colors_ == &dark;
                    const int divisions = is_essential_slider(id) ? essential_sliders[id - first_essential_slider].divisions :
                                          id == sepia_slider ? 3 : 0;
                    const SliderTicks ticks{divisions,
                                            dark_theme ? RGB(37, 32, 26) : colors_.edge,
                                            dark_theme ? RGB(76, 52, 22) : RGB(172, 144, 115)};
                    const auto thumb = dark_theme ? RGB(171, 99, 20) : colors_.accent;
                    auto palette = detail_colors_;
                    std::optional<COLORREF> disabled_fill;
                    if (id == threshold_saturation || id == threshold_similarity) {
                        palette.knob = dark_theme ? RGB(118, 118, 118) : colors_.knob;
                        disabled_fill = colors_.edge;
                    }
                    return draw_slider(*reinterpret_cast<NMCUSTOMDRAW *>(lp), palette, false, thumb, ticks, disabled_fill);
                }
            }
            break;
        }
        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLOREDIT: {
            const auto dc = reinterpret_cast<HDC>(wp);
            auto background = colors_.track;
            if (message == WM_CTLCOLORLISTBOX)
                background = colors_.button;
            else if (const auto id = GetDlgCtrlID(reinterpret_cast<HWND>(lp));
                     is_crop_edge(id) || color_type(id, ColorControl::hex))
                background = detail_colors_.track;
            SetTextColor(dc, colors_.text);
            SetBkColor(dc, background);
            SetDCBrushColor(dc, background);
            return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
        }
        case WM_DESTROY: save_sliders(); KillTimer(window_, render_refresh); break;
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

bool modal_open(HWND window) {
    const auto view = View::get(window);
    return view && view->has_modal();
}

}
