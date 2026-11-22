// Windows interface module for the media tracks panel in VLC 3.0.24.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define UNICODE
#include <windows.h>
#include <commctrl.h>

#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <memory>
#include <new>
#include <optional>
#include <string_view>
#include <string>
#include <utility>
#include <vector>

// The VLC 3.0 module ABI used here is defined by vlc_common.h, vlc_interface.h,
// vlc_plugin.h, vlc_variables.h and vlc_aout.h. Keep these declarations local.
struct Playlist;
struct Input;
struct InputItem;
struct AudioOutput;
struct LibVlc;
struct Media;
struct VlcInterface {
    const char *object_type;
    char *header;
    int flags;
    bool forced;
    void *libvlc;
    Playlist *parent;
};

struct VlcList;
union VlcValue {
    std::int64_t integer;
    char *string;
    VlcList *list;
};
struct VlcList {
    int type;
    int count;
    VlcValue *values;
};
struct MediaTrack {
    std::uint32_t codec, original_codec;
    int id, type;
};

extern "C" {
__declspec(dllimport) Input *playlist_CurrentInput(Playlist *);
__declspec(dllimport) AudioOutput *playlist_GetAout(Playlist *);
__declspec(dllimport) void vlc_object_release(void *);
__declspec(dllimport) int aout_DevicesList(AudioOutput *, char ***, char ***);
__declspec(dllimport) char *aout_DeviceGet(AudioOutput *);
__declspec(dllimport) int aout_DeviceSet(AudioOutput *, const char *);
__declspec(dllimport) int var_Get(Input *, const char *, VlcValue *);
__declspec(dllimport) int var_Set(Input *, const char *, VlcValue);
__declspec(dllimport) int var_Change(Input *, const char *, int, VlcValue *, VlcValue *);
__declspec(dllimport) void var_FreeList(VlcValue *, VlcValue *);
__declspec(dllimport) InputItem *input_GetItem(Input *);
__declspec(dllimport) char *input_item_GetURI(InputItem *);
__declspec(dllimport) const char *vlc_gettext(const char *);
__declspec(dllimport) LibVlc *libvlc_new(int, const char *const *);
__declspec(dllimport) void libvlc_release(LibVlc *);
__declspec(dllimport) void libvlc_free(void *);
__declspec(dllimport) Media *libvlc_media_new_location(LibVlc *, const char *);
__declspec(dllimport) void libvlc_media_release(Media *);
__declspec(dllimport) int libvlc_media_parse_with_options(Media *, int, int);
__declspec(dllimport) int libvlc_media_get_parsed_status(Media *);
__declspec(dllimport) void libvlc_media_parse_stop(Media *);
__declspec(dllimport) unsigned libvlc_media_tracks_get(Media *, MediaTrack ***);
__declspec(dllimport) void libvlc_media_tracks_release(MediaTrack **, unsigned);
}

namespace {
constexpr wchar_t host_class[] = L"VlcRedesignSubtitleTracks";
constexpr wchar_t no_devices_label[] = L"No output devices available";
// Match the layout dimensions and section positions in build.py.
constexpr int panel_width = 368, video_height = 481, audio_height = 504, subtitles_height = 480;
constexpr int list_x = 26, list_y = 96, list_width = panel_width - 52;
constexpr int row_height = 24, visible_tracks = 10, audio_rows = 4;
constexpr int devices_y = 280, device_rows = 6, refresh_y = 241, refresh_size = 32;
constexpr int preferences_margin = 18, preferences_height = 32;
constexpr LONG popup_right_margin = 16, popup_gap = 20;
constexpr int none_control = 1, track_control = 2, device_control = 3, refresh_control = 4;
constexpr DWORD dismiss_poll_ms = 15, dismiss_delay_ms = 50;
constexpr int get_choices = 0x24;
constexpr int no_track = -1;
constexpr COLORREF dark_panel = RGB(15, 15, 15), light_panel = RGB(194, 193, 193);
constexpr COLORREF accent = RGB(211, 121, 24);

using InputRef = std::unique_ptr<Input, decltype(&vlc_object_release)>;
using AudioOutputRef = std::unique_ptr<AudioOutput, decltype(&vlc_object_release)>;

enum class Page { audio = 0, video = 1, subtitles = 2 };  // libvlc_track_type_t

struct Track {
    std::int64_t id;
    std::wstring label;

    bool operator==(const Track &other) const {
        return id == other.id && label == other.label;
    }
};

struct AudioDevice {
    std::string id;
    std::wstring label;

    bool operator==(const AudioDevice &other) const {
        return id == other.id && label == other.label;
    }
};

std::wstring from_utf8(const char *label) {
    if (!label || !*label)
        return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, label, -1, nullptr, 0);
    if (!size)
        return {};
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, label, -1, result.data(), size);
    result.pop_back();
    return result;
}

struct Choices {
    VlcValue ids{}, labels{};
    ~Choices() {
        if (ids.list)
            var_FreeList(&ids, &labels);
    }
};

