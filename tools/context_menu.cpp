#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define UNICODE
#include "context_menu.hpp"

#include <objidl.h>
#include <gdiplus.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

extern "C" {
__declspec(dllimport) std::size_t vlc_actions_get_keycodes(void *, const char *, bool, std::uint32_t **);
__declspec(dllimport) const char *const *vlc_actions_get_key_names(void *);
__declspec(dllimport) std::uint32_t vlc_str2keycode(const char *);
__declspec(dllimport) char *vlc_keycode2str(std::uint32_t, bool);
__declspec(dllimport) void libvlc_free(void *);
}

namespace context_menu {
namespace {
namespace g = Gdiplus;
constexpr wchar_t window_class[] = L"VlcRedesignContextMenu";
constexpr int menu_width = 292, row_height = 48, submenu_row_height = 36;
constexpr int padding = 4, radius = 10;
constexpr UINT_PTR hover_timer = 1, focus_timer = 2;
std::atomic<bool> open{false};
const g::Color background(255, 23, 23, 23), foreground(255, 159, 159, 159);
const g::Color hint(255, 133, 133, 133), disabled(255, 99, 99, 99);
const g::Color hover_color(255, 44, 44, 44), separator(255, 32, 32, 32);

enum class Group { none, playback, video, audio, subtitle, settings, playlist, media };

struct Item {
    const wchar_t *label = nullptr;
    const char *action = nullptr;
    SkinCommand command = SkinCommand::none;
    Group group = Group::none;
    Icon icon = Icon::none;
    bool enabled = true;
    const wchar_t *shortcut = nullptr;
};

struct Row {
    Item item;
    int top;
    int height;
    std::wstring shortcut;
};

struct Menu;
struct Pane {
    explicit Pane(Menu *owner) : menu(owner) {}

    Menu *menu;
    HWND window = nullptr;
    std::vector<Row> rows;
    RECT bounds{};
    int height = 0;
    int selected = -1;
};

std::wstring shortcut_label(void *vlc, const char *action) {
    std::uint32_t *keys = nullptr;
    const auto count = vlc_actions_get_keycodes(vlc, action, false, &keys);
    std::unique_ptr<std::uint32_t, decltype(&libvlc_free)> binding(keys, libvlc_free);
    if (!count || !keys[0])
        return {};
    std::unique_ptr<char, decltype(&libvlc_free)> label(vlc_keycode2str(keys[0], false), libvlc_free);
    if (!label)
        return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, label.get(), -1, nullptr, 0);
    if (size < 2)
        return {};
    std::wstring text(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, label.get(), -1, text.data(), size);
    text.pop_back();

    std::wstring result;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const wchar_t character = text[i];
        if (character == L'+' && i > 0 && i + 1 < text.size())
            result += L" + ";
        else
            result += character;
    }
    if (!result.empty() && result.back() >= L'a' && result.back() <= L'z' &&
        (result.size() == 1 || result[result.size() - 2] == L' '))
        result.back() -= L'a' - L'A';
    return result;
}

class Artwork {
public:
    bool load(HINSTANCE module) {
        g::GdiplusStartupInput input;
        if (g::GdiplusStartup(&startup_, &input, nullptr) != g::Ok)
            return false;

        DWORD bytes = 0;
        auto *pixels = resource(module, 101, bytes);
        constexpr int stride = icon_count * icon_size * 4;
        if (pixels && bytes == stride * icon_size)
            icons_ = std::make_unique<g::Bitmap>(icon_count * icon_size, icon_size, stride,
                                                PixelFormat32bppPARGB, pixels);
        const auto *font = resource(module, 102, bytes);
        fonts_ = std::make_unique<g::PrivateFontCollection>();
        if (font && fonts_->AddMemoryFont(font, static_cast<INT>(bytes)) == g::Ok) {
            INT count = 0;
            family_ = std::make_unique<g::FontFamily>();
            fonts_->GetFamilies(1, family_.get(), &count);
        }
        return icons_ && icons_->GetLastStatus() == g::Ok;
    }

    ~Artwork() {
        icons_.reset();
        family_.reset();
        fonts_.reset();
        if (startup_)
            g::GdiplusShutdown(startup_);
    }

