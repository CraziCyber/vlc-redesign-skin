#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define UNICODE
#include "subtitle_timeline.hpp"
#include "playback_preferences.hpp"
#include "preferences_widgets.hpp"
#include "subtitle_preferences.hpp"
#include "vlc_config.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cwchar>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

struct Stream;
struct Interrupt;

// The fields read by the demux output callbacks in VLC 3.0.24 Win64.
struct Demux {
    std::byte header[0x68];
    int (*step)(Demux *);
};

struct Format {
    int category;
    std::uint32_t codec, original_codec;
    int id, group;
};

struct Block {
    Block *next;
    std::byte *buffer;
    std::size_t size;
    std::byte allocation[0x10];
    std::uint32_t flags, samples;
    std::int64_t pts, dts, length;
    void (*release)(Block *);
};

struct Cue { std::int64_t start, length; };
struct Track {
    int id;
    bool subtitle;
    std::vector<Cue> cues;
};

struct Output {
    Track *(*add)(Output *, const Format *);
    int (*send)(Output *, Track *, Block *);
    void (*remove)(Output *, Track *);
    int (*control)(Output *, int, va_list);
    void (*destroy)(Output *);
    void *data;
};

struct MediaSlave {
    const char *uri;
    int type;
    unsigned priority;
};

static_assert(offsetof(Block, pts) == 0x30 && offsetof(Block, release) == 0x48 &&
              offsetof(Output, control) == 0x18 && offsetof(Format, id) == 0x0c);

extern "C" {
__declspec(dllimport) Stream *vlc_stream_NewURL(void *, const char *);
__declspec(dllimport) void vlc_stream_Delete(Stream *);
__declspec(dllimport) Demux *demux_New(void *, const char *, const char *, Stream *, Output *);
__declspec(dllimport) void demux_Delete(Demux *);
__declspec(dllimport) Interrupt *vlc_interrupt_create();
__declspec(dllimport) void vlc_interrupt_destroy(Interrupt *);
__declspec(dllimport) Interrupt *vlc_interrupt_set(Interrupt *);
__declspec(dllimport) void vlc_interrupt_kill(Interrupt *);
__declspec(dllimport) char *input_item_GetURI(InputItem *);
__declspec(dllimport) char *vlc_path2uri(const char *, const char *);
__declspec(dllimport) unsigned libvlc_media_slaves_get(Media *, MediaSlave ***);
__declspec(dllimport) void libvlc_media_slaves_release(MediaSlave **, unsigned);
}

constexpr int subtitle_category = 3;
constexpr char file_scheme[] = "file://";
constexpr wchar_t timeline_class[] = L"VlcRedesignSubtitleTimeline";

bool is_file(std::string_view uri) {
    return uri.substr(0, sizeof(file_scheme) - 1) == file_scheme;
}

struct Scan {
    std::vector<std::unique_ptr<Track>> tracks;
    std::string uri;
    std::unique_ptr<Interrupt, decltype(&vlc_interrupt_destroy)> interrupt{vlc_interrupt_create(), vlc_interrupt_destroy};
    std::atomic<bool> cancelled{false}, ready{false};
    std::thread worker;

    explicit Scan(std::string source) : uri(std::move(source)) {}

    ~Scan() {
        if (worker.joinable()) {
            cancelled.store(true);
            vlc_interrupt_kill(interrupt.get());
            worker.join();
        }
    }

    const Track *find(std::int64_t id) const {
        if (!ready.load())
            return nullptr;
        const auto match = std::find_if(tracks.cbegin(), tracks.cend(),
            [id](const auto &track) { return track->subtitle && (id < 0 || track->id == id); });
        return match == tracks.cend() ? nullptr : match->get();
    }
};

Track *add_track(Output *output, const Format *format) {
    if (format->group < 0)
        return nullptr;
    auto &scan = *static_cast<Scan *>(output->data);
    try {
        // VLC assigns the registration index when a container supplies no track ID.
        const int id = format->id < 0 ? static_cast<int>(scan.tracks.size()) : format->id;
        scan.tracks.push_back(std::make_unique<Track>(Track{id, format->category == subtitle_category, {}}));
        return scan.tracks.back().get();
    } catch (const std::bad_alloc &) {
        scan.cancelled.store(true);
        return nullptr;
    }
}

