#pragma once

#include <atomic>
#include <string>
#include <string_view>
#include <vector>

namespace opensubtitles {

struct Query {
    std::wstring title;
    int season = -1;
    int episode = -1;
};

struct File {
    std::wstring filename;
    int id = 0;
};

struct Subtitle {
    File file;
    std::wstring release;
    int season = -1;
    int episode = -1;
};

enum class Status {
    ok, missing_key, rejected_key, rate_limited, connection_error, response_error,
    download_limit, download_unavailable, cache_error,
};

struct Result {
    Status status = Status::connection_error;
    int pages = 0;
    std::vector<Subtitle> subtitles;
};

struct Download {
    Status status = Status::connection_error;
    std::wstring path{};
    int remaining = -1;
};

bool valid_key(std::wstring_view key);
bool save_key(std::wstring_view key);
Query parse_query(std::wstring_view text);
Result search(std::wstring_view query, std::wstring_view language, bool single_file, int page,
              const std::atomic_bool &cancelled);
Result search(const Query &query, std::wstring_view language, int page, const std::atomic_bool &cancelled);
Download download(int file_id, const std::atomic_bool &cancelled, bool request_allowed);

}
