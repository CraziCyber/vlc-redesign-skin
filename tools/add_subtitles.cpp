#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define UNICODE
#include "add_subtitles.hpp"
#include "local_subtitles.hpp"
#include "opensubtitles.hpp"
#include "subtitle_folder.hpp"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <memory>
#include <thread>
#include <utility>

namespace add_subtitles {
namespace {

constexpr wchar_t title[] = L"Add Subtitles";
// The original Add Subtitles frame in reference/figma/page-metadata.xml.
constexpr LONG design_width = 1048, design_height = 800;
constexpr int margin = 18;
constexpr int button_width = 100, button_height = 32;
constexpr int header_height = 96, dropdown_margin = 9, dropdown_top = 53;
constexpr int dropdown_width = 200, dropdown_height = 34, dropdown_gap = 10;
constexpr int option_height = 46, dropdown_radius = 7;
constexpr int search_min_width = 160, search_text_left = 42;
constexpr int single_row_width = 2 * dropdown_margin + 3 * (dropdown_width + dropdown_gap) + search_min_width;
constexpr int max_result_row_height = 40, status_height = 34, key_button_width = 116;
constexpr UINT_PTR position_subclass = 1, search_timer = 1, download_timer = 2;
constexpr UINT start_local_search_message = WM_APP + 1;
thread_local HWND dialog = nullptr;
thread_local CurrentMedia current_media = nullptr;
thread_local CurrentPlaylist current_playlist = nullptr;
thread_local AttachSubtitles attach_subtitles = nullptr;
thread_local Media media;
thread_local std::wstring automatic_query;
thread_local RECT search_field{};
thread_local bool prefilling_query = false;

struct SearchJob {
    std::atomic_bool cancelled{false};
    std::atomic_bool finished{false};
    opensubtitles::Result result;
};

struct SearchView {
    std::shared_ptr<SearchJob> job;
    int requested_page = 1, page = 0, pages = 0;
    bool needs_key = false;
};
thread_local SearchView search;

struct ListInteraction {
    int hovered = -1;
    int wheel_delta = 0;
};
thread_local ListInteraction single_list, folder_list;
thread_local std::vector<opensubtitles::File> result_rows;
thread_local HWND result_tooltip = nullptr;

struct FolderRow {
    std::wstring filename;
    std::vector<std::wstring> choices;
    std::size_t selected = 0;
    Media media{};
    std::vector<opensubtitles::File> online{};