int receive(Output *output, Track *track, Block *block) {
    auto &scan = *static_cast<Scan *>(output->data);
    while (block) {
        auto *next = block->next;
        if (track && track->subtitle && block->pts > 0 && block->size && !scan.cancelled.load()) {
            try {
                // Strip the VLC clock origin. A zero-duration subtitle still has a start marker.
                track->cues.push_back({block->pts - 1, std::max<std::int64_t>(0, block->length)});
            } catch (const std::bad_alloc &) {
                scan.cancelled.store(true);
            }
        }
        block->release(block);
        block = next;
    }
    return scan.cancelled.load() ? -1 : 0;
}

int control(Output *, int query, va_list args) {
    switch (query) {
    case 5: { // ES_OUT_GET_ES_STATE
        auto *track = va_arg(args, Track *);
        *va_arg(args, bool *) = track && track->subtitle;
        return 0;
    }
    case 19: // ES_OUT_GET_EMPTY
        *va_arg(args, bool *) = true;
        return 0;
    case 0:  // ES_OUT_SET_ES
    case 3:  // ES_OUT_SET_ES_DEFAULT
    case 4:  // ES_OUT_SET_ES_STATE
    case 6:  // ES_OUT_SET_ES_CAT_POLICY
    case 7:  // ES_OUT_SET_GROUP
    case 8:  // ES_OUT_SET_PCR
    case 9:  // ES_OUT_SET_GROUP_PCR
    case 10: // ES_OUT_RESET_PCR
    case 11: // ES_OUT_SET_ES_FMT
    case 12: // ES_OUT_SET_NEXT_DISPLAY_TIME
    case 13: // ES_OUT_SET_GROUP_META
    case 20: // ES_OUT_SET_META
        return 0;
    default:
        return -1;
    }
}

void read_cues(void *vlc, Scan &scan, bool external) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    const auto previous = vlc_interrupt_set(scan.interrupt.get());
    Output output{add_track, receive, [](Output *, Track *) {}, control, [](Output *) {}, &scan};
    if (auto *stream = vlc_stream_NewURL(vlc, scan.uri.c_str())) {
        const auto location = scan.uri.c_str() + sizeof(file_scheme) - 1;
        auto *demux = demux_New(vlc, external ? "subtitle" : "any", location, stream, &output);
        if (demux) {
            while (!scan.cancelled.load() && demux->step && demux->step(demux) > 0) {}
            demux_Delete(demux);
        } else {
            vlc_stream_Delete(stream);
        }
    }
    vlc_interrupt_set(previous);
    if (scan.cancelled.load())
        scan.tracks.clear();
    scan.ready.store(true);
}

void start_scan(std::unique_ptr<Scan> &job, void *vlc, std::string uri, bool external) {
    if (job && job->uri == uri)
        return;
    job.reset();
    if (!is_file(uri))
        return;
    job = std::make_unique<Scan>(std::move(uri));
    if (job->interrupt) {
        try {
            job->worker = std::thread(read_cues, vlc, std::ref(*job), external);
            return;
        } catch (const std::system_error &) {}
    }
    job->ready.store(true);
}

std::string find_slave(Input *input, std::int64_t selected, const Scan *embedded, LibVlcRef &metadata) {
    if (!metadata)
        metadata.reset(new_metadata_library());
    if (!metadata)
        return {};
    const MediaRef media(libvlc_media_new_from_input_item(metadata.get(), input_GetItem(input)), libvlc_media_release);
    if (!media)
        return {};
    MediaSlave **slaves = nullptr;
    const auto count = libvlc_media_slaves_get(media.get(), &slaves);
    std::vector<std::string> files;
    for (unsigned i = 0; i < count; ++i) {
        if (slaves[i]->type == 0 && slaves[i]->uri)
            files.emplace_back(slaves[i]->uri);
    }
    libvlc_media_slaves_release(slaves, count);
    if (files.empty()) {
        VlcValue value{};
        constexpr int string_type = 0x40;
        if (var_Inherit(input, "sub-file", string_type, &value) == 0) {
            auto preferred = vlc_config::take_string(value.string);
            if (!preferred.empty()) {
                if (!is_file(preferred))
                    preferred = vlc_config::take_string(vlc_path2uri(preferred.c_str(), nullptr));
                files.push_back(std::move(preferred));
            }
        }
    }
    const VlcChoices choices(input, "spu-es");
    std::vector<std::int64_t> external_ids;
    if (choices.ids.list) {
        for (int i = 0; i < choices.ids.list->count; ++i) {
            const auto id = choices.ids.list->values[i].integer;
            if (id >= 0 && (!embedded || !embedded->find(id)))
                external_ids.push_back(id);
        }
    }
    if (external_ids.size() != files.size())
        return {};
    const auto match = std::find(external_ids.begin(), external_ids.end(), selected);
    return match == external_ids.end() ? std::string{} : files[match - external_ids.begin()];
}

