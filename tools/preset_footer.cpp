#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define UNICODE
#include "preset_footer.hpp"
#include "preferences_presets.hpp"
#include "preferences_widgets.hpp"
#include "preset_footer_icons.hpp"
#include "vlc_config.hpp"

#include <iomanip>
#include <optional>
#include <sstream>
#include <utility>
#include <vector>

namespace preset_footer {
namespace {

using namespace preferences_widgets;

constexpr wchar_t footer_class[] = L"VlcRedesignPresetFooter", menu_class[] = L"VlcRedesignPresetMenu";
constexpr wchar_t default_label[] = L"Default Preset", new_label[] = L"New Preset";
constexpr const wchar_t *action_labels[] = {L"Rename", L"Delete"};
constexpr int inset = 16, button_top = 21, button_height = 44;
constexpr int option_height = 40, max_visible_options = 6, max_name_length = 80;
constexpr int action_column = 50, action_width = 148, action_height = 36;
enum Control : INT_PTR { preset = 1, advanced, name, save_name };
enum Action : LRESULT { rename_action, delete_action };

bool contains(HWND parent, HWND target) {
    return parent && target && (target == parent || IsChild(parent, target));
}

std::string catalog(const std::vector<std::wstring> &names, std::size_t selected) {
    std::ostringstream data;
    data << selected;
    for (const auto &label : names)
        data << ' ' << std::quoted(utf8(label.c_str()));
    return data.str();
}

LRESULT clicked_row(HWND list, LPARAM point) {
    const auto hit = SendMessageW(list, LB_ITEMFROMPOINT, 0, point);
    return HIWORD(hit) == 0 ? LOWORD(hit) : LB_ERR;
}

class Footer {
public:
    Footer(HINSTANCE module, const Colors &colors, VlcPlayer player, LONG content_top, void (*refresh_page)())
        : module_(module), colors_(colors), player_(player), content_top_(content_top), refresh_page_(refresh_page) {
        icons_.load(module, preset_footer_icons::resource, preset_footer_icons::size, preset_footer_icons::count);
        check_.load(module);
        load_names();
    }

    ~Footer() {
        DeleteObject(font_);
        DeleteObject(small_);
    }

    static Footer *get(HWND window) {
        return window ? reinterpret_cast<Footer *>(GetWindowLongPtrW(window, GWLP_USERDATA)) : nullptr;
    }

    static HWND create(HWND parent, RECT area, LONG content_top, VlcPlayer player, bool light_theme, void (*refresh_page)()) {
        HINSTANCE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(&procedure), &module) || !register_class(module, footer_class))
            return nullptr;
        auto footer = std::make_unique<Footer>(module, light_theme ? light : dark, player, content_top, refresh_page);
        const auto window = footer->create_panel(parent, footer_class, area);
        if (!window)
            return nullptr;
        footer->window_ = window;
        auto *view = footer.release();
        if (!view->initialize(area.right - area.left)) {
            DestroyWindow(window);
            return nullptr;
        }
        return window;
    }

    void close() {
        close_menu(false);
        close_editor(false);
    }

    bool owns(HWND target) const {
        return contains(window_, target) || (menu_open_ && contains(menu_, target));
    }

    bool input(const MSG &message) {
        if (!IsWindowVisible(window_))
            return false;
        const auto target = message.hwnd;
        const UINT event = message.message;
        if (event == WM_KEYDOWN && message.wParam == VK_ESCAPE && (menu_open_ || edited_row_)) {
            if (action_row_)
                close_actions(true);
            else {
                close_menu(true);
                close_editor(true);
            }
            return true;
        }
        if (menu_open_ && menu_input(message))
            return true;
        if (edited_row_ && event == WM_KEYDOWN && contains(window_, target)) {
            if (message.wParam == VK_RETURN) {
                save_editor();
                return true;
            }
            if (message.wParam == VK_TAB) {
                SetFocus(control(target == control(name) ? save_name : name));
                return true;
            }
        }
        if (edited_row_ && !contains(window_, target) && mouse_pressed(event))
            close_editor(false);
        if (target == control(preset) && event == WM_KEYDOWN && message.wParam == VK_RETURN) {
            open_menu();
            return true;
        }
        return false;
    }

private:
    HINSTANCE module_;
    const Colors &colors_;
    VlcPlayer player_;
    LONG content_top_;
    void (*refresh_page_)();
    HWND window_ = nullptr, menu_ = nullptr, list_ = nullptr, actions_ = nullptr;
    HFONT font_ = make_font(16), small_ = make_font(12);
    IconStrip icons_;
    DropdownCheck check_;
    std::vector<std::wstring> names_;
    std::size_t active_ = 0, action_row_ = 0;
    std::optional<std::size_t> edited_row_;
    int visible_rows_ = 0, wheel_delta_ = 0, editor_top_ = 0;
    bool menu_open_ = false;
    const wchar_t *notice_ = nullptr;

