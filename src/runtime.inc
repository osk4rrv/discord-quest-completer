// Local process lifecycle and Discord Rich Presence IPC. Quest rewards are verified in Discord.
enum class RuntimeStatus : LONG {
    Starting,
    ProcessRunning,
    Connecting,
    Active,
    Disconnected,
    Error,
    Stopped,
    Complete,
    LaunchFailed
};
struct alignas(8) RuntimeShared {
    volatile LONG status = static_cast<LONG>(RuntimeStatus::Starting);
    volatile LONG error = 0;
    alignas(8) volatile LONG64 elapsedMs = 0;
    alignas(8) volatile LONG64 window = 0;
};
struct RuntimeSnapshot {
    RuntimeStatus status = RuntimeStatus::Starting;
    LONG error = 0;
    double elapsed = 0;
    bool operator!=(const RuntimeSnapshot &other) const {
        return status != other.status || error != other.error ||
               static_cast<int>(elapsed) != static_cast<int>(other.elapsed);
    }
};
inline RuntimeSnapshot ReadRuntime(RuntimeShared *state) {
    return {static_cast<RuntimeStatus>(InterlockedCompareExchange(&state->status, 0, 0)),
            InterlockedCompareExchange(&state->error, 0, 0),
            static_cast<double>(InterlockedCompareExchange64(&state->elapsedMs, 0, 0)) / 1000};
}
inline void SetRuntime(RuntimeShared *state, RuntimeStatus status, LONG error = 0) {
    InterlockedExchange(&state->error, error);
    InterlockedExchange(&state->status, static_cast<LONG>(status));
}
inline std::wstring RuntimeLabel(RuntimeStatus status) {
    switch (status) {
    case RuntimeStatus::Starting:
        return L"Starting…";
    case RuntimeStatus::ProcessRunning:
        return L"Process running";
    case RuntimeStatus::Connecting:
        return L"Connecting to Discord…";
    case RuntimeStatus::Active:
        return L"Rich Presence active";
    case RuntimeStatus::Disconnected:
        return L"Waiting for Discord";
    case RuntimeStatus::Error:
        return L"Discord rejected activity";
    case RuntimeStatus::Stopped:
        return L"Session stopped";
    case RuntimeStatus::Complete:
        return L"Session complete";
    case RuntimeStatus::LaunchFailed:
        return L"Could not start session";
    }
    return {};
}
class NativeHandle {
    HANDLE handle = nullptr;

  public:
    explicit NativeHandle(HANDLE h = nullptr) : handle(h == INVALID_HANDLE_VALUE ? nullptr : h) {}
    ~NativeHandle() {
        if (handle)
            CloseHandle(handle);
    }
    NativeHandle(const NativeHandle &) = delete;
    NativeHandle &operator=(const NativeHandle &) = delete;
    HANDLE Get() const {
        return handle;
    }
    void Reset(HANDLE h = nullptr) {
        if (handle)
            CloseHandle(handle);
        handle = h == INVALID_HANDLE_VALUE ? nullptr : h;
    }
};
inline bool ApplicationIdValid(const std::string &id) {
    return id.size() >= 15 && id.size() <= 20 && id.find_first_not_of("0123456789") == std::string::npos;
}
inline Json RuntimeGameJson(const Game &g) {
    Json executables = Json::array();
    for (const auto &name : g.windowsExecutables)
        executables.push_back(Utf8(name));
    return {{"id", g.id},
            {"name", Utf8(g.name)},
            {"art", Utf8(g.artFile.wstring())},
            {"provider", static_cast<int>(g.provider)},
            {"catalogId", g.catalogId},
            {"namespace", g.catalogNamespace},
            {"discordId", g.discordApplicationId},
            {"discordMatch", g.discordMatch},
            {"storeUrl", g.storeUrl},
            {"steamApplicationId", g.steamApplicationId},
            {"steamInstallDirectory", Utf8(g.steamInstallDirectory)},
            {"standardUnavailableReason", g.standardUnavailableReason},
            {"standardRequiresSteam", g.standardRequiresSteam},
            {"executables", executables},
            {"executableSource", g.windowsExecutableSource},
            {"executableSourceUrl", g.windowsExecutableSourceUrl},
            {"discordExecutableRule", g.discordExecutableRule}};
}
inline Game RuntimeGame(const Json &j) {
    Game g;
    g.id = j.value("id", 0);
    g.name = Wide(j.value("name", std::string{}));
    g.artFile = Wide(j.value("art", std::string{}));
    g.provider = static_cast<Provider>(j.value("provider", 0));
    g.catalogId = j.value("catalogId", std::string{});
    g.catalogNamespace = j.value("namespace", std::string{});
    g.discordApplicationId = j.value("discordId", std::string{});
    g.discordMatch = j.value("discordMatch", std::string{});
    g.storeUrl = j.value("storeUrl", std::string{});
    g.steamApplicationId = j.value("steamApplicationId", std::string{});
    g.steamInstallDirectory = Wide(j.value("steamInstallDirectory", std::string{}));
    g.standardUnavailableReason = j.value("standardUnavailableReason", std::string{});
    g.standardRequiresSteam = j.value("standardRequiresSteam", false);
    for (const auto &exe : j.value("executables", Json::array()))
        if (exe.is_string())
            g.windowsExecutables.push_back(Wide(exe.get<std::string>()));
    if (g.windowsExecutables.empty())
        g.windowsExecutables.push_back(Wide(j.value("executable", std::string{})));
    g.windowsExecutableSource = j.value("executableSource", std::string{});
    g.windowsExecutableSourceUrl = j.value("executableSourceUrl", std::string{});
    g.discordExecutableRule = j.value("discordExecutableRule", false);
    return g;
}
inline std::wstring RuntimeKey() {
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid)))
        throw std::runtime_error("Could not create a session identifier.");
    wchar_t buffer[40]{};
    StringFromGUID2(guid, buffer, 40);
    return buffer;
}
inline std::wstring MappingName(const std::wstring &key) {
    return L"Local\\QuestCompleter.session." + key;
}
inline std::wstring StopName(const std::wstring &key) {
    return L"Local\\QuestCompleter.stop." + key;
}

