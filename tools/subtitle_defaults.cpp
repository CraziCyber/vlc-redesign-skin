#include "subtitle_defaults.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <vector>

namespace {

struct Language {
    const char *name;
    char alpha2[3], alpha3[4], bibliographic[4];
};

extern "C" {
__declspec(dllimport) const Language *GetLang_1(const char *);
__declspec(dllimport) const Language *GetLang_2T(const char *);
__declspec(dllimport) const Language *GetLang_2B(const char *);
}

bool known(const Language *language) {
    return language && language->alpha3[0] != '?';
}

std::string lowercase(std::string_view value) {
    std::string text(value);
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return text;
}

const Language *lookup_code(std::string_view value) {
    const auto prefix = value.substr(0, value.find_first_of("-_"));
    if (prefix.size() < 2 || prefix.size() > 3 || prefix.find_first_not_of("abcdefghijklmnopqrstuvwxyz") != prefix.npos)
        return nullptr;
    const std::string code(prefix);
    const auto language = code.size() == 2 ? GetLang_1(code.c_str()) : GetLang_2T(code.c_str());
    if (known(language))
        return language;
    if (code.size() == 3) {
        const auto other = GetLang_2B(code.c_str());
        return known(other) ? other : nullptr;
    }
    return nullptr;
}

const Language *lookup_name(std::string_view name) {
    static const auto languages = [] {
        std::vector<std::pair<std::string, const Language *>> entries;
        char code[3]{'a', 'a', '\0'};
        for (char first = 'a'; first <= 'z'; ++first) {
            for (char second = 'a'; second <= 'z'; ++second) {
                code[0] = first;
                code[1] = second;
                const auto language = GetLang_1(code);
                if (known(language))
                    entries.emplace_back(lowercase(language->name), language);
            }
        }
        return entries;
    }();
    const auto found = std::find_if(languages.begin(), languages.end(),
                                    [&](const auto &entry) { return entry.first == name; });
    return found == languages.end() ? nullptr : found->second;
}

std::string language_code(const Language *language) {
    return language->alpha2[0] == '?' ? language->alpha3 : language->alpha2;
}

bool matches(std::string_view preferred, const char *language) {
    if (!language)
        return false;
    const auto code = subtitle_defaults::parse_language(language);
    return code && (*code == preferred || (preferred == "no" && (*code == "nb" || *code == "nn")));
}

struct MediaTracks {
    MediaTrack **entries = nullptr;
    unsigned count;

    explicit MediaTracks(Media *media) : count(libvlc_media_tracks_get(media, &entries)) {}
    ~MediaTracks() { libvlc_media_tracks_release(entries, count); }

    MediaTracks(const MediaTracks &) = delete;
    MediaTracks &operator=(const MediaTracks &) = delete;
};

std::optional<std::int64_t> preferred_track(const subtitle_defaults::Preset &preset,
                                            const std::vector<std::int64_t> &available, const MediaTracks *metadata) {
    const auto numbered = preset.track > 0 && static_cast<std::size_t>(preset.track) <= available.size()
        ? std::optional<std::int64_t>{available[preset.track - 1]} : std::nullopt;
    if (!metadata)
        return numbered;

    std::optional<std::int64_t> first_match;
    for (const auto id : available) {
        for (unsigned i = 0; i < metadata->count; ++i) {
            const auto *track = metadata->entries[i];
            constexpr int subtitle = 2;  // libvlc_track_text
            if (track->type != subtitle || track->id != id || !matches(preset.language, track->language))
                continue;
            if (numbered == id)
                return id;
            if (!first_match)
                first_match = id;
            break;
        }
    }
    return first_match;
}

}

std::optional<int> subtitle_defaults::parse_track(std::string_view value) {
    if (value.empty() || lowercase(value) == "auto")
        return 0;
    int number = 0;
    const auto end = value.data() + value.size();
    const auto result = std::from_chars(value.data(), end, number);
    if (result.ec != std::errc{} || result.ptr != end || number < 0)
        return std::nullopt;
    return number;
}

std::optional<std::string> subtitle_defaults::parse_language(std::string_view value) {
    auto name = lowercase(value);
    if (name.empty() || name == "any" || name == "any language" || name == "auto")
        return std::string{};
    if (name == "norsk")
        name = "no";
    if (name == u8"bokmål")
        name = "nb";
    if (name == "nynorsk")
        name = "nn";
    const auto language = lookup_code(name);
    if (language)
        return language_code(language);
    if (const auto named = lookup_name(name))
        return language_code(named);
    return std::nullopt;
}

std::string_view subtitle_defaults::language_name(const std::string &code) {
    if (const auto language = lookup_code(code))
        return language->name;
    return "Any language";
}

void subtitle_defaults::AutoSelect::refresh(subtitle_preferences::Player player, LibVlcRef &library) {
    auto preset = load(player);
    if (!preset.track && preset.language.empty()) {
        preset_ = {};
        input_.reset();
        applied_ = false;
        return;
    }

    InputRef current(playlist_CurrentInput(player.playlist), vlc_object_release);
    if (current.get() != input_.get() || !(preset == preset_)) {
        input_ = std::move(current);
        preset_ = std::move(preset);
        applied_ = false;
    }
    if (applied_ || !active_playback(input_.get()))
        return;

    const VlcChoices choices(input_.get(), "spu-es");
    if (!choices.ids.list)
        return;
    std::vector<std::int64_t> available;
    for (int i = 0; i < choices.ids.list->count; ++i) {
        const auto id = choices.ids.list->values[i].integer;
        if (id >= 0)
            available.push_back(id);
    }
    if (available.empty())
        return;

    std::optional<MediaTracks> tracks;
    if (!preset_.language.empty()) {
        if (!library)
            library.reset(new_metadata_library());
        if (!library)
            return;
        const MediaRef media(libvlc_media_new_from_input_item(library.get(), input_GetItem(input_.get())),
                             libvlc_media_release);
        if (!media)
            return;
        tracks.emplace(media.get());
        if (!tracks->count)
            return;
    }

    // Apply once per input or preset edit so a manual switch stays in effect.
    if (const auto id = preferred_track(preset_, available, tracks ? &*tracks : nullptr)) {
        VlcValue selected{};
        applied_ = (var_Get(input_.get(), "spu-es", &selected) == 0 && selected.integer == *id) ||
                   var_Set(input_.get(), "spu-es", VlcValue{*id}) == 0;
    } else {
        applied_ = true;
    }
}
