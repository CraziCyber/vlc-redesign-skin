#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "opensubtitles.hpp"

#include <windows.h>
#include <shlobj.h>
#include <wincred.h>
#include <winhttp.h>
#include <roapi.h>
#include <winstring.h>
#include <windows.data.json.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <regex>

namespace opensubtitles {
namespace {

constexpr wchar_t credential_name[] = L"VLC Redesign/OpenSubtitles API Key";
constexpr wchar_t host[] = L"api.opensubtitles.com";
constexpr std::wstring_view origin = L"https://api.opensubtitles.com";
constexpr std::wstring_view endpoint = L"/api/v1/subtitles?";
constexpr wchar_t download_endpoint[] = L"/api/v1/download";
constexpr std::size_t max_response_size = 4 * 1024 * 1024;
constexpr std::size_t max_key_length = 512;

std::wstring load_key() {
    PCREDENTIALW raw = nullptr;
    if (!CredReadW(credential_name, CRED_TYPE_GENERIC, 0, &raw))
        return {};
    const std::unique_ptr<CREDENTIALW, decltype(&CredFree)> credential(raw, CredFree);
    if (!credential->CredentialBlob || credential->CredentialBlobSize % sizeof(wchar_t) != 0)
        return {};
    const auto *text = reinterpret_cast<const wchar_t *>(credential->CredentialBlob);
    const std::wstring_view key(text, credential->CredentialBlobSize / sizeof(wchar_t));
    return valid_key(key) ? std::wstring(key) : std::wstring{};
}

std::wstring encode_query(std::wstring_view text) {
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (!size)
        return {};
    std::string utf8(size, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                        utf8.data(), size, nullptr, nullptr);
    constexpr wchar_t hex[] = L"0123456789ABCDEF";
    std::wstring encoded;
    for (const unsigned char byte : utf8) {
        if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') ||
            byte == '-' || byte == '_' || byte == '.' || byte == '~') {
            encoded += byte;
        } else if (byte == ' ') {
            encoded += L'+';
        } else {
            encoded += L'%';
            encoded += hex[byte >> 4];
            encoded += hex[byte & 0x0F];
        }
    }
    return encoded;
}

std::wstring request_path(const Query &query, std::wstring_view language, int page) {
    auto title = query.title;
    for (auto &character : title)
        character = static_cast<wchar_t>(std::towlower(character));
    const auto encoded = encode_query(title);
    if (encoded.empty())
        return {};

    std::wstring path(endpoint);
    if (query.episode >= 0)
        path += L"episode_number=" + std::to_wstring(query.episode) + L"&";
    path += L"languages=";
    path += language;
    if (page > 1)
        path += L"&page=" + std::to_wstring(page);
    path += L"&query=" + encoded;
    if (query.season >= 0)
        path += L"&season_number=" + std::to_wstring(query.season) + L"&type=episode";
    return path;
}

namespace json = ABI::Windows::Data::Json;
using Microsoft::WRL::ComPtr;

class String {
public:
    explicit String(std::wstring_view text = {}) {
        if (!text.empty())
            WindowsCreateString(text.data(), static_cast<UINT32>(text.size()), &value_);
    }
    String(const String &) = delete;
    String &operator=(const String &) = delete;
    ~String() { WindowsDeleteString(value_); }
    HSTRING get() const { return value_; }
    HSTRING *out() { return &value_; }
    std::wstring text() const {
        UINT32 length = 0;
        const wchar_t *buffer = WindowsGetStringRawBuffer(value_, &length);
        return std::wstring(buffer, length);
    }
private:
    HSTRING value_ = nullptr;
};

struct Runtime {
    const HRESULT status = RoInitialize(RO_INIT_MULTITHREADED);
    ~Runtime() { if (SUCCEEDED(status)) RoUninitialize(); }
};

UINT32 array_size(json::IJsonArray *array) {
    ComPtr<ABI::Windows::Foundation::Collections::IVector<json::IJsonValue *>> items;
    UINT32 size = 0;
    if (SUCCEEDED(array->QueryInterface(IID_PPV_ARGS(items.GetAddressOf()))))
        items->get_Size(&size);
    return size;
}

std::wstring named_text(json::IJsonObject *object, const wchar_t *name) {
    const String property(name);
    String value;
    return SUCCEEDED(object->GetNamedString(property.get(), value.out())) ? value.text() : std::wstring{};
}

int named_int(json::IJsonObject *object, const wchar_t *name) {
    double number = -1;
    return SUCCEEDED(object->GetNamedNumber(String(name).get(), &number)) && number >= 0 && number <= INT_MAX &&
        std::floor(number) == number
        ? static_cast<int>(number) : -1;
}

ComPtr<json::IJsonObject> json_object(std::string_view response) {
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, response.data(),
                                         static_cast<int>(response.size()), nullptr, 0);
    if (!size)
        return {};
    std::wstring utf16(size, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, response.data(), static_cast<int>(response.size()), utf16.data(), size);
    const String class_name(RuntimeClass_Windows_Data_Json_JsonValue), input(utf16);
    ComPtr<json::IJsonValueStatics> factory;
    ComPtr<json::IJsonValue> value;
    ComPtr<json::IJsonObject> root;
    if (FAILED(RoGetActivationFactory(class_name.get(), IID_PPV_ARGS(factory.GetAddressOf()))) ||
        FAILED(factory->Parse(input.get(), value.GetAddressOf())) ||
        FAILED(value->GetObject(root.GetAddressOf())))
        return {};
    return root;
}

