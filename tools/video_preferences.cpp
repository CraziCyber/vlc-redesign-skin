#include "video_preferences.hpp"
#include "vlc_config.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <string_view>

struct ConfigOption {
    ConfigOption *next;
    char *name, *value;
};

namespace {

// Win64 VLC 3.0.24 es_format_t and filter_t fields used by the format-only filter.
struct alignas(8) StreamFormat {
    std::byte metadata[0x38];
    std::uint32_t chroma;
    std::byte remaining[0xcc];
};

struct Picture;
struct VideoFilter {
    std::byte module_state[0x38];
    StreamFormat input, output;
    bool can_change_output;
    const char *shortcut;
    ConfigOption *options;
    Picture *(*apply)(VideoFilter *, Picture *);
};

static_assert(sizeof(StreamFormat) == 0x108 && offsetof(VideoFilter, output) == 0x140 &&
              offsetof(VideoFilter, can_change_output) == 0x248 && offsetof(VideoFilter, apply) == 0x260,
              "VLC 3.0.24 Win64 video filter layout");

struct ChromaDescription { unsigned plane_count; };

}

extern "C" {
__declspec(dllimport) char *config_ChainCreate(char **, ConfigOption **, const char *);
__declspec(dllimport) void config_ChainDestroy(ConfigOption *);
__declspec(dllimport) char *config_StringEscape(const char *);
__declspec(dllimport) const ChromaDescription *vlc_fourcc_GetChromaDescription(std::uint32_t);
__declspec(dllimport) bool es_format_IsSimilar(const StreamFormat *, const StreamFormat *);
__declspec(dllimport) void es_format_Clean(StreamFormat *);
__declspec(dllimport) int es_format_Copy(StreamFormat *, const StreamFormat *);
}

