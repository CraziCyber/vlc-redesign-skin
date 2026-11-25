#pragma once

#include <initializer_list>
#include <optional>

#include "vlc_input.hpp"

namespace video_preferences {

enum class Flip { none, horizontal, vertical };

struct Transform {
    Flip flip = Flip::none;
    double degrees = 0;
};

enum class Field { zoom, hdr, resolution, aspect, crop };

struct Choice {
    const wchar_t *label;
    const char *text = nullptr;
    float number = 0;
};

Transform transform(VlcPlayer player);
bool set_transform(VlcPlayer player, Transform value);
std::initializer_list<Choice> choices(Field field);
std::optional<int> selected(VlcPlayer player, Field field);
bool set_choice(VlcPlayer player, Field field, int index);

}