Result parse_response(const std::string &response) {
    Result result{Status::response_error, 0, {}};
    const Runtime runtime;
    if (FAILED(runtime.status))
        return result;
    const auto root = json_object(response);
    ComPtr<json::IJsonArray> entries;
    if (!root || FAILED(root->GetNamedArray(String(L"data").get(), entries.GetAddressOf())))
        return result;
    result.pages = std::max(0, named_int(root.Get(), L"total_pages"));

    const String attributes_name(L"attributes"), files_name(L"files"), feature_name(L"feature_details");
    const auto entry_count = array_size(entries.Get());
    for (UINT32 i = 0; i < entry_count; ++i) {
        ComPtr<json::IJsonObject> entry, attributes;
        ComPtr<json::IJsonArray> files;
        if (FAILED(entries->GetObjectAt(i, entry.GetAddressOf())) ||
            FAILED(entry->GetNamedObject(attributes_name.get(), attributes.GetAddressOf())) ||
            FAILED(attributes->GetNamedArray(files_name.get(), files.GetAddressOf())))
            continue;
        const auto release = named_text(attributes.Get(), L"release");
        ComPtr<json::IJsonObject> feature;
        const bool has_feature = SUCCEEDED(attributes->GetNamedObject(feature_name.get(), feature.GetAddressOf()));
        const int season = has_feature ? named_int(feature.Get(), L"season_number") : -1;
        const int episode = has_feature ? named_int(feature.Get(), L"episode_number") : -1;
        const auto file_count = array_size(files.Get());
        for (UINT32 j = 0; j < file_count; ++j) {
            ComPtr<json::IJsonObject> file;
            if (SUCCEEDED(files->GetObjectAt(j, file.GetAddressOf()))) {
                auto name = named_text(file.Get(), L"file_name");
                if (!name.empty())
                    result.subtitles.push_back({{std::move(name), named_int(file.Get(), L"file_id")}, release, season, episode});
            }
        }
    }
    result.status = Status::ok;
    return result;
}

std::wstring redirect_path(HINTERNET request) {
    wchar_t location[4096]{};
    DWORD size = sizeof(location);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                             location, &size, WINHTTP_NO_HEADER_INDEX))
        return {};
    std::wstring_view path(location);
    if (path.substr(0, origin.size()) == origin)
        path.remove_prefix(origin.size());
    if (path.substr(0, endpoint.size()) != endpoint || path.find_first_of(L"\r\n#") != std::wstring_view::npos)
        return {};
    return std::wstring(path);
}