std::vector<Track> media_tracks(Input *input, const char *variable) {
    Choices choices;
    std::vector<Track> tracks;
    if (!input || var_Change(input, variable, get_choices, &choices.ids, &choices.labels))
        return tracks;
    for (int i = 0; i < choices.ids.list->count; ++i) {
        const auto id = choices.ids.list->values[i].integer;
        if (id == no_track)
            continue;
        auto label = from_utf8(choices.labels.list->values[i].string);
        if (label.empty())
            label = L"Track " + std::to_wstring(tracks.size() + 1);
        tracks.push_back({id, std::move(label)});
    }
    return tracks;
}

std::vector<AudioDevice> audio_devices(AudioOutput *output) {
    struct DeviceList {
        int count = 0;
        char **ids = nullptr, **names = nullptr;

        ~DeviceList() {
            for (int i = 0; i < count; ++i) {
                libvlc_free(ids[i]);
                libvlc_free(names[i]);
            }
            libvlc_free(ids);
            libvlc_free(names);
        }
    } list;

    std::vector<AudioDevice> devices;
    if (!output)
        return devices;
    list.count = aout_DevicesList(output, &list.ids, &list.names);
    for (int i = 0; i < list.count; ++i) {
        const char *id = list.ids[i];
        auto label = from_utf8(list.names[i]);
        if (label.empty())
            label = *id ? from_utf8(id) : L"Default";
        devices.push_back({id, std::move(label)});
    }
    return devices;
}

class EmbeddedTracks {
public:
    void set_input(Input *input) {
        if (media_)
            libvlc_media_parse_stop(media_.get());
        media_.reset();
        entries_.reset();
        if (!input)
            return;
        auto item = input_GetItem(input);
        std::unique_ptr<char, decltype(&libvlc_free)> uri(item ? input_item_GetURI(item) : nullptr, libvlc_free);
        if (!uri || std::string_view(uri.get()).substr(0, 7) != "file://")
            return;

        // Read the file's metadata without opening a second player or loading subtitle sidecars.
        if (!library_) {
            const char *opts[] = {"--ignore-config", "--no-media-library", "--no-sub-autodetect-file", "--intf=dummy"};
            library_.reset(libvlc_new(4, opts));
        }
        if (library_)
            media_.reset(libvlc_media_new_location(library_.get(), uri.get()));
        if (media_ && libvlc_media_parse_with_options(media_.get(), 0, 2500))
            media_.reset();
    }

    void filter(std::vector<Track> &tracks, Page page) {
        if (media_) {
            const int status = libvlc_media_get_parsed_status(media_.get());
            if (status == 0) {
                tracks.clear();
                return;
            }
            if (status == 4) {  // libvlc_media_parsed_status_done
                MediaTrack **entries = nullptr;
                const auto count = libvlc_media_tracks_get(media_.get(), &entries);
                if (count) {
                    entries_.emplace();
                    for (unsigned i = 0; i < count; ++i)
                        entries_->push_back({entries[i]->id, entries[i]->type});
                }
                libvlc_media_tracks_release(entries, count);
            }
            media_.reset();
        }
        if (entries_) {
            const auto external = [&](const Track &track) {
                return std::none_of(entries_->begin(), entries_->end(), [&](const Entry &entry) {
                    return track.id == entry.id && entry.type == static_cast<int>(page);
                });
            };
            tracks.erase(std::remove_if(tracks.begin(), tracks.end(), external), tracks.end());
        }
    }

private:
    struct Entry { int id, type; };
    std::unique_ptr<LibVlc, decltype(&libvlc_release)> library_{nullptr, libvlc_release};
    std::unique_ptr<Media, decltype(&libvlc_media_release)> media_{nullptr, libvlc_media_release};
    std::optional<std::vector<Entry>> entries_;
};

struct PanelWindow {
    HWND hwnd = nullptr;
    Page page = Page::subtitles;
};

bool is_skin_window(HWND window) {
    wchar_t name[32];
    return GetClassNameW(window, name, 32) && wcscmp(name, L"SkinWindowClass") == 0;
}

std::optional<Page> panel_page(HWND window) {
    RECT rect;
    if (!is_skin_window(window) || !GetClientRect(window, &rect) || rect.right != panel_width)
        return std::nullopt;
    switch (rect.bottom) {
    case video_height: return Page::video;
    case audio_height: return Page::audio;
    case subtitles_height: return Page::subtitles;
    default: return std::nullopt;
    }
}

BOOL CALLBACK find_panel(HWND window, LPARAM data) {
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid != GetCurrentProcessId())
        return TRUE;

    const auto page = panel_page(window);
    if (!page)
        return TRUE;
    auto &match = *reinterpret_cast<PanelWindow *>(data);
    const bool visible = IsWindowVisible(window);
    if (!match.hwnd || visible)
        match = {window, *page};
    return !visible;
}

std::optional<RECT> subtitles_button(HWND window) {
    RECT client;
    if (!is_skin_window(window) || !IsWindowVisible(window) || IsIconic(window) ||
        !GetClientRect(window, &client))
        return std::nullopt;
    // The main player and fullscreen controller share control_bar() in build.py.
    const bool fullscreen_controls = client.right == 1000 && client.bottom == 56;
    if (!fullscreen_controls && (client.right < 860 || client.bottom < 420))
        return std::nullopt;
    const LONG top = client.bottom - (fullscreen_controls ? 44 : 43);
    return RECT{client.right - 142, top, client.right - 112, top + 32};
}