    std::size_t choice_count() const { return choices.size() + online.size(); }
};

struct FolderView {
    std::shared_ptr<subtitle_folder::Search> job;
    std::vector<FolderRow> rows;
    std::size_t completed = 0;
};
thread_local FolderView folder;

struct LocalJob {
    std::atomic_bool cancelled{false}, finished{false};
    std::vector<FolderRow> rows;
    const wchar_t *error = nullptr;
    std::size_t selected = 0;
};
thread_local std::shared_ptr<LocalJob> local_job;

struct DownloadSelection {
    Media media;
    opensubtitles::File file;
};

struct DownloadJob {
    std::atomic_bool cancelled{false}, finished{false};
    std::vector<LocalAttachment> attachments;
    std::size_t requested = 0;
    opensubtitles::Status status = opensubtitles::Status::ok;
};
thread_local std::shared_ptr<DownloadJob> download_job;

struct Dropdown {
    int button_id, options_id;
    const wchar_t *const *labels;
    std::size_t count;
    int selected = 0;
};

enum DropdownIndex { target, source, language };
constexpr int from_computer = 0;
constexpr int from_open_subtitles = 1;
constexpr int to_this_folder = 1;
constexpr int to_this_playlist = 2;
constexpr std::array target_labels{L"To This File", L"To This Folder", L"To This Playlist"};
constexpr std::array source_labels{L"From Computer", L"From OpenSubtitles", L"From Link ..."};
constexpr std::array playlist_source_labels{source_labels.front(), source_labels.back()};
constexpr wchar_t link_status[] = L"Support for subtitle links may be added in a future update.";
thread_local int other_source_selection = 0;
constexpr std::array language_labels{
    L"English", L"Danish", L"Finnish", L"French", L"German", L"Italian", L"Norwegian", L"Portuguese", L"Spanish", L"Swedish",
};
constexpr std::array language_codes{L"en", L"da", L"fi", L"fr", L"de", L"it", L"no", L"pt-pt", L"es", L"sv"};
static_assert(language_labels.size() == language_codes.size());
thread_local std::array<Dropdown, 3> dropdowns{{
    {IDC_SUBTITLE_TARGET, IDC_SUBTITLE_TARGET_OPTIONS, target_labels.data(), target_labels.size()},
    {IDC_SUBTITLE_SOURCE, IDC_SUBTITLE_SOURCE_OPTIONS, source_labels.data(), source_labels.size()},
    {IDC_SUBTITLE_LANGUAGE, IDC_SUBTITLE_LANGUAGE_OPTIONS, language_labels.data(), language_labels.size()},
}};

bool playlist_selected() {
    return dropdowns[target].selected == to_this_playlist;
}

bool open_subtitles_selected() {
    return !playlist_selected() && dropdowns[source].selected == from_open_subtitles;
}

bool folder_selected() {
    return dropdowns[target].selected == to_this_folder;
}

bool computer_selected() {
    return dropdowns[source].selected == from_computer;
}

bool paired_local_results() {
    return folder_selected() || playlist_selected();
}

bool has_local_matches() {
    return std::any_of(folder.rows.begin(), folder.rows.end(), [](const auto &row) { return !row.choices.empty(); });
}

const opensubtitles::File *selected_result(HWND window) {
    const auto index = SendDlgItemMessageW(window, IDC_SUBTITLE_RESULTS, LB_GETCURSEL, 0, 0);
    return index >= 0 && static_cast<std::size_t>(index) < result_rows.size() ? &result_rows[index] : nullptr;
}

bool has_online_selection(HWND window) {
    if (folder_selected()) {
        return std::any_of(folder.rows.begin(), folder.rows.end(), [](const FolderRow &row) {
            return !row.online.empty() && row.online[row.selected].id > 0;
        });
    }
    const auto *file = selected_result(window);
    return (!media.path.empty() || !media.uri.empty()) && file && file->id > 0;
}

void update_save_button(HWND window) {
    const bool local = computer_selected() && !local_job && has_local_matches();
    const bool online = open_subtitles_selected() && !search.needs_key && !search.job && !folder.job &&
        has_online_selection(window);
    EnableWindow(GetDlgItem(window, IDOK), !download_job && (local || online));
}

LONG header_bottom() {
    return std::max<LONG>(header_height, search_field.bottom + dropdown_margin);
}

Dropdown *find_dropdown(int control_id) {
    for (auto &dropdown : dropdowns) {
        if (control_id == dropdown.button_id || control_id == dropdown.options_id)
            return &dropdown;
    }
    return nullptr;
}

Dropdown *visible_dropdown(HWND window) {
    for (auto &dropdown : dropdowns) {
        if (IsWindowVisible(GetDlgItem(window, dropdown.options_id)))
            return &dropdown;
    }
    return nullptr;
}

struct Colors {
    COLORREF header, body, text, button, highlight, save, save_text;
    COLORREF dropdown_button, dropdown_menu;
};

constexpr Colors dark{
    RGB(0, 0, 0), RGB(17, 17, 17), RGB(151, 151, 151), RGB(26, 26, 26),
    RGB(43, 43, 43), RGB(55, 31, 6), RGB(211, 121, 24),
    RGB(22, 22, 22), RGB(24, 24, 24),
};
constexpr Colors light{
    RGB(222, 222, 222), RGB(194, 193, 193), RGB(54, 54, 54), RGB(177, 177, 177),
    RGB(159, 159, 159), RGB(216, 186, 154), RGB(138, 67, 0),
    RGB(201, 201, 201), RGB(199, 199, 199),
};

void fill(HDC dc, const RECT &area, COLORREF color) {
    SetDCBrushColor(dc, color);
    FillRect(dc, &area, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

void fill_rounded(HDC dc, const RECT &area, int radius, COLORREF color) {
    SetDCBrushColor(dc, color);
    SelectObject(dc, GetStockObject(DC_BRUSH));
    SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, area.left, area.top, area.right, area.bottom, radius * 2, radius * 2);
}

bool center_on_player(HWND window, HWND player) {
    RECT area{};
    if (!GetWindowRect(player, &area))
        return false;
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    RECT onscreen{};
    if (GetMonitorInfoW(MonitorFromWindow(player, MONITOR_DEFAULTTONEAREST), &monitor) &&
        IntersectRect(&onscreen, &area, &monitor.rcMonitor))
        area = onscreen;

    const LONG width = area.right - area.left, height = area.bottom - area.top;
    const double scale = std::min({1.0, double(width - 2 * margin) / design_width,
                                  double(height - 2 * margin) / design_height});
    if (scale <= 0)
        return false;
    const LONG w = std::lround(scale * design_width), h = std::lround(scale * design_height);
    return SetWindowPos(window, nullptr, area.left + (width - w) / 2, area.top + (height - h) / 2,
                        w, h, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

LRESULT CALLBACK follow_player(HWND player, UINT message, WPARAM wparam, LPARAM lparam,
                               UINT_PTR id, DWORD_PTR data) {
    if (message == WM_NCDESTROY)
        RemoveWindowSubclass(player, follow_player, id);
    const auto result = DefSubclassProc(player, message, wparam, lparam);
    if (message == WM_WINDOWPOSCHANGED && !IsIconic(player)) {
        const HWND window = reinterpret_cast<HWND>(data);
        const UINT flags = reinterpret_cast<const WINDOWPOS *>(lparam)->flags;
        constexpr UINT stationary = SWP_NOMOVE | SWP_NOSIZE;
        if ((flags & stationary) != stationary && IsWindowVisible(window))
            center_on_player(window, player);
    }
    return result;
}

void round_window(HWND window, int width, int height, int radius) {
    if (const HRGN shape = CreateRoundRectRgn(0, 0, width + 1, height + 1, radius * 2, radius * 2)) {
        if (!SetWindowRgn(window, shape, TRUE))
            DeleteObject(shape);
    }
}

void layout_dropdown(HWND window, const Dropdown &dropdown, int left, int top, int width, int row_height) {
    const int height = row_height * static_cast<int>(dropdown.count);
    const HWND options = GetDlgItem(window, dropdown.options_id);
    SendMessageW(options, LB_SETITEMHEIGHT, 0, row_height);
    MoveWindow(GetDlgItem(window, dropdown.button_id), left, top, width, dropdown_height, TRUE);
    MoveWindow(options, left, top, width, height, TRUE);
    round_window(options, width, height, dropdown_radius);
}

void layout_header(HWND window, int width, int height) {
    const int available = width - 2 * dropdown_margin - dropdown_gap;
    const int field_width = std::clamp(available / 2, 1, dropdown_width);
    layout_dropdown(window, dropdowns[target], dropdown_margin, dropdown_top, field_width, option_height);
    layout_dropdown(window, dropdowns[source], dropdown_margin + field_width + dropdown_gap,
                    dropdown_top, field_width, option_height);
    search_field = {};
    if (!open_subtitles_selected())
        return;

    const bool stacked = width < single_row_width;
    const int left = dropdown_margin + (stacked ? 0 : 2 * (field_width + dropdown_gap));
    const int top = dropdown_top + (stacked ? dropdown_height + dropdown_gap : 0);
    const int language_left = width - dropdown_margin - field_width;
    const int search_right = language_left - dropdown_gap;
    search_field = {left, top, search_right, top + dropdown_height};
    MoveWindow(GetDlgItem(window, IDC_SUBTITLE_QUERY), left + search_text_left, top + 8,
               std::max(1, search_right - left - search_text_left - 8), 22, TRUE);
    const int available_height = height - top - margin - button_height - dropdown_gap;
    const int language_row_height = std::clamp(available_height / static_cast<int>(language_labels.size()), 20, 34);
    layout_dropdown(window, dropdowns[language], language_left, top, field_width, language_row_height);
}

void layout_result_list(HWND list, int top, int width, int height) {
    const int available = std::max(1, height - top - 2 * margin - button_height);
    const int row_height = std::clamp(available / 15, 30, max_result_row_height);
    if (SendMessageW(list, LB_GETITEMHEIGHT, 0, 0) != row_height)
        SendMessageW(list, LB_SETITEMHEIGHT, 0, row_height);
    const int whole_rows_height = available / row_height * row_height;
    MoveWindow(list, margin, top, width, whole_rows_height ? whole_rows_height : available, TRUE);
}

void layout_results(HWND window, int width, int height) {
    const int top = header_bottom() + 12;
    const int inner_width = std::max(1, width - 2 * margin);
    const int results_top = top + status_height + 8;
    const bool action = search.page < search.pages || computer_selected();
    MoveWindow(GetDlgItem(window, IDC_SUBTITLE_STATUS), margin, top,
               std::max(1, inner_width - (action ? button_width + 10 : 0)), status_height, TRUE);
    MoveWindow(GetDlgItem(window, IDC_MORE_SUBTITLES), width - margin - button_width, top,
               button_width, button_height, TRUE);
    MoveWindow(GetDlgItem(window, IDC_BROWSE_SUBTITLE), width - margin - button_width, top,
               button_width, button_height, TRUE);
    const bool has_status = GetWindowTextLengthW(GetDlgItem(window, IDC_SUBTITLE_STATUS)) != 0;
    const int list_top = has_status || computer_selected() ? results_top : header_bottom() + 18;
    layout_result_list(GetDlgItem(window, IDC_SUBTITLE_RESULTS), list_top, inner_width, height);
    layout_result_list(GetDlgItem(window, IDC_SUBTITLE_FOLDER_RESULTS),
                       list_top + (search.needs_key ? button_height + 10 : 0), inner_width, height);

    const int key_width = std::clamp(inner_width - key_button_width - 10, 1, 340);
    MoveWindow(GetDlgItem(window, IDC_OPENSUBTITLES_KEY), margin + 8, results_top + 5,
               std::max(1, key_width - 16), 22, TRUE);
    MoveWindow(GetDlgItem(window, IDC_SAVE_OPENSUBTITLES_KEY), margin + key_width + 10, results_top,
               key_button_width, button_height, TRUE);
}

void resize_dialog(HWND window, int width, int height) {
    round_window(window, width, height, 14);
    layout_header(window, width, height);
    layout_results(window, width, height);
    const int bottom = height - margin - button_height;
    MoveWindow(GetDlgItem(window, IDCANCEL), margin, bottom, button_width, button_height, TRUE);
    MoveWindow(GetDlgItem(window, IDOK), width - margin - button_width, bottom, button_width, button_height, TRUE);
    MoveWindow(GetDlgItem(window, IDC_CLOSE_ADD_SUBTITLES), width - 41, 9, 32, 32, TRUE);
    InvalidateRect(window, nullptr, FALSE);
}

void update_search_visibility(HWND window) {
    RECT bounds{};
    GetClientRect(window, &bounds);
    layout_header(window, bounds.right, bounds.bottom);
    const int visibility = open_subtitles_selected() ? SW_SHOWNOACTIVATE : SW_HIDE;
    ShowWindow(GetDlgItem(window, IDC_SUBTITLE_QUERY), visibility);
    ShowWindow(GetDlgItem(window, IDC_SUBTITLE_LANGUAGE), visibility);
    InvalidateRect(window, nullptr, FALSE);
}

void update_results(HWND window) {
    const bool active = open_subtitles_selected();
    const bool local = computer_selected();
    const bool paired = folder_selected() || (local && playlist_selected());
    const auto show = [window](int control, bool visible) {
        const HWND widget = GetDlgItem(window, control);
        if (bool(GetWindowLongPtrW(widget, GWL_STYLE) & WS_VISIBLE) != visible) {
            constexpr UINT flags = SWP_NOSIZE | SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE;
            SetWindowPos(widget, nullptr, 0, 0, 0, 0, flags | (visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
        }
    };
    const bool has_status = GetWindowTextLengthW(GetDlgItem(window, IDC_SUBTITLE_STATUS)) != 0;
    show(IDC_SUBTITLE_STATUS, has_status);
    show(IDC_SUBTITLE_RESULTS, !paired && (local || playlist_selected() || (active && !search.needs_key)) &&
         !result_rows.empty());
    show(IDC_SUBTITLE_FOLDER_RESULTS, (local || active) && paired && !folder.rows.empty());
    show(IDC_MORE_SUBTITLES, active && !paired && !search.needs_key && !search.job && search.page < search.pages);
    show(IDC_OPENSUBTITLES_KEY, active && search.needs_key);
    show(IDC_SAVE_OPENSUBTITLES_KEY, active && search.needs_key);
    show(IDC_BROWSE_SUBTITLE, local);
    EnableWindow(GetDlgItem(window, IDC_BROWSE_SUBTITLE), local && !local_job && !folder.rows.empty());
    for (const int control : {IDC_SUBTITLE_SOURCE, IDC_SUBTITLE_TARGET, IDC_SUBTITLE_QUERY, IDC_SUBTITLE_LANGUAGE,
                              IDC_MORE_SUBTITLES, IDC_SUBTITLE_RESULTS, IDC_SUBTITLE_FOLDER_RESULTS})
        EnableWindow(GetDlgItem(window, control), !download_job);
    update_save_button(window);
    RECT bounds{};
    GetClientRect(window, &bounds);
    layout_results(window, bounds.right, bounds.bottom);
    InvalidateRect(window, nullptr, FALSE);
}

std::wstring field_text(HWND window, int control) {
    const HWND edit = GetDlgItem(window, control);
    const int length = GetWindowTextLengthW(edit);
    std::wstring text(length + 1, L'\0');
    text.resize(GetWindowTextW(edit, text.data(), length + 1));
    return text;
}

void prefill_query(HWND window) {
    if (current_media && media.path.empty() && media.title.empty())
        media = current_media();
    // Keep a manually edited query when changing the target or returning to OpenSubtitles.
    if (field_text(window, IDC_SUBTITLE_QUERY) != automatic_query)
        return;
    automatic_query = folder_selected() ? subtitle_folder::default_query(media.title) : media.title;
    prefilling_query = true;
    SetDlgItemTextW(window, IDC_SUBTITLE_QUERY, automatic_query.c_str());
    prefilling_query = false;
}

void cancel_search(HWND window) {
    KillTimer(window, search_timer);
    if (search.job)
        search.job->cancelled.store(true);
    search.job.reset();
    if (folder.job)
        folder.job->cancelled.store(true);
    folder.job.reset();
    if (local_job)
        local_job->cancelled.store(true);
    local_job.reset();
}

void clear_results(HWND window) {
    SendMessageW(result_tooltip, TTM_POP, 0, 0);
    SendDlgItemMessageW(window, IDC_SUBTITLE_RESULTS, LB_RESETCONTENT, 0, 0);
    SendDlgItemMessageW(window, IDC_SUBTITLE_FOLDER_RESULTS, LB_RESETCONTENT, 0, 0);
    result_rows.clear();
    search.page = search.pages = 0;
    folder.rows.clear();
    folder.completed = 0;
    single_list = {};
    folder_list = {};
}

void show_search_status(HWND window, const wchar_t *text) {
    SetDlgItemTextW(window, IDC_SUBTITLE_STATUS, text);
    update_results(window);
}

bool append_result(HWND list, std::wstring label, int file_id = 0) {
    if (SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str())) < 0)
        return false;
    result_rows.push_back({std::move(label), file_id});
    return true;
}

void show_playlist(HWND window) {
    const HWND list = GetDlgItem(window, IDC_SUBTITLE_RESULTS);
    try {
        if (current_playlist) {
            for (auto &track : current_playlist()) {
                if (!append_result(list, std::move(track.title))) {
                    show_search_status(window, L"Could not display all playlist tracks.");
                    return;
                }
            }
        }
    } catch (...) {
        show_search_status(window, L"Could not read the playlist.");
        return;
    }
    show_search_status(window, link_status);
}

const wchar_t *opensubtitles_error(opensubtitles::Status status) {
    using opensubtitles::Status;
    switch (status) {
    case Status::missing_key:
        return L"Enter your API key from OpenSubtitles.com under API Consumers.";
    case Status::rejected_key:
        return L"OpenSubtitles rejected your API key. Enter a valid key to retry.";
    case Status::rate_limited:
        return L"OpenSubtitles is limiting requests. Wait briefly and try again.";
    case Status::connection_error:
        return L"Could not connect to OpenSubtitles. Check your connection and try again.";
    case Status::response_error:
        return L"OpenSubtitles returned an unexpected response. Try again.";
    case Status::download_limit:
        return L"OpenSubtitles download limit reached.";
    case Status::download_unavailable:
        return L"OpenSubtitles could not provide the selected subtitle.";
    case Status::cache_error:
        return L"Could not save the subtitle in the local download cache.";
    case Status::ok:
        return L"";
    }
    return L"";
}

void set_search_error(HWND window, opensubtitles::Status status) {
    using opensubtitles::Status;
    search.needs_key = status == Status::missing_key || status == Status::rejected_key;
    show_search_status(window, opensubtitles_error(status));
}

const wchar_t *folder_subtitle(std::size_t index) {
    const auto &row = folder.rows[index];
    if (!row.choices.empty()) {
        const auto &choice = row.choices[row.selected];
        return choice.c_str() + (choice.find_last_of(L"/\\") + 1);
    }
    if (!row.online.empty())
        return row.online[row.selected].filename.c_str();
    if (index < folder.completed)
        return L"No matching subtitle";
    return folder.job ? L"Searching..." : L"Search incomplete";
}

std::wstring folder_row_label(std::size_t index) {
    return folder.rows[index].filename + L" \u2014 " + folder_subtitle(index);
}

void update_folder_labels(HWND list, std::size_t first, std::size_t end) {
    if (first == end)
        return;
    const auto top = SendMessageW(list, LB_GETTOPINDEX, 0, 0);
    const auto selected = SendMessageW(list, LB_GETCURSEL, 0, 0);
    const bool visible = IsWindowVisible(list);
    if (visible)
        SendMessageW(list, WM_SETREDRAW, FALSE, 0);
    for (auto index = first; index < end; ++index) {
        const auto label = folder_row_label(index);
        // Insert first so the previous label survives if the control runs out of space.
        if (SendMessageW(list, LB_INSERTSTRING, index, reinterpret_cast<LPARAM>(label.c_str())) >= 0)
            SendMessageW(list, LB_DELETESTRING, index + 1, 0);
    }
    SendMessageW(list, LB_SETCURSEL, selected, 0);
    SendMessageW(list, LB_SETTOPINDEX, top, 0);
    if (visible) {
        SendMessageW(list, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(list, nullptr, TRUE);
    }
}

void finish_folder_search(HWND window) {
    auto progress = folder.job->take_progress();
    const HWND list = GetDlgItem(window, IDC_SUBTITLE_FOLDER_RESULTS);
    const auto directory = std::filesystem::path(media.path).parent_path();
    for (auto &filename : progress.filenames) {
        auto path = (directory / filename).native();
        folder.rows.push_back({std::move(filename), {}, 0, {std::move(path), {}, {}}});
        const auto label = folder_row_label(folder.rows.size() - 1);
        if (SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str())) < 0) {
            folder.rows.pop_back();
            cancel_search(window);
            show_search_status(window, L"Could not display all video files in this folder.");
            return;
        }
    }
    const auto first = folder.completed;
    for (auto &matches : progress.matches)
        folder.rows[folder.completed++].online = std::move(matches);

    if (progress.finished) {
        cancel_search(window);
        update_folder_labels(list, first, folder.rows.size());
        if (progress.folder_error)
            show_search_status(window, progress.folder_error);
        else
            set_search_error(window, progress.status);
    } else {
        update_folder_labels(list, first, folder.completed);
        if (!progress.filenames.empty())
            show_search_status(window, L"");
    }
}

void find_local_subtitles(LocalJob &job, std::vector<Media> items, const Media &active, bool folder_target) {
    try {
        if (folder_target) {
            const std::filesystem::path active_path(active.path);
            std::error_code error;
            const auto names = subtitle_folder::video_names(active.path, error, job.cancelled);
            if (error) {
                job.error = L"Could not read the current video's folder.";
                return;
            }
            items.clear();
            for (const auto &name : names)
                items.push_back({(active_path.parent_path() / name).native(), name, {}});
        }

        std::vector<std::wstring> paths;
        for (const auto &item : items)
            paths.push_back(item.path);
        auto matches = local_subtitles::find_matches(paths, job.cancelled);
        for (std::size_t i = 0; i < items.size() && !job.cancelled.load(); ++i) {
            if ((!active.path.empty() && active.path == items[i].path) ||
                (!active.uri.empty() && active.uri == items[i].uri))
                job.selected = i;
            auto label = std::filesystem::path(items[i].path).filename().native();
            if (!items[i].title.empty())
                label = std::move(items[i].title);
            if (label.empty())
                label = L"Current media";
            job.rows.push_back({std::move(label), std::move(matches[i]), 0, std::move(items[i])});
        }
    } catch (...) {
        job.error = L"Could not look for local subtitles.";
    }
}

void start_local_search(HWND window) {
    try {
        auto active = current_media ? current_media() : Media{};
        const bool in_folder = folder_selected();
        std::vector<Media> items;
        if (playlist_selected() && current_playlist) {
            items = current_playlist();
            if (items.empty()) {
                show_search_status(window, L"The playlist is empty.");
                return;
            }
        } else if (in_folder ? !active.path.empty() : (!active.path.empty() || !active.uri.empty())) {
            items.push_back(active);
        } else {
            show_search_status(window, in_folder ? L"Open a local video to search its folder."
                                                 : L"Play a video or track to add subtitles.");
            return;
        }

        const auto job = std::make_shared<LocalJob>();
        std::thread([job, items = std::move(items), active = std::move(active), in_folder]() mutable {
            find_local_subtitles(*job, std::move(items), active, in_folder);
            job->finished.store(true, std::memory_order_release);
        }).detach();
        local_job = job;
        if (SetTimer(window, search_timer, 50, nullptr)) {
            show_search_status(window, L"Looking for local subtitles...");
            return;
        }
    } catch (...) {
    }
    cancel_search(window);
    show_search_status(window, L"Could not start the local subtitle search.");
}

const wchar_t *local_search_status() {
    if (folder.rows.empty())
        return L"No videos found in this folder.";
    if (paired_local_results())
        return L"Select a row to browse for its subtitle.";
    return has_local_matches() ? L"" : L"No matching .srt files found. Choose Browse...";
}

void finish_local_search(HWND window) {
    if (!local_job->finished.load(std::memory_order_acquire))
        return;
    auto rows = std::move(local_job->rows);
    const auto error = local_job->error;
    const auto selected = local_job->selected;
    cancel_search(window);
    const bool paired = paired_local_results();
    const HWND list = GetDlgItem(window, paired ? IDC_SUBTITLE_FOLDER_RESULTS : IDC_SUBTITLE_RESULTS);
    folder.rows = std::move(rows);
    if (paired) {
        folder.completed = folder.rows.size();
        for (std::size_t i = 0; i < folder.rows.size(); ++i) {
            const auto label = folder_row_label(i);
            if (SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str())) < 0) {
                folder.rows.resize(i);
                break;
            }
        }
        if (!folder.rows.empty())
            SendMessageW(list, LB_SETCURSEL, selected < folder.rows.size() ? selected : 0, 0);
    } else if (!folder.rows.empty()) {
        auto &choices = folder.rows.front().choices;
        for (std::size_t i = 0; i < choices.size(); ++i) {
            if (!append_result(list, std::filesystem::path(choices[i]).filename().native())) {
                choices.resize(i);
                break;
            }
        }
        if (!choices.empty())
            SendMessageW(list, LB_SETCURSEL, 0, 0);
    }
    show_search_status(window, error ? error : local_search_status());
}

void browse_subtitle(HWND window) {
    if (!computer_selected() || local_job || folder.rows.empty())
        return;
    const bool paired = paired_local_results();
    const HWND list = GetDlgItem(window, paired ? IDC_SUBTITLE_FOLDER_RESULTS : IDC_SUBTITLE_RESULTS);
    const auto selected = SendMessageW(list, LB_GETCURSEL, 0, 0);
    const auto index = paired && selected >= 0 ? static_cast<std::size_t>(selected) : 0;
    auto &row = folder.rows[index];
    std::wstring path;
    const auto result = local_subtitles::browse(window, row.media.path, path);
    if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED) || !IsWindow(window))
        return;
    if (FAILED(result)) {
        show_search_status(window, L"Could not open the subtitle file picker.");
        return;
    }
    if (!local_subtitles::is_srt_file(path)) {
        show_search_status(window, L"Choose an existing .srt file.");
        return;
    }
    const auto previous = std::find(row.choices.begin(), row.choices.end(), path);
    const auto choice = static_cast<std::size_t>(previous - row.choices.begin());
    if (previous == row.choices.end()) {
        if (!paired && !append_result(list, std::filesystem::path(path).filename().native())) {
            show_search_status(window, L"Could not display the selected subtitle.");
            return;
        }
        row.choices.push_back(std::move(path));
    }
    if (paired) {
        row.selected = choice;
        update_folder_labels(list, index, index + 1);
    }
    show_search_status(window, local_search_status());
    // The first manual subtitle can make the list visible for the first time.
    // Select and reveal the chosen row after the dialog has laid it out.
    SendMessageW(list, LB_SETCURSEL, paired ? index : choice, 0);
    InvalidateRect(list, nullptr, FALSE);
    SetFocus(list);
}