    HWND control(Control id) const { return GetDlgItem(window_, static_cast<int>(id)); }

    static bool mouse_pressed(UINT message) {
        return message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK ||
               message == WM_RBUTTONDOWN || message == WM_MBUTTONDOWN;
    }

    const wchar_t *caption() const { return active_ ? names_[active_ - 1].c_str() : default_label; }
    std::size_t active_row() const { return active_ ? active_ : names_.size() + 1; }
    bool is_custom(LRESULT row) const { return row > 0 && static_cast<std::size_t>(row) <= names_.size(); }
    int last_top_row() const { return std::max(0, static_cast<int>(names_.size()) + 2 - visible_rows_); }

    void load_names() {
        std::istringstream data(vlc_config::take_string(config_GetPsz(player_.vlc, preferences_presets::names_key)));
        std::size_t selected = 0;
        if (!(data >> selected))
            return;
        std::string label;
        while (data >> std::quoted(label))
            names_.push_back(wstring(label));
        if (selected <= names_.size())
            active_ = selected;
    }

    bool select_preset(std::size_t selected) {
        const auto previous = active_ ? utf8(names_[active_ - 1].c_str()) : std::string{};
        const auto next = selected ? utf8(names_[selected - 1].c_str()) : std::string{};
        if (!preferences_presets::select(player_, previous, next, catalog(names_, selected)))
            return false;
        active_ = selected;
        SetWindowTextW(control(preset), caption());
        refresh_page_();
        return true;
    }

    static bool register_class(HINSTANCE module, const wchar_t *name) {
        WNDCLASSW cls{};
        cls.hInstance = module;
        cls.lpfnWndProc = procedure;
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        cls.lpszClassName = name;
        return RegisterClassW(&cls) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }

    HWND create_panel(HWND parent, const wchar_t *name, RECT area) {
        const auto window = CreateWindowExW(WS_EX_COMPOSITED, name, nullptr,
                                            WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                                            area.left, area.top, area.right - area.left, area.bottom - area.top,
                                            parent, nullptr, module_, nullptr);
        if (window)
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        return window;
    }

