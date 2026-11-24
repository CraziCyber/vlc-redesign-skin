// Windows interface module for the VLC Redesign skin in VLC 3.0.24.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define UNICODE
#include <windows.h>
#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string_view>
#include <string>
#include <utility>
#include <vector>

#include "add_subtitles.hpp"
#include "context_menu.hpp"
#include "hidden_controls.hpp"
#include "player_window.hpp"
#include "preferences_sidebar.hpp"
#include "subtitle_drop.hpp"
#include "vlc_input.hpp"
#include "subtitle_defaults.hpp"

// The VLC 3.0 module ABI used here is defined by vlc_common.h, vlc_interface.h,
// vlc_plugin.h, vlc_variables.h, vlc_playlist.h and vlc_aout.h.
struct AudioOutput;
template<class Parent>
struct VlcObject {
    const char *object_type;
    char *header;
    int flags;
    bool forced;
    void *libvlc;
    Parent *parent;
};
using VlcInterface = VlcObject<Playlist>;

struct PlaylistItem {
    InputItem *input;
    PlaylistItem **children, *parent;
    int child_count;
    unsigned played;
    int id;
    std::uint8_t flags;
};

struct PlaylistItems {
    int capacity, count;
    PlaylistItem **items;
};

struct Playlist {
    VlcObject<void> object;
    PlaylistItems items, current;
    int current_index;
    PlaylistItem root;
    PlaylistItem *playing;
};
static_assert(offsetof(Playlist, playing) == 120, "VLC 3.0.24 64-bit playlist layout");

extern "C" {
__declspec(dllimport) void playlist_Lock(Playlist *);
__declspec(dllimport) void playlist_Unlock(Playlist *);
__declspec(dllimport) AudioOutput *playlist_GetAout(Playlist *);
__declspec(dllimport) int aout_DevicesList(AudioOutput *, char ***, char ***);
__declspec(dllimport) char *aout_DeviceGet(AudioOutput *);
__declspec(dllimport) int aout_DeviceSet(AudioOutput *, const char *);
__declspec(dllimport) void var_Destroy(void *, const char *);
__declspec(dllimport) char *input_item_GetURI(InputItem *);
__declspec(dllimport) char *input_item_GetTitleFbName(InputItem *);
__declspec(dllimport) char *input_item_GetMeta(InputItem *, int);
__declspec(dllimport) char *vlc_uri2path(const char *);
__declspec(dllimport) char *vlc_path2uri(const char *, const char *);
__declspec(dllimport) const char *vlc_gettext(const char *);
__declspec(dllimport) int playlist_Status(Playlist *);
__declspec(dllimport) char *config_GetPsz(void *, const char *);
__declspec(dllimport) int vlc_actions_get_id(const char *);
__declspec(dllimport) Media *libvlc_media_new_location(LibVlc *, const char *);
__declspec(dllimport) int libvlc_media_parse_with_options(Media *, int, int);
__declspec(dllimport) int libvlc_media_get_parsed_status(Media *);
__declspec(dllimport) void libvlc_media_parse_stop(Media *);
}

