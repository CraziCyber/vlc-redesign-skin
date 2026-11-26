#pragma once

#include <string>
#include <string_view>

#include "vlc_input.hpp"

extern "C" {
__declspec(dllimport) std::int64_t config_GetInt(void *, const char *);
__declspec(dllimport) float config_GetFloat(void *, const char *);
__declspec(dllimport) char *config_GetPsz(void *, const char *);
__declspec(dllimport) void config_PutInt(void *, const char *, std::int64_t);
__declspec(dllimport) void config_PutFloat(void *, const char *, float);
__declspec(dllimport) void config_PutPsz(void *, const char *, const char *);
__declspec(dllimport) int config_SaveConfigFile(void *);
__declspec(dllimport) int var_Inherit(void *, const char *, int, VlcValue *);
}

namespace vlc_config {

inline std::string take_string(char *raw) {
    const std::unique_ptr<char, decltype(&libvlc_free)> value(raw, libvlc_free);
    return value ? value.get() : "";
}

inline bool save(void *vlc) {
    constexpr int boolean = 0x20;  // VLC_VAR_BOOL
    VlcValue ignore{};
    return (var_Inherit(vlc, "ignore-config", boolean, &ignore) == 0 && ignore.boolean) ||
           config_SaveConfigFile(vlc) == 0;
}

inline void append_filter(std::string &chain, std::string_view filter) {
    if (filter.empty())
        return;
    if (!chain.empty())
        chain += ':';
    chain += filter;
}

struct FilterChain {
    std::string before, selected, after;
};

template<class Visit>
void for_each_filter(std::string_view chain, Visit visit) {
    std::size_t start = 0;
    int braces = 0;
    char quote = '\0';
    for (std::size_t i = 0; i <= chain.size(); ++i) {
        const char ch = i < chain.size() ? chain[i] : '\0';
        if (i == chain.size() || (ch == ':' && !quote && !braces)) {
            const auto filter = chain.substr(start, i - start);
            const auto first = filter.find_first_not_of(" \t");
            if (first != std::string_view::npos) {
                const auto name = filter.substr(first, filter.find_first_of("{ \t", first) - first);
                visit(name, filter);
            }
            start = i + 1;
        } else if (quote) {
            if (ch == '\\' && i + 1 < chain.size())
                ++i;
            else if (ch == quote)
                quote = '\0';
        } else if (ch == '\'' || ch == '"') {
            quote = ch;
        } else if (ch == '{') {
            ++braces;
        } else if (ch == '}' && braces) {
            --braces;
        }
    }
}

template<class Match>
FilterChain split_filters(std::string_view chain, Match matches) {
    FilterChain result;
    for_each_filter(chain, [&](auto name, auto filter) {
        if (matches(name, filter))
            append_filter(result.selected, filter);
        else
            append_filter(result.selected.empty() ? result.before : result.after, filter);
    });
    return result;
}

inline FilterChain split_filter(std::string_view chain, std::string_view target) {
    return split_filters(chain, [target](std::string_view name, std::string_view) { return name == target; });
}

template<class Match>
std::string replace_filters(std::string_view previous, std::string_view selected, Match matches) {
    auto chain = split_filters(previous, matches);
    append_filter(chain.before, selected);
    append_filter(chain.before, chain.after);
    return chain.before;
}

}
