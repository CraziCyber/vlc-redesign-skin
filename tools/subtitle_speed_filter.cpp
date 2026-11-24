#include "subtitle_preferences.hpp"
#include "vlc_input.hpp"

#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

namespace {

// VLC 3.0.24 Win64's demux_t and the parsed cues owned by its subtitle demuxer.
// Keep native parsing and packet delivery so delay and seeking use VLC's clock.
struct Module;
struct ElementaryStream;
struct Output {
    std::byte add_send_and_delete[0x18];
    int (*control)(Output *, int, va_list);
};
struct Demux {
    std::byte object[0x20];
    void *parent;
    Module *module;
    std::byte paths[0x20];
    Demux *next;
    Output *output;
    bool preparsing;
    int (*step)(Demux *);
    int (*control)(Demux *, int, va_list);
    std::byte info[0x10];
    void *state;
    Input *input;
};
struct CueTime {
    std::int64_t start, stop;
};
struct NativeCue {
    CueTime time;
    char *text;
};
struct NativeSubtitles {
    ElementaryStream *track;
    bool slave, first, sorted;
    std::int64_t next_time;
    NativeCue *cues;
    std::size_t count, current;
    std::int64_t length;
    int type;
    std::int64_t frame_interval;
};

static_assert(offsetof(Demux, next) == 0x50 && offsetof(Demux, output) == 0x58 &&
              offsetof(Demux, step) == 0x68 && offsetof(Demux, state) == 0x88 &&
              offsetof(NativeSubtitles, cues) == 0x18 && offsetof(NativeSubtitles, frame_interval) == 0x40 &&
              sizeof(NativeCue) == 0x18);

extern "C" {
__declspec(dllimport) const char *module_get_object(const Module *);
__declspec(dllimport) int demux_vaControl(Demux *, int, va_list);
}

constexpr int microdvd = 0, subrip = 1;
constexpr double ticks_per_second = 1000000.0;
constexpr int end_of_stream = 0, demux_success = 1;
constexpr int restart_es = 1, get_es_state = 5;

struct SpeedState {
    std::vector<CueTime> original;
    std::int64_t length;
    double scale = 1.0;
    float original_fps, parsed_override;
};

float read_float(void *object, const char *key) {
    VlcValue value{};
    return var_Get(object, key, &value) == 0 && std::isfinite(value.real) ? value.real : 0.0f;
}

double speed_scale(const SpeedState &state, const NativeSubtitles &subtitles, float desired) {
    if (subtitles.type == subrip)
        return desired > 1.0f ? state.original_fps / static_cast<double>(desired) : 1.0;

    // MicroDVD's parser may already have used a frame rate from the file or a
    // previous override. Use its actual frame interval when scaling its cues.
    if (desired <= 1.0f && state.parsed_override >= 1.0f)
        desired = state.original_fps > 1.0f ? state.original_fps : 25.0f;
    return desired > 1.0f ? ticks_per_second / desired / subtitles.frame_interval : 1.0;
}

std::int64_t scaled(std::int64_t time, double scale) {
    if (time <= 0 || scale == 1.0)
        return time;
    const long double result = static_cast<long double>(time) * scale;
    constexpr auto max = std::numeric_limits<std::int64_t>::max();
    return result >= max ? max : std::llround(result);
}

int control_output(Output *output, int query, ...) {
    va_list args;
    va_start(args, query);
    const auto result = output->control(output, query, args);
    va_end(args);
    return result;
}

void retime(Demux *source, SpeedState &state, NativeSubtitles &subtitles) {
    const auto scale = speed_scale(state, subtitles, read_float(source->input, "sub-fps"));
    if (scale == state.scale)
        return;

    VlcValue delay{};
    var_Get(source->parent, "spu-delay", &delay);
    const auto adjusted = subtitles.next_time - delay.integer;
    const auto barrier = adjusted >= 0 ? adjusted : subtitles.next_time;

    subtitles.current = subtitles.count;
    for (std::size_t i = 0; i < subtitles.count; ++i) {
        const auto &original = state.original[i];
        auto &cue = subtitles.cues[i].time;
        cue = {scaled(original.start, scale), scaled(original.stop, scale)};
        if (subtitles.current == subtitles.count && (cue.start >= barrier || cue.stop > barrier))
            subtitles.current = i;
    }
    subtitles.length = scaled(state.length, scale);
    state.scale = scale;

    // Drop cues already queued at the previous speed before sending the new
    // timings. Restart this stream only if VLC currently has it selected.
    bool selected = false;
    if (source->output && subtitles.track &&
        control_output(source->output, get_es_state, subtitles.track, &selected) == 0 && selected)
        control_output(source->output, restart_es, subtitles.track);
}

int demux_step(Demux *filter) {
    auto *source = filter->next;
    auto &subtitles = *static_cast<NativeSubtitles *>(source->state);
    if (subtitles.slave)
        retime(source, *static_cast<SpeedState *>(filter->state), subtitles);

    const auto result = source->step(source);
    // A subsequent speed change can move a cue to a time after the last one
    // VLC delivered. Keep a subtitle slave available until the video ends.
    return subtitles.slave && result == end_of_stream ? demux_success : result;
}

int demux_control(Demux *filter, int query, va_list args) {
    return demux_vaControl(filter->next, query, args);
}

}

int subtitle_preferences::open_speed_filter(void *object) {
    auto *filter = static_cast<Demux *>(object);
    auto *source = filter->next;
    if (!source || !source->input || !source->module ||
        std::strcmp(module_get_object(source->module), "subtitle") != 0)
        return -1;
    auto *subtitles = static_cast<const NativeSubtitles *>(source->state);
    const auto fps = read_float(source->input, "sub-original-fps");
    if (!subtitles || !subtitles->count ||
        !((subtitles->type == subrip && fps > 1.0f) ||
          (subtitles->type == microdvd && subtitles->frame_interval > 0)))
        return -1;

    try {
        auto state = std::make_unique<SpeedState>();
        state->length = subtitles->length;
        state->original_fps = fps;
        state->parsed_override = read_float(source, "sub-fps");
        if (subtitles->count > state->original.max_size())
            return -1;
        state->original.reserve(subtitles->count);
        for (std::size_t i = 0; i < subtitles->count; ++i)
            state->original.push_back(subtitles->cues[i].time);
        filter->state = state.release();
    } catch (const std::bad_alloc &) {
        return -1;
    }
    filter->step = demux_step;
    filter->control = demux_control;
    return 0;
}

void subtitle_preferences::close_speed_filter(void *object) {
    delete static_cast<SpeedState *>(static_cast<Demux *>(object)->state);
}