void save_local_selection(HWND window) {
    if (!computer_selected() || local_job || !attach_subtitles)
        return;
    try {
        std::vector<LocalAttachment> chosen;
        if (paired_local_results()) {
            for (const auto &row : folder.rows) {
                if (!row.choices.empty())
                    chosen.push_back({row.media, row.choices[row.selected]});
            }
        } else if (!folder.rows.empty()) {
            const auto index = SendDlgItemMessageW(window, IDC_SUBTITLE_RESULTS, LB_GETCURSEL, 0, 0);
            const auto &row = folder.rows.front();
            if (index >= 0 && static_cast<std::size_t>(index) < row.choices.size())
                chosen.push_back({row.media, row.choices[index]});
        }
        if (chosen.empty())
            return;
        if (std::any_of(chosen.begin(), chosen.end(), [](const auto &file) {
                return !local_subtitles::is_srt_file(file.subtitle);
            })) {
            show_search_status(window, L"A selected subtitle is unavailable. Choose Browse... to replace it.");
            return;
        }
        if (attach_subtitles(chosen, true)) {
            DestroyWindow(window);
            return;
        }
    } catch (...) {
    }
    show_search_status(window, L"Could not add the selected subtitles.");
}

void download_selected(DownloadJob &job, const std::vector<DownloadSelection> &chosen) {
    bool request_allowed = true;
    try {
        for (const auto &selection : chosen) {
            if (job.cancelled.load())
                break;
            auto result = opensubtitles::download(selection.file.id, job.cancelled, request_allowed);
            if (result.status == opensubtitles::Status::download_limit) {
                request_allowed = false;
                job.status = result.status;
                continue;  // Remaining selections may already be cached.
            }
            if (result.status != opensubtitles::Status::ok) {
                job.status = result.status;
                break;
            }
            if (result.remaining == 0)
                request_allowed = false;
            auto label = selection.file.filename;
            std::replace_if(label.begin(), label.end(), [](wchar_t character) { return character < L' '; }, L' ');
            job.attachments.push_back({selection.media, std::move(result.path), std::move(label)});
        }
    } catch (...) {
        job.status = opensubtitles::Status::response_error;
    }
    job.finished.store(true, std::memory_order_release);
}