namespace {
struct LocalSubtitle {
    std::string uri;
    std::wstring name;
};
using SubtitleRequest = std::shared_ptr<const LocalSubtitle>;

std::atomic<VlcInterface *> context_interface{nullptr};
std::atomic<HWND> context_target{nullptr};
std::mutex local_subtitles_mutex;
std::map<std::string, SubtitleRequest> local_subtitles;
std::atomic_bool has_local_subtitles{false};

constexpr wchar_t host_class[] = L"VlcRedesignSubtitleTracks";
constexpr wchar_t no_devices_label[] = L"No output devices available";
// Match the layout dimensions and section positions in build.py.
constexpr int panel_width = 368, video_height = 481, audio_height = 504, subtitles_height = 480;
constexpr int equalizer_width = 400, equalizer_height = 496;
constexpr LONG equalizer_offset = -42;
constexpr int list_x = 26, list_y = 96, list_width = panel_width - 52;
constexpr int row_height = 24, visible_tracks = 10, audio_rows = 4;
constexpr int devices_y = 280, device_rows = 6, refresh_y = 241, refresh_size = 32;
constexpr int preferences_margin = 18, preferences_height = 32;
constexpr int subtitle_body_height = subtitles_height - list_y - preferences_margin - preferences_height;
constexpr int external_header_height = 38, min_embedded_rows = 4;
constexpr LONG popup_right_margin = 16, popup_gap = 20;
constexpr int none_control = 1, track_control = 2, device_control = 3, refresh_control = 4;
constexpr int external_control = 5, add_control = 6;
constexpr DWORD dismiss_poll_ms = 15, dismiss_delay_ms = 50;
constexpr int no_track = -1;
constexpr char subtitle_description[] = "sub-description";
constexpr COLORREF dark_panel = RGB(15, 15, 15), light_panel = RGB(194, 193, 193);
constexpr COLORREF accent = RGB(211, 121, 24);

using AudioOutputRef = std::unique_ptr<AudioOutput, decltype(&vlc_object_release)>;

struct DroppedSubtitles {
    InputRef input;
    std::list<SubtitleRequest> files;
};
std::optional<DroppedSubtitles> dropped_subtitles;  // Guarded by local_subtitles_mutex.

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

std::vector<Track> media_tracks(Input *input, const char *variable) {
    const VlcChoices choices(input, variable);
    std::vector<Track> tracks;
    if (!choices.ids.list)
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
    explicit EmbeddedTracks(LibVlcRef &library) : library_(library) {}

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
        if (!library_)
            library_.reset(new_metadata_library());
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
    LibVlcRef &library_;
    MediaRef media_{nullptr, libvlc_media_release};
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

COLORREF panel_background(HWND window) {
    const HDC dc = GetDC(window);
    if (!dc)
        return CLR_INVALID;
    const COLORREF color = GetPixel(dc, 6, list_y - 4);
    ReleaseDC(window, dc);
    return color;
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

bool fullscreen_surface(HWND window) {
    RECT bounds{};
    return is_skin_window(window) && IsWindowVisible(window) && !GetWindow(window, GW_OWNER) &&
        GetClientRect(window, &bounds) && bounds.right >= 860 && bounds.bottom >= 420;
}

bool fullscreen_toolbar(HWND window) {
    RECT bounds{};
    return is_skin_window(window) && GetClientRect(window, &bounds) && bounds.right == 1000 && bounds.bottom == 56;
}

HWND fullscreen_owner(HWND toolbar) {
    if (fullscreen_toolbar(toolbar)) {
        const HWND owner = GetWindow(toolbar, GW_OWNER);
        if (fullscreen_surface(owner))
            return owner;
    }
    return nullptr;
}

HWND toolbar_for_fullscreen(HWND surface) {
    if (fullscreen_owner(surface))
        return surface;
    if (!fullscreen_surface(surface))
        return nullptr;
    struct Lookup { HWND surface, toolbar; } lookup{surface, nullptr};
    EnumThreadWindows(GetCurrentThreadId(), [](HWND window, LPARAM data) -> BOOL {
        auto &lookup = *reinterpret_cast<Lookup *>(data);
        if (GetWindow(window, GW_OWNER) != lookup.surface || !fullscreen_toolbar(window))
            return TRUE;
        lookup.toolbar = window;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&lookup));
    return lookup.toolbar;
}

std::optional<RECT> subtitles_button(HWND window) {
    RECT client;
    if (!is_skin_window(window) || IsIconic(window) || !GetClientRect(window, &client))
        return std::nullopt;
    // The main player and fullscreen controller share control_bar() in build.py.
    const bool fullscreen_controls = client.right == 1000 && client.bottom == 56;
    if (!fullscreen_controls && (!IsWindowVisible(window) || client.right < 860 || client.bottom < 420))
        return std::nullopt;
    const LONG top = client.bottom - (fullscreen_controls ? 44 : 43);
    return RECT{client.right - 142, top, client.right - 112, top + 32};
}

std::optional<POINT> panel_position(HWND toolbar, LONG width, LONG height, LONG offset = 0) {
    const auto button = subtitles_button(toolbar);
    POINT origin{};
    if (!button || !ClientToScreen(toolbar, &origin))
        return std::nullopt;

    POINT position{origin.x + button->right + 112 - popup_right_margin - width + offset,
                   origin.y + button->top - popup_gap - height};
    const POINT button_center{origin.x + (button->left + button->right) / 2 + offset,
                              origin.y + (button->top + button->bottom) / 2};
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (GetMonitorInfoW(MonitorFromPoint(button_center, MONITOR_DEFAULTTONEAREST), &monitor)) {
        const RECT &work = fullscreen_owner(toolbar) ? monitor.rcMonitor : monitor.rcWork;
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
        // The fullscreen controller belongs to the video surface. The skin panels
        // still belong to the same owner as the windowed player.
        const HWND player = fullscreen_owner(toolbar) ? context_target.load() : toolbar;
        if (const HWND owner = GetWindow(player, GW_OWNER))
            EnumThreadWindows(GetCurrentThreadId(), attach_popup_candidate, reinterpret_cast<LPARAM>(owner));
    }
}

enum class ToolbarPopup : WPARAM { none, tracks, equalizer };

bool equalizer_panel(HWND window) {
    RECT bounds{};
    return is_skin_window(window) && GetClientRect(window, &bounds) &&
        bounds.right == equalizer_width && bounds.bottom == equalizer_height;
}

struct FullscreenPopup {
    HWND window = nullptr;
    POINT previous_position{};
    bool was_topmost = false;
};

thread_local FullscreenPopup fullscreen_tracks, fullscreen_equalizer;

void restore_fullscreen_popup(FullscreenPopup &popup, bool tracks) {
    const auto saved = std::exchange(popup, {});
    if (GetWindowThreadProcessId(saved.window, nullptr) != GetCurrentThreadId() ||
        !(tracks ? panel_page(saved.window).has_value() : equalizer_panel(saved.window)))
        return;

    POINT position = saved.previous_position;
    if (tracks) {
        RECT bounds{};
        const HWND toolbar = context_target.load();
        if (GetClientRect(saved.window, &bounds))
            position = panel_position(toolbar, bounds.right, bounds.bottom).value_or(position);
        RemoveWindowSubclass(anchor.toolbar, follow_toolbar, anchor_subclass);
        anchor.toolbar = toolbar;
        if (toolbar)
            SetWindowSubclass(toolbar, follow_toolbar, anchor_subclass, 0);
    }
    SetWindowPos(saved.window, saved.was_topmost ? HWND_TOPMOST : HWND_NOTOPMOST,
                 position.x, position.y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOCOPYBITS);
    InvalidateRect(saved.window, nullptr, FALSE);
}

LRESULT CALLBACK follow_fullscreen(HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR) {
    if (message == WM_NCDESTROY || (message == WM_WINDOWPOSCHANGED &&
        (reinterpret_cast<const WINDOWPOS *>(lparam)->flags & SWP_HIDEWINDOW))) {
        RemoveWindowSubclass(window, follow_fullscreen, id);
        restore_fullscreen_popup(fullscreen_tracks, true);
        restore_fullscreen_popup(fullscreen_equalizer, false);
    }
    return DefSubclassProc(window, message, wparam, lparam);
}

void raise_fullscreen_popup(HWND toolbar, ToolbarPopup kind) {
    const HWND surface = fullscreen_owner(toolbar);
    if (!surface || (kind != ToolbarPopup::tracks && kind != ToolbarPopup::equalizer))
        return;
    const bool tracks = kind == ToolbarPopup::tracks;
    struct Lookup { bool tracks; HWND window; } lookup{tracks, nullptr};
    EnumThreadWindows(GetCurrentThreadId(), [](HWND window, LPARAM data) -> BOOL {
        auto &lookup = *reinterpret_cast<Lookup *>(data);
        if (!IsWindowVisible(window) || !(lookup.tracks ? panel_page(window).has_value() : equalizer_panel(window)))
            return TRUE;
        lookup.window = window;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&lookup));

    RECT bounds{};
    if (!lookup.window || !GetWindowRect(lookup.window, &bounds) ||
        !SetWindowSubclass(surface, follow_fullscreen, 1, 0))
        return;
    auto &saved = tracks ? fullscreen_tracks : fullscreen_equalizer;
    if (saved.window != lookup.window) {
        const auto styles = GetWindowLongPtrW(lookup.window, GWL_EXSTYLE);
        saved = {lookup.window, {bounds.left, bounds.top}, (styles & WS_EX_TOPMOST) != 0};
    }
    const auto position = panel_position(toolbar, bounds.right - bounds.left, bounds.bottom - bounds.top,
                                         tracks ? 0 : equalizer_offset).value_or(POINT{bounds.left, bounds.top});
    // Raise the panel above VLC's topmost fullscreen video without taking focus.
    SetWindowPos(lookup.window, HWND_TOPMOST, position.x, position.y, 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOCOPYBITS);
    InvalidateRect(lookup.window, nullptr, FALSE);
}

UINT fullscreen_popup_message() {
    static const UINT id = RegisterWindowMessageW(L"VlcRedesign.RaiseFullscreenPopup");
    return id;
}

bool redesign_is_selected(void *vlc);

void handle_toolbar_click(const MSG &message) {
    if (message.message != WM_LBUTTONDOWN && message.message != WM_LBUTTONDBLCLK && message.message != WM_LBUTTONUP)
        return;
    struct Press { HWND toolbar; ToolbarPopup popup; };
    thread_local Press pressed{};
    const POINT cursor{GET_X_LPARAM(message.lParam), GET_Y_LPARAM(message.lParam)};
    ToolbarPopup popup = ToolbarPopup::none;
    const HWND fullscreen = fullscreen_owner(message.hwnd);
    if (auto button = subtitles_button(message.hwnd)) {
        if (PtInRect(&*button, cursor)) {
            popup = ToolbarPopup::tracks;
        } else if (fullscreen) {
            OffsetRect(&*button, equalizer_offset, 0);
            if (PtInRect(&*button, cursor))
                popup = ToolbarPopup::equalizer;
        }
    }
    if (message.message == WM_LBUTTONUP) {
        const auto press = std::exchange(pressed, {});
        if (press.toolbar == message.hwnd && press.popup == popup && popup != ToolbarPopup::none) {
            if (popup == ToolbarPopup::tracks)
                anchor_panel_to(message.hwnd);
            if (fullscreen) {
                const auto interface = context_interface.load();
                if (interface && redesign_is_selected(interface->libvlc))
                    PostMessageW(message.hwnd, fullscreen_popup_message(), static_cast<WPARAM>(popup), 0);
            }
        }
    } else {
        pressed = {message.hwnd, popup};
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

void request_preferences(HWND window, LPARAM position) {
    if (preferences.timer)
        return;
    preferences.expires = GetTickCount64() + 5000;
    preferences.timer = SetTimer(nullptr, 0, 25, preferences_ready);
    press_skin_button(window, position);
}

void show_preferences(HWND panel, LPARAM position) {
    if (const HWND window = preferences_window()) {
        close_skin_panel(panel);
        finish_preferences_request(window);
        return;
    }
    request_preferences(panel, position);
    close_skin_panel(panel);
}

void open_context_preferences(HWND window, LPARAM position) {
    if (const HWND dialog = preferences_window()) {
        finish_preferences_request(dialog);
    } else {
        request_preferences(window, position);
    }
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

UINT context_menu_message() {
    static const UINT id = RegisterWindowMessageW(L"VlcRedesign.OpenContextMenu");
    return id;
}

UINT context_key_message() {
    static const UINT id = RegisterWindowMessageW(L"VlcRedesign.ContextMenuKey");
    return id;
}

enum class ContextKey { dismiss, navigate, hide_ui };

bool redesign_is_selected(void *vlc) {
    std::unique_ptr<char, decltype(&libvlc_free)> skin(config_GetPsz(vlc, "skins2-last"), libvlc_free);
    if (!skin)
        return false;
    const std::string_view path(skin.get());
    const auto name = path.substr(path.find_last_of("/\\") + 1);
    return name == "VLC-Redesign.vlt";
}

bool context_player(HWND window) {
    if (!is_skin_window(window) || !GetWindow(window, GW_OWNER) || !IsWindowVisible(window) || IsIconic(window))
        return false;
    RECT bounds{};
    return GetClientRect(window, &bounds) &&
        ((bounds.right == 391 && bounds.bottom == 339) || subtitles_button(window).has_value());
}

bool video_window(HWND window) {
    wchar_t name[64];
    return GetClassNameW(window, name, 64) && wcsncmp(name, L"VLC video ", 10) == 0;
}

bool subtitle_drop_window(HWND window) {
    return context_player(window) || fullscreen_surface(window) || fullscreen_owner(window) ||
        panel_page(window) == Page::subtitles;
}

BOOL CALLBACK find_context_player(HWND window, LPARAM result) {
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    if (process != GetCurrentProcessId() || !context_player(window))
        return TRUE;
    RECT bounds{};
    if (GetClientRect(window, &bounds) && bounds.bottom == 56)
        return TRUE;
    *reinterpret_cast<HWND *>(result) = window;
    return FALSE;
}

HWND controls_player(HWND window) {
    if (fullscreen_toolbar(window)) {
        const HWND main = context_target.load();
        return context_player(main) ? main : nullptr;
    }
    return window;
}

HWND add_subtitles_owner() {
    if (const HWND fullscreen = fullscreen_owner(anchor.toolbar))
        return fullscreen;
    const HWND player = controls_player(anchor.toolbar);
    return context_player(player) ? player : context_target.load();
}

UINT add_subtitles_message() {
    static const UINT id = RegisterWindowMessageW(L"VlcRedesign.AddSubtitles");
    return id;
}

UINT show_saved_subtitles_message() {
    static const UINT id = RegisterWindowMessageW(L"VlcRedesign.ShowSavedSubtitles");
    return id;
}

std::wstring local_media_path(const char *uri) {
    using Text = std::unique_ptr<char, decltype(&libvlc_free)>;
    const Text path(uri && std::string_view(uri).substr(0, 7) == "file://" ? vlc_uri2path(uri) : nullptr, libvlc_free);
    return from_utf8(path.get());
}

std::string to_utf8(std::wstring_view text) {
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                                          static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (!length)
        return {};
    std::string utf8(length, '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                             utf8.data(), length, nullptr, nullptr))
        return {};
    return utf8;
}

std::string file_uri(std::wstring_view path) {
    const auto utf8 = to_utf8(path);
    if (utf8.empty())
        return {};
    const std::unique_ptr<char, decltype(&libvlc_free)> uri(vlc_path2uri(utf8.c_str(), nullptr), libvlc_free);
    return uri ? uri.get() : std::string{};
}

std::string local_media_key(Input *input) {
    auto *item = input ? input_GetItem(input) : nullptr;
    if (!item)
        return {};
    const std::unique_ptr<char, decltype(&libvlc_free)> uri(input_item_GetURI(item), libvlc_free);
    if (!uri)
        return {};
    const auto path = local_media_path(uri.get());
    return path.empty() ? uri.get() : file_uri(path);
}

SubtitleRequest local_subtitle(std::wstring_view path, std::wstring_view label = {}) {
    auto uri = file_uri(path);
    if (uri.empty())
        return {};
    auto name = label;
    if (name.empty()) {
        name = path.substr(path.find_last_of(L"/\\") + 1);
        if (const auto extension = name.find_last_of(L'.'); extension != 0)
            name = name.substr(0, extension);
    }
    return std::make_shared<LocalSubtitle>(LocalSubtitle{std::move(uri), std::wstring(name)});
}

InputRef drop_input() {
    const auto interface = context_interface.load();
    if (!interface || !redesign_is_selected(interface->libvlc))
        return InputRef(nullptr, vlc_object_release);
    return InputRef(playlist_CurrentInput(interface->parent), vlc_object_release);
}

bool can_drop_subtitles() {
    return active_playback(drop_input().get());
}

bool queue_dropped_subtitles(const std::vector<std::wstring> &paths) {
    auto input = drop_input();
    if (paths.empty() || !active_playback(input.get()))
        return false;
    auto media = local_media_key(input.get());
    if (media.empty())
        return false;

    std::list<SubtitleRequest> files;
    for (const auto &path : paths) {
        auto request = local_subtitle(path);
        if (!request)
            return false;
        files.push_back(std::move(request));
    }
    const std::lock_guard<std::mutex> lock(local_subtitles_mutex);
    local_subtitles.insert_or_assign(std::move(media), files.back());
    if (!dropped_subtitles || dropped_subtitles->input.get() != input.get())
        dropped_subtitles.emplace(DroppedSubtitles{std::move(input), {}});
    dropped_subtitles->files.splice(dropped_subtitles->files.end(), files);
    has_local_subtitles.store(true);
    return true;
}

bool save_local_subtitles(const std::vector<add_subtitles::LocalAttachment> &attachments, bool show_tracks) {
    if (attachments.empty())
        return false;
    std::map<std::string, SubtitleRequest> updates;
    for (const auto &attachment : attachments) {
        auto media = attachment.media.path.empty() ? attachment.media.uri : file_uri(attachment.media.path);
        auto subtitle = local_subtitle(attachment.subtitle, attachment.label);
        if (media.empty() || !subtitle)
            return false;
        updates.insert_or_assign(std::move(media), std::move(subtitle));
    }
    {
        const std::lock_guard<std::mutex> lock(local_subtitles_mutex);
        for (const auto &update : updates)
            local_subtitles.erase(update.first);
        local_subtitles.merge(updates);
    }
    has_local_subtitles.store(true);
    if (show_tracks) {
        if (const HWND toolbar = anchor.toolbar ? anchor.toolbar : context_target.load())
            PostMessageW(toolbar, show_saved_subtitles_message(), 0, 0);
    }
    return true;
}

add_subtitles::Media current_subtitle_media() {
    const auto interface = context_interface.load();
    InputRef input(interface ? playlist_CurrentInput(interface->parent) : nullptr, vlc_object_release);
    const auto item = input ? input_GetItem(input.get()) : nullptr;
    if (!item)
        return {};

    using Text = std::unique_ptr<char, decltype(&libvlc_free)>;
    const Text uri(input_item_GetURI(item), libvlc_free);
    auto local_path = local_media_path(uri.get());
    const std::string source = uri ? uri.get() : "";
    if (!local_path.empty()) {
        const auto filename = local_path.substr(local_path.find_last_of(L"/\\") + 1);
        const auto extension = filename.find_last_of(L'.');
        return {std::move(local_path), filename.substr(0, extension == 0 ? std::wstring::npos : extension), source};
    }
    const Text title(input_item_GetTitleFbName(item), libvlc_free);
    auto display = from_utf8(title.get());
    if (display.find(L"://") != std::wstring::npos)
        display.clear();
    return {{}, std::move(display), source};
}

std::vector<add_subtitles::Media> current_playlist_tracks() {
    const auto interface = context_interface.load();
    Playlist *playlist = interface ? interface->parent : nullptr;
    if (!playlist)
        return {};

    playlist_Lock(playlist);
    const std::unique_ptr<Playlist, decltype(&playlist_Unlock)> locked(playlist, playlist_Unlock);
    std::vector<add_subtitles::Media> tracks;
    std::vector<const PlaylistItem *> remaining{playlist->playing};
    while (!remaining.empty()) {
        const auto *item = remaining.back();
        remaining.pop_back();
        if (!item)
            continue;
        if (item->child_count >= 0) {
            for (int i = item->child_count; i > 0; --i)
                remaining.push_back(item->children[i - 1]);
            continue;
        }
        if (!item->input)
            continue;

        using Text = std::unique_ptr<char, decltype(&libvlc_free)>;
        constexpr int artist_metadata = 1;  // vlc_meta_Artist in vlc_meta.h.
        const Text title(input_item_GetTitleFbName(item->input), libvlc_free);
        const Text artist(input_item_GetMeta(item->input, artist_metadata), libvlc_free);
        auto label = from_utf8(title.get());
        const auto name = from_utf8(artist.get());
        if (label.empty())
            label = L"Untitled track";
        if (!name.empty() && name != label)
            label += L" \u2014 " + name;
        const Text uri(input_item_GetURI(item->input), libvlc_free);
        tracks.push_back({local_media_path(uri.get()), std::move(label), uri ? uri.get() : ""});
    }
    return tracks;
}

void open_add_subtitles(HWND panel) {
    const auto interface = context_interface.load();
    if (!interface || !redesign_is_selected(interface->libvlc) || !IsWindowVisible(panel) ||
        panel_page(panel) != Page::subtitles)
        return;
    const HWND player = add_subtitles_owner();
    const bool light_theme = panel_background(panel) == light_panel;
    close_skin_panel(panel);
    add_subtitles::show(player, light_theme, current_subtitle_media, current_playlist_tracks, save_local_subtitles);
}

int playlist_button(const MSG &message) {
    RECT bounds{};
    if (!context_player(message.hwnd) || !GetClientRect(message.hwnd, &bounds) || bounds.bottom < 420)
        return 0;
    const POINT position{GET_X_LPARAM(message.lParam), GET_Y_LPARAM(message.lParam)};
    // The sidebar toggle in top_bar() and the last button in control_bar().
    const RECT top{89, 6, 117, 34};
    const RECT bottom{bounds.right - 50, bounds.bottom - 43, bounds.right - 20, bounds.bottom - 11};
    return PtInRect(&top, position) ? 1 : PtInRect(&bottom, position) ? 2 : 0;
}

bool handle_playlist_button(const MSG &message) {
    struct Press { HWND window; int button; };
    thread_local Press pressed{};
    if (message.message == WM_LBUTTONDOWN || message.message == WM_LBUTTONDBLCLK) {
        const int button = playlist_button(message);
        VlcInterface *interface = context_interface.load();
        pressed = button && interface && redesign_is_selected(interface->libvlc) ? Press{message.hwnd, button} : Press{};
    } else if (message.message == WM_LBUTTONUP) {
        const Press press = std::exchange(pressed, {});
        if (press.window && press.window == message.hwnd && press.button == playlist_button(message)) {
            player_window::toggle_playlist(message.hwnd, message.lParam, true);
            hidden_controls::playlist_layout_changed(message.hwnd);
            return true;
        }
    }
    return false;
}

void run_context_action(HWND window, HWND surface, const char *action, context_menu::SkinCommand command) {
    VlcInterface *interface = context_interface.load();
    if (!interface || !context_player(window))
        return;
    if (action) {
        const std::string key = std::string("key-") + action;
        if (const int id = vlc_actions_get_id(key.c_str()))
            var_Set(interface->libvlc, "key-action", VlcValue{id});
        return;
    }
    using Command = context_menu::SkinCommand;
    if (command == Command::toggle_ui) {
        hidden_controls::toggle(controls_player(window));
        return;
    }
    const LPARAM position = MAKELPARAM(static_cast<WORD>(context_menu::command_x), static_cast<WORD>(command));
    if (command == Command::toggle_playlist) {
        if (const HWND player = controls_player(window)) {
            player_window::toggle_playlist(player, position, false);
            hidden_controls::playlist_layout_changed(player);
        }
    } else if (command == Command::settings) {
        open_context_preferences(window, position);
    } else if (command != Command::none) {
        const bool tracks = command == Command::video_tracks || command == Command::audio_tracks || command == Command::subtitles;
        const bool equalizer = command == Command::eq;
        const HWND toolbar = (tracks || equalizer) ? toolbar_for_fullscreen(surface) : nullptr;
        if (tracks)
            anchor_panel_to(toolbar ? toolbar : window);
        press_skin_button(window, position);
        if (toolbar)
            raise_fullscreen_popup(toolbar, tracks ? ToolbarPopup::tracks : ToolbarPopup::equalizer);
    }
}

bool open_context_menu(HWND window, HWND surface, VlcInterface &interface, POINT location, bool keyboard) {
    PanelWindow tracks;
    EnumWindows(find_panel, reinterpret_cast<LPARAM>(&tracks));
    if (tracks.hwnd)
        close_skin_panel(tracks.hwnd);
    InputRef input(playlist_CurrentInput(interface.parent), vlc_object_release);
    VlcValue recording{};
    const bool is_recording = input && var_Get(input.get(), "record", &recording) == 0 && recording.boolean;
    constexpr int playlist_running = 1;  // VLC 3.0's playlist_status_t.
    return context_menu::show({window, surface, interface.libvlc, playlist_Status(interface.parent) == playlist_running,
                               bool(input), is_recording, hidden_controls::hidden(controls_player(window)),
                               run_context_action},
                              location, keyboard);
}

bool handle_context_click(const MSG &message) {
    VlcInterface *interface = context_interface.load();
    if (!interface)
        return false;
    if (message.message == context_menu_message()) {
        const POINT location{GET_X_LPARAM(message.lParam), GET_Y_LPARAM(message.lParam)};
        const HWND surface = reinterpret_cast<HWND>(message.wParam);
        if (context_player(message.hwnd) && redesign_is_selected(interface->libvlc))
            open_context_menu(message.hwnd, IsWindowVisible(surface) ? surface : message.hwnd, *interface, location, false);
        return true;
    }
    if (message.message == context_key_message()) {
        const auto key = static_cast<ContextKey>(message.lParam);
        context_menu::handle_key(message.wParam, key == ContextKey::navigate);
        if (key == ContextKey::hide_ui)
            run_context_action(message.hwnd, message.hwnd, nullptr, context_menu::SkinCommand::toggle_ui);
        return true;
    }
    const bool keyboard = message.message == WM_KEYDOWN &&
        (message.wParam == VK_APPS || (message.wParam == VK_F10 && GetKeyState(VK_SHIFT) < 0));
    if (!keyboard && message.message != WM_RBUTTONDOWN && message.message != WM_RBUTTONUP &&
        message.message != WM_RBUTTONDBLCLK && message.message != WM_CONTEXTMENU)
        return false;
    HWND surface = GetAncestor(message.hwnd, GA_ROOT);
    if (const HWND player = hidden_controls::player_for_overlay(surface))
        surface = player;
    const HWND window = context_player(surface) ? surface : fullscreen_surface(surface) ? context_target.load() : nullptr;
    if (!context_player(window) || !redesign_is_selected(interface->libvlc))
        return false;
    if (message.message == WM_RBUTTONDOWN || message.message == WM_RBUTTONDBLCLK)
        return true;
    POINT location{GET_X_LPARAM(message.lParam), GET_Y_LPARAM(message.lParam)};
    if (keyboard || (message.message == WM_CONTEXTMENU && message.lParam == -1)) {
        RECT bounds{};
        GetWindowRect(window, &bounds);
        location = {bounds.left + 24, bounds.top + 40};
    } else if (message.message != WM_CONTEXTMENU) {
        ClientToScreen(message.hwnd, &location);
    }
    return open_context_menu(window, surface, *interface, location, keyboard || message.lParam == -1);
}

bool is_hide_ui_shortcut(const MSG &message) {
    if (message.message != WM_KEYDOWN || GetKeyState(VK_CONTROL) >= 0 || GetKeyState(VK_MENU) < 0)
        return false;
    const SHORT scan = VkKeyScanW(L'\\');
    return scan != -1 && message.wParam == LOBYTE(scan) &&
           bool(HIBYTE(scan) & 1) == (GetKeyState(VK_SHIFT) < 0);
}

bool handle_hide_ui_shortcut(const MSG &message) {
    if (!is_hide_ui_shortcut(message))
        return false;
    VlcInterface *interface = context_interface.load();
    const HWND window = context_player(message.hwnd) ? message.hwnd :
        fullscreen_surface(message.hwnd) ? context_target.load() : nullptr;
    if (!interface || !context_player(window) || !redesign_is_selected(interface->libvlc) ||
        !context_menu::hide_ui_shortcut_available(interface->libvlc))
        return false;
    if (!(message.lParam & (1L << 30)))
        run_context_action(window, window, nullptr, context_menu::SkinCommand::toggle_ui);
    return true;
}

bool handle_playback_sidebar(const MSG &message) {
    if (message.message == WM_LBUTTONDOWN || message.message == WM_LBUTTONDBLCLK ||
        message.message == WM_LBUTTONUP) {
        subtitle_preferences::Player player{};
        const HWND surface = fullscreen_owner(message.hwnd);
        if (surface || context_player(message.hwnd)) {
            if (auto button = subtitles_button(message.hwnd)) {
                OffsetRect(&*button, 42, 0);
                const POINT cursor{GET_X_LPARAM(message.lParam), GET_Y_LPARAM(message.lParam)};
                const auto interface = context_interface.load();
                if (PtInRect(&*button, cursor) && interface && redesign_is_selected(interface->libvlc))
                    player = {interface->libvlc, interface->parent};
            }
        }
        if (preferences_sidebar::handle_gear(message, player, surface))
            return true;
    }
    return preferences_sidebar::handle_message(message);
}

bool handle_video_context_event(const MSG &message) {
    const bool keyboard = message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN;
    const bool right_click = message.message == WM_RBUTTONDOWN || message.message == WM_RBUTTONDBLCLK ||
                             message.message == WM_RBUTTONUP;
    if ((!keyboard && !right_click) || !video_window(message.hwnd))
        return false;
    const HWND target = context_target.load();
    VlcInterface *interface = context_interface.load();
    if (!target || !interface)
        return false;
    if (keyboard) {
        if (is_hide_ui_shortcut(message) && redesign_is_selected(interface->libvlc) &&
            context_menu::hide_ui_shortcut_available(interface->libvlc)) {
            return (message.lParam & (1L << 30)) ||
                PostMessageW(target, context_key_message(), message.wParam, static_cast<LPARAM>(ContextKey::hide_ui));
        }
        if (context_menu::is_open()) {
            const bool navigate = context_menu::is_navigation_key(message.wParam);
            const auto event = navigate ? ContextKey::navigate : ContextKey::dismiss;
            const bool queued = PostMessageW(target, context_key_message(), message.wParam, static_cast<LPARAM>(event));
            return queued && navigate;
        }
        return false;
    }
    if (!redesign_is_selected(interface->libvlc))
        return false;
    if (message.message == WM_RBUTTONUP) {
        POINT location{GET_X_LPARAM(message.lParam), GET_Y_LPARAM(message.lParam)};
        ClientToScreen(message.hwnd, &location);
        const auto surface = reinterpret_cast<WPARAM>(GetAncestor(message.hwnd, GA_ROOT));
        return PostMessageW(target, context_menu_message(), surface, MAKELPARAM(location.x, location.y));
    }
    return true;
}

LRESULT CALLBACK video_events_from_queue(int code, WPARAM removal, LPARAM data) {
    if (code == HC_ACTION && removal == PM_REMOVE) {
        auto &message = *reinterpret_cast<MSG *>(data);
        if (preferences_sidebar::handle_video_event(message) || handle_video_context_event(message))
            message.message = WM_NULL;
    }
    return CallNextHookEx(nullptr, code, removal, data);
}

LRESULT CALLBACK skin_events_from_queue(int code, WPARAM removal, LPARAM data) {
    if (code == HC_ACTION && removal == PM_REMOVE) {
        auto &message = *reinterpret_cast<MSG *>(data);
        if (message.message == fullscreen_popup_message()) {
            message.message = WM_NULL;
            raise_fullscreen_popup(message.hwnd, static_cast<ToolbarPopup>(message.wParam));
        } else if (message.message == dismiss_message()) {
            message.message = WM_NULL;
            close_skin_panel(message.hwnd);
        } else if (message.message == add_subtitles_message()) {
            message.message = WM_NULL;
            open_add_subtitles(message.hwnd);
        } else if (message.message == show_saved_subtitles_message()) {
            message.message = WM_NULL;
            const auto interface = context_interface.load();
            if (interface && redesign_is_selected(interface->libvlc) && !IsWindowVisible(anchor.panel))
                run_context_action(context_target.load(), message.hwnd, nullptr, context_menu::SkinCommand::subtitles);
        } else if (message.message == subtitle_drop::install_message()) {
            message.message = WM_NULL;
            const auto interface = context_interface.load();
            if (interface && redesign_is_selected(interface->libvlc) && subtitle_drop_window(message.hwnd))
                subtitle_drop::install(message.hwnd, can_drop_subtitles, queue_dropped_subtitles);
        } else if (preferences_sidebar::owns_input_target(message.hwnd)) {
            if (preferences_sidebar::handle_message(message))
                message.message = WM_NULL;
        } else if (add_subtitles::handle_key(message) || context_menu::handle_message(message) ||
                   handle_playback_sidebar(message) || handle_hide_ui_shortcut(message) || handle_context_click(message) ||
                   handle_playlist_button(message)) {
            message.message = WM_NULL;
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
    struct LoadedSubtitle {
        SubtitleRequest file;
        Track track;
    };

    struct PendingSubtitle {
        SubtitleRequest file;
        std::vector<Track> previous_tracks;
        std::unique_ptr<char, decltype(&libvlc_free)> previous_description;
        ULONGLONG deadline;
    };

    Playlist *playlist_;
    HANDLE stop_ = nullptr, thread_ = nullptr;
    HINSTANCE module_ = nullptr;
    HHOOK skin_hook_ = nullptr;
    DWORD skin_thread_ = 0;
    std::vector<std::pair<DWORD, HHOOK>> video_hooks_;
    HWND host_ = nullptr, list_ = nullptr, none_ = nullptr;
    HWND device_list_ = nullptr, external_list_ = nullptr, add_button_ = nullptr;
    HFONT font_ = nullptr;
    Page page_ = Page::subtitles;
    InputRef input_{nullptr, vlc_object_release};
    InputRef local_input_{nullptr, vlc_object_release};
    std::string local_media_key_;
    SubtitleRequest attempted_local_subtitle_;
    std::optional<PendingSubtitle> pending_local_subtitle_;
    std::vector<LoadedSubtitle> loaded_local_subtitles_;
    LibVlcRef metadata_library_{nullptr, libvlc_release};
    EmbeddedTracks embedded_{metadata_library_};
    subtitle_defaults::AutoSelect subtitle_defaults_;
    std::vector<Track> tracks_, external_tracks_;
    int embedded_rows_ = min_embedded_rows;
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
        if (page_ == Page::audio)
            return audio_rows;
        return page_ == Page::subtitles ? embedded_rows_ : visible_tracks;
    }

    int external_header_y() const {
        return (embedded_rows_ + 1) * row_height + 4;
    }

    int external_rows() const {
        return (subtitle_body_height - external_header_y() - external_header_height) / row_height;
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
        context_menu::load_artwork(module_);

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
        context_target.store(nullptr);
        for (const auto &hook : video_hooks_)
            UnhookWindowsHookEx(hook.second);
        context_menu::unload_artwork();
        if (host_)
            DestroyWindow(host_);
        clear_pending_subtitle();
        {
            const std::lock_guard<std::mutex> lock(local_subtitles_mutex);
            dropped_subtitles.reset();
        }
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

    void watch_video_windows() {
        HWND window = nullptr;
        if (skin_thread_)
            EnumThreadWindows(skin_thread_, find_context_player, reinterpret_cast<LPARAM>(&window));
        context_target.store(window);
        struct Threads {
            DWORD process, skin_thread;
            std::vector<DWORD> ids;
        } threads{GetCurrentProcessId(), skin_thread_, {}};
        const auto track_video = [](HWND child, LPARAM data) -> BOOL {
            auto &threads = *reinterpret_cast<Threads *>(data);
            DWORD process = 0;
            const DWORD thread = GetWindowThreadProcessId(child, &process);
            if (process == threads.process && thread != threads.skin_thread && video_window(child) &&
                std::find(threads.ids.begin(), threads.ids.end(), thread) == threads.ids.end())
                threads.ids.push_back(thread);
            return TRUE;
        };
        struct Search {
            Threads &threads;
            WNDENUMPROC track;
        } search{threads, track_video};
        if (window) {
            EnumWindows([](HWND top, LPARAM data) -> BOOL {
                auto &search = *reinterpret_cast<Search *>(data);
                DWORD process = 0;
                const auto thread = GetWindowThreadProcessId(top, &process);
                if (process == search.threads.process && IsWindowVisible(top)) {
                    if (thread == search.threads.skin_thread && subtitle_drop_window(top))
                        subtitle_drop::request(top);
                    const auto flags = reinterpret_cast<LPARAM>(&search.threads);
                    search.track(top, flags);
                    EnumChildWindows(top, search.track, flags);
                }
                return TRUE;
            }, reinterpret_cast<LPARAM>(&search));
        }
        for (auto it = video_hooks_.begin(); it != video_hooks_.end();) {
            if (std::find(threads.ids.begin(), threads.ids.end(), it->first) != threads.ids.end()) {
                ++it;
            } else {
                UnhookWindowsHookEx(it->second);
                it = video_hooks_.erase(it);
            }
        }
        for (const DWORD id : threads.ids) {
            const auto attached = std::find_if(video_hooks_.begin(), video_hooks_.end(),
                                              [id](const auto &hook) { return hook.first == id; });
            if (attached == video_hooks_.end())
                if (const HHOOK hook = SetWindowsHookExW(WH_GETMESSAGE, video_events_from_queue, module_, id))
                    video_hooks_.emplace_back(id, hook);
        }
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
        if (const auto interface = context_interface.load()) {
            const subtitle_preferences::Player player{interface->libvlc, interface->parent};
            subtitle_preferences::reset_duration_on_media_change(player);
            if (redesign_is_selected(player.vlc))
                subtitle_defaults_.refresh(player, metadata_library_);
        }
        refresh_local_subtitle();
        PanelWindow window;
        EnumWindows(find_panel, reinterpret_cast<LPARAM>(&window));
        if (window.hwnd)
            watch_skin_thread(window.hwnd);
        else if (!skin_hook_) {
            HWND toolbar = nullptr;
            EnumWindows(find_context_player, reinterpret_cast<LPARAM>(&toolbar));
            if (toolbar)
                watch_skin_thread(toolbar);
        }
        watch_video_windows();
        if (!window.hwnd || !IsWindowVisible(window.hwnd)) {
            if (host_)
                DestroyWindow(host_);
            if (input_)
                embedded_.set_input(nullptr);
            input_.reset();
            tracks_.clear();
            external_tracks_.clear();
            devices_.clear();
            return;
        }
        if (host_ && (GetParent(host_) != window.hwnd || window.page != page_))
            DestroyWindow(host_);
        page_ = window.page;

        const COLORREF color = panel_background(window.hwnd);
        if ((color == light_panel || color == dark_panel) && color != background_) {
            background_ = color;
            if (host_)
                RedrawWindow(host_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
        }

        const bool new_host = !host_;
        if (new_host) {
            const bool audio = page_ == Page::audio;
            const bool subtitles = page_ == Page::subtitles;
            const int x = subtitles ? 0 : list_x;
            const int width = subtitles ? panel_width : audio ? panel_width - list_x - 18 : list_width;
            const int height = subtitles ? subtitle_body_height : audio ? devices_y - list_y + device_rows * row_height
                                                                       : row_height * (visible_tracks + 1);
            if (!CreateWindowExW(WS_EX_NOPARENTNOTIFY, host_class, L"Media Tracks",
                                 WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, x, list_y,
                                 width, height, window.hwnd, nullptr, module_, this))
                return;
        }

        refresh_tracks(new_host);
        if (page_ == Page::audio)
            refresh_devices(new_host);
    }

    void clear_pending_subtitle() {
        if (!pending_local_subtitle_)
            return;
        VlcValue description{};
        description.string = pending_local_subtitle_->previous_description.get();
        if (description.string)
            var_Set(local_input_.get(), subtitle_description, description);
        var_Destroy(local_input_.get(), subtitle_description);
        pending_local_subtitle_.reset();
    }

    void finish_loading_subtitle(const std::vector<Track> &tracks) {
        const auto &pending = *pending_local_subtitle_;
        const auto &name = pending.file->name;
        const auto language_prefix = name + L" - [";
        const auto added = std::find_if(tracks.begin(), tracks.end(), [&](const Track &track) {
            return (track.label == name || track.label.compare(0, language_prefix.size(), language_prefix) == 0) &&
                std::none_of(pending.previous_tracks.begin(), pending.previous_tracks.end(),
                             [&](const Track &previous) { return previous.id == track.id; });
        });
        if (added != tracks.end()) {
            auto label = added->label;
            if (label != name)
                label.erase(name.size(), 2);
            loaded_local_subtitles_.push_back({pending.file, {added->id, std::move(label)}});
            clear_pending_subtitle();
        } else if (GetTickCount64() >= pending.deadline) {
            clear_pending_subtitle();
        }
    }

    void mark_subtitle_requested(const SubtitleRequest &subtitle, bool dropped) {
        attempted_local_subtitle_ = subtitle;
        if (!dropped)
            return;
        const std::lock_guard<std::mutex> lock(local_subtitles_mutex);
        if (dropped_subtitles && dropped_subtitles->files.front() == subtitle) {
            dropped_subtitles->files.pop_front();
            if (dropped_subtitles->files.empty())
                dropped_subtitles.reset();
        }
    }

    void refresh_local_subtitle() {
        if (!has_local_subtitles.load())
            return;
        InputRef current(playlist_CurrentInput(playlist_), vlc_object_release);
        if (current.get() != local_input_.get()) {
            clear_pending_subtitle();
            local_input_ = std::move(current);
            local_media_key_ = local_media_key(local_input_.get());
            attempted_local_subtitle_.reset();
            loaded_local_subtitles_.clear();
        }

        SubtitleRequest subtitle;
        bool dropped = false;
        {
            const std::lock_guard<std::mutex> lock(local_subtitles_mutex);
            if (dropped_subtitles && dropped_subtitles->input.get() != local_input_.get()) {
                // A drop can arrive while the worker is switching to the next input.
                const InputRef live(playlist_CurrentInput(playlist_), vlc_object_release);
                if (live.get() != local_input_.get())
                    return;
                dropped_subtitles.reset();
            }
            if (dropped_subtitles) {
                dropped = true;
                subtitle = dropped_subtitles->files.front();
            } else if (const auto chosen = local_subtitles.find(local_media_key_); chosen != local_subtitles.end()) {
                subtitle = chosen->second;
            }
        }
        if (!subtitle || (!pending_local_subtitle_ && !dropped && subtitle == attempted_local_subtitle_))
            return;

        auto tracks = media_tracks(local_input_.get(), "spu-es");
        if (pending_local_subtitle_) {
            finish_loading_subtitle(tracks);
            if (pending_local_subtitle_ || (!dropped && subtitle == attempted_local_subtitle_))
                return;
        }

        const auto loaded = std::find_if(loaded_local_subtitles_.begin(), loaded_local_subtitles_.end(),
                                         [&](const LoadedSubtitle &entry) { return entry.file->uri == subtitle->uri; });
        if (loaded != loaded_local_subtitles_.end()) {
            const auto id = loaded->track.id;
            if (std::any_of(tracks.begin(), tracks.end(), [&](const Track &track) { return track.id == id; })) {
                if (var_Set(local_input_.get(), "spu-es", VlcValue{id}) == 0)
                    mark_subtitle_requested(subtitle, dropped);
                return;
            }
            loaded_local_subtitles_.erase(loaded);
        }

        // Wait until VLC finishes loading the media and its automatic subtitle files.
        if (!active_playback(local_input_.get()))
            return;

        // The text subtitle demuxer inherits this description. It lets the next
        // refresh associate the new VLC track ID with the chosen file.
        constexpr int string_variable = 0x40;
        auto name = to_utf8(subtitle->name);
        if (name.empty() || var_Create(local_input_.get(), subtitle_description, string_variable))
            return;
        VlcValue previous{}, description{};
        var_Get(local_input_.get(), subtitle_description, &previous);
        pending_local_subtitle_.emplace(PendingSubtitle{
            subtitle, std::move(tracks), {previous.string, libvlc_free}, GetTickCount64() + 10000});
        description.string = name.data();
        constexpr int subtitle_type = 0;  // SLAVE_TYPE_SPU in VLC 3.0.24.
        if (var_Set(local_input_.get(), subtitle_description, description) ||
            input_Control(local_input_.get(), input_add_slave, subtitle_type, subtitle->uri.c_str(), true, dropped, true)) {
            clear_pending_subtitle();
        } else {
            mark_subtitle_requested(subtitle, dropped);
        }
    }

    template <typename Item, typename Id>
    void update_list(HWND list, const std::vector<Item> &items, const Id &selected,
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
        if (list == external_list_ && index >= 0)
            top = std::clamp<LRESULT>(top, index - rows + 1, index);
        SendMessageW(list, LB_SETTOPINDEX, static_cast<WPARAM>(std::clamp<LRESULT>(top, 0, last_top)), 0);
        SendMessageW(list, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(list, nullptr, TRUE);
    }

    void layout_subtitle_tracks() {
        MoveWindow(list_, list_x, row_height, list_width, embedded_rows_ * row_height, TRUE);
        const int divider = external_header_y();
        MoveWindow(add_button_, panel_width - 52, divider + 4, 32, 32, TRUE);
        MoveWindow(external_list_, list_x, divider + external_header_height,
                   list_width, external_rows() * row_height, TRUE);
        InvalidateRect(host_, nullptr, FALSE);
    }

    void refresh_tracks(bool new_host) {
        InputRef current(playlist_CurrentInput(playlist_), vlc_object_release);
        const bool new_input = current.get() != input_.get();
        if (new_input)
            embedded_.set_input(current.get());
        const char *variable = track_variable();
        auto entries = media_tracks(current.get(), variable);
        EnableWindow(none_, !entries.empty());
        std::vector<Track> external;
        if (page_ == Page::subtitles && current.get() == local_input_.get()) {
            entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const Track &track) {
                const auto loaded = std::find_if(loaded_local_subtitles_.begin(), loaded_local_subtitles_.end(),
                    [&](const LoadedSubtitle &entry) { return entry.track.id == track.id; });
                if (loaded == loaded_local_subtitles_.end())
                    return false;
                external.push_back(loaded->track);
                return true;
            }), entries.end());
        }
        embedded_.filter(entries, page_);
        VlcValue selection{no_track};
        if (current)
            var_Get(current.get(), variable, &selection);

        const bool reset = new_host || new_input;
        const bool selection_changed = selected_track_ != selection.integer;
        const bool reload = reset || entries != tracks_;
        const bool reload_external = external_list_ && (reset || external != external_tracks_);
        selected_track_ = selection.integer;

        bool layout_changed = false;
        if (external_list_) {
            const auto rows = std::clamp(static_cast<int>(entries.size()), min_embedded_rows, visible_tracks);
            layout_changed = new_host || rows != embedded_rows_;
            if (layout_changed) {
                embedded_rows_ = rows;
                layout_subtitle_tracks();
            }
        }
        if (reload || selection_changed) {
            const auto top = reset ? 0 : SendMessageW(list_, LB_GETTOPINDEX, 0, 0);
            tracks_ = std::move(entries);
            update_list(list_, tracks_, selected_track_, reload, top, track_rows());
            InvalidateRect(none_, nullptr, TRUE);
        }
        if (external_list_ && (reload_external || selection_changed || layout_changed)) {
            const auto top = reset ? 0 : SendMessageW(external_list_, LB_GETTOPINDEX, 0, 0);
            external_tracks_ = std::move(external);
            update_list(external_list_, external_tracks_, selected_track_, reload_external, top, external_rows());
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
        } else if (item.CtlID == track_control || item.CtlID == external_control) {
            const auto &tracks = item.CtlID == track_control ? tracks_ : external_tracks_;
            if (item.itemID < tracks.size()) {
                const auto &track = tracks[item.itemID];
                label = track.label.c_str();
                active = track.id == selected_track_;
            }
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
        else if (item.CtlID == add_control)
            draw_add_subtitles(item);
        else
            draw_row(item, tone);
    }

    COLORREF subtitle_header_color() const {
        return background_ == light_panel ? RGB(54, 54, 54) : RGB(143, 143, 143);
    }

    void draw_add_subtitles(const DRAWITEMSTRUCT &item) const {
        const LONG x = (item.rcItem.left + item.rcItem.right) / 2;
        const LONG y = (item.rcItem.top + item.rcItem.bottom) / 2;
        const RECT horizontal{x - 7, y - 1, x + 8, y + 1};
        const RECT vertical{x - 1, y - 7, x + 1, y + 8};
        const auto brush = reinterpret_cast<HBRUSH>(GetStockObject(DC_BRUSH));
        SetDCBrushColor(item.hDC, item.itemState & ODS_SELECTED ? accent : subtitle_header_color());
        FillRect(item.hDC, &horizontal, brush);
        FillRect(item.hDC, &vertical, brush);
    }

    void paint_subtitle_body(HWND window) const {
        PAINTSTRUCT paint{};
        const HDC dc = BeginPaint(window, &paint);
        const auto brush = reinterpret_cast<HBRUSH>(GetStockObject(DC_BRUSH));
        SetDCBrushColor(dc, background_);
        FillRect(dc, &paint.rcPaint, brush);
        const auto y = external_header_y();
        const RECT divider{0, y, panel_width, y + 1};
        SetDCBrushColor(dc, background_ == light_panel ? RGB(162, 162, 162) : RGB(38, 38, 38));
        FillRect(dc, &divider, brush);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, subtitle_header_color());
        const auto previous = font_ ? SelectObject(dc, font_) : nullptr;
        RECT header{list_x, y + 4, panel_width - 52, y + 36};
        DrawTextW(dc, L"External Tracks", -1, &header, DT_SINGLELINE | DT_VCENTER);
        if (previous)
            SelectObject(dc, previous);
        EndPaint(window, &paint);
    }

    HWND make_button(int control, const wchar_t *label, int x, int y, int w, int h, DWORD extra_style = 0) {
        return CreateWindowExW(0, L"BUTTON", label,
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW | extra_style,
                               x, y, w, h, host_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(control)),
                               module_, nullptr);
    }

    HWND make_list(int control, int x, int y, int rows) {
        return CreateWindowExW(0, L"LISTBOX", nullptr, WS_CHILD | WS_VISIBLE |
                               WS_VSCROLL | WS_TABSTOP | LBS_NOTIFY | LBS_OWNERDRAWFIXED |
                               LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
                               x, y, list_width, rows * row_height, host_,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(control)), module_, nullptr);
    }

    bool create_controls(HWND hwnd) {
        host_ = hwnd;
        const int x = page_ == Page::subtitles ? list_x : 0;
        none_ = make_button(none_control, L"None", x, 0, list_width, row_height, WS_DISABLED);
        list_ = make_list(track_control, x, row_height, track_rows());
        if (!none_ || !list_)
            return false;
        if (page_ == Page::subtitles) {
            const auto y = external_header_y();
            add_button_ = make_button(add_control, L"Add Subtitles", panel_width - 52, y + 4, 32, 32);
            external_list_ = make_list(external_control, x, y + external_header_height, external_rows());
            return add_button_ && external_list_;
        }
        if (page_ != Page::audio)
            return true;

        const int refresh_x = panel_width - 52 - list_x;
        const auto refresh_button = make_button(refresh_control, L"Refresh output devices",
                                                refresh_x, refresh_y - list_y, refresh_size, refresh_size);
        device_list_ = make_list(device_control, 0, devices_y - list_y, device_rows);
        return refresh_button && device_list_;
    }

    void control_changed(WORD control, WORD notification) {
        if (control == none_control && notification == BN_CLICKED) {
            select_track(no_track);
        } else if (control == add_control && notification == BN_CLICKED) {
            PostMessageW(GetParent(host_), add_subtitles_message(), 0, 0);
        } else if (control == refresh_control && notification == BN_CLICKED) {
            refresh_devices(true);
        } else if ((control == track_control || control == external_control) && notification == LBN_SELCHANGE) {
            const HWND list = control == track_control ? list_ : external_list_;
            const auto &tracks = control == track_control ? tracks_ : external_tracks_;
            const auto index = SendMessageW(list, LB_GETCURSEL, 0, 0);
            if (index >= 0 && static_cast<size_t>(index) < tracks.size())
                select_track(tracks[index].id);
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
        case WM_PAINT:
            if (panel->page_ == Page::subtitles) {
                panel->paint_subtitle_body(hwnd);
                return 0;
            }
            break;
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
            panel->add_button_ = panel->external_list_ = nullptr;
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
    if (!panel)
        return -1;
    if (redesign_is_selected(interface->libvlc))
        player_window::restore_startup_bounds(interface->libvlc);
    subtitle_preferences::use_saved_style({interface->libvlc, interface->parent});
    context_interface.store(interface);
    if (!panel->start()) {
        context_interface.store(nullptr);
        return -1;
    }
    active_panel = panel.release();
    return 0;
}

void close_panel(VlcInterface *) {
    context_interface.store(nullptr);
    delete active_panel;
    active_panel = nullptr;
    subtitle_preferences::release_media();
}

enum ModuleProperty {
    create_module = 0, create_config = 1, shortcut = 0x101, capability = 0x102, score = 0x103, open_callback = 0x104,
    close_callback = 0x105, no_unload = 0x106, name = 0x107, description = 0x109,
    config_name = 0x1000, config_value = 0x1001, config_description = 0x100c,
};
}

extern "C" __declspec(dllexport) int vlc_entry__3_0_0f(
    int (*set)(void *, void *, int, ...), void *data) {
    void *module;
    if (set(data, nullptr, create_module, &module))
        return -1;
    if (set(data, module, name, "subtitlepanel") ||
        set(data, module, description, "VLC Redesign controls and context menu") ||
        set(data, module, capability, "interface") || set(data, module, score, 0) ||
        set(data, module, open_callback, "open_panel", reinterpret_cast<void *>(open_panel)) ||
        set(data, module, close_callback, "close_panel", reinterpret_cast<void *>(close_panel)))
        return -1;
    constexpr int config_string = 0x80;
    void *config;
    if (set(data, nullptr, create_config, config_string, &config) ||
        set(data, config, config_name, subtitle_defaults::config_key) ||
        set(data, config, config_value, "") ||
        set(data, config, config_description, "Subtitle defaults preset", "Preferred subtitle track number and language."))
        return -1;
    const char *const shortcuts[] = {subtitle_preferences::text_style_filter};
    if (set(data, nullptr, create_module, &module) ||
        set(data, module, shortcut, std::size(shortcuts), shortcuts) ||
        set(data, module, description, "VLC Redesign text subtitle appearance") ||
        set(data, module, capability, "sub filter") || set(data, module, score, 0) ||
        set(data, module, no_unload) ||
        set(data, module, open_callback, "open_style_filter", reinterpret_cast<void *>(subtitle_preferences::open_style_filter)))
        return -1;
    const char *const demux_shortcuts[] = {subtitle_preferences::speed_filter};
    if (set(data, nullptr, create_module, &module) ||
        set(data, module, shortcut, std::size(demux_shortcuts), demux_shortcuts) ||
        set(data, module, description, "VLC Redesign subtitle speed") ||
        set(data, module, capability, "demux_filter") || set(data, module, score, 0) ||
        set(data, module, open_callback, "open_speed_filter", reinterpret_cast<void *>(subtitle_preferences::open_speed_filter)) ||
        set(data, module, close_callback, "close_speed_filter", reinterpret_cast<void *>(subtitle_preferences::close_speed_filter)))
        return -1;
    return 0;
}