BOOL CALLBACK find_toolbar(HWND window, LPARAM data) {
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    if (process != GetCurrentProcessId() || !subtitles_button(window))
        return TRUE;
    *reinterpret_cast<HWND *>(data) = window;
    return FALSE;
}

std::optional<POINT> panel_position(HWND toolbar, LONG width, LONG height) {
    const auto button = subtitles_button(toolbar);
    POINT origin{};
    if (!button || !ClientToScreen(toolbar, &origin))
        return std::nullopt;

    POINT position{origin.x + button->right + 112 - popup_right_margin - width,
                   origin.y + button->top - popup_gap - height};
    const POINT button_center{origin.x + (button->left + button->right) / 2,
                              origin.y + (button->top + button->bottom) / 2};
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (GetMonitorInfoW(MonitorFromPoint(button_center, MONITOR_DEFAULTTONEAREST), &monitor)) {
        const RECT &work = monitor.rcWork;
        const LONG below = origin.y + button->bottom + popup_gap;
        if (position.y < work.top && below + height <= work.bottom)
            position.y = below;
        position.x = std::clamp(position.x, work.left, std::max(work.left, work.right - width));
        position.y = std::clamp(position.y, work.top, std::max(work.top, work.bottom - height));
    }
    return position;
}

struct PanelAnchor {
    HWND toolbar = nullptr;
    HWND panel = nullptr;
    HWND pressed_toolbar = nullptr;
};

// Position changes and mouse messages are handled on the skin's UI thread.
thread_local PanelAnchor anchor;
constexpr UINT_PTR anchor_subclass = 1;

LRESULT CALLBACK anchor_popup(HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR);

BOOL CALLBACK detach_popup_candidates(HWND window, LPARAM kept) {
    if (window != reinterpret_cast<HWND>(kept))
        RemoveWindowSubclass(window, anchor_popup, anchor_subclass);
    return TRUE;
}

LRESULT CALLBACK anchor_popup(HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR) {
    if (message == WM_WINDOWPOSCHANGING) {
        auto &change = *reinterpret_cast<WINDOWPOS *>(lparam);
        RECT current;
        // Skins2 also sends position notifications when it updates the window's shape.
        if (!(change.flags & (SWP_NOMOVE | SWP_HIDEWINDOW)) && GetWindowRect(window, &current)) {
            const bool current_size = change.flags & SWP_NOSIZE;
            const LONG width = current_size ? current.right - current.left : change.cx;
            const LONG height = current_size ? current.bottom - current.top : change.cy;
            // On the first open, skins2's hidden windows do not yet have their layout sizes.
            if (width == panel_width && (height == audio_height || height == video_height || height == subtitles_height)) {
                if (const auto position = panel_position(anchor.toolbar, width, height)) {
                    if (change.x != position->x || change.y != position->y)
                        change.flags |= SWP_NOCOPYBITS;
                    change.x = position->x;
                    change.y = position->y;
                    if (anchor.panel != window) {
                        anchor.panel = window;
                        EnumThreadWindows(GetCurrentThreadId(), detach_popup_candidates, reinterpret_cast<LPARAM>(window));
                    }
                }
            }
        }
    } else if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(window, anchor_popup, id);
        if (anchor.panel == window)
            anchor.panel = nullptr;
    }
    const LRESULT result = DefSubclassProc(window, message, wparam, lparam);
    if (message == WM_WINDOWPOSCHANGED && anchor.panel == window && IsWindowVisible(window)) {
        const UINT flags = reinterpret_cast<const WINDOWPOS *>(lparam)->flags;
        constexpr UINT unchanged_geometry = SWP_NOMOVE | SWP_NOSIZE;
        // Repaint the entire skin after the move, including the areas around the native lists.
        if (flags & SWP_SHOWWINDOW || (flags & unchanged_geometry) != unchanged_geometry)
            InvalidateRect(window, nullptr, FALSE);
    }
    return result;
}

