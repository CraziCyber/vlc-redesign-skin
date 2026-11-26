#include "subtitle_preferences.hpp"
#include "vlc_input.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>

namespace {

// The VLC 3.0.24 Win64 subtitle and filter ABI. Only the fields used by the
// sub-filter are represented, with the remaining format data left to VLC.
struct VideoFormat;
struct Module;
struct TextStyle {
    char *font, *mono_font;
    std::uint16_t features, flags;
    float relative_size;
    int pixel_size;
};
struct FreeType {
    std::byte renderer[0x20];
    TextStyle *forced_style;
};
struct TextSegment {
    char *text;
    TextStyle *style;
    TextSegment *next;
};
struct Region {
    std::byte format_and_picture[0xc8];
    TextSegment *text;
    std::byte region_layout[0x10];
    Region *next;
};

struct Subtitle;
struct Updater {
    int (*validate)(Subtitle *, bool, const VideoFormat *, bool, const VideoFormat *, std::int64_t);
    void (*update)(Subtitle *, const VideoFormat *, const VideoFormat *, std::int64_t);
    void (*destroy)(Subtitle *);
    void *state;
};
struct Subtitle {
    int channel;
    std::int64_t order;
    Subtitle *next;
    Region *regions;
    std::int64_t start, stop;
    bool ephemeral, fade, is_subtitle, absolute;
    int width, height, alpha;
    Updater updater;
};
struct Filter {
    const char *type;
    std::byte object[0x18];
    void *parent;
    Module *module;
    FreeType *state;
    std::byte formats[0x228];
    Subtitle *(*apply)(Filter *, Subtitle *);
};

static_assert(offsetof(Region, next) == 0xe0 && offsetof(Subtitle, height) == 0x38 && offsetof(Subtitle, updater) == 0x40 &&
              offsetof(Filter, state) == 0x30 && offsetof(Filter, apply) == 0x260 &&
              offsetof(TextStyle, pixel_size) == 0x18 && offsetof(FreeType, forced_style) == 0x20,
              "VLC 3.0.24 Win64 text renderer and sub-filter layout");

extern "C" {
__declspec(dllimport) void text_style_Delete(TextStyle *);
__declspec(dllimport) const char *module_get_object(const Module *);
__declspec(dllimport) VlcList *vlc_list_children(void *);
__declspec(dllimport) void vlc_list_release(VlcList *);
}

struct FontSize {
    int pixels = -1, divisor = 0;

    bool operator!=(FontSize other) const { return pixels != other.pixels || divisor != other.divisor; }
};
std::atomic<FontSize> requested_size{FontSize{}};
std::atomic_bool keep_formatting{true};

struct CueStyle {
    Updater original;
    void *spu;
    bool absolute;
    bool text = false;
    FontSize size;
};

void refresh_font_size(void *spu, const Subtitle &subtitle, FontSize size) {
    if (!spu || size.pixels < 0)
        return;
    const int height = std::min(subtitle.width, subtitle.height);
    if (!size.pixels && height <= 0)
        return;
    const int pixels = size.pixels > 0 ? size.pixels : subtitle_preferences::relative_font_pixels(height, size.divisor);
    // The updater runs under VLC's SPU render lock. Change FreeType's cached
    // pixel size here so it overrides the size in individual text segments.
    const std::unique_ptr<VlcList, decltype(&vlc_list_release)> children(vlc_list_children(spu), vlc_list_release);
    if (!children)
        return;
    for (int i = 0; i < children->count; ++i) {
        const auto filter = static_cast<const Filter *>(children->values[i].address);
        if (std::strcmp(filter->type, "spu text") != 0 || !filter->module ||
            std::strcmp(module_get_object(filter->module), "freetype") != 0 || !filter->state)
            continue;
        if (auto *style = filter->state->forced_style) {
            style->pixel_size = pixels;
            style->relative_size = 0.0f;
        }
        break;
    }
}

bool apply_text_style(Subtitle *subtitle) {
    const bool preserve = keep_formatting.load();
    bool has_text = false;
    for (auto *region = subtitle->regions; region; region = region->next) {
        has_text |= region->text != nullptr;
        if (!preserve) {
            for (auto *segment = region->text; segment; segment = segment->next) {
                if (segment->style) {
                    text_style_Delete(segment->style);
                    segment->style = nullptr;
                }
            }
        }
    }
    return has_text;
}

int validate(Subtitle *subtitle, bool src_changed, const VideoFormat *src,
             bool dst_changed, const VideoFormat *dst, std::int64_t stamp) {
    const auto wrapper = subtitle->updater;
    const auto &style = *static_cast<const CueStyle *>(wrapper.state);
    const bool resize = style.spu && style.text && style.size != requested_size.load();
    const bool rendered_absolute = subtitle->absolute;
    // VLC caches relative placement as absolute once drawn. Let the decoder
    // use the original alignment whenever it regenerates a text region.
    if (style.text)
        subtitle->absolute = style.absolute;
    subtitle->updater = style.original;
    const int result = subtitle->updater.validate(subtitle, src_changed, src, dst_changed, dst, stamp);
    subtitle->updater = wrapper;
    if (style.text && result == 0 && !resize)
        subtitle->absolute = rendered_absolute;
    return resize && result == 0 ? -1 : result;
}

void update(Subtitle *subtitle, const VideoFormat *src, const VideoFormat *dst, std::int64_t stamp) {
    const auto wrapper = subtitle->updater;
    auto &style = *static_cast<CueStyle *>(wrapper.state);
    subtitle->updater = style.original;
    subtitle->updater.update(subtitle, src, dst, stamp);
    subtitle->updater = wrapper;
    style.size = requested_size.load();
    style.text = apply_text_style(subtitle);
    if (style.text)
        refresh_font_size(style.spu, *subtitle, style.size);
}

void destroy(Subtitle *subtitle) {
    const std::unique_ptr<CueStyle> style(static_cast<CueStyle *>(subtitle->updater.state));
    subtitle->updater = style->original;
    if (subtitle->updater.destroy)
        subtitle->updater.destroy(subtitle);
}

Subtitle *apply(Filter *filter, Subtitle *subtitle) {
    if (!subtitle || !subtitle->is_subtitle)
        return subtitle;

    const bool text = apply_text_style(subtitle);
    const auto parent = static_cast<const Filter *>(filter->parent);
    auto *spu = parent && std::strcmp(parent->type, "subpicture") == 0 ? filter->parent : nullptr;
    // Decoders such as SubRip generate text regions on the first render and
    // again when the render size changes. Keep their own updater callbacks.
    if (subtitle->updater.validate && subtitle->updater.update) {
        auto *style = new (std::nothrow) CueStyle{subtitle->updater, spu, subtitle->absolute, text, {}};
        if (style)
            subtitle->updater = {validate, update, destroy, style};
    }
    return subtitle;
}

}

void subtitle_preferences::keep_embedded_style(bool enabled) {
    keep_formatting.store(enabled);
}

void subtitle_preferences::set_live_font_size(int pixels, int divisor) {
    requested_size.store({pixels, divisor});
}

int subtitle_preferences::open_style_filter(void *object) {
    static_cast<Filter *>(object)->apply = apply;
    return 0;
}