class RunnerRuntime;
inline std::shared_ptr<RunnerRuntime> CreateRpcRuntime(const std::wstring &key, const std::string &appId,
                                                       const std::wstring &testPipe, const fs::path &diagnostic);
#include "steam_placement.inc"
class ParentRun {
    NativeHandle mapping, stopped, job, process, localProfileLock;
    RuntimeShared *shared = nullptr;
    std::thread launch;
    std::atomic<bool> launched{false};
    fs::path config, runnerRoot, runnerExecutable;
    std::shared_ptr<RunnerRuntime> rpcRuntime;
    std::unique_ptr<SteamSessionPlacement> steamPlacement;

  public:
    ParentRun(const Game &game, bool rpc, const Preferences &preferences, const fs::path &data,
              bool hidden = false, const std::wstring &testPipe = {},
              const std::optional<fs::path> &qaSteamRoot = std::nullopt, unsigned prepareSeconds = 0) {
        if (rpc && !ApplicationIdValid(game.discordApplicationId))
            throw std::runtime_error("A valid Discord application ID is required.");
        const auto key = RuntimeKey();
        mapping.Reset(CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(RuntimeShared),
                                         MappingName(key).c_str()));
        stopped.Reset(CreateEventW(nullptr, TRUE, FALSE, StopName(key).c_str()));
        if (!mapping.Get() || !stopped.Get())
            throw std::runtime_error("Could not allocate a session.");
        shared = static_cast<RuntimeShared *>(
            MapViewOfFile(mapping.Get(), FILE_MAP_ALL_ACCESS, 0, 0, sizeof(RuntimeShared)));
        if (!shared)
            throw std::runtime_error("Could not map session state.");
        new (shared) RuntimeShared{};
        // Presence alone cannot replace Play Quest process detection. Use the sourced helper when possible.
        if (rpc && (StandardPath(game).empty() || game.standardRequiresSteam)) {
            try {
                rpcRuntime = CreateRpcRuntime(key, game.discordApplicationId, testPipe,
                                              data / L"info" / Wide(game.discordApplicationId + ".rpc.json"));
                launched = true;
                return;
            } catch (...) {
                UnmapViewOfFile(shared);
                shared = nullptr;
                throw;
            }
        }
        job.Reset(CreateJobObjectW(nullptr, nullptr));
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!job.Get() ||
            !SetInformationJobObject(job.Get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
            UnmapViewOfFile(shared);
            shared = nullptr;
            throw std::runtime_error("Could not configure session cleanup.");
        }
        config = data / L"sessions" / (key + L".json");
        const auto bytes = Json{{"game", RuntimeGameJson(game)},
                                {"rpc", rpc},
                                {"testPipe", Utf8(testPipe)},
                                {"preferences", Json::parse(preferences.Serialize())},
                                {"data", Utf8(data.wstring())}}
                               .dump();
        launch = std::thread([this, key, bytes, game, rpc, data, hidden, qaSteamRoot, prepareSeconds] {
            struct Finish {
                std::atomic<bool> &flag;
                ~Finish() {
                    flag = true;
                }
            } finish{launched};
            try {
                Save(config, bytes);
                wchar_t module[32768]{};
                if (!GetModuleFileNameW(nullptr, module, 32768))
                    throw std::runtime_error("Executable unavailable.");
                const auto name = StandardPath(game);
                if (name.empty())
                    throw std::runtime_error("No Windows executable in catalog.");
                auto folder = data / L"runners" / key;
                auto executable = folder / name;
                std::string placement = "Local folder";
                if (!game.steamInstallDirectory.empty()) {
                    if (!Fc27DetectionProfile(game))
                        throw std::runtime_error("Unrecognized FC27 folder profile.");
                    const auto steamRoot = qaSteamRoot ? *qaSteamRoot : InstalledSteamRoot();
                    if (!steamRoot.empty()) {
                        steamPlacement = std::make_unique<SteamSessionPlacement>();
                        steamPlacement->Prepare(game, data, key, fs::path(module), steamRoot);
                        folder = steamPlacement->Folder();
                        executable = steamPlacement->Executable();
                        placement = "Steam library";
                    } else {
                        localProfileLock.Reset(CreateMutexW(nullptr, TRUE, L"Local\\QuestCompleter.Steam.4080220"));
                        if (!localProfileLock.Get() || GetLastError() == ERROR_ALREADY_EXISTS)
                            throw std::runtime_error("An FC27 helper session is already running.");
                        folder /= game.steamInstallDirectory;
                        executable = folder / name;
                    }
                }
                if (!steamPlacement) {
                    runnerRoot = data / L"runners" / key;
                    runnerExecutable = executable;
                    fs::create_directories(executable.parent_path());
                    fs::copy_file(module, executable, fs::copy_options::overwrite_existing);
                }
                if (prepareSeconds) {
                    Save(data / L"info" / Wide(game.discordApplicationId + ".process.json"),
                         Json{{"applicationId", game.discordApplicationId}, {"stage", "prepared"},
                              {"executable", Utf8(executable.wstring())}, {"placement", placement},
                              {"steamManifestCreated", steamPlacement != nullptr},
                              {"preparedAt", UnixNow()}, {"launchDelaySeconds", std::min(prepareSeconds, 60u)}}.dump(2));
                }
                if (WaitForSingleObject(stopped.Get(), std::min(prepareSeconds, 60u) * 1000) == WAIT_OBJECT_0) {
                    SetRuntime(shared, RuntimeStatus::Stopped);
                    return;
                }
                std::wstring command = L"\"" + executable.wstring() + L"\" --session-runner \"" +
                                       config.wstring() + L"\" --runtime-key " + key +
                                       (hidden || rpc ? L" --runner-hidden" : L"");
                STARTUPINFOW startup{sizeof(startup)};
                PROCESS_INFORMATION info{};
                if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED,
                                    nullptr, folder.c_str(), &startup, &info)) {
                    SetRuntime(shared, RuntimeStatus::LaunchFailed, static_cast<LONG>(GetLastError()));
                    return;
                }
                process.Reset(info.hProcess);
                NativeHandle thread(info.hThread);
                if (!AssignProcessToJobObject(job.Get(), process.Get())) {
                    const auto error = GetLastError();
                    TerminateProcess(process.Get(), error);
                    SetRuntime(shared, RuntimeStatus::LaunchFailed, static_cast<LONG>(error));
                    return;
                }
                if (ResumeThread(thread.Get()) == static_cast<DWORD>(-1)) {
                    const auto error = GetLastError();
                    TerminateProcess(process.Get(), error);
                    SetRuntime(shared, RuntimeStatus::LaunchFailed, static_cast<LONG>(error));
                } else {
                    std::error_code ignored;
                    fs::remove(data / L"info" / Wide(game.discordApplicationId + ".launch.json"), ignored);
                    try { Save(data / L"info" / Wide(game.discordApplicationId + ".process.json"),
                         Json{{"applicationId", game.discordApplicationId}, {"stage", "running"},
                              {"mode", rpc ? "RPC + process" : "Standard"},
                              {"processId", GetProcessId(process.Get())}, {"executable", Utf8(executable.wstring())},
                              {"placement", placement}, {"steamManifestCreated", steamPlacement != nullptr},
                              {"startedAt", UnixNow()}, {"questProgressAvailable", false}}.dump(2)); } catch (...) { }
                }
            } catch (const std::exception &failure) {
                SetRuntime(shared, RuntimeStatus::LaunchFailed, ERROR_BAD_ENVIRONMENT);
                try {
                    Save(data / L"info" / Wide(game.discordApplicationId + ".launch.json"),
                         Json{{"message", failure.what()}, {"recordedAt", UnixNow()}}.dump(2));
                } catch (...) { }
            }
        });
    }
    ~ParentRun() {
        Stop();
        rpcRuntime.reset();
        if (launch.joinable())
            launch.join();
        if (process.Get())
            WaitForSingleObject(process.Get(), 1200);
        job.Reset();
        if (process.Get())
            WaitForSingleObject(process.Get(), 1200);
        steamPlacement.reset();
        std::error_code ignored;
        if (!config.empty())
            fs::remove(config, ignored);
        if (!runnerExecutable.empty()) {
            fs::remove(runnerExecutable, ignored);
            auto folder = runnerExecutable.parent_path();
            for (int depth = 0; depth < 13 && !folder.empty(); ++depth) {
                fs::remove(folder, ignored);
                if (folder == runnerRoot)
                    break;
                folder = folder.parent_path();
            }
        }
        if (shared)
            UnmapViewOfFile(shared);
    }
    void Stop() {
        if (stopped.Get())
            SetEvent(stopped.Get());
    }
    RuntimeSnapshot Snapshot() const {
        if (!launched)
            return {RuntimeStatus::Starting, 0, 0};
        auto state = ReadRuntime(shared);
        if (launched && process.Get() && WaitForSingleObject(process.Get(), 0) == WAIT_OBJECT_0 &&
            state.status != RuntimeStatus::Complete && state.status != RuntimeStatus::Error &&
            state.status != RuntimeStatus::LaunchFailed)
            state.status = RuntimeStatus::Stopped;
        return state;
    }
    void Show() const {
        const auto window =
            reinterpret_cast<HWND>(static_cast<INT_PTR>(InterlockedCompareExchange64(&shared->window, 0, 0)));
        DWORD pid = 0;
        if (launched && process.Get() && IsWindow(window) && GetWindowThreadProcessId(window, &pid) &&
            pid == GetProcessId(process.Get())) {
            ShowWindow(window, SW_RESTORE);
            SetForegroundWindow(window);
        }
    }
    HWND Window() const {
        return reinterpret_cast<HWND>(static_cast<INT_PTR>(InterlockedCompareExchange64(&shared->window, 0, 0)));
    }
};