void start_download(HWND window) {
    if (download_job || search.job || folder.job || search.needs_key || !attach_subtitles)
        return;
    try {
        std::vector<DownloadSelection> chosen;
        if (folder_selected()) {
            for (const auto &row : folder.rows) {
                if (!row.online.empty() && row.online[row.selected].id > 0)
                    chosen.push_back({row.media, row.online[row.selected]});
            }
            const auto playing = current_media ? current_media() : media;
            const auto first = std::find_if(chosen.begin(), chosen.end(), [&](const DownloadSelection &entry) {
                return CompareStringOrdinal(entry.media.path.c_str(), -1, playing.path.c_str(), -1, TRUE) == CSTR_EQUAL;
            });
            if (first != chosen.end())
                std::rotate(chosen.begin(), first, first + 1);
        } else if (const auto *file = selected_result(window);
                   file && file->id > 0 && (!media.path.empty() || !media.uri.empty())) {
            chosen.push_back({media, *file});
        }
        if (chosen.empty())
            return;

        const auto job = std::make_shared<DownloadJob>();
        job->requested = chosen.size();
        if (SetTimer(window, download_timer, 50, nullptr)) {
            std::thread([job, chosen = std::move(chosen)] { download_selected(*job, chosen); }).detach();
            download_job = job;
            show_search_status(window, job->requested == 1 ? L"Downloading subtitle..." : L"Downloading subtitles...");
            return;
        }
    } catch (...) {
    }
    KillTimer(window, download_timer);
    show_search_status(window, L"Could not start the subtitle download.");
}

void finish_download(HWND window) {
    if (!download_job || !download_job->finished.load(std::memory_order_acquire))
        return;
    KillTimer(window, download_timer);
    const auto job = std::exchange(download_job, {});
    const bool complete = job->status == opensubtitles::Status::ok;
    try {
        if (!job->attachments.empty()) {
            if (!attach_subtitles || !attach_subtitles(job->attachments, complete)) {
                show_search_status(window, L"Could not add the downloaded subtitles.");
                return;
            }
            if (complete) {
                DestroyWindow(window);
                return;
            }
        }
    } catch (...) {
        show_search_status(window, L"Could not add the downloaded subtitles.");
        return;
    }

    using opensubtitles::Status;
    search.needs_key = job->status == Status::missing_key || job->status == Status::rejected_key;
    auto message = std::wstring(opensubtitles_error(job->status));
    if (!job->attachments.empty()) {
        message = L"Added " + std::to_wstring(job->attachments.size()) + L" of " +
                  std::to_wstring(job->requested) + L" subtitles. " + message;
    }
    show_search_status(window, message.c_str());
}

