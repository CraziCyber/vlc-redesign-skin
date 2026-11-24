#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

struct Input;
struct InputItem;
struct Playlist;
struct LibVlc;
struct Media;
struct VlcList;
struct AudioOutput;

struct VlcPlayer {
    void *vlc;
    Playlist *playlist;
};

// VLC 3.0's vlc_value_t is passed by value to var_Set on this platform.
union VlcValue {
    std::int64_t integer;
    bool boolean;
    float real;
    char *string;
    void *address;
    VlcList *list;
};

struct VlcList {
    int type;
    int count;
    VlcValue *values;
};

struct MediaTrack {
    std::uint32_t codec, original_codec;
    int id, type, profile, level;
    void *details;
    unsigned bitrate;
    char *language;
};
static_assert(offsetof(MediaTrack, language) == 0x28, "VLC 3.0.24 Win64 media track layout");

extern "C" {
__declspec(dllimport) Input *playlist_CurrentInput(Playlist *);
__declspec(dllimport) AudioOutput *playlist_GetAout(Playlist *);
__declspec(dllimport) InputItem *input_GetItem(Input *);
__declspec(dllimport) int input_Control(Input *, int, ...);
__declspec(dllimport) void vlc_object_release(void *);
__declspec(dllimport) int var_Create(void *, const char *, int);
__declspec(dllimport) int var_Get(void *, const char *, VlcValue *);
__declspec(dllimport) int var_Set(void *, const char *, VlcValue);
__declspec(dllimport) int var_Change(void *, const char *, int, VlcValue *, VlcValue *);
__declspec(dllimport) void var_FreeList(VlcValue *, VlcValue *);
__declspec(dllimport) void libvlc_free(void *);
__declspec(dllimport) LibVlc *libvlc_new(int, const char *const *);
__declspec(dllimport) void libvlc_release(LibVlc *);
__declspec(dllimport) Media *libvlc_media_new_from_input_item(LibVlc *, InputItem *);
__declspec(dllimport) void libvlc_media_release(Media *);
__declspec(dllimport) unsigned libvlc_media_tracks_get(Media *, MediaTrack ***);
__declspec(dllimport) void libvlc_media_tracks_release(MediaTrack **, unsigned);
}

using InputRef = std::unique_ptr<Input, decltype(&vlc_object_release)>;
using AudioOutputRef = std::unique_ptr<AudioOutput, decltype(&vlc_object_release)>;
using LibVlcRef = std::unique_ptr<LibVlc, decltype(&libvlc_release)>;
using MediaRef = std::unique_ptr<Media, decltype(&libvlc_media_release)>;

struct VlcChoices {
    VlcValue ids{}, labels{};

    VlcChoices(void *object, const char *variable) {
        constexpr int get_choices = 0x24;
        if (object)
            var_Change(object, variable, get_choices, &ids, &labels);
    }
    ~VlcChoices() {
        if (ids.list)
            var_FreeList(&ids, &labels);
    }

    VlcChoices(const VlcChoices &) = delete;
    VlcChoices &operator=(const VlcChoices &) = delete;
};

inline LibVlc *new_metadata_library() {
    const char *options[] = {"--ignore-config", "--no-media-library", "--no-sub-autodetect-file", "--intf=dummy"};
    return libvlc_new(4, options);
}

// These query numbers are from VLC 3.0.24's input_query_e.
constexpr int input_add_slave = 0x24, input_restart_es = 0x27, input_get_vouts = 0x2b;

inline bool active_playback(Input *input) {
    VlcValue state{};
    constexpr int playing = 2, paused = 3;
    return input && var_Get(input, "state", &state) == 0 && (state.integer == playing || state.integer == paused);
}
