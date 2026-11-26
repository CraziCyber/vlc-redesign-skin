#include "video_renderers.hpp"
#include "vlc_config.hpp"

#include <algorithm>

namespace video_renderers {

struct Description {
    const char *name;
    const char *long_name;
};

struct EventManager;
struct Event {
    int type;
    void *sender;
    Item *item;
};

extern "C" {
__declspec(dllimport) std::size_t libvlc_renderer_discoverer_list_get(LibVlc *, Description ***);
__declspec(dllimport) void libvlc_renderer_discoverer_list_release(Description **, std::size_t);
__declspec(dllimport) Discoverer *libvlc_renderer_discoverer_new(LibVlc *, const char *);
__declspec(dllimport) void libvlc_renderer_discoverer_release(Discoverer *);
__declspec(dllimport) EventManager *libvlc_renderer_discoverer_event_manager(Discoverer *);
__declspec(dllimport) int libvlc_renderer_discoverer_start(Discoverer *);
__declspec(dllimport) int libvlc_event_attach(EventManager *, int, void (*)(const Event *, void *), void *);
__declspec(dllimport) Item *vlc_renderer_item_hold(Item *);
__declspec(dllimport) void vlc_renderer_item_release(Item *);
__declspec(dllimport) int vlc_renderer_item_flags(const Item *);
__declspec(dllimport) const char *vlc_renderer_item_name(const Item *);
__declspec(dllimport) const char *vlc_renderer_item_sout(const Item *);
__declspec(dllimport) int playlist_SetRenderer(Playlist *, Item *);
}

namespace {

// LibVLC 3.0 renderer discovery events carry a retained VLC renderer item.
constexpr int added = 0x502, deleted = 0x503, can_render_video = 0x02;
constexpr char name_key[] = "redesign-renderer-name", id_key[] = "redesign-renderer-id";
const Output local{"Local", "", nullptr};

void set_string(Playlist *playlist, const char *key, const std::string &value) {
    constexpr int string_variable = 0x40;
    if (!var_Type(playlist, key) && var_Create(playlist, key, string_variable) != 0)
        return;
    VlcValue data{};
    data.string = const_cast<char *>(value.c_str());
    var_Set(playlist, key, data);
}

std::string playlist_string(Playlist *playlist, const char *key) {
    VlcValue data{};
    return var_Get(playlist, key, &data) == 0 ? vlc_config::take_string(data.string) : "";
}

}

void Discovery::start() {
    if (library_)
        return;
    library_.reset(new_metadata_library());
    if (!library_)
        return;

    auto receive_event = [](const Event *event, void *context) {
        auto &discovery = *static_cast<Discovery *>(context);
        auto item = event->item;
        if (!item || !(vlc_renderer_item_flags(item) & can_render_video))
            return;
        const std::lock_guard<std::mutex> lock(discovery.mutex_);
        auto &outputs = discovery.outputs_;
        if (event->type == added) {
            outputs.push_back({vlc_renderer_item_name(item), vlc_renderer_item_sout(item),
                               {vlc_renderer_item_hold(item), vlc_renderer_item_release}});
        } else {
            outputs.erase(std::remove_if(outputs.begin(), outputs.end(),
                                         [item](const Output &output) { return output.item.get() == item; }),
                          outputs.end());
        }
    };

    Description **descriptions = nullptr;
    const auto count = libvlc_renderer_discoverer_list_get(library_.get(), &descriptions);
    for (std::size_t i = 0; i < count; ++i) {
        DiscovererRef finder(libvlc_renderer_discoverer_new(library_.get(), descriptions[i]->name),
                             libvlc_renderer_discoverer_release);
        if (!finder)
            continue;
        const auto events = libvlc_renderer_discoverer_event_manager(finder.get());
        if (!libvlc_event_attach(events, added, receive_event, this) &&
            !libvlc_event_attach(events, deleted, receive_event, this) &&
            !libvlc_renderer_discoverer_start(finder.get()))
            discoverers_.push_back(std::move(finder));
    }
    libvlc_renderer_discoverer_list_release(descriptions, count);
}

std::vector<Output> Discovery::outputs() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Output> result{local};
    for (const auto &output : outputs_) {
        const auto duplicate = [&](const Output &existing) { return existing.id == output.id; };
        if (std::none_of(result.begin(), result.end(), duplicate))
            result.push_back(output);
    }
    return result;
}

Output selected(VlcPlayer player) {
    if (!player.playlist)
        return local;
    const auto id = playlist_string(player.playlist, id_key);
    return id.empty() ? local : Output{playlist_string(player.playlist, name_key), id, nullptr};
}

bool select(VlcPlayer player, const Output &output) {
    if (!player.playlist || playlist_SetRenderer(player.playlist, output.item.get()) != 0)
        return false;
    set_string(player.playlist, name_key, output.name);
    set_string(player.playlist, id_key, output.id);
    return true;
}

}
