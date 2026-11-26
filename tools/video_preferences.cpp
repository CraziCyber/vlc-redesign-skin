#include "video_preferences.hpp"
#include "vlc_config.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
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
constexpr char overlay_key[] = "sub-source";
constexpr char crop_key[] = "crop";
constexpr CropEdge crop_order[] = {left_edge, top_edge, right_edge, bottom_edge};

constexpr const char *color_modules[] = {"extract", "colorthres", "invert", "posterize", "gradient", "sepia"};
static_assert(std::size(color_modules) == static_cast<int>(ColorEffect::count));

enum class OptionType { integer, mode, flag };
struct ColorOption {
    ColorEffect effect;
    const char *key, *name;
    int minimum, maximum;
    OptionType type = OptionType::integer;
};
constexpr ColorOption color_options[] = {
    {ColorEffect::extraction, "extract-component", "component", 1, 0xffffff},
    {ColorEffect::threshold, "colorthres-color", "color", 0, 0xffffff},
    {ColorEffect::threshold, "colorthres-saturationthres", "saturationthres", 0, threshold_limit},
    {ColorEffect::threshold, "colorthres-similaritythres", "similaritythres", 0, threshold_limit},
    {ColorEffect::posterize, "posterize-level", "level", 2, 256},
    {ColorEffect::gradient, "gradient-mode", "mode", 0, int(std::size(gradient_modes)) - 1, OptionType::mode},
    {ColorEffect::gradient, "gradient-type", "type", 0, 1},
    {ColorEffect::gradient, "gradient-cartoon", "cartoon", 0, 1, OptionType::flag},
    {ColorEffect::sepia, "sepia-intensity", "intensity", 0, sepia_limit},
};
static_assert(std::size(color_options) == static_cast<int>(ColorValue::count));

struct EffectModule {
    const char *name;
    const char *chain = filter_key;
    const char *text_key = nullptr;
    const char *text_option = nullptr;
};
constexpr EffectModule effect_modules[] = {
    {"magnify"},
    {"mirror"},
    {"marq", overlay_key, "marq-marquee", "marquee"},
    {"logo", overlay_key, "logo-file", "file"},
    {"hqdn3d"},
    {"antiflicker"},
    {"motionblur"},
};
static_assert(std::size(effect_modules) == static_cast<int>(VideoEffect::count));

constexpr EffectOption effect_options[] = {
    {VideoEffect::mirror, "mirror-split", "split", 0, 1},
    {VideoEffect::mirror, "mirror-direction", "direction", 0, 1},
    {VideoEffect::text_overlay, "marq-position", "position", -1, 10},
    {VideoEffect::text_overlay, "marq-size", "size", 0, 4096},
    {VideoEffect::logo_overlay, "logo-position", "position", -1, 10},
    {VideoEffect::logo_overlay, "logo-opacity", "opacity", 0, 255},
    {VideoEffect::denoiser, "hqdn3d-luma-spat", "luma-spat", 0, 254, true},
    {VideoEffect::denoiser, "hqdn3d-chroma-spat", "chroma-spat", 0, 254, true},
    {VideoEffect::denoiser, "hqdn3d-luma-temp", "luma-temp", 0, 254, true},
    {VideoEffect::denoiser, "hqdn3d-chroma-temp", "chroma-temp", 0, 254, true},
    {VideoEffect::antiflicker, "antiflicker-window-size", "window-size", 0, 100},
    {VideoEffect::antiflicker, "antiflicker-softening-size", "softening-size", 0, 31},
    {VideoEffect::motion_blur, "blur-factor", "factor", 1, 127},
};
static_assert(std::size(effect_options) == static_cast<int>(EffectValue::count));

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

int mode_index(std::string_view value) {
    const auto choice = std::find_if(std::begin(gradient_modes), std::end(gradient_modes),
                                     [value](const Choice &mode) { return mode.text == value; });
    return choice == std::end(gradient_modes) ? 0 : static_cast<int>(choice - std::begin(gradient_modes));
}

