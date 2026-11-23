#pragma once

#include "opensubtitles.hpp"

#include <mutex>
#include <system_error>

namespace subtitle_folder {

std::wstring default_query(std::wstring_view filename);
std::vector<std::wstring> video_names(std::wstring_view active_path, std::error_code &error,
    const std::atomic_bool &cancelled);

struct Progress {
    std::vector<std::wstring> filenames;
    std::vector<std::vector<opensubtitles::File>> matches;
    const wchar_t *folder_error = nullptr;
    opensubtitles::Status status = opensubtitles::Status::ok;
    bool finished = false;
};

class Search {
public:
    void run(std::wstring_view active_path, std::wstring_view query, std::wstring_view language);
    Progress take_progress();
    std::atomic_bool cancelled{false};

private:
    std::mutex mutex_;
    Progress progress_;
};

}