struct Mark { double start, end; };
struct Timeline {
    Timeline(Input *source, std::int64_t duration)
        : input(static_cast<Input *>(vlc_object_hold(source)), vlc_object_release), length(duration) {}

    InputRef input;
    std::int64_t length;
    std::vector<Mark> marks;
};

std::shared_ptr<const Timeline> display_timeline;
std::unique_ptr<Scan> embedded_scan, external_scan;
InputRef scanned_input{nullptr, vlc_object_release};
std::string media_uri;
std::int64_t selected_track = -1;
std::int64_t last_length = 0, last_delay = 0;
const Track *last_track = nullptr;
bool was_enabled = false;

void publish(const Track *track, std::int64_t length, std::int64_t delay) {
    if (track == last_track && length == last_length && delay == last_delay)
        return;
    last_track = track;
    last_length = length;
    last_delay = delay;
    std::shared_ptr<Timeline> timeline;
    if (track && length > 0 && !track->cues.empty()) {
        timeline = std::make_shared<Timeline>(scanned_input.get(), length);
        timeline->marks.reserve(track->cues.size());
        const auto ratio = [length](double time) { return std::clamp(time / length, 0.0, 1.0); };
        for (const auto &cue : track->cues) {
            const auto start = double(cue.start) + delay;
            const auto end = start + cue.length;
            if (start < length && end >= 0)
                timeline->marks.push_back({ratio(start), ratio(end)});
        }
    }
    std::atomic_store(&display_timeline, std::shared_ptr<const Timeline>(std::move(timeline)));
}

using preferences_widgets::fill;
using preferences_widgets::round_rect;

struct Colors {
    COLORREF chrome, track, progress, played, upcoming, handle;
};

constexpr Colors dark{RGB(27, 27, 27), RGB(9, 9, 9), RGB(55, 31, 6),
                      RGB(79, 60, 37), RGB(43, 43, 43), RGB(211, 121, 24)};
constexpr Colors light{RGB(171, 171, 171), RGB(222, 222, 222), RGB(216, 186, 154),
                       RGB(174, 141, 106), RGB(157, 157, 157), RGB(232, 133, 26)};

constexpr struct {
    int milliseconds;
    const wchar_t *label;
} delay_buttons[] = {
    {-500, L"Show subtitles 500 ms earlier"}, {-50, L"Show subtitles 50 ms earlier"},
    {50, L"Show subtitles 50 ms later"}, {500, L"Show subtitles 500 ms later"},
};
constexpr int no_hit = -1, seek = std::size(delay_buttons);

struct TimelineView {
    std::shared_ptr<const Timeline> timeline;
    const Colors *colors = &dark;
    RECT area{};
    RECT track{};
    std::array<RECT, seek> buttons{};
    HWND tooltip = nullptr;
    LONG playhead = 0, thumb = 4;
    int hovered = no_hit, pressed = no_hit;

    LONG start_x() const { return track.left + thumb; }
    LONG end_x() const { return track.right - thumb; }
    LONG pixel_at(double fraction) const { return start_x() + std::lround((end_x() - start_x()) * fraction); }

    double position() const {
        VlcValue value{};
        return var_Get(timeline->input.get(), "position", &value) == 0 && std::isfinite(value.real)
            ? std::clamp<double>(value.real, 0.0, 1.0) : 0.0;
    }