using HttpHandle = std::unique_ptr<void, decltype(&WinHttpCloseHandle)>;

HttpHandle open_session() {
    HttpHandle session(WinHttpOpen(L"VLC Redesign v1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                   WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0), WinHttpCloseHandle);
    if (session && !WinHttpSetTimeouts(session.get(), 4000, 5000, 5000, 7000))
        session.reset();
    return session;
}

HttpHandle send_request(HINTERNET connection, const wchar_t *verb, const std::wstring &path,
                        std::wstring_view headers, std::string_view body = {},
                        DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER) {
    HttpHandle request(connection ? WinHttpOpenRequest(connection, verb, path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE | WINHTTP_FLAG_ESCAPE_DISABLE)
        : nullptr, WinHttpCloseHandle);
    if (!request || !WinHttpSetOption(request.get(), WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect)) ||
        !WinHttpSendRequest(request.get(), headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.data(),
                            static_cast<DWORD>(headers.size()),
                            body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char *>(body.data()),
                            static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0) ||
        !WinHttpReceiveResponse(request.get(), nullptr))
        request.reset();
    return request;
}

DWORD status_code(HINTERNET request) {
    DWORD status = 0, size = sizeof(status);
    if (!request || !WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX))
        return 0;
    return status;
}

Status api_status(DWORD status) {
    if (status == 0)
        return Status::connection_error;
    if (status == 401 || status == 403)
        return Status::rejected_key;
    if (status == 429)
        return Status::rate_limited;
    return status == 200 ? Status::ok : Status::response_error;
}

Status read_body(HINTERNET request, std::string &body, const std::atomic_bool &cancelled) {
    char buffer[8192];
    while (!cancelled.load()) {
        DWORD read = 0;
        if (!WinHttpReadData(request, buffer, sizeof(buffer), &read))
            break;
        if (read == 0)
            return Status::ok;
        if (body.size() + read > max_response_size)
            return Status::response_error;
        body.append(buffer, read);
    }
    return Status::connection_error;
}

struct DownloadLink {
    std::wstring url;
    int remaining = -1;
};

DownloadLink parse_download_link(const std::string &body) {
    const Runtime runtime;
    if (FAILED(runtime.status))
        return {};
    if (const auto root = json_object(body))
        return {named_text(root.Get(), L"link"), named_int(root.Get(), L"remaining")};
    return {};
}

Status get_subtitle_body(HINTERNET session, const std::wstring &url, std::string &body,
                         const std::atomic_bool &cancelled) {
    if (url.empty() || url.size() > 8192 || std::any_of(url.begin(), url.end(), [](wchar_t character) {
        return character <= L' ' || character == L'\\' || character == L'#';
    }))
        return Status::response_error;
    URL_COMPONENTS url_parts{};
    url_parts.dwStructSize = sizeof(url_parts);
    url_parts.dwHostNameLength = url_parts.dwUrlPathLength = url_parts.dwExtraInfoLength =
        url_parts.dwUserNameLength = url_parts.dwPasswordLength = DWORD(-1);
    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &url_parts) ||
        url_parts.nScheme != INTERNET_SCHEME_HTTPS || url_parts.dwHostNameLength == 0 ||
        url_parts.dwUserNameLength != 0 || url_parts.dwPasswordLength != 0)
        return Status::response_error;

    const std::wstring hostname(url_parts.lpszHostName, url_parts.dwHostNameLength);
    auto path = url_parts.dwUrlPathLength ? std::wstring(url_parts.lpszUrlPath, url_parts.dwUrlPathLength) : L"/";
    if (url_parts.dwExtraInfoLength)
        path.append(url_parts.lpszExtraInfo, url_parts.dwExtraInfoLength);
    const HttpHandle connection(WinHttpConnect(session, hostname.c_str(), url_parts.nPort, 0), WinHttpCloseHandle);
    // The signed URL grants file access. Only the API request carries the API key.
    const auto request = send_request(connection.get(), L"GET", path, {}, {},
                                      WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP);
    if (const DWORD status = status_code(request.get()); status != 200)
        return status == 0 ? Status::connection_error : Status::download_unavailable;
    return read_body(request.get(), body, cancelled);
}

