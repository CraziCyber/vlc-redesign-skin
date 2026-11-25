#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "subtitle_timeline.hpp"
#include "playback_preferences.hpp"
#include "vlc_config.hpp"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <limits>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
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
constexpr wchar_t marker_class[] = L"VlcRedesignSubtitleTimeline";

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
using Marks = std::vector<Mark>;
std::shared_ptr<const Marks> display_marks;
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
    std::shared_ptr<Marks> marks;
    if (track && length > 0) {
        marks = std::make_shared<Marks>();
        marks->reserve(track->cues.size());
        const auto ratio = [length](double time) { return std::clamp(time / length, 0.0, 1.0); };
        for (const auto &cue : track->cues) {
            const auto start = double(cue.start) + delay;
            const auto end = start + cue.length;
            if (start < length && end >= 0)
                marks->push_back({ratio(start), ratio(end)});
        }
        std::sort(marks->begin(), marks->end(), [](auto left, auto right) { return left.start < right.start; });
    }
    std::atomic_store(&display_marks, std::shared_ptr<const Marks>(std::move(marks)));
}

struct MarkWindow {
    std::shared_ptr<const Marks> marks;
    RECT area{};
};

LRESULT CALLBACK marker_proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    const auto draw = [](HWND target, HDC dc) {
        RECT area{};
        GetClientRect(target, &area);
        SetDCBrushColor(dc, RGB(211, 121, 24));
        FillRect(dc, &area, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    };
    switch (message) {
    case WM_NCHITTEST: return HTTRANSPARENT;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        const auto dc = BeginPaint(window, &paint);
        draw(window, dc);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_PRINTCLIENT: draw(window, reinterpret_cast<HDC>(wp)); return 0;
    case WM_NCDESTROY:
        delete reinterpret_cast<MarkWindow *>(GetWindowLongPtrW(window, GWLP_USERDATA));
        break;
    default:
        break;
    }
    return DefWindowProcW(window, message, wp, lp);
}

HWND create_mark_window(HWND parent) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&marker_proc), &module))
        return nullptr;
    WNDCLASSW cls{};
    cls.hInstance = module;
    cls.lpfnWndProc = marker_proc;
    cls.lpszClassName = marker_class;
    if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return nullptr;
    const auto child = CreateWindowExW(WS_EX_TRANSPARENT, marker_class, L"", WS_CHILD | WS_CLIPSIBLINGS,
                                       0, 0, 0, 0, parent, nullptr, module, nullptr);
    if (child)
        SetWindowLongPtrW(child, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(new MarkWindow));
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

void update_window(HWND window, bool draw_marks, bool fullscreen_toolbar) {
    auto child = FindWindowExW(window, nullptr, marker_class, nullptr);
    const auto marks = std::atomic_load(&display_marks);
    if (!draw_marks || !marks || marks->empty() || !IsWindowVisible(window)) {
        if (child)
            DestroyWindow(child);
        return;
    }
    if (!child)
        child = create_mark_window(window);
    if (!child)
        return;
    const auto state = reinterpret_cast<MarkWindow *>(GetWindowLongPtrW(child, GWLP_USERDATA));
    RECT bounds{};
    GetClientRect(window, &bounds);
    // Slider endpoints and lower track edge in build.py's main, mini and fullscreen layouts.
    const bool mini = bounds.right == 391 && bounds.bottom == 339;
    const LONG top = mini ? 235 : bounds.bottom - (fullscreen_toolbar ? 18 : 17);
    const RECT area{mini ? 12 : 260, top, mini ? 379 : bounds.right - 434, top + 2};
    if (area.right <= area.left || (state->marks == marks && EqualRect(&state->area, &area)))
        return;

    const LONG width = area.right - area.left;
    const auto region = CreateRectRgn(0, 0, 0, 0);
    const auto part = CreateRectRgn(0, 0, 0, 0);
    if (region && part) {
        for (const auto &mark : *marks) {
            const LONG left = std::min(width - 1, static_cast<LONG>(mark.start * width));
            const LONG right = std::max(left + 1, static_cast<LONG>(mark.end * width));
            SetRectRgn(part, left, 0, right, area.bottom - area.top);
            CombineRgn(region, region, part, RGN_OR);
        }
        if (SetWindowRgn(child, region, FALSE)) {
            state->marks = marks;
            state->area = area;
            SetWindowPos(child, HWND_TOP, area.left, area.top, width, area.bottom - area.top,
                         SWP_NOACTIVATE | SWP_SHOWWINDOW);
            InvalidateRect(child, nullptr, FALSE);
        } else {
            DeleteObject(region);
        }
    } else if (region) {
        DeleteObject(region);
    }
    if (part)
        DeleteObject(part);
}

}
