#include <windows.h>
#include <windowsx.h>
#include <d2d1.h>
#include <dwrite_3.h>
#include <dwmapi.h>
#include <wincodec.h>
#include <shellapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
constexpr float Sidebar = 66.0f;
constexpr wchar_t WindowClass[] = L"QuestCompleter.Native";

void Check(HRESULT hr) {
    if (FAILED(hr))
        throw std::runtime_error("Windows graphics error: " + std::to_string(static_cast<unsigned long>(hr)));
}

D2D1_RECT_F Box(float x, float y, float w, float h) {
    return D2D1::RectF(x, y, x + w, y + h);
}
bool Contains(D2D1_RECT_F r, D2D1_POINT_2F p) {
    return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom;
}
D2D1_COLOR_F Color(unsigned rgb, float alpha = 1) {
    return D2D1::ColorF(static_cast<float>((rgb >> 16) & 255) / 255, static_cast<float>((rgb >> 8) & 255) / 255,
                        static_cast<float>(rgb & 255) / 255, alpha);
}
D2D1_COLOR_F Mix(D2D1_COLOR_F a, D2D1_COLOR_F b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}

namespace Icon {
constexpr wchar_t Bolt = 0xf0e7, Grid = 0xf009, Flag = 0xf11e, Gift = 0xf06b;
constexpr wchar_t Gear = 0xf013, Search = 0xf002, Bell = 0xf0f3, Arrow = 0xf061;
constexpr wchar_t Play = 0xf04b, Pause = 0xf04c, Check = 0xf00c, Clock = 0xf017;
constexpr wchar_t Game = 0xf11b, Screen = 0xf26c, Film = 0xf008, Trophy = 0xf091;
constexpr wchar_t Close = 0xf00d, Down = 0xf078, Right = 0xf054, Star = 0xf005;
constexpr wchar_t Link = 0xf0c1, Refresh = 0xf2f1, Moon = 0xf186, Sun = 0xf185;
constexpr wchar_t Info = 0xf05a, Spark = 0xf0d0, CircleCheck = 0xf058;
} // namespace Icon

struct Palette {
    D2D1_COLOR_F bg, sidebar, surface, raised, border, text, secondary, muted, accent, accentSoft, green;
    static Palette Make(bool light) {
        if (light)
            return {Color(0xf5f5f7), Color(0xededf1), Color(0xffffff), Color(0xf0f0f4),
                    Color(0xe4e4ea), Color(0x24242c), Color(0x70707c), Color(0x8c8c96),
                    Color(0x7865dc), Color(0xeeeafa), Color(0x299775)};
        return {Color(0x151518), Color(0x111114), Color(0x1c1c21), Color(0x25252c),
                Color(0x303038), Color(0xededf2), Color(0xa2a2ae), Color(0x7c7c88),
                Color(0xb6a1fc), Color(0x292336), Color(0x74d6ac)};
    }
};
