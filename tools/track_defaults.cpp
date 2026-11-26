#include "track_defaults.hpp"
#include "vlc_config.hpp"

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

const char *config_key(track_defaults::Kind kind) {
    return kind == track_defaults::Kind::audio ? track_defaults::audio_key : track_defaults::subtitle_key;
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
    const auto code = track_defaults::parse_language(language);
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

std::optional<std::int64_t> preferred_track(const track_defaults::Preset &preset,
                                            const std::vector<std::int64_t> &available, const MediaTracks *metadata,
                                            track_defaults::Kind kind) {
    const auto numbered = preset.track > 0 && static_cast<std::size_t>(preset.track) <= available.size()
        ? std::optional<std::int64_t>{available[preset.track - 1]} : std::nullopt;
    if (!metadata)
        return numbered;

    const int type = kind == track_defaults::Kind::audio ? 0 : 2;  // libvlc_track_type_t
    std::optional<std::int64_t> first_match;
    for (const auto id : available) {
        for (unsigned i = 0; i < metadata->count; ++i) {
            const auto *track = metadata->entries[i];
            if (track->type != type || track->id != id || !matches(preset.language, track->language))
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

std::optional<int> track_defaults::parse_track(std::string_view value) {
    if (value.empty() || lowercase(value) == "auto")
        return 0;
    int number = 0;
    const auto end = value.data() + value.size();
    const auto result = std::from_chars(value.data(), end, number);
    if (result.ec != std::errc{} || result.ptr != end || number < 0)
        return std::nullopt;
    return number;
}

std::optional<std::string> track_defaults::parse_language(std::string_view value) {
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

std::string_view track_defaults::language_name(const std::string &code) {
    if (const auto language = lookup_code(code))
        return language->name;
    return "Any language";
}

track_defaults::Preset track_defaults::load(VlcPlayer player, Kind kind) {
    const std::unique_ptr<char, decltype(&libvlc_free)> stored(config_GetPsz(player.vlc, config_key(kind)), libvlc_free);
    const std::string_view text = stored ? stored.get() : "";
    const auto separator = text.find(':');
    if (separator == text.npos)
        return {};
    return {parse_track(text.substr(0, separator)).value_or(0),
            parse_language(text.substr(separator + 1)).value_or("")};
}

bool track_defaults::save(VlcPlayer player, Kind kind, const Preset &preset) {
    const auto value = std::to_string(preset.track) + ':' + preset.language;
    config_PutPsz(player.vlc, config_key(kind), value.c_str());
    return vlc_config::save(player.vlc);
}

void track_defaults::AutoSelect::restore_track(const char *variable) {
    if (track_override_ && active_playback(input_.get())) {
        VlcValue current{};
        if (var_Get(input_.get(), variable, &current) == 0 && current.integer == track_override_->selected)
            var_Set(input_.get(), variable, VlcValue{track_override_->original});
    }
    track_override_.reset();
}

void track_defaults::AutoSelect::refresh(VlcPlayer player, LibVlcRef &library) {
    auto preset = load(player, kind_);
    const bool automatic = !preset.track && preset.language.empty();
    if (automatic && !input_)
        return;

    const char *variable = kind_ == Kind::audio ? "audio-es" : "spu-es";
    InputRef current(playlist_CurrentInput(player.playlist), vlc_object_release);
    if (current.get() != input_.get()) {
        input_ = std::move(current);
        track_override_.reset();
        applied_ = false;
    }
    if (automatic) {
        restore_track(variable);
        input_.reset();
    }
    if (!(preset == preset_)) {
        preset_ = std::move(preset);
        applied_ = false;
    }
    if (applied_ || !active_playback(input_.get()))
        return;

    const VlcChoices choices(input_.get(), variable);
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
    if (const auto id = preferred_track(preset_, available, tracks ? &*tracks : nullptr, kind_)) {
        VlcValue selected{};
        if (var_Get(input_.get(), variable, &selected) != 0)
            return;
        if (track_override_ && track_override_->selected != selected.integer)
            track_override_.reset();
        applied_ = selected.integer == *id;
        if (!applied_ && var_Set(input_.get(), variable, VlcValue{*id}) == 0) {
            const auto original = track_override_ ? track_override_->original : selected.integer;
            track_override_ = TrackOverride{original, *id};
            applied_ = true;
        }
    } else {
        restore_track(variable);
        applied_ = true;
    }
}
