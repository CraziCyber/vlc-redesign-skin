#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define UNICODE
#include "audio_preferences_view.hpp"
#include "audio_preferences.hpp"
#include "equalizer_icons.hpp"
#include "preferences_widgets.hpp"
#include "track_defaults.hpp"
#include "vlc_config.hpp"

#include <utility>

namespace audio_preferences_view {
namespace {

using namespace preferences_widgets;
using namespace audio_preferences;
constexpr wchar_t class_name[] = L"VlcRedesignAudioPreferences";
constexpr int inset = 20, label_inset = 24, field_width = 142, toggle_width = 68, option_height = 27;
constexpr int preset_height = 46, preset_list_height = 300, dropdown_radius = 6;
constexpr int sync_y = 292, filters_y = 476, compressor_y = 756, spatializer_y = 881;
constexpr int widener_y = 1006, pitch_y = 1131, content_height = 1229;
constexpr int expand_height = 48, detail_inset = 16, slider_units = 10;
constexpr int equalizer_expand_y = filters_y + 154, compressor_expand_y = compressor_y + 57;
constexpr int spatializer_expand_y = spatializer_y + 57, widener_expand_y = widener_y + 57;
constexpr int gain_spacing = 90, gain_start = equalizer_expand_y + 60;
constexpr int detail_start = 87, detail_spacing = 76, detail_end = 47;
constexpr int gain_count = std::size(EqualizerGains{});
constexpr int compressor_count = std::size(compressor_parameters);
constexpr int spatializer_count = std::size(spatializer_parameters);
constexpr int widener_count = std::size(widener_parameters);
constexpr int spatializer_steps_per_unit = 10;
constexpr int equalizer_height = 60 + (gain_count - 1) * gain_spacing + 88;
constexpr int compressor_height = detail_start + (compressor_count - 1) * detail_spacing + detail_end;
constexpr int spatializer_height = detail_start + (spatializer_count - 1) * detail_spacing + detail_end;
constexpr int widener_height = detail_start + (widener_count - 1) * detail_spacing + detail_end;
constexpr UINT_PTR refresh_timer = 1;
constexpr auto defaults_kind = track_defaults::Kind::audio;

enum Control {
    audio_track = 201, audio_language, stereo_test, stereo_mode, delay, delay_slider,
    equalizer, expand_equalizer, two_pass, compressor, expand_compressor, spatializer,
    expand_spatializer, widener, expand_widener, pitch_slider, choices, preset_choices, first_gain = 220,
    first_compressor_slider = 250, first_spatializer_slider = 260, first_widener_slider = 270,
};
constexpr Control gain_edit(int index) { return static_cast<Control>(first_gain + 2 * index); }
constexpr Control gain_slider(int index) { return static_cast<Control>(first_gain + 2 * index + 1); }
constexpr bool is_gain(int id) { return id >= first_gain && id < first_gain + 2 * gain_count; }
constexpr int gain_index(int id) { return (id - first_gain) / 2; }
constexpr Control compressor_slider(int index) { return static_cast<Control>(first_compressor_slider + index); }
constexpr bool is_compressor_slider(int id) { return id >= first_compressor_slider && id < first_compressor_slider + compressor_count; }
constexpr int compressor_index(int id) { return id - first_compressor_slider; }
constexpr Control spatializer_slider(int index) { return static_cast<Control>(first_spatializer_slider + index); }
constexpr bool is_spatializer_slider(int id) { return id >= first_spatializer_slider && id < first_spatializer_slider + spatializer_count; }
constexpr int spatializer_index(int id) { return id - first_spatializer_slider; }
constexpr Control widener_slider(int index) { return static_cast<Control>(first_widener_slider + index); }
constexpr bool is_widener_slider(int id) { return id >= first_widener_slider && id < first_widener_slider + widener_count; }
constexpr int widener_index(int id) { return id - first_widener_slider; }

constexpr int spatializer_position(float value) {
    // VLC displays whole steps, including 8.0 for the native room size of 0.85.
    return static_cast<int>(value * spatializer_steps_per_unit);
}

constexpr int widener_position(const FilterParameter &parameter, float value) {
    return static_cast<int>(value * parameter.steps_per_unit);
}

enum class Section { equalizer, compressor, spatializer, widener, page };
struct SectionLayout {
    Control header, first_slider;
    int y, height;
};
constexpr SectionLayout sections[] = {
    {expand_equalizer, gain_slider(0), equalizer_expand_y, equalizer_height},
    {expand_compressor, compressor_slider(0), compressor_expand_y, compressor_height},
    {expand_spatializer, spatializer_slider(0), spatializer_expand_y, spatializer_height},
    {expand_widener, widener_slider(0), widener_expand_y, widener_height},
};

Section section_for(Control header) {
    for (std::size_t i = 0; i < std::size(sections); ++i)
        if (sections[i].header == header)
            return static_cast<Section>(i);
    return Section::page;
}

enum class Type { edit, button, slider, expand };
struct Row {
    Control id;
    const wchar_t *caption;
    int y, height, width;  // Zero width fills the row.
    Type type = Type::button;
    Section section = Section::page;
};
constexpr Row page_rows[] = {
    {audio_track, L"Audio Track", 57, 36, field_width, Type::edit},
    {audio_language, L"Audio Language", 110, 36, field_width, Type::edit},
    {stereo_test, L"Test", 171, 34, toggle_width},
    {stereo_mode, L"Stereo Mode", 224, 48, 0},
    {delay, L"Audio Track Delay", sync_y + 59, 36, field_width, Type::edit},
    {delay_slider, L"Audio Track Delay", sync_y + 109, 31, 0, Type::slider},
    {equalizer, L"Equalizer", filters_y + 96, 48, 0},
    {expand_equalizer, L"Expand Settings", equalizer_expand_y, expand_height, 0, Type::expand},
    {two_pass, L"2 Pass", filters_y + 220, 32, toggle_width},
    {compressor, L"Compressor", compressor_y + 14, 32, toggle_width},
    {expand_compressor, L"Expand Settings", compressor_expand_y, expand_height, 0, Type::expand},
    {spatializer, L"Spatializer", spatializer_y + 14, 32, toggle_width},
    {expand_spatializer, L"Expand Settings", spatializer_expand_y, expand_height, 0, Type::expand},
    {widener, L"Stereo Widener", widener_y + 14, 32, toggle_width},
    {expand_widener, L"Expand Settings", widener_expand_y, expand_height, 0, Type::expand},
    {pitch_slider, L"Adjust Pitch", pitch_y + 49, 31, 0, Type::slider},
};
constexpr const wchar_t *gain_labels[gain_count] = {
    L"Preamp", L"60 Hz", L"170 Hz", L"310 Hz", L"600 Hz", L"1 kHz", L"3 kHz", L"6 kHz", L"12 kHz", L"14 kHz", L"16 kHz"
};
constexpr auto rows = [] {
    std::array<Row, std::size(page_rows) + 2 * gain_count + compressor_count + spatializer_count + widener_count> result{};
    auto row = result.begin();
    for (const auto &entry : page_rows) {
        *row++ = entry;
        if (entry.id == expand_equalizer) {
            for (int i = 0; i < gain_count; ++i) {
                const int y = gain_start + i * gain_spacing;
                *row++ = {gain_edit(i), gain_labels[i], y, 32, 138, Type::edit, Section::equalizer};
                *row++ = {gain_slider(i), gain_labels[i], y + 41, 30, 0, Type::slider, Section::equalizer};
            }
        }
        if (entry.id == expand_compressor) {
            for (int i = 0; i < compressor_count; ++i) {
                const int y = compressor_expand_y + detail_start + i * detail_spacing;
                *row++ = {compressor_slider(i), compressor_parameters[i].label, y, 30, 0, Type::slider, Section::compressor};
            }
        }
        if (entry.id == expand_spatializer) {
            for (int i = 0; i < spatializer_count; ++i) {
                const int y = spatializer_expand_y + detail_start + i * detail_spacing;
                *row++ = {spatializer_slider(i), spatializer_parameters[i].label, y, 30, 0, Type::slider, Section::spatializer};
            }
        }
        if (entry.id == expand_widener) {
            for (int i = 0; i < widener_count; ++i) {
                const int y = widener_expand_y + detail_start + i * detail_spacing;
                *row++ = {widener_slider(i), widener_parameters[i].label, y, 30, 0, Type::slider, Section::widener};
            }
        }
    }
    return result;
}();
constexpr const wchar_t *stereo_modes[] = {
    L"Automatic", L"Stereo", L"Reverse Stereo", L"Left", L"Right", L"Dolby Surround", L"Headphones", L"Mono"
};

class PresetIcons {
public:
    void load(HINSTANCE module) {
        using namespace equalizer_icons;
        icons_.load(module, resource, size, count);
    }

