#pragma once

#include <mutex>
#include <string>
#include <vector>

#include "vlc_input.hpp"

namespace video_renderers {

struct Item;
struct Discoverer;

struct Output {
    std::string name, id;
    std::shared_ptr<Item> item;
};

class Discovery {
public:
    void start();
    std::vector<Output> outputs() const;

private:
    using DiscovererRef = std::unique_ptr<Discoverer, void (*)(Discoverer *)>;

    LibVlcRef library_{nullptr, libvlc_release};
    mutable std::mutex mutex_;
    std::vector<Output> outputs_;
    // Stop the discoverers before releasing the items that their callbacks use.
    std::vector<DiscovererRef> discoverers_;
};

Output selected(VlcPlayer player);
bool select(VlcPlayer player, const Output &output);

}