void start_search(HWND window, int page = 1) {
    if (download_job)
        return;
    cancel_search(window);
    if (page == 1)
        clear_results(window);
    search.needs_key = false;
    if (!open_subtitles_selected()) {
        SetDlgItemTextW(window, IDC_OPENSUBTITLES_KEY, L"");
        if (computer_selected())
            start_local_search(window);
        else if (playlist_selected())
            show_playlist(window);
        else
            show_search_status(window, link_status);
        return;
    }
    if (folder_selected() && media.path.empty()) {
        show_search_status(window, L"Open a local video to search its folder.");
        return;
    }
    auto text = field_text(window, IDC_SUBTITLE_QUERY);
    if (!folder_selected() && text.find_first_not_of(L" \t\r\n") == std::wstring::npos) {
        show_search_status(window, L"Enter a title to search OpenSubtitles.");
        return;
    }

    try {
        const auto code = language_codes[dropdowns[language].selected];
        if (folder_selected()) {
            const auto job = std::make_shared<subtitle_folder::Search>();
            std::thread([job, path = media.path, text = std::move(text), code] { job->run(path, text, code); }).detach();
            folder.job = job;
        } else {
            const auto job = std::make_shared<SearchJob>();
            std::thread([job, text = std::move(text), code, page] {
                try {
                    job->result = opensubtitles::search(text, code, true, page, job->cancelled);
                } catch (...) {
                    job->result.status = opensubtitles::Status::response_error;
                }
                job->finished.store(true, std::memory_order_release);
            }).detach();
            search.job = job;
            search.requested_page = page;
        }
        if (SetTimer(window, search_timer, 50, nullptr)) {
            show_search_status(window, L"Searching OpenSubtitles...");
            return;
        }
        cancel_search(window);
    } catch (...) {
        cancel_search(window);
    }
    show_search_status(window, L"Could not start the subtitle search. Try again.");
}

void finish_search(HWND window) {
    if (local_job) {
        finish_local_search(window);
        return;
    }
    if (folder.job) {
        finish_folder_search(window);
        return;
    }
    if (!search.job || !search.job->finished.load(std::memory_order_acquire))
        return;
    auto result = std::move(search.job->result);
    cancel_search(window);
    using opensubtitles::Status;
    if (result.status == Status::ok) {
        search.page = search.requested_page;
        search.pages = result.pages;
        const HWND list = GetDlgItem(window, IDC_SUBTITLE_RESULTS);
        for (auto &row : result.subtitles) {
            if (!append_result(list, std::move(row.file.filename), row.file.id))
                break;
        }
        if (result_rows.empty()) {
            show_search_status(window, L"No subtitles found. Try another title or language.");
        } else {
            const auto count = std::to_wstring(result_rows.size()) + (result_rows.size() == 1 ? L" subtitle shown" : L" subtitles shown");
            show_search_status(window, count.c_str());
        }
        return;
    }
    set_search_error(window, result.status);
}

void remember_key(HWND window) {
    auto key = field_text(window, IDC_OPENSUBTITLES_KEY);
    const auto first = key.find_first_not_of(L" \t\r\n");
    const auto value = first == std::wstring::npos ? std::wstring_view{} :
        std::wstring_view(key).substr(first, key.find_last_not_of(L" \t\r\n") - first + 1);
    const bool valid = opensubtitles::valid_key(value);
    const bool saved = valid && opensubtitles::save_key(value);
    SecureZeroMemory(key.data(), key.size() * sizeof(wchar_t));
    if (!valid) {
        show_search_status(window, L"Enter the API key from your OpenSubtitles.com account.");
        return;
    }
    if (!saved) {
        show_search_status(window, L"Windows could not save the API key. Try again.");
        return;
    }
    SetDlgItemTextW(window, IDC_OPENSUBTITLES_KEY, L"");
    SetFocus(GetDlgItem(window, IDC_SUBTITLE_QUERY));
    start_search(window);
}

void close_dropdown(HWND window, const Dropdown &dropdown, bool restore_focus) {
    const HWND options = GetDlgItem(window, dropdown.options_id);
    if (!IsWindowVisible(options))
        return;
    const HWND button = GetDlgItem(window, dropdown.button_id);
    ShowWindow(button, SW_SHOWNOACTIVATE);
    ShowWindow(options, SW_HIDE);
    if (restore_focus)
        SetFocus(button);
}

void open_dropdown(HWND window, const Dropdown &dropdown) {
    if (const auto *other = visible_dropdown(window)) {
        if (other == &dropdown)
            return;
        close_dropdown(window, *other, false);
    }
    const HWND options = GetDlgItem(window, dropdown.options_id);
    SendMessageW(options, LB_SETCURSEL, dropdown.selected, 0);
    if (SetWindowPos(options, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW)) {
        SetFocus(options);
        ShowWindow(GetDlgItem(window, dropdown.button_id), SW_HIDE);
    }
}

void fill_dropdown(HWND window, const Dropdown &dropdown) {
    SetDlgItemTextW(window, dropdown.button_id, dropdown.labels[dropdown.selected]);
    const HWND options = GetDlgItem(window, dropdown.options_id);
    SendMessageW(options, LB_RESETCONTENT, 0, 0);
    for (std::size_t i = 0; i < dropdown.count; ++i)
        SendMessageW(options, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(dropdown.labels[i]));
}

void update_source_options(HWND window) {
    auto &options = dropdowns[source];
    const auto *labels = playlist_selected() ? playlist_source_labels.data() : source_labels.data();
    if (options.labels == labels)
        return;
    options.labels = labels;
    options.count = playlist_selected() ? playlist_source_labels.size() : source_labels.size();
    std::swap(options.selected, other_source_selection);
    fill_dropdown(window, options);
}

void choose_option(HWND window, Dropdown &dropdown, LRESULT row) {
    if (row < 0 || row >= static_cast<LRESULT>(dropdown.count))
        return;
    dropdown.selected = static_cast<int>(row);
    SetDlgItemTextW(window, dropdown.button_id, dropdown.labels[row]);
    close_dropdown(window, dropdown, true);
    if (&dropdown == &dropdowns[target])
        update_source_options(window);
    if (&dropdown != &dropdowns[language]) {
        update_search_visibility(window);
        if (open_subtitles_selected())
            prefill_query(window);
    }
    start_search(window);
}

LRESULT CALLBACK dropdown_proc(HWND options, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR data) {
    if (message == WM_ERASEBKGND)
        return TRUE;  // The owner-drawn rows paint the entire menu.
    // Leave the list's native mouse and keyboard selection in place until a choice is confirmed.
    if (message == WM_NCDESTROY)
        RemoveWindowSubclass(options, dropdown_proc, id);
    const auto result = DefSubclassProc(options, message, wparam, lparam);
    auto &dropdown = *reinterpret_cast<Dropdown *>(data);
    if (message == WM_MOUSEMOVE || message == WM_LBUTTONUP) {
        const auto hit = SendMessageW(options, LB_ITEMFROMPOINT, 0, lparam);
        if (!HIWORD(hit)) {
            if (message == WM_LBUTTONUP)
                choose_option(GetParent(options), dropdown, LOWORD(hit));
            else if (SendMessageW(options, LB_GETCURSEL, 0, 0) != LOWORD(hit))
                SendMessageW(options, LB_SETCURSEL, LOWORD(hit), 0);
        }
    } else if (message == WM_KILLFOCUS || message == WM_CANCELMODE) {
        close_dropdown(GetParent(options), dropdown, false);
    }
    return result;
}

void init_dropdowns(HWND window) {
    dropdowns[source].labels = source_labels.data();
    dropdowns[source].count = source_labels.size();
    other_source_selection = 0;
    for (auto &dropdown : dropdowns) {
        dropdown.selected = 0;
        const HWND options = GetDlgItem(window, dropdown.options_id);
        ShowWindow(options, SW_HIDE);
        SendMessageW(options, LB_SETITEMHEIGHT, 0, option_height);
        fill_dropdown(window, dropdown);
        SetWindowSubclass(options, dropdown_proc, 1, reinterpret_cast<DWORD_PTR>(&dropdown));
    }
}

void draw_stroke(HDC dc, COLORREF color, const POINT (&points)[3]) {
    if (const HPEN pen = CreatePen(PS_SOLID, 2, color)) {
        const HGDIOBJ previous = SelectObject(dc, pen);
        Polyline(dc, points, 3);
        SelectObject(dc, previous);
        DeleteObject(pen);
    }
}