void move_anchored_panel() {
    RECT current;
    if (!GetWindowRect(anchor.panel, &current))
        return;
    const auto position = panel_position(anchor.toolbar, current.right - current.left, current.bottom - current.top);
    if (position && (position->x != current.left || position->y != current.top))
        SetWindowPos(anchor.panel, nullptr, position->x, position->y, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

LRESULT CALLBACK follow_toolbar(HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR) {
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(window, follow_toolbar, id);
        if (anchor.toolbar == window)
            anchor.toolbar = nullptr;
    }
    const LRESULT result = DefSubclassProc(window, message, wparam, lparam);
    if (message == WM_WINDOWPOSCHANGED && anchor.toolbar == window && IsWindowVisible(anchor.panel)) {
        const UINT flags = reinterpret_cast<const WINDOWPOS *>(lparam)->flags;
        if ((flags & (SWP_NOMOVE | SWP_NOSIZE)) != (SWP_NOMOVE | SWP_NOSIZE))
            move_anchored_panel();
    }
    return result;
}

BOOL CALLBACK attach_popup_candidate(HWND window, LPARAM owner) {
    if (window != anchor.toolbar && GetWindow(window, GW_OWNER) == reinterpret_cast<HWND>(owner) &&
        !IsWindowVisible(window) && is_skin_window(window))
        SetWindowSubclass(window, anchor_popup, anchor_subclass, 0);
    return TRUE;
}

void anchor_panel_to(HWND toolbar) {
    if (IsWindowVisible(anchor.panel))
        return;
    if (anchor.toolbar && anchor.toolbar != toolbar)
        RemoveWindowSubclass(anchor.toolbar, follow_toolbar, anchor_subclass);
    anchor.toolbar = toolbar;
    SetWindowSubclass(toolbar, follow_toolbar, anchor_subclass, 0);

    if (anchor.panel) {
        move_anchored_panel();
    } else {
        EnumThreadWindows(GetCurrentThreadId(), attach_popup_candidate,
                          reinterpret_cast<LPARAM>(GetWindow(toolbar, GW_OWNER)));
    }
}

void handle_toolbar_click(const MSG &message) {
    if (message.message != WM_LBUTTONDOWN && message.message != WM_LBUTTONDBLCLK && message.message != WM_LBUTTONUP)
        return;
    const POINT cursor{static_cast<short>(LOWORD(message.lParam)), static_cast<short>(HIWORD(message.lParam))};
    const auto button = subtitles_button(message.hwnd);
    const bool hit = button && PtInRect(&*button, cursor);
    if (message.message == WM_LBUTTONUP) {
        const HWND pressed = std::exchange(anchor.pressed_toolbar, nullptr);
        if (hit && pressed == message.hwnd)
            anchor_panel_to(message.hwnd);
    } else {
        anchor.pressed_toolbar = hit ? message.hwnd : nullptr;
    }
}

void press_skin_button(HWND window, LPARAM position) {
    SendMessageW(window, WM_MOUSEMOVE, 0, position);
    SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, position);
    SendMessageW(window, WM_LBUTTONUP, 0, position);
}

void close_skin_panel(HWND window) {
    const auto page = panel_page(window);
    if (!page || !IsWindowVisible(window))
        return;

    // Hide the native window before the close button can raise it. This runs on
    // the skin's UI thread so a layout change cannot come between the messages.
    ShowWindow(window, SW_HIDE);
    constexpr int tab_centers[] = {188, 72, 304};  // Audio, Video, Subtitles in build.py.
    press_skin_button(window, MAKELPARAM(tab_centers[static_cast<int>(*page)], 23));
}

struct PreferencesWindows {
    std::wstring simple = from_utf8(vlc_gettext("Simple Preferences"));
    std::wstring advanced = from_utf8(vlc_gettext("Advanced Preferences"));
    HWND window = nullptr;
};

BOOL CALLBACK find_preferences(HWND window, LPARAM data) {
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    wchar_t name[32], title[256];
    if (pid != GetCurrentProcessId() || !GetClassNameW(window, name, 32) ||
        wcsncmp(name, L"Qt", 2) != 0 || !InternalGetWindowText(window, title, 256))
        return TRUE;
    auto &match = *reinterpret_cast<PreferencesWindows *>(data);
    if (match.simple != title && match.advanced != title)
        return TRUE;
    match.window = window;
    return FALSE;
}

HWND preferences_window() {
    PreferencesWindows match;
    EnumWindows(find_preferences, reinterpret_cast<LPARAM>(&match));
    return match.window;
}

struct PreferencesPlacement {
    HWND dialog;
    HWND insert_after = HWND_TOP;
    bool reorder = false;
};

BOOL CALLBACK find_preferences_placement(HWND window, LPARAM data) {
    auto &placement = *reinterpret_cast<PreferencesPlacement *>(data);
    if (window == placement.dialog)
        return FALSE;
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(window) || !is_skin_window(window))
        return TRUE;
    // Place the dialog above VLC's windows even when Windows keeps another app active.
    placement.insert_after = GetWindow(window, GW_HWNDPREV);
    placement.reorder = true;
    return FALSE;
}

struct PreferencesRequest {
    HWND pressed_panel = nullptr;
    UINT_PTR timer = 0;
    ULONGLONG expires = 0;
};

// The button and its timer both run on the skin's UI thread.
thread_local PreferencesRequest preferences;

void finish_preferences_request(HWND window) {
    if (preferences.timer) {
        KillTimer(nullptr, preferences.timer);
        preferences.timer = 0;
    }
    if (!window)
        return;
    if (IsIconic(window))
        ShowWindowAsync(window, SW_RESTORE);
    SetForegroundWindow(window);
    PreferencesPlacement placement{window};
    EnumWindows(find_preferences_placement, reinterpret_cast<LPARAM>(&placement));
    const UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS;
    SetWindowPos(window, placement.insert_after, 0, 0, 0, 0, flags | (placement.reorder ? 0 : SWP_NOZORDER));
}

void CALLBACK preferences_ready(HWND, UINT, UINT_PTR timer, DWORD) {
    if (timer != preferences.timer)
        return;
    const HWND window = preferences_window();
    if (window || GetTickCount64() >= preferences.expires)
        finish_preferences_request(window);
}