    void curve(HDC dc, int x, int y, std::string_view key) const {
        using namespace equalizer_icons;
        const auto icon = std::find(std::begin(keys), std::end(keys), key);
        if (icon != std::end(keys))
            icons_.draw(dc, x, y, static_cast<int>(icon - std::begin(keys)));
    }

    void selected(HDC dc, int x, int y) const {
        icons_.draw(dc, x, y, equalizer_icons::count - 1);
    }

private:
    IconStrip icons_;
};

class View {
public:
    View(VlcPlayer player, bool light_theme)
        : player_(player), colors_(light_theme ? light : dark), defaults_(track_defaults::load(player, defaults_kind)),
          presets_(equalizer_presets(player)) {
        detail_colors_ = colors_;
        detail_colors_.panel = colors_.button;
        if (!light_theme)
            detail_colors_.track = RGB(9, 9, 9);
    }

    ~View() {
        for (auto font : {regular_, section_, small_})
            if (font)
                DeleteObject(font);
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
        // Present the native controls together when a menu opens or the page is shown.
        const HWND window = CreateWindowExW(WS_EX_CONTROLPARENT | WS_EX_COMPOSITED, class_name, L"Audio Preferences",
                                            WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                                            bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
                                            parent, nullptr, module, view.get());
        if (window)
            view.release()->window_owned_ = true;
        return window;
    }

    void visible(bool show) {
        if (!show) {
            close_choices(false);
            if (GetFocus() == window_ || IsChild(window_, GetFocus()))
                SetFocus(GetParent(window_));
            test_.stop();
            save_slider_settings();
        }
        ShowWindow(window_, show ? SW_SHOWNOACTIVATE : SW_HIDE);
        if (show)
            refresh();
    }