void paint_option(const DRAWITEMSTRUCT &item, const Colors &colors, const Dropdown &dropdown) {
    if (item.itemID >= dropdown.count)
        return;
    const HDC dc = item.hDC;
    const int saved = SaveDC(dc);
    const bool selected = item.itemID == static_cast<UINT>(dropdown.selected);
    const bool highlighted = !selected && (item.itemState & ODS_SELECTED);
    RECT menu{};
    GetClientRect(item.hwndItem, &menu);
    const auto &row = item.rcItem;
    IntersectClipRect(dc, row.left, row.top, row.right, row.bottom);
    // Each row paints its portion of the same rounded menu over the dialog colors.
    POINT origin{};
    MapWindowPoints(item.hwndItem, GetParent(item.hwndItem), &origin, 1);
    const LONG header_end = std::clamp(header_bottom() - origin.y, 0L, menu.bottom);
    fill(dc, {0, 0, menu.right, header_end}, colors.header);
    fill(dc, {0, header_end, menu.right, menu.bottom}, colors.body);
    const RECT outline{0, 0, menu.right + 1, menu.bottom + 1};
    fill_rounded(dc, outline, dropdown_radius, highlighted ? colors.highlight : colors.dropdown_menu);
    const auto font = reinterpret_cast<HFONT>(SendMessageW(item.hwndItem, WM_GETFONT, 0, 0));
    if (font)
        SelectObject(dc, font);
    SetTextColor(dc, colors.text);
    SetBkMode(dc, TRANSPARENT);
    RECT label = item.rcItem;
    label.left += 15;
    label.right -= 39;
    DrawTextW(dc, dropdown.labels[item.itemID], -1, &label, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (selected) {
        const LONG x = item.rcItem.right - 26, y = (item.rcItem.top + item.rcItem.bottom) / 2;
        draw_stroke(dc, colors.save_text, {{x - 7, y}, {x - 2, y + 5}, {x + 8, y - 7}});
    }
    RestoreDC(dc, saved);
}

RECT subtitle_cell(const RECT &row) {
    return {(row.left + row.right) / 2 + 1, row.top + 2, row.right - 2, row.bottom - 2};
}

void stroke(HDC dc, LONG from_x, LONG from_y, LONG to_x, LONG to_y) {
    MoveToEx(dc, from_x, from_y, nullptr);
    LineTo(dc, to_x, to_y);
}

void paint_video_icon(HDC dc, LONG x, LONG y) {
    RoundRect(dc, x, y - 8, x + 18, y + 9, 5, 5);
    stroke(dc, x + 1, y - 3, x + 17, y - 3);
    stroke(dc, x + 5, y - 8, x + 8, y - 3);
    stroke(dc, x + 11, y - 8, x + 14, y - 3);
}

void paint_document_icon(HDC dc, LONG x, LONG y) {
    const POINT page[]{{x, y + 6}, {x, y - 4}, {x + 5, y - 9}, {x + 14, y - 9},
                       {x + 17, y - 7}, {x + 17, y + 7}, {x + 15, y + 9}, {x + 2, y + 9}, {x, y + 6}};
    Polyline(dc, page, std::size(page));
    const POINT fold[]{{x, y - 3}, {x + 6, y - 3}, {x + 6, y - 8}};
    Polyline(dc, fold, std::size(fold));
    stroke(dc, x + 4, y + 1, x + 13, y + 1);
    stroke(dc, x + 4, y + 5, x + 11, y + 5);
}

void paint_music_icon(HDC dc, LONG x, LONG y) {
    Ellipse(dc, x + 2, y + 2, x + 10, y + 9);
    stroke(dc, x + 9, y + 5, x + 9, y - 8);
    const POINT flag[]{{x + 9, y - 8}, {x + 16, y - 3}, {x + 16, y}};
    Polyline(dc, flag, std::size(flag));
}

void paint_swap_icon(HDC dc, LONG x, LONG y) {
    stroke(dc, x - 14, y - 4, x, y - 4);
    stroke(dc, x - 14, y + 4, x, y + 4);
    const POINT top[]{{x - 9, y - 8}, {x - 14, y - 4}, {x - 9, y}};
    const POINT bottom[]{{x - 5, y}, {x, y + 4}, {x - 5, y + 8}};
    Polyline(dc, top, std::size(top));
    Polyline(dc, bottom, std::size(bottom));
}

bool highlighted_row(const DRAWITEMSTRUCT &item, const ListInteraction &interaction) {
    return static_cast<int>(item.itemID) == interaction.hovered ||
           ((item.itemState & ODS_SELECTED) && GetFocus() == item.hwndItem);
}

COLORREF row_highlight(const Colors &colors) {
    return colors.body == dark.body ? colors.header : colors.highlight;
}

void set_row_font(HWND list, HDC dc) {
    if (const auto font = reinterpret_cast<HFONT>(SendMessageW(list, WM_GETFONT, 0, 0)))
        SelectObject(dc, font);
}

void prepare_row(const DRAWITEMSTRUCT &item, const Colors &colors) {
    const HDC dc = item.hDC;
    SelectObject(dc, GetStockObject(DC_PEN));
    SetDCPenColor(dc, colors.text);
    SelectObject(dc, GetStockObject(NULL_BRUSH));
    set_row_font(item.hwndItem, dc);
    SetTextColor(dc, colors.text);
    SetBkMode(dc, TRANSPARENT);
}

constexpr UINT row_text_flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS;

RECT single_label(const RECT &row) {
    const bool selectable = computer_selected() || open_subtitles_selected();
    return {row.left + 41, row.top, row.right - (selectable ? 40 : 12), row.bottom};
}

void paint_single_row(const DRAWITEMSTRUCT &item, const Colors &colors) {
    const HDC dc = item.hDC;
    const auto &bounds = item.rcItem;
    fill(dc, bounds, colors.body);
    if (item.itemID >= result_rows.size())
        return;
    const int saved = SaveDC(dc);
    const auto &file = result_rows[item.itemID];
    const bool chosen = (computer_selected() || (open_subtitles_selected() && file.id > 0)) &&
        (item.itemState & ODS_SELECTED);
    if (chosen || highlighted_row(item, single_list))
        fill_rounded(dc, {bounds.left + 1, bounds.top + 2, bounds.right - 2, bounds.bottom - 2}, 7, row_highlight(colors));

    prepare_row(item, colors);
    const LONG y = (bounds.top + bounds.bottom) / 2;
    if (playlist_selected())
        paint_music_icon(dc, bounds.left + 8, y);
    else
        paint_document_icon(dc, bounds.left + 8, y);
    if (chosen) {
        const LONG x = bounds.right - 19;
        draw_stroke(dc, colors.save_text, {{x - 7, y}, {x - 2, y + 5}, {x + 8, y - 7}});
    }

    RECT label = single_label(bounds);
    DrawTextW(dc, file.filename.c_str(), -1, &label, row_text_flags);
    RestoreDC(dc, saved);
}

void paint_folder_row(const DRAWITEMSTRUCT &item, const Colors &colors) {
    const HDC dc = item.hDC;
    const auto &bounds = item.rcItem;
    fill(dc, bounds, colors.body);
    if (item.itemID >= folder.rows.size())
        return;
    const int saved = SaveDC(dc);
    const auto &row = folder.rows[item.itemID];
    const RECT cell = subtitle_cell(bounds);
    const LONG y = (bounds.top + bounds.bottom) / 2;
    const bool highlighted = row.choice_count() != 0 && highlighted_row(item, folder_list);
    if (computer_selected() && (item.itemState & ODS_SELECTED))
        fill_rounded(dc, {bounds.left + 1, bounds.top + 2, cell.left - 5, bounds.bottom - 2}, 7, row_highlight(colors));
    if (highlighted)
        fill_rounded(dc, cell, 7, row_highlight(colors));

    prepare_row(item, colors);
    if (playlist_selected())
        paint_music_icon(dc, bounds.left + 6, y);
    else
        paint_video_icon(dc, bounds.left + 6, y);
    paint_document_icon(dc, cell.left + 11, y);
    if (highlighted && row.choice_count() > 1)
        paint_swap_icon(dc, cell.right - 14, y);

    RECT filename{bounds.left + 41, bounds.top, cell.left - 52, bounds.bottom};
    DrawTextW(dc, row.filename.c_str(), -1, &filename, row_text_flags);
    SIZE text_size{};
    GetTextExtentPoint32W(dc, row.filename.c_str(), static_cast<int>(row.filename.size()), &text_size);
    SetDCPenColor(dc, colors.body == dark.body ? RGB(57, 57, 57) : RGB(133, 132, 132));
    const LONG dash_start = std::min(filename.left + text_size.cx, filename.right) + 12;
    for (LONG x = dash_start; x < cell.left - 16; x += 16)
        stroke(dc, x, y, std::min(x + 8, cell.left - 16), y);

    RECT subtitle{cell.left + 45, bounds.top, cell.right - 36, bounds.bottom};
    DrawTextW(dc, folder_subtitle(item.itemID), -1, &subtitle, row_text_flags);
    RestoreDC(dc, saved);
}

void cycle_subtitle(HWND list, LRESULT index) {
    if (download_job || index < 0 || static_cast<std::size_t>(index) >= folder.rows.size())
        return;
    auto &row = folder.rows[index];
    if (row.choice_count() > 1) {
        row.selected = (row.selected + 1) % row.choice_count();
        update_folder_labels(list, index, index + 1);
        update_save_button(GetParent(list));
    }
}

int result_hit(HWND list, LPARAM position, bool paired) {
    const auto hit = SendMessageW(list, LB_ITEMFROMPOINT, 0, position);
    RECT row{};
    if (HIWORD(hit) || SendMessageW(list, LB_GETITEMRECT, LOWORD(hit), reinterpret_cast<LPARAM>(&row)) == LB_ERR)
        return -1;
    const POINT pointer{GET_X_LPARAM(position), GET_Y_LPARAM(position)};
    const RECT cell = paired ? subtitle_cell(row) : row;
    return PtInRect(&cell, pointer) ? LOWORD(hit) : -1;
}

LRESULT CALLBACK results_list_proc(HWND list, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR) {
    const bool paired = GetDlgCtrlID(list) == IDC_SUBTITLE_FOLDER_RESULTS;
    auto &interaction = paired ? folder_list : single_list;
    switch (message) {
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, list, 0};
        TrackMouseEvent(&tracking);
        const int hit = result_hit(list, lparam, paired);
        if (std::exchange(interaction.hovered, hit) != hit) {
            if (!paired)
                SendMessageW(result_tooltip, TTM_POP, 0, 0);
            InvalidateRect(list, nullptr, FALSE);
        }
        break;
    }
    case WM_MOUSELEAVE:
        interaction.hovered = -1;
        InvalidateRect(list, nullptr, FALSE);
        break;
    case WM_MOUSEWHEEL: {
        RECT bounds{};
        GetClientRect(list, &bounds);
        const auto row_height = std::max(1L, static_cast<LONG>(SendMessageW(list, LB_GETITEMHEIGHT, 0, 0)));
        const int visible = std::max(1L, bounds.bottom / row_height);
        const auto count = static_cast<int>(SendMessageW(list, LB_GETCOUNT, 0, 0));
        const int last = std::max(0, count - visible);
        UINT lines = 3;
        SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
        const int step = lines == WHEEL_PAGESCROLL ? visible : static_cast<int>(lines);
        const int top = static_cast<int>(SendMessageW(list, LB_GETTOPINDEX, 0, 0));
        interaction.wheel_delta += GET_WHEEL_DELTA_WPARAM(wparam);
        const int next = std::clamp(top - step * (interaction.wheel_delta / WHEEL_DELTA), 0, last);
        interaction.wheel_delta %= WHEEL_DELTA;
        if (next != top) {
            interaction.hovered = -1;
            if (!paired)
                SendMessageW(result_tooltip, TTM_POP, 0, 0);
            SendMessageW(list, LB_SETTOPINDEX, next, 0);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        const auto result = DefSubclassProc(list, message, wparam, lparam);
        if (paired)
            cycle_subtitle(list, result_hit(list, lparam, true));
        return result;
    }
    case WM_NCDESTROY:
        RemoveWindowSubclass(list, results_list_proc, id);
        break;
    }
    return DefSubclassProc(list, message, wparam, lparam);
}

