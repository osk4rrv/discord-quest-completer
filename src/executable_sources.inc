inline std::wstring ExecutableName(std::wstring name) {
    // Catalog paths are metadata, never destinations. Only a conservative basename can be deployed.
    const auto slash = name.find_last_of(L"/\\");
    if (slash != std::wstring::npos)
        name.erase(0, slash + 1);
    if (name.size() < 5 || name.size() > 120 || Lower(name.substr(name.size() - 4)) != L".exe")
        return {};
    if (name.front() == L'.' || name.find_first_of(L"<>:\"/\\|?*") != std::wstring::npos)
        return {};
    for (wchar_t c : name)
        if (c < 32)
            return {};
    const auto stem = Lower(name.substr(0, name.find(L'.')));
    if (stem == L"con" || stem == L"prn" || stem == L"aux" || stem == L"nul" ||
        (stem.size() == 4 && (stem.substr(0, 3) == L"com" || stem.substr(0, 3) == L"lpt") && stem[3] >= L'0' &&
         stem[3] <= L'9'))
        return {};
    return name;
}
inline std::wstring StandardExecutable(const Game &game) {
    for (const auto &name : game.windowsExecutables) {
        const auto safe = ExecutableName(name);
        if (!safe.empty())
            return safe;
    }
    return {};
}
inline fs::path ExecutableRelativePath(std::wstring name) {
    std::replace(name.begin(), name.end(), L'\\', L'/');
    if (name.empty() || name.front() == L'/' || name.size() > 500)
        return {};
    fs::path result;
    size_t start = 0;
    int depth = 0;
    while (start < name.size()) {
        const auto end = name.find(L'/', start);
        const auto part = name.substr(start, end == std::wstring::npos ? end : end - start);
        if (part.empty() || part == L"." || part == L".." ||
            part.find_first_of(L"<>:\"|?*{}") != std::wstring::npos || part.back() == L'.' || part.back() == L' ' ||
            ++depth > 12)
            return {};
        for (wchar_t c : part)
            if (c < 32)
                return {};
        result /= part;
        if (end == std::wstring::npos)
            break;
        start = end + 1;
    }
    return ExecutableName(result.filename().wstring()).empty() ? fs::path{} : result;
}
inline fs::path StandardPath(const Game &game) {
    for (const auto &name : game.windowsExecutables) {
        auto relative = ExecutableRelativePath(name);
        if (!relative.empty())
            return relative;
    }
    return {};
}
inline bool SteamIdValid(const std::string &id) {
    return !id.empty() && id.size() <= 10 && id != "0" && id.find_first_not_of("0123456789") == std::string::npos;
}
inline void BindSteamSku(Game &game, const Json &entry) {
    std::set<std::string> ids;
    for (const auto &sku : entry.value("skus", Json::array()))
        if (CatalogText(sku, "distributor") == "steam" && SteamIdValid(CatalogText(sku, "id")))
            ids.insert(CatalogText(sku, "id"));
    game.steamApplicationId = game.provider == Provider::Steam ? std::to_string(game.id)
                              : ids.size() == 1                ? *ids.begin()
                                                               : "";
}
inline Json ParseSteamExecutables(const std::string &bytes, const std::string &steamId) {
    const auto response = Json::parse(bytes);
    if (!SteamIdValid(steamId) || CatalogText(response, "status") != "success" || !response.contains("data") ||
        !response["data"].is_object() || !response["data"].contains(steamId))
        throw std::runtime_error("Steam launch metadata is unavailable.");
    const auto &app = response["data"][steamId];
    const auto common = app.value("common", Json::object());
    if (!CatalogText(common, "gameid").empty() && CatalogText(common, "gameid") != steamId)
        throw std::runtime_error("Steam metadata ID mismatch.");
    const auto config = app.value("config", Json::object());
    const auto directory = Wide(CatalogText(config, "installdir"));
    Json names = Json::array();
    std::set<std::wstring> seen;
    bool launcherOnly = false;
    for (const auto &option : config.value("launch", Json::object())) {
        if (!option.is_object())
            continue;
        const auto os = CatalogText(option.value("config", Json::object()), "oslist");
        if (!os.empty() && os.find("windows") == std::string::npos)
            continue;
        const auto executable = Wide(CatalogText(option, "executable"));
        const auto relative = ExecutableRelativePath(executable);
        const auto description = Lower(Wide(CatalogText(option, "description")));
        const auto filename = Lower(relative.filename().wstring());
        if (relative.empty() || filename.find(L"launcher") != std::wstring::npos ||
            description.find(L"dedicated server") != std::wstring::npos || filename == L"steam.exe" ||
            filename == L"cmd.exe") {
            launcherOnly |= !executable.empty();
            continue;
        }
        const auto path =
            ExecutableRelativePath(directory.empty() ? executable : directory + L"/" + relative.generic_wstring());
        if (!path.empty() && seen.insert(Lower(path.generic_wstring())).second)
            names.push_back(Utf8(path.generic_wstring()));
    }
    return {{"version", 1},
            {"steamId", steamId},
            {"executables", names},
            {"source", "Steam launch metadata (SteamCMD.net)"},
            {"sourceUrl", "https://api.steamcmd.net/v1/info/" + steamId},
            {"reason", names.empty() ? (launcherOnly ? "Launcher only; no direct Windows game executable"
                                                     : "No Windows launch executable published")
                                     : ""}};
}
inline Json ParseCommunityExecutables(const std::string &text) {
    Json records = Json::array();
    size_t offset = 0;
    while (offset < text.size()) {
        const auto end = text.find('\n', offset);
        const auto line = text.substr(offset, end == std::string::npos ? end : end - offset);
        offset = end == std::string::npos ? text.size() : end + 1;
        if (line.empty() || line.front() != '|')
            continue;
        const auto divider = line.find('|', 1);
        if (divider == std::string::npos)
            continue;
        auto title = line.substr(1, divider - 1);
        const bool required = title.find("Steam Demo Required") != std::string::npos;
        const auto qualifier = title.find("*(");
        if (qualifier != std::string::npos)
            title.resize(qualifier);
        const auto first = title.find_first_not_of(" \t"), last = title.find_last_not_of(" \t");
        if (first == std::string::npos)
            continue;
        title = title.substr(first, last - first + 1);
        Json paths = Json::array();
        auto start = line.find(char(96), divider);
        while (start != std::string::npos) {
            const auto finish = line.find(char(96), start + 1);
            if (finish == std::string::npos)
                break;
            auto path = Wide(line.substr(start + 1, finish - start - 1));
            // The table's leading '..' denotes the documented root, not an AppData parent.
            if (path.rfind(L"..\\", 0) == 0 || path.rfind(L"../", 0) == 0)
                path.erase(0, 3);
            const auto safe = ExecutableRelativePath(path);
            if (!safe.empty() && Lower(path).find(L"xxxx") == std::wstring::npos)
                paths.push_back(Utf8(safe.generic_wstring()));
            start = line.find(char(96), finish + 1);
        }
        if (!paths.empty())
            records.push_back({{"name", title}, {"executables", paths}, {"requiresSteam", required}});
    }
    if (records.empty())
        throw std::runtime_error("Community executable table is unavailable.");
    return {{"version", 1}, {"games", records}};
}
inline void ApplyExecutableRecord(Game &game, const Json &record, bool preferSource = false) {
    if (!preferSource && !StandardPath(game).empty())
        return;
    // Replace unusable catalog entries so every displayed path has the stated source.
    game.windowsExecutables.clear();
    for (const auto &name : record.value("executables", Json::array()))
        if (name.is_string() && !ExecutableRelativePath(Wide(name.get<std::string>())).empty())
            game.windowsExecutables.push_back(Wide(name.get<std::string>()));
    game.windowsExecutableSource = CatalogText(record, "source");
    game.windowsExecutableSourceUrl = CatalogText(record, "sourceUrl");
    game.standardUnavailableReason = CatalogText(record, "reason");
    game.standardRequiresSteam = record.value("requiresSteam", false);
    if (game.standardRequiresSteam)
        game.standardUnavailableReason = "Requires a Steam demo installation";
    game.discordExecutableRule = false;
}
inline Json BundledQuestPaths() {
    const auto resource = FindResourceW(nullptr, MAKEINTRESOURCEW(106), RT_RCDATA);
    if (!resource)
        return Json::object();
    const auto *bytes = static_cast<const char *>(LockResource(LoadResource(nullptr, resource)));
    return Json::parse(std::string(bytes, SizeofResource(nullptr, resource)));
}
inline void ApplyCuratedExecutableBatch(std::vector<Game> &games, const Json &catalog,
                                        const Json &directory) {
    std::map<std::wstring, std::set<std::string>> titles;
    for (const auto &app : catalog) {
        titles[TitleKey(Wide(CatalogText(app, "name")))].insert(CatalogText(app, "id"));
        for (const auto &alias : app.value("aliases", Json::array()))
            if (alias.is_string())
                titles[TitleKey(Wide(alias.get<std::string>()))].insert(CatalogText(app, "id"));
    }
    std::map<std::string, std::vector<Json>> matches;
    for (const auto &record : directory.value("games", Json::array())) {
        std::set<std::string> ids;
        const auto primary = titles.find(TitleKey(Wide(CatalogText(record, "name"))));
        if (primary != titles.end())
            ids = primary->second;
        std::vector<std::wstring> names;
        if (ids.empty())
            for (const auto &alias : record.value("aliases", Json::array()))
                if (alias.is_string())
                    names.push_back(Wide(alias.get<std::string>()));
        for (const auto &name : names) {
            const auto found = titles.find(TitleKey(name));
            if (found != titles.end())
                ids.insert(found->second.begin(), found->second.end());
        }
        const auto explicitId = CatalogText(record, "discordId");
        // Explicit profiles must agree with the catalog title and its own store SKU.
        if (ids.size() == 1 && (explicitId.empty() || ids.count(explicitId)))
            matches[*ids.begin()].push_back(record);
    }
    for (auto &game : games) {
        const auto found = matches.find(game.discordApplicationId);
        if (found == matches.end() || found->second.size() != 1)
            continue;
        const auto &record = found->second.front();
        const auto steamId = CatalogText(record, "steamId");
        if (!steamId.empty() && game.steamApplicationId != steamId)
            continue;
        ApplyExecutableRecord(game, record, true);
        game.steamInstallDirectory = Wide(CatalogText(record, "steamInstallDirectory"));
        for (const auto &alias : record.value("aliases", Json::array()))
            if (alias.is_string())
                game.searchAliases.push_back(Wide(alias.get<std::string>()));
    }
}
inline void ApplyCommunityExecutables(Game &game, const Json &catalog, const Json &directory) {
    if (!StandardPath(game).empty() || game.discordApplicationId.empty())
        return;
    std::set<std::wstring> keys{TitleKey(game.name)};
    for (const auto &alias : game.searchAliases)
        keys.insert(TitleKey(alias));
    std::vector<Json> matches;
    for (const auto &record : directory.value("games", Json::array())) {
        const auto key = TitleKey(Wide(CatalogText(record, "name")));
        if (!keys.count(key))
            continue;
        std::set<std::string> ids;
        for (const auto &app : catalog) {
            bool same = TitleKey(Wide(CatalogText(app, "name"))) == key;
            for (const auto &alias : app.value("aliases", Json::array()))
                if (alias.is_string())
                    same |= TitleKey(Wide(alias.get<std::string>())) == key;
            if (same)
                ids.insert(CatalogText(app, "id"));
        }
        if (ids.size() == 1 && ids.count(game.discordApplicationId))
            matches.push_back(record);
    }
    if (matches.size() != 1)
        return;
    auto record = matches.front();
    record["source"] = "Community Quest executable directory";
    record["sourceUrl"] = "https://github.com/shizeh/discord-quests-database";
    ApplyExecutableRecord(game, record);
}
inline Json LoadCommunityDirectory(const fs::path &data, Cancellation *cancel) {
    const auto file = data / L"catalog" / L"quest-executables.json";
    Json cached;
    try {
        cached = Json::parse(Read(file));
        if (!cached.contains("games") || !cached["games"].is_array() || cached["games"].empty())
            cached = Json{};
        if (!cached.empty() && Fresh(file, 24))
            return cached;
    } catch (...) {
    }
    try {
        auto directory = ParseCommunityExecutables(
            Get(L"https://raw.githubusercontent.com/shizeh/discord-quests-database/main/README.md", cancel));
        Save(file, directory.dump());
        return directory;
    } catch (...) {
        if (!cached.empty())
            return cached;
        throw;
    }
}
inline void ApplyCommunityExecutableBatch(std::vector<Game> &games, const Json &catalog,
                                          const Json &directory) {
    std::map<std::wstring, std::set<std::string>> titles;
    for (const auto &app : catalog) {
        titles[TitleKey(Wide(CatalogText(app, "name")))].insert(CatalogText(app, "id"));
        for (const auto &alias : app.value("aliases", Json::array()))
            if (alias.is_string())
                titles[TitleKey(Wide(alias.get<std::string>()))].insert(CatalogText(app, "id"));
    }
    std::map<std::string, std::vector<Json>> records;
    for (const auto &record : directory.value("games", Json::array())) {
        const auto found = titles.find(TitleKey(Wide(CatalogText(record, "name"))));
        if (found != titles.end() && found->second.size() == 1)
            records[*found->second.begin()].push_back(record);
    }
    for (auto &game : games) {
        const auto found = records.find(game.discordApplicationId);
        if (found == records.end() || found->second.size() != 1)
            continue;
        auto record = found->second.front();
        record["source"] = "Community Quest executable directory";
        record["sourceUrl"] = "https://github.com/shizeh/discord-quests-database";
        ApplyExecutableRecord(game, record);
    }
}
inline void LoadCachedExecutableMetadata(Game &game, const fs::path &data) {
    if (!StandardPath(game).empty())
        return;
    const auto id = game.provider == Provider::Steam ? std::to_string(game.id) : game.steamApplicationId;
    if (!SteamIdValid(id))
        return;
    const auto file = data / L"catalog" / L"steam-launch" / Wide(id + ".json");
    try {
        if (Fresh(file, 24)) {
            const auto record = Json::parse(Read(file));
            if (CatalogText(record, "steamId") == id) {
                ApplyExecutableRecord(game, record);
                game.executableLookupAttempted = true;
            }
        }
    } catch (...) {
    }
}
inline void ResolveExecutableMetadata(Game &game, const fs::path &data, Cancellation *cancel,
                                      const Json &catalog) {
    if (!StandardPath(game).empty())
        return;
    try {
        ApplyCommunityExecutables(game, catalog, LoadCommunityDirectory(data, cancel));
    } catch (...) {
    }
    if (StandardPath(game).empty()) {
        const auto id = game.provider == Provider::Steam ? std::to_string(game.id) : game.steamApplicationId;
        if (SteamIdValid(id)) {
            const auto file = data / L"catalog" / L"steam-launch" / Wide(id + ".json");
            try {
                Json record;
                if (Fresh(file, 24))
                    record = Json::parse(Read(file));
                else {
                    record =
                        ParseSteamExecutables(Get(L"https://api.steamcmd.net/v1/info/" + Wide(id), cancel), id);
                    Save(file, record.dump());
                }
                if (CatalogText(record, "steamId") == id)
                    ApplyExecutableRecord(game, record);
            } catch (const std::exception &error) {
                game.standardUnavailableReason = error.what();
            }
        } else
            game.standardUnavailableReason = "No published Windows executable or linked Steam app";
    }
    game.executableLookupAttempted = true;
    game.executableResolving = false;
}