namespace video_preferences {
namespace {

constexpr char filter_key[] = "video-filter";
constexpr char crop_key[] = "crop";
constexpr CropEdge crop_order[] = {left_edge, top_edge, right_edge, bottom_edge};

enum class Type { text, real, integer };
struct Setting {
    const char *key;
    std::initializer_list<Choice> options;
    Type type = Type::text;
    bool live = false;
};

constexpr Setting settings[] = {
    {"zoom", {{L"1:4 Quarter", nullptr, 0.25f}, {L"1:2 Half", nullptr, 0.5f},
              {L"1:1 Original", nullptr, 1.0f}, {L"2:1 Double", nullptr, 2.0f}}, Type::real, true},
    {"d3d11-hdr-mode", {{L"Auto", "auto"}, {L"Off", "never"}, {L"Always HDR", "always"},
                       {L"Generate HDR from SDR", "generate"}}},
    {"preferred-resolution", {{L"Best Available", nullptr, -1}, {L"1080p", nullptr, 1080},
                              {L"720p", nullptr, 720}, {L"576p / 480p", nullptr, 576},
                              {L"360p", nullptr, 360}, {L"240p", nullptr, 240}}, Type::integer},
    {"aspect-ratio", {{L"Default", ""}, {L"16:9", "16:9"}, {L"4:3", "4:3"},
                     {L"1:1", "1:1"}, {L"16:10", "16:10"}, {L"21:9", "21:9"},
                     {L"2.35:1", "235:100"}}, Type::text, true},
    {crop_key, {{L"Default", ""}, {L"16:9", "16:9"}, {L"4:3", "4:3"},
              {L"1:1", "1:1"}, {L"16:10", "16:10"}, {L"21:9", "21:9"},
              {L"2.35:1", "235:100"}}, Type::text, true},
};

const Setting &setting(Field field) {
    return settings[static_cast<int>(field)];
}

std::string read_string(VlcPlayer player, const char *key) {
    return vlc_config::take_string(config_GetPsz(player.vlc, key));
}

template<class Apply>
void apply_live(VlcPlayer player, Apply apply) {
    if (!player.playlist)
        return;
    apply(player.playlist);
    const InputRef input(playlist_CurrentInput(player.playlist), vlc_object_release);
    const VideoOutputs outputs(input.get());
    for (std::size_t i = 0; i < outputs.count; ++i)
        apply(outputs.items[i]);
}

using ConfigOptions = std::unique_ptr<ConfigOption, decltype(&config_ChainDestroy)>;

ConfigOptions filter_options(std::string_view filter) {
    if (filter.find('{') == std::string_view::npos)
        return {nullptr, config_ChainDestroy};
    char *name = nullptr;
    ConfigOption *options = nullptr;
    const std::string clause(filter);
    libvlc_free(config_ChainCreate(&name, &options, clause.c_str()));
    libvlc_free(name);
    return {options, config_ChainDestroy};
}

const char *option_value(const ConfigOption *options, std::string_view key) {
    for (auto item = options; item; item = item->next)
        if (key == item->name)
            return item->value;
    return nullptr;
}

std::string option_value(std::string_view filter, std::string_view key, std::string fallback) {
    const auto options = filter_options(filter);
    if (const auto value = option_value(options.get(), key))
        return value;
    return fallback;
}

const char *module_name(Effect effect) {
    constexpr const char *names[] = {"adjust", "sharpen", "gradfun", "grain"};
    return names[static_cast<int>(effect)];
}

int slider_position(VlcPlayer player, const EssentialSlider &slider, const ConfigOption *options = nullptr) {
    double value = slider.effect == Effect::deband ? static_cast<double>(config_GetInt(player.vlc, slider.key)) :
                                                    config_GetFloat(player.vlc, slider.key);
    if (const auto option = option_value(options, slider.option))
        std::from_chars(option, option + std::char_traits<char>::length(option), value);
    if (!std::isfinite(value))
        return slider.initial;
    const double scaled = std::clamp(value * slider.units, double(slider.minimum), double(slider.maximum));
    return static_cast<int>(std::lround(scaled));
}

int adjusted_position(VlcPlayer player, const EssentialSlider &slider, const EssentialSlider &changed,
                      int position, const ConfigOption *options) {
    return std::string_view(slider.option) == changed.option ? position : slider_position(player, slider, options);
}

bool needs_adjustment(VlcPlayer player, const EssentialSlider &changed, int position, const ConfigOption *options) {
    if (config_GetInt(player.vlc, "brightness-threshold"))
        return true;
    for (const auto &slider : essential_sliders) {
        if (slider.effect == Effect::adjust &&
            adjusted_position(player, slider, changed, position, options) != slider.initial)
            return true;
    }
    // Retain adjustment filters with additional options set outside the sidebar.
    for (auto item = options; item; item = item->next) {
        const auto is_slider = [&](const auto &slider) {
            return slider.effect == Effect::adjust && std::string_view(item->name) == slider.option;
        };
        if (std::none_of(std::begin(essential_sliders), std::end(essential_sliders), is_slider))
            return true;
    }
    return false;
}

std::string essential_clause(VlcPlayer player, const EssentialSlider &slider, int position, std::string_view old) {
    const auto options = filter_options(old);
    if (slider.effect == Effect::adjust ? !needs_adjustment(player, slider, position, options.get()) :
                                         position == slider.minimum)
        return {};

    std::string result = module_name(slider.effect);
    result += '{';
    for (auto item = options.get(); item; item = item->next) {
        if (std::string_view(item->name) == slider.option)
            continue;
        result += item->name;
        if (item->value) {
            result += "='";
            result += vlc_config::take_string(config_StringEscape(item->value));
            result += '\'';
        }
        result += ',';
    }
    char value[32];
    const auto formatted = std::to_chars(std::begin(value), std::end(value), position / double(slider.units),
                                         std::chars_format::fixed, slider.units == 1 ? 0 : 2);
    result += slider.option;
    result += '=';
    result.append(value, formatted.ptr);
    result += '}';
    // Direct3D's adjust filters have no gamma processing. Request CPU-readable frames
    // for the native adjustment filter while gamma differs from its neutral value.
    if (slider.effect == Effect::adjust &&
        adjusted_position(player, gamma_slider, slider, position, options.get()) != gamma_slider.initial)
        result.insert(0, std::string(software_video_filter) + ':');
    return result;
}

// Keep every unrelated filter and the order of the chain, including native VLC filters with inline options.
template<class Replace>
void update_filter_chain(VlcPlayer player, Replace replace) {
    const auto configured = replace(read_string(player, filter_key));
    config_PutPsz(player.vlc, filter_key, configured.c_str());
    apply_live(player, [&](void *object) {
        VlcValue data{};
        if (var_Get(object, filter_key, &data) == 0) {
            const auto previous = vlc_config::take_string(data.string);
            auto next = replace(previous);
            if (next != previous) {
                data.string = next.data();
                var_Set(object, filter_key, data);
            }
        }
    });
}

// Both native VLC rotation filters can already be present in a saved filter chain.
bool read_transform_filter(VlcPlayer player, Transform &value, std::string_view name, std::string_view filter) {
    if (name == "rotate") {
        const auto degrees = config_GetFloat(player.vlc, "rotate-angle");
        const auto angle = option_value(filter, "angle", std::to_string(degrees));
        value.degrees = degrees;
        std::from_chars(angle.data(), angle.data() + angle.size(), value.degrees);
        return true;
    }
    if (name != "transform")
        return false;
    const auto type = option_value(filter, "type", read_string(player, "transform-type"));
    if (type == "hflip" || type == "vflip") {
        value.flip = type == "hflip" ? Flip::horizontal : Flip::vertical;
        return true;
    }
    if (type == "90" || type == "180" || type == "270") {
        value.degrees = std::stoi(type);
        return true;
    }
    return false;
}

std::string transform_filters(Transform value) {
    std::string result;
    if (value.flip != Flip::none)
        result = value.flip == Flip::horizontal ? "transform{type=hflip}" : "transform{type=vflip}";
    const int angle = static_cast<int>(std::lround(value.degrees * 10));
    if (angle == 0)
        return result;
    if (angle % 900 == 0)
        vlc_config::append_filter(result, "transform{type=" + std::to_string(angle / 10) + "}");
    else
        vlc_config::append_filter(result, "rotate{angle=" + std::to_string(angle / 10) + "." +
                                  std::to_string(angle % 10) + "}");
    return result;
}

double normalized_angle(double degrees) {
    if (!std::isfinite(degrees))
        return 0;
    const long tenths = std::lround(std::fmod(degrees, 360.0) * 10);
    return (tenths + 3600) % 3600 / 10.0;
}

}

int open_software_video_filter(void *object) {
    auto &filter = *static_cast<VideoFilter *>(object);
    const auto chroma = vlc_fourcc_GetChromaDescription(filter.input.chroma);
    // VLC retries a filter that rejects an opaque surface with its chroma conversion chain.
    if (!chroma || !chroma->plane_count)
        return -1;
    if (filter.can_change_output) {
        es_format_Clean(&filter.output);
        if (es_format_Copy(&filter.output, &filter.input) != 0)
            return -1;
    } else if (!es_format_IsSimilar(&filter.input, &filter.output)) {
        return -1;
    }
    filter.apply = [](VideoFilter *, Picture *picture) { return picture; };
    return 0;
}

Transform transform(VlcPlayer player) {
    Transform value;
    vlc_config::split_filters(read_string(player, filter_key), [&](auto name, auto filter) {
        return read_transform_filter(player, value, name, filter);
    });
    value.degrees = normalized_angle(value.degrees);
    return value;
}

bool set_transform(VlcPlayer player, Transform value) {
    if (!std::isfinite(value.degrees))
        return false;
    value.degrees = normalized_angle(value.degrees);
    const auto filters = transform_filters(value);
    update_filter_chain(player, [&](std::string_view previous) {
        auto chain = vlc_config::split_filters(previous, [&](auto name, auto filter) {
            Transform discarded;
            return read_transform_filter(player, discarded, name, filter);
        });
        vlc_config::append_filter(chain.before, filters);
        vlc_config::append_filter(chain.before, chain.after);
        return chain.before;
    });
    return vlc_config::save(player.vlc);
}

std::initializer_list<Choice> choices(Field field) {
    return setting(field).options;
}

std::optional<int> selected(VlcPlayer player, Field field) {
    const auto &config = setting(field);
    const auto text = config.type == Type::text ? read_string(player, config.key) : std::string{};
    const auto real = config.type == Type::real ? config_GetFloat(player.vlc, config.key) : 0.0f;
    const auto integer = config.type == Type::integer ? config_GetInt(player.vlc, config.key) : 0;
    int index = 0;
    for (const auto &choice : config.options) {
        if ((config.type == Type::text && text == choice.text) ||
            (config.type == Type::real && real == choice.number) ||
            (config.type == Type::integer && integer == static_cast<int>(choice.number)))
            return index;
        ++index;
    }
    return std::nullopt;
}

bool set_choice(VlcPlayer player, Field field, int index) {
    const auto &config = setting(field);
    if (index < 0 || static_cast<std::size_t>(index) >= config.options.size())
        return false;
    const auto &choice = config.options.begin()[index];
    VlcValue value{};
    switch (config.type) {
    case Type::text:
        config_PutPsz(player.vlc, config.key, choice.text);
        value.string = const_cast<char *>(choice.text);
        break;
    case Type::real:
        config_PutFloat(player.vlc, config.key, choice.number);
        value.real = choice.number;
        break;
    case Type::integer:
        value.integer = static_cast<int>(choice.number);
        config_PutInt(player.vlc, config.key, value.integer);
        break;
    }
    if (field == Field::zoom) {
        // Embedded Skins2 video fits its viewport while autoscale is enabled.
        VlcValue autoscale{};
        autoscale.boolean = value.real == 1.0f;
        config_PutInt(player.vlc, "autoscale", autoscale.boolean);
        apply_live(player, [&](void *object) {
            var_Set(object, "autoscale", autoscale);
            var_Set(object, config.key, value);
        });
    } else if (config.live) {
        apply_live(player, [&](void *object) { var_Set(object, config.key, value); });
    } else if (player.playlist) {
        var_Set(player.playlist, config.key, value);
    }
    return vlc_config::save(player.vlc);
}

CropMargins crop_margins(VlcPlayer player) {
    const auto geometry = read_string(player, crop_key);
    const char *next = geometry.data(), *end = next + geometry.size();
    CropMargins margins{};
    for (const auto edge : crop_order) {
        const auto parsed = std::from_chars(next, end, margins[edge]);
        if (parsed.ec != std::errc{} || margins[edge] < 0 || margins[edge] > max_crop_margin)
            return {};
        if (edge == bottom_edge)
            return parsed.ptr == end ? margins : CropMargins{};
        if (parsed.ptr == end || *parsed.ptr != '+')
            return {};
        next = parsed.ptr + 1;
    }
    return margins;
}

bool set_crop_margins(VlcPlayer player, const CropMargins &margins) {
    std::string geometry;
    if (margins != CropMargins{}) {
        // VLC accepts pixel borders together in left+top+right+bottom order.
        for (const auto edge : crop_order) {
            if (!geometry.empty())
                geometry += '+';
            geometry += std::to_string(margins[edge]);
        }
    }
    config_PutPsz(player.vlc, crop_key, geometry.c_str());
    VlcValue value{};
    value.string = geometry.data();
    apply_live(player, [&](void *object) { var_Set(object, crop_key, value); });
    return vlc_config::save(player.vlc);
}

EssentialPositions essential_positions(VlcPlayer player) {
    EssentialPositions result{};
    std::array<bool, std::size(essential_sliders)> active{};
    for (std::size_t i = 0; i < result.size(); ++i)
        result[i] = slider_position(player, essential_sliders[i]);
    vlc_config::for_each_filter(read_string(player, filter_key), [&](auto name, auto filter) {
        if (std::none_of(std::begin(essential_sliders), std::end(essential_sliders),
                         [&](const auto &slider) { return name == module_name(slider.effect); }))
            return;
        const auto options = filter_options(filter);
        for (std::size_t i = 0; i < result.size(); ++i) {
            if (name == module_name(essential_sliders[i].effect)) {
                result[i] = slider_position(player, essential_sliders[i], options.get());
                active[i] = true;
            }
        }
    });
    for (std::size_t i = 0; i < result.size(); ++i) {
        const auto &slider = essential_sliders[i];
        if (!active[i] && (slider.effect == Effect::sharpen ||
                          (slider.effect == Effect::deband && result[i] == slider.minimum + 1)))
            result[i] = slider.minimum;
    }
    return result;
}

void set_essential_position(VlcPlayer player, const EssentialSlider &slider, int position) {
    position = std::clamp(position, slider.minimum, slider.maximum);
    if (slider.effect == Effect::deband)
        config_PutInt(player.vlc, slider.key, std::max(position, slider.minimum + 1));
    else
        config_PutFloat(player.vlc, slider.key, position / float(slider.units));

    update_filter_chain(player, [&](std::string_view previous) {
        const std::string_view module = module_name(slider.effect);
        std::string result;
        bool found = false;
        vlc_config::for_each_filter(previous, [&](auto name, auto filter) {
            if (slider.effect == Effect::adjust && name == software_video_filter)
                return;
            if (name == module) {
                found = true;
                vlc_config::append_filter(result, essential_clause(player, slider, position, filter));
            } else
                vlc_config::append_filter(result, filter);
        });
        if (!found)
            vlc_config::append_filter(result, essential_clause(player, slider, position, {}));
        return result;
    });
}

}
