void Ink(D2D1_COLOR_F c) {
    brush->SetColor(c);
}
void Rect(D2D1_RECT_F r, D2D1_COLOR_F c) {
    Ink(c);
    target->FillRectangle(r, brush.Get());
}
void Round(D2D1_RECT_F r, float radius, D2D1_COLOR_F c, bool outline = false, float thickness = 1) {
    Ink(c);
    if (outline)
        target->DrawRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush.Get(), thickness);
    else
        target->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush.Get());
}
void Line(float x1, float y1, float x2, float y2, D2D1_COLOR_F c, float thickness = 1) {
    Ink(c);
    target->DrawLine({x1, y1}, {x2, y2}, brush.Get(), thickness);
}
void Circle(float x, float y, float radius, D2D1_COLOR_F c, bool outline = false, float thickness = 1) {
    Ink(c);
    const auto e = D2D1::Ellipse({x, y}, radius, radius);
    if (outline)
        target->DrawEllipse(e, brush.Get(), thickness);
    else
        target->FillEllipse(e, brush.Get());
}
void Gradient(D2D1_RECT_F r, float radius, unsigned start, unsigned end) {
    const D2D1_GRADIENT_STOP stops[]{{0, Color(start)}, {1, Color(end)}};
    ComPtr<ID2D1GradientStopCollection> collection;
    Check(target->CreateGradientStopCollection(stops, 2, collection.GetAddressOf()));
    ComPtr<ID2D1LinearGradientBrush> gradient;
    Check(target->CreateLinearGradientBrush(
        D2D1::LinearGradientBrushProperties({r.left, r.top}, {r.right, r.bottom}), collection.Get(),
        gradient.GetAddressOf()));
    target->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), gradient.Get());
}
void Glow(D2D1_RECT_F r, D2D1_COLOR_F color, float strength = .12f, float spread = 18) {
    if (Light())
        strength *= .45f;
    const unsigned key = (static_cast<unsigned>(color.r * 255) << 16) |
                         (static_cast<unsigned>(color.g * 255) << 8) | static_cast<unsigned>(color.b * 255);
    if (glowBrushes.size() >= 64 && glowBrushes.find(key) == glowBrushes.end())
        glowBrushes.erase(glowBrushes.begin());
    auto &gradient = glowBrushes[key];
    if (!gradient) {
        const float positions[]{0, .18f, .4f, .68f, .85f, 1};
        const float alphas[]{1, .78f, .38f, .1f, .018f, 0};
        D2D1_GRADIENT_STOP stops[6]{};
        for (int i = 0; i < 6; ++i) {
            stops[i] = {positions[i], color};
            stops[i].color.a = alphas[i];
        }
        ComPtr<ID2D1GradientStopCollection> collection;
        Check(target->CreateGradientStopCollection(stops, 6, collection.GetAddressOf()));
        Check(target->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties({0, 0}, {0, 0}, 1, 1),
                                                collection.Get(), gradient.GetAddressOf()));
    }
    const auto center = D2D1::Point2F((r.left + r.right) / 2, (r.top + r.bottom) / 2);
    const float rx = (r.right - r.left) / 2 + spread, ry = (r.bottom - r.top) / 2 + spread;
    gradient->SetCenter(center);
    gradient->SetRadiusX(rx);
    gradient->SetRadiusY(ry);
    gradient->SetOpacity(strength);
    target->FillEllipse(D2D1::Ellipse(center, rx, ry), gradient.Get());
}
IDWriteTextFormat *Format(float size, int weight, bool icon, bool brand = false) {
    const int key = static_cast<int>(size * 10) + weight * 1000 + (icon ? 1000000 : 0) + (brand ? 2000000 : 0);
    auto found = formats.find(key);
    if (found != formats.end())
        return found->second.Get();
    ComPtr<IDWriteTextFormat> format;
    Check(write->CreateTextFormat(brand  ? brandFamily.c_str()
                                  : icon ? iconFamily.c_str()
                                         : L"Inter",
                                  fonts.Get(),
                                  static_cast<DWRITE_FONT_WEIGHT>(brand  ? 400
                                                                  : icon ? 900
                                                                         : weight),
                                  DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-US",
                                  format.GetAddressOf()));
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    return formats.emplace(key, std::move(format)).first->second.Get();
}
void Text(const std::wstring &s, D2D1_RECT_F r, float size, D2D1_COLOR_F c, int weight = 400,
          DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING, bool icon = false, bool brand = false) {
    if (preferences.preset == 4) {
        const auto point = D2D1::Point2F((r.left + r.right) / 2, (r.top + r.bottom) / 2);
        const bool onCard = ((runner || runnerPreview) && point.x >= 24 && point.x <= width - 24 &&
                             point.y >= 133 && point.y <= height - 60) ||
                            (colorOpen && Contains(colorPanel, point)) ||
                            (modeOpen && Contains(modePanel, point)) ||
                            (page == Page::Settings && point.x >= X() && point.x < X() + W() && point.y >= 115 &&
                             point.y <= 451) ||
                            (page == Page::Dashboard && point.x >= X() && point.x < X() + W() &&
                             ((point.y >= 115 && point.y <= 223) || (point.y >= 242 && point.y <= height - 24))) ||
                            (page == Page::SelectQuest && point.x >= X() && point.x < X() + W() &&
                             ((point.y >= 115 && point.y <= 153) || (point.y >= 169 && point.y <= height - 79) ||
                              (point.y >= height - 62 && point.y <= height - 29 && point.x >= X() + W() - 230)));
        if (onCard) {
            const auto same = [](D2D1_COLOR_F a, D2D1_COLOR_F b) {
                return std::abs(a.r - b.r) + std::abs(a.g - b.g) + std::abs(a.b - b.b) < .0001f;
            };
            const float luminance = p.surface.r * .2126f + p.surface.g * .7152f + p.surface.b * .0722f;
            const auto foreground = Color(luminance > .55f ? 0x24242c : 0xededf2);
            const float alpha = c.a;
            if (same(c, p.text))
                c = foreground;
            else if (same(c, p.secondary))
                c = Mix(p.surface, foreground, .72f);
            else if (same(c, p.muted))
                c = Mix(p.surface, foreground, .58f);
            c.a = alpha;
        }
    }
    const auto cacheKey = s + L"|" + std::to_wstring(size) + L"|" + std::to_wstring(weight) + L"|" +
                          std::to_wstring(std::lround((r.right - r.left) * 100)) + L"|" +
                          std::to_wstring(std::lround((r.bottom - r.top) * 100)) + L"|" + std::to_wstring(align) +
                          (brand  ? L"B"
                           : icon ? L"I"
                                  : L"T");
    auto found = layouts.find(cacheKey);
    if (found == layouts.end()) {
        if (layouts.size() >= 512)
            layouts.erase(layouts.begin());
        ComPtr<IDWriteTextLayout> layout;
        Check(write->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), Format(size, weight, icon, brand),
                                      std::max(1.0f, r.right - r.left), std::max(1.0f, r.bottom - r.top),
                                      layout.GetAddressOf()));
        layout->SetTextAlignment(align);
        layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        if (!icon && !s.empty())
            layout->SetTypography(typography.Get(), {0, static_cast<UINT32>(s.size())});
        DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        ComPtr<IDWriteInlineObject> ellipsis;
        if (SUCCEEDED(
                write->CreateEllipsisTrimmingSign(Format(size, weight, icon, brand), ellipsis.GetAddressOf())))
            layout->SetTrimming(&trim, ellipsis.Get());
        found = layouts.emplace(cacheKey, std::move(layout)).first;
    }
    Ink(c);
    target->DrawTextLayout({r.left, r.top}, found->second.Get(), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}