void show_preferences(HWND panel, LPARAM position) {
    if (const HWND window = preferences_window()) {
        close_skin_panel(panel);
        finish_preferences_request(window);
        return;
    }
    if (!preferences.timer) {
        preferences.expires = GetTickCount64() + 5000;
        preferences.timer = SetTimer(nullptr, 0, 25, preferences_ready);
        press_skin_button(panel, position);
    }
    close_skin_panel(panel);
}

bool over_preferences_button(const MSG &message) {
    const int x = static_cast<short>(LOWORD(message.lParam));
    const int y = static_cast<short>(HIWORD(message.lParam));
    if (x < preferences_margin || x >= panel_width - preferences_margin ||
        y < subtitles_height - preferences_margin - preferences_height)
        return false;
    const auto page = panel_page(message.hwnd);
    if (!page)
        return false;
    constexpr int heights[] = {audio_height, video_height, subtitles_height};
    const int bottom = heights[static_cast<int>(*page)] - preferences_margin;
    return y >= bottom - preferences_height && y < bottom;
}

void handle_preferences_click(MSG &message) {
    if (message.message == WM_LBUTTONDOWN || message.message == WM_LBUTTONDBLCLK) {
        preferences.pressed_panel = over_preferences_button(message) ? message.hwnd : nullptr;
        if (preferences.pressed_panel)
            message.message = WM_NULL;
    } else if (message.message == WM_LBUTTONUP && preferences.pressed_panel) {
        const HWND panel = std::exchange(preferences.pressed_panel, nullptr);
        if (panel == message.hwnd) {
            const bool activate = IsWindowVisible(panel) && over_preferences_button(message);
            message.message = WM_NULL;
            if (activate)
                show_preferences(panel, message.lParam);
        }
    }
}

UINT dismiss_message() {
    static const UINT id = RegisterWindowMessageW(L"VlcRedesign.DismissTracksPanel");
    return id;
}

LRESULT CALLBACK skin_events_from_queue(int code, WPARAM removal, LPARAM data) {
    if (code == HC_ACTION && removal == PM_REMOVE) {
        auto &message = *reinterpret_cast<MSG *>(data);
        if (message.message == dismiss_message()) {
            message.message = WM_NULL;
            close_skin_panel(message.hwnd);
        } else {
            handle_preferences_click(message);
            handle_toolbar_click(message);
        }
    }
    return CallNextHookEx(nullptr, code, removal, data);
}

class Panel {
public:
    explicit Panel(Playlist *playlist) : playlist_(playlist) {}

    ~Panel() {
        if (thread_) {
            SetEvent(stop_);
            WaitForSingleObject(thread_, INFINITE);
            CloseHandle(thread_);
        }
        if (stop_)
            CloseHandle(stop_);
    }

    bool start() {
        stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!stop_)
            return false;
        thread_ = CreateThread(nullptr, 0, run_thread, this, 0, nullptr);
        return thread_ != nullptr;
    }

