#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "local_subtitles.hpp"
#include "opensubtitles.hpp"

#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <memory>
#include <utility>

namespace local_subtitles {
namespace {
namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

bool same_name(std::wstring_view first, std::wstring_view second) {
    return first.size() == second.size() && (first.empty() ||
        CompareStringOrdinal(first.data(), static_cast<int>(first.size()),
                             second.data(), static_cast<int>(second.size()), TRUE) == CSTR_EQUAL);
}

std::wstring normalize(std::wstring_view name) {
    std::wstring result;
    for (const auto letter : name) {
        if (std::wstring_view(L". _-()[]{}\t").find(letter) == std::wstring_view::npos)
            result += letter;
        else if (!result.empty() && result.back() != L' ')
            result += L' ';
    }
    if (!result.empty() && result.back() == L' ')
        result.pop_back();
    return result;
}

std::wstring_view without_subtitle_tags(std::wstring_view title) {
    constexpr std::wstring_view tags[]{L"en", L"eng", L"english", L"no", L"nor", L"nob", L"nno", L"nb", L"nn",
        L"norwegian", L"norsk", L"danish", L"da", L"dan", L"swedish", L"sv", L"swe", L"finnish", L"fi", L"fin",
        L"french", L"fr", L"fra", L"german", L"de", L"deu", L"italian", L"it", L"ita", L"spanish", L"es", L"spa",
        L"portuguese", L"pt", L"por", L"sdh", L"hi", L"cc", L"forced", L"subs", L"subtitles"};
    auto split = title.rfind(L' ');
    while (split != std::wstring_view::npos) {
        const auto last = title.substr(split + 1);
        if (std::none_of(std::begin(tags), std::end(tags), [last](auto tag) { return same_name(last, tag); }))
            break;
        title = title.substr(0, split);
        split = title.rfind(L' ');
    }
    return title;
}

std::wstring_view without_year(std::wstring_view title) {
    if (title.size() > 5 && title[title.size() - 5] == L' ') {
        const auto last = title.substr(title.size() - 4);
        if ((last.substr(0, 2) == L"19" || last.substr(0, 2) == L"20") &&
            std::all_of(last.begin(), last.end(), [](auto c) { return c >= L'0' && c <= L'9'; }))
            return title.substr(0, title.size() - 5);
    }
    return title;
}

struct Filename {
    std::wstring stem, full, title;
    int season, episode;
};

Filename identify(std::wstring_view stem) {
    auto query = opensubtitles::parse_query(stem);
    return {std::wstring(stem), normalize(stem), normalize(query.title), query.season, query.episode};
}

int match_score(const Filename &media, const Filename &subtitle) {
    if (media.full.empty() || subtitle.full.empty())
        return 0;
    if (same_name(media.stem, subtitle.stem))
        return 5;
    if (same_name(media.full, subtitle.full))
        return 4;
    if (media.season != subtitle.season || media.episode != subtitle.episode)
        return 0;
    if (same_name(media.full, without_subtitle_tags(subtitle.full)))
        return 3;
    const auto title = without_subtitle_tags(subtitle.title);
    if (same_name(media.title, title))
        return 2;
    // Allow a missing release year, but keep remakes with different years apart.
    return same_name(media.title, without_year(title)) || same_name(without_year(media.title), title) ? 1 : 0;
}

struct Candidate {
    std::wstring path;
    Filename name;
};

std::vector<Candidate> candidates_in(const fs::path &directory, const std::atomic_bool &cancelled) {
    std::vector<Candidate> files;
    if (directory.empty())
        return files;
    for (const auto *subfolder : {L"", L"Subs", L"Subtitles"}) {
        if (cancelled.load())
            break;
        std::error_code error;
        const fs::path folder = subfolder[0] ? directory / subfolder : directory;
        for (fs::directory_iterator file(folder, error), end; file != end && !error && !cancelled.load();
             file.increment(error)) {
            const auto &path = file->path();
            std::error_code type_error;
            if (same_name(path.extension().native(), L".srt") && file->is_regular_file(type_error))
                files.push_back({path.native(), identify(path.stem().native())});
        }
    }
    return files;
}

struct Apartment {
    HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ~Apartment() { if (SUCCEEDED(result)) CoUninitialize(); }
};
}

bool is_srt_file(std::wstring_view filename) {
    const fs::path path(filename);
    std::error_code error;
    return same_name(path.extension().native(), L".srt") && fs::is_regular_file(path, error);
}

std::vector<std::vector<std::wstring>> find_matches(const std::vector<std::wstring> &media,
        const std::atomic_bool &cancelled) {
    std::map<fs::path, std::vector<Candidate>> folders;
    std::vector<std::vector<std::wstring>> results(media.size());
    for (std::size_t i = 0; i < media.size() && !cancelled.load(); ++i) {
        if (media[i].empty())
            continue;
        const fs::path path(media[i]);
        const auto [folder, inserted] = folders.try_emplace(path.parent_path());
        if (inserted)
            folder->second = candidates_in(folder->first, cancelled);
        const auto name = identify(path.stem().native());
        std::vector<std::pair<int, const std::wstring *>> ranked;
        for (const auto &candidate : folder->second) {
            if (cancelled.load())
                break;
            if (const int score = match_score(name, candidate.name))
                ranked.emplace_back(score, &candidate.path);
        }
        std::sort(ranked.begin(), ranked.end(), [](const auto &left, const auto &right) {
            return left.first != right.first ? left.first > right.first : *left.second < *right.second;
        });
        for (const auto &[score, match] : ranked)
            results[i].push_back(*match);
    }
    return results;
}

HRESULT browse(HWND owner, std::wstring_view media_path, std::wstring &selected) {
    const Apartment apartment;
    if (FAILED(apartment.result))
        return apartment.result;
    ComPtr<IFileOpenDialog> picker;
    HRESULT result = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&picker));
    if (FAILED(result))
        return result;
    constexpr COMDLG_FILTERSPEC filters[]{{L"SubRip subtitles (*.srt)", L"*.srt"}};
    if (FAILED(result = picker->SetFileTypes(1, filters)))
        return result;
    FILEOPENDIALOGOPTIONS options{};
    if (FAILED(result = picker->GetOptions(&options)) ||
        FAILED(result = picker->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_NOCHANGEDIR)))
        return result;
    picker->SetTitle(L"Choose Subtitle File");
    const auto folder = fs::path(media_path).parent_path();
    ComPtr<IShellItem> directory;
    if (!folder.empty() && SUCCEEDED(SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(&directory))))
        picker->SetFolder(directory.Get());
    if (FAILED(result = picker->Show(owner)))
        return result;
    ComPtr<IShellItem> file;
    if (FAILED(result = picker->GetResult(&file)))
        return result;
    PWSTR name = nullptr;
    result = file->GetDisplayName(SIGDN_FILESYSPATH, &name);
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> path(name, CoTaskMemFree);
    if (SUCCEEDED(result) && name)
        selected = name;
    return result;
}
}