void Glyph(wchar_t code, D2D1_RECT_F r, float size, D2D1_COLOR_F c) {
    Text(std::wstring(1, code), r, size, c, 900, DWRITE_TEXT_ALIGNMENT_CENTER, true);
}
float Hover(int id) {
    return hoverAnimations[id].value;
}
void HitBox(int id, D2D1_RECT_F r, const wchar_t *tip = L"") {
    hits.push_back({id, r, tip});
}
void Button(int id, D2D1_RECT_F r, const std::wstring &label, wchar_t icon = 0, bool primary = false) {
    const float h = Hover(id);
    const auto fill = primary ? Mix(p.accent, Color(Light() ? 0x8d7be8 : 0xc5b3ff), h)
                              : Mix(p.raised, Mix(p.raised, p.text, .06f), h);
    const float accentLuminance = p.accent.r * .2126f + p.accent.g * .7152f + p.accent.b * .0722f;
    const auto fg = primary ? Color(accentLuminance > .55f ? 0x211a34 : 0xffffff) : p.text;
    D2D1::Matrix3x2F old;
    target->GetTransform(&old);
    const float press = preferences.animations ? pressAnimations[id].value : (pressed == id ? 1.0f : 0.0f);
    if (press > .001f)
        target->SetTransform(D2D1::Matrix3x2F::Scale(1 - .018f * press, 1 - .018f * press,
                                                     {(r.left + r.right) / 2, (r.top + r.bottom) / 2}) *
                             old);
    if (primary)
        Glow(r, p.accent, .15f + h * .05f, 15);
    Round(r, 9, fill);
    if (!primary)
        Round(r, 9, p.border, true);
    if (icon)
        Glyph(icon, Box(r.left + 12, r.top, 18, r.bottom - r.top), 11, fg);
    Text(label,
         Box(r.left + (icon ? 38.0f : 10.0f), r.top, r.right - r.left - (icon ? 48.0f : 20.0f), r.bottom - r.top),
         11.5f, fg, 550, icon ? DWRITE_TEXT_ALIGNMENT_LEADING : DWRITE_TEXT_ALIGNMENT_CENTER);
    target->SetTransform(old);
    HitBox(id, r);
}
void IconButton(int id, D2D1_RECT_F r, wchar_t icon, const wchar_t *tip, bool accent = false) {
    Round(r, 9, Mix(accent ? p.accentSoft : p.raised, p.accentSoft, Hover(id)));
    Glyph(icon, r, 12, accent ? p.accent : Mix(p.secondary, p.text, Hover(id)));
    HitBox(id, r, tip);
}
void Badge(D2D1_RECT_F r, const std::wstring &label, D2D1_COLOR_F fg, D2D1_COLOR_F bg, bool dot = false) {
    Round(r, static_cast<float>(r.bottom - r.top) / 2, bg);
    if (dot)
        Circle(r.left + 12, (r.top + r.bottom) / 2, 2.5f, fg);
    Text(label,
         Box(r.left + (dot ? 22.0f : 6.0f), r.top, r.right - r.left - (dot ? 29.0f : 12.0f), r.bottom - r.top), 10,
         fg, 500, dot ? DWRITE_TEXT_ALIGNMENT_LEADING : DWRITE_TEXT_ALIGNMENT_CENTER);
}
