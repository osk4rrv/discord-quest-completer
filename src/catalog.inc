// Public metadata only. Store IDs and Discord application IDs are distinct identifiers.
inline std::string CatalogText(const Json &entry, const char *key) {
    const auto value = entry.find(key);
    return value != entry.end() && value->is_string() ? value->get<std::string>() : std::string{};
}
inline std::string CacheStem(const Game &game) {
    if (game.provider == Provider::Steam)
        return std::to_string(game.id);
    if (game.provider == Provider::Discord)
        return "discord-" + game.discordApplicationId;
    uint64_t hash = 14695981039346656037ull;
    // Use the immutable store key; resolving a Discord ID must not change cache paths.
    for (unsigned char c : game.catalogNamespace + ":" + game.catalogId)
        hash = (hash ^ c) * 1099511628211ull;
    return "epic-" + std::to_string(hash);
}
inline bool Fresh(const fs::path &file, int hours) {
    return fs::exists(file) &&
           fs::file_time_type::clock::now() - fs::last_write_time(file) < std::chrono::hours(hours);
}
inline std::wstring TitleKey(std::wstring text) {
    text = Lower(std::move(text));
    text.erase(std::remove_if(text.begin(), text.end(), [](wchar_t c) { return !iswalnum(c); }), text.end());
    return text;
}
#include "executable_sources.inc"
inline Json
CompactDiscordCatalog(const std::string &bytes,
                      const std::string &sourceUrl = "https://discord.com/api/v10/applications/detectable") {
    const auto source = Json::parse(bytes);
    if (!source.is_array())
        throw std::runtime_error("Unexpected Discord catalog response.");
    Json out = Json::array();
    for (const auto &entry : source) {
        if (!entry.is_object() || !entry.contains("id") || !entry["id"].is_string())
            continue;
        const auto id = entry["id"].get<std::string>();
        if (id.empty() || id.size() > 20 || id.find_first_not_of("0123456789") != std::string::npos)
            continue;
        out.push_back({{"id", id},
                       {"name", entry.value("name", std::string{})},
                       {"aliases", entry.value("aliases", Json::array())},
                       {"icon", entry.value("icon_hash", Json{})},
                       {"executables", entry.value("executables", Json::array())},
                       {"catalogSourceUrl", sourceUrl},
                       {"skus", entry.value("third_party_skus", Json::array())}});
    }
    if (out.empty())
        throw std::runtime_error("Discord application catalog is empty.");
    return out;
}
inline Json LoadDiscordCatalog(const fs::path &data, Cancellation *cancel = nullptr) {
    const auto path = data / L"catalog" / L"discord-apps.json";
    Json cached;
    try {
        cached = Json::parse(Read(path));
        if (!cached.is_array() || cached.empty() || !cached.front().contains("executables"))
            cached = Json{};
        if (!cached.empty() && Fresh(path, 24))
            return cached;
    } catch (...) {
        cached = Json{};
    }
    try {
        auto catalog = CompactDiscordCatalog(Get(L"https://discord.com/api/v10/applications/detectable", cancel));
        Save(path, catalog.dump());
        return catalog;
    } catch (...) {
        if (!cached.empty())
            return cached;
        auto catalog = CompactDiscordCatalog(
            Get(L"https://markterence.github.io/discord-quest-completer/detectable.json", cancel),
            "https://markterence.github.io/discord-quest-completer/detectable.json");
        Save(path, catalog.dump());
        return catalog;
    }
}
inline void SetDiscordExecutableSource(Game &game, const Json &entry) {
    game.discordExecutableRule = !game.windowsExecutables.empty();
    const auto source = entry.value("catalogSourceUrl", "https://discord.com/api/v10/applications/detectable");
    game.windowsExecutableSource = !game.discordExecutableRule ? ""
                                   : source == "https://discord.com/api/v10/applications/detectable"
                                       ? "Discord detectable catalog"
                                       : "Discord detectable catalog (community mirror)";
    game.windowsExecutableSourceUrl = game.discordExecutableRule ? source : "";
}
inline void ResolveDiscordIds(std::vector<Game> &games, const Json &catalog) {
    // Index once per batch instead of repeatedly scanning thousands of apps for every result.
    std::map<std::wstring, std::set<std::string>> skuIndex, titleIndex;
    std::map<std::string, const Json *> applications;
    std::map<std::string, std::vector<std::wstring>> aliases;
    std::map<std::string, std::vector<std::wstring>> executables;
    for (const auto &entry : catalog) {
        const auto appId = entry.at("id").get<std::string>();
        applications[appId] = &entry;
        for (const auto &executable : entry.value("executables", Json::array()))
            if (executable.is_object() && CatalogText(executable, "os") == "win32" &&
                !executable.value("is_launcher", false)) {
                auto name = Wide(CatalogText(executable, "name"));
                if (!name.empty())
                    executables[appId].push_back(std::move(name));
            }
        const auto title = TitleKey(Wide(CatalogText(entry, "name")));
        aliases[appId].push_back(Wide(CatalogText(entry, "name")));
        if (!title.empty())
            titleIndex[title].insert(appId);
        for (const auto &alias : entry.at("aliases"))
            if (alias.is_string()) {
                const auto key = TitleKey(Wide(alias.get<std::string>()));
                aliases[appId].push_back(Wide(alias.get<std::string>()));
                if (!key.empty())
                    titleIndex[key].insert(appId);
            }
        for (const auto &sku : entry.at("skus")) {
            const auto distributor = CatalogText(sku, "distributor"), id = CatalogText(sku, "id");
            if ((distributor == "steam" || distributor == "epic") && !id.empty())
                skuIndex[Wide(distributor) + L":" + Lower(Wide(id))].insert(appId);
        }
    }
    for (auto &game : games) {
        game.discordLookupAttempted = true;
        if (game.provider == Provider::Steam)
            game.steamApplicationId = std::to_string(game.id);
        if (!game.discordApplicationId.empty()) {
            game.searchAliases = aliases[game.discordApplicationId];
            game.windowsExecutables = executables[game.discordApplicationId];
            if (applications.count(game.discordApplicationId)) {
                SetDiscordExecutableSource(game, *applications.at(game.discordApplicationId));
                BindSteamSku(game, *applications.at(game.discordApplicationId));
            }
            continue;
        }
        const auto storeId = game.provider == Provider::Steam ? std::to_string(game.id) : game.catalogId;
        const std::wstring distributor = game.provider == Provider::Steam ? L"steam" : L"epic";
        const auto *skuMatches = &skuIndex[distributor + L":" + Lower(Wide(storeId))];
        bool namespaceMatch = false;
        if (skuMatches->empty() && game.provider == Provider::Epic && !game.catalogNamespace.empty()) {
            skuMatches = &skuIndex[L"epic:" + Lower(Wide(game.catalogNamespace))];
            namespaceMatch = !skuMatches->empty();
        }
        const auto &nameMatches = titleIndex[TitleKey(game.name)];
        const auto &matches = skuMatches->empty() ? nameMatches : *skuMatches;
        if (matches.size() == 1) {
            game.discordApplicationId = *matches.begin();
            game.searchAliases = aliases[game.discordApplicationId];
            game.windowsExecutables = executables[game.discordApplicationId];
            SetDiscordExecutableSource(game, *applications.at(game.discordApplicationId));
            BindSteamSku(game, *applications.at(game.discordApplicationId));
            game.discordMatch = skuMatches->empty() ? "Unique catalog title"
                                : namespaceMatch    ? "Epic namespace in Discord catalog"
                                                    : "Store SKU";
        }
    }
}
inline std::vector<Game> ParseEpic(const std::string &bytes, const fs::path &data) {
    const auto response = Json::parse(bytes);
    if (response.contains("errors"))
        throw std::runtime_error("Epic catalog query is unavailable.");
    const auto &elements = response.at("data").at("Catalog").at("searchStore").at("elements");
    if (!elements.is_array())
        throw std::runtime_error("Unexpected Epic catalog response.");
    std::vector<Game> games;
    for (const auto &entry : elements) {
        Game game;
        game.provider = Provider::Epic;
        game.catalogId = CatalogText(entry, "id");
        game.catalogNamespace = CatalogText(entry, "namespace");
        game.name = Wide(CatalogText(entry, "title"));
        game.description = CatalogText(entry, "description");
        const auto type = CatalogText(entry, "offerType");
        if (!game.Valid() || (!type.empty() && type != "BASE_GAME" && type != "EDITION"))
            continue;
        std::string slug = CatalogText(entry, "productSlug");
        if (entry.contains("catalogNs") && entry["catalogNs"].is_object())
            for (const auto &mapping : entry["catalogNs"].value("mappings", Json::array()))
                if (mapping.is_object() && mapping.value("pageType", std::string{}) == "productHome")
                    slug = mapping.value("pageSlug", slug);
        if (slug.size() > 5 && slug.substr(slug.size() - 5) == "/home")
            slug.resize(slug.size() - 5);
        game.storeUrl = slug.empty() ? "https://store.epicgames.com/en-US/browse?q=" + Utf8(Encode(game.name))
                                     : "https://store.epicgames.com/en-US/p/" + Utf8(Encode(Wide(slug)));
        for (const auto &image : entry.value("keyImages", Json::array())) {
            const auto imageType = image.value("type", std::string{});
            if (game.artUrl.empty() || imageType == "Thumbnail" || imageType == "OfferImageSquare")
                game.artUrl = Wide(image.value("url", std::string{}));
            if (imageType == "Thumbnail" || imageType == "OfferImageSquare")
                break;
        }
        game.artFile = data / L"art" / Wide(CacheStem(game) + "-icon.jpg");
        games.push_back(std::move(game));
        if (games.size() == 20)
            break;
    }
    return games;
}
inline std::vector<Game> EpicReferenceSearch(const std::wstring &query, const Json &catalog,
                                             const fs::path &data) {
    std::vector<Game> games;
    const auto key = Lower(query);
    for (const auto &entry : catalog) {
        const auto name = Wide(entry.value("name", std::string{}));
        bool matches = Lower(name).find(key) != std::wstring::npos;
        for (const auto &alias : entry.at("aliases"))
            if (alias.is_string() && Lower(Wide(alias.get<std::string>())).find(key) != std::wstring::npos)
                matches = true;
        if (!matches)
            continue;
        for (const auto &sku : entry.at("skus")) {
            if (sku.value("distributor", std::string{}) != "epic" || !sku["id"].is_string())
                continue;
            Game game;
            game.provider = Provider::Epic;
            game.referenceOnly = true;
            game.discordLookupAttempted = true;
            game.catalogId = sku["id"].get<std::string>();
            game.name = name;
            game.discordApplicationId = entry.at("id").get<std::string>();
            game.discordMatch = "Epic SKU in Discord catalog";
            if (entry.at("icon").is_string())
                game.artUrl = L"https://cdn.discordapp.com/app-icons/" + Wide(game.discordApplicationId) + L"/" +
                              Wide(entry.at("icon").get<std::string>()) + L".png?size=128";
            game.storeUrl = "https://store.epicgames.com/en-US/browse?q=" + Utf8(Encode(name));
            game.artFile = data / L"art" / Wide(CacheStem(game) + "-icon.jpg");
            if (game.Valid())
                games.push_back(std::move(game));
            break;
        }
    }
    std::stable_sort(games.begin(), games.end(), [&](const Game &a, const Game &b) {
        const bool exactA = TitleKey(a.name) == TitleKey(query), exactB = TitleKey(b.name) == TitleKey(query);
        return exactA != exactB ? exactA : a.name.size() < b.name.size();
    });
    if (games.size() > 20)
        games.resize(20);
    return games;
}
inline std::vector<Game> SearchProviderCatalog(const std::wstring &query, Provider provider, const fs::path &data,
                                               Cancellation *cancel, Json &discordCatalog, std::wstring &notice) {
    notice.clear();
    auto loadDiscord = [&] {
        if (discordCatalog.empty())
            discordCatalog = LoadDiscordCatalog(data, cancel);
    };
    std::vector<Game> games;
    if (provider == Provider::Steam) {
        games = SearchGames(query, data, cancel);
    } else {
        static thread_local std::chrono::steady_clock::time_point nextStoreAttempt{};
        const std::string text =
            "{ Catalog { searchStore(keywords:" + Json(Utf8(query)).dump() +
            ", country:\"PL\", locale:\"en-US\", count:20, start:0) { elements { id namespace title "
            "description offerType productSlug keyImages { type url } catalogNs { mappings { pageSlug pageType } "
            "} } } } }";
        try {
            if (std::chrono::steady_clock::now() < nextStoreAttempt)
                throw std::runtime_error("Epic store temporarily unavailable.");
            games =
                ParseEpic(Get(L"https://store.epicgames.com/graphql?query=" + Encode(Wide(text)), cancel), data);
        } catch (...) {
            if (std::chrono::steady_clock::now() >= nextStoreAttempt)
                nextStoreAttempt = std::chrono::steady_clock::now() + std::chrono::minutes(5);
            // Epic's storefront can deny native clients. Never bypass its access controls.
            notice = L"Epic store unavailable · using public reference catalog";
            try {
                loadDiscord();
                games = EpicReferenceSearch(query, discordCatalog, data);
            } catch (...) {
            }
            try {
                const auto path = data / L"catalog" / L"epic-promotions.json";
                std::string bytes;
                if (Fresh(path, 6))
                    bytes = Read(path);
                else {
                    bytes = Get(L"https://store-site-backend-static.ak.epicgames.com/"
                                L"freeGamesPromotions?locale=en-US&country=PL&allowCountries=PL",
                                cancel);
                    ParseEpic(bytes, data);
                    Save(path, bytes);
                }
                for (auto &game : ParseEpic(bytes, data)) {
                    if (Lower(game.name).find(Lower(query)) == std::wstring::npos)
                        continue;
                    auto found = std::find_if(games.begin(), games.end(), [&](const Game &g) {
                        return TitleKey(g.name) == TitleKey(game.name);
                    });
                    if (found == games.end() && games.size() < 20)
                        games.push_back(std::move(game));
                    else if (found != games.end()) {
                        game.discordApplicationId = found->discordApplicationId;
                        game.discordMatch = found->discordMatch;
                        *found = std::move(game);
                    }
                }
            } catch (...) {
            }
            if (games.empty() && discordCatalog.empty())
                throw std::runtime_error("Epic catalog is unavailable. Please try again later.");
        }
    }
    try {
        loadDiscord();
        ResolveDiscordIds(games, discordCatalog);
    } catch (...) {
        if (notice.empty())
            notice = L"Discord application IDs are currently unavailable";
    }
    return games;
}
inline std::vector<std::wstring> SearchWords(const std::wstring &text) {
    std::vector<std::wstring> words;
    std::wstring word;
    for (wchar_t c : Lower(text)) {
        if (iswalnum(c))
            word.push_back(c);
        else if (!word.empty()) {
            words.push_back(std::move(word));
            word.clear();
        }
    }
    if (!word.empty())
        words.push_back(std::move(word));
    return words;
}
inline int EditDistance(const std::wstring &a, const std::wstring &b) {
    std::vector<int> row(b.size() + 1);
    for (size_t i = 0; i <= b.size(); ++i)
        row[i] = static_cast<int>(i);
    for (size_t i = 0; i < a.size(); ++i) {
        int previous = row[0];
        row[0] = static_cast<int>(i + 1);
        for (size_t j = 0; j < b.size(); ++j) {
            const int old = row[j + 1];
            row[j + 1] = std::min({row[j + 1] + 1, row[j] + 1, previous + (a[i] != b[j])});
            previous = old;
        }
    }
    return row.back();
}
inline int SearchScore(const Game &game, const std::wstring &query) {
    const auto title = TitleKey(game.name), key = TitleKey(query);
    if (key.empty() || title.empty())
        return 0;
    if (title == key)
        return 100000;
    const bool aliasExact = std::any_of(game.searchAliases.begin(), game.searchAliases.end(),
                                        [&](const std::wstring &alias) { return TitleKey(alias) == key; });
    const auto wanted = SearchWords(query), words = SearchWords(game.name);
    int matched = 0, meaningful = 0;
    for (size_t i = 0; i < wanted.size(); ++i) {
        const auto &token = wanted[i];
        const bool number = std::all_of(token.begin(), token.end(), [](wchar_t c) { return iswdigit(c); });
        const bool found = std::any_of(words.begin(), words.end(), [&](const std::wstring &word) {
            return word == token || (!number && i + 1 == wanted.size() && word.find(token) == 0);
        });
        // A different sequel is not a useful match for a numbered title.
        if (number && !found)
            return aliasExact ? 95000 : 0;
        matched += found;
        meaningful += found && !number;
    }
    int tier = title.find(key) == 0                                            ? 90000
               : !wanted.empty() && matched == static_cast<int>(wanted.size()) ? 80000
               : title.find(key) != std::wstring::npos                         ? 70000
               : meaningful > 0                                                ? 50000 + matched * 1000
                                                                               : 0;
    if (aliasExact)
        tier = std::max(tier, 95000);
    if (!tier && key.size() >= 4 && key.size() <= 128 && title.size() <= 192) {
        const int distance = EditDistance(key, title);
        if (distance <= 2)
            tier = 40000 - distance * 1000;
    }
    if (!tier)
        return 0;
    int penalty = std::min(500, static_cast<int>(title.size() - std::min(title.size(), key.size())));
    for (const auto &word : words)
        if ((word == L"demo" || word == L"soundtrack" || word == L"dlc" || word == L"edition" ||
             word == L"bundle") &&
            std::find(wanted.begin(), wanted.end(), word) == wanted.end())
            penalty += 20;
    return tier - penalty;
}
inline std::vector<Game> RankGames(std::vector<Game> games, const std::wstring &query, size_t limit = 200) {
    struct Candidate {
        Game game;
        int score;
        size_t order;
    };
    std::vector<Candidate> candidates;
    for (size_t i = 0; i < games.size(); ++i) {
        const int score = SearchScore(games[i], query);
        if (score > 0 && games[i].Valid())
            candidates.push_back({std::move(games[i]), score, i});
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate &a, const Candidate &b) {
        if (a.score != b.score)
            return a.score > b.score;
        if (a.game.discordApplicationId.empty() != b.game.discordApplicationId.empty())
            return !a.game.discordApplicationId.empty();
        if (a.game.referenceOnly != b.game.referenceOnly)
            return !a.game.referenceOnly;
        if (a.game.provider != b.game.provider)
            return a.game.provider == Provider::Steam;
        return a.order < b.order;
    });
    std::vector<Game> result;
    for (auto &candidate : candidates) {
        auto duplicate = std::find_if(result.begin(), result.end(), [&](const Game &game) {
            if (!game.discordApplicationId.empty() && !candidate.game.discordApplicationId.empty())
                return game.discordApplicationId == candidate.game.discordApplicationId;
            if (TitleKey(game.name) != TitleKey(candidate.game.name))
                return false;
            return true;
        });
        if (duplicate != result.end()) {
            if (duplicate->artUrl.empty() && !candidate.game.artUrl.empty()) {
                duplicate->artUrl = candidate.game.artUrl;
                duplicate->artFile = candidate.game.artFile;
            }
            continue;
        }
        result.push_back(std::move(candidate.game));
        if (limit && result.size() == limit)
            break;
    }
    return result;
}
inline std::vector<Game> DiscordReferenceSearch(const std::wstring &query, const Json &catalog,
                                                const fs::path &data) {
    std::vector<Game> games;
    // Put common campaign games first; the rest of the catalog remains browsable.
    const std::vector<std::wstring> picks{L"Roblox",
                                          L"VALORANT",
                                          L"Genshin Impact",
                                          L"GODDESS OF VICTORY: NIKKE",
                                          L"Honkai: Star Rail",
                                          L"Marvel Rivals",
                                          L"Delta Force",
                                          L"Apex Legends",
                                          L"Fortnite",
                                          L"World of Warcraft",
                                          L"Diablo IV",
                                          L"Street Fighter 6",
                                          L"Where Winds Meet",
                                          L"R.E.P.O.", L"Arknights: Endfield", L"NTE: Neverness to Everness",
                                          L"EA Sports FC 27", L"EA Sports FC 26", L"Battlefield 6",
                                          L"Enginefall", L"Comet", L"Opera GX", L"Rune Dice", L"PRAGMATA",
                                          L"ARC Raiders", L"2XKO", L"Arena Breakout Infinite",
                                          L"Blue Protocol: Star Resonance", L"Duet Night Abyss",
                                          L"Destiny 2", L"Fallout 76", L"Fallout 4", L"Grounded 2",
                                          L"Hunt: Showdown 1896", L"Warframe", L"War Thunder",
                                          L"Path of Exile 2", L"Path of Exile", L"Overwatch 2",
                                          L"League of Legends", L"Counter-Strike 2", L"Dota 2",
                                          L"PUBG: BATTLEGROUNDS", L"Rocket League", L"THE FINALS",
                                          L"Dead by Daylight", L"Deadlock", L"Palworld", L"Wuthering Waves",
                                          L"Zenless Zone Zero", L"Lost Ark", L"Once Human", L"Sea of Thieves",
                                          L"The First Descendant", L"Throne and Liberty", L"Tower of Fantasy",
                                          L"Infinity Nikki", L"SUPERVIVE", L"Monster Hunter Wilds",
                                          L"Warhammer 40,000: Space Marine 2", L"Rainbow Six Siege",
                                          L"Call of Duty", L"Minecraft", L"Final Fantasy XIV",
                                          L"Guild Wars 2", L"EVE Online", L"The Elder Scrolls Online",
                                          L"World of Tanks", L"World of Warships", L"War Robots",
                                          L"Rust", L"DayZ", L"Terraria", L"Stardew Valley", L"Valheim",
                                          L"Satisfactory", L"No Man's Sky", L"Brawlhalla", L"SMITE 2",
                                          L"Splitgate 2", L"FragPunk", L"Mecha BREAK", L"Umamusume: Pretty Derby",
                                          L"Portal 2", L"Grand Theft Auto V", L"Red Dead Redemption 2",
                                          L"Cyberpunk 2077", L"Baldur's Gate 3", L"Elden Ring",
                                          L"Hogwarts Legacy", L"Forza Horizon 5", L"Forza Horizon 6"};
    std::map<std::wstring, size_t> pickKeys;
    for (size_t i = 0; i < picks.size(); ++i)
        pickKeys[TitleKey(picks[i])] = i;
    std::map<std::string, size_t> priority;
    const auto curated = BundledQuestPaths();
    for (const auto &entry : catalog) {
        Game game;
        game.provider = Provider::Discord;
        game.referenceOnly = true;
        game.discordLookupAttempted = true;
        game.name = Wide(CatalogText(entry, "name"));
        game.discordApplicationId = CatalogText(entry, "id");
        game.catalogId = game.discordApplicationId;
        game.discordMatch = "Discord application catalog";
        BindSteamSku(game, entry);
        for (const auto &alias : entry.value("aliases", Json::array()))
            if (alias.is_string())
                game.searchAliases.push_back(Wide(alias.get<std::string>()));
        for (const auto &record : curated.value("games", Json::array()))
            if (CatalogText(record, "discordId") == game.discordApplicationId)
                for (const auto &alias : record.value("aliases", Json::array()))
                    if (alias.is_string())
                        game.searchAliases.push_back(Wide(alias.get<std::string>()));
        if (!query.empty() && !SearchScore(game, query))
            continue;
        for (const auto &exe : entry.value("executables", Json::array()))
            if (exe.is_object() && CatalogText(exe, "os") == "win32" && !exe.value("is_launcher", false))
                game.windowsExecutables.push_back(Wide(CatalogText(exe, "name")));
        SetDiscordExecutableSource(game, entry);
        if (entry.contains("icon") && entry["icon"].is_string())
            game.artUrl = L"https://cdn.discordapp.com/app-icons/" + Wide(game.discordApplicationId) + L"/" +
                          Wide(entry["icon"].get<std::string>()) + L".png?size=128";
        game.artFile = data / L"art" / Wide(CacheStem(game) + "-icon.jpg");
        if (game.Valid()) {
            size_t order = picks.size();
            for (const auto &name : game.searchAliases) {
                const auto found = pickKeys.find(TitleKey(name));
                if (found != pickKeys.end())
                    order = std::min(order, found->second);
            }
            const auto found = pickKeys.find(TitleKey(game.name));
            if (found != pickKeys.end())
                order = std::min(order, found->second);
            priority[game.discordApplicationId] = order;
            games.push_back(std::move(game));
        }
    }
    if (!query.empty())
        return RankGames(std::move(games), query, 0);
    std::stable_sort(games.begin(), games.end(), [&](const Game &a, const Game &b) {
        const auto ai = priority.at(a.discordApplicationId), bi = priority.at(b.discordApplicationId);
        if (ai != bi)
            return ai < bi;
        const bool standardA = !StandardPath(a).empty(), standardB = !StandardPath(b).empty();
        if (standardA != standardB)
            return standardA;
        return Lower(a.name) < Lower(b.name);
    });
    std::set<std::string> seen;
    games.erase(std::remove_if(games.begin(), games.end(),
                               [&](const Game &g) { return !seen.insert(g.discordApplicationId).second; }),
                games.end());
    return games;
}
inline std::vector<Game> SearchCatalogResults(const std::wstring &query, Provider provider, const fs::path &data,
                                              Cancellation *cancel, Json &discordCatalog, std::wstring &notice) {
    if (query.empty() || provider == Provider::Discord) {
        if (discordCatalog.empty())
            discordCatalog = LoadDiscordCatalog(data, cancel);
        return DiscordReferenceSearch(query, discordCatalog, data);
    }
    if (provider != Provider::All)
        return RankGames(SearchProviderCatalog(query, provider, data, cancel, discordCatalog, notice), query);
    // Steam and Epic requests run together; only the Epic branch accesses the reference catalog here.
    auto steamRequest = std::async(std::launch::async, [&] { return SearchGames(query, data, cancel); });
    std::vector<Game> steam, epic;
    bool steamFailed = false, epicFailed = false;
    try {
        epic = SearchProviderCatalog(query, Provider::Epic, data, cancel, discordCatalog, notice);
    } catch (...) {
        epicFailed = true;
        notice = L"Epic unavailable";
    }
    try {
        steam = steamRequest.get();
    } catch (...) {
        steamFailed = true;
        notice = notice.empty() ? L"Steam unavailable" : L"Steam unavailable · " + notice;
    }
    try {
        if (discordCatalog.empty())
            discordCatalog = LoadDiscordCatalog(data, cancel);
        ResolveDiscordIds(steam, discordCatalog);
        auto references = DiscordReferenceSearch(query, discordCatalog, data);
        for (auto &game : references)
            steam.push_back(std::move(game));
    } catch (...) {
        if (notice.empty())
            notice = L"Discord application IDs are currently unavailable";
    }
    for (auto &game : epic)
        steam.push_back(std::move(game));
    if (steamFailed && epicFailed && steam.empty())
        throw std::runtime_error("The game catalogs are unavailable. Please try again.");
    return RankGames(std::move(steam), query);
}
inline std::vector<Game> PageCatalogGames(std::vector<Game> games, bool standardOnly, size_t page,
                                         size_t *total = nullptr) {
    if (standardOnly)
        games.erase(std::remove_if(games.begin(), games.end(), [](const Game &game) {
            return game.standardRequiresSteam || StandardPath(game).empty();
        }), games.end());
    if (total)
        *total = games.size();
    constexpr size_t pageSize = 100;
    const size_t start = std::min(page, games.empty() ? size_t(0) : (games.size() - 1) / pageSize) * pageSize;
    const size_t end = std::min(start + pageSize, games.size());
    return {std::make_move_iterator(games.begin() + start), std::make_move_iterator(games.begin() + end)};
}
inline std::vector<Game> SearchCatalog(const std::wstring &query, Provider provider, const fs::path &data,
                                       Cancellation *cancel, Json &discordCatalog, std::wstring &notice,
                                       bool standardOnly = false, size_t page = 0, size_t *total = nullptr) {
    auto games = SearchCatalogResults(query, provider, data, cancel, discordCatalog, notice);
    // One compact directory fetch also fills missing paths before availability filtering.
    try {
        ApplyCommunityExecutableBatch(games, discordCatalog, LoadCommunityDirectory(data, cancel));
    } catch (...) {
    }
    for (auto &game : games)
        LoadCachedExecutableMetadata(game, data);
    ApplyCuratedExecutableBatch(games, discordCatalog, BundledQuestPaths());
    return PageCatalogGames(std::move(games), standardOnly, page, total);
}
