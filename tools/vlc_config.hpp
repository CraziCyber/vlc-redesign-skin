#pragma once

#include <string>

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

}