    void icon(g::Graphics &graphics, Icon icon, float x, float y) const {
        if (icon == Icon::none)
            return;
        const g::RectF target(x, y, icon_size, icon_size);
        graphics.DrawImage(icons_.get(), target, static_cast<int>(icon) * icon_size, 0,
                           icon_size, icon_size, g::UnitPixel);
    }

    const g::FontFamily *family() const {
        return family_ && family_->IsAvailable() ? family_.get() : g::FontFamily::GenericSansSerif();
    }

private:
    static BYTE *resource(HINSTANCE module, WORD id, DWORD &size) {
        const HRSRC entry = FindResourceW(module, MAKEINTRESOURCEW(id), RT_RCDATA);
        size = entry ? SizeofResource(module, entry) : 0;
        return entry ? static_cast<BYTE *>(LockResource(LoadResource(module, entry))) : nullptr;
    }

    ULONG_PTR startup_ = 0;
    std::unique_ptr<g::PrivateFontCollection> fonts_;
    std::unique_ptr<g::FontFamily> family_;
    std::unique_ptr<g::Bitmap> icons_;
};

// The interface worker owns the GDI+ session. A draw can finish while it is stopping.
std::shared_ptr<const Artwork> artwork;

void rounded_rectangle(g::Graphics &graphics, const g::Color &color, float x, float y,
                       float width, float height, float corner) {
    const float diameter = 2 * corner;
    g::GraphicsPath path;
    path.AddArc(x, y, diameter, diameter, 180, 90);
    path.AddArc(x + width - diameter, y, diameter, diameter, 270, 90);
    path.AddArc(x + width - diameter, y + height - diameter, diameter, diameter, 0, 90);
    path.AddArc(x, y + height - diameter, diameter, diameter, 90, 90);
    path.CloseFigure();
    g::SolidBrush brush(color);
    graphics.FillPath(&brush, &path);
}