namespace fs = std::filesystem;

fs::path cache_folder() {
    PWSTR path = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path)) || !path)
        return {};
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> folder(path, CoTaskMemFree);
    return fs::path(folder.get()) / L"VLC Redesign" / L"OpenSubtitles";
}

bool is_subrip(const std::string &body) {
    if (body.empty() || body.find('\0') != std::string::npos ||
        !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, body.data(), static_cast<int>(body.size()), nullptr, 0))
        return false;
    static const std::regex timing(
        R"((^|\n)[ \t]{0,32}[0-9]{1,6}:[0-9]{2}:[0-9]{2}[,.][0-9]{1,3}[ \t]{1,32}-->[ \t]{1,32}[0-9]{1,6}:[0-9]{2}:[0-9]{2}[,.][0-9]{1,3})");
    return std::regex_search(body, timing);
}

bool cached_subtitle(const fs::path &file) {
    std::error_code error;
    const auto size = fs::file_size(file, error);
    if (error || size == 0 || size > max_response_size)
        return false;
    std::ifstream input(file, std::ios::binary);
    std::string body(static_cast<std::size_t>(size), '\0');
    return input.read(body.data(), static_cast<std::streamsize>(body.size())) && is_subrip(body);
}

bool cache_subtitle(const fs::path &file, const std::string &body) {
    struct TemporaryFile {
        wchar_t name[MAX_PATH]{};
        ~TemporaryFile() { if (name[0]) DeleteFileW(name); }
    } temp;
    if (!GetTempFileNameW(file.parent_path().c_str(), L"vlc", 0, temp.name))
        return false;
    std::ofstream stream(fs::path(temp.name), std::ios::binary | std::ios::trunc);
    stream.write(body.data(), static_cast<std::streamsize>(body.size()));
    stream.close();
    if (!stream || !MoveFileExW(temp.name, file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return false;
    temp.name[0] = L'\0';
    return true;
}

}

Query parse_query(std::wstring_view text) {
    Query query{std::wstring(text)};
    static const std::wregex episode(
        LR"([\s._-]+(?:s([0-9]{1,2})(?:[\s._-]*e([0-9]{1,3}))?|([0-9]{1,2})x([0-9]{1,3}))(?![a-z0-9]))",
        std::regex::icase);
    static const std::wregex release(LR"([\s._-]+(?:2160p|1080p|720p|480p|bluray|bdrip|webrip|web[ ._-]?dl|hdtv)\b.*$)", std::regex::icase);
    std::wsmatch match;
    if (std::regex_search(query.title, match, episode) && match.position() > 0) {
        query.season = std::stoi(match[1].matched ? match[1] : match[3]);
        const auto &number = match[1].matched ? match[2] : match[4];
        if (number.matched)
            query.episode = std::stoi(number);
        query.title.erase(match.position());
    } else if (std::regex_search(query.title, match, release) && match.position() > 0) {
        query.title.erase(match.position());
    }
    std::replace(query.title.begin(), query.title.end(), L'.', L' ');
    std::replace(query.title.begin(), query.title.end(), L'_', L' ');
    const auto first = query.title.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos)
        query.title.clear();
    else
        query.title = query.title.substr(first, query.title.find_last_not_of(L" \t\r\n") - first + 1);
    return query;
}

bool valid_key(std::wstring_view key) {
    return !key.empty() && key.size() <= max_key_length && std::all_of(key.begin(), key.end(), [](wchar_t character) {
        return character >= L'!' && character <= L'~';
    });
}

bool save_key(std::wstring_view key) {
    if (!valid_key(key))
        return false;
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<wchar_t *>(credential_name);
    credential.CredentialBlobSize = static_cast<DWORD>(key.size() * sizeof(wchar_t));
    credential.CredentialBlob = reinterpret_cast<BYTE *>(const_cast<wchar_t *>(key.data()));
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    return CredWriteW(&credential, 0) != FALSE;
}