int color_option_value(VlcPlayer player, const ColorOption &option, const ConfigOption *inline_options = nullptr) {
    if (option.type == OptionType::mode) {
        if (const auto mode = option_value(inline_options, option.name))
            return mode_index(mode);
        return mode_index(read_string(player, option.key));
    }
    auto value = config_GetInt(player.vlc, option.key);
    for (auto item = inline_options; item; item = item->next) {
        const std::string_view name = item->name;
        if (option.type == OptionType::flag) {
            if (name == option.name)
                value = 1;
            else if (name == "no-cartoon" || name == "nocartoon")
                value = 0;
        } else if (name == option.name && item->value) {
            char *end = nullptr;
            const auto parsed = std::strtoll(item->value, &end, 0);
            if (end != item->value && *end == '\0')
                value = parsed;
        }
    }
    return static_cast<int>(std::clamp<std::int64_t>(value, option.minimum, option.maximum));
}

double effect_parameter(VlcPlayer player, const EffectOption &option, const ConfigOption *inline_options = nullptr) {
    double value = option.real ? static_cast<double>(config_GetFloat(player.vlc, option.key)) :
                                static_cast<double>(config_GetInt(player.vlc, option.key));
    if (const auto text = option_value(inline_options, option.name)) {
        if (option.real) {
            double parsed = 0;
            const auto end = text + std::char_traits<char>::length(text);
            const auto result = std::from_chars(text, end, parsed);
            if (result.ec == std::errc{} && result.ptr == end)
                value = parsed;
        } else {
            char *end = nullptr;
            const auto parsed = std::strtoll(text, &end, 0);
            if (end != text && *end == '\0')
                value = static_cast<double>(parsed);
        }
    }
    return std::clamp(std::isfinite(value) ? value : double(option.minimum),
                      double(option.minimum), double(option.maximum));
}

void append_option(std::string &options, std::string_view name, const char *value) {
    if (!options.empty())
        options += ',';
    options += name;
    if (value) {
        options += "='";
        options += vlc_config::take_string(config_StringEscape(value));
        options += '\'';
    }
}

