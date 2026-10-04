#pragma once
#include <windows.h>
#include <shlobj.h>
#include <winhttp.h>
#include <filesystem>
#include <fstream>
#include <regex>
#include <thread>
#include <future>
#include <mutex>
#include <memory>
#include <condition_variable>
#include <atomic>
#include <set>
#include <deque>
#include <map>
#include <algorithm>
#include <chrono>
#include <string>
#include <stdexcept>
#include <optional>
#include <cstdint>
#include "../third_party/json.hpp"

namespace services {
using Json = nlohmann::json;
namespace fs = std::filesystem;
constexpr UINT Updated = WM_APP + 20;

inline std::string Utf8(const std::wstring &s) {
    if (s.empty())
        return {};
    const int n =
        WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}
inline std::wstring Wide(const std::string &s) {
    if (s.empty())
        return {};
    const int n =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(n, 0);
    if (n)
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}
inline std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return s;
}
inline std::string Read(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}
inline void Save(const fs::path &path, const std::string &bytes) {
    fs::create_directories(path.parent_path());
    // Session windows are separate processes and can generate the same report concurrently.
    static std::atomic<uint64_t> sequence{0};
    const fs::path pending = path.wstring() + L"." + std::to_wstring(GetCurrentProcessId()) + L"." +
                             std::to_wstring(++sequence) + L".tmp";
    {
        std::ofstream out(pending, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!out)
            throw std::runtime_error("Could not save application data.");
    }
    if (!MoveFileExW(pending.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::error_code ignored;
        fs::remove(pending, ignored);
        throw std::runtime_error("Could not commit application data.");
    }
}
inline fs::path ExecutableDirectory() {
    wchar_t exe[32768]{};
    GetModuleFileNameW(nullptr, exe, 32768);
    return fs::path(exe).parent_path();
}
inline fs::path DataDirectory() {
    PWSTR folder = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &folder)))
        throw std::runtime_error("Could not locate Local AppData.");
    fs::path data = fs::path(folder) / L"QuestCompleter";
    CoTaskMemFree(folder);
    return data;
}
inline void InstallAssets(const fs::path &data) {
    for (const auto &[id, name] :
         std::vector<std::pair<int, const wchar_t *>>{{101, L"fonts/Inter.ttf"},
                                                      {102, L"fonts/fa-solid-900.ttf"},
                                                      {104, L"fonts/fa-brands-400.ttf"},
                                                      {105, L"assets/quest.ico"},
                                                      {201, L"licenses/Inter-OFL.txt"},
                                                      {202, L"licenses/FontAwesome-LICENSE.txt"},
                                                      {203, L"licenses/JSON-MIT.txt"}}) {
        const HRSRC resource = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
        if (!resource)
            throw std::runtime_error("Bundled application asset is missing.");
        const auto size = SizeofResource(nullptr, resource);
        const auto path = data / name;
        const auto *bytes = static_cast<const char *>(LockResource(LoadResource(nullptr, resource)));
        if (fs::exists(path) && fs::file_size(path) == size && Read(path) == std::string(bytes, size))
            continue;
        Save(path, std::string(bytes, size));
    }
}
inline void MigrateLegacyData(const fs::path &data) {
    const auto exe = ExecutableDirectory();
    for (const auto &source : {exe.parent_path() / L"QuestCompleter-v3/data", exe / L"data"}) {
        if (!fs::exists(source) || source == data)
            continue;
        for (const auto &item : fs::recursive_directory_iterator(source)) {
            if (!item.is_regular_file())
                continue;
            const auto relative = fs::relative(item.path(), source);
            const auto path = data / relative;
            if (!fs::exists(path))
                Save(path, Read(item.path()));
        }
    }
}
inline double UnixNow() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER n;
    n.LowPart = ft.dwLowDateTime;
    n.HighPart = ft.dwHighDateTime;
    return static_cast<double>(n.QuadPart) / 10000000.0 - 11644473600.0;
}
struct Internet {
    HINTERNET h = nullptr;
    explicit Internet(HINTERNET value = nullptr) : h(value) {}
    ~Internet() {
        if (h)
            WinHttpCloseHandle(h);
    }
    Internet(const Internet &) = delete;
    Internet &operator=(const Internet &) = delete;
};
struct Cancellation {
    std::mutex mutex;
    std::set<HINTERNET> active, closed;
    bool stopping = false;
    void Stop() {
        std::lock_guard<std::mutex> lock(mutex);
        stopping = true;
        for (HINTERNET request : active) {
            if (closed.insert(request).second)
                WinHttpCloseHandle(request);
        }
    }
};
struct RequestGuard {
    Cancellation *cancel;
    Internet &request;
    RequestGuard(Cancellation *value, Internet &handle) : cancel(value), request(handle) {
        if (!cancel)
            return;
        std::lock_guard<std::mutex> lock(cancel->mutex);
        if (cancel->stopping)
            throw std::runtime_error("Request cancelled.");
        cancel->active.insert(request.h);
    }
    ~RequestGuard() {
        if (!cancel)
            return;
        std::lock_guard<std::mutex> lock(cancel->mutex);
        const HINTERNET handle = request.h;
        if (cancel->closed.erase(handle))
            request.h = nullptr;
        cancel->active.erase(handle);
    }
};
inline std::string Get(const std::wstring &url, Cancellation *cancel = nullptr) {
    URL_COMPONENTS parts{sizeof(parts)};
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS)
        throw std::runtime_error("Invalid HTTPS URL.");
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    // Only contact the public catalogs and their artwork hosts.
    const bool steam = host == L"store.steampowered.com" || host == L"steamcommunity.com" ||
                       host == L"cdn.cloudflare.steamstatic.com" || host == L"shared.akamai.steamstatic.com" ||
                       host == L"shared.fastly.steamstatic.com" || host == L"cdn.akamai.steamstatic.com";
    const bool epic = host == L"store.epicgames.com" || host == L"store-site-backend-static.ak.epicgames.com" ||
                      host == L"cdn1.epicgames.com" || host == L"cdn2.unrealengine.com";
    const bool discord = host == L"discord.com" || host == L"cdn.discordapp.com";
    const bool metadata = host == L"api.steamcmd.net";
    const bool community = host == L"raw.githubusercontent.com";
    const bool mirror = host == L"markterence.github.io";
    if (!steam && !epic && !discord && !metadata && !community && !mirror)
        throw std::runtime_error("Unsupported game artwork host.");
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength)
        path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    if (community && path != L"/shizeh/discord-quests-database/main/README.md")
        throw std::runtime_error("Unsupported metadata source.");
    if (mirror && path != L"/discord-quest-completer/detectable.json")
        throw std::runtime_error("Unsupported catalog mirror.");
    if (metadata && (path.rfind(L"/v1/info/", 0) != 0 || path.size() <= 9 ||
                     path.substr(9).find_first_not_of(L"0123456789") != std::wstring::npos))
        throw std::runtime_error("Invalid Steam metadata URL.");
    std::wstring proxy;
    wchar_t configuredProxy[4096]{};
    const auto proxyLength = GetEnvironmentVariableW(L"HTTPS_PROXY", configuredProxy, 4096);
    if (proxyLength && proxyLength < 4096) {
        URL_COMPONENTS proxyParts{sizeof(proxyParts)};
        proxyParts.dwHostNameLength = proxyParts.dwUserNameLength = proxyParts.dwPasswordLength =
            static_cast<DWORD>(-1);
        if (WinHttpCrackUrl(configuredProxy, 0, 0, &proxyParts) && proxyParts.nScheme == INTERNET_SCHEME_HTTP &&
            !proxyParts.dwUserNameLength && !proxyParts.dwPasswordLength)
            proxy = std::wstring(proxyParts.lpszHostName, proxyParts.dwHostNameLength) + L":" +
                    std::to_wstring(proxyParts.nPort);
    }
    Internet session(
        WinHttpOpen(L"QuestCompleter/4.6",
                    proxy.empty() ? WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY : WINHTTP_ACCESS_TYPE_NAMED_PROXY,
                    proxy.empty() ? WINHTTP_NO_PROXY_NAME : proxy.c_str(), WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.h)
        throw std::runtime_error("Could not initialize networking.");
    WinHttpSetTimeouts(session.h, 2500, 2500, 4000, 4000);
    Internet connection(WinHttpConnect(session.h, host.c_str(), parts.nPort, 0));
    if (!connection.h)
        throw std::runtime_error("Could not connect to the catalog.");
    Internet request(WinHttpOpenRequest(connection.h, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!request.h)
        throw std::runtime_error("Could not create network request.");
    RequestGuard registration(cancel, request);
    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(request.h, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));
    if (!WinHttpSendRequest(request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.h, nullptr))
        throw std::runtime_error("The catalog is unavailable. Please try again.");
    DWORD status = 0, length = sizeof(status);
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &length, WINHTTP_NO_HEADER_INDEX);
    if (status != 200)
        throw std::runtime_error("Catalog returned HTTP " + std::to_string(status) + ".");
    std::string bytes;
    for (;;) {
        DWORD available = 0, read = 0;
        if (!WinHttpQueryDataAvailable(request.h, &available))
            throw std::runtime_error("Network read failed.");
        if (!available)
            break;
        const size_t limit = host == L"discord.com" || mirror ? 32 * 1024 * 1024 : 8 * 1024 * 1024;
        if (bytes.size() + available > limit)
            throw std::runtime_error("Game response is too large.");
        const size_t old = bytes.size();
        bytes.resize(old + available);
        if (!WinHttpReadData(request.h, bytes.data() + old, available, &read))
            throw std::runtime_error("Network read failed.");
        bytes.resize(old + read);
    }
    return bytes;
}
inline std::wstring Encode(const std::wstring &s) {
    const auto bytes = Utf8(s);
    std::wstring out;
    constexpr wchar_t hex[] = L"0123456789ABCDEF";
    for (unsigned char c : bytes) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')
            out.push_back(c);
        else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}
