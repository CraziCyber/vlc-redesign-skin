#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define UNICODE
#include "subtitle_preferences_view.hpp"
#include "preferences_widgets.hpp"
#include "track_defaults.hpp"

#include <commctrl.h>
#include <commdlg.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cwchar>
#include <memory>
#include <optional>
#include <string_view>

namespace subtitle_preferences_view {
namespace {

using namespace subtitle_preferences;
using namespace preferences_widgets;
constexpr wchar_t class_name[] = L"VlcRedesignSubtitlePreferences";
constexpr int inset = 20, field_width = 142, field_height = 34, slider_height = 30;
constexpr int sync_y = 166, appearance_y = sync_y + 371;
constexpr UINT_PTR refresh_timer = 1;

enum Control {
    delay = 101, speed, duration, delay_slider, speed_slider, duration_slider,
    formatted, font, text_color, text_swatch, outline_color, outline_swatch, offset,
    relative, pixels, font_size, thickness, opacity, default_track, default_language,
};

struct Row { Control id; const wchar_t *label; int y; };
constexpr Row rows[] = {
    {default_track, L"Subtitle Track", 58}, {default_language, L"Subtitle Language", 112},
    {delay, L"Subtitle Track Delay", sync_y + 63}, {speed, L"Subtitle Speed", sync_y + 181},
    {duration, L"Subtitle Duration Factor", sync_y + 274}, {formatted, L"Embedded Sub Format", appearance_y + 60},
    {font, L"Subtitle Font", appearance_y + 111}, {text_color, L"Text Color", appearance_y + 162},
    {outline_color, L"Outline Color", appearance_y + 213}, {offset, L"Subtitle Offset", appearance_y + 264},
    {relative, L"Size Values", appearance_y + 315}, {font_size, L"Font Size", appearance_y + 366},
    {thickness, L"Outline Thickness", appearance_y + 417}, {opacity, L"Background Opacity", appearance_y + 468},
};
constexpr int content_height = rows[std::size(rows) - 1].y + field_height + 20;

struct Slider { Control id; Control field; Timing setting; int y, min, max, step; };
constexpr Slider sliders[] = {
    {delay_slider, delay, Timing::delay, sync_y + 111, -10000, 10000, 50},
    {speed_slider, speed, Timing::speed, sync_y + 230, 1000, 100000, 200},
    {duration_slider, duration, Timing::duration, sync_y + 323, 0, 20000, 200},
};

COLORREF colorref(std::uint32_t rgb) {
    return RGB((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
}

std::wstring hex_text(std::uint32_t color) {
    wchar_t text[8];
    std::swprintf(text, std::size(text), L"#%06X", static_cast<unsigned>(color & 0xffffff));
    return text;
}

std::optional<std::uint32_t> parse_color(std::wstring_view text) {
    text = trim(text);
    if (!text.empty() && text.front() == L'#')
        text.remove_prefix(1);
    if (text.size() != 6 || text.find_first_not_of(L"0123456789abcdefABCDEF") != std::wstring_view::npos)
        return std::nullopt;
    return static_cast<std::uint32_t>(std::wcstoul(std::wstring(text).c_str(), nullptr, 16));
}

bool is_edit(Control id) {
    return id == default_track || id == default_language || id == delay || id == speed || id == duration ||
           id == text_color || id == outline_color || id == offset || id == font_size || id == thickness || id == opacity;
}

class View {
public:
    View(HWND video_owner, Player player, bool light_theme) : player_(player), colors_(light_theme ? light : dark),
        defaults_(track_defaults::load(player, track_defaults::Kind::subtitles)),
        style_(appearance(player)), video_owner_(video_owner),
        last_pixel_size_(style_.font_pixels) {}

    ~View() {
        for (const auto font_handle : {regular_, section_, small_})
            if (font_handle)
                DeleteObject(font_handle);
    }

    static HWND open(HWND parent, HWND video_owner, const RECT &bounds, Player player, bool light_theme) {
        HINSTANCE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(&procedure), &module))
            return nullptr;
        WNDCLASSW cls{};
        cls.lpfnWndProc = procedure;
        cls.hInstance = module;
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        cls.lpszClassName = class_name;
        if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return nullptr;

        auto view = std::make_unique<View>(video_owner, player, light_theme);
        // Present the native controls together when switching to this page.
        HWND window = CreateWindowExW(WS_EX_CONTROLPARENT | WS_EX_COMPOSITED, class_name, L"Subtitle Preferences",
                                      WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                                      bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
                                      parent, nullptr, module, view.get());
        if (window)
            view.release()->window_owned_ = true;
        return window;
    }

    static View *get(HWND window) {
        return reinterpret_cast<View *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    }

    bool has_modal() const { return modal_; }

    void visible(bool show) {
        if (!show && (GetFocus() == window_ || IsChild(window_, GetFocus())))
            SetFocus(GetParent(window_));
        ShowWindow(window_, show ? SW_SHOWNOACTIVATE : SW_HIDE);
        if (show)
            refresh_timing();
    }

    bool input(const MSG &message) {
        if (modal_ || !IsWindowVisible(window_))
            return false;
        if (wheel_over(window_, message)) {
            on_wheel(GET_WHEEL_DELTA_WPARAM(message.wParam));
            return true;
        }
        if (message.message != WM_KEYDOWN ||
            (message.hwnd != window_ && !IsChild(window_, message.hwnd)))
            return false;
        if (message.wParam == VK_TAB) {
            move_focus(GetKeyState(VK_SHIFT) < 0);
            return true;
        }
        if (edited_ && (message.wParam == VK_RETURN || message.wParam == VK_ESCAPE)) {
            if (message.wParam == VK_ESCAPE)
                SetWindowTextW(control(*edited_), original_.c_str());
            SetFocus(window_);
            return true;
        }
        if (edited_ && message.wParam == 'A' && GetKeyState(VK_CONTROL) < 0) {
            SendMessageW(control(*edited_), EM_SETSEL, 0, -1);
            return true;
        }
        return false;
    }

private:
    Player player_;
    const Colors &colors_;
    track_defaults::Preset defaults_;
    Appearance style_;
    Synchronization sync_{};
    HWND window_ = nullptr, video_owner_;
    HFONT regular_ = nullptr, section_ = nullptr, small_ = nullptr;
    Scroll scroll_{content_height};
    int last_pixel_size_;
    bool window_owned_ = false, modal_ = false, reopen_media_ = false, save_failed_ = false;
    const wchar_t *defaults_notice_ = nullptr;
    std::optional<Control> edited_;
    std::wstring original_;
    std::array<COLORREF, 16> custom_colors_{};

    HWND control(Control id) const { return GetDlgItem(window_, id); }

    RECT bounds() const {
        RECT area{};
        GetClientRect(window_, &area);
        return area;
    }

    bool add_control(Control id, const wchar_t *class_id, DWORD style, const wchar_t *caption = L"") {
        const HWND child = CreateWindowExW(0, class_id, caption, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style,
                                           0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                           reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(window_, GWLP_HINSTANCE)), nullptr);
        if (child) {
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(regular_), FALSE);
            if (is_edit(id)) {
                SendMessageW(child, EM_SETLIMITTEXT, 32, 0);
                SendMessageW(child, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(0, 1));
            }
        }
        return child != nullptr;
    }