    void seek_to(HWND window, LONG x) {
        auto *input = timeline->input.get();
        VlcValue can_seek{};
        const LONG next = std::clamp(x, start_x(), end_x());
        if (next == playhead || !active_playback(input) || var_Get(input, "can-seek", &can_seek) != 0 || !can_seek.boolean)
            return;
        VlcValue value{};
        value.real = static_cast<float>(next - start_x()) / (end_x() - start_x());
        if (var_Set(input, "position", value) == 0) {
            playhead = next;
            InvalidateRect(window, nullptr, FALSE);
        }
    }

    int hit(POINT point) const {
        for (int i = 0; i < seek; ++i)
            if (PtInRect(&buttons[i], point))
                return i;
        return PtInRect(&track, point) ? seek : no_hit;
    }

    void hover(HWND window, int part) {
        if (hovered == part)
            return;
        hovered = part;
        if (part != no_hit) {
            TRACKMOUSEEVENT leave{sizeof(leave), TME_LEAVE, window, 0};
            TrackMouseEvent(&leave);
        }
        InvalidateRect(window, nullptr, FALSE);
    }

    void layout(HWND window, RECT bounds) {
        area = bounds;
        const LONG width = area.right - area.left, height = area.bottom - area.top;
        const LONG bottom = height - 2;
        buttons = {{{2, 2, 20, bottom}, {22, 2, 44, bottom},
                    {width - 44, 2, width - 22, bottom}, {width - 20, 2, width - 2, bottom}}};
        track = {45, 2, width - 45, bottom};
        thumb = height < 24 ? 3 : 4;

        const bool new_tooltip = !tooltip;
        if (new_tooltip) {
            const auto module = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(window, GWLP_HINSTANCE));
            tooltip = CreateWindowExW(0, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX,
                                      0, 0, 0, 0, window, nullptr, module, nullptr);
        }
        if (tooltip) {
            for (int i = 0; i <= seek; ++i) {
                TOOLINFOW tool{};
                tool.cbSize = TTTOOLINFOW_V2_SIZE;
                tool.hwnd = window;
                tool.uId = i + 1;
                tool.uFlags = TTF_SUBCLASS;
                tool.rect = i == seek ? track : buttons[i];
                tool.lpszText = LPSTR_TEXTCALLBACKW;
                SendMessageW(tooltip, new_tooltip ? TTM_ADDTOOLW : TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&tool));
            }
        }
        SetWindowPos(window, HWND_TOP, area.left, area.top, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    void tooltip_text(NMTTDISPINFOW &tip) const {
        const auto part = tip.hdr.idFrom - 1;
        if (part < std::size(delay_buttons)) {
            VlcValue delay{};
            var_Get(timeline->input.get(), "spu-delay", &delay);
            std::swprintf(tip.szText, std::size(tip.szText), L"%ls (delay %+.0f ms)",
                          delay_buttons[part].label, delay.integer / 1000.0);
        } else {
            const auto elapsed = static_cast<long long>(position() * timeline->length / 1000000);
            const auto total = timeline->length / 1000000;
            std::swprintf(tip.szText, std::size(tip.szText), L"%lld:%02lld / %lld:%02lld",
                          elapsed / 60, elapsed % 60, total / 60, total % 60);
        }
    }

    void draw(HDC dc, RECT bounds) const {
        fill(dc, bounds, colors->chrome);
        round_rect(dc, bounds, 6, colors->track);
        const LONG before = std::max(track.left, playhead - thumb - 4);
        const LONG after = std::min(track.right, playhead + thumb + 4);
        if (before > track.left)
            round_rect(dc, {track.left, track.top, before, track.bottom}, 4, colors->progress);
        for (const auto &mark : timeline->marks) {
            const LONG left = std::min(end_x() - 1, pixel_at(mark.start));
            const LONG right = std::max(left + 1, pixel_at(mark.end));
            if (left < before)
                fill(dc, {left, track.top, std::min(before, right), track.bottom}, colors->played);
            if (right > after)
                fill(dc, {std::max(after, left), track.top, right, track.bottom}, colors->upcoming);
        }
        round_rect(dc, {playhead - thumb, track.top, playhead + thumb, track.bottom}, thumb, colors->handle);

        const auto pen = CreatePen(PS_SOLID, 2, RGB(17, 17, 17));
        const auto previous = SelectObject(dc, pen);
        for (int i = 0; i < seek; ++i) {
            const auto rect = buttons[i];
            const auto face = hovered == i ? (pressed == i ? RGB(153, 84, 15) : RGB(221, 136, 37)) : RGB(174, 103, 25);
            round_rect(dc, rect, 4, face);
            const LONG x = (rect.left + rect.right) / 2, y = (rect.top + rect.bottom) / 2;
            if (i == 0 || i == seek - 1) {
                const LONG direction = i == 0 ? -1 : 1;
                const POINT arrow[]{{x - 2 * direction, y - 5}, {x + 2 * direction, y}, {x - 2 * direction, y + 5}};
                Polyline(dc, arrow, std::size(arrow));
            } else {
                MoveToEx(dc, x - 5, y, nullptr);
                LineTo(dc, x + 6, y);
                if (delay_buttons[i].milliseconds > 0) {
                    MoveToEx(dc, x, y - 5, nullptr);
                    LineTo(dc, x, y + 6);
                }
            }
        }
        SelectObject(dc, previous);
        DeleteObject(pen);
    }
};

