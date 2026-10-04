constexpr double SessionDuration = 15 * 60;
inline double SteadyNow() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
enum class SessionState { Running, Completed, Stopped };
struct Session {
    int uid = 0;
    Game game;
    bool rpc = false;
    double started = 0, finished = 0, elapsedAtFinish = 0;
    SessionState state = SessionState::Running;
    double runtimeElapsed = -1;
    double Elapsed(double now) const {
        return state == SessionState::Running
                   ? std::clamp(runtimeElapsed >= 0 ? runtimeElapsed : now - started, 0.0, SessionDuration)
                   : elapsedAtFinish;
    }
};
struct SessionBook {
    std::vector<Session> items;
    uint64_t completedRuns = 0;
    int nextId = 1;
    bool IsRunning(const Game &game) const {
        return std::any_of(items.begin(), items.end(), [&](const Session &s) {
            return s.game.SameApplication(game) && s.state == SessionState::Running;
        });
    }
    int Start(const Game &game, bool rpc, double now) {
        if (!game.Valid() || IsRunning(game))
            return 0;
        items.push_back({nextId++, game, rpc, now});
        return items.back().uid;
    }
    bool Stop(int uid, double now) {
        for (auto &s : items)
            if (s.uid == uid && s.state == SessionState::Running) {
                s.elapsedAtFinish = s.Elapsed(now);
                s.finished = now;
                s.state = s.elapsedAtFinish >= SessionDuration ? SessionState::Completed : SessionState::Stopped;
                if (s.state == SessionState::Completed)
                    ++completedRuns;
                return true;
            }
        return false;
    }
    bool Update(double now) {
        bool changed = false;
        for (auto &s : items)
            if (s.state == SessionState::Running && s.Elapsed(now) >= SessionDuration) {
                s.elapsedAtFinish = SessionDuration;
                s.finished = now;
                s.state = SessionState::Completed;
                ++completedRuns;
                changed = true;
            }
        return changed;
    }
    size_t ActiveCount() const {
        return static_cast<size_t>(std::count_if(
            items.begin(), items.end(), [](const Session &s) { return s.state == SessionState::Running; }));
    }
    double SavedSeconds() const {
        return static_cast<double>(completedRuns) * SessionDuration;
    }
    bool Prune(double now, double delay = .28) {
        const size_t count = items.size();
        items.erase(std::remove_if(items.begin(), items.end(),
                                   [&](const Session &s) {
                                       return s.state != SessionState::Running && now - s.finished >= delay;
                                   }),
                    items.end());
        return items.size() != count;
    }
};
struct Preferences {
    int theme = 0, accent = 0;
    int preset = 0;
    unsigned customBackground = 0x15151c, customSurface = 0x22222c, customAccent = 0xb7a0ed;
    bool animations = true;
    uint64_t completedRuns = 0;
    void FromJson(const Json &j) {
        theme = std::clamp(j.value("theme", 0), 0, 2);
        const int savedPreset = j.value("preset", theme == 1 ? 3 : 0);
        preset = savedPreset >= 0 && savedPreset <= 4 ? savedPreset : 0;
        accent = std::clamp(j.value("accent", 0), 0, 5);
        // v4 custom themes used the custom accent implicitly. Preserve their visible color.
        if (j.value("version", 0) <= 4 && preset == 4)
            accent = 5;
        theme = preset == 3 ? 1 : 0;
        customBackground = j.value("customBackground", customBackground) & 0xffffff;
        customSurface = j.value("customSurface", customSurface) & 0xffffff;
        customAccent = j.value("customAccent", customAccent) & 0xffffff;
        animations = j.value("animations", true);
        // v2 counts described detected PC games, so never migrate them into local session counts.
        completedRuns = std::min<uint64_t>(j.value("completedRuns", uint64_t(0)), 100000000);
    }
    void Load(const fs::path &dir) {
        try {
            FromJson(Json::parse(Read(dir / L"preferences.json")));
        } catch (...) {
        }
    }
    std::string Serialize() const {
        return Json{{"version", 5},
                    {"theme", theme},
                    {"preset", preset},
                    {"accent", accent},
                    {"customBackground", customBackground},
                    {"customSurface", customSurface},
                    {"customAccent", customAccent},
                    {"animations", animations},
                    {"completedRuns", completedRuns}}
            .dump(2);
    }
    void SaveTo(const fs::path &dir) const {
        Save(dir / L"preferences.json", Serialize());
    }
};
inline void FetchArtwork(Game game, Cancellation *cancel = nullptr) {
    if (fs::exists(game.artFile) || game.artUrl.empty())
        return;
    Save(game.artFile, Get(game.artUrl, cancel));
}
inline std::string Html(const std::string &text) {
    std::string out;
    for (char c : text)
        switch (c) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        case '\'':
            out += "&#39;";
            break;
        default:
            out += c;
        }
    return out;
}
inline std::string PlainText(std::string text) {
    text = std::regex_replace(text, std::regex("<[^>]*>"), "");
    for (const auto &[from, to] : std::vector<std::pair<std::string, std::string>>{
             {"&quot;", "\""}, {"&#39;", "'"}, {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&nbsp;", " "}}) {
        size_t start = 0;
        while ((start = text.find(from, start)) != std::string::npos) {
            text.replace(start, from.size(), to);
            start += to.size();
        }
    }
    return text;
}
inline std::string InfoHtml(const Game &game, bool rpc, double elapsed, const Json &details,
                            const Json &rpcDiagnostic = Json{}) {
    const auto escaped = [](const Json &j, const char *key) { return Html(j.value(key, std::string{})); };
    const auto list = [&](const char *key) {
        std::string names;
        if (details.contains(key) && details[key].is_array())
            for (const auto &name : details[key])
                if (name.is_string()) {
                    if (!names.empty())
                        names += ", ";
                    names += Html(name.get<std::string>());
                }
        return names.empty() ? std::string("—") : names;
    };
    const unsigned seconds = static_cast<unsigned>(std::clamp(elapsed, 0.0, SessionDuration));
    const auto timer =
        std::to_string(seconds / 60) + ":" + (seconds % 60 < 10 ? "0" : "") + std::to_string(seconds % 60);
    std::string document = R"html(<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; style-src 'unsafe-inline'; font-src file:; img-src file:; base-uri 'none'; form-action 'none'">
<title>)html" + Html(Utf8(game.name)) +
                           R"html( · Session info</title><style>
@font-face{font-family:Inter;src:url('../fonts/Inter.ttf')}*{box-sizing:border-box}body{margin:0;background:#121216;color:#efeff3;font:15px/1.6 Inter,system-ui,Segoe UI,sans-serif}
main{max-width:800px;margin:64px auto;padding:0 24px}header{display:flex;align-items:center;gap:20px;margin:0 0 36px}
.icon{width:64px;height:64px;object-fit:cover;border-radius:16px;background:#222228}h1{font-size:30px;letter-spacing:-.8px;margin:0}
p{margin:8px 0;color:#a5a5b0}.eyebrow{font-size:11px;letter-spacing:2px;text-transform:uppercase;color:#9994b0}
.stats{display:grid;grid-template-columns:repeat(3,1fr);gap:12px;margin:0 0 24px}.stat,section{background:#1b1b21;border:1px solid #303037;border-radius:16px;padding:20px}
.label{font-size:12px;color:#9797a3}.value{font-size:22px;font-weight:600;margin-top:6px;font-variant-numeric:tabular-nums}
section{margin-bottom:20px}h2{font-size:16px;margin:0 0 12px}.details{display:grid;grid-template-columns:100px 1fr;gap:12px;font-size:13px}
.details span:nth-child(odd){color:#9797a3}.details span{overflow-wrap:anywhere}a{display:inline-block;color:#e8e3f7;background:#302b3d;text-decoration:none;padding:10px 18px;border-radius:10px}
footer{font-size:12px;color:#777782;margin:24px 0}@media(max-width:560px){main{margin:32px auto}.stats{grid-template-columns:1fr}h1{font-size:23px}}
</style></head><body><main><header>)html";
    if (fs::exists(game.artFile))
        document += "<img class=\"icon\" alt=\"Game icon\" src=\"../art/" +
                    Html(Utf8(game.artFile.filename().wstring())) + "\">";
    const bool epic = game.provider == Provider::Epic, discord = game.provider == Provider::Discord;
    document += "<div><div class=\"eyebrow\">Session info</div><h1>" + Html(Utf8(game.name)) + "</h1><p>" +
                (discord ? std::string("Discord application catalog")
                 : epic  ? std::string(game.referenceOnly ? "Epic reference catalog" : "Epic Store catalog")
                         : std::string("Steam catalog")) +
                "</p></div></header>";
    document +=
        "<div class=\"stats\"><div class=\"stat\"><div class=\"label\">Local elapsed</div><div class=\"value\">" +
        timer +
        "</div></div><div class=\"stat\"><div class=\"label\">Target</div><div class=\"value\">15:00</div></div>"
        "<div class=\"stat\"><div class=\"label\">Session</div><div class=\"value\">" +
        (rpc ? "RPC" : "Standard") + "</div></div></div>";
    if (!details.empty()) {
        document += "<section><h2>About the game</h2><p>" +
                    Html(PlainText(details.value("short_description", std::string{}))) + "</p></section>";
        if (details.contains("developers") || details.contains("publishers") || details.contains("release_date")) {
            document += "<section><div class=\"details\"><span>Developer</span><span>" + list("developers") +
                        "</span><span>Publisher</span><span>" + list("publishers") + "</span>";
            if (details.contains("release_date"))
                document += "<span>Released</span><span>" + escaped(details["release_date"], "date") + "</span>";
            document += "</div></section>";
        }
    }
    document += "<section><h2>Identifiers</h2><div class=\"details\"><span>" +
                std::string(discord ? "Catalog App ID"
                            : epic  ? (game.referenceOnly ? "Epic SKU" : "Epic offer")
                                    : "Steam App ID") +
                "</span><span>" +
                Html(discord ? game.discordApplicationId
                     : epic  ? game.catalogId
                             : std::to_string(game.id)) +
                "</span>";
    if (!game.catalogNamespace.empty())
        document += "<span>Epic namespace</span><span>" + Html(game.catalogNamespace) + "</span>";
    document += "<span>Discord App ID</span><span>" +
                (game.discordApplicationId.empty()
                     ? std::string(game.discordLookupAttempted ? "No unique catalog match"
                                                               : "Catalog currently unavailable")
                     : Html(game.discordApplicationId)) +
                "</span>";
    if (!game.discordApplicationId.empty())
        document += "<span>Matched by</span><span>" + Html(game.discordMatch) + "</span>";
    document += "</div></section>";
    document += "<section><h2>Session details</h2><div class=\"details\"><span>Windows EXE</span><span>";
    if (game.windowsExecutables.empty())
        document += "No published Windows executable";
    else
        for (size_t i = 0; i < game.windowsExecutables.size(); ++i)
            document += (i ? "<br>" : "") + Html(Utf8(game.windowsExecutables[i]));
    document +=
        "</span><span>EXE source</span><span>" +
        (game.windowsExecutableSource.empty() ? std::string("Unavailable") : Html(game.windowsExecutableSource)) +
        "</span><span>Discord rule</span><span>" +
        (game.discordExecutableRule ? "Published" : "Not published for this executable") + "</span></div>";
    if (!game.standardUnavailableReason.empty())
        document += "<p>Standard: " + Html(game.standardUnavailableReason) + "</p>";
    if (!CatalogText(rpcDiagnostic, "launchMessage").empty())
        document += "<p>Latest launch error: " + Html(CatalogText(rpcDiagnostic, "launchMessage")) + "</p>";
    if (rpc)
        document += "<p>RPC publishes Rich Presence and starts a hidden game helper when a Standard profile "
                    "is available. Without a profile it publishes presence only. Its timer measures "
                    "acknowledged activity time, not Quest progress. A Playing status does not confirm "
                    "that a Play Quest is being credited.</p>";
    else if (!game.windowsExecutables.empty() && !game.discordExecutableRule)
        document += "<p>This fallback uses a Windows executable from the source listed above. "
                    "Its folder structure comes from community observations. "
                    "Quest credit remains unverified.</p>";
    if (!game.steamInstallDirectory.empty())
        document += "<p>Start automatically locates your registered Steam libraries, creates EA SPORTS FC 27 "
                    "and starts its fc27.exe helper. If no usable Steam library is found, it uses a local folder. "
                    "Steam placement also creates a temporary manifest. "
                    "Existing game folders and manifests are never replaced. Stop removes this session's files. "
                    "If Discord cached its game list before starting, restart Discord to refresh detection.</p>";
    if (game.windowsExecutableSourceUrl.rfind("https://discord.com/", 0) == 0 ||
        game.windowsExecutableSourceUrl.rfind("https://forums.ea.com/", 0) == 0 ||
        game.windowsExecutableSourceUrl.rfind("https://github.com/shizeh/", 0) == 0 ||
        game.windowsExecutableSourceUrl.rfind("https://github.com/Sinnisterly/", 0) == 0 ||
        game.windowsExecutableSourceUrl == "https://www.reddit.com/r/DiscordQuests/comments/1wq2m86/ea_sports_fc_27_quest/" ||
        game.windowsExecutableSourceUrl ==
            "https://markterence.github.io/discord-quest-completer/detectable.json" ||
        game.windowsExecutableSourceUrl.rfind("https://api.steamcmd.net/", 0) == 0)
        document += "<a href=\"" + Html(game.windowsExecutableSourceUrl) + "\">View EXE source ↗</a>";
    document += "</section>";
    if (rpc && !rpcDiagnostic.empty()) {
        document += "<section><h2>Latest RPC diagnostic</h2><div class=\"details\"><span>Stage</span><span>" +
                    Html(CatalogText(rpcDiagnostic, "stage")) + "</span><span>READY</span><span>" +
                    (rpcDiagnostic.value("ready", false) ? "Received" : "Not received") +
                    "</span><span>Activity ACK</span><span>" +
                    (rpcDiagnostic.value("activityAcknowledged", false) ? "Received" : "Not received") +
                    "</span><span>Error code</span><span>" + std::to_string(rpcDiagnostic.value("errorCode", 0)) +
                    "</span></div><p>" + Html(CatalogText(rpcDiagnostic, "message")) +
                    "</p><p>Latest connection attempt; Quest progress is not available through RPC.</p></section>";
    }
    const std::string url = discord ? "https://discord.com/discovery/quests"
                            : epic  ? (game.storeUrl.rfind("https://store.epicgames.com/", 0) == 0
                                           ? game.storeUrl
                                           : "https://store.epicgames.com/en-US/browse")
                                    : "https://store.steampowered.com/app/" + std::to_string(game.id) + "/";
    document += "<a href=\"" + Html(url) + "\">Open " +
                (discord ? "Discord Quests"
                 : epic  ? "Epic"
                         : "Steam") +
                " page ↗</a><footer>Public catalog metadata. Session values are a local snapshot. "
                "Discord IDs come from Discord's public application catalog.</footer></main></body></html>";
    return document;
}
inline fs::path GenerateInfo(const Game &game, bool rpc, double elapsed, const fs::path &data,
                             Cancellation *cancel = nullptr, std::string *error = nullptr) {
    Json details;
    const auto cache = data / L"info" / Wide(CacheStem(game) + ".json");
    try {
        if (fs::exists(cache))
            details = Json::parse(Read(cache));
        else if (game.provider != Provider::Steam) {
            if (!game.description.empty()) {
                details = {{"short_description", game.description}};
                Save(cache, details.dump());
            }
        } else {
            const auto response = Json::parse(Get(L"https://store.steampowered.com/api/appdetails?appids=" +
                                                      std::to_wstring(game.id) + L"&l=english",
                                                  cancel));
            const auto &entry = response.at(std::to_string(game.id));
            if (entry.value("success", false)) {
                details = entry.at("data");
                Save(cache, details.dump());
            }
        }
    } catch (const std::exception &failure) {
        if (error)
            *error = failure.what();
        // Session information remains available offline.
    }
    Game resolved = game;
    try {
        if (resolved.discordApplicationId.empty()) {
            std::vector<Game> games{resolved};
            const auto catalog = LoadDiscordCatalog(data, cancel);
            ResolveDiscordIds(games, catalog);
            resolved = std::move(games.front());
        }
    } catch (...) {
    }
    if (StandardPath(resolved).empty())
        try {
            ResolveExecutableMetadata(resolved, data, cancel, LoadDiscordCatalog(data, cancel));
        } catch (...) {
        }
    try {
        FetchArtwork(resolved, cancel);
    } catch (...) {
    }
    const auto report = data / L"info" / Wide(CacheStem(game) + ".html");
    Json diagnostic;
    if (rpc && !resolved.discordApplicationId.empty())
        try {
            diagnostic = Json::parse(Read(data / L"info" / Wide(resolved.discordApplicationId + ".rpc.json")));
        } catch (...) {
        }
    try {
        const auto launch = Json::parse(Read(data / L"info" / Wide(resolved.discordApplicationId + ".launch.json")));
        diagnostic["launchMessage"] = CatalogText(launch, "message");
    } catch (...) { }
    Save(report, InfoHtml(resolved, rpc, elapsed, details, diagnostic));
    return report;
}
struct Snapshot {
    unsigned generation = 0, reportSerial = 0;
    std::wstring query, error, notice;
    std::vector<Game> games;
    fs::path report;
    bool searching = false;
    size_t totalGames = 0, page = 0;
};
class Worker {
    fs::path data;
    std::thread thread;
    std::mutex mutex;
    std::condition_variable condition;
    std::atomic<bool> stop{false};
    std::atomic<unsigned> generation{0};
    Cancellation cancellation;
    std::wstring requested;
    Json discordCatalog;
    std::string preferences;
    bool pending = false;
    bool requestedStandardOnly = false;
    size_t requestedPage = 0;
    struct InfoRequest {
        Game game;
        bool rpc;
        double elapsed;
    };
    struct ArtRequest {
        Game game;
        unsigned ticket;
    };
    struct ExecutableRequest {
        Game game;
        unsigned ticket;
    };
    std::deque<InfoRequest> reports;
    std::deque<ArtRequest> artwork;
    std::optional<ExecutableRequest> executable;
    HWND window;
    Snapshot state;
    void Notify() {
        if (window && !stop)
            PostMessageW(window, Updated, 0, 0);
    }
    void Run() {
        while (!stop) {
            std::wstring query;
            bool standardOnly = false;
            size_t page = 0;
            std::string prefs;
            std::optional<InfoRequest> info;
            std::optional<ArtRequest> art;
            std::optional<ExecutableRequest> metadata;
            unsigned ticket = 0;
            {
                std::unique_lock<std::mutex> lock(mutex);
                condition.wait(lock, [&] {
                    return stop || pending || !reports.empty() || !preferences.empty() || !artwork.empty() ||
                           executable.has_value();
                });
                if (stop)
                    break;
                prefs = std::move(preferences);
                preferences.clear();
                if (!reports.empty()) {
                    info = std::move(reports.front());
                    reports.pop_front();
                } else if (pending) {
                    query = requested;
                    standardOnly = requestedStandardOnly;
                    page = requestedPage;
                    ticket = generation;
                    pending = false;
                } else if (executable) {
                    metadata = std::move(executable);
                    executable.reset();
                } else if (!artwork.empty()) {
                    art = std::move(artwork.front());
                    artwork.pop_front();
                }
            }
            try {
                if (!prefs.empty())
                    Save(data / L"preferences.json", prefs);
                if (metadata && metadata->ticket == generation) {
                    ResolveExecutableMetadata(metadata->game, data, &cancellation, discordCatalog);
                    if (metadata->ticket == generation && !stop) {
                        {
                            std::lock_guard<std::mutex> lock(mutex);
                            for (auto &game : state.games)
                                if (game.Identity() == metadata->game.Identity())
                                    game = metadata->game;
                        }
                        Notify();
                    }
                }
                if (art && art->ticket == generation && !fs::exists(art->game.artFile)) {
                    try {
                        FetchArtwork(art->game, &cancellation);
                        Notify();
                    } catch (...) {
                    }
                }
                if (info) {
                    const auto file = GenerateInfo(info->game, info->rpc, info->elapsed, data, &cancellation);
                    if (stop)
                        continue;
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        state.report = file;
                        ++state.reportSerial;
                    }
                    Notify();
                }
                if (!ticket)
                    continue;
                std::wstring notice;
                size_t total = 0;
                auto games = SearchCatalog(query, Provider::All, data, &cancellation, discordCatalog, notice,
                                           standardOnly, page, &total);
                if (ticket != generation || stop)
                    continue;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    state.generation = ticket;
                    state.query = query;
                    state.error.clear();
                    state.notice = notice;
                    state.games = games;
                    state.totalGames = total;
                    state.page = std::min(page, total ? (total - 1) / 100 : size_t(0));
                    state.searching = false;
                    artwork.clear();
                    for (const auto &game : games)
                        artwork.push_back({game, ticket});
                }
                Notify();
            } catch (const std::exception &error) {
                if (!ticket || ticket != generation || stop)
                    continue;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    state.generation = ticket;
                    state.query = query;
                    state.games.clear();
                    state.error = Wide(error.what());
                    state.searching = false;
                }
                Notify();
            }
        }
        std::string remaining;
        {
            std::lock_guard<std::mutex> lock(mutex);
            remaining = std::move(preferences);
        }
        try {
            if (!remaining.empty())
                Save(data / L"preferences.json", remaining);
        } catch (...) {
        }
    }

  public:
    Worker(fs::path dir, HWND hwnd) : data(std::move(dir)), window(hwnd) {
        thread = std::thread([this] { Run(); });
    }
    ~Worker() {
        stop = true;
        cancellation.Stop();
        condition.notify_all();
        if (thread.joinable())
            thread.join();
    }
    void Persist(std::string bytes) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            preferences = std::move(bytes);
        }
        condition.notify_one();
    }
    void Info(const Game &game, bool rpc, double elapsed) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (reports.size() < 4)
                reports.push_back({game, rpc, elapsed});
        }
        condition.notify_one();
    }
    void Executables(const Game &game) {
        if (!StandardPath(game).empty() || game.executableLookupAttempted || game.executableResolving)
            return;
        {
            std::lock_guard<std::mutex> lock(mutex);
            executable = ExecutableRequest{game, generation.load()};
            for (auto &entry : state.games)
                if (entry.Identity() == game.Identity())
                    entry.executableResolving = true;
        }
        condition.notify_one();
    }
    unsigned Search(const std::wstring &query, bool standardOnly = false, size_t page = 0) {
        std::lock_guard<std::mutex> lock(mutex);
        const unsigned ticket = ++generation;
        requested = query;
        requestedStandardOnly = standardOnly;
        requestedPage = page;
        pending = query.empty() || query.size() >= 2;
        state.games.clear();
        artwork.clear();
        state.totalGames = 0;
        state.page = page;
        executable.reset();
        state.error.clear();
        state.notice.clear();
        state.searching = pending;
        state.generation = ticket;
        state.query = query;
        condition.notify_one();
        return ticket;
    }
    Snapshot Take() {
        std::lock_guard<std::mutex> lock(mutex);
        return state;
    }
};
