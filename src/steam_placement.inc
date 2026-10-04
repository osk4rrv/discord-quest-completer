// Scoped Steam context for sourced profiles. Existing installations are never replaced.
inline fs::path RegisteredSteamRoot() {
    wchar_t path[32768]{};
    DWORD size = sizeof(path);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath",
                     RRF_RT_REG_SZ, nullptr, path, &size) != ERROR_SUCCESS)
        return {};
    const fs::path root(path);
    return root.is_absolute() && fs::is_directory(root / L"steamapps") ? root : fs::path{};
}
inline bool PlainDirectoryTree(const fs::path &path) {
    for (auto part = path; !part.empty(); part = part.parent_path()) {
        const auto attributes = GetFileAttributesW(part.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            return false;
        if (part == part.parent_path())
            break;
    }
    return true;
}
inline std::vector<fs::path> SteamLibraryRoots(const fs::path &registeredRoot) {
    std::vector<fs::path> libraries;
    auto add = [&](const fs::path &path) {
        if (!path.is_absolute() || !PlainDirectoryTree(path / L"steamapps" / L"common"))
            return;
        if (std::none_of(libraries.begin(), libraries.end(), [&](const fs::path &existing) {
                return Lower(existing.lexically_normal().wstring()) == Lower(path.lexically_normal().wstring());
            }))
            libraries.push_back(path);
    };
    add(registeredRoot);
    if (!registeredRoot.empty()) {
        const auto text = Read(registeredRoot / L"steamapps" / L"libraryfolders.vdf");
        const std::regex pathEntry(R"vdf("path"\s*"([^"\r\n]+)")vdf");
        for (std::sregex_iterator it(text.begin(), text.end(), pathEntry), end; it != end; ++it) {
            auto path = Wide((*it)[1].str());
            for (size_t pos = 0; (pos = path.find(L"\\\\", pos)) != std::wstring::npos; ++pos)
                path.replace(pos, 2, L"\\");
            add(fs::path(path));
        }
    }
    return libraries;
}
inline fs::path SelectSteamLibrary(const std::vector<fs::path> &libraries) {
    // Prefer a library containing a real FC27 installation, so it is protected rather than duplicated.
    for (const auto &library : libraries)
        if (fs::exists(library / L"steamapps" / L"appmanifest_4080220.acf") ||
            fs::exists(library / L"steamapps" / L"common" / L"EA SPORTS FC 27"))
            return library;
    return libraries.empty() ? fs::path{} : libraries.front();
}
inline fs::path InstalledSteamRoot() {
    return SelectSteamLibrary(SteamLibraryRoots(RegisteredSteamRoot()));
}
inline bool Fc27DetectionProfile(const Game &game) {
    return game.discordApplicationId == "1531874756096295054" && game.steamApplicationId == "4080220" &&
           game.steamInstallDirectory == L"EA SPORTS FC 27" && StandardPath(game) == fs::path(L"fc27.exe");
}
inline std::string FileIdentity(const fs::path &path) {
    NativeHandle file(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE |
                                 FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    BY_HANDLE_FILE_INFORMATION info{};
    if (!file.Get() || !GetFileInformationByHandle(file.Get(), &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
        return {};
    return std::to_string(info.dwVolumeSerialNumber) + ":" + std::to_string(info.nFileIndexHigh) + ":" +
           std::to_string(info.nFileIndexLow) + ":" + std::to_string(info.nFileSizeHigh) + ":" +
           std::to_string(info.nFileSizeLow) + ":" + std::to_string(info.ftLastWriteTime.dwHighDateTime) + ":" +
           std::to_string(info.ftLastWriteTime.dwLowDateTime);
}
inline std::string SteamSessionManifest(const std::wstring &key) {
    return "\"AppState\"\n{\n\t\"appid\" \"4080220\"\n\t\"name\" \"EA SPORTS FC 27\"\n"
           "\t\"StateFlags\" \"4\"\n\t\"installdir\" \"EA SPORTS FC 27\"\n}\n"
           "// QuestCompleter session " + Utf8(key) + "\n";
}
inline void RecoverSteamPlacements(const fs::path &data, const fs::path &steamRoot) {
    const auto journals = data / L"placements";
    if (!fs::is_directory(journals) || !PlainDirectoryTree(steamRoot / L"steamapps" / L"common"))
        return;
    const auto gameFolder = steamRoot / L"steamapps" / L"common" / L"EA SPORTS FC 27";
    const auto executable = gameFolder / L"fc27.exe";
    const auto manifest = steamRoot / L"steamapps" / L"appmanifest_4080220.acf";
    for (const auto &entry : fs::directory_iterator(journals)) {
        try {
            if (!entry.is_regular_file() || entry.path().extension() != L".json" ||
                FileIdentity(entry.path()).empty())
                continue;
            const auto key = entry.path().stem().wstring();
            GUID parsed{};
            if (FAILED(CLSIDFromString(key.c_str(), &parsed)))
                continue;
            const auto record = Json::parse(Read(entry.path()));
            auto samePath = [](const std::string &stored, const fs::path &expected) {
                return Lower(fs::path(Wide(stored)).lexically_normal().wstring()) ==
                       Lower(expected.lexically_normal().wstring());
            };
            if (!samePath(CatalogText(record, "steamRoot"), steamRoot) ||
                !samePath(CatalogText(record, "executable"), executable) ||
                !samePath(CatalogText(record, "manifest"), manifest) ||
                CatalogText(record, "manifestContents") != SteamSessionManifest(key))
                continue;
            const auto owner = record.value("ownerPid", DWORD(0));
            if (!owner || owner == GetCurrentProcessId())
                continue;
            NativeHandle process(OpenProcess(SYNCHRONIZE, FALSE, owner));
            if (process.Get() ? WaitForSingleObject(process.Get(), 0) != WAIT_OBJECT_0
                              : GetLastError() != ERROR_INVALID_PARAMETER)
                continue; // Live or inaccessible owner: leave its files alone.
            std::error_code ignored;
            if (!FileIdentity(manifest).empty() && Read(manifest) == SteamSessionManifest(key))
                fs::remove(manifest, ignored);
            const auto identity = CatalogText(record, "executableIdentity");
            if (!identity.empty() && PlainDirectoryTree(gameFolder) && FileIdentity(executable) == identity)
                fs::remove(executable, ignored);
            if (PlainDirectoryTree(gameFolder))
                fs::remove(gameFolder, ignored);
            if (!fs::exists(executable) && !fs::exists(manifest))
                fs::remove(entry.path(), ignored);
        } catch (...) { }
    }
}
inline void CopyNewFile(const fs::path &source, const fs::path &destination) {
    if (!CopyFileW(source.c_str(), destination.c_str(), TRUE))
        throw std::runtime_error("Could not create Steam session helper (Windows error " +
                                 std::to_string(GetLastError()) + "). Existing files are preserved.");
}
inline void WriteNewFile(const fs::path &path, const std::string &contents) {
    NativeHandle file(CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                                 FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file.Get())
        throw std::runtime_error("Could not create Steam session manifest (Windows error " +
                                 std::to_string(GetLastError()) + "). Existing manifests are preserved.");
    DWORD written = 0;
    if (!WriteFile(file.Get(), contents.data(), static_cast<DWORD>(contents.size()), &written, nullptr) ||
        written != contents.size() || !FlushFileBuffers(file.Get())) {
        file.Reset();
        std::error_code ignored;
        fs::remove(path, ignored);
        throw std::runtime_error("Could not write Steam session manifest.");
    }
}
class SteamSessionPlacement {
    NativeHandle exclusive;
    fs::path root, gameFolder, executable, manifest, journal;
    std::string executableIdentity, manifestContents;
    bool createdFolder = false, createdManifest = false;

  public:
    SteamSessionPlacement() = default;
    void Prepare(const Game &game, const fs::path &data, const std::wstring &key, const fs::path &module,
                 const fs::path &qaRoot = {}) {
        // Only this researched profile may write a Steam manifest. Metadata cannot choose arbitrary roots.
        if (!Fc27DetectionProfile(game))
            throw std::runtime_error("Unrecognized Steam detection profile.");
        exclusive.Reset(CreateMutexW(nullptr, TRUE, L"Local\\QuestCompleter.Steam.4080220"));
        if (!exclusive.Get() || GetLastError() == ERROR_ALREADY_EXISTS)
            throw std::runtime_error("A Steam FC 27 session is already running.");
        root = qaRoot.empty() ? InstalledSteamRoot() : qaRoot;
        if (root.empty() || !root.is_absolute() || !PlainDirectoryTree(root / L"steamapps" / L"common"))
            throw std::runtime_error("Steam is required for FC 27. No usable local Steam library was found.");
        RecoverSteamPlacements(data, root);
        gameFolder = root / L"steamapps" / L"common" / game.steamInstallDirectory;
        manifest = root / L"steamapps" / L"appmanifest_4080220.acf";
        if (fs::exists(gameFolder) || fs::exists(manifest))
            throw std::runtime_error("FC 27 files already exist in Steam. Start your installed game; no files were replaced.");
        if (!fs::create_directory(gameFolder))
            throw std::runtime_error("Could not create a new FC 27 session folder.");
        createdFolder = true;
        executable = gameFolder / L"fc27.exe";
        journal = data / L"placements" / (key + L".json");
        CopyNewFile(module, executable);
        executableIdentity = FileIdentity(executable);
        if (executableIdentity.empty())
            throw std::runtime_error("Could not verify the new session helper.");
        manifestContents = SteamSessionManifest(key);
        Save(journal, Json{{"steamRoot", Utf8(root.wstring())}, {"executable", Utf8(executable.wstring())},
                           {"executableIdentity", executableIdentity}, {"manifest", Utf8(manifest.wstring())},
                           {"manifestContents", manifestContents}, {"ownerPid", GetCurrentProcessId()}}
                          .dump(2));
        WriteNewFile(manifest, manifestContents);
        createdManifest = true;
    }
    fs::path Executable() const { return executable; }
    fs::path Folder() const { return gameFolder; }
    ~SteamSessionPlacement() {
        try {
        std::error_code ignored;
        if (createdManifest && PlainDirectoryTree(manifest.parent_path()) && !FileIdentity(manifest).empty() &&
            Read(manifest) == manifestContents)
            fs::remove(manifest, ignored);
        if (!executableIdentity.empty() && PlainDirectoryTree(executable.parent_path()) &&
            FileIdentity(executable) == executableIdentity)
            fs::remove(executable, ignored);
        if (createdFolder && PlainDirectoryTree(gameFolder))
            fs::remove(gameFolder, ignored); // Empty directory only; never recursive.
        if (!journal.empty() && !fs::exists(executable) && (!createdManifest || !fs::exists(manifest)))
            fs::remove(journal, ignored);
        } catch (...) { } // Keep the journal if cleanup is denied; a later run can recover it.
    }
};