LRESULT CALLBACK timeline_proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    const auto view = reinterpret_cast<TimelineView *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!view)
        return DefWindowProcW(window, message, wp, lp);
    switch (message) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT:
    case WM_PRINTCLIENT: {
        RECT bounds{};
        GetClientRect(window, &bounds);
        if (message == WM_PAINT)
            preferences_widgets::paint(window, bounds, [&](HDC dc) { view->draw(dc, bounds); });
        else
            view->draw(reinterpret_cast<HDC>(wp), bounds);
        return 0;
    }
    case WM_MOUSEMOVE: {
        const POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        view->hover(window, view->hit(point));
        if (view->pressed == seek)
            view->seek_to(window, point.x);
        return 0;
    }
    case WM_MOUSELEAVE:
        view->hover(window, no_hit);
        return 0;
    case WM_LBUTTONDOWN:
        view->pressed = view->hit({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        if (view->pressed != no_hit) {
            SetCapture(window);
            if (view->pressed == seek)
                view->seek_to(window, GET_X_LPARAM(lp));
            else
                InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP: {
        const int pressed = std::exchange(view->pressed, no_hit);
        const POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (pressed == seek)
            view->seek_to(window, point.x);
        else if (pressed != no_hit && pressed == view->hit(point))
            subtitle_preferences::adjust_delay(view->timeline->input.get(), delay_buttons[pressed].milliseconds);
        if (GetCapture() == window)
            ReleaseCapture();
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    }
    case WM_CAPTURECHANGED:
        view->pressed = no_hit;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_NOTIFY: {
        auto &tip = *reinterpret_cast<NMTTDISPINFOW *>(lp);
        if (tip.hdr.hwndFrom == view->tooltip && tip.hdr.code == TTN_GETDISPINFOW)
            view->tooltip_text(tip);
        return 0;
    }
    case WM_DESTROY:
        if (view->tooltip)
            DestroyWindow(view->tooltip);
        break;
    case WM_NCDESTROY:
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        delete view;
        break;
    default:
        break;
    }
    return DefWindowProcW(window, message, wp, lp);
}

HWND create_timeline_window(HWND parent) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&timeline_proc), &module))
        return nullptr;
    WNDCLASSW cls{};
    cls.hInstance = module;
    cls.lpfnWndProc = timeline_proc;
    cls.hCursor = LoadCursorW(nullptr, IDC_HAND);
    cls.lpszClassName = timeline_class;
    if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return nullptr;
    const auto child = CreateWindowExW(0, timeline_class, L"Subtitle timeline", WS_CHILD | WS_CLIPSIBLINGS,
                                       0, 0, 0, 0, parent, nullptr, module, nullptr);
    if (child)
        SetWindowLongPtrW(child, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(new TimelineView));
    return child;
}

}