    bool initialize() {
        INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_BAR_CLASSES};
        InitCommonControlsEx(&icc);
        regular_ = make_font(15);
        section_ = make_font(18, FW_SEMIBOLD);
        small_ = make_font(12);
        for (const auto &row : rows) {
            if (!add_control(row.id, is_edit(row.id) ? L"EDIT" : L"BUTTON",
                             is_edit(row.id) ? ES_AUTOHSCROLL : BS_OWNERDRAW, is_edit(row.id) ? L"" : row.label))
                return false;
        }
        if (!add_control(pixels, L"BUTTON", BS_OWNERDRAW, L"Pixels") ||
            !add_control(text_swatch, L"BUTTON", BS_OWNERDRAW, L"Choose text color") ||
            !add_control(outline_swatch, L"BUTTON", BS_OWNERDRAW, L"Choose outline color"))
            return false;
        SetWindowTextW(control(relative), L"Relative");
        for (const auto &slider : sliders) {
            if (!add_control(slider.id, TRACKBAR_CLASSW, TBS_NOTICKS | TBS_FIXEDLENGTH | TBS_TRANSPARENTBKGND))
                return false;
            const auto child = control(slider.id);
            SendMessageW(child, TBM_SETRANGEMIN, FALSE, slider.min);
            SendMessageW(child, TBM_SETRANGEMAX, FALSE, slider.max);
            SendMessageW(child, TBM_SETTHUMBLENGTH, 27, 0);
            SendMessageW(child, TBM_SETLINESIZE, 0, slider.step);
            SendMessageW(child, TBM_SETPAGESIZE, 0, 10 * slider.step);
        }
        refresh_defaults();
        refresh_timing();
        refresh_appearance();
        SetTimer(window_, refresh_timer, 240, nullptr);
        return true;
    }

    RECT field_box(Control id, int y) const {
        int right = bounds().right - inset;
        int width = field_width;
        if (id == formatted)
            width = 70;
        if (id == relative || id == pixels) {
            width = 91;
            if (id == relative)
                right -= width + 2;
        }
        return {right - width, y - scroll_.offset(), right, y - scroll_.offset() + field_height};
    }

    void place(Control id, const RECT &box) {
        SetWindowPos(control(id), nullptr, box.left, box.top, box.right - box.left, box.bottom - box.top,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS | SWP_NOREDRAW);
    }

    void layout() {
        const auto area = bounds();
        for (const auto &row : rows) {
            auto box = field_box(row.id, row.y);
            if (is_edit(row.id)) {
                box.left += 12;
                box.right -= 8;
                box.top += 7;
                box.bottom -= 6;
                if (row.id == text_color || row.id == outline_color)
                    box.right -= 27;
            }
            place(row.id, box);
            if (row.id == relative)
                place(pixels, field_box(pixels, row.y));
            if (row.id == text_color || row.id == outline_color) {
                const auto edge = field_box(row.id, row.y);
                place(row.id == text_color ? text_swatch : outline_swatch,
                      {edge.right - 29, edge.top + 5, edge.right - 5, edge.bottom - 5});
            }
        }
        for (const auto &slider : sliders)
            place(slider.id, {inset - 7, slider.y - scroll_.offset(), area.right - inset + 7, slider.y - scroll_.offset() + slider_height});
        // During a scroll, a child can move through another control's old
        // position. Repaint the viewport and controls after all moves finish.
        RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_NOERASE);
    }

    void on_wheel(int delta) {
        if (scroll_.wheel(delta, bounds()))
            layout();
    }

    void show_control(HWND child) {
        if (scroll_.show(window_, child, bounds()))
            layout();
    }

    void move_focus(bool backwards) {
        constexpr Control order[] = {default_track, default_language, delay, delay_slider,
                                     speed, speed_slider, duration, duration_slider,
                                     formatted, font, text_color, text_swatch, outline_color, outline_swatch,
                                     offset, relative, pixels, font_size, thickness, opacity};
        const auto focus = GetFocus();
        const auto begin = std::begin(order), end = std::end(order);
        const auto found = std::find(begin, end, static_cast<Control>(GetDlgCtrlID(focus)));
        const int count = static_cast<int>(std::size(order));
        int index = found == end ? (backwards ? 0 : count - 1) : static_cast<int>(found - begin);
        for (int i = 0; i < count; ++i) {
            index = (index + (backwards ? count - 1 : 1)) % count;
            const auto child = control(order[index]);
            if (IsWindowEnabled(child)) {
                show_control(child);
                SetFocus(child);
                break;
            }
        }
    }

    void draw(HDC dc) const {
        const auto area = bounds();
        fill(dc, area, colors_.panel);
        text(dc, L"Defaults", {inset, 17 - scroll_.offset(), area.right - inset, 45 - scroll_.offset()}, section_, colors_.heading);
        if (defaults_notice_)
            text(dc, defaults_notice_, {inset, 147 - scroll_.offset(), area.right - inset, 164 - scroll_.offset()}, small_, colors_.dim);
        fill(dc, {0, sync_y - scroll_.offset(), area.right, sync_y - scroll_.offset() + 1}, colors_.edge);
        text(dc, L"Synchronization", {inset, sync_y + 17 - scroll_.offset(), area.right - inset, sync_y + 45 - scroll_.offset()},
             section_, colors_.heading);
        if (!sync_.active)
            text(dc, L"Play media to adjust synchronization.",
                 {inset, sync_y + 44 - scroll_.offset(), area.right - inset, sync_y + 59 - scroll_.offset()}, small_, colors_.dim);

        for (const auto &row : rows) {
            const auto box = field_box(row.id, row.y);
            const int label_end = row.id == relative ? box.left - 8 : area.right - inset - field_width - 8;
            text(dc, row.label, {inset, box.top, label_end, box.bottom}, regular_, colors_.text);
            if (is_edit(row.id)) {
                round_rect(dc, box, 6, colors_.edge);
                auto inner = box;
                InflateRect(&inner, -2, -2);
                round_rect(dc, inner, 4, colors_.track);
            }
        }

        const RECT ticks{inset - 2, sync_y + 144 - scroll_.offset(), area.right - inset + 2, sync_y + 165 - scroll_.offset()};
        text(dc, L"-10 s", ticks, small_, colors_.dim);
        text(dc, L"0 s", ticks, small_, colors_.dim, DT_CENTER);
        text(dc, L"+10 s", ticks, small_, colors_.dim, DT_RIGHT);
        fill(dc, {0, appearance_y - scroll_.offset(), area.right, appearance_y - scroll_.offset() + 1}, colors_.edge);
        text(dc, L"Appearance", {inset, appearance_y + 11 - scroll_.offset(), area.right - inset, appearance_y + 40 - scroll_.offset()},
             section_, colors_.heading);
        if (const auto note = save_failed_ ? L"VLC could not save these preferences." :
                             reopen_media_ ? L"Reopen the video to apply all appearance changes." : nullptr)
            text(dc, note, {inset, appearance_y + 40 - scroll_.offset(), area.right - inset, appearance_y + 55 - scroll_.offset()},
                 small_, colors_.dim);
        if (scroll_.limit(area))
            round_rect(dc, scroll_.thumb(area), 2, colors_.knob);
    }

    void draw_button(const DRAWITEMSTRUCT &item) const {
        const auto id = static_cast<Control>(item.CtlID);
        RECT box = item.rcItem;
        const auto dc = item.hDC;
        fill(dc, box, colors_.panel);
        if (id == text_swatch || id == outline_swatch) {
            fill(dc, box, colors_.track);
            round_rect(dc, box, 3, colors_.knob);
            InflateRect(&box, -2, -2);
            round_rect(dc, box, 2, colorref(id == text_swatch ? style_.color : style_.outline_color));
        } else if (id == formatted) {
            draw_toggle(dc, box, style_.formatted, colors_, small_);
        } else {
            const bool selected = (id == relative && !style_.font_pixels) || (id == pixels && style_.font_pixels);
            round_rect(dc, box, 6, colors_.edge);
            InflateRect(&box, -2, -2);
            const auto background = (item.itemState & ODS_SELECTED) ? colors_.hover :
                selected ? colors_.fill : id == font ? colors_.track : colors_.button;
            round_rect(dc, box, 4, background);
            if (id == font) {
                const auto label = style_.font.empty() ? std::wstring(L"Default") : wstring(style_.font);
                text(dc, label.c_str(), {box.left + 10, box.top, box.right - 22, box.bottom}, regular_, colors_.text);
                const int x = box.right - 13, y = (box.top + box.bottom) / 2;
                fill(dc, {x-3, y-1, x+4, y}, colors_.dim);
                fill(dc, {x-2, y, x+3, y+1}, colors_.dim);
                fill(dc, {x-1, y+1, x+2, y+2}, colors_.dim);
            } else {
                text(dc, id == relative ? L"Relative" : L"Pixels", box, regular_,
                     selected ? colors_.accent : colors_.text, DT_CENTER);
            }
        }
    }

    void set_text(Control id, const std::wstring &text) const {
        set_unfocused_text(control(id), text);
    }

    void show_timing(const Slider &slider, double value, bool enabled) const {
        const auto w = control(slider.id);
        const int position = static_cast<int>(std::lround(std::clamp(value * 1000, double(slider.min), double(slider.max))));
        if (SendMessageW(w, TBM_GETPOS, 0, 0) != position)
            SendMessageW(w, TBM_SETPOS, TRUE, position);
        if (IsWindowEnabled(w) != enabled) {
            EnableWindow(w, enabled);
            EnableWindow(control(slider.field), enabled);
        }
        set_text(slider.field, number_text(value, 3, unit(slider.field), true));
    }

    void refresh_timing() {
        const auto was_active = sync_.active;
        sync_ = timing(player_);
        show_timing(sliders[0], sync_.delay, sync_.active);
        show_timing(sliders[1], sync_.speed, sync_.active);
        show_timing(sliders[2], sync_.duration, sync_.video);
        if (was_active != sync_.active)
            InvalidateRect(window_, nullptr, FALSE);
    }

    const wchar_t *unit(Control id) const {
        switch (id) {
        case delay: return L" s";
        case speed: return L" fps";
        case duration: return sync_.duration_in_seconds ? L" s" : L"";
        case offset: return L" px";
        case font_size:
        case thickness: return style_.font_pixels ? L" px" : L"%";
        case opacity: return L"%";
        default: return L"";
        }
    }

    void refresh_defaults() const {
        set_text(default_track, defaults_.track ? std::to_wstring(defaults_.track) : L"Auto");
        set_text(default_language, wstring(std::string(track_defaults::language_name(defaults_.language))));
    }

    void edit_default(Control id, std::wstring_view value) {
        const auto text = utf8(std::wstring(trim(value)).c_str());
        auto next = defaults_;
        if (id == default_track) {
            if (const auto track = track_defaults::parse_track(text)) {
                next.track = *track;
                defaults_notice_ = nullptr;
            } else {
                defaults_notice_ = L"Enter a track number or Auto.";
            }
        } else {
            if (const auto language = track_defaults::parse_language(text)) {
                next.language = *language;
                defaults_notice_ = nullptr;
            } else {
                defaults_notice_ = L"Enter a language name, code, or Any language.";
            }
        }
        if (!(next == defaults_)) {
            defaults_ = std::move(next);
            if (!track_defaults::save(player_, track_defaults::Kind::subtitles, defaults_))
                defaults_notice_ = L"VLC could not save the subtitle defaults.";
        }
        refresh_defaults();
        InvalidateRect(window_, nullptr, FALSE);
    }

    void refresh_appearance() const {
        set_text(text_color, hex_text(style_.color));
        set_text(outline_color, hex_text(style_.outline_color));
        set_text(offset, number_text(style_.offset, 0, unit(offset)));
        const int divisor = style_.font_divisor > 0 ? style_.font_divisor : default_relative_divisor;
        const double size = style_.font_pixels ? style_.font_pixels : 100.0 / divisor;
        const double outline = style_.font_pixels ? style_.font_pixels * style_.outline_percent / 100.0 : style_.outline_percent;
        set_text(font_size, number_text(size, 2, unit(font_size)));
        set_text(thickness, number_text(outline, 2, unit(thickness)));
        set_text(opacity, number_text(std::lround(style_.background_alpha * 100.0 / 255), 0, unit(opacity)));
        for (const auto id : {formatted, font, text_swatch, outline_swatch, relative, pixels})
            InvalidateRect(control(id), nullptr, FALSE);
    }

    void save(Style setting) {
        const auto result = set_style(player_, setting, style_);
        reopen_media_ |= result.reopen_media;
        save_failed_ = !result.saved;
        refresh_appearance();
        InvalidateRect(window_, nullptr, FALSE);
    }

    void set_number(Control id, double value) {
        for (const auto &slider : sliders) {
            if (id == slider.field) {
                set_timing(player_, slider.setting, value);
                refresh_timing();
                return;
            }
        }
        switch (id) {
        case offset:
            style_.offset = std::lround(std::clamp(value, -4096.0, 4096.0));
            save(Style::offset);
            break;
        case font_size:
            if (style_.font_pixels)
                last_pixel_size_ = style_.font_pixels = std::lround(std::clamp(value, 1.0, 4096.0));
            else
                style_.font_divisor = std::lround(100 / std::clamp(value, 0.1, 100.0));
            save(Style::size);
            break;
        case thickness:
            style_.outline_percent = std::lround(std::clamp(style_.font_pixels ? value * (100.0 / style_.font_pixels) : value,
                                                            0.0, 50.0));
            save(Style::thickness);
            break;
        case opacity:
            style_.background_alpha = std::lround(std::clamp(value, 0.0, 100.0) * 255 / 100);
            save(Style::opacity);
            break;
        default: break;
        }
    }

    void set_color(Control id, std::uint32_t value) {
        if (id == text_color) {
            style_.color = value;
            save(Style::text_color);
        } else {
            style_.outline_color = value;
            save(Style::outline_color);
        }
    }

    void commit_edit(Control id) {
        const auto edited = window_text(control(id));
        edited_.reset();
        if (edited != original_) {
            if (id == default_track || id == default_language) {
                edit_default(id, edited);
            } else if (id == text_color || id == outline_color) {
                if (const auto color = parse_color(edited))
                    set_color(id, *color);
            } else if (const auto value = parse_number(edited, unit(id))) {
                set_number(id, *value);
            }
        }
        refresh_timing();
        refresh_appearance();
    }

    void choose_color(Control id) {
        CHOOSECOLORW choice{};
        choice.lStructSize = sizeof(choice);
        choice.hwndOwner = GetAncestor(window_, GA_ROOT);
        choice.lpCustColors = custom_colors_.data();
        choice.rgbResult = colorref(id == text_color ? style_.color : style_.outline_color);
        choice.Flags = CC_RGBINIT | CC_FULLOPEN;
        modal_ = true;
        const bool accepted = ChooseColorW(&choice);
        modal_ = false;
        if (accepted) {
            const auto value = choice.rgbResult;
            set_color(id, (GetRValue(value) << 16) | (GetGValue(value) << 8) | GetBValue(value));
        }
    }

    void choose_font() {
        LOGFONTW parameters{};
        const auto family = wstring(style_.font);
        family.copy(parameters.lfFaceName, LF_FACESIZE - 1);
        parameters.lfHeight = -15;
        parameters.lfWeight = FW_NORMAL;
        parameters.lfCharSet = DEFAULT_CHARSET;
        CHOOSEFONTW choice{};
        choice.lStructSize = sizeof(choice);
        choice.hwndOwner = GetAncestor(window_, GA_ROOT);
        choice.lpLogFont = &parameters;
        choice.Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT | CF_NOSIZESEL | CF_NOSTYLESEL;
        modal_ = true;
        const bool accepted = ChooseFontW(&choice);
        modal_ = false;
        if (accepted && parameters.lfFaceName[0]) {
            style_.font = utf8(parameters.lfFaceName);
            save(Style::font);
        }
    }

    int starting_pixel_size() const {
        int short_side = 0;
        EnumChildWindows(video_owner_, [](HWND child, LPARAM data) -> BOOL {
            constexpr wchar_t output_class[] = L"VLC video output ";
            wchar_t name[64];
            RECT area{};
            if (!IsWindowVisible(child) || !GetClassNameW(child, name, std::size(name)) ||
                std::wcsncmp(name, output_class, std::size(output_class) - 1) != 0 ||
                !GetClientRect(child, &area))
                return TRUE;
            const int side = std::min(area.right, area.bottom);
            if (side <= 0)
                return TRUE;
            *reinterpret_cast<int *>(data) = side;
            return FALSE;
        }, reinterpret_cast<LPARAM>(&short_side));
        constexpr int reference_height = 720;
        return relative_font_pixels(short_side ? short_side : reference_height, style_.font_divisor);
    }

    void command(Control id, int event) {
        if (is_edit(id)) {
            if (event == EN_SETFOCUS) {
                edited_ = id;
                original_ = window_text(control(id));
                SendMessageW(control(id), EM_SETSEL, 0, -1);
                show_control(control(id));
            } else if (event == EN_KILLFOCUS && edited_ == id) {
                commit_edit(id);
            }
            return;
        }
        if (event != BN_CLICKED)
            return;
        switch (id) {
        case formatted:
            style_.formatted = !style_.formatted;
            save(Style::format);
            break;
        case font: choose_font(); break;
        case text_swatch: choose_color(text_color); break;
        case outline_swatch: choose_color(outline_color); break;
        case relative:
        case pixels:
            if (bool(style_.font_pixels) != (id == pixels)) {
                if (id == relative)
                    style_.font_pixels = 0;
                else
                    style_.font_pixels = last_pixel_size_ > 0 ? last_pixel_size_ : starting_pixel_size();
                save(Style::size);
            }
            break;
        default: break;
        }
    }

    LRESULT message(UINT message, WPARAM wp, LPARAM lp) {
        switch (message) {
        case WM_CREATE: return initialize() ? 0 : -1;
        case WM_SIZE:
            scroll_.move(scroll_.offset(), bounds());
            layout();
            return 0;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: paint(window_, bounds(), [this](HDC dc) { draw(dc); }); return 0;
        case WM_PRINTCLIENT: draw(reinterpret_cast<HDC>(wp)); return 0;
        case WM_TIMER:
            if (IsWindowVisible(window_))
                refresh_timing();
            return 0;
        case WM_MOUSEWHEEL: on_wheel(GET_WHEEL_DELTA_WPARAM(wp)); return 0;
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
        case WM_HSCROLL:
            for (const auto &slider : sliders)
                if (control(slider.id) == reinterpret_cast<HWND>(lp))
                    set_number(slider.field, SendMessageW(control(slider.id), TBM_GETPOS, 0, 0) / 1000.0);
            return 0;
        case WM_DRAWITEM:
            draw_button(*reinterpret_cast<const DRAWITEMSTRUCT *>(lp));
            return TRUE;
        case WM_NOTIFY: {
            const auto notification = reinterpret_cast<NMHDR *>(lp);
            if (notification->code == NM_CUSTOMDRAW)
                return draw_slider(*reinterpret_cast<NMCUSTOMDRAW *>(lp), colors_, notification->idFrom == delay_slider);
            break;
        }
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORSTATIC: {
            const auto dc = reinterpret_cast<HDC>(wp);
            SetTextColor(dc, message == WM_CTLCOLOREDIT ? colors_.text : colors_.dim);
            SetBkColor(dc, colors_.track);
            SetDCBrushColor(dc, colors_.track);
            return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
        }
        case WM_DESTROY: KillTimer(window_, refresh_timer); break;
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

HWND create(HWND parent, HWND video_owner, const RECT &bounds, Player player, bool light) {
    return View::open(parent, video_owner, bounds, player, light);
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