struct Menu {
    Pane root{this};
    Pane submenu{this};
    Player player{};
    HINSTANCE module = nullptr;
    RECT work{};
    float scale = 1;
    Group opened_group = Group::none;
    bool was_foreground = false;

    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
        auto *pane = reinterpret_cast<Pane *>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            pane = static_cast<Pane *>(reinterpret_cast<const CREATESTRUCTW *>(lp)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pane));
        }
        if (!pane)
            return DefWindowProcW(window, message, wp, lp);
        auto &menu = *pane->menu;
        switch (message) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_RBUTTONUP: {
            POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ClientToScreen(window, &point);
            menu.mouse(message, point);
            return 0;
        }
        case WM_CANCELMODE:
        case WM_CLOSE:
            menu.dismiss();
            return 0;
        case WM_CAPTURECHANGED:
            if (window == menu.root.window && reinterpret_cast<HWND>(lp) != window)
                menu.dismiss();
            return 0;
        case WM_TIMER:
            if (wp == focus_timer) {
                DWORD process = 0;
                GetWindowThreadProcessId(GetForegroundWindow(), &process);
                const bool foreground = process == GetCurrentProcessId();
                if (!std::atomic_load(&artwork) || !IsWindowVisible(menu.player.surface) || IsIconic(menu.player.surface) ||
                    (menu.was_foreground && !foreground))
                    menu.dismiss();
                menu.was_foreground = foreground;
            } else if (wp == hover_timer) {
                KillTimer(window, hover_timer);
                menu.open_submenu(false);
            }
            return 0;
        case WM_NCDESTROY:
            if (pane->window == window) {
                pane->window = nullptr;
                if (pane == &menu.root)
                    open.store(false);
            }
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            break;
        }
        return DefWindowProcW(window, message, wp, lp);
    }

    bool prepare() {
        if (!std::atomic_load(&artwork))
            return false;
        if (module)
            return true;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(&window_proc), &module))
            return false;
        WNDCLASSW cls{};
        cls.lpfnWndProc = window_proc;
        cls.hInstance = module;
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        cls.lpszClassName = window_class;
        return RegisterClassW(&cls) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }

    void fill(Pane &pane, std::initializer_list<Item> items) const {
        pane.rows.clear();
        int top = padding;
        for (const Item &item : items) {
            const int height = !item.label ? 1 : &pane == &root ? row_height : submenu_row_height;
            std::wstring shortcut;
            if (item.shortcut)
                shortcut = item.shortcut;
            else if (item.action)
                shortcut = shortcut_label(player.vlc, item.action);
            pane.rows.push_back({item, top, height, std::move(shortcut)});
            top += height;
        }
        pane.height = top + padding;
        pane.selected = -1;
    }

    void build_root() {
        using I = Icon;
        using G = Group;
        fill(root, {
            {player.playing ? L"Pause" : L"Play", "play-pause", SkinCommand::none, G::none,
             player.playing ? I::pause : I::play},
            {L"Stop", "stop", SkinCommand::none, G::none, I::stop},
            {L"Previous Item", "prev", SkinCommand::none, G::none, I::previous},
            {L"Next Item", "next", SkinCommand::none, G::none, I::next},
            {player.recording ? L"Stop Recording" : L"Record", "record", SkinCommand::none, G::none, I::record, player.has_input},
            {},
            {player.ui_hidden ? L"Show UI" : L"Hide UI", nullptr, SkinCommand::toggle_ui, G::none,
             I::hide_ui, true, hide_ui_shortcut_available(player.vlc) ? L"Ctrl + \\" : nullptr},
            {},
            {L"Playback", nullptr, SkinCommand::none, G::playback, I::playback},
            {L"Video", nullptr, SkinCommand::none, G::video, I::video},
            {L"Audio", nullptr, SkinCommand::none, G::audio, I::audio},
            {L"Subtitle", nullptr, SkinCommand::none, G::subtitle, I::subtitle},
            {},
            {L"More Settings", nullptr, SkinCommand::none, G::settings, I::settings},
            {L"Playlist", nullptr, SkinCommand::none, G::playlist, I::playlist},
            {L"Open Media", nullptr, SkinCommand::none, G::media, I::open_media},
            {L"Quit VLC", "quit", SkinCommand::none, G::none, I::quit},
        });
    }

    void build_submenu(Group group) {
        using S = SkinCommand;
        switch (group) {
        case Group::playback:
            fill(submenu, {{L"Faster", "rate-faster-fine"}, {L"Slower", "rate-slower-fine"},
                           {L"Normal Speed", "rate-normal"}, {}, {L"Next Frame", "frame-next"}});
            break;
        case Group::video:
            fill(submenu, {{L"Fullscreen", "toggle-fullscreen"}, {L"Take Snapshot", "snapshot"},
                           {L"Cycle Aspect Ratio", "aspect-ratio"}, {L"Cycle Crop", "crop"}, {},
                           {L"Video Tracks...", nullptr, S::video_tracks}});
            break;
        case Group::audio:
            fill(submenu, {{L"Mute / Unmute", "vol-mute"}, {L"Volume Up", "vol-up"},
                           {L"Volume Down", "vol-down"}, {}, {L"Audio Tracks and Devices...", nullptr, S::audio_tracks}});
            break;
        case Group::subtitle:
            fill(submenu, {{L"Subtitle Tracks...", nullptr, S::subtitles}, {L"Cycle Subtitle Track", "subtitle-track"},
                           {L"Toggle Subtitles", "subtitle-toggle"}, {}, {L"Increase Subtitle Delay", "subdelay-up"},
                           {L"Decrease Subtitle Delay", "subdelay-down"}});
            break;
        case Group::settings:
            fill(submenu, {{L"Preferences...", nullptr, S::settings}, {L"Media Information", nullptr, S::media_info},
                           {L"Messages", nullptr, S::messages}, {L"Always on Top", nullptr, S::on_top}, {},
                           {L"Change Skin...", nullptr, S::change_skin}, {L"VLC Menu...", nullptr, S::vlc_menu}});
            break;
        case Group::playlist:
            fill(submenu, {{L"Show / Hide Playlist", nullptr, S::toggle_playlist},
                           {L"Shuffle", "random"}, {L"Repeat", "loop"}, {},
                           {L"Load Playlist...", nullptr, S::load_playlist},
                           {L"Save Playlist...", nullptr, S::save_playlist}});
            break;
        case Group::media:
            fill(submenu, {{L"Open File(s)...", nullptr, S::open_files}, {L"Open Folder...", nullptr, S::open_folder},
                           {L"Open Disc...", nullptr, S::open_disc}, {L"Open Network Stream...", nullptr, S::open_network},
                           {}, {L"Convert / Stream...", nullptr, S::convert}});
            break;
        case Group::none:
            break;
        }
    }

    int pixels(int value) const {
        return static_cast<int>(std::lround(value * scale));
    }

    void draw_rows(g::Graphics &graphics, const Pane &pane, const Artwork &art) const {
        const g::Font font(art.family(), 14, g::FontStyleRegular, g::UnitPixel);
        g::StringFormat text(g::StringFormat::GenericTypographic());
        text.SetLineAlignment(g::StringAlignmentCenter);
        text.SetTrimming(g::StringTrimmingEllipsisCharacter);
        text.SetFormatFlags(g::StringFormatFlagsNoWrap);
        for (std::size_t index = 0; index < pane.rows.size(); ++index) {
            const auto &row = pane.rows[index];
            const auto &item = row.item;
            const auto top = static_cast<float>(row.top);
            const auto height = static_cast<float>(row.height);
            if (!item.label) {
                const g::SolidBrush line(separator);
                graphics.FillRectangle(&line, 4.0f, top, 284.0f, 1.0f);
                continue;
            }
            const bool active = static_cast<int>(index) == pane.selected && item.enabled;
            if (active)
                rounded_rectangle(graphics, hover_color, 4, top, 284, height, 5);
            art.icon(graphics, item.icon, 18, top + (height - icon_size) / 2);
            if (item.group != Group::none)
                art.icon(graphics, Icon::chevron, 250, top + (height - icon_size) / 2);

            const g::SolidBrush color(item.enabled ? foreground : disabled);
            const float left = &pane == &root ? 52.0f : 18.0f;
            const float right = row.shortcut.empty() ? (item.group == Group::none ? 274.0f : 242.0f) : 187.0f;
            text.SetAlignment(g::StringAlignmentNear);
            graphics.DrawString(item.label, -1, &font, g::RectF(left, top, right-left, height), &text, &color);
            if (!row.shortcut.empty()) {
                const g::SolidBrush shortcut_color(item.enabled ? hint : disabled);
                text.SetAlignment(g::StringAlignmentFar);
                graphics.DrawString(row.shortcut.c_str(), -1, &font, g::RectF(168, top, 106, height),
                                    &text, &shortcut_color);
            }
        }
    }

    bool draw(const Pane &pane) const {
        const auto art = std::atomic_load(&artwork);
        if (!art)
            return false;
        SIZE size{pane.bounds.right - pane.bounds.left, pane.bounds.bottom - pane.bounds.top};
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = size.cx;
        info.bmiHeader.biHeight = -size.cy;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        const HDC dc = CreateCompatibleDC(nullptr);
        if (!dc)
            return false;
        void *bits = nullptr;
        const HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bitmap) {
            DeleteDC(dc);
            return false;
        }
        const HGDIOBJ old = SelectObject(dc, bitmap);
        {
            g::Bitmap surface(size.cx, size.cy, size.cx * 4, PixelFormat32bppPARGB, static_cast<BYTE *>(bits));
            g::Graphics graphics(&surface);
            graphics.Clear(g::Color(0, 0, 0, 0));
            graphics.SetPageUnit(g::UnitPixel);
            graphics.ScaleTransform(scale, scale);
            graphics.SetSmoothingMode(g::SmoothingModeAntiAlias);
            graphics.SetPixelOffsetMode(g::PixelOffsetModeHalf);
            graphics.SetTextRenderingHint(g::TextRenderingHintAntiAliasGridFit);
            rounded_rectangle(graphics, background, 0, 0, menu_width, pane.height, radius);
            draw_rows(graphics, pane, *art);
        }
        POINT target{pane.bounds.left, pane.bounds.top}, source{};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        const bool updated = UpdateLayeredWindow(pane.window, nullptr, &target, &size, dc, &source, 0, &blend, ULW_ALPHA);
        SelectObject(dc, old);
        DeleteObject(bitmap);
        DeleteDC(dc);
        return updated;
    }

    bool place(Pane &pane, POINT position, HWND owner) {
        const int width = pixels(menu_width), height = pixels(pane.height);
        position.x = std::clamp(position.x, work.left, work.right - width);
        position.y = std::clamp(position.y, work.top, work.bottom - height);
        pane.bounds = {position.x, position.y, position.x + width, position.y + height};
        if (!pane.window)
            pane.window = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, window_class,
                                          &pane == &root ? L"Playback menu" : L"Playback submenu", WS_POPUP,
                                          position.x, position.y, width, height, owner, nullptr, module, &pane);
        return pane.window && draw(pane) &&
            SetWindowPos(pane.window, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_SHOWWINDOW | SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE);
    }

    bool show(const Player &source, POINT location, bool keyboard) {
        dismiss();
        player = source;
        build_root();
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        if (!GetMonitorInfoW(MonitorFromPoint(location, MONITOR_DEFAULTTONEAREST), &monitor))
            return false;
        work = monitor.rcWork;
        scale = std::min({1.0f, (work.right-work.left) / static_cast<float>(menu_width),
                          (work.bottom-work.top) / static_cast<float>(root.height)});
        if (pixels(menu_width) < 1 || pixels(root.height) < 1)
            return false;
        if (!prepare())
            return false;
        if (location.x + pixels(menu_width) > work.right)
            location.x -= pixels(menu_width);
        if (location.y + pixels(root.height) > work.bottom)
            location.y -= pixels(root.height);
        if (keyboard)
            root.selected = next_item(root, 1);
        DWORD process = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &process);
        was_foreground = process == GetCurrentProcessId();
        if (!place(root, location, player.surface)) {
            dismiss();
            return false;
        }
        SetCapture(root.window);
        SetTimer(root.window, focus_timer, 60, nullptr);
        open.store(true);
        return true;
    }

    void clear_submenu() {
        opened_group = Group::none;
        if (const auto window = submenu.window)
            DestroyWindow(window);
        submenu.selected = -1;
    }

    void dismiss() {
        const HWND window = root.window;
        root.window = nullptr;
        if (window)
            open.store(false);
        if (window && GetCapture() == window)
            ReleaseCapture();
        clear_submenu();
        if (window)
            DestroyWindow(window);
    }

    void open_submenu(bool keyboard) {
        if (!root.window || root.selected < 0)
            return;
        const Row &row = root.rows[root.selected];
        const Group group = row.item.group;
        if (group == Group::none)
            return;
        if (group != opened_group) {
            build_submenu(group);
            const int width = pixels(menu_width), gap = pixels(4);
            const LONG right = root.bounds.right + gap;
            const LONG left = root.bounds.left - width - gap;
            const LONG x = right + width <= work.right || left < work.left ? right : left;
            if (!place(submenu, {x, root.bounds.top + pixels(row.top - padding)}, root.window)) {
                clear_submenu();
                return;
            }
            opened_group = group;
        }
        if (keyboard) {
            submenu.selected = next_item(submenu, 1);
            draw(submenu);
        }
    }

    static int next_item(const Pane &pane, int direction) {
        const int count = static_cast<int>(pane.rows.size());
        int index = pane.selected < 0 && direction < 0 ? 0 : pane.selected;
        for (int checked = 0; checked < count; ++checked) {
            index = (index + direction + count) % count;
            const auto &item = pane.rows[index].item;
            if (item.label && item.enabled)
                return index;
        }
        return -1;
    }

    int item_at(const Pane &pane, POINT point) const {
        const float y = (point.y - pane.bounds.top) / scale;
        for (std::size_t i = 0; i < pane.rows.size(); ++i) {
            const auto &row = pane.rows[i];
            if (y >= row.top && y < row.top + row.height && row.item.label && row.item.enabled)
                return static_cast<int>(i);
        }
        return -1;
    }

    void select(Pane &pane, int index) const {
        if (pane.selected != index) {
            pane.selected = index;
            draw(pane);
        }
    }

    void activate(Pane &pane) {
        if (pane.selected < 0)
            return;
        const auto item = pane.rows[pane.selected].item;
        if (item.group != Group::none) {
            KillTimer(root.window, hover_timer);
            open_submenu(false);
            return;
        }
        dismiss();
        player.run(player.window, player.surface, item.action, item.command);
    }

    void mouse(UINT message, POINT point) {
        if (!root.window)
            return;
        Pane *pane = nullptr;
        if (submenu.window && PtInRect(&submenu.bounds, point))
            pane = &submenu;
        else if (PtInRect(&root.bounds, point))
            pane = &root;
        if (!pane || message == WM_RBUTTONUP) {
            if (message != WM_MOUSEMOVE)
                dismiss();
            return;
        }
        const int index = item_at(*pane, point);
        if (pane == &root && index != root.selected) {
            KillTimer(root.window, hover_timer);
            clear_submenu();
            if (index >= 0 && root.rows[index].item.group != Group::none)
                SetTimer(root.window, hover_timer, 160, nullptr);
        }
        select(*pane, index);
        if (message == WM_LBUTTONUP)
            activate(*pane);
    }

    bool handle(const MSG &message) {
        if (!root.window)
            return false;
        if (message.hwnd == root.window || message.hwnd == submenu.window)
            return false;
        if (message.message == WM_LBUTTONDOWN || message.message == WM_MBUTTONDOWN ||
            message.message == WM_RBUTTONDOWN) {
            dismiss();
            return false;
        }
        if (message.message != WM_KEYDOWN && message.message != WM_SYSKEYDOWN)
            return false;
        return key(message.wParam, is_navigation_key(message.wParam));
    }

    bool key(WPARAM code, bool navigate) {
        if (!root.window)
            return false;
        if (!navigate) {
            dismiss();
            return false;
        }
        KillTimer(root.window, hover_timer);
        Pane &pane = submenu.window ? submenu : root;
        switch (code) {
        case VK_UP:
        case VK_DOWN:
            select(pane, next_item(pane, code == VK_DOWN ? 1 : -1));
            return true;
        case VK_RIGHT:
            if (&pane == &root)
                open_submenu(true);
            return true;
        case VK_LEFT:
            clear_submenu();
            return true;
        case VK_RETURN:
            if (&pane == &root && pane.selected >= 0 && pane.rows[pane.selected].item.group != Group::none)
                open_submenu(true);
            else
                activate(pane);
            return true;
        case VK_ESCAPE:
            if (submenu.window) {
                clear_submenu();
                return true;
            }
            dismiss();
            return true;
        case VK_APPS:
            dismiss();
            return true;
        default:
            // VLC still receives its own hotkeys when the menu closes.
            dismiss();
            return false;
        }
    }
};