private:
    Playlist *playlist_;
    HANDLE stop_ = nullptr, thread_ = nullptr;
    HINSTANCE module_ = nullptr;
    HHOOK skin_hook_ = nullptr;
    DWORD skin_thread_ = 0;
    HWND host_ = nullptr, list_ = nullptr, none_ = nullptr;
    HWND device_list_ = nullptr;
    HFONT font_ = nullptr;
    Page page_ = Page::subtitles;
    InputRef input_{nullptr, vlc_object_release};
    EmbeddedTracks embedded_;
    std::vector<Track> tracks_;
    std::int64_t selected_track_ = no_track;
    std::vector<AudioDevice> devices_;
    std::string selected_device_;
    COLORREF background_ = dark_panel;
    HWND click_panel_ = nullptr;
    bool mouse_down_ = false, outside_press_ = false;
    ULONGLONG dismiss_at_ = 0;

    const char *track_variable() const {
        switch (page_) {
        case Page::video: return "video-es";
        case Page::audio: return "audio-es";
        default: return "spu-es";
        }
    }

    int track_rows() const {
        return page_ == Page::audio ? audio_rows : visible_tracks;
    }

    static DWORD WINAPI run_thread(void *data) {
        static_cast<Panel *>(data)->run();
        return 0;
    }

    void run() {
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&run_thread), &module_);
        WNDCLASSW cls{};
        cls.lpfnWndProc = window_proc;
        cls.hInstance = module_;
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        cls.lpszClassName = host_class;
        if (!RegisterClassW(&cls))
            return;

        font_ = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                            DEFAULT_PITCH, L"Segoe UI");
        auto next_refresh = GetTickCount64();
        for (;;) {
            const auto now = GetTickCount64();
            auto wait = static_cast<DWORD>(now < next_refresh ? next_refresh - now : 0);
            if (host_)
                wait = std::min(wait, dismiss_poll_ms);
            const auto event = MsgWaitForMultipleObjects(1, &stop_, FALSE, wait, QS_ALLINPUT);
            if (event == WAIT_OBJECT_0 || event == WAIT_FAILED)
                break;
            MSG message;
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if (GetTickCount64() >= next_refresh) {
                refresh();
                next_refresh = GetTickCount64() + 150;
            }
            dismiss_outside_click();
        }
        if (skin_hook_)
            UnhookWindowsHookEx(skin_hook_);
        if (host_)
            DestroyWindow(host_);
        embedded_.set_input(nullptr);
        if (font_)
            DeleteObject(font_);
        UnregisterClassW(host_class, module_);
    }

    bool watch_skin_thread(HWND window) {
        DWORD process = 0;
        const DWORD owner = GetWindowThreadProcessId(window, &process);
        if (!owner || process != GetCurrentProcessId())
            return false;
        if (skin_thread_ != owner || !skin_hook_) {
            if (skin_hook_)
                UnhookWindowsHookEx(skin_hook_);
            // A hook callback or a preferences timer can outlive the worker thread.
            HMODULE hook_module = nullptr;
            skin_hook_ = nullptr;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                    reinterpret_cast<LPCWSTR>(&skin_events_from_queue), &hook_module))
                return false;
            skin_hook_ = SetWindowsHookExW(WH_GETMESSAGE, skin_events_from_queue, hook_module, owner);
            skin_thread_ = owner;
        }
        return skin_hook_ != nullptr;
    }

    void request_dismiss(HWND window) {
        const UINT message = dismiss_message();
        if (message && watch_skin_thread(window))
            PostMessageW(window, message, 0, 0);
    }

    void dismiss_outside_click() {
        const HWND parent = host_ ? GetParent(host_) : nullptr;
        const HWND panel = parent && IsWindowVisible(parent) ? parent : nullptr;
        if (panel != click_panel_) {
            click_panel_ = panel;
            mouse_down_ = outside_press_ = false;
            dismiss_at_ = 0;
        }
        if (!panel)
            return;

        const bool down = (GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON) |
                           GetAsyncKeyState(VK_MBUTTON)) & 0x8000;
        const auto now = GetTickCount64();
        if (down && !mouse_down_) {
            POINT point;
            outside_press_ = GetCursorPos(&point) && GetAncestor(WindowFromPoint(point), GA_ROOT) != panel;
            dismiss_at_ = 0;
        } else if (!down && mouse_down_ && outside_press_) {
            // Let the real click run first, including a click on the toolbar's own close toggle.
            dismiss_at_ = now + dismiss_delay_ms;
        }
        mouse_down_ = down;

        if (dismiss_at_ && now >= dismiss_at_) {
            dismiss_at_ = 0;
            request_dismiss(panel);
        }
    }

    void refresh() {
        PanelWindow window;
        EnumWindows(find_panel, reinterpret_cast<LPARAM>(&window));
        if (window.hwnd)
            watch_skin_thread(window.hwnd);
        else if (!skin_hook_) {
            HWND toolbar = nullptr;
            EnumWindows(find_toolbar, reinterpret_cast<LPARAM>(&toolbar));
            if (toolbar)
                watch_skin_thread(toolbar);
        }
        if (!window.hwnd || !IsWindowVisible(window.hwnd)) {
            if (host_)
                DestroyWindow(host_);
            if (input_)
                embedded_.set_input(nullptr);
            input_.reset();
            tracks_.clear();
            devices_.clear();
            return;
        }
        if (host_ && (GetParent(host_) != window.hwnd || window.page != page_))
            DestroyWindow(host_);
        page_ = window.page;

        const HDC dc = GetDC(window.hwnd);
        if (dc) {
            const COLORREF color = GetPixel(dc, 6, list_y);
            ReleaseDC(window.hwnd, dc);
            if ((color == light_panel || color == dark_panel) && color != background_) {
                background_ = color;
                if (host_)
                    RedrawWindow(host_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
            }
        }

        const bool new_host = !host_;
        if (new_host) {
            const bool audio = page_ == Page::audio;
            const int width = audio ? panel_width - list_x - 18 : list_width;
            const int height = audio ? devices_y - list_y + device_rows * row_height
                                     : row_height * (visible_tracks + 1);
            if (!CreateWindowExW(WS_EX_NOPARENTNOTIFY, host_class, L"Media Tracks",
                                 WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, list_x, list_y,
                                 width, height, window.hwnd, nullptr, module_, this))
                return;
        }

        refresh_tracks(new_host);
        if (page_ == Page::audio)
            refresh_devices(new_host);
    }

    template <typename Item, typename Id>
    static void update_list(HWND list, const std::vector<Item> &items, const Id &selected,
                            bool reload, LRESULT top, int rows, const wchar_t *empty_label = nullptr) {
        SendMessageW(list, WM_SETREDRAW, FALSE, 0);
        if (reload) {
            SendMessageW(list, LB_RESETCONTENT, 0, 0);
            for (const auto &item : items)
                SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.label.c_str()));
            if (items.empty() && empty_label)
                SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(empty_label));
        }
        const auto active = std::find_if(items.begin(), items.end(),
                                         [&](const Item &item) { return item.id == selected; });
        const auto index = active == items.end() ? -1 : active - items.begin();
        SendMessageW(list, LB_SETCURSEL, static_cast<WPARAM>(index), 0);
        const auto last_top = std::max<LRESULT>(0, static_cast<LRESULT>(items.size()) - rows);
        SendMessageW(list, LB_SETTOPINDEX, static_cast<WPARAM>(std::clamp<LRESULT>(top, 0, last_top)), 0);
        SendMessageW(list, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(list, nullptr, TRUE);
    }

    void refresh_tracks(bool new_host) {
        InputRef current(playlist_CurrentInput(playlist_), vlc_object_release);
        const bool new_input = current.get() != input_.get();
        if (new_input)
            embedded_.set_input(current.get());
        const char *variable = track_variable();
        auto entries = media_tracks(current.get(), variable);
        EnableWindow(none_, !entries.empty());
        embedded_.filter(entries, page_);
        VlcValue selection{no_track};
        if (current)
            var_Get(current.get(), variable, &selection);

        const bool reload = new_host || new_input || entries != tracks_;
        if (reload || selected_track_ != selection.integer) {
            const auto top = new_host || new_input ? 0 : SendMessageW(list_, LB_GETTOPINDEX, 0, 0);
            tracks_ = std::move(entries);
            selected_track_ = selection.integer;
            update_list(list_, tracks_, selected_track_, reload, top, track_rows());
            InvalidateRect(none_, nullptr, TRUE);
        }
        input_ = std::move(current);
    }

    void refresh_devices(bool force) {
        AudioOutputRef output(playlist_GetAout(playlist_), vlc_object_release);
        auto available = audio_devices(output.get());
        std::unique_ptr<char, decltype(&libvlc_free)> id(output ? aout_DeviceGet(output.get()) : nullptr,
                                                       libvlc_free);
        const std::string selected = id ? id.get() : "";
        const bool reload = force || available != devices_;
        if (reload || selected_device_ != selected) {
            const auto top = SendMessageW(device_list_, LB_GETTOPINDEX, 0, 0);
            devices_ = std::move(available);
            selected_device_ = selected;
            update_list(device_list_, devices_, selected_device_, reload, top, device_rows,
                        no_devices_label);
            EnableWindow(device_list_, !devices_.empty());
        }
    }

    void select_track(std::int64_t id) {
        InputRef current(playlist_CurrentInput(playlist_), vlc_object_release);
        if (!current || current.get() != input_.get())
            return;
        // The input may change its available streams without switching media.
        const char *variable = track_variable();
        const auto available = media_tracks(current.get(), variable);
        const auto can_select = [&](const Track &entry) { return id == no_track || id == entry.id; };
        if (std::any_of(available.begin(), available.end(), can_select))
            var_Set(current.get(), variable, VlcValue{id});
    }

    void select_device(const AudioDevice &device) {
        AudioOutputRef output(playlist_GetAout(playlist_), vlc_object_release);
        if (output)
            aout_DeviceSet(output.get(), device.id.empty() ? nullptr : device.id.c_str());
        refresh_devices(true);
    }

    void draw_row(const DRAWITEMSTRUCT &item, COLORREF tone) const {
        const wchar_t *label = nullptr;
        bool active = false;
        if (item.CtlID == none_control) {
            label = L"None";
            active = selected_track_ == no_track;
        } else if (item.CtlID == track_control && item.itemID < tracks_.size()) {
            const auto &track = tracks_[item.itemID];
            label = track.label.c_str();
            active = track.id == selected_track_;
        } else if (item.CtlID == device_control && item.itemID < devices_.size()) {
            const auto &device = devices_[item.itemID];
            label = device.label.c_str();
            active = device.id == selected_device_;
        } else if (item.CtlID == device_control && item.itemID == 0 && devices_.empty()) {
            label = no_devices_label;
        }
        if (!label)
            return;

        RECT text = item.rcItem;
        text.left += 32;
        text.right -= 5;
        SetBkMode(item.hDC, TRANSPARENT);
        SetTextColor(item.hDC, tone);
        const HGDIOBJ previous_font = font_ ? SelectObject(item.hDC, font_) : nullptr;
        DrawTextW(item.hDC, label, -1, &text, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
        if (previous_font)
            SelectObject(item.hDC, previous_font);

        if (active) {
            const HPEN pen = CreatePen(PS_SOLID, 2, accent);
            const auto previous_pen = SelectObject(item.hDC, pen);
            const int y = item.rcItem.top + row_height / 2;
            MoveToEx(item.hDC, 3, y, nullptr);
            LineTo(item.hDC, 8, y + 5);
            LineTo(item.hDC, 17, y - 4);
            SelectObject(item.hDC, previous_pen);
            DeleteObject(pen);
        }
    }

    static void draw_refresh(const DRAWITEMSTRUCT &item, COLORREF tone) {
        const int x = item.rcItem.left, y = item.rcItem.top;
        const POINT upper[] = {{x + 7, y + 13}, {x + 11, y + 5}, {x + 23, y + 4}, {x + 25, y + 14}};
        const POINT lower[] = {{x + 25, y + 19}, {x + 21, y + 27}, {x + 9, y + 28}, {x + 7, y + 18}};
        const POINT left_arrow[] = {{x + 5, y + 9}, {x + 7, y + 14}, {x + 12, y + 12}};
        const POINT right_arrow[] = {{x + 27, y + 23}, {x + 25, y + 18}, {x + 20, y + 20}};
        const HPEN pen = CreatePen(PS_SOLID, 2, item.itemState & ODS_SELECTED ? accent : tone);
        const auto previous = SelectObject(item.hDC, pen);
        PolyBezier(item.hDC, upper, 4);
        PolyBezier(item.hDC, lower, 4);
        Polyline(item.hDC, left_arrow, 3);
        Polyline(item.hDC, right_arrow, 3);
        SelectObject(item.hDC, previous);
        DeleteObject(pen);
    }

    void draw_item(const DRAWITEMSTRUCT &item) const {
        const auto tone = background_ == light_panel ? RGB(54, 54, 54) : RGB(172, 172, 172);
        SetDCBrushColor(item.hDC, background_);
        FillRect(item.hDC, &item.rcItem, reinterpret_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        if (item.CtlID == refresh_control)
            draw_refresh(item, tone);
        else
            draw_row(item, tone);
    }

    HWND make_button(int control, const wchar_t *label, int x, int y, int w, int h, DWORD extra_style = 0) {
        return CreateWindowExW(0, L"BUTTON", label,
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW | extra_style,
                               x, y, w, h, host_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(control)),
                               module_, nullptr);
    }

    HWND make_list(int control, int y, int rows) {
        return CreateWindowExW(0, L"LISTBOX", nullptr, WS_CHILD | WS_VISIBLE |
                               WS_VSCROLL | WS_TABSTOP | LBS_NOTIFY | LBS_OWNERDRAWFIXED |
                               LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
                               0, y, list_width, rows * row_height, host_,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(control)), module_, nullptr);
    }

    bool create_controls(HWND hwnd) {
        host_ = hwnd;
        none_ = make_button(none_control, L"None", 0, 0, list_width, row_height, WS_DISABLED);
        list_ = make_list(track_control, row_height, track_rows());
        if (!none_ || !list_)
            return false;
        if (page_ != Page::audio)
            return true;

        const int refresh_x = panel_width - 52 - list_x;
        const auto refresh_button = make_button(refresh_control, L"Refresh output devices",
                                                refresh_x, refresh_y - list_y, refresh_size, refresh_size);
        device_list_ = make_list(device_control, devices_y - list_y, device_rows);
        return refresh_button && device_list_;
    }

    void control_changed(WORD control, WORD notification) {
        if (control == none_control && notification == BN_CLICKED) {
            select_track(no_track);
        } else if (control == refresh_control && notification == BN_CLICKED) {
            refresh_devices(true);
        } else if (control == track_control && notification == LBN_SELCHANGE) {
            const auto index = SendMessageW(list_, LB_GETCURSEL, 0, 0);
            if (index >= 0 && static_cast<size_t>(index) < tracks_.size())
                select_track(tracks_[index].id);
        } else if (control == device_control && notification == LBN_SELCHANGE) {
            const auto index = SendMessageW(device_list_, LB_GETCURSEL, 0, 0);
            if (index >= 0 && static_cast<size_t>(index) < devices_.size())
                select_device(devices_[index]);
        }
    }

    static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_NCCREATE) {
            const auto create = reinterpret_cast<const CREATESTRUCTW *>(lp);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
        const auto panel = reinterpret_cast<Panel *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (!panel)
            return DefWindowProcW(hwnd, msg, wp, lp);

        switch (msg) {
        case WM_CREATE:
            return panel->create_controls(hwnd) ? 0 : -1;
        case WM_MEASUREITEM:
            reinterpret_cast<MEASUREITEMSTRUCT *>(lp)->itemHeight = row_height;
            return TRUE;
        case WM_DRAWITEM:
            panel->draw_item(*reinterpret_cast<const DRAWITEMSTRUCT *>(lp));
            return TRUE;
        case WM_CTLCOLORLISTBOX:
            SetDCBrushColor(reinterpret_cast<HDC>(wp), panel->background_);
            return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
        case WM_COMMAND:
            panel->control_changed(LOWORD(wp), HIWORD(wp));
            return 0;
        case WM_DESTROY:
            panel->host_ = nullptr;
            panel->list_ = panel->none_ = panel->device_list_ = nullptr;
            break;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
};

Panel *active_panel = nullptr;

int open_panel(VlcInterface *interface) {
    if (active_panel || !interface->parent)
        return -1;
    auto panel = std::unique_ptr<Panel>(new (std::nothrow) Panel(interface->parent));
    if (!panel || !panel->start())
        return -1;
    active_panel = panel.release();
    return 0;
}

void close_panel(VlcInterface *) {
    delete active_panel;
    active_panel = nullptr;
}

enum ModuleProperty {
    create_module = 0, capability = 0x102, score = 0x103, open_callback = 0x104,
    close_callback = 0x105, name = 0x107, description = 0x109,
};
}

extern "C" __declspec(dllexport) int vlc_entry__3_0_0f(
    int (*set)(void *, void *, int, ...), void *data) {
    void *module;
    if (set(data, nullptr, create_module, &module))
        return -1;
    if (set(data, module, name, "subtitlepanel") ||
        set(data, module, description, "VLC Redesign media track controls") ||
        set(data, module, capability, "interface") || set(data, module, score, 0) ||
        set(data, module, open_callback, "open_panel", reinterpret_cast<void *>(open_panel)) ||
        set(data, module, close_callback, "close_panel", reinterpret_cast<void *>(close_panel)))
        return -1;
    return 0;
}
