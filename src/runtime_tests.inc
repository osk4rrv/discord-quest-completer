// Integration tests use private QA pipes and a QA helper; they never connect to Discord.
int RuntimeTest(const services::fs::path &data) {
    using namespace services;
    auto require = [](bool ok, const char *why) {
        if (!ok)
            throw std::runtime_error(why);
    };
    const auto key = RuntimeKey();
    NativeHandle mapping(CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                            sizeof(RuntimeShared), MappingName(key).c_str()));
    NativeHandle stop(CreateEventW(nullptr, TRUE, FALSE, StopName(key).c_str()));
    auto *state = static_cast<RuntimeShared *>(
        MapViewOfFile(mapping.Get(), FILE_MAP_ALL_ACCESS, 0, 0, sizeof(RuntimeShared)));
    require(state != nullptr && stop.Get(), "QA mapping failed.");
    new (state) RuntimeShared{};
    struct Unmap {
        void *p;
        ~Unmap() {
            UnmapViewOfFile(p);
        }
    } unmap{state};
    const auto pipeName = L"\\\\.\\pipe\\QuestCompleter.qa." + key;
    NativeHandle pipe(CreateNamedPipeW(pipeName.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                       PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 8192, 8192, 0,
                                       nullptr));
    require(pipe.Get() != nullptr, "QA pipe failed.");
    NativeHandle connected(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    OVERLAPPED connection{};
    connection.hEvent = connected.Get();
    const bool connecting = ConnectNamedPipe(pipe.Get(), &connection) || GetLastError() == ERROR_IO_PENDING ||
                            GetLastError() == ERROR_PIPE_CONNECTED;
    require(connecting, "QA pipe connect failed.");
    auto io = [&](bool write, void *bytes, DWORD size) {
        NativeHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        OVERLAPPED transfer{};
        transfer.hEvent = event.Get();
        DWORD count = 0;
        BOOL ok = write ? WriteFile(pipe.Get(), bytes, size, &count, &transfer)
                        : ReadFile(pipe.Get(), bytes, size, &count, &transfer);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            if (WaitForSingleObject(event.Get(), 3000) != WAIT_OBJECT_0) {
                CancelIoEx(pipe.Get(), &transfer);
                WaitForSingleObject(event.Get(), 1000);
                return false;
            }
            ok = GetOverlappedResult(pipe.Get(), &transfer, &count, FALSE);
        }
        return ok && count == size;
    };
    auto read = [&] {
        unsigned char header[8]{};
        require(io(false, header, 8), "Missing RPC request header.");
        uint32_t size = 0;
        for (int i = 0; i < 4; ++i)
            size |= uint32_t(header[4 + i]) << (i * 8);
        require(size < 65536, "RPC payload bound failed.");
        std::string bytes(size, 0);
        require(io(false, bytes.data(), size), "Missing RPC request payload.");
        return Json::parse(bytes);
    };
    auto send = [&](uint32_t opcode, const std::string &message, bool fragmented = false) {
        auto bytes = EncodeRpcFrame(opcode, message);
        if (fragmented) {
            require(io(true, bytes.data(), 5), "QA fragment failed.");
            require(io(true, bytes.data() + 5, 3), "QA fragment failed.");
            require(io(true, bytes.data() + 8, static_cast<DWORD>(bytes.size() - 8)), "QA fragment failed.");
        } else
            require(io(true, bytes.data(), static_cast<DWORD>(bytes.size())), "QA response failed.");
    };
    auto awaitStatus = [&](RuntimeStatus expected, DWORD limit) {
        const auto until = GetTickCount64() + limit;
        while (GetTickCount64() < until) {
            if (ReadRuntime(state).status == expected)
                return true;
            WaitForSingleObject(stop.Get(), 10);
        }
        return false;
    };
    {
        RunnerRuntime runtime(key, true, "123456789012345678", pipeName, data / L"info" / L"qa-rpc.json");
        if (WaitForSingleObject(connected.Get(), 3000) != WAIT_OBJECT_0)
            throw std::runtime_error("RPC never connected: " + Read(data / L"info" / L"qa-rpc.json"));
        const auto handshake = read();
        require(handshake.at("v") == 1 && handshake.at("client_id") == "123456789012345678",
                "Handshake is invalid.");
        send(3, "ping");
        unsigned char pong[12]{};
        require(io(false, pong, 12) && pong[0] == 4 && std::string(pong + 8, pong + 12) == "ping",
                "PING/PONG failed.");
        send(1, R"({"cmd":"UNRELATED","evt":"READY","data":{}})");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        DWORD available = 0;
        require(PeekNamedPipe(pipe.Get(), nullptr, 0, nullptr, &available, nullptr) && available == 0,
                "Unrelated READY started an activity.");
        send(1, R"({"cmd":"DISPATCH","evt":"READY","data":{}})", true);
        const auto activity = read();
        require(activity.at("cmd") == "SET_ACTIVITY" && activity.at("args").at("pid") == GetCurrentProcessId(),
                "Activity payload is invalid.");
        require(ReadRuntime(state).elapsed == 0, "Timer started before ACK.");
        send(1,
             Json{{"cmd", "SET_ACTIVITY"},
                  {"nonce", activity.at("nonce")},
                  {"evt", nullptr},
                  {"data", Json::object()}}
                 .dump(),
             true);
        require(awaitStatus(RuntimeStatus::Active, 1500), "Activity ACK was not accepted.");
        const auto until = GetTickCount64() + 1500;
        while (ReadRuntime(state).elapsed < .25 && GetTickCount64() < until)
            WaitForSingleObject(stop.Get(), 20);
        require(ReadRuntime(state).elapsed >= .25, "Accepted RPC time did not advance.");
        send(2, R"({"code":1000,"message":"QA disconnect"})");
        require(awaitStatus(RuntimeStatus::Disconnected, 1500), "Disconnect did not pause activity.");
        const auto paused = ReadRuntime(state).elapsed;
        WaitForSingleObject(stop.Get(), 150);
        require(ReadRuntime(state).elapsed == paused, "Disconnected timer advanced.");
        const auto cleared = read();
        require(cleared.at("args").at("activity").is_null(), "Disconnected activity was not cleared.");
        DisconnectNamedPipe(pipe.Get());
        ResetEvent(connected.Get());
        OVERLAPPED reconnect{};
        reconnect.hEvent = connected.Get();
        ConnectNamedPipe(pipe.Get(), &reconnect);
        require(WaitForSingleObject(connected.Get(), 4000) == WAIT_OBJECT_0, "RPC did not reconnect.");
        require(read().at("client_id") == "123456789012345678", "Reconnect handshake changed the application.");
        send(1, R"({"cmd":"DISPATCH","evt":"READY","data":{}})");
        const auto restored = read();
        require(restored.at("args").at("activity").at("timestamps") ==
                    activity.at("args").at("activity").at("timestamps") &&
                    restored.at("nonce") != activity.at("nonce"),
                "Reconnect reset the session timestamp or reused its nonce.");
        require(ReadRuntime(state).elapsed == paused, "Reconnect counted time before activity ACK.");
        send(1, Json{{"cmd", "SET_ACTIVITY"}, {"nonce", restored.at("nonce")}, {"evt", nullptr}}.dump());
        require(awaitStatus(RuntimeStatus::Active, 1500), "Reconnect ACK did not resume activity.");
        send(1, R"({"cmd":"UNRELATED","evt":"ERROR","data":{"code":4006}})");
        WaitForSingleObject(stop.Get(), 150);
        require(ReadRuntime(state).status == RuntimeStatus::Active && ReadRuntime(state).elapsed > paused,
                "Unrelated RPC error interrupted active time.");
        runtime.Stop();
        require(read().at("args").at("activity").is_null(), "Stop did not send activity cleanup.");
    }
    require(ReadRuntime(state).status == RuntimeStatus::Stopped, "RPC did not stop cleanly.");
    DisconnectNamedPipe(pipe.Get());
    ResetEvent(stop.Get());
    ResetEvent(connected.Get());
    InterlockedExchange64(&state->elapsedMs, 0);
    SetRuntime(state, RuntimeStatus::Starting);
    OVERLAPPED rejectedConnection{};
    rejectedConnection.hEvent = connected.Get();
    ConnectNamedPipe(pipe.Get(), &rejectedConnection);
    {
        RunnerRuntime runtime(key, true, "123456789012345678", pipeName);
        require(WaitForSingleObject(connected.Get(), 3000) == WAIT_OBJECT_0,
                "RPC handshake error test did not connect.");
        read();
        send(1, R"({"cmd":"DISPATCH","evt":"ERROR","data":{"code":4000,"message":"QA rejection"}})");
        require(awaitStatus(RuntimeStatus::Error, 1500) && ReadRuntime(state).error == 4000,
                "Handshake rejection was not preserved.");
        require(ReadRuntime(state).elapsed == 0, "Rejected handshake advanced the timer.");
        runtime.Stop();
    }
    DisconnectNamedPipe(pipe.Get());
    ResetEvent(stop.Get());
    ResetEvent(connected.Get());
    SetRuntime(state, RuntimeStatus::Starting);
    OVERLAPPED activityErrorConnection{};
    activityErrorConnection.hEvent = connected.Get();
    ConnectNamedPipe(pipe.Get(), &activityErrorConnection);
    {
        RunnerRuntime runtime(key, true, "123456789012345678", pipeName);
        require(WaitForSingleObject(connected.Get(), 3000) == WAIT_OBJECT_0, "RPC error test did not connect.");
        read();
        send(1, R"({"cmd":"DISPATCH","evt":"READY","data":{}})");
        const auto activity = read();
        send(1, Json{{"cmd", "SET_ACTIVITY"},
                     {"nonce", activity.at("nonce")},
                     {"evt", "ERROR"},
                     {"data", {{"code", 4006}}}}
                    .dump());
        require(awaitStatus(RuntimeStatus::Error, 1500) && ReadRuntime(state).error == 4006,
                "Discord error was not preserved.");
        require(ReadRuntime(state).elapsed == 0, "Rejected RPC advanced the timer.");
        runtime.Stop();
    }
    DisconnectNamedPipe(pipe.Get());
    ResetEvent(connected.Get());
    OVERLAPPED inProcessConnection{};
    inProcessConnection.hEvent = connected.Get();
    ConnectNamedPipe(pipe.Get(), &inProcessConnection);
    // Exercise the production RPC entry point, including its absence of a child process/window/files.
    auto files = [](const fs::path &folder) {
        size_t count = 0;
        if (fs::exists(folder))
            for (const auto &entry : fs::recursive_directory_iterator(folder))
                if (entry.is_regular_file())
                    ++count;
        return count;
    };
    const auto configsBefore = files(data / L"sessions"), runnersBefore = files(data / L"runners");
    {
        Game rpcGame{99999998, L"QA RPC", {}, {}};
        rpcGame.discordApplicationId = "123456789012345678";
        ParentRun run(rpcGame, true, Preferences{}, data, false, pipeName);
        require(WaitForSingleObject(connected.Get(), 3000) == WAIT_OBJECT_0, "In-process RPC did not connect.");
        read();
        send(1, R"({"cmd":"DISPATCH","evt":"READY","data":{}})");
        const auto activity = read();
        require(activity.at("args").at("pid") == GetCurrentProcessId(), "RPC launched a helper process.");
        send(1, Json{{"cmd", "SET_ACTIVITY"}, {"nonce", "wrong-nonce"}, {"evt", nullptr}}.dump());
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        require(run.Snapshot().elapsed == 0, "Unrelated RPC ACK advanced local time.");
        send(1, Json{{"cmd", "SET_ACTIVITY"}, {"nonce", activity.at("nonce")}, {"evt", nullptr}}.dump());
        const auto deadline = GetTickCount64() + 2000;
        while (run.Snapshot().elapsed < .25 && GetTickCount64() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        require(run.Snapshot().status == RuntimeStatus::Active && run.Snapshot().elapsed >= .25,
                "In-process RPC time did not advance.");
        require(!run.Window() && files(data / L"sessions") == configsBefore &&
                    files(data / L"runners") == runnersBefore,
                "RPC created a Standard helper/window/config.");
        send(1, R"({"cmd":"SET_ACTIVITY","evt":"ERROR","data":{"code":4002,"message":"QA revoked"}})");
        const auto rejectionDeadline = GetTickCount64() + 1500;
        while (run.Snapshot().status != RuntimeStatus::Error && GetTickCount64() < rejectionDeadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        require(run.Snapshot().status == RuntimeStatus::Error && run.Snapshot().error == 4002,
                "Active RPC rejection was not preserved.");
        const auto rejectedElapsed = run.Snapshot().elapsed;
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        require(run.Snapshot().elapsed == rejectedElapsed, "Rejected active RPC kept counting time.");
        run.Stop();
        const auto closing = GetTickCount64() + 2000;
        while (run.Snapshot().status != RuntimeStatus::Stopped && GetTickCount64() < closing)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        require(run.Snapshot().status == RuntimeStatus::Stopped, "In-process RPC did not stop.");
    }
    DisconnectNamedPipe(pipe.Get());
    // Exercise the full copy / launch / render / stop path with a name Discord does not recognize.
    Game game{99999999, L"QA session", {}, {}};
    game.windowsExecutables = {L"qa/QuestCompleterQaHelper.exe"};
    Preferences preferences;
    {
        ParentRun run(game, false, preferences, data, true);
        const auto deadline = GetTickCount64() + 5000;
        while ((run.Snapshot().status == RuntimeStatus::Starting || !run.Window()) && GetTickCount64() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        require(run.Snapshot().status == RuntimeStatus::ProcessRunning, "Native helper did not start.");
        require(IsWindow(run.Window()) &&
                    (GetWindowLongPtrW(run.Window(), GWL_STYLE) & (WS_CAPTION | WS_MAXIMIZEBOX)) == 0,
                "Runner window has native chrome.");
        MINMAXINFO sizes{};
        SendMessageTimeoutW(run.Window(), WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM>(&sizes), SMTO_ABORTIFHUNG,
                            500, nullptr);
        const auto dpi = GetDpiForWindow(run.Window());
        require(sizes.ptMinTrackSize.x == static_cast<LONG>(440 * dpi / 96) &&
                    sizes.ptMinTrackSize.y == static_cast<LONG>(354 * dpi / 96),
                "Runner DPI layout is invalid.");
        run.Stop();
        const auto closing = GetTickCount64() + 3000;
        while (run.Snapshot().status != RuntimeStatus::Stopped && GetTickCount64() < closing)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        require(run.Snapshot().status == RuntimeStatus::Stopped, "Native helper did not stop.");
    }
    // RPC with a usable game profile must publish from the helper PID, not the launcher PID.
    ResetEvent(connected.Get());
    OVERLAPPED helperConnection{};
    helperConnection.hEvent = connected.Get();
    ConnectNamedPipe(pipe.Get(), &helperConnection);
    game.discordApplicationId = "123456789012345678";
    {
        ParentRun run(game, true, preferences, data, true, pipeName);
        require(WaitForSingleObject(connected.Get(), 4000) == WAIT_OBJECT_0, "RPC helper did not connect.");
        read();
        send(1, R"({"cmd":"DISPATCH","evt":"READY","data":{}})");
        const auto activity = read();
        require(activity.at("args").at("pid") != GetCurrentProcessId(), "RPC omitted game detection helper.");
        send(1, Json{{"cmd", "SET_ACTIVITY"}, {"nonce", activity.at("nonce")}, {"evt", nullptr},
                     {"data", {{"name", "QA presence"}, {"application_id", game.discordApplicationId}}}}.dump());
        const auto deadline = GetTickCount64() + 2500;
        while (run.Snapshot().elapsed < .15 && GetTickCount64() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        require(run.Snapshot().status == RuntimeStatus::Active && run.Snapshot().elapsed >= .15,
                "Helper RPC did not become active.");
        run.Stop();
        require(read().at("args").at("activity").is_null(), "Helper RPC did not clear activity.");
    }
    DisconnectNamedPipe(pipe.Get());
    // Steam placement is tested against a private fake library, never the user's Steam installation.
    const auto qaSteam = data / L"steam-placement-tests" / RuntimeKey();
    fs::create_directories(qaSteam / L"steamapps" / L"common");
    Game steamGame{4080220, L"EA SPORTS FC 27", {}, {}};
    steamGame.discordApplicationId = "1531874756096295054";
    steamGame.steamApplicationId = "4080220";
    steamGame.steamInstallDirectory = L"EA SPORTS FC 27";
    steamGame.windowsExecutables = {L"fc27.exe"};
    const auto manifest = qaSteam / L"steamapps" / L"appmanifest_4080220.acf";
    const auto gameFolder = qaSteam / L"steamapps" / L"common" / L"EA SPORTS FC 27";
    {
        ParentRun run(steamGame, false, preferences, data, true, {}, qaSteam);
        const auto deadline = GetTickCount64() + 5000;
        while (run.Snapshot().status == RuntimeStatus::Starting && GetTickCount64() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        require(run.Snapshot().status == RuntimeStatus::ProcessRunning && fs::exists(gameFolder / L"fc27.exe") &&
                    Read(manifest).find("4080220") != std::string::npos, "Steam detection placement failed.");
        run.Stop();
    }
    require(!fs::exists(manifest) && !fs::exists(gameFolder), "Steam placement was not cleaned up.");
    Save(manifest, "Existing Steam manifest must survive.");
    {
        ParentRun run(steamGame, false, preferences, data, true, {}, qaSteam);
        const auto deadline = GetTickCount64() + 2000;
        while (run.Snapshot().status == RuntimeStatus::Starting && GetTickCount64() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        require(run.Snapshot().status == RuntimeStatus::LaunchFailed &&
                    Read(manifest) == "Existing Steam manifest must survive." && !fs::exists(gameFolder),
                "Existing Steam installation was modified.");
    }
    fs::remove(manifest);
    fs::create_directory(gameFolder);
    Save(gameFolder / L"fc27.exe", "Existing installed game must survive.");
    {
        ParentRun run(steamGame, false, preferences, data, true, {}, qaSteam);
        const auto deadline = GetTickCount64() + 2000;
        while (run.Snapshot().status == RuntimeStatus::Starting && GetTickCount64() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        require(run.Snapshot().status == RuntimeStatus::LaunchFailed && !fs::exists(manifest) &&
                    Read(gameFolder / L"fc27.exe") == "Existing installed game must survive.",
                "Existing game files were replaced.");
    }
    fs::remove(gameFolder / L"fc27.exe");
    fs::remove(gameFolder);
    // An abandoned, owned lease can be recovered; foreign files and live owners are preserved.
    const auto abandonedKey = RuntimeKey();
    const auto abandonedJournal = data / L"placements" / (abandonedKey + L".json");
    fs::create_directory(gameFolder);
    Save(gameFolder / L"fc27.exe", "QA abandoned helper");
    Save(manifest, SteamSessionManifest(abandonedKey));
    Json abandoned{{"steamRoot", Utf8(qaSteam.wstring())}, {"executable", Utf8((gameFolder / L"fc27.exe").wstring())},
                   {"executableIdentity", FileIdentity(gameFolder / L"fc27.exe")},
                   {"manifest", Utf8(manifest.wstring())}, {"manifestContents", SteamSessionManifest(abandonedKey)},
                   {"ownerPid", GetCurrentProcessId()}};
    Save(abandonedJournal, abandoned.dump());
    RecoverSteamPlacements(data, qaSteam);
    require(fs::exists(manifest) && fs::exists(gameFolder / L"fc27.exe"), "Live Steam session was removed.");
    abandoned["ownerPid"] = DWORD(0xfffffffe);
    Save(abandonedJournal, abandoned.dump());
    Save(manifest, "Foreign modified manifest");
    RecoverSteamPlacements(data, qaSteam);
    require(Read(manifest) == "Foreign modified manifest" && !fs::exists(gameFolder / L"fc27.exe"),
            "Recovery deleted a modified manifest or retained an owned helper.");
    Save(manifest, SteamSessionManifest(abandonedKey));
    RecoverSteamPlacements(data, qaSteam);
    require(!fs::exists(manifest) && !fs::exists(abandonedJournal), "Abandoned lease was not recovered.");
    const auto secondLibrary = data / L"steam-placement-tests" / RuntimeKey();
    fs::create_directories(secondLibrary / L"steamapps" / L"common");
    const auto librariesFile = qaSteam / L"steamapps" / L"libraryfolders.vdf";
    Save(librariesFile, "\"libraryfolders\" { \"0\" { \"path\" \"" + Utf8(qaSteam.generic_wstring()) +
                       "\" } \"1\" { \"path\" \"" + Utf8(secondLibrary.generic_wstring()) +
                       "\" } \"2\" { \"path\" \"relative/invalid\" } }");
    const auto libraries = SteamLibraryRoots(qaSteam);
    require(libraries.size() == 2 && SelectSteamLibrary(libraries) == qaSteam,
            "Registered Steam libraries were not discovered or deduplicated.");
    const auto foreignManifest = secondLibrary / L"steamapps" / L"appmanifest_4080220.acf";
    Save(foreignManifest, "Existing FC27 installation");
    require(SelectSteamLibrary(libraries) == secondLibrary && Read(foreignManifest) == "Existing FC27 installation",
            "FC27 installation in another library was ignored.");
    fs::remove(foreignManifest);
    fs::remove(secondLibrary / L"steamapps" / L"common");
    fs::remove(secondLibrary / L"steamapps");
    fs::remove(secondLibrary);
    fs::remove(librariesFile);
    // No-Steam mode implements the documented ordinary-folder alternative automatically.
    fs::path localExecutable;
    {
        ParentRun run(steamGame, false, preferences, data, true, {}, std::optional<fs::path>(fs::path{}));
        const auto deadline = GetTickCount64() + 5000;
        while (run.Snapshot().status == RuntimeStatus::Starting && GetTickCount64() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        require(run.Snapshot().status == RuntimeStatus::ProcessRunning && !fs::exists(manifest),
                "Ordinary FC27 folder helper failed or wrote to Steam.");
        const auto diagnostic = Json::parse(Read(data / L"info" / L"1531874756096295054.process.json"));
        localExecutable = Wide(CatalogText(diagnostic, "executable"));
        require(localExecutable.filename() == L"fc27.exe" &&
                    localExecutable.parent_path().filename() == L"EA SPORTS FC 27" &&
                    CatalogText(diagnostic, "placement") == "Local folder" &&
                    !diagnostic.value("steamManifestCreated", true) && fs::exists(localExecutable),
                "FC27 fallback did not preserve the required name/folder structure.");
        run.Stop();
    }
    require(!fs::exists(localExecutable) && !fs::exists(localExecutable.parent_path().parent_path()),
            "Ordinary FC27 folder was not cleaned up.");
    {
        ParentRun run(steamGame, false, preferences, data, true, {}, std::optional<fs::path>(fs::path{}), 60);
        const auto deadline = GetTickCount64() + 3000;
        while (GetTickCount64() < deadline) {
            try {
                const auto prepared = Json::parse(Read(data / L"info" / L"1531874756096295054.process.json"));
                if (CatalogText(prepared, "stage") == "prepared") {
                    localExecutable = Wide(CatalogText(prepared, "executable"));
                    break;
                }
            } catch (...) { }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        require(fs::exists(localExecutable) && run.Snapshot().status == RuntimeStatus::Starting && !run.Window(),
                "Prepared profile launched before the requested delay.");
        run.Stop();
    }
    require(!fs::exists(localExecutable), "Cancelling preparation left an FC27 helper behind.");
    fs::remove(qaSteam / L"steamapps" / L"common");
    fs::remove(qaSteam / L"steamapps");
    fs::remove(qaSteam);
    return 0;
}