class RunReaper {
    std::thread thread;
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::unique_ptr<ParentRun>> retired;
    bool exiting = false;

  public:
    RunReaper() {
        thread = std::thread([this] {
            for (;;) {
                std::unique_ptr<ParentRun> run;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    changed.wait(lock, [&] { return exiting || !retired.empty(); });
                    if (retired.empty())
                        return;
                    run = std::move(retired.front());
                    retired.pop_front();
                }
                run.reset();
            }
        });
    }
    void Retire(std::unique_ptr<ParentRun> run) {
        run->Stop();
        {
            std::lock_guard<std::mutex> lock(mutex);
            retired.push_back(std::move(run));
        }
        changed.notify_one();
    }
    ~RunReaper() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            exiting = true;
        }
        changed.notify_one();
        if (thread.joinable())
            thread.join();
    }
};
struct RpcFrame {
    uint32_t opcode = 0;
    std::string payload;
};
inline std::vector<unsigned char> EncodeRpcFrame(uint32_t opcode, const std::string &payload) {
    if (payload.size() > 65536)
        throw std::runtime_error("RPC frame too large.");
    std::vector<unsigned char> bytes(8 + payload.size());
    const uint32_t size = static_cast<uint32_t>(payload.size());
    for (int i = 0; i < 4; ++i) {
        bytes[i] = static_cast<unsigned char>(opcode >> (i * 8));
        bytes[4 + i] = static_cast<unsigned char>(size >> (i * 8));
    }
    std::copy(payload.begin(), payload.end(), bytes.begin() + 8);
    return bytes;
}
inline std::optional<RpcFrame> DecodeRpcFrame(std::vector<unsigned char> &bytes) {
    if (bytes.size() < 8)
        return {};
    uint32_t opcode = 0, size = 0;
    for (int i = 0; i < 4; ++i) {
        opcode |= uint32_t(bytes[i]) << (i * 8);
        size |= uint32_t(bytes[4 + i]) << (i * 8);
    }
    if (size > 65536 || opcode > 4)
        throw std::runtime_error("Invalid RPC frame.");
    if (bytes.size() < 8 + size)
        return {};
    RpcFrame frame{opcode, std::string(bytes.begin() + 8, bytes.begin() + 8 + size)};
    bytes.erase(bytes.begin(), bytes.begin() + 8 + size);
    return frame;
}
inline Json ActivityCommand(DWORD pid, int64_t start, const std::string &nonce, bool clear = false) {
    return {{"cmd", "SET_ACTIVITY"},
            {"nonce", nonce},
            {"args",
             {{"pid", pid},
              {"activity", clear ? Json(nullptr)
                                 : Json{{"type", 0}, {"timestamps", {{"start", start}}}, {"instance", false}}}}}};
}
class DiscordIpc {
    NativeHandle pipe;
    HANDLE stop;
    std::vector<unsigned char> incoming;
    bool Transfer(bool write, void *bytes, DWORD size, DWORD timeout, bool cleanup = false) {
        NativeHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!event.Get())
            return false;
        OVERLAPPED io{};
        io.hEvent = event.Get();
        DWORD count = 0;
        BOOL ok = write ? WriteFile(pipe.Get(), bytes, size, &count, &io)
                        : ReadFile(pipe.Get(), bytes, size, &count, &io);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            HANDLE wait[]{stop, event.Get()};
            const bool completed = cleanup ? WaitForSingleObject(event.Get(), timeout) == WAIT_OBJECT_0
                                           : WaitForMultipleObjects(2, wait, FALSE, timeout) == WAIT_OBJECT_0 + 1;
            if (!completed) {
                CancelIoEx(pipe.Get(), &io);
                WaitForSingleObject(event.Get(), INFINITE);
                GetOverlappedResult(pipe.Get(), &io, &count, FALSE);
                return false;
            }
            ok = GetOverlappedResult(pipe.Get(), &io, &count, FALSE);
        }
        return ok && count == size;
    }

  public:
    explicit DiscordIpc(HANDLE stopEvent) : stop(stopEvent) {}
    bool Open(const std::wstring &testPipe = {}) {
        incoming.clear();
        for (int index = 0; index < (testPipe.empty() ? 10 : 1); ++index) {
            auto name = testPipe.empty() ? L"\\\\?\\pipe\\discord-ipc-" + std::to_wstring(index) : testPipe;
            pipe.Reset(CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                   FILE_FLAG_OVERLAPPED, nullptr));
            if (pipe.Get())
                return true;
        }
        return false;
    }
    bool Send(uint32_t opcode, const std::string &payload, bool cleanup = false) {
        auto bytes = EncodeRpcFrame(opcode, payload);
        return Transfer(true, bytes.data(), static_cast<DWORD>(bytes.size()), 800, cleanup);
    }
    std::optional<RpcFrame> Read(DWORD timeout = 1000) {
        const auto deadline = GetTickCount64() + timeout;
        while (GetTickCount64() < deadline) {
            if (auto frame = DecodeRpcFrame(incoming))
                return frame;
            DWORD available = 0;
            if (!PeekNamedPipe(pipe.Get(), nullptr, 0, nullptr, &available, nullptr))
                throw std::runtime_error("Discord disconnected.");
            if (available) {
                const DWORD count = std::min<DWORD>(available, 4096);
                const size_t offset = incoming.size();
                incoming.resize(offset + count);
                if (!Transfer(false, incoming.data() + offset, count, 800))
                    throw std::runtime_error("Discord read failed.");
            } else if (WaitForSingleObject(stop, 15) == WAIT_OBJECT_0)
                return {};
        }
        return DecodeRpcFrame(incoming);
    }
};
inline Json ProbeDiscordIpc(const std::string &applicationId) {
    if (!ApplicationIdValid(applicationId))
        throw std::runtime_error("A valid Discord application ID is required.");
    NativeHandle stop(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    DiscordIpc ipc(stop.Get());
    Json result{
        {"applicationId", applicationId}, {"connected", false}, {"ready", false}, {"activityPublished", false}};
    if (!ipc.Open()) {
        result["status"] = "Discord desktop IPC is unavailable";
        return result;
    }
    result["connected"] = true;
    if (!ipc.Send(0, Json{{"v", 1}, {"client_id", applicationId}}.dump())) {
        result["status"] = "Handshake write failed";
        return result;
    }
    const auto deadline = GetTickCount64() + 4000;
    try {
        while (GetTickCount64() < deadline) {
            const auto frame = ipc.Read(200);
            if (!frame)
                continue;
            if (frame->opcode == 3) {
                ipc.Send(4, frame->payload);
                continue;
            }
            if (frame->opcode != 1 && frame->opcode != 2)
                continue;
            const auto reply = Json::parse(frame->payload);
            if (frame->opcode == 2 || CatalogText(reply, "evt") == "ERROR") {
                result["status"] = "Handshake rejected";
                result["errorCode"] = frame->opcode == 2 ? reply.value("code", 0)
                                                         : reply.value("data", Json::object()).value("code", 0);
                return result;
            }
            if (CatalogText(reply, "cmd") == "DISPATCH" && CatalogText(reply, "evt") == "READY") {
                result["ready"] = true;
                result["status"] = "Handshake accepted";
                return result;
            }
        }
        result["status"] = "Handshake timed out";
    } catch (...) {
        result["status"] = "Discord disconnected";
    }
    return result;
}
class RunnerRuntime {
    NativeHandle mapping, stopped;
    RuntimeShared *shared = nullptr;
    std::thread thread;
    bool rpc;
    std::string appId;
    std::wstring testPipe;
    fs::path diagnostic;
    bool readyReceived = false, acknowledged = false, activityCleared = false;
    std::string lastMessage;
    std::string acknowledgedName, acknowledgedApplication;
    unsigned attempts = 0;
    void Trace(const std::string &stage, const std::string &message = {}) {
        if (!rpc || diagnostic.empty())
            return;
        if (!message.empty())
            lastMessage = message;
        try {
            const auto state = ReadRuntime(shared);
            Save(diagnostic, Json{{"applicationId", appId},
                                  {"processId", GetCurrentProcessId()},
                                  {"stage", stage},
                                  {"message", lastMessage},
                                  {"attempts", attempts},
                                  {"ready", readyReceived},
                                  {"activityAcknowledged", acknowledged},
                                  {"activityCleared", activityCleared},
                                  {"activityName", acknowledgedName},
                                  {"activityApplicationId", acknowledgedApplication},
                                  {"status", Utf8(RuntimeLabel(state.status))},
                                  {"errorCode", state.error},
                                  {"localElapsed", state.elapsed},
                                  {"recordedAt", UnixNow()},
                                  {"questProgressAvailable", false}}
                                 .dump(2));
        } catch (...) {
        }
    }
    void Run() {
        double elapsed = 0;
        const auto epoch = static_cast<int64_t>(UnixNow());
        uint64_t sequence = 0;
        while (WaitForSingleObject(stopped.Get(), 0) != WAIT_OBJECT_0) {
            bool active = !rpc;
            bool activitySubmitted = false;
            std::unique_ptr<DiscordIpc> ipc;
            if (rpc) {
                try {
                    ++attempts;
                    lastMessage.clear();
                    readyReceived = acknowledged = activityCleared = false;
                    SetRuntime(shared, RuntimeStatus::Connecting);
                    Trace("connecting");
                    ipc = std::make_unique<DiscordIpc>(stopped.Get());
                    if (!ipc->Open(testPipe))
                        throw std::runtime_error("Discord IPC unavailable (Windows error " +
                                                 std::to_string(GetLastError()) + ").");
                    if (!ipc->Send(0, Json{{"v", 1}, {"client_id", appId}}.dump()))
                        throw std::runtime_error("Handshake failed.");
                    Trace("handshake-sent");
                    bool ready = false;
                    auto nextMessage = [&](DWORD timeout) -> Json {
                        auto frame = ipc->Read(timeout);
                        if (!frame)
                            return {};
                        if (frame->opcode == 3) {
                            if (!ipc->Send(4, frame->payload))
                                throw std::runtime_error("Pong failed.");
                            return {};
                        }
                        if (frame->opcode == 2) {
                            auto j = Json::parse(frame->payload);
                            const auto code = j.value("code", 0);
                            SetRuntime(shared, code >= 4000 ? RuntimeStatus::Error : RuntimeStatus::Disconnected,
                                       code);
                            const auto message = CatalogText(j, "message");
                            throw std::runtime_error(message.empty() ? "Discord closed the connection." : message);
                        }
                        return frame->opcode == 1 ? Json::parse(frame->payload) : Json{};
                    };
                    const auto deadline = GetTickCount64() + 4000;
                    while (GetTickCount64() < deadline && WaitForSingleObject(stopped.Get(), 0) != WAIT_OBJECT_0) {
                        const auto response = nextMessage(200);
                        if (response.is_object() && CatalogText(response, "evt") == "ERROR") {
                            const auto failure = response.value("data", Json::object());
                            SetRuntime(shared, RuntimeStatus::Error, failure.value("code", 0));
                            const auto message = CatalogText(failure, "message");
                            throw std::runtime_error(message.empty() ? "Discord rejected the RPC handshake."
                                                                     : message);
                        }
                        if (response.is_object() && CatalogText(response, "cmd") == "DISPATCH" &&
                            CatalogText(response, "evt") == "READY") {
                            ready = true;
                            readyReceived = true;
                            Trace("ready");
                            break;
                        }
                        if (ReadRuntime(shared).status == RuntimeStatus::Error)
                            break;
                    }
                    if (!ready)
                        throw std::runtime_error("No READY response.");
                    const auto nonce = std::to_string(GetCurrentProcessId()) + ":" + std::to_string(++sequence);
                    if (!ipc->Send(1, ActivityCommand(GetCurrentProcessId(), epoch, nonce).dump()))
                        throw std::runtime_error("Activity send failed.");
                    activitySubmitted = true;
                    Trace("activity-sent");
                    const auto ackDeadline = GetTickCount64() + 4000;
                    while (GetTickCount64() < ackDeadline &&
                           WaitForSingleObject(stopped.Get(), 0) != WAIT_OBJECT_0) {
                        const auto response = nextMessage(200);
                        if (!response.is_object() || CatalogText(response, "nonce") != nonce)
                            continue;
                        if (CatalogText(response, "evt") == "ERROR") {
                            const auto failure = response.value("data", Json::object());
                            SetRuntime(shared, RuntimeStatus::Error, failure.value("code", 0));
                            const auto message = CatalogText(failure, "message");
                            throw std::runtime_error(message.empty() ? "Discord rejected the activity." : message);
                        }
                        if (CatalogText(response, "cmd") == "SET_ACTIVITY") {
                            if (response.contains("data") && response["data"].is_null())
                                throw std::runtime_error("Discord returned an empty activity, rather than publishing Playing.");
                            const auto activity = response.value("data", Json::object());
                            acknowledgedName = CatalogText(activity, "name");
                            acknowledgedApplication = CatalogText(activity, "application_id");
                            active = true;
                            acknowledged = true;
                            break;
                        }
                    }
                    if (!active)
                        throw std::runtime_error("Activity was not accepted.");
                } catch (const std::exception &error) {
                    const auto state = ReadRuntime(shared);
                    if (state.status != RuntimeStatus::Error)
                        SetRuntime(shared, RuntimeStatus::Disconnected, state.error);
                    Trace("connection-failed", error.what());
                }
            }
            if (active) {
                SetRuntime(shared, rpc ? RuntimeStatus::Active : RuntimeStatus::ProcessRunning);
                Trace("activity-acknowledged");
                double last = SteadyNow();
                double lastTrace = last;
                while (WaitForSingleObject(stopped.Get(), 100) == WAIT_TIMEOUT) {
                    if (rpc) {
                        try {
                            if (auto frame = ipc->Read(1)) {
                                if (frame->opcode == 3 && !ipc->Send(4, frame->payload))
                                    throw std::runtime_error("Pong failed.");
                                if (frame->opcode == 2) {
                                    const auto close = Json::parse(frame->payload);
                                    const auto code = close.value("code", 0);
                                    SetRuntime(shared, code >= 4000 ? RuntimeStatus::Error : RuntimeStatus::Disconnected,
                                               code);
                                    Trace("connection-closed", CatalogText(close, "message"));
                                    break;
                                }
                                if (frame->opcode == 1) {
                                    const auto response = Json::parse(frame->payload);
                                    if (CatalogText(response, "cmd") == "SET_ACTIVITY" &&
                                        CatalogText(response, "evt") == "ERROR") {
                                        const auto failure = response.value("data", Json::object());
                                        SetRuntime(shared, RuntimeStatus::Error, failure.value("code", 0));
                                        Trace("activity-rejected", CatalogText(failure, "message"));
                                        break;
                                    }
                                }
                            }
                        } catch (const std::exception &error) {
                            SetRuntime(shared, RuntimeStatus::Disconnected);
                            Trace("transport-failed", error.what());
                            break;
                        }
                    }
                    const auto now = SteadyNow();
                    elapsed = std::min(SessionDuration, elapsed + now - last);
                    last = now;
                    InterlockedExchange64(&shared->elapsedMs, static_cast<LONG64>(elapsed * 1000));
                    if (now - lastTrace >= 5) {
                        Trace("activity-active");
                        lastTrace = now;
                    }
                    if (elapsed >= SessionDuration)
                        break;
                }
                if (rpc && ipc) {
                    activityCleared = ipc->Send(1, ActivityCommand(GetCurrentProcessId(), 0, "clear", true).dump(), true);
                    activitySubmitted = false;
                }
                if (elapsed >= SessionDuration) {
                    SetRuntime(shared, RuntimeStatus::Complete);
                    Trace("local-session-complete");
                    SetEvent(stopped.Get());
                    return;
                }
                if (WaitForSingleObject(stopped.Get(), 0) != WAIT_OBJECT_0 &&
                    ReadRuntime(shared).status != RuntimeStatus::Error)
                    SetRuntime(shared, RuntimeStatus::Disconnected, ReadRuntime(shared).error);
                Trace("disconnected", "Activity ended; reconnecting if the transport is available.");
            }
            if (rpc && ipc && activitySubmitted)
                activityCleared = ipc->Send(1, ActivityCommand(GetCurrentProcessId(), 0, "clear", true).dump(), true);
            // Invalid applications and rejected activities require a user restart, not an endless retry loop.
            if (ReadRuntime(shared).status == RuntimeStatus::Error) {
                Trace("activity-rejected", "Discord rejected the request; stop and restart after correcting it.");
                WaitForSingleObject(stopped.Get(), INFINITE);
                break;
            }
            if (WaitForSingleObject(stopped.Get(), 2500) == WAIT_OBJECT_0)
                break;
        }
        SetRuntime(shared, RuntimeStatus::Stopped);
        Trace("stopped");
    }

  public:
    RunnerRuntime(const std::wstring &key, bool useRpc, const std::string &applicationId,
                  const std::wstring &mockPipe = {}, const fs::path &diagnosticFile = {})
        : rpc(useRpc), appId(applicationId), testPipe(mockPipe), diagnostic(diagnosticFile) {
        mapping.Reset(OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, MappingName(key).c_str()));
        stopped.Reset(OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, StopName(key).c_str()));
        if (!mapping.Get() || !stopped.Get())
            throw std::runtime_error("Parent session is unavailable.");
        shared = static_cast<RuntimeShared *>(
            MapViewOfFile(mapping.Get(), FILE_MAP_ALL_ACCESS, 0, 0, sizeof(RuntimeShared)));
        if (!shared)
            throw std::runtime_error("Session state is unavailable.");
        thread = std::thread([this] { Run(); });
    }
    ~RunnerRuntime() {
        Stop();
        if (thread.joinable())
            thread.join();
        if (shared)
            UnmapViewOfFile(shared);
    }
    void Stop() {
        SetEvent(stopped.Get());
    }
    RuntimeSnapshot Snapshot() const {
        return ReadRuntime(shared);
    }
    bool Closing() const {
        return WaitForSingleObject(stopped.Get(), 0) == WAIT_OBJECT_0;
    }
    void Window(HWND hwnd) {
        InterlockedExchange64(&shared->window, static_cast<LONG64>(reinterpret_cast<INT_PTR>(hwnd)));
    }
};
inline std::shared_ptr<RunnerRuntime> CreateRpcRuntime(const std::wstring &key, const std::string &appId,
                                                       const std::wstring &testPipe, const fs::path &diagnostic) {
    return std::make_shared<RunnerRuntime>(key, true, appId, testPipe, diagnostic);
}