thread_local Menu menu;
}

void load_artwork(HINSTANCE module) {
    auto assets = std::make_shared<Artwork>();
    if (assets->load(module))
        std::atomic_store(&artwork, std::shared_ptr<const Artwork>(std::move(assets)));
}

void unload_artwork() {
    std::atomic_store(&artwork, std::shared_ptr<const Artwork>{});
}

bool show(const Player &player, POINT location, bool keyboard) {
    return menu.show(player, location, keyboard);
}

bool handle_message(const MSG &message) {
    return menu.handle(message);
}

bool is_open() {
    return open.load();
}

bool is_navigation_key(WPARAM key) {
    if (GetKeyState(VK_CONTROL) < 0 || GetKeyState(VK_MENU) < 0 || GetKeyState(VK_SHIFT) < 0)
        return false;
    switch (key) {
    case VK_UP: case VK_DOWN: case VK_LEFT: case VK_RIGHT:
    case VK_RETURN: case VK_ESCAPE: case VK_APPS:
        return true;
    default:
        return false;
    }
}

bool handle_key(WPARAM key, bool navigate) {
    return menu.key(key, navigate);
}

bool hide_ui_shortcut_available(void *vlc) {
    const auto shortcut = vlc_str2keycode("Ctrl+\\");
    if (!shortcut)
        return false;
    for (auto names = vlc_actions_get_key_names(vlc); names && *names; ++names) {
        const char *name = std::strncmp(*names, "key-", 4) == 0 ? *names + 4 : *names;
        for (const bool global : {false, true}) {
            std::uint32_t *keys = nullptr;
            const auto count = vlc_actions_get_keycodes(vlc, name, global, &keys);
            std::unique_ptr<std::uint32_t, decltype(&libvlc_free)> binding(keys, libvlc_free);
            if (count && std::find(keys, keys + count, shortcut) != keys + count)
                return false;
        }
    }
    return true;
}

}