HWND create_result_tooltip(HWND window, HWND list) {
    const HWND tip = CreateWindowExW(0, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX,
                                     CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, window, nullptr,
                                     GetModuleHandleW(nullptr), nullptr);
    if (!tip)
        return nullptr;
    TOOLINFOW tool{};
    tool.cbSize = TTTOOLINFOW_V2_SIZE;
    tool.uFlags = TTF_SUBCLASS | TTF_IDISHWND;
    tool.hwnd = window;
    tool.uId = reinterpret_cast<UINT_PTR>(list);
    tool.lpszText = LPSTR_TEXTCALLBACKW;
    if (!SendMessageW(tip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool))) {
        DestroyWindow(tip);
        return nullptr;
    }
    SendMessageW(tip, TTM_SETMAXTIPWIDTH, 0, 760);
    return tip;
}

void show_result_tip(NMTTDISPINFOW &info) {
    const auto index = single_list.hovered;
    if (index < 0 || static_cast<std::size_t>(index) >= result_rows.size())
        return;
    const HWND list = reinterpret_cast<HWND>(info.hdr.idFrom);
    RECT bounds{};
    if (!IsWindowVisible(list) || !GetClientRect(list, &bounds))
        return;
    if (const HDC dc = GetDC(list)) {
        const auto saved = SaveDC(dc);
        set_row_font(list, dc);
        auto &label = result_rows[index].filename;
        SIZE size{};
        GetTextExtentPoint32W(dc, label.c_str(), static_cast<int>(label.size()), &size);
        RestoreDC(dc, saved);
        ReleaseDC(list, dc);
        const auto area = single_label(bounds);
        if (size.cx > area.right - area.left)
            info.lpszText = label.data();
    }
}

void paint_search_field(HDC dc, const Colors &colors) {
    if (IsRectEmpty(&search_field))
        return;
    const int saved = SaveDC(dc);
    fill_rounded(dc, search_field, 6, colors.dropdown_button);
    SetDCPenColor(dc, colors.text);
    SelectObject(dc, GetStockObject(DC_PEN));
    SelectObject(dc, GetStockObject(NULL_BRUSH));
    const LONG x = search_field.left, y = search_field.top;
    Ellipse(dc, x + 12, y + 8, x + 25, y + 21);
    MoveToEx(dc, x + 22, y + 18, nullptr);
    LineTo(dc, x + 28, y + 24);
    RestoreDC(dc, saved);
}

