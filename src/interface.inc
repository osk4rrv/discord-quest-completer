static float Ease(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    const float inverse = 1.0f - t;
    return 1.0f - inverse * inverse * inverse * inverse;
}
void Artwork(const services::Game &game, D2D1_RECT_F r) {
    Round(r, 8, p.raised);
    if (Bitmap(game.artFile))
        Image(game.artFile, r, 8);
    else
        Glyph(Icon::Game, r, 15, p.secondary);
}
void Card(D2D1_RECT_F r, float radius = 13) {
    auto shadow = r;
    shadow.top += 2;
    shadow.bottom += 2;
    Round(shadow, radius, Color(0, Light() ? .025f : .12f));
    Round(r, radius, p.surface);
    Round(r, radius, p.border, true, .8f);
}
void Header(const std::wstring &title) {
    Text(title, Box(X(), 57, W(), 37), 23, p.text, 600);
}
void DrawChrome() {
    const bool sessionWindow = runner || runnerPreview;
    if (!sessionWindow) {
        Rect(Box(0, 0, Sidebar, height), p.sidebar);
        Line(Sidebar, 0, Sidebar, height, Mix(p.sidebar, p.border, .7f));
        Glow(Box(19, 17, 28, 28), p.accent, .24f, 15);
        Round(Box(19, 17, 28, 28), 9, Mix(p.raised, p.accent, .1f));
        Glyph(Icon::Bolt, Box(19, 17, 28, 28), 14, p.accent);
    } else {
        Glyph(Icon::Bolt, Box(16, 6, 22, 24), 11, p.accent);
        Text(L"Session", Box(43, 4, 160, 28), 10, p.secondary, 500);
    }
    for (int i = 0; i < 2; ++i) {
        int id = i == 0 ? 900 : 902;
        auto r = Box(width - 72 + i * 36.0f, 0, 36, 36);
        Round(Box(r.left + 4, 4, 28, 27), 7,
              i == 1 ? Mix(p.bg, Color(0xbb5260), Hover(id)) : Mix(p.bg, p.raised, Hover(id)));
        const auto c = i == 1 ? Mix(p.secondary, Color(0xffffff), Hover(id)) : p.secondary;
        if (i == 0)
            Line(r.left + 13, 17, r.left + 23, 17, c, 1.1f);
        else {
            Line(r.left + 14, 13, r.left + 22, 21, c, 1.1f);
            Line(r.left + 22, 13, r.left + 14, 21, c, 1.1f);
        }
        HitBox(id, r);
    }
    if (sessionWindow)
        return;
    const std::array<wchar_t, 4> icons{Icon::Grid, Icon::Search, Icon::Gear, 0xf0c3};
    // Draw hover backgrounds before the moving selection so they cannot erase its path.
    for (int i = 0; i < 4; ++i) {
        const float h = std::clamp(Hover(10 + i), 0.0f, 1.0f);
        if (static_cast<int>(page) != i && h > .002f)
            Round(Box(12, NavY(i), 42, 40), 11, Mix(p.sidebar, p.raised, h * .7f));
    }
    Glow(Box(12, nav.value, 42, 40), p.accent, .1f, 17);
    Round(Box(12, nav.value, 42, 40), 11, p.accentSoft);
    Round(Box(0, nav.value + 13, 2, 14), 1, p.accent);
    for (int i = 0; i < 4; ++i) {
        int id = 10 + i;
        const float y = NavY(i), h = std::clamp(Hover(id), 0.0f, 1.0f);
        const float proximity = std::clamp(1 - std::abs(nav.value - y) / 40, 0.0f, 1.0f);
        const auto color = Mix(Mix(p.muted, p.text, h), p.accent, proximity);
        D2D1::Matrix3x2F old;
        target->GetTransform(&old);
        target->SetTransform(D2D1::Matrix3x2F::Translation(0, -h * 1.5f) * old);
        Glyph(icons[i], Box(12, y, 42, 40), 15, color);
        target->SetTransform(old);
        HitBox(id, Box(12, y, 42, 40), PageNames[i].c_str());
    }
    Line(22, NavY(1) + 50, 44, NavY(1) + 50, p.border);
    auto r = Box(12, height - 57, 42, 40);
    Round(r, 11, Mix(p.sidebar, p.raised, Hover(700) * .7f));
    Text(std::wstring(1, 0xf09b), r, 19, Mix(p.muted, p.text, Hover(700)), 400, DWRITE_TEXT_ALIGNMENT_CENTER, true,
         true);
    HitBox(700, r, L"GitHub · @osk4rrv");
}
static std::wstring Time(double seconds, bool clock = false) {
    const auto n = static_cast<uint64_t>(std::max(0.0, seconds));
    if (clock)
        return std::to_wstring(n / 60) + L":" + (n % 60 < 10 ? L"0" : L"") + std::to_wstring(n % 60);
    if (n >= 3600)
        return std::to_wstring(n / 3600) + L"h " + std::to_wstring(n / 60 % 60) + L"m";
    return std::to_wstring(n / 60) + L"m";
}
void Stat(float x, float w, const std::wstring &title, const std::wstring &value, const std::wstring &note,
          wchar_t icon, D2D1_COLOR_F color) {
    Card(Box(x, 115, w, 108));
    Text(title, Box(x + 17, 126, w - 57, 23), 11.5f, p.secondary, 500);
    Glyph(icon, Box(x + w - 35, 127, 18, 22), 11, color);
    Text(value, Box(x + 17, 151, w - 32, 38), 28, p.text, 550);
    Text(note, Box(x + 17, 192, w - 34, 18), 9.5f, p.muted);
}
bool RowHovered(int uid) const {
    auto found = rowBounds.find(uid);
    const int first = 2000 + uid * 3;
    return (found != rowBounds.end() && Contains(found->second, mouse)) || keyboardFocus == first ||
           keyboardFocus == first + 1;
}
void RunningRow(const services::Session &session, D2D1_RECT_F r, double now) {
    rowBounds[session.uid] = r;
    const float h = std::clamp(rowHover[session.uid].value, 0.0f, 1.0f);
    const int first = 2000 + session.uid * 3;
    const auto accent = session.state == services::SessionState::Completed ? p.green : p.accent;
    Round(Box(r.left + 5, r.top + 2, r.right - r.left - 10, 59), 9, Mix(p.surface, p.raised, h * .55f));
    Artwork(session.game, Box(r.left + 17, r.top + 15, 33, 33));
    Text(session.game.name, Box(r.left + 64, r.top + 8, r.right - r.left - 260, 26), 12.5f, p.text, 500);
    HitBox(first + 2, Box(r.left + 17, r.top + 8, r.right - r.left - 270, 42),
           session.rpc ? L"Rich Presence info · Local session time" : L"Show session · Local session time");
    const auto status = SessionStatus(session.uid);
    if (!status.empty())
        Text((session.rpc ? L"RPC · " : L"") + status, Box(r.left + 64, r.top + 29, r.right - r.left - 290, 15), 8,
             p.muted);
    if (session.rpc && status.empty()) {
        Text(L"RPC", Box(r.left + 64, r.top + 32, 30, 19), 8.5f, p.muted, 600);
    }
    const float barX = r.left + (session.rpc ? 104 : 64), barW = std::max(30.0f, r.right - 235 - barX);
    const float barY = r.top + (status.empty() ? 42 : 49);
    Round(Box(barX, barY, barW, 2.5f), 1.25f, p.raised);
    const float progress = static_cast<float>(session.Elapsed(now) / services::SessionDuration);
    if (progress > 0)
        Round(Box(barX, barY, std::max(2.0f, barW * progress), 2.5f), 1.25f, accent);
    Text(Time(session.Elapsed(now), true), Box(r.right - 80 - h * 112, r.top + 12, 61, 37), 12, p.secondary, 500,
         DWRITE_TEXT_ALIGNMENT_TRAILING);
    auto info = Box(r.right - 121, r.top + 19, 44, 29), stop = Box(r.right - 69, r.top + 19, 51, 29);
    if (h > .01f) {
        Round(info, 7, Mix(p.surface, p.raised, h));
        Round(stop, 7, Mix(p.surface, Color(Light() ? 0xfbe8e9 : 0x35252a), h));
        auto label = p.secondary;
        label.a = h;
        Text(L"Info", info, 10.5f, label, 500, DWRITE_TEXT_ALIGNMENT_CENTER);
        auto danger = Color(Light() ? 0xa34955 : 0xd6979f, h);
        Text(L"Stop", stop, 10.5f, danger, 500, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    if (session.state == services::SessionState::Running) {
        HitBox(first, info, L"Open game and session info");
        HitBox(first + 1, stop, L"Stop this session");
    }
}
void Scrollbar(float top, float bottom, float x) {
    scrollTrack = scrollThumb = {};
    const float maximum = MaxScroll();
    if (maximum <= 0 || bottom <= top)
        return;
    const float length = bottom - top, thumb = std::max(30.0f, length * length / (length + maximum)),
                y = top + (length - thumb) * std::clamp(scroll.value / maximum, 0.0f, 1.0f);
    scrollTrack = Box(x - 6, top, 16, length);
    scrollThumb = Box(x - 6, y, 16, thumb);
    const bool pointerNearby = Contains(scrollTrack, mouse) || scrollDragging;
    Round(Box(x, y, pointerNearby ? 5.0f : 3.0f, thumb), 2.5f, Mix(p.border, p.muted, pointerNearby ? .9f : .45f));
}
void DragScroll(float y) {
    const float available = scrollTrack.bottom - scrollTrack.top - (scrollThumb.bottom - scrollThumb.top);
    if (available <= 0)
        return;
    scrollTarget = std::clamp((y - scrollGrab - scrollTrack.top) / available, 0.0f, 1.0f) * MaxScroll();
    scroll.value = scrollTarget;
    scroll.velocity = 0;
    tipVisible = false;
    sceneDirty = dirty = true;
}
void DrawDashboard() {
    Header(L"Dashboard");
    const float w = (W() - 20) / 3;
    Stat(X(), w, L"Completed sessions", std::to_wstring(sessions.completedRuns), L"15 minutes per session",
         Icon::CircleCheck, p.accent);
    Stat(X() + w + 10, w, L"Currently running", std::to_wstring(sessions.ActiveCount()), L"Started in this app",
         Icon::Play, p.green);
    Stat(X() + 2 * (w + 10), w, L"Saved time", Time(sessions.SavedSeconds()), L"From completed sessions",
         Icon::Clock, Color(0xceba92));
    auto panel = Box(X(), 242, W(), height - 266);
    Card(panel);
    Text(L"Currently running", Box(X() + 18, 251, W() - 36, 28), 13, p.text, 550);
    Line(X() + 18, 287, X() + W() - 18, 287, Mix(p.border, p.surface, .3f));
    rowBounds.clear();
    if (sessions.items.empty()) {
        const float middle = (291 + panel.bottom) / 2;
        Glyph(Icon::Game, Box(X() + W() / 2 - 17, middle - 48, 34, 34), 22, p.muted);
        Text(L"No active sessions", Box(X() + 20, middle - 8, W() - 40, 26), 12, p.secondary, 500,
             DWRITE_TEXT_ALIGNMENT_CENTER);
        Button(61, Box(X() + W() / 2 - 62, middle + 30, 124, 31), L"Select Quest", 0, false);
        return;
    }
    const float top = 292, bottom = height - 37;
    target->PushAxisAlignedClip(Box(X() + 1, top, W() - 2, bottom - top), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    float y = top - scroll.value;
    const double now = services::SteadyNow();
    for (const auto &session : sessions.items) {
        const float entering =
                        preferences.animations ? Ease(static_cast<float>((now - session.started) / .28)) : 1,
                    exiting = session.state == services::SessionState::Running
                                  ? 1
                                  : 1 - Ease(static_cast<float>((now - session.finished) / .28)),
                    opacity = entering * exiting;
        if (y + 66 > top && y < bottom && opacity > .005f) {
            const bool fade = opacity < .999f;
            if (fade) {
                target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
                target->PushLayer(D2D1::LayerParameters(Box(X(), top, W(), bottom - top), nullptr,
                                                        D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                        D2D1::Matrix3x2F::Identity(), opacity),
                                  nullptr);
            }
            RunningRow(session, Box(X(), y + (1 - entering) * 5, W(), 66), now);
            if (fade) {
                target->PopLayer();
                target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
            }
        }
        y += 66 * exiting;
    }
    target->PopAxisAlignedClip();
    Scrollbar(top + 3, bottom - 3, X() + W() - 8);
}
void InputText(D2D1_RECT_F r) {
    if (inputFocus != 40) {
        Text(query.empty() ? L"Search games…" : query, r, 12.5f, query.empty() ? p.muted : p.text);
        return;
    }
    ComPtr<IDWriteTextLayout> layout;
    Check(write->CreateTextLayout(query.c_str(), static_cast<UINT32>(query.size()), Format(12.5f, 400, false),
                                  4096, r.bottom - r.top, layout.GetAddressOf()));
    layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    float cx = 0, cy = 0, ax = 0, ay = 0;
    DWRITE_HIT_TEST_METRICS metrics{};
    layout->HitTestTextPosition(static_cast<UINT32>(caret), FALSE, &cx, &cy, &metrics);
    layout->HitTestTextPosition(static_cast<UINT32>(anchor), FALSE, &ax, &ay, &metrics);
    const float offset = std::max(0.0f, cx - (r.right - r.left) + 8), middle = (r.top + r.bottom) / 2;
    target->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    if (caret != anchor)
        Round(Box(r.left + std::min(cx, ax) - offset, middle - 10, std::abs(cx - ax), 20), 3,
              Mix(p.accentSoft, p.accent, .18f));
    if (query.empty())
        Text(L"Search games…", r, 12.5f, p.muted);
    else {
        Ink(p.text);
        target->DrawTextLayout({r.left - offset, r.top}, layout.Get(), brush.Get());
    }
    if (caretOn)
        Line(r.left + cx - offset + .5f, middle - 8, r.left + cx - offset + .5f, middle + 8, p.accent, 1.1f);
    target->PopAxisAlignedClip();
}
void SearchField(float y) {
    auto r = Box(X(), y, W(), 38);
    Round(r, 10, p.surface);
    Round(r, 10, inputFocus == 40 ? Mix(p.border, p.accent, .6f) : p.border, true);
    if ((catalog.searching || queryPending) && preferences.animations) {
        D2D1::Matrix3x2F old;
        target->GetTransform(&old);
        target->SetTransform(
            D2D1::Matrix3x2F::Rotation(static_cast<float>(std::fmod(services::SteadyNow() * 220, 360)),
                                       {X() + 21.5f, y + 19}) *
            old);
        Glyph(0xf1ce, Box(X() + 13, y, 17, 38), 12, p.accent);
        target->SetTransform(old);
    } else
        Glyph(Icon::Search, Box(X() + 13, y, 17, 38), 12, p.muted);
    InputText(Box(X() + 40, y, W() - 80, 38));
    HitBox(40, r);
    if (!query.empty()) {
        Glyph(Icon::Close, Box(r.right - 31, y, 23, 38), 10, p.muted);
        HitBox(41, Box(r.right - 33, y + 5, 27, 28), L"Clear search");
    }
}
void DrawSelect() {
    Header(L"Select Quest");
    const float field = 115;
    SearchField(field);
    const bool busy = catalog.searching || queryPending;
    const auto filter = Box(X(), field + 45, 138, 25);
    Round(filter, 7, standardOnly ? p.accentSoft : p.surface);
    Text(standardOnly ? L"Standard available" : L"All games", filter, 10,
         standardOnly ? p.accent : p.secondary, 500, DWRITE_TEXT_ALIGNMENT_CENTER);
    if (!busy)
        HitBox(42, filter, L"Filter games with a usable Standard executable");
    const size_t total = catalog.totalGames ? catalog.totalGames : catalog.games.size();
    if (!busy && total) {
        const auto count = std::to_wstring(catalog.page * 100 + 1) + L"–" +
                           std::to_wstring(catalog.page * 100 + catalog.games.size()) + L" of " +
                           std::to_wstring(total);
        Text(count, Box(X() + 149, field + 45, W() - 215, 25), 9.5f, p.muted);
        if (catalog.page > 0) {
            Text(L"‹", Box(X() + W() - 53, field + 45, 23, 25), 18, p.text, 500,
                 DWRITE_TEXT_ALIGNMENT_CENTER);
            HitBox(43, Box(X() + W() - 53, field + 45, 23, 25), L"Previous 100 games");
        }
        if ((catalog.page + 1) * 100 < total) {
            Glyph(Icon::Right, Box(X() + W() - 24, field + 45, 23, 25), 10, p.secondary);
            HitBox(44, Box(X() + W() - 24, field + 45, 23, 25), L"Next 100 games");
        }
    }
    const bool showNotice = !catalog.notice.empty() && !catalog.searching && !queryPending;
    if (showNotice)
        Text(catalog.notice, Box(X() + 2, field + 75, W() - 4, 18), 9, p.muted);
    float top = field + (showNotice ? 98 : 86), bottom = height - 79;
    if (!catalog.searching && !queryPending && !catalog.games.empty()) {
        target->PushAxisAlignedClip(Box(X() - 1, top, W() + 2, bottom - top), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        for (size_t i = 0; i < catalog.games.size(); ++i) {
            const float y = top + static_cast<float>(i) * 58 - scroll.value;
            if (y + 51 <= top || y >= bottom)
                continue;
            const int id = GameControlBase + static_cast<int>(i);
            const float age = std::chrono::duration<float>(Clock::now() - resultsTime).count();
            const float reveal = preferences.animations ? Ease((age - std::min(i, size_t(6)) * .027f) / .28f) : 1;
            auto r = Box(X(), y + (1 - reveal) * 7, W(), 50);
            const bool fading = reveal < .999f;
            if (fading) {
                target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
                target->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr,
                                                        D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                        D2D1::Matrix3x2F::Identity(), reveal),
                                  nullptr);
            }
            const float selection =
                selected < 0
                    ? 0
                    : std::clamp(1 - std::abs(selectionSlide.value - static_cast<float>(i) * 58) / 58, 0.0f, 1.0f);
            Round(r, 10, Mix(Mix(p.surface, p.raised, Hover(id) * .5f), p.accentSoft, selection));
            Round(r, 10, Mix(p.border, p.accent, selection * .5f), true, .8f);
            Artwork(catalog.games[i], Box(X() + 11, r.top + 9, 32, 32));
            Text(catalog.games[i].name, Box(X() + 56, r.top + 3, W() - 101, 43), 12.5f, p.text, 500);
            Glyph(selected == static_cast<int>(i) ? Icon::Check : Icon::Right,
                  Box(X() + W() - 31, r.top + 10, 19, 31), 10,
                  selected == static_cast<int>(i) ? p.accent : p.muted);
            auto clip = r;
            clip.top = std::max(top, r.top);
            clip.bottom = std::min(bottom, r.bottom);
            if (reveal > .5f)
                HitBox(id, clip);
            if (fading) {
                target->PopLayer();
                target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
            }
        }
        target->PopAxisAlignedClip();
        Scrollbar(top, bottom, X() + W() + 8);
    } else if (catalog.searching || queryPending) {
        const float phase =
            preferences.animations ? static_cast<float>((std::sin(services::SteadyNow() * 2.4) + 1) / 2) : .5f;
        const auto ink = Mix(p.raised, p.border, phase * .36f);
        for (int i = 0; i < 3 && top + i * 58 + 50 < bottom; ++i) {
            const float y = top + i * 58.0f;
            Round(Box(X(), y, W(), 50), 10, p.surface);
            Round(Box(X() + 11, y + 9, 32, 32), 8, ink);
            Round(Box(X() + 56, y + 20, 110 + i * 29.0f, 9), 4.5f, ink);
            Round(Box(X() + W() - 27, y + 21, 9, 9), 4.5f, ink);
        }
    } else {
        const std::wstring status = query.size() < 2                    ? L"Search for a game"
                                    : catalog.searching || queryPending ? L"Searching…"
                                    : !catalog.error.empty()            ? L"Could not load games"
                                                                        : L"No results";
        Text(status, Box(X(), top + 44, W(), 28), 12, p.muted, 400, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    if (selected >= 0 && selected < static_cast<int>(catalog.games.size())) {
        auto footer = Box(X(), height - 61, W() - 260, 31);
        target->PushAxisAlignedClip(footer, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        auto oldColor = p.secondary;
        oldColor.a = 1 - footerFade;
        auto newColor = p.secondary;
        newColor.a = footerFade;
        if (footerFade < 1 && !previousFooter.empty())
            Text(previousFooter, Box(footer.left, footer.top - footerFade * 9, footer.right - footer.left, 31),
                 11.5f, oldColor, 500);
        Text(catalog.games[selected].name,
             Box(footer.left, footer.top + (1 - footerFade) * 9, footer.right - footer.left, 31), 11.5f, newColor,
             500);
        target->PopAxisAlignedClip();
        const bool already = sessions.IsRunning(catalog.games[selected]);
        IconButton(63, Box(X() + W() - 230, height - 62, 33, 33), Icon::Info, L"Game info");
        auto mode = Box(X() + W() - 189, height - 62, 49, 33);
        Round(mode, 9, modeOpen ? p.accentSoft : Mix(p.raised, p.accentSoft, Hover(62)));
        Round(mode, 9, p.border, true);
        Glyph(rpcMode ? Icon::Bolt : Icon::Game, Box(mode.left + 5, mode.top, 24, 33), 12,
              rpcMode ? Color(0xd6aa8b) : p.secondary);
        Glyph(Icon::Down, Box(mode.right - 20, mode.top, 14, 33), 8, p.muted);
        HitBox(62, mode, rpcMode ? L"Mode: RPC + available game detection" : L"Mode: Standard");
        const auto start = Box(X() + W() - 132, height - 62, 132, 33);
        if (already || ModeAvailable(catalog.games[selected], rpcMode))
            Button(60, start, already ? L"Stop session" : L"Start session", already ? Icon::Pause : Icon::Play,
                   !already);
        else {
            Round(start, 9, p.raised);
            Glyph(0xf05e, Box(start.left + 13, start.top, 18, 33), 10, p.muted);
            Text(!rpcMode && catalog.games[selected].executableResolving ? L"Checking…" : L"Unavailable",
                 Box(start.left + 37, start.top, 85, 33), 11, p.muted, 500);
        }
    }
}
void DrawRunner() {
    const float margin = 24, inner = width - margin * 2;
    const bool complete = runnerState.status == services::RuntimeStatus::Complete;
    const bool active = complete || runnerState.status == services::RuntimeStatus::Active ||
                        runnerState.status == services::RuntimeStatus::ProcessRunning;
    const auto color = complete ? p.green : active ? p.accent : Color(0xd6aa8b);
    Glow(Box(width / 2 - 65, 48, 130, 65), color, .14f, 60);
    Artwork(runnerGame, Box(margin, 60, 52, 52));
    Text(runnerGame.name, Box(margin + 68, 60, inner - 68, 29), 19, p.text, 550);
    Text(runnerRpc ? L"RPC session" : L"Standard session", Box(margin + 68, 91, inner - 68, 18), 10, p.secondary);
    const float cardHeight = height - 193;
    Card(Box(margin, 133, inner, cardHeight), 14);
    Circle(margin + 19, 155, 3, color);
    Text(services::RuntimeLabel(runnerState.status), Box(margin + 31, 143, inner - 48, 24), 10.5f, p.secondary,
         500);
    if (runnerState.error)
        Text(L"Code " + std::to_wstring(runnerState.error), Box(width - 101, 144, 57, 22), 9, p.muted, 400,
             DWRITE_TEXT_ALIGNMENT_TRAILING);
    Text(Time(runnerState.elapsed, true), Box(margin + 19, 178, inner - 38, 50), 39, p.text, 550);
    Text(L"/ 15:00", Box(width - 117, 195, 69, 24), 12, p.muted, 500, DWRITE_TEXT_ALIGNMENT_TRAILING);
    const float barY = 241,
                progress =
                    std::clamp(runnerProgress.value / static_cast<float>(services::SessionDuration), 0.0f, 1.0f);
    Round(Box(margin + 19, barY, inner - 38, 4), 2, p.raised);
    if (progress > 0) {
        Glow(Box(margin + 19, barY, (inner - 38) * progress, 4), color, .13f, 9);
        Round(Box(margin + 19, barY, std::max(4.0f, (inner - 38) * progress), 4), 2, color);
    }
    Text(runnerRpc ? L"Local RPC time · Quest progress is shown in Discord."
                   : L"Local runtime · Quest progress is shown in Discord.",
         Box(margin + 19, 258, inner - 38, 20), 9, p.muted);
    Button(81, Box(margin, height - 45, 82, 32), L"Info", Icon::Info);
    Button(80, Box(width - margin - 138, height - 45, 138, 32), L"Stop session", Icon::Pause, true);
}
void Toggle(int id, float x, float y, bool on) {
    const float t =
        std::clamp(preferences.animations ? toggleAnimations[id].value : (on ? 1.0f : 0.0f), 0.0f, 1.0f);
    Round(Box(x, y, 36, 21), 10.5f, Mix(p.border, p.accent, t));
    Circle(x + 10.5f + 15 * t, y + 10.5f, 7.5f, Color(Light() ? 0xffffff : 0x25222e));
    HitBox(id, Box(x - 5, y - 9, 47, 40));
}
#include "appearance.inc"
void DrawOverlay() {
    DrawModeMenu();
    DrawColorPicker();
    if (keyboardFocus && keyboardFocus != 40)
        for (const auto &h : hits)
            if (h.id == keyboardFocus) {
                auto r = h.rect;
                r.left -= 2;
                r.right += 2;
                r.top -= 2;
                r.bottom += 2;
                Round(r, 9, p.accent, true, 1.1f);
                break;
            }
    if (!toast.empty()) {
        const float w = 236, x = X() + (W() - w) / 2, y = height - 70 + (1 - toastAlpha) * 7;
        auto c = p.raised;
        c.a = toastAlpha;
        Round(Box(x, y, w, 34), 10, c);
        auto text = p.text;
        text.a = toastAlpha;
        Text(toast, Box(x + 12, y, w - 24, 34), 11, text, 500, DWRITE_TEXT_ALIGNMENT_CENTER);
    } else if (tipVisible && !pressed && !scrollDragging)
        for (const auto &h : hits)
            if (h.id == hover && !h.tip.empty()) {
                const float w = std::clamp(static_cast<float>(h.tip.size()) * 6.2f + 20, 82.0f, 300.0f),
                            x = h.id >= 10 && h.id <= 14 ? Sidebar + 9
                                                         : std::clamp(mouse.x - w / 2, 8.0f, width - w - 8),
                            y = h.id >= 10 && h.id <= 14 ? h.rect.top + 6 : std::min(height - 37, mouse.y + 18);
                Card(Box(x, y, w, 28), 8);
                Text(h.tip, Box(x + 8, y, w - 16, 28), 10.5f, p.text, 400, DWRITE_TEXT_ALIGNMENT_CENTER);
                break;
            }
}