namespace subtitle_timeline {

bool refresh(VlcPlayer player, Input *input, LibVlcRef &metadata, std::string_view loaded_subtitle) {
    const bool enabled = playback_preferences::switch_enabled(player, playback_preferences::timeline_key);
    const bool update_windows = enabled || was_enabled;
    was_enabled = enabled;
    VlcValue track{}, length{}, delay{};
    if (!enabled || !active_playback(input) || var_Get(input, "spu-es", &track) != 0 || track.integer < 0 ||
        var_Get(input, "length", &length) != 0 || length.integer <= 0) {
        publish(nullptr, 0, 0);
        if (!enabled || input != scanned_input.get())
            release();
        return update_windows;
    }

    if (input != scanned_input.get()) {
        publish(nullptr, 0, 0);
        embedded_scan.reset();
        external_scan.reset();
        scanned_input.reset(static_cast<Input *>(vlc_object_hold(input)));
        media_uri = vlc_config::take_string(input_item_GetURI(input_GetItem(input)));
        selected_track = -1;
    }
    var_Get(input, "spu-delay", &delay);

    const Track *selected = nullptr;
    if (!loaded_subtitle.empty()) {
        selected_track = -1;
        if (!external_scan || external_scan->uri != loaded_subtitle)
            publish(nullptr, 0, 0);
        start_scan(external_scan, player.vlc, std::string(loaded_subtitle), true);
        if (external_scan)
            selected = external_scan->find(-1);
    } else {
        start_scan(embedded_scan, player.vlc, media_uri, false);
        if (embedded_scan)
            selected = embedded_scan->find(track.integer);
        if (!selected && (!embedded_scan || embedded_scan->ready.load())) {
            if (selected_track != track.integer) {
                publish(nullptr, 0, 0);
                start_scan(external_scan, player.vlc, find_slave(input, track.integer, embedded_scan.get(), metadata), true);
                selected_track = track.integer;
            }
            if (external_scan)
                selected = external_scan->find(-1);
        }
    }
    publish(selected, length.integer, delay.integer);
    return update_windows;
}

void release() {
    publish(nullptr, 0, 0);
    embedded_scan.reset();
    external_scan.reset();
    scanned_input.reset();
    media_uri.clear();
    selected_track = -1;
}

UINT update_message() {
    static const auto message = RegisterWindowMessageW(L"VlcRedesign.UpdateSubtitleTimeline");
    return message;
}

void update_window(HWND window, bool show_timeline, bool fullscreen_toolbar) {
    auto child = FindWindowExW(window, nullptr, timeline_class, nullptr);
    const auto timeline = std::atomic_load(&display_timeline);
    RECT bounds{};
    GetClientRect(window, &bounds);
    // Cover the Skins2 track in build.py. The mini player has room for taller buttons here.
    const bool mini = bounds.right == 391 && bounds.bottom == 339;
    const LONG top = mini ? 223 : bounds.bottom - (fullscreen_toolbar ? 42 : 41);
    const RECT area{mini ? 8 : 254, top, mini ? 383 : bounds.right - 428, top + (mini ? 20 : 28)};
    if (!show_timeline || !timeline || !IsWindowVisible(window) || area.right - area.left < 112) {
        if (child)
            DestroyWindow(child);
        return;
    }
    if (!child)
        child = create_timeline_window(window);
    if (!child)
        return;
    const auto view = reinterpret_cast<TimelineView *>(GetWindowLongPtrW(child, GWLP_USERDATA));
    bool redraw = view->timeline != timeline;
    if (view->timeline && view->timeline->input.get() != timeline->input.get() && GetCapture() == child)
        ReleaseCapture();
    view->timeline = timeline;

    if (const auto dc = GetDC(window)) {
        const auto chrome = GetPixel(dc, area.left - 1, area.top - 1);
        ReleaseDC(window, dc);
        const auto colors = chrome == light.chrome ? &light : chrome == dark.chrome ? &dark : view->colors;
        redraw |= view->colors != colors;
        view->colors = colors;
    }
    if (!EqualRect(&view->area, &area)) {
        view->layout(child, area);
        redraw = true;
    }
    if (view->pressed != seek) {
        const LONG playhead = view->pixel_at(view->position());
        redraw |= view->playhead != playhead;
        view->playhead = playhead;
    }
    if (redraw)
        InvalidateRect(child, nullptr, FALSE);
}

}
