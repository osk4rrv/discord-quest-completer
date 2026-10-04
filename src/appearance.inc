static D2D1_COLOR_F HSV(float h, float s, float v) {
    h = h - std::floor(h);
    const float sector = h * 6, fraction = sector - std::floor(sector);
    const float a = v * (1 - s), b = v * (1 - s * fraction), c = v * (1 - s * (1 - fraction));
    switch (static_cast<int>(sector) % 6) {
    case 0:
        return {v, c, a, 1};
    case 1:
        return {b, v, a, 1};
    case 2:
        return {a, v, c, 1};
    case 3:
        return {a, b, v, 1};
    case 4:
        return {c, a, v, 1};
    default:
        return {v, a, b, 1};
    }
}
static unsigned PackRgb(D2D1_COLOR_F c) {
    return (static_cast<unsigned>(std::lround(c.r * 255)) << 16) |
           (static_cast<unsigned>(std::lround(c.g * 255)) << 8) | static_cast<unsigned>(std::lround(c.b * 255));
}
unsigned &CustomColor() {
    return colorChannel == 0   ? preferences.customBackground
           : colorChannel == 1 ? preferences.customSurface
                               : preferences.customAccent;
}
const std::array<unsigned, 8> &ColorSamples() const {
    static const std::array<unsigned, 8> theme{0x151518, 0x19191d, 0x101722, 0x0e1a17,
                                               0x1e1520, 0x22222c, 0xf5f5f7, 0xe9edf3};
    static const std::array<unsigned, 8> accent{0xb7a0ed, 0x94b5dc, 0x8ac7b3, 0xd6a0bc,
                                                0xd6bb8e, 0xf0727a, 0x8accdf, 0xedf2f7};
    return colorChannel == 2 ? accent : theme;
}
void LoadColorPicker() {
    const auto c = Color(CustomColor());
    const float high = std::max({c.r, c.g, c.b}), low = std::min({c.r, c.g, c.b}), delta = high - low;
    colorValue = high;
    colorSaturation = high == 0 ? 0 : delta / high;
    colorHue = 0;
    if (delta > 0) {
        colorHue = c.r == high   ? (c.g - c.b) / delta
                   : c.g == high ? 2 + (c.b - c.r) / delta
                                 : 4 + (c.r - c.g) / delta;
        colorHue /= 6;
        if (colorHue < 0)
            colorHue += 1;
    }
}
void UpdateColorPicker(float x, float y) {
    if (colorDrag == 1) {
        colorSaturation =
            std::clamp((x - saturationRect.left) / (saturationRect.right - saturationRect.left), 0.0f, 1.0f);
        colorValue =
            1 - std::clamp((y - saturationRect.top) / (saturationRect.bottom - saturationRect.top), 0.0f, 1.0f);
    } else if (colorDrag == 2)
        colorHue = std::clamp((x - hueRect.left) / (hueRect.right - hueRect.left), 0.0f, .9999f);
    CustomColor() = PackRgb(HSV(colorHue, colorSaturation, colorValue));
    UpdateTheme();
    sceneDirty = dirty = true;
}
void DrawSettings() {
    Header(L"Appearance");
    const bool custom = preferences.preset == 4;
    const float themeHeight = custom ? 178.0f : 122.0f;
    Card(Box(X(), 115, W(), themeHeight));
    Text(L"Theme", Box(X() + 17, 122, W() - 70, 24), 12, p.text, 550);
    if (custom)
        IconButton(343, Box(X() + W() - 46, 123, 28, 24), Icon::Refresh, L"Reset theme colors");
    const float tileWidth = (W() - 36 - 36) / 5;
    const std::array<std::wstring, 5> names{L"Default", L"Graphite", L"Midnight", L"White", L"Custom"};
    const std::array<int, 5> order{0, 3, 1, 2, 4};
    const std::array<unsigned, 4> backgrounds{0x151518, 0x19191d, 0x101722, 0xf5f5f7};
    for (int slot = 0; slot < 5; ++slot) {
        const int i = order[slot];
        const auto r = Box(X() + 18 + slot * (tileWidth + 9), 152, tileWidth, 68);
        const bool active = preferences.preset == i;
        if (active)
            Glow(r, p.accent, .12f, 12);
        Round(r, 9, active ? p.accentSoft : Mix(p.raised, p.accentSoft, Hover(330 + i) * .45f));
        Round(r, 9, active ? Mix(p.border, p.accent, .55f) : p.border, true, .9f);
        if (i < 4) {
            const auto preview = Box(r.left + 8, r.top + 7, tileWidth - 16, 30);
            const auto base = Color(backgrounds[i]);
            Round(preview, 5, base);
            Round(Box(preview.left + 4, preview.top + 4, 9, 22), 2, Mix(base, Color(i == 3 ? 0 : 0xffffff), .08f));
            const auto ink = Mix(base, Color(i == 3 ? 0 : 0xffffff), .2f);
            Round(Box(preview.left + 18, preview.top + 5, preview.right - preview.left - 24, 4), 2, ink);
            Round(Box(preview.left + 18, preview.top + 13, (preview.right - preview.left - 29) / 2, 11), 3, ink);
            Round(Box(preview.left + 22 + (preview.right - preview.left - 29) / 2, preview.top + 13,
                      (preview.right - preview.left - 29) / 2, 11),
                  3, Mix(base, p.accent, .25f));
        } else {
            Glyph(0xf53f, Box(r.left, r.top + 4, tileWidth, 33), 19, p.accent);
        }
        Text(names[i], Box(r.left + 4, r.top + 40, tileWidth - 8, 22), 10.5f, active ? p.text : p.secondary, 500,
             DWRITE_TEXT_ALIGNMENT_CENTER);
        HitBox(330 + i, r);
    }
    if (custom) {
        const float fieldWidth = (W() - 46) / 2;
        const std::array<std::wstring, 2> labels{L"Background", L"Surface"};
        const std::array<unsigned, 2> colors{preferences.customBackground, preferences.customSurface};
        for (int i = 0; i < 2; ++i) {
            auto r = Box(X() + 18 + i * (fieldWidth + 10), 233, fieldWidth, 43);
            Round(r, 9, p.raised);
            Round(r, 9, p.border, true);
            Round(Box(r.left + 9, r.top + 10, 23, 23), 6, Color(colors[i]));
            Round(Box(r.left + 9, r.top + 10, 23, 23), 6, Mix(p.border, p.text, .2f), true);
            Text(labels[i], Box(r.left + 42, r.top, fieldWidth - 79, 43), 10.5f, p.secondary, 500);
            Glyph(0xf1fb, Box(r.right - 32, r.top, 24, 43), 11, p.muted);
            HitBox(340 + i, r);
        }
    }
    const float accentY = 115 + themeHeight + 14;
    Card(Box(X(), accentY, W(), 80));
    Text(L"Accent", Box(X() + 18, accentY + 26, W() - 300, 28), 12, p.text, 500);
    for (int i = 0; i < 5; ++i) {
        const float cx = X() + W() - 246 + i * 34.0f;
        Circle(cx, accentY + 40, 10, Color(Accents[i]));
        if (preferences.accent == i)
            Circle(cx, accentY + 40, 14, p.secondary, true, 1.1f);
        HitBox(320 + i, Box(cx - 15, accentY + 24, 30, 32));
    }
    const auto customAccent = Box(X() + W() - 86, accentY + 22, 67, 36);
    const bool customSelected = preferences.accent == 5;
    Round(customAccent, 9, customSelected ? p.accentSoft : p.raised);
    Round(customAccent, 9, customSelected ? p.accent : p.border, true);
    Circle(customAccent.left + 18, accentY + 40, 9, Color(preferences.customAccent));
    Glyph(0xf1fb, Box(customAccent.left + 35, customAccent.top, 24, 36), 12, p.secondary);
    HitBox(325, customAccent, L"Custom accent");
    const float motionY = accentY + 94;
    Card(Box(X(), motionY, W(), 66));
    Text(L"Motion", Box(X() + 18, motionY + 18, W() - 100, 30), 12, p.text, 500);
    Toggle(300, X() + W() - 55, motionY + 23, preferences.animations);
    Text(L"@osk4rrv", Box(X(), height - 43, W(), 24), 10, p.muted);
}
void DrawModeMenu() {
    const float amount = std::clamp(modeMenu.value, 0.0f, 1.0f);
    if (page != Page::SelectQuest || amount < .002f)
        return;
    modePanel = Box(X() + W() - 238, height - 223, 238, 149);
    const float shift = (1 - amount) * 6;
    target->SetTransform(D2D1::Matrix3x2F::Translation(0, shift));
    target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    target->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                            D2D1::Matrix3x2F::Identity(), amount),
                      nullptr);
    Glow(modePanel, Color(0), .42f, 24);
    Card(modePanel, 12);
    for (int i = 0; i < 2; ++i) {
        const int id = 64 + i;
        auto r = Box(modePanel.left + 7, modePanel.top + 7 + i * 36, 224, 33);
        Round(r, 7, Mix(p.surface, p.raised, Hover(id)));
        Glyph(i ? Icon::Bolt : Icon::Game, Box(r.left + 9, r.top, 20, 33), 11, i ? Color(0xd6aa8b) : p.secondary);
        Text(i ? L"RPC" : L"Standard", Box(r.left + 37, r.top, 100, 33), 11.5f, p.text, 500);
        if (i)
            Badge(Box(r.left + 105, r.top + 7, 65, 19), L"Dangerous", Color(0xd6aa8b),
                  Mix(p.surface, Color(0xd6aa8b), .08f));
        if (rpcMode == (i == 1))
            Glyph(Icon::Check, Box(r.right - 28, r.top, 20, 33), 10, p.accent);
        if (modeOpen) {
            r.top += shift;
            r.bottom += shift;
            HitBox(id, r);
        }
    }
    Line(modePanel.left + 14, modePanel.top + 83, modePanel.right - 14, modePanel.top + 83, p.border);
    const bool fc27 = selected >= 0 && selected < static_cast<int>(catalog.games.size()) &&
                      services::Fc27DetectionProfile(catalog.games[selected]);
    Text(fc27 ? L"FC27 folder setup is automatic." : L"RPC also starts game detection",
         Box(modePanel.left + 14, modePanel.top + 90, 210, 16), 9.5f, p.muted);
    Text(fc27 ? L"Uses Steam or a local folder." : L"when a Standard profile is available.",
         Box(modePanel.left + 14, modePanel.top + 106, 210, 16), 9.5f, p.muted);
    Text(L"Check Quest progress in Discord.", Box(modePanel.left + 14, modePanel.top + 122, 210, 16), 9.5f,
         p.muted);
    target->PopLayer();
    target->SetTransform(D2D1::Matrix3x2F::Identity());
    target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
}
void DrawColorPicker() {
    if (!colorOpen || page != Page::Settings)
        return;
    colorPanel = Box(X() + W() - 278, 149, 278, 293);
    Glow(colorPanel, Color(0), .5f, 24);
    Card(colorPanel, 13);
    const std::array<std::wstring, 3> labels{L"Background", L"Surface", L"Accent"};
    Text(labels[colorChannel], Box(colorPanel.left + 16, colorPanel.top + 10, 220, 28), 12, p.text, 550);
    IconButton(349, Box(colorPanel.right - 37, colorPanel.top + 11, 25, 25), Icon::Close, L"Close palette");
    saturationRect = Box(colorPanel.left + 16, colorPanel.top + 48, 246, 139);
    hueRect = Box(colorPanel.left + 16, colorPanel.top + 203, 246, 12);
    ComPtr<ID2D1GradientStopCollection> stops;
    ComPtr<ID2D1LinearGradientBrush> gradient;
    const D2D1_GRADIENT_STOP horizontal[]{{0, Color(0xffffff)}, {1, HSV(colorHue, 1, 1)}};
    Check(target->CreateGradientStopCollection(horizontal, 2, stops.GetAddressOf()));
    Check(target->CreateLinearGradientBrush(
        D2D1::LinearGradientBrushProperties({saturationRect.left, 0}, {saturationRect.right, 0}), stops.Get(),
        gradient.GetAddressOf()));
    target->FillRectangle(saturationRect, gradient.Get());
    stops.Reset();
    gradient.Reset();
    const D2D1_GRADIENT_STOP vertical[]{{0, Color(0, 0)}, {1, Color(0)}};
    Check(target->CreateGradientStopCollection(vertical, 2, stops.GetAddressOf()));
    Check(target->CreateLinearGradientBrush(
        D2D1::LinearGradientBrushProperties({0, saturationRect.top}, {0, saturationRect.bottom}), stops.Get(),
        gradient.GetAddressOf()));
    target->FillRectangle(saturationRect, gradient.Get());
    stops.Reset();
    gradient.Reset();
    D2D1_GRADIENT_STOP hueStops[7]{};
    for (int i = 0; i < 7; ++i)
        hueStops[i] = {i / 6.0f, HSV(i / 6.0f, 1, 1)};
    Check(target->CreateGradientStopCollection(hueStops, 7, stops.GetAddressOf()));
    Check(target->CreateLinearGradientBrush(
        D2D1::LinearGradientBrushProperties({hueRect.left, 0}, {hueRect.right, 0}), stops.Get(),
        gradient.GetAddressOf()));
    target->FillRoundedRectangle(D2D1::RoundedRect(hueRect, 6, 6), gradient.Get());
    const float sx = saturationRect.left + colorSaturation * (saturationRect.right - saturationRect.left),
                sy = saturationRect.top + (1 - colorValue) * (saturationRect.bottom - saturationRect.top),
                hx = hueRect.left + colorHue * (hueRect.right - hueRect.left);
    Circle(sx, sy, 5, Color(0xffffff), true, 1.8f);
    Circle(sx, sy, 6.5f, Color(0, .5f), true, 1);
    Circle(hx, (hueRect.top + hueRect.bottom) / 2, 7, Color(0xffffff), true, 2);
    wchar_t hex[9]{};
    swprintf_s(hex, L"#%06X", CustomColor());
    Round(Box(colorPanel.left + 17, colorPanel.top + 231, 23, 23), 6, Color(CustomColor()));
    Text(hex, Box(colorPanel.left + 51, colorPanel.top + 231, 200, 23), 12, p.secondary, 500);
    const auto &samples = ColorSamples();
    for (int i = 0; i < 8; ++i) {
        const float cx = colorPanel.left + 25 + i * 31.5f;
        Circle(cx, colorPanel.top + 274, 7, Color(samples[i]));
        HitBox(350 + i, Box(cx - 11, colorPanel.top + 263, 22, 22));
    }
    HitBox(360, saturationRect);
    HitBox(361, Box(hueRect.left, hueRect.top - 4, hueRect.right - hueRect.left, 20));
}
void DrawBeta() {
    const float cx = X() + W() / 2, cy = height / 2 + 22;
    Glow(Box(cx - 150, cy - 85, 300, 170), p.accent, .29f, 95);
    Glow(Box(cx + 65, cy - 85, 85, 85), Color(0x7ac7d5), .16f, 75);
    D2D1::Matrix3x2F old;
    target->GetTransform(&old);
    target->SetTransform(D2D1::Matrix3x2F::Rotation(-6, {cx, cy}) * old);
    const float w = std::min(460.0f, W() - 70);
    auto app = Box(cx - w / 2, cy - 113, w, 226);
    Round(app, 18, Mix(p.bg, p.raised, .42f));
    Round(app, 18, Mix(p.bg, p.border, .62f), true);
    for (int i = 0; i < 3; ++i)
        Circle(app.left + 18 + i * 12.0f, app.top + 17, 2.7f, Mix(p.bg, p.muted, .35f));
    Line(app.left + 12, app.top + 34, app.right - 12, app.top + 34, Mix(p.bg, p.border, .35f));
    Round(Box(app.left + 14, app.top + 49, 42, 163), 10, Mix(p.bg, p.raised, .36f));
    for (int i = 0; i < 4; ++i)
        Round(Box(app.left + 26, app.top + 67 + i * 31.0f, 18, 13), 4, Mix(p.bg, p.accent, i == 0 ? .22f : .08f));
    for (int i = 0; i < 3; ++i) {
        Round(Box(app.left + 72 + i * ((w - 88) / 3), app.top + 49, (w - 111) / 3, 47), 9,
              Mix(p.bg, p.raised, .55f));
        Round(Box(app.left + 85 + i * ((w - 88) / 3), app.top + 61, 27, 4), 2, Mix(p.bg, p.muted, .22f));
    }
    Round(Box(app.left + 72, app.top + 111, w - 88, 101), 12, Mix(p.bg, p.raised, .38f));
    target->SetTransform(old);
    Round(Box(cx - 135, cy - 34, 270, 68), 17, Mix(p.bg, p.surface, .85f));
    Round(Box(cx - 135, cy - 34, 270, 68), 17, Mix(p.bg, p.border, .8f), true);
    Glyph(0xf0c3, Box(cx - 130, cy - 20, 40, 40), 16, p.accent);
    Text(L"Coming Soon", Box(cx - 87, cy - 26, 199, 52), 23, p.text, 550, DWRITE_TEXT_ALIGNMENT_CENTER);
}
void DrawBetaMotion() {
    if (page != Page::Beta)
        return;
    const float cx = X() + W() / 2, cy = height / 2 + 22;
    const float time = preferences.animations ? static_cast<float>(services::SteadyNow() * .25) : 0;
    for (int i = 0; i < 3; ++i) {
        const float phase = time + i * 2.0944f, x = cx + std::cos(phase) * 242, y = cy + std::sin(phase) * 130;
        auto color = Mix(p.accent, Color(0x88caca), i / 3.0f);
        color.a = intro.value * .5f;
        Glow(Box(x - 3, y - 3, 6, 6), color, .15f * intro.value, 15);
        Circle(x, y, 1.5f, color);
    }
}
