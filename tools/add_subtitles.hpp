#pragma once

#define IDD_ADD_SUBTITLES 104
#define IDC_CLOSE_ADD_SUBTITLES 1001
#define IDC_SUBTITLE_TARGET 1002
#define IDC_SUBTITLE_TARGET_OPTIONS 1003
#define IDC_SUBTITLE_SOURCE 1004
#define IDC_SUBTITLE_SOURCE_OPTIONS 1005
#define IDC_SUBTITLE_QUERY 1006
#define IDC_SUBTITLE_LANGUAGE 1007
#define IDC_SUBTITLE_LANGUAGE_OPTIONS 1008
#define IDC_SUBTITLE_STATUS 1009
#define IDC_SUBTITLE_RESULTS 1010
#define IDC_OPENSUBTITLES_KEY 1011
#define IDC_SAVE_OPENSUBTITLES_KEY 1012
#define IDC_MORE_SUBTITLES 1013
#define IDC_SUBTITLE_FOLDER_RESULTS 1014
#define IDC_BROWSE_SUBTITLE 1015

#ifndef RC_INVOKED
#include <windows.h>
#include <string>
#include <vector>

namespace add_subtitles {
struct Media {
    std::wstring path;
    std::wstring title;
    std::string uri;
};

struct LocalAttachment {
    Media media;
    std::wstring subtitle;
    std::wstring label{};
};

using CurrentMedia = Media (*)();
using CurrentPlaylist = std::vector<Media> (*)();
using AttachSubtitles = bool (*)(const std::vector<LocalAttachment> &, bool show_tracks);
bool show(HWND player, bool light_theme, CurrentMedia query, CurrentPlaylist playlist, AttachSubtitles attach);
bool handle_key(MSG &message);
}
#endif