    bool input(const MSG &message) {
        if (!IsWindowVisible(window_))
            return false;
        if (dropdown_) {
            const auto id = *dropdown_;
            const auto list = choice_list(id);
            if (id == equalizer && message.hwnd == control(id) &&
                (message.message == WM_LBUTTONDOWN || message.message == WM_LBUTTONDBLCLK)) {
                close_choices(true);
                return true;
            }
            if (message.hwnd == list && message.message == WM_LBUTTONUP) {
                const auto item = SendMessageW(list, LB_ITEMFROMPOINT, 0, message.lParam);
                if (HIWORD(item) == 0) {
                    select_choice(LOWORD(item));
                    return true;
                }
                return false;
            }
            if (message.message == WM_KEYDOWN && message.hwnd == list) {
                if (message.wParam == VK_RETURN) {
                    select_choice(static_cast<int>(SendMessageW(list, LB_GETCURSEL, 0, 0)));
                    return true;
                }
                if (message.wParam == VK_ESCAPE) {
                    close_choices(true);
                    return true;
                }
                if (message.wParam == VK_TAB)
                    close_choices(true);
            }
            if (message.message == WM_LBUTTONDOWN && message.hwnd != list && message.hwnd != control(id))
                close_choices(false);
            if (message.message == WM_MOUSEWHEEL) {
                RECT menu{};
                GetWindowRect(list, &menu);
                const POINT pointer{GET_X_LPARAM(message.lParam), GET_Y_LPARAM(message.lParam)};
                bool over_menu = message.hwnd == list || PtInRect(&menu, pointer);
                if (id == equalizer) {
                    RECT header{};
                    GetWindowRect(control(id), &header);
                    over_menu |= PtInRect(&header, pointer) != FALSE;
                }
                if (over_menu) {
                    scroll_choices(id, GET_WHEEL_DELTA_WPARAM(message.wParam));
                    return true;
                }
                close_choices(true);
            }
        }
        if (wheel_over(window_, message)) {
            wheel(GET_WHEEL_DELTA_WPARAM(message.wParam));
            return true;
        }
        if (message.message != WM_KEYDOWN ||
            (message.hwnd != window_ && !IsChild(window_, message.hwnd)))
            return false;
        if (message.wParam == VK_RETURN) {
            const auto section = section_for(static_cast<Control>(GetDlgCtrlID(message.hwnd)));
            if (section != Section::page) {
                toggle_section(section);
                return true;
            }
        }
        if (message.wParam == VK_TAB) {
            const auto focus = IsChild(window_, GetFocus()) ? GetFocus() : nullptr;
            const auto next = GetNextDlgTabItem(window_, focus, GetKeyState(VK_SHIFT) < 0);
            if (next) {
                show_control(next);
                SetFocus(next);
            }
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
    VlcPlayer player_;
    const Colors &colors_;
    Colors detail_colors_{};
    track_defaults::Preset defaults_;
    std::vector<Preset> presets_;
    PresetIcons preset_icons_;
    Settings settings_{};
    StereoTest test_;
    Scroll scroll_{content_height};
    HWND window_ = nullptr;
    HFONT regular_ = nullptr, section_ = nullptr, small_ = nullptr;
    bool window_owned_ = false, slider_dirty_ = false;
    bool widener_reload_pending_ = false;
    std::array<bool, std::size(sections)> expanded_{};
    const wchar_t *defaults_notice_ = nullptr;
    std::optional<Control> edited_;
    std::optional<Control> dropdown_;
    int choice_wheel_ = 0;
    std::wstring original_;

    HWND control(Control id) const { return GetDlgItem(window_, id); }
    HWND choice_list(Control id) const { return control(id == equalizer ? preset_choices : choices); }

    RECT bounds() const {
        RECT area{};
        GetClientRect(window_, &area);
        return area;
    }

    bool initialize() {
        INITCOMMONCONTROLSEX common{sizeof(common), ICC_BAR_CLASSES};
        InitCommonControlsEx(&common);
        regular_ = make_font(15);
        section_ = make_font(18, FW_SEMIBOLD);
        small_ = make_font(12);
        for (const auto &row : rows) {
            const bool edit = row.type == Type::edit, slider = row.type == Type::slider, gain = is_gain(row.id);
            const bool pending = row.type == Type::expand && section_for(row.id) == Section::page;
            const DWORD style = edit ? ES_AUTOHSCROLL :
                                slider ? TBS_NOTICKS | TBS_FIXEDLENGTH | TBS_TRANSPARENTBKGND : BS_OWNERDRAW;
            const HWND child = CreateWindowExW(0, edit ? L"EDIT" : slider ? TRACKBAR_CLASSW : L"BUTTON",
                                               edit ? L"" : row.caption,
                                               WS_CHILD | (row.section == Section::page ? WS_VISIBLE : 0) | WS_CLIPSIBLINGS |
                                                   (pending ? WS_DISABLED : WS_TABSTOP) | style,
                                               0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(row.id)),
                                               reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(window_, GWLP_HINSTANCE)), nullptr);
            if (!child)
                return false;
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(gain && edit ? small_ : regular_), FALSE);
            if (edit) {
                SendMessageW(child, EM_SETLIMITTEXT, 40, 0);
                SendMessageW(child, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(0, 1));
            } else if (slider) {
                int minimum = -10000, maximum = 10000;
                if (gain) {
                    minimum = slider_units * equalizer_min_db;
                    maximum = slider_units * equalizer_max_db;
                } else if (row.id == pitch_slider) {
                    minimum = -120;
                    maximum = 120;
                } else if (row.section == Section::compressor) {
                    const auto &parameter = compressor_parameters[compressor_index(row.id)];
                    minimum = std::lround(parameter.minimum * slider_units);
                    maximum = std::lround(parameter.maximum * slider_units);
                } else if (row.section == Section::spatializer) {
                    const auto &parameter = spatializer_parameters[spatializer_index(row.id)];
                    minimum = spatializer_position(parameter.minimum);
                    maximum = spatializer_position(parameter.maximum);
                } else if (row.section == Section::widener) {
                    const auto &parameter = widener_parameters[widener_index(row.id)];
                    minimum = widener_position(parameter, parameter.minimum);
                    maximum = widener_position(parameter, parameter.maximum);
                }
                const int step = row.id == delay_slider ? 50 : 1;
                SendMessageW(child, TBM_SETRANGEMIN, FALSE, minimum);
                SendMessageW(child, TBM_SETRANGEMAX, FALSE, maximum);
                SendMessageW(child, TBM_SETTHUMBLENGTH, 27, 0);
                SendMessageW(child, TBM_SETLINESIZE, 0, step);
                SendMessageW(child, TBM_SETPAGESIZE, 0, std::clamp((maximum - minimum) / 10, 1, step * 10));
            }
        }
        const auto module = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(window_, GWLP_HINSTANCE));
        for (const auto id : {choices, preset_choices}) {
            const auto list = CreateWindowExW(0, L"LISTBOX", nullptr,
                                             WS_CHILD | WS_CLIPSIBLINGS | LBS_NOINTEGRALHEIGHT |
                                                 LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
                                             0, 0, 0, 0, window_,
                                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), module, nullptr);
            if (!list)
                return false;
            SendMessageW(list, WM_SETFONT, reinterpret_cast<WPARAM>(regular_), FALSE);
            SendMessageW(list, LB_SETITEMHEIGHT, 0, id == choices ? option_height : preset_height);
        }
        preset_icons_.load(module);
        show_defaults();
        refresh();
        SetTimer(window_, refresh_timer, 240, nullptr);
        return true;
    }

    bool is_expanded(Section section) const {
        return section != Section::page && expanded_[static_cast<std::size_t>(section)];
    }

    int content_y(int y) const {
        int position = y;
        for (std::size_t i = 0; i < std::size(sections); ++i) {
            if (expanded_[i] && y > sections[i].y)
                position += sections[i].height - expand_height;
        }
        return position;
    }

    bool show_row(const Row &row) const {
        return row.section == Section::page || is_expanded(row.section);
    }

    RECT box(const Row &row, LONG width) const {
        const bool detail = row.section != Section::page;
        const int channel_inset = row.section == Section::spatializer || row.section == Section::widener ? 9 : 7;
        const int outer = inset + (detail ? detail_inset : 0) - (row.type == Type::slider ? channel_inset : 0);
        // Detail rows use their header as the anchor for shifts from earlier sections.
        const int anchor = detail ? sections[static_cast<std::size_t>(row.section)].y : row.y;
        const LONG right = width - outer, top = row.y + content_y(anchor) - anchor - scroll_.offset();
        return {row.width ? right - row.width : outer, top, right, top + row.height};
    }

    void layout() {
        const auto width = bounds().right;
        for (const auto &row : rows) {
            if (!show_row(row))
                continue;
            auto area = box(row, width);
            if (row.type == Type::edit) {
                area.left += 13;
                area.right -= 8;
                area.top += 8;
                area.bottom -= 7;
            }
            SetWindowPos(control(row.id), nullptr, area.left, area.top, area.right - area.left, area.bottom - area.top,
                         SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS | SWP_NOREDRAW);
        }
        RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_NOERASE);
    }

    void wheel(int delta) {
        if (scroll_.wheel(delta, bounds()))
            layout();
    }

    void show_control(HWND child) {
        if (scroll_.show(window_, child, bounds()))
            layout();
    }

    void toggle_section(Section section) {
        close_choices(false);
        const auto index = static_cast<std::size_t>(section);
        const auto &details = sections[index];
        const auto button = control(details.header);
        const bool expand = !expanded_[index];
        if (!expand) {
            const auto focus = GetFocus();
            for (const auto &row : rows) {
                if (row.section == section && control(row.id) == focus) {
                    SetFocus(button);
                    break;
                }
            }
        }
        expanded_[index] = expand;
        SetWindowTextW(button, expand ? L"Collapse Settings" : L"Expand Settings");
        scroll_.set_content_height(content_y(content_height));
        scroll_.move(scroll_.offset(), bounds());
        layout();
        for (const auto &row : rows) {
            if (row.section == section)
                ShowWindow(control(row.id), expand ? SW_SHOWNOACTIVATE : SW_HIDE);
        }
        if (expand) {
            refresh();
            show_control(control(details.first_slider));
        }
    }

    COLORREF presets_background() const { return &colors_ == &dark ? RGB(25, 25, 25) : colors_.button; }
    COLORREF preset_selection() const { return &colors_ == &dark ? RGB(31, 31, 31) : colors_.hover; }

    void draw_preset_label(HDC dc, RECT box, int index, const wchar_t *label) const {
        box.left += 16;
        if (index > 0 && static_cast<std::size_t>(index) <= presets_.size()) {
            preset_icons_.curve(dc, box.left, (box.top + box.bottom - equalizer_icons::size) / 2,
                                presets_[index - 1].key);
            box.left += equalizer_icons::size + 10;
        }
        text(dc, label, box, regular_, colors_.text);
    }

    void draw(HDC dc) const {
        const auto area = bounds();
        const auto y = scroll_.offset();
        const auto right = area.right - inset;
        const auto right_of_label = right - field_width - 8;
        fill(dc, area, colors_.panel);
        for (const int divider : {sync_y, filters_y, compressor_y, spatializer_y, widener_y, pitch_y, content_height - 1}) {
            const int line = content_y(divider) - y;
            fill(dc, {0, line, area.right, line + 1}, colors_.edge);
        }
        for (std::size_t i = 0; i < std::size(sections); ++i) {
            if (expanded_[i]) {
                const int top = content_y(sections[i].y) - y;
                round_rect(dc, {inset, top, right, top + sections[i].height}, 6, detail_colors_.panel);
            }
        }
        text(dc, L"Defaults", {label_inset, 14 - y, right, 45 - y}, section_, colors_.heading);
        text(dc, L"Synchronization", {label_inset, sync_y + 13 - y, right, sync_y + 48 - y}, section_, colors_.heading);
        text(dc, L"Filters", {label_inset, filters_y + 13 - y, right, filters_y + 48 - y}, section_, colors_.heading);
        if (defaults_notice_)
            text(dc, defaults_notice_, {label_inset, 146 - y, right, 170 - y}, small_, colors_.dim);
        for (const auto &row : rows) {
            if (!show_row(row))
                continue;
            const auto field = box(row, area.right);
            if (row.section == Section::compressor || row.section == Section::spatializer || row.section == Section::widener) {
                const RECT labels{inset + detail_inset, field.top - 33, right - detail_inset, field.top - 7};
                std::wstring value;
                if (row.section == Section::compressor) {
                    const int index = compressor_index(row.id);
                    value = number_text(settings_.compressor_values[index], 1, compressor_parameters[index].suffix, true);
                } else if (row.section == Section::spatializer) {
                    const int index = spatializer_index(row.id);
                    value = number_text(spatializer_position(settings_.spatializer_values[index]), 1, L"", true);
                } else {
                    const int index = widener_index(row.id);
                    const auto &parameter = widener_parameters[index];
                    const double number = widener_position(parameter, settings_.widener_values[index]) /
                                          double(parameter.steps_per_unit);
                    value = number_text(number, 1, parameter.suffix, true);
                }
                text(dc, row.caption, labels, regular_, colors_.dim);
                text(dc, value.c_str(), labels, regular_, colors_.dim, DT_RIGHT);
            } else if (row.type == Type::edit) {
                const bool gain = is_gain(row.id);
                const int left = gain ? inset + detail_inset : label_inset;
                text(dc, row.caption, {left, field.top, field.left - 8, field.bottom},
                     regular_, gain ? colors_.dim : colors_.text);
                if (gain) {
                    round_rect(dc, field, 4, detail_colors_.track);
                } else {
                    round_rect(dc, field, 6, colors_.edge);
                    auto interior = field;
                    InflateRect(&interior, -2, -2);
                    round_rect(dc, interior, 4, colors_.track);
                }
            } else if (row.id == stereo_test) {
                text(dc, L"Stereo Mode", {label_inset, field.top, right_of_label, field.bottom}, regular_, colors_.text);
            } else if (row.width && row.type == Type::button) {
                text(dc, row.caption, {label_inset, field.top, field.left - 8, field.bottom}, regular_, colors_.text);
            }
        }
        text(dc, L"Equalizer", {label_inset, filters_y + 50 - y, right, filters_y + 88 - y}, regular_, colors_.text);
        const RECT ticks{inset, sync_y + 140 - y, right, sync_y + 163 - y};
        text(dc, L"-10 s", ticks, small_, colors_.dim);
        text(dc, L"0 s", ticks, small_, colors_.dim, DT_CENTER);
        text(dc, L"+10 s", ticks, small_, colors_.dim, DT_RIGHT);
        const int pitch_top = content_y(pitch_y) - y;
        const RECT pitch{label_inset, pitch_top + 12, right, pitch_top + 46};
        text(dc, L"Adjust Pitch", pitch, regular_, colors_.text);
        text(dc, number_text(settings_.pitch, 1, L" semitones", true).c_str(), pitch, regular_, colors_.text, DT_RIGHT);
        if (scroll_.limit(area))
            round_rect(dc, scroll_.thumb(area), 2, colors_.knob);
    }

    void draw_button(const DRAWITEMSTRUCT &item) const {
        const auto id = static_cast<Control>(item.CtlID);
        const auto dc = item.hDC;
        auto area = item.rcItem;
        fill(dc, area, colors_.panel);
        switch (id) {
        case two_pass: draw_toggle(dc, area, settings_.two_pass, colors_, small_); return;
        case compressor: draw_toggle(dc, area, settings_.compressor, colors_, small_); return;
        case spatializer: draw_toggle(dc, area, settings_.spatializer, colors_, small_); return;
        case widener: draw_toggle(dc, area, settings_.widener, colors_, small_); return;
        case stereo_test:
            round_rect(dc, area, 6, (item.itemState & ODS_SELECTED) ? colors_.hover : colors_.fill);
            text(dc, L"Test", area, regular_, colors_.accent, DT_CENTER);
            return;
        default: break;
        }
        if (id == equalizer && dropdown_ == equalizer) {
            const auto background = preset_selection();
            round_rect(dc, area, dropdown_radius, background);
            fill(dc, {area.left, area.top + dropdown_radius, area.right, area.bottom}, background);
            preset_icons_.selected(dc, area.right - 40, (area.top + area.bottom - equalizer_icons::size) / 2);
            const auto label = window_text(item.hwndItem);
            area.right -= 45;
            draw_preset_label(dc, area, equalizer_choice(), label.c_str());
            return;
        }
        const bool dropdown = id == stereo_mode || id == equalizer;
        const bool expanded = is_expanded(section_for(id));
        round_rect(dc, area, 6, expanded ? detail_colors_.panel : colors_.track);
        if (expanded)
            fill(dc, {area.left, area.top + 6, area.right, area.bottom}, detail_colors_.panel);
        if (dropdown) {
            InflateRect(&area, -2, -2);
            round_rect(dc, area, 4, (item.itemState & ODS_SELECTED) ? colors_.hover : colors_.button);
        }
        const auto label = window_text(item.hwndItem);
        if (id == equalizer) {
            draw_preset_label(dc, {area.left - 2, area.top, area.right - 38, area.bottom},
                              equalizer_choice(), label.c_str());
        } else {
            text(dc, label.c_str(), {area.left + 16, area.top, area.right - 38, area.bottom},
                 regular_, dropdown ? colors_.text : colors_.dim);
        }
        arrow(dc, area.right - 23, (area.top + area.bottom) / 2, dropdown || expanded ? Arrow::down : Arrow::right,
              dropdown ? colors_.text : colors_.dim);
    }

    void draw_choice(const DRAWITEMSTRUCT &item) const {
        const auto dc = item.hDC;
        const bool equalizer_list = item.CtlID == preset_choices;
        const bool selected = (item.itemState & ODS_SELECTED) != 0;
        const auto background = equalizer_list ? (selected ? preset_selection() : presets_background()) :
                                                (selected ? colors_.fill : colors_.button);
        fill(dc, item.rcItem, background);
        if (item.itemID == UINT(-1))
            return;
        wchar_t label[128]{};
        if (SendMessageW(item.hwndItem, LB_GETTEXTLEN, item.itemID, 0) < static_cast<LRESULT>(std::size(label))) {
            SendMessageW(item.hwndItem, LB_GETTEXT, item.itemID, reinterpret_cast<LPARAM>(label));
            auto box = item.rcItem;
            box.right -= 8;
            if (equalizer_list) {
                const auto index = SendMessageW(item.hwndItem, LB_GETITEMDATA, item.itemID, 0);
                draw_preset_label(dc, box, static_cast<int>(index), label);
            } else {
                box.left += 13;
                text(dc, label, box, regular_, selected ? colors_.accent : colors_.text);
            }
        }
    }

    int stereo_choice() const {
        const int count = std::size(stereo_modes);
        const int mode = settings_.stereo_mode;
        return mode >= 0 && mode < count ? mode : 0;
    }

    int equalizer_choice() const {
        if (settings_.equalizer.empty())
            return 0;
        const auto preset = std::find_if(presets_.begin(), presets_.end(),
                                         [&](const Preset &item) { return item.key == settings_.equalizer; });
        return preset == presets_.end() ? -1 : 1 + static_cast<int>(preset - presets_.begin());
    }

    void show_defaults() const {
        set_unfocused_text(control(audio_track), defaults_.track ? std::to_wstring(defaults_.track) : L"Auto");
        set_unfocused_text(control(audio_language), wstring(track_defaults::language_name(defaults_.language)));
    }

    void set_button_text(Control id, const std::wstring &label) const {
        const auto child = control(id);
        if (window_text(child) != label)
            SetWindowTextW(child, label.c_str());
    }

    void set_slider_position(Control id, int position) const {
        const auto slider = control(id);
        if (SendMessageW(slider, TBM_GETPOS, 0, 0) != position)
            SendMessageW(slider, TBM_SETPOS, TRUE, position);
    }

    void refresh_gains() {
        for (int i = 0; i < gain_count; ++i) {
            const int position = std::lround(settings_.gains[i] * slider_units);
            set_unfocused_text(control(gain_edit(i)), number_text(position / double(slider_units), 1, L" dB", true));
            set_slider_position(gain_slider(i), position);
        }
    }

    bool change_gain(int index, double value) {
        const double bounded = std::clamp(value, double(equalizer_min_db), double(equalizer_max_db));
        const float step = std::lround(bounded * slider_units) / float(slider_units);
        if (step == settings_.gains[index])
            return false;
        auto gains = settings_.gains;
        gains[index] = step;
        set_equalizer_gains(player_, gains);
        return true;
    }

    void refresh() {
        const auto old = std::exchange(settings_, read(player_, presets_));
        if (dropdown_ == equalizer && old.equalizer != settings_.equalizer)
            close_choices(true);
        set_button_text(stereo_mode, stereo_modes[stereo_choice()]);
        const int choice = equalizer_choice();
        const auto preset = choice > 0 ? wstring(presets_[choice - 1].label) : std::wstring(choice == 0 ? L"None" : L"Custom");
        set_button_text(equalizer, preset);
        if (is_expanded(Section::equalizer))
            refresh_gains();
        if (is_expanded(Section::compressor)) {
            for (int i = 0; i < compressor_count; ++i)
                set_slider_position(compressor_slider(i), std::lround(settings_.compressor_values[i] * slider_units));
            if (old.compressor_values != settings_.compressor_values)
                InvalidateRect(window_, nullptr, FALSE);
        }
        if (is_expanded(Section::spatializer)) {
            for (int i = 0; i < spatializer_count; ++i)
                set_slider_position(spatializer_slider(i), spatializer_position(settings_.spatializer_values[i]));
            if (old.spatializer_values != settings_.spatializer_values)
                InvalidateRect(window_, nullptr, FALSE);
        }
        if (is_expanded(Section::widener)) {
            for (int i = 0; i < widener_count; ++i)
                set_slider_position(widener_slider(i), widener_position(widener_parameters[i], settings_.widener_values[i]));
            if (old.widener_values != settings_.widener_values)
                InvalidateRect(window_, nullptr, FALSE);
        }
        const double seconds = settings_.delay.value_or(0);
        set_unfocused_text(control(delay), number_text(seconds, 3, L" s", true));
        const int delay_position = std::lround(std::clamp(seconds, -10.0, 10.0) * 1000);
        const int pitch_position = std::lround(std::clamp(settings_.pitch, -12.0f, 12.0f) * 10);
        set_slider_position(delay_slider, delay_position);
        set_slider_position(pitch_slider, pitch_position);
        for (const auto id : {delay, delay_slider})
            if (bool(IsWindowEnabled(control(id))) != settings_.delay.has_value())
                EnableWindow(control(id), settings_.delay.has_value());
        if (old.pitch != settings_.pitch)
            InvalidateRect(window_, nullptr, FALSE);
        if (old.two_pass != settings_.two_pass)
            InvalidateRect(control(two_pass), nullptr, FALSE);
        if (old.compressor != settings_.compressor)
            InvalidateRect(control(compressor), nullptr, FALSE);
        if (old.spatializer != settings_.spatializer)
            InvalidateRect(control(spatializer), nullptr, FALSE);
        if (old.widener != settings_.widener)
            InvalidateRect(control(widener), nullptr, FALSE);
    }

    void edit_default(Control id, const std::wstring &value) {
        const auto entered = utf8(std::wstring(trim(value)).c_str());
        auto next = defaults_;
        if (id == audio_track) {
            const auto track = track_defaults::parse_track(entered);
            defaults_notice_ = track ? nullptr : L"Enter a track number or Auto.";
            if (track)
                next.track = *track;
        } else {
            const auto language = track_defaults::parse_language(entered);
            defaults_notice_ = language ? nullptr : L"Enter a language name, code, or Any language.";
            if (language)
                next.language = *language;
        }
        if (!(next == defaults_)) {
            defaults_ = std::move(next);
            if (!track_defaults::save(player_, defaults_kind, defaults_))
                defaults_notice_ = L"VLC could not save the audio defaults.";
        }
        show_defaults();
        InvalidateRect(window_, nullptr, FALSE);
    }

    void close_choices(bool focus_button) {
        const auto id = std::exchange(dropdown_, std::nullopt);
        if (!id)
            return;
        const auto list = choice_list(*id);
        if (GetCapture() == list)
            ReleaseCapture();
        ShowWindow(list, SW_HIDE);
        InvalidateRect(control(*id), nullptr, FALSE);
        if (focus_button)
            SetFocus(control(*id));
    }

    void open_choices(Control id, const std::vector<std::wstring> &labels, int selected) {
        const bool closing = dropdown_ == id;
        close_choices(closing);
        if (closing)
            return;
        const auto field = control(id);
        show_control(field);
        RECT box{};
        GetWindowRect(field, &box);
        MapWindowPoints(nullptr, window_, reinterpret_cast<POINT *>(&box), 2);
        const auto area = bounds();
        const bool equalizer_list = id == equalizer;
        int top, height;
        if (equalizer_list) {
            const int desired = std::min<int>(preset_list_height, (labels.size() - 1) * preset_height);
            height = std::min<int>(desired, std::max(1L, area.bottom - (box.bottom - box.top)));
            const int offset = scroll_.offset();
            const int shortage = std::max(0L, box.bottom + height - area.bottom);
            if (scroll_.move(offset + shortage, area)) {
                layout();
                OffsetRect(&box, 0, offset - scroll_.offset());
            }
            top = box.bottom;
            height = std::min<int>(height, area.bottom - top);
            if (height <= 0)
                return;
        } else {
            const int below = area.bottom - box.bottom, above = box.top;
            const int desired = std::min<int>(labels.size(), 8) * option_height;
            const bool down = below >= desired || below >= above;
            const int count = std::max(1, (down ? below : above) / option_height);
            height = std::min(desired, count * option_height);
            top = std::clamp<int>(down ? box.bottom : box.top - height, 0, std::max(0L, area.bottom - height));
        }
        const auto list = choice_list(id);
        SendMessageW(list, LB_RESETCONTENT, 0, 0);
        for (std::size_t i = 0; i < labels.size(); ++i) {
            if (equalizer_list && static_cast<int>(i) == selected)
                continue;
            const auto index = SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(labels[i].c_str()));
            if (equalizer_list && index >= 0)
                SendMessageW(list, LB_SETITEMDATA, index, i);
        }
        dropdown_ = id;
        choice_wheel_ = 0;
        const int width = box.right - box.left;
        if (equalizer_list) {
            // The button stays above the scrolling choices and supplies the rounded top of the menu.
            const auto shape = CreateRoundRectRgn(0, -dropdown_radius, width + 1, height + 1,
                                                  2 * dropdown_radius, 2 * dropdown_radius);
            if (shape && !SetWindowRgn(list, shape, FALSE))
                DeleteObject(shape);
            InvalidateRect(field, nullptr, FALSE);
        }
        SetWindowPos(list, HWND_TOP, box.left, top, width, height, SWP_SHOWWINDOW | SWP_NOACTIVATE);
        SendMessageW(list, LB_SETCURSEL, equalizer_list ? -1 : selected, 0);
        SetFocus(list);
    }

    void scroll_choices(Control id, int delta) {
        const auto list = choice_list(id);
        RECT area{};
        GetClientRect(list, &area);
        const int count = SendMessageW(list, LB_GETCOUNT, 0, 0);
        const int current = SendMessageW(list, LB_GETTOPINDEX, 0, 0);
        const int row_height = id == equalizer ? preset_height : option_height;
        const int visible = std::max(1L, area.bottom / row_height);
        int step = 2;
        if (id == stereo_mode) {
            UINT lines = 3;
            SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
            step = lines == WHEEL_PAGESCROLL ? visible : static_cast<int>(std::min<UINT>(lines, std::size(stereo_modes)));
        }
        choice_wheel_ += delta;
        const int position = std::clamp(current - step * (choice_wheel_ / WHEEL_DELTA), 0, std::max(0, count - visible));
        choice_wheel_ %= WHEEL_DELTA;
        if (position != current)
            SendMessageW(list, LB_SETTOPINDEX, position, 0);
    }

    void select_choice(int selected) {
        const auto field = dropdown_;
        if (!field || selected < 0)
            return;
        const auto list = choice_list(*field);
        if (selected >= SendMessageW(list, LB_GETCOUNT, 0, 0))
            return;
        const auto index = *field == equalizer ? SendMessageW(list, LB_GETITEMDATA, selected, 0) : selected;
        if (index < 0 || (*field == equalizer && static_cast<std::size_t>(index) > presets_.size()))
            return;
        close_choices(true);
        if (*field == stereo_mode)
            set_stereo_mode(player_, static_cast<int>(index));
        else
            set_equalizer(player_, index ? &presets_[index - 1] : nullptr);
        vlc_config::save(player_.vlc);
        refresh();
    }

    bool button(Control id) {
        switch (id) {
        case stereo_test:
            defaults_notice_ = test_.play(player_, stereo_choice()) ? nullptr : L"VLC could not play the stereo test.";
            InvalidateRect(window_, nullptr, FALSE);
            return false;
        case stereo_mode: {
            const std::vector<std::wstring> labels(std::begin(stereo_modes), std::end(stereo_modes));
            open_choices(id, labels, stereo_choice());
            return false;
        }
        case equalizer: {
            std::vector<std::wstring> labels{L"None"};
            for (const auto &preset : presets_)
                labels.push_back(wstring(preset.label));
            open_choices(id, labels, equalizer_choice());
            return false;
        }
        case expand_equalizer:
        case expand_compressor:
        case expand_spatializer:
        case expand_widener: toggle_section(section_for(id)); return false;
        case two_pass: set_two_pass(player_, !settings_.two_pass); return true;
        case compressor: set_filter(player_, Filter::compressor, !settings_.compressor); return true;
        case spatializer: set_filter(player_, Filter::spatializer, !settings_.spatializer); return true;
        case widener: set_filter(player_, Filter::widener, !settings_.widener); return true;
        default: return false;
        }
    }

    void command(Control id, int event) {
        if ((id == choices || id == preset_choices) && event == LBN_KILLFOCUS &&
            dropdown_ && control(id) == choice_list(*dropdown_)) {
            close_choices(false);
            return;
        }
        const bool gain = is_gain(id) && id == gain_edit(gain_index(id));
        if (id == audio_track || id == audio_language || id == delay || gain) {
            if (event == EN_SETFOCUS) {
                edited_ = id;
                original_ = window_text(control(id));
                SendMessageW(control(id), EM_SETSEL, 0, -1);
                show_control(control(id));
            } else if (event == EN_KILLFOCUS && edited_ == id) {
                edited_.reset();
                const auto value = window_text(control(id));
                if (id == delay || gain) {
                    if (value != original_) {
                        if (const auto number = parse_number(value, gain ? L" dB" : L" s")) {
                            if (!gain)
                                set_delay(player_, *number);
                            else if (change_gain(gain_index(id), *number))
                                vlc_config::save(player_.vlc);
                        }
                    }
                    refresh();
                } else if (value != original_) {
                    edit_default(id, value);
                }
            }
            return;
        }
        if (event == BN_CLICKED && button(id)) {
            vlc_config::save(player_.vlc);
            refresh();
        }
    }

    void save_slider_settings() {
        if (std::exchange(widener_reload_pending_, false))
            reload_widener(player_);
        if (std::exchange(slider_dirty_, false))
            vlc_config::save(player_.vlc);
    }

    LRESULT message(UINT message, WPARAM wp, LPARAM lp) {
        switch (message) {
        case WM_CREATE: return initialize() ? 0 : -1;
        case WM_SIZE:
            close_choices(false);
            scroll_.move(scroll_.offset(), bounds());
            layout();
            return 0;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: paint(window_, bounds(), [this](HDC dc) { draw(dc); }); return 0;
        case WM_PRINTCLIENT: draw(reinterpret_cast<HDC>(wp)); return 0;
        case WM_TIMER:
            if (IsWindowVisible(window_)) {
                test_.release_finished();
                refresh();
            }
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
        case WM_HSCROLL: {
            const auto slider = reinterpret_cast<HWND>(lp);
            const auto id = GetDlgCtrlID(slider);
            const auto position = SendMessageW(slider, TBM_GETPOS, 0, 0);
            const bool gain = is_expanded(Section::equalizer) && is_gain(id) && id == gain_slider(gain_index(id));
            const bool compressor = is_expanded(Section::compressor) && is_compressor_slider(id);
            const bool spatializer_setting = is_expanded(Section::spatializer) && is_spatializer_slider(id);
            const bool widener_setting = is_expanded(Section::widener) && is_widener_slider(id);
            if (id == delay_slider && IsWindowEnabled(slider))
                set_delay(player_, position / 1000.0);
            if (id == pitch_slider) {
                const float semitones = position / 10.0f;
                if (semitones != settings_.pitch) {
                    set_pitch(player_, semitones);
                    slider_dirty_ = true;
                }
            }
            if (gain)
                slider_dirty_ |= change_gain(gain_index(id), position / double(slider_units));
            if (compressor) {
                const int index = compressor_index(id);
                const float value = position / float(slider_units);
                if (value != settings_.compressor_values[index]) {
                    set_filter_value(player_, compressor_parameters[index], value);
                    slider_dirty_ = true;
                }
            }
            if (spatializer_setting) {
                const int index = spatializer_index(id);
                if (position != spatializer_position(settings_.spatializer_values[index])) {
                    set_filter_value(player_, spatializer_parameters[index], position / float(spatializer_steps_per_unit));
                    slider_dirty_ = true;
                }
            }
            if (widener_setting) {
                const int index = widener_index(id);
                const auto &parameter = widener_parameters[index];
                if (position != widener_position(parameter, settings_.widener_values[index])) {
                    set_filter_value(player_, parameter, position / float(parameter.steps_per_unit));
                    widener_reload_pending_ |= parameter.reload;
                    slider_dirty_ = true;
                }
            }
            if ((gain || compressor || spatializer_setting || widener_setting || id == pitch_slider) && LOWORD(wp) != TB_THUMBTRACK)
                save_slider_settings();
            refresh();
            return 0;
        }
        case WM_DRAWITEM: {
            const auto &item = *reinterpret_cast<const DRAWITEMSTRUCT *>(lp);
            if (item.CtlID == choices || item.CtlID == preset_choices)
                draw_choice(item);
            else
                draw_button(item);
            return TRUE;
        }
        case WM_NOTIFY: {
            const auto note = reinterpret_cast<NMHDR *>(lp);
            const int id = static_cast<int>(note->idFrom);
            if (note->code == NM_RELEASEDCAPTURE && is_widener_slider(id)) {
                save_slider_settings();
                return 0;
            }
            if (note->code == NM_CUSTOMDRAW) {
                auto &drawing = *reinterpret_cast<NMCUSTOMDRAW *>(lp);
                const bool spatializer_slider = is_spatializer_slider(id), widener_slider = is_widener_slider(id);
                const bool dark_theme = &colors_ == &dark;
                if (is_gain(id) || is_compressor_slider(id) || spatializer_slider || widener_slider) {
                    int divisions = 0;
                    if (spatializer_slider) {
                        const auto &parameter = spatializer_parameters[spatializer_index(id)];
                        divisions = spatializer_position(parameter.maximum);
                    } else if (widener_slider && id != first_widener_slider) {
                        const auto &parameter = widener_parameters[widener_index(id)];
                        divisions = widener_position(parameter, parameter.maximum);
                    }
                    const SliderTicks ticks{divisions, dark_theme ? RGB(37, 32, 26) : colors_.edge,
                                            dark_theme ? RGB(76, 52, 22) : RGB(172, 144, 115)};
                    return draw_slider(drawing, detail_colors_, false, dark_theme ? RGB(171, 99, 20) : colors_.accent, ticks);
                }
                return draw_slider(drawing, colors_, true);
            }
            break;
        }
        case WM_CTLCOLORLISTBOX: {
            const auto dc = reinterpret_cast<HDC>(wp);
            const auto background = reinterpret_cast<HWND>(lp) == control(preset_choices)
                ? presets_background() : colors_.button;
            SetBkColor(dc, background);
            SetDCBrushColor(dc, background);
            return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
        }
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORSTATIC: {
            const auto dc = reinterpret_cast<HDC>(wp);
            const auto background = is_gain(GetDlgCtrlID(reinterpret_cast<HWND>(lp))) ? detail_colors_.track : colors_.track;
            SetTextColor(dc, message == WM_CTLCOLOREDIT ? colors_.text : colors_.dim);
            SetBkColor(dc, background);
            SetDCBrushColor(dc, background);
            return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
        }
        case WM_DESTROY: KillTimer(window_, refresh_timer); save_slider_settings(); break;
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