std::string numeric_option(const EffectOption &option, double value) {
    if (!option.real)
        return std::to_string(static_cast<int>(value));
    char text[64];
    const auto result = std::to_chars(std::begin(text), std::end(text), static_cast<float>(value));
    return {text, result.ptr};
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
void update_filter_chain(VlcPlayer player, Replace replace, const char *key = filter_key) {
    const auto configured = replace(read_string(player, key));
    config_PutPsz(player.vlc, key, configured.c_str());
    apply_live(player, [&](void *object) {
        VlcValue data{};
        if (var_Get(object, key, &data) == 0) {
            const auto previous = vlc_config::take_string(data.string);
            auto next = replace(previous);
            if (next != previous) {
                data.string = next.data();
                var_Set(object, key, data);
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

ColorSettings color_settings(VlcPlayer player) {
    ColorSettings result;
    for (std::size_t i = 0; i < std::size(color_options); ++i)
        result.values[i] = color_option_value(player, color_options[i]);

    vlc_config::for_each_filter(read_string(player, filter_key), [&](auto name, auto filter) {
        const auto module = std::find(std::begin(color_modules), std::end(color_modules), name);
        if (module == std::end(color_modules))
            return;
        const auto effect = static_cast<ColorEffect>(module - std::begin(color_modules));
        result.enabled(effect) = true;
        const auto options = filter_options(filter);
        for (std::size_t i = 0; i < std::size(color_options); ++i) {
            if (color_options[i].effect == effect)
                result.values[i] = color_option_value(player, color_options[i], options.get());
        }
    });
    return result;
}

void apply_color_settings(VlcPlayer player, ColorEffect effect, const ColorSettings &settings) {
    const std::string_view module = color_modules[static_cast<int>(effect)];
    std::string options;
    for (std::size_t i = 0; i < std::size(color_options); ++i) {
        const auto &option = color_options[i];
        if (option.effect != effect)
            continue;
        const int value = std::clamp(settings.values[i], option.minimum, option.maximum);
        if (!options.empty())
            options += ',';
        switch (option.type) {
        case OptionType::integer:
            config_PutInt(player.vlc, option.key, value);
            options += std::string(option.name) + '=' + std::to_string(value);
            break;
        case OptionType::mode:
            config_PutPsz(player.vlc, option.key, gradient_modes[value].text);
            options += std::string(option.name) + '=' + gradient_modes[value].text;
            break;
        case OptionType::flag:
            config_PutInt(player.vlc, option.key, value);
            // VLC parses boolean filter options by their names.
            if (!value)
                options += "no-";
            options += option.name;
            break;
        }
    }

    const std::string clause = options.empty() ? std::string(module) : std::string(module) + '{' + options + '}';
    const bool enabled = settings.enabled(effect);
    update_filter_chain(player, [&](std::string_view previous) {
        std::string next;
        bool found = false;
        vlc_config::for_each_filter(previous, [&](auto name, auto filter) {
            if (name == module) {
                if (enabled)
                    vlc_config::append_filter(next, clause);
                found = true;
            } else {
                vlc_config::append_filter(next, filter);
            }
        });
        if (enabled && !found)
            vlc_config::append_filter(next, clause);
        return next;
    });
}

const EffectOption &effect_option(EffectValue id) {
    return effect_options[static_cast<int>(id)];
}

EffectSettings effect_settings(VlcPlayer player) {
    EffectSettings result;
    for (std::size_t i = 0; i < std::size(effect_options); ++i)
        result.values[i] = effect_parameter(player, effect_options[i]);
    result.text = read_string(player, effect_modules[static_cast<int>(VideoEffect::text_overlay)].text_key);
    result.logo = read_string(player, effect_modules[static_cast<int>(VideoEffect::logo_overlay)].text_key);

    for (const auto chain : {filter_key, overlay_key}) {
        vlc_config::for_each_filter(read_string(player, chain), [&](auto name, auto filter) {
            const auto module = std::find_if(std::begin(effect_modules), std::end(effect_modules),
                [&](const EffectModule &candidate) { return name == candidate.name && chain == candidate.chain; });
            if (module == std::end(effect_modules))
                return;
            const auto effect = static_cast<VideoEffect>(module - std::begin(effect_modules));
            if (result.enabled(effect))
                return;
            result.enabled(effect) = true;
            const auto options = filter_options(filter);
            for (std::size_t i = 0; i < std::size(effect_options); ++i) {
                if (effect_options[i].effect == effect)
                    result.values[i] = effect_parameter(player, effect_options[i], options.get());
            }
            if (module->text_option) {
                if (const auto text = option_value(options.get(), module->text_option))
                    (effect == VideoEffect::text_overlay ? result.text : result.logo) = text;
            }
        });
    }
    return result;
}

void apply_effect_settings(VlcPlayer player, VideoEffect effect, const EffectSettings &settings) {
    const auto &module = effect_modules[static_cast<int>(effect)];
    std::string options;
    if (module.text_key) {
        const auto &text = effect == VideoEffect::text_overlay ? settings.text : settings.logo;
        config_PutPsz(player.vlc, module.text_key, text.c_str());
        append_option(options, module.text_option, text.c_str());
    }
    for (std::size_t i = 0; i < std::size(effect_options); ++i) {
        const auto &option = effect_options[i];
        if (option.effect != effect)
            continue;
        const auto value = settings.values[i];
        if (option.real)
            config_PutFloat(player.vlc, option.key, static_cast<float>(value));
        else
            config_PutInt(player.vlc, option.key, static_cast<int>(value));
        append_option(options, option.name, numeric_option(option, value).c_str());
    }

    const bool enabled = settings.enabled(effect);
    const auto clause = [&](std::string_view old) {
        auto combined = options;
        // Keep inline options from VLC's native preferences that the sidebar does not expose.
        const auto original = filter_options(old);
        for (auto item = original.get(); item; item = item->next) {
            const std::string_view name = item->name;
            const bool managed = (module.text_option && name == module.text_option) ||
                std::any_of(std::begin(effect_options), std::end(effect_options), [&](const auto &option) {
                    return option.effect == effect && name == option.name;
                });
            if (!managed)
                append_option(combined, item->name, item->value);
        }
        return combined.empty() ? std::string(module.name) : std::string(module.name) + '{' + combined + '}';
    };
    update_filter_chain(player, [&](std::string_view previous) {
        std::string next;
        bool found = false;
        vlc_config::for_each_filter(previous, [&](auto name, auto filter) {
            if (name == module.name) {
                if (enabled)
                    vlc_config::append_filter(next, found ? std::string(filter) : clause(filter));
                found = true;
            } else {
                vlc_config::append_filter(next, filter);
            }
        });
        if (enabled && !found)
            vlc_config::append_filter(next, clause({}));
        return next;
    }, module.chain);
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

namespace {

bool preset_video_filter(std::string_view name, std::string_view) {
    return name == "rotate" || name == "transform" || name == software_video_filter ||
        std::find(std::begin(color_modules), std::end(color_modules), name) != std::end(color_modules) ||
        std::any_of(std::begin(essential_sliders), std::end(essential_sliders),
                    [&](const auto &slider) { return name == module_name(slider.effect); }) ||
        std::any_of(std::begin(effect_modules), std::end(effect_modules),
                    [&](const auto &module) { return module.chain == filter_key && name == module.name; });
}

bool preset_overlay(std::string_view name, std::string_view) {
    return std::any_of(std::begin(effect_modules), std::end(effect_modules),
                    [&](const auto &module) { return module.chain == overlay_key && name == module.name; });
}

}

void append_preset_keys(std::vector<const char *> &keys) {
    keys.insert(keys.end(), {"autoscale", "rotate-angle", "transform-type"});
    for (const auto &setting : settings)
        keys.push_back(setting.key);
    for (const auto &slider : essential_sliders)
        keys.push_back(slider.key);
    for (const auto &option : color_options)
        keys.push_back(option.key);
    for (const auto &option : effect_options)
        keys.push_back(option.key);
    for (const auto &module : effect_modules)
        if (module.text_key)
            keys.push_back(module.text_key);
}

PresetFilters preset_filters(VlcPlayer player) {
    return {vlc_config::split_filters(read_string(player, filter_key), preset_video_filter).selected,
            vlc_config::split_filters(read_string(player, overlay_key), preset_overlay).selected};
}

void apply_preset(VlcPlayer player, const PresetFilters &filters) {
    VlcValue autoscale{};
    autoscale.boolean = config_GetInt(player.vlc, "autoscale") != 0;
    apply_live(player, [&](void *object) { var_Set(object, "autoscale", autoscale); });
    for (const auto &setting : settings) {
        VlcValue value{};
        std::string text;
        switch (setting.type) {
        case Type::text:
            text = read_string(player, setting.key);
            value.string = text.data();
            break;
        case Type::real:
            value.real = config_GetFloat(player.vlc, setting.key);
            break;
        case Type::integer:
            value.integer = config_GetInt(player.vlc, setting.key);
            break;
        }
        if (setting.live)
            apply_live(player, [&](void *object) { var_Set(object, setting.key, value); });
        else if (player.playlist)
            var_Set(player.playlist, setting.key, value);
    }
    update_filter_chain(player, [&](std::string_view previous) {
        return vlc_config::replace_filters(previous, filters.video, preset_video_filter);
    });
    update_filter_chain(player, [&](std::string_view previous) {
        return vlc_config::replace_filters(previous, filters.overlays, preset_overlay);
    }, overlay_key);
}

}