enum class Provider { Steam, Epic, All, Discord };
struct Game {
    int id = 0;
    std::wstring name, artUrl;
    fs::path artFile;
    Provider provider = Provider::Steam;
    std::string catalogId, catalogNamespace, description, storeUrl;
    std::string discordApplicationId, discordMatch;
    bool referenceOnly = false;
    bool discordLookupAttempted = false;
    std::vector<std::wstring> searchAliases;
    std::vector<std::wstring> windowsExecutables;
    std::string windowsExecutableSource, windowsExecutableSourceUrl;
    bool discordExecutableRule = false;
    std::string steamApplicationId, standardUnavailableReason;
    std::wstring steamInstallDirectory;
    bool executableLookupAttempted = false, executableResolving = false, standardRequiresSteam = false;
    std::string Identity() const {
        if (provider == Provider::Discord)
            return "discord:" + discordApplicationId;
        return provider == Provider::Steam
                   ? "steam:" + std::to_string(id)
                   : "epic:" + (discordApplicationId.empty() ? catalogNamespace + ":" + catalogId
                                                             : "discord:" + discordApplicationId);
    }
    bool Valid() const {
        return !name.empty() && (provider == Provider::Steam     ? id > 0
                                 : provider == Provider::Epic    ? !catalogId.empty()
                                 : provider == Provider::Discord ? !discordApplicationId.empty()
                                                                 : false);
    }
    bool SameApplication(const Game &other) const {
        if (!discordApplicationId.empty() && !other.discordApplicationId.empty())
            return discordApplicationId == other.discordApplicationId;
        return Identity() == other.Identity();
    }
};
inline std::vector<Game> ParseSearch(const std::string &bytes, const fs::path &data) {
    const auto response = Json::parse(bytes);
    std::vector<Game> games;
    if (!response.is_array())
        throw std::runtime_error("Unexpected game catalog response.");
    for (const auto &item : response) {
        Game g;
        try {
            g.id = std::stoi(item.value("appid", std::string{}));
        } catch (...) {
            continue;
        }
        g.name = Wide(item.value("name", std::string{}));
        g.artUrl = Wide(item.value("icon", std::string{}));
        if (g.artUrl.empty())
            g.artUrl = Wide(item.value("logo", std::string{}));
        g.artFile = data / L"art" / (std::to_wstring(g.id) + L"-icon.jpg");
        if (g.id > 0 && !g.name.empty())
            games.push_back(std::move(g));
        if (games.size() == 20)
            break;
    }
    return games;
}
inline std::vector<Game> SearchGames(const std::wstring &query, const fs::path &data,
                                     Cancellation *cancel = nullptr) {
    return ParseSearch(Get(L"https://steamcommunity.com/actions/SearchApps/" + Encode(query), cancel), data);
}
#include "catalog.inc"
#include "session.inc"
#include "runtime.inc"
} // namespace services