    HWND create_control(Control id, const wchar_t *type, const wchar_t *label, DWORD style, RECT area) const {
        const auto child = CreateWindowExW(0, type, label, WS_CHILD | WS_CLIPSIBLINGS | style,
                                           area.left, area.top, area.right - area.left, area.bottom - area.top,
                                           window_, reinterpret_cast<HMENU>(id), module_, nullptr);
        if (child)
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font_), FALSE);
        return child;
    }

    bool initialize(LONG width) const {
        const LONG right = width - inset;
        constexpr int button_bottom = button_top + button_height;
        if (!create_control(preset, L"BUTTON", caption(), WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                            {inset, button_top, right, button_bottom}) ||
            !create_control(advanced, L"BUTTON", L"Open Advanced Settings", WS_VISIBLE | WS_DISABLED | BS_OWNERDRAW,
                            {inset, 77, right, 109}) ||
            !create_control(name, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL,
                            {inset + 12, button_top + 11, right - 102, button_bottom - 9}) ||
            !create_control(save_name, L"BUTTON", L"Create", WS_TABSTOP | BS_OWNERDRAW,
                            {right - 82, button_top, right, button_bottom}))
            return false;
        const auto edit = control(name);
        SendMessageW(edit, EM_SETLIMITTEXT, max_name_length, 0);
        SendMessageW(edit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Preset name"));
        return true;
    }

    bool create_menu() {
        if (!register_class(module_, menu_class))
            return false;
        menu_ = create_panel(GetParent(window_), menu_class, {});
        if (!menu_)
            return false;
        list_ = make_list(WS_VISIBLE);
        if (list_)
            return true;
        DestroyWindow(menu_);
        return false;
    }

    HWND make_list(DWORD style) const {
        const auto list = CreateWindowExW(0, L"LISTBOX", nullptr, WS_CHILD | WS_CLIPSIBLINGS | style |
                                          LBS_HASSTRINGS | LBS_OWNERDRAWFIXED | LBS_NOINTEGRALHEIGHT | LBS_NOTIFY,
                                          0, 0, 0, 0, menu_, nullptr, module_, nullptr);
        if (list)
            SendMessageW(list, WM_SETFONT, reinterpret_cast<WPARAM>(font_), FALSE);
        return list;
    }

    void open_menu(std::optional<int> top_row = 0) {
        if (!menu_ && !create_menu())
            return;
        RECT field{};
        const auto button = control(preset);
        GetWindowRect(button, &field);
        MapWindowPoints(nullptr, GetParent(window_), reinterpret_cast<POINT *>(&field), 2);
        const int room = field.bottom - content_top_;
        const int row = std::clamp(room / 2, 24, option_height);
        visible_rows_ = static_cast<int>(std::min<std::size_t>(names_.size() + 2,
                                           std::clamp(room / row, 2, max_visible_options)));
        const LONG height = visible_rows_ * row, width = field.right - field.left;
        wheel_delta_ = 0;
        notice_ = nullptr;
        SendMessageW(list_, WM_SETREDRAW, FALSE, 0);
        SendMessageW(list_, LB_RESETCONTENT, 0, 0);
        SendMessageW(list_, LB_SETITEMHEIGHT, 0, row);
        SendMessageW(list_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(new_label));
        for (const auto &label : names_)
            SendMessageW(list_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        SendMessageW(list_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(default_label));
        SetWindowPos(list_, nullptr, 0, 0, width, height, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW);
        SendMessageW(list_, LB_SETCURSEL, active_row(), 0);
        if (top_row)
            SendMessageW(list_, LB_SETTOPINDEX, std::clamp(*top_row, 0, last_top_row()), 0);
        // When the open menu shrinks, repaint the preferences controls it uncovers.
        const UINT repaint = menu_open_ ? SWP_NOCOPYBITS : SWP_NOREDRAW;
        SetWindowPos(menu_, HWND_TOP, field.left, field.bottom - height, width, height, SWP_NOACTIVATE | repaint);
        round_dropdown(menu_, width, height);
        SendMessageW(list_, WM_SETREDRAW, TRUE, 0);
        menu_open_ = true;
        ShowWindow(menu_, SW_SHOWNOACTIVATE);
        RedrawWindow(menu_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_NOERASE);
        SetFocus(list_);
        if (menu_open_)
            ShowWindow(button, SW_HIDE);
        InvalidateRect(window_, nullptr, FALSE);
    }

    void close_menu(bool focus) {
        if (!std::exchange(menu_open_, false))
            return;
        close_actions(false);
        const auto button = control(preset);
        ShowWindow(button, SW_SHOWNOACTIVATE);
        if (GetCapture() == list_)
            ReleaseCapture();
        ShowWindow(menu_, SW_HIDE);
        if (focus)
            SetFocus(button);
    }

    void open_actions(std::size_t row) {
        if (GetCapture() == list_)
            ReleaseCapture();
        RECT item{}, viewport{};
        if (SendMessageW(list_, LB_GETITEMRECT, row, reinterpret_cast<LPARAM>(&item)) == LB_ERR ||
            !GetClientRect(menu_, &viewport))
            return;
        const LONG width = std::min<LONG>(action_width, viewport.right - action_column);
        const LONG row_height = std::min<LONG>(action_height, viewport.bottom / 2);
        if (width <= 0 || row_height <= 0)
            return;
        if (!actions_) {
            actions_ = make_list(0);
            if (!actions_)
                return;
            for (const auto label : action_labels)
                SendMessageW(actions_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
        }
        action_row_ = row;
        const LONG height = row_height * 2;
        const LONG top = std::clamp(item.top, 0L, viewport.bottom - height);
        SendMessageW(actions_, LB_SETITEMHEIGHT, 0, row_height);
        SetWindowPos(actions_, HWND_TOP, action_column, top, width, height, SWP_NOACTIVATE | SWP_NOREDRAW);
        round_dropdown(actions_, width, height);
        SendMessageW(actions_, LB_SETCURSEL, rename_action, 0);
        ShowWindow(actions_, SW_SHOWNOACTIVATE);
        InvalidateRect(actions_, nullptr, FALSE);
        SetFocus(actions_);
    }

    void close_actions(bool focus) {
        if (!std::exchange(action_row_, 0))
            return;
        if (GetCapture() == actions_)
            ReleaseCapture();
        if (focus)
            SetFocus(list_);
        ShowWindow(actions_, SW_HIDE);
        if (menu_open_)
            RedrawWindow(menu_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_NOERASE);
    }

    void open_editor(std::size_t row) {
        edited_row_ = row;
        editor_top_ = SendMessageW(list_, LB_GETTOPINDEX, 0, 0);
        const auto edit = control(name);
        SetWindowTextW(edit, row ? names_[row - 1].c_str() : L"");
        SetWindowTextW(control(save_name), row ? L"Save" : L"Create");
        ShowWindow(edit, SW_SHOWNOACTIVATE);
        ShowWindow(control(save_name), SW_SHOWNOACTIVATE);
        SetFocus(edit);
        SendMessageW(edit, EM_SETSEL, 0, -1);
        ShowWindow(control(preset), SW_HIDE);
        InvalidateRect(window_, nullptr, FALSE);
    }

    void close_editor(bool focus) {
        if (!std::exchange(edited_row_, std::nullopt))
            return;
        notice_ = nullptr;
        const auto button = control(preset);
        ShowWindow(button, SW_SHOWNOACTIVATE);
        ShowWindow(control(name), SW_HIDE);
        ShowWindow(control(save_name), SW_HIDE);
        if (focus)
            SetFocus(button);
        InvalidateRect(window_, nullptr, FALSE);
    }

    void show_notice(const wchar_t *message) {
        notice_ = message;
        InvalidateRect(window_, nullptr, FALSE);
    }

    void save_editor() {
        const auto row = *edited_row_;
        const auto value = window_text(control(name));
        const std::wstring label(trim(value));
        if (label.empty() || label.find_first_of(L"\r\n") != std::wstring::npos) {
            show_notice(L"Enter a preset name.");
            return;
        }
        const auto same = [&](const std::wstring &other) {
            return CompareStringOrdinal(label.c_str(), -1, other.c_str(), -1, TRUE) == CSTR_EQUAL;
        };
        const auto original = row ? &names_[row - 1] : nullptr;
        if (same(default_label) || same(new_label) || std::any_of(names_.begin(), names_.end(),
                [&](const auto &other) { return &other != original && same(other); })) {
            show_notice(L"That name is already in use.");
            return;
        }
        if (row) {
            if (*original != label) {
                auto updated = names_;
                updated[row - 1] = label;
                if (!preferences_presets::rename(player_, utf8(original->c_str()), utf8(label.c_str()),
                                                 catalog(updated, active_))) {
                    show_notice(L"VLC could not rename the preset.");
                    return;
                }
                names_ = std::move(updated);
                SetWindowTextW(control(preset), caption());
            }
        } else {
            names_.push_back(label);
            if (!select_preset(names_.size())) {
                names_.pop_back();
                show_notice(L"VLC could not save the preset.");
                return;
            }
        }
        close_editor(false);
        open_menu(row ? std::optional<int>{editor_top_} : std::nullopt);
    }

    void delete_preset(std::size_t row) {
        const auto top = SendMessageW(list_, LB_GETTOPINDEX, 0, 0);
        auto updated = names_;
        updated.erase(updated.begin() + (row - 1));
        const bool selected = row == active_;
        const auto next = selected ? 0 : active_ - (row < active_);
        if (!preferences_presets::remove(player_, utf8(names_[row - 1].c_str()), catalog(updated, next))) {
            close_menu(true);
            show_notice(L"VLC could not delete the preset.");
            return;
        }
        names_ = std::move(updated);
        active_ = next;
        SetWindowTextW(control(preset), caption());
        if (selected)
            refresh_page_();
        close_actions(true);
        open_menu(static_cast<int>(top));
    }

    void choose_action(LRESULT action) {
        const auto row = action_row_;
        if (action == rename_action) {
            close_menu(false);
            open_editor(row);
        } else if (action == delete_action) {
            delete_preset(row);
        }
    }

    void choose(LRESULT row) {
        if (row < 0 || static_cast<std::size_t>(row) > names_.size() + 1)
            return;
        close_menu(row != 0);
        if (row == 0) {
            open_editor(0);
            return;
        }
        const auto selected = static_cast<std::size_t>(row) <= names_.size() ? static_cast<std::size_t>(row) : 0;
        if ((!selected || selected != active_) && !select_preset(selected))
            show_notice(L"VLC could not load the preset.");
    }

    bool menu_input(const MSG &message) {
        if (action_row_) {
            if (message.hwnd == actions_) {
                if (message.message == WM_LBUTTONUP) {
                    if (const auto action = clicked_row(actions_, message.lParam); action != LB_ERR) {
                        choose_action(action);
                        return true;
                    }
                }
                if (message.message == WM_KEYDOWN) {
                    if (message.wParam == VK_RETURN || message.wParam == VK_SPACE) {
                        choose_action(SendMessageW(actions_, LB_GETCURSEL, 0, 0));
                        return true;
                    }
                    if (message.wParam == VK_TAB) {
                        close_actions(true);
                        return true;
                    }
                }
            }
            if (message.message == WM_MOUSEWHEEL || (message.hwnd != actions_ && mouse_pressed(message.message)))
                close_actions(contains(menu_, message.hwnd) || message.message == WM_MOUSEWHEEL);
        }
        if (message.hwnd == list_) {
            if (message.message == WM_LBUTTONUP) {
                const auto row = clicked_row(list_, message.lParam);
                if (row != LB_ERR) {
                    if (is_custom(row) && GET_X_LPARAM(message.lParam) < action_column)
                        open_actions(static_cast<std::size_t>(row));
                    else
                        choose(row);
                    return true;
                }
            } else if (message.message == WM_KEYDOWN) {
                if (message.wParam == VK_RETURN || message.wParam == VK_SPACE) {
                    choose(SendMessageW(list_, LB_GETCURSEL, 0, 0));
                    return true;
                }
                if (message.wParam == VK_TAB) {
                    close_menu(true);
                    return true;
                }
                if (message.wParam == VK_APPS || (message.wParam == VK_F10 && (GetKeyState(VK_SHIFT) & 0x8000))) {
                    const auto row = SendMessageW(list_, LB_GETCURSEL, 0, 0);
                    if (is_custom(row))
                        open_actions(static_cast<std::size_t>(row));
                    return true;
                }
            }
        }
        if (!contains(menu_, message.hwnd) && mouse_pressed(message.message))
            close_menu(false);
        if (message.message != WM_MOUSEWHEEL)
            return false;
        RECT area{};
        GetWindowRect(menu_, &area);
        if (!PtInRect(&area, {GET_X_LPARAM(message.lParam), GET_Y_LPARAM(message.lParam)})) {
            close_menu(false);
            return false;
        }
        wheel_delta_ += GET_WHEEL_DELTA_WPARAM(message.wParam);
        const int top = SendMessageW(list_, LB_GETTOPINDEX, 0, 0);
        const int next = std::clamp(top - 2 * (wheel_delta_ / WHEEL_DELTA), 0, last_top_row());
        wheel_delta_ %= WHEEL_DELTA;
        if (next != top)
            SendMessageW(list_, LB_SETTOPINDEX, next, 0);
        return true;
    }

    void draw_background(HWND window, HDC dc, RECT area) const {
        if (window == menu_) {
            fill(dc, area, dropdown_background(colors_));
            return;
        }
        fill(dc, area, colors_.panel);
        fill(dc, {area.left, area.top, area.right, area.top + 1}, colors_.edge);
        if (edited_row_)
            round_rect(dc, {inset, button_top, area.right - inset - 90, button_top + button_height}, 6, colors_.track);
        if (notice_)
            text(dc, notice_, {inset, 65, area.right - inset, 79}, small_, colors_.text);
    }

    void draw_row(const DRAWITEMSTRUCT &item) const {
        const auto area = item.rcItem;
        const auto dc = item.hDC;
        const auto row = item.itemID;
        const bool current = row == active_row();
        draw_dropdown_row(dc, area, colors_, current || (item.itemState & ODS_SELECTED));
        if (row > names_.size() + 1)
            return;
        const bool is_default = row == names_.size() + 1;
        const auto label = is_default ? default_label : row == 0 ? new_label : names_[row - 1].c_str();
        if (!is_default)
            icons_.draw(dc, area.left + 15, (area.top + area.bottom - preset_footer_icons::size) / 2,
                        row == 0 ? preset_footer_icons::add : preset_footer_icons::more);
        if (current)
            check_.draw(dc, area);
        text(dc, label, {area.left + (is_default ? 16 : action_column), area.top, area.right - 45, area.bottom},
             font_, row == 0 ? colors_.dim : colors_.text);
    }

    void draw_action(const DRAWITEMSTRUCT &item) const {
        if (item.itemID >= std::size(action_labels))
            return;
        const auto area = item.rcItem;
        const auto dc = item.hDC;
        draw_dropdown_row(dc, area, colors_, (item.itemState & ODS_SELECTED) != 0);
        text(dc, action_labels[item.itemID], {area.left + 14, area.top, area.right - 10, area.bottom}, font_, colors_.text);
        RECT viewport{};
        GetClientRect(actions_, &viewport);
        SetDCBrushColor(dc, colors_.edge);
        FrameRect(dc, &viewport, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    }

    void draw_button(const DRAWITEMSTRUCT &item) const {
        const auto area = item.rcItem;
        const auto dc = item.hDC;
        const auto label = window_text(item.hwndItem);
        const bool pressed = (item.itemState & ODS_SELECTED) != 0;
        constexpr int size = preset_footer_icons::size;
        const LONG center = (area.top + area.bottom) / 2;
        fill(dc, area, colors_.panel);
        if (item.CtlID == preset) {
            round_rect(dc, area, 6, pressed ? colors_.hover : colors_.track);
            icons_.draw(dc, area.right - 29 - size / 2, center - size / 2, preset_footer_icons::chevron);
            text(dc, label.c_str(), {area.left + 17, area.top, area.right - 48, area.bottom}, font_, colors_.text);
        } else if (item.CtlID == save_name) {
            round_rect(dc, area, 6, pressed ? colors_.hover : colors_.button);
            text(dc, label.c_str(), area, font_, colors_.text, DT_CENTER);
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

    void command(HWND window, WORD id, WORD event, HWND source) {
        if (window == menu_) {
            if (event == LBN_KILLFOCUS && (action_row_ ? source == actions_ : source == list_))
                close_menu(false);
        } else if (id == name && event == EN_CHANGE) {
            show_notice(nullptr);
        } else if (event == BN_CLICKED) {
            if (id == preset)
                open_menu();
            else if (id == save_name && edited_row_)
                save_editor();
        }
    }

    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wp, LPARAM lp) {
        const auto self = get(window);
        if (!self)
            return DefWindowProcW(window, message, wp, lp);
        switch (message) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT:
        case WM_PRINTCLIENT: {
            RECT area{};
            GetClientRect(window, &area);
            if (message == WM_PAINT)
                paint(window, area, [&](HDC dc) { self->draw_background(window, dc, area); });
            else
                self->draw_background(window, reinterpret_cast<HDC>(wp), area);
            return 0;
        }
        case WM_DRAWITEM: {
            const auto &item = *reinterpret_cast<const DRAWITEMSTRUCT *>(lp);
            if (item.hwndItem == self->actions_)
                self->draw_action(item);
            else if (window == self->menu_)
                self->draw_row(item);
            else
                self->draw_button(item);
            return TRUE;
        }
        case WM_COMMAND:
            self->command(window, LOWORD(wp), HIWORD(wp), reinterpret_cast<HWND>(lp));
            return 0;
        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLOREDIT: {
            const auto dc = reinterpret_cast<HDC>(wp);
            const auto background = message == WM_CTLCOLORLISTBOX ? dropdown_background(self->colors_) : self->colors_.track;
            SetTextColor(dc, self->colors_.text);
            SetBkColor(dc, background);
            SetDCBrushColor(dc, background);
            return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
        }
        case WM_NCDESTROY:
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            self->menu_open_ = false;
            if (window == self->menu_) {
                self->menu_ = nullptr;
                self->list_ = nullptr;
                self->actions_ = nullptr;
                self->action_row_ = 0;
            } else {
                if (self->menu_)
                    DestroyWindow(self->menu_);
                delete self;
            }
            break;
        }
        return DefWindowProcW(window, message, wp, lp);
    }
};

}

HWND create(HWND parent, RECT area, LONG content_top, VlcPlayer player, bool light_theme, void (*refresh_page)()) {
    return Footer::create(parent, area, content_top, player, light_theme, refresh_page);
}

void close(HWND footer) {
    if (const auto view = Footer::get(footer))
        view->close();
}

bool handle_input(HWND footer, const MSG &message) {
    const auto view = Footer::get(footer);
    return view && view->input(message);
}

bool owns_input_target(HWND footer, HWND target) {
    const auto view = Footer::get(footer);
    return view && view->owns(target);
}

}