void paint_dialog(HWND window, const Colors &colors) {
    PAINTSTRUCT paint{};
    const HDC dc = BeginPaint(window, &paint);
    RECT bounds{};
    GetClientRect(window, &bounds);
    const LONG header = std::min(header_bottom(), bounds.bottom);
    fill(dc, {0, header, bounds.right, bounds.bottom}, colors.body);
    fill(dc, {0, 0, bounds.right, header}, colors.header);
    paint_search_field(dc, colors);
    const HWND key = GetDlgItem(window, IDC_OPENSUBTITLES_KEY);
    RECT key_bounds{};
    if (IsWindowVisible(key) && GetWindowRect(key, &key_bounds)) {
        MapWindowPoints(nullptr, window, reinterpret_cast<POINT *>(&key_bounds), 2);
        InflateRect(&key_bounds, 8, 5);
        const int saved = SaveDC(dc);
        fill_rounded(dc, key_bounds, 6, colors.dropdown_button);
        RestoreDC(dc, saved);
    }
    const auto font = reinterpret_cast<HFONT>(SendDlgItemMessageW(window, IDCANCEL, WM_GETFONT, 0, 0));
    const HGDIOBJ previous = font ? SelectObject(dc, font) : nullptr;
    SetTextColor(dc, colors.text);
    SetBkMode(dc, TRANSPARENT);
    RECT label{margin, 9, bounds.right - 44, 41};
    DrawTextW(dc, title, -1, &label, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (previous)
        SelectObject(dc, previous);
    EndPaint(window, &paint);
}

void paint_button(const DRAWITEMSTRUCT &item, const Colors &colors, bool dropdown) {
    const HDC dc = item.hDC;
    RECT bounds = item.rcItem;
    const bool close = item.CtlID == IDC_CLOSE_ADD_SUBTITLES;
    const bool save = item.CtlID == IDOK;
    const bool pressed = (item.itemState & (ODS_SELECTED | ODS_HOTLIGHT)) != 0;
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const int saved = SaveDC(dc);
    fill(dc, bounds, close || dropdown ? colors.header : colors.body);
    if (!close || pressed) {
        const COLORREF button = dropdown ? colors.dropdown_button : colors.button;
        fill_rounded(dc, bounds, 6, save ? (disabled ? colors.save : colors.save_text)
                                         : pressed ? colors.highlight : button);
    }
    if (close) {
        SetDCPenColor(dc, colors.text);
        SelectObject(dc, GetStockObject(DC_PEN));
        const int x = (bounds.left + bounds.right) / 2, y = (bounds.top + bounds.bottom) / 2;
        MoveToEx(dc, x - 5, y - 5, nullptr);
        LineTo(dc, x + 6, y + 6);
        MoveToEx(dc, x + 5, y - 5, nullptr);
        LineTo(dc, x - 6, y + 6);
    } else {
        const auto font = reinterpret_cast<HFONT>(SendMessageW(item.hwndItem, WM_GETFONT, 0, 0));
        if (font)
            SelectObject(dc, font);
        SetTextColor(dc, save ? (disabled ? colors.save_text : colors.header) : colors.text);
        SetBkMode(dc, TRANSPARENT);
        wchar_t label[32]{};
        GetWindowTextW(item.hwndItem, label, 32);
        if (dropdown) {
            const LONG x = bounds.right - 20, y = (bounds.top + bounds.bottom) / 2;
            draw_stroke(dc, colors.text, {{x - 5, y - 2}, {x, y + 3}, {x + 5, y - 2}});
            bounds.left += 13;
            bounds.right -= 33;
        }
        DrawTextW(dc, label, -1, &bounds,
                  (dropdown ? DT_LEFT : DT_CENTER) | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
    RestoreDC(dc, saved);
}

INT_PTR CALLBACK dialog_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_INITDIALOG) {
        SetWindowLongPtrW(window, DWLP_USER, lparam);
        SetWindowTextW(window, title);
        automatic_query.clear();
        search_field = {};
        init_dropdowns(window);
        const HWND edit = GetDlgItem(window, IDC_SUBTITLE_QUERY);
        const HWND key = GetDlgItem(window, IDC_OPENSUBTITLES_KEY);
        ShowWindow(edit, SW_HIDE);
        ShowWindow(GetDlgItem(window, IDC_SUBTITLE_LANGUAGE), SW_HIDE);
        SendMessageW(edit, EM_SETLIMITTEXT, 512, 0);
        SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
        SendMessageW(edit, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"Search"));
        SendMessageW(key, EM_SETLIMITTEXT, 512, 0);
        SendMessageW(key, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"OpenSubtitles API key"));
        const HWND results = GetDlgItem(window, IDC_SUBTITLE_RESULTS);
        SetWindowSubclass(results, results_list_proc, 1, 0);
        SetWindowSubclass(GetDlgItem(window, IDC_SUBTITLE_FOLDER_RESULTS), results_list_proc, 1, 0);
        result_tooltip = create_result_tooltip(window, results);
        update_results(window);
        return TRUE;
    }
    const auto &colors = GetWindowLongPtrW(window, DWLP_USER) ? light : dark;
    switch (message) {
    case start_local_search_message:
        if (computer_selected() && !local_job && folder.rows.empty())
            start_search(window);
        return TRUE;
    case WM_SIZE:
        if (wparam != SIZE_MINIMIZED)
            resize_dialog(window, LOWORD(lparam), HIWORD(lparam));
        return TRUE;
    case WM_PAINT:
        paint_dialog(window, colors);
        return TRUE;
    case WM_ERASEBKGND:
        return TRUE;  // WM_PAINT fills the entire dialog.
    case WM_TIMER:
        if (wparam == search_timer) {
            finish_search(window);
            return TRUE;
        }
        if (wparam == download_timer) {
            finish_download(window);
            return TRUE;
        }
        break;
    case WM_NOTIFY: {
        auto &notice = *reinterpret_cast<NMHDR *>(lparam);
        if (notice.hwndFrom == result_tooltip && notice.code == TTN_GETDISPINFOW) {
            show_result_tip(*reinterpret_cast<NMTTDISPINFOW *>(lparam));
            return TRUE;
        }
        break;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        const int control = GetDlgCtrlID(reinterpret_cast<HWND>(lparam));
        if (control == IDC_SUBTITLE_QUERY || control == IDC_OPENSUBTITLES_KEY ||
            control == IDC_SUBTITLE_STATUS || control == IDC_SUBTITLE_RESULTS || control == IDC_SUBTITLE_FOLDER_RESULTS) {
            const auto dc = reinterpret_cast<HDC>(wparam);
            const auto background = message == WM_CTLCOLOREDIT ? colors.dropdown_button : colors.body;
            SetTextColor(dc, colors.text);
            SetBkColor(dc, background);
            SetDCBrushColor(dc, background);
            return reinterpret_cast<INT_PTR>(GetStockObject(DC_BRUSH));
        }
        break;
    }
    case WM_DRAWITEM: {
        const auto &item = *reinterpret_cast<const DRAWITEMSTRUCT *>(lparam);
        const auto *dropdown = find_dropdown(item.CtlID);
        if (item.CtlID == IDC_SUBTITLE_RESULTS)
            paint_single_row(item, colors);
        else if (item.CtlID == IDC_SUBTITLE_FOLDER_RESULTS)
            paint_folder_row(item, colors);
        else if (dropdown && static_cast<int>(item.CtlID) == dropdown->options_id)
            paint_option(item, colors, *dropdown);
        else
            paint_button(item, colors, dropdown != nullptr);
        return TRUE;
    }
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN: {
        if (const auto *dropdown = visible_dropdown(window))
            close_dropdown(window, *dropdown, true);
        const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (message == WM_LBUTTONDOWN && PtInRect(&search_field, point)) {
            SetFocus(GetDlgItem(window, IDC_SUBTITLE_QUERY));
            if (point.x < search_field.left + search_text_left)
                start_search(window);
            return TRUE;
        }
        break;
    }
    case WM_COMMAND:
        if (HIWORD(wparam) == LBN_SELCHANGE && LOWORD(wparam) == IDC_SUBTITLE_RESULTS) {
            update_save_button(window);
            return TRUE;
        }
        if (HIWORD(wparam) == EN_CHANGE && LOWORD(wparam) == IDC_SUBTITLE_QUERY &&
            open_subtitles_selected() && !prefilling_query) {
            cancel_search(window);
            clear_results(window);
            if (!search.needs_key)
                show_search_status(window, L"Press Enter or click the magnifying glass to search.");
            return TRUE;
        }
        if (HIWORD(wparam) == BN_CLICKED) {
            const int control_id = LOWORD(wparam);
            if (const auto *dropdown = find_dropdown(control_id); dropdown && control_id == dropdown->button_id) {
                open_dropdown(window, *dropdown);
                return TRUE;
            }
            switch (control_id) {
            case IDC_BROWSE_SUBTITLE:
                try {
                    browse_subtitle(window);
                } catch (...) {
                    show_search_status(window, L"Could not select the subtitle file.");
                }
                return TRUE;
            case IDOK:
                if (open_subtitles_selected())
                    start_download(window);
                else
                    save_local_selection(window);
                return TRUE;
            case IDC_SAVE_OPENSUBTITLES_KEY:
                if (search.needs_key)
                    remember_key(window);
                return TRUE;
            case IDC_MORE_SUBTITLES:
                if (!search.job && search.page < search.pages)
                    start_search(window, search.page + 1);
                return TRUE;
            case IDCANCEL:
            case IDC_CLOSE_ADD_SUBTITLES:
                DestroyWindow(window);
                return TRUE;
            }
        }
        break;
    case WM_CLOSE:
        DestroyWindow(window);
        return TRUE;
    case WM_NCDESTROY:
        cancel_search(window);
        KillTimer(window, download_timer);
        if (download_job)
            download_job->cancelled.store(true);
        download_job.reset();
        search = {};
        folder = {};
        single_list = {};
        folder_list = {};
        result_rows.clear();
        result_tooltip = nullptr;
        media = {};
        current_media = nullptr;
        current_playlist = nullptr;
        attach_subtitles = nullptr;
        RemoveWindowSubclass(GetWindow(window, GW_OWNER), follow_player, position_subclass);
        if (dialog == window)
            dialog = nullptr;
        break;
    }
    return FALSE;
}

}

bool show(HWND player, bool light_theme, CurrentMedia query, CurrentPlaylist playlist, AttachSubtitles attach) {
    if (dialog) {
        SetActiveWindow(dialog);
        return true;
    }
    HMODULE module = nullptr;
    if (!player || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                      reinterpret_cast<LPCWSTR>(&dialog_proc), &module))
        return false;
    current_media = query;
    current_playlist = playlist;
    attach_subtitles = attach;
    dialog = CreateDialogParamW(module, MAKEINTRESOURCEW(IDD_ADD_SUBTITLES), player, dialog_proc, light_theme);
    if (!dialog)
        return false;
    if (!center_on_player(dialog, player)) {
        DestroyWindow(dialog);
        return false;
    }
    SetWindowSubclass(player, follow_player, position_subclass, reinterpret_cast<DWORD_PTR>(dialog));
    ShowWindow(dialog, SW_SHOWNORMAL);
    PostMessageW(dialog, start_local_search_message, 0, 0);
    return true;
}

bool handle_key(MSG &message) {
    if (!dialog || message.message < WM_KEYFIRST || message.message > WM_KEYLAST ||
        (message.hwnd != dialog && !IsChild(dialog, message.hwnd)))
        return false;
    if (message.message == WM_KEYDOWN) {
        if (auto *dropdown = visible_dropdown(dialog)) {
            if (message.wParam == VK_ESCAPE) {
                close_dropdown(dialog, *dropdown, true);
                return true;
            }
            if (message.wParam == VK_RETURN || message.wParam == VK_SPACE) {
                const HWND options = GetDlgItem(dialog, dropdown->options_id);
                choose_option(dialog, *dropdown, SendMessageW(options, LB_GETCURSEL, 0, 0));
                return true;
            }
        } else if (message.wParam == VK_DOWN) {
            const int control_id = GetDlgCtrlID(message.hwnd);
            if (const auto *dropdown = find_dropdown(control_id); dropdown && control_id == dropdown->button_id) {
                open_dropdown(dialog, *dropdown);
                return true;
            }
        }
        const int control = GetDlgCtrlID(message.hwnd);
        if (control == IDC_SUBTITLE_FOLDER_RESULTS && (message.wParam == VK_RETURN || message.wParam == VK_SPACE)) {
            if (!(message.lParam & (1L << 30)))
                cycle_subtitle(message.hwnd, SendMessageW(message.hwnd, LB_GETCURSEL, 0, 0));
            return true;
        }
        if (message.wParam == VK_RETURN) {
            if (control == IDC_SUBTITLE_QUERY) {
                if (!(message.lParam & (1L << 30)))
                    start_search(dialog);
                return true;
            }
            if (control == IDC_OPENSUBTITLES_KEY && search.needs_key) {
                remember_key(dialog);
                return true;
            }
        }
    }
    return IsDialogMessageW(dialog, &message);
}

}