Result search(std::wstring_view query, std::wstring_view language, bool single_file, int page,
              const std::atomic_bool &cancelled) {
    auto parsed = parse_query(query);
    if (!single_file)
        parsed.episode = -1;
    return search(parsed, language, page, cancelled);
}

Result search(const Query &query, std::wstring_view language, int page, const std::atomic_bool &cancelled) {
    const auto key = load_key();
    if (key.empty())
        return {Status::missing_key, 0, {}};
    auto path = request_path(query, language, page);
    if (path.empty())
        return {Status::ok, 0, {}};
    const auto session = open_session();
    if (!session)
        return {};
    const HttpHandle connection(WinHttpConnect(session.get(), host, INTERNET_DEFAULT_HTTPS_PORT, 0), WinHttpCloseHandle);
    if (!connection)
        return {};
    const auto headers = L"Accept: application/json\r\nApi-Key: " + key + L"\r\n";
    for (int attempt = 0; attempt < 4 && !cancelled.load(); ++attempt) {
        const auto request = send_request(connection.get(), L"GET", path, headers);
        const auto status = status_code(request.get());
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            path = redirect_path(request.get());
            if (path.empty())
                return {Status::response_error, 0, {}};
            continue;
        }
        if (const auto state = api_status(status); state != Status::ok)
            return {state, 0, {}};

        std::string response;
        const auto read = read_body(request.get(), response, cancelled);
        return read == Status::ok ? parse_response(response) : Result{read, 0, {}};
    }
    return {};
}

Download download(int file_id, const std::atomic_bool &cancelled, bool request_allowed) {
    if (file_id <= 0)
        return {Status::download_unavailable};
    if (cancelled.load())
        return {};

    const auto folder = cache_folder();
    if (folder.empty())
        return {Status::cache_error};
    const auto file = folder / (std::to_wstring(file_id) + L".srt");
    if (cached_subtitle(file))
        return {Status::ok, file.native()};
    if (!request_allowed)
        return {Status::download_limit};

    const auto key = load_key();
    if (key.empty())
        return {Status::missing_key};
    std::error_code error;
    fs::create_directories(folder, error);
    if (error)
        return {Status::cache_error};

    const auto session = open_session();
    if (!session || cancelled.load())
        return {};
    const HttpHandle connection(WinHttpConnect(session.get(), host, INTERNET_DEFAULT_HTTPS_PORT, 0), WinHttpCloseHandle);
    const auto headers = L"Accept: application/json\r\nContent-Type: application/json\r\nApi-Key: " + key + L"\r\n";
    const auto payload = "{\"file_id\":" + std::to_string(file_id) + ",\"sub_format\":\"srt\"}";
    const auto request = send_request(connection.get(), L"POST", download_endpoint, headers, payload);
    const auto http_status = status_code(request.get());
    const auto status = api_status(http_status);
    if (status != Status::ok && http_status != 406)
        return {http_status == 404 || http_status == 410 ? Status::download_unavailable : status};
    std::string response;
    if (const auto read = read_body(request.get(), response, cancelled); read != Status::ok)
        return {read};
    const auto link = parse_download_link(response);
    if (http_status == 406)
        return {link.remaining == 0 ? Status::download_limit : Status::download_unavailable};
    if (cancelled.load())
        return {};

    std::string body;
    auto fetched = get_subtitle_body(session.get(), link.url, body, cancelled);
    if (fetched == Status::connection_error && !cancelled.load()) {
        // Retrying the file transfer with the same link uses no additional download request.
        body.clear();
        fetched = get_subtitle_body(session.get(), link.url, body, cancelled);
    }
    if (fetched != Status::ok)
        return {fetched};
    if (cancelled.load())
        return {};
    if (!is_subrip(body))
        return {Status::download_unavailable};
    if (!cache_subtitle(file, body) && !cached_subtitle(file))
        return {Status::cache_error};
    return {Status::ok, file.native(), link.remaining};
}

}
