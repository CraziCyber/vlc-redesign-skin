#include "video_preferences.hpp"
#include "vlc_config.hpp"

#include <charconv>
#include <cmath>
#include <string_view>

struct ConfigOption {
    ConfigOption *next;
    char *name, *value;
};

extern "C" {
__declspec(dllimport) char *config_ChainCreate(char **, ConfigOption **, const char *);
__declspec(dllimport) void config_ChainDestroy(ConfigOption *);
}

namespace video_preferences {
namespace {

constexpr char filter_key[] = "video-filter";

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
    {"crop", {{L"Default", ""}, {L"16:9", "16:9"}, {L"4:3", "4:3"},
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

std::string option_value(std::string_view filter, std::string_view key, std::string fallback) {
    if (filter.find('{') == std::string_view::npos)
        return fallback;
    char *name = nullptr;
    ConfigOption *options = nullptr;
    const std::string clause(filter);
    libvlc_free(config_ChainCreate(&name, &options, clause.c_str()));
    libvlc_free(name);
    const std::unique_ptr<ConfigOption, decltype(&config_ChainDestroy)> owned(options, config_ChainDestroy);
    for (auto item = options; item; item = item->next)
        if (item->value && key == item->name)
            return item->value;
    return fallback;
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
    const auto replace = [&](std::string_view previous) {
        auto chain = vlc_config::split_filters(previous, [&](auto name, auto filter) {
            Transform discarded;
            return read_transform_filter(player, discarded, name, filter);
        });
        vlc_config::append_filter(chain.before, filters);
        vlc_config::append_filter(chain.before, chain.after);
        return chain.before;
    };

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

}
