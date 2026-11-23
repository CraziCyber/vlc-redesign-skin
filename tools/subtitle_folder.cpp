#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "subtitle_folder.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <optional>
#include <thread>
#include <tuple>
#include <unordered_set>
#include <utility>

namespace subtitle_folder {
namespace {

namespace fs = std::filesystem;
using opensubtitles::Query;
using opensubtitles::Status;

bool same_text(std::wstring_view left, std::wstring_view right) {
    if (left.empty() || right.empty())
        return left.empty() && right.empty();
    return CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(),
                                static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

bool video_file(const fs::path &path) {
    constexpr std::wstring_view extensions[]{
        L".mkv", L".mp4", L".avi", L".mov", L".webm", L".m4v", L".wmv", L".mpg", L".mpeg",
        L".ts", L".m2ts", L".mts", L".vob", L".flv", L".3gp", L".3g2", L".ogv", L".ogm",
        L".asf", L".divx", L".rm", L".rmvb", L".mxf", L".m2v", L".mpv", L".f4v",
    };
    const auto extension = path.extension().wstring();
    return std::any_of(std::begin(extensions), std::end(extensions),
                       [&extension](auto supported) { return same_text(extension, supported); });
}

std::vector<std::wstring> find_videos(const fs::path &active, std::error_code &error, const std::atomic_bool &cancelled) {
    std::vector<std::wstring> filenames;
    const auto active_name = active.filename().wstring();
    for (fs::directory_iterator file(active.parent_path(), error), end;
         file != end && !error && !cancelled.load(); file.increment(error)) {
        std::error_code file_error;
        if (!file->is_regular_file(file_error))
            continue;
        const auto name = file->path().filename().wstring();
        if (video_file(file->path()) || same_text(name, active_name))
            filenames.push_back(name);
    }
    std::sort(filenames.begin(), filenames.end(), [](const auto &left, const auto &right) {
        const auto order = CompareStringEx(LOCALE_NAME_INVARIANT, SORT_DIGITSASNUMBERS | NORM_IGNORECASE,
                                           left.c_str(), -1, right.c_str(), -1, nullptr, nullptr, 0);
        return order == CSTR_EQUAL || order == 0 ? left < right : order == CSTR_LESS_THAN;
    });
    return filenames;
}

std::wstring_view subtitle_stem(std::wstring_view filename) {
    constexpr std::wstring_view extensions[]{L".srt", L".ass", L".ssa", L".sub", L".smi", L".vtt", L".txt", L".idx"};
    const auto dot = filename.find_last_of(L'.');
    if (dot != std::wstring_view::npos && std::any_of(std::begin(extensions), std::end(extensions),
        [extension = filename.substr(dot)](auto supported) { return same_text(extension, supported); }))
        return filename.substr(0, dot);
    return filename;
}

bool different_episode(const Query &expected, const opensubtitles::Subtitle &subtitle) {
    int season = subtitle.season, episode = subtitle.episode;
    if (season < 0 || episode < 0) {
        auto parsed = opensubtitles::parse_query(subtitle.file.filename);
        if (parsed.episode < 0)
            parsed = opensubtitles::parse_query(subtitle.release);
        if (season < 0)
            season = parsed.season;
        if (episode < 0)
            episode = parsed.episode;
    }
    if (expected.season < 0)
        return episode >= 0;
    return (season >= 0 && season != expected.season) ||
           (expected.episode >= 0 && episode >= 0 && episode != expected.episode);
}

std::vector<opensubtitles::File> matches_for(const std::wstring &stem, const Query &query,
                                           const std::vector<opensubtitles::Subtitle> &subtitles) {
    std::vector<std::pair<int, const opensubtitles::File *>> ranked;
    for (const auto &subtitle : subtitles) {
        if (different_episode(query, subtitle))
            continue;
        const auto &file = subtitle.file;
        const auto name = subtitle_stem(file.filename);
        const bool exact_release = same_text(stem, name) || same_text(stem, subtitle.release);
        const std::wstring_view full_name = file.filename;
        const auto dot = full_name.find_last_of(L'.');
        const bool srt = dot != std::wstring_view::npos && same_text(full_name.substr(dot), L".srt");
        ranked.emplace_back(4 * (file.id > 0) + 2 * exact_release + srt, &file);
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](const auto &left, const auto &right) {
        return left.first > right.first;
    });
    std::vector<opensubtitles::File> matches;
    std::unordered_set<std::wstring_view> seen;
    for (const auto &item : ranked) {
        if (seen.insert(item.second->filename).second)
            matches.push_back(*item.second);
    }
    return matches;
}

}

std::wstring default_query(std::wstring_view filename) {
    const auto query = opensubtitles::parse_query(filename);
    if (query.season < 0)
        return std::wstring(filename);
    return query.title + (query.season < 10 ? L" S0" : L" S") + std::to_wstring(query.season);
}

std::vector<std::wstring> video_names(std::wstring_view active_path, std::error_code &error,
        const std::atomic_bool &cancelled) {
    return find_videos(fs::path(active_path), error, cancelled);
}

void Search::run(std::wstring_view active_path, std::wstring_view text, std::wstring_view language) {
    Status status = Status::ok;
    const wchar_t *folder_error = nullptr;
    try {
        const fs::path active(active_path);
        std::error_code error;
        auto filenames = find_videos(active, error, cancelled);
        if (error) {
            folder_error = L"Could not read the current video's folder.";
        } else if (filenames.empty()) {
            folder_error = L"No video files found in this folder.";
        } else if (!cancelled.load()) {
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                progress_.filenames = filenames;
            }
            const auto active_name = active.filename().wstring();
            const auto active_query = opensubtitles::parse_query(active.stem().wstring());
            const auto entered = opensubtitles::parse_query(text);
            std::optional<std::tuple<std::wstring, int, int>> previous_query;
            std::vector<opensubtitles::Subtitle> candidates;
            for (const auto &filename : filenames) {
                if (cancelled.load())
                    break;
                const auto stem = fs::path(filename).stem().wstring();
                auto query = opensubtitles::parse_query(stem);
                const bool same_series = active_query.episode >= 0 && query.episode >= 0 &&
                                         same_text(active_query.title, query.title);
                if (!entered.title.empty() && (same_series || same_text(active_name, filename)))
                    query.title = entered.title;
                auto key = std::make_tuple(query.title, query.season, query.episode);
                if (key != previous_query) {
                    if (previous_query)
                        std::this_thread::sleep_for(std::chrono::milliseconds(350));
                    if (cancelled.load())
                        break;
                    auto result = opensubtitles::search(query, language, 1, cancelled);
                    if (result.status != Status::ok) {
                        status = result.status;
                        break;
                    }
                    previous_query = std::move(key);
                    candidates = std::move(result.subtitles);
                }
                auto matches = matches_for(stem, query, candidates);
                const std::lock_guard<std::mutex> lock(mutex_);
                progress_.matches.push_back(std::move(matches));
            }
        }
    } catch (...) {
        status = Status::response_error;
    }
    const std::lock_guard<std::mutex> lock(mutex_);
    progress_.folder_error = folder_error;
    progress_.status = status;
    progress_.finished = true;
}

Progress Search::take_progress() {
    const std::lock_guard<std::mutex> lock(mutex_);
    return std::exchange(progress_, {});
}

}
