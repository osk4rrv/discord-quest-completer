#include <d2d1_1.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include "base.inc"
#include "services.hpp"
#include <memory>

enum class Page { Dashboard, SelectQuest, Settings, Beta };
const std::array<std::wstring, 4> PageNames{L"Dashboard", L"Select Quest", L"Appearance", L"Beta"};
const std::array<unsigned, 5> Accents{0xb0a0da, 0x94b5dc, 0x8ac7b3, 0xd6a0bc, 0xd6bb8e};
constexpr int GameControlBase = 100000;
struct Spring {
    float value = 0, velocity = 0;
    bool Step(float target, float dt, bool animate, float frequency = 28, float damping = .86f) {
        if (!animate) {
            bool c = value != target;
            value = target;
            velocity = 0;
            return c;
        }
        if (std::abs(target - value) < .001f && std::abs(velocity) < .001f) {
            value = target;
            velocity = 0;
            return false;
        }
        const float d = value - target;
        if (damping >= .999f) {
            const float b = velocity + frequency * d, e = std::exp(-frequency * dt);
            value = target + (d + b * dt) * e;
            velocity = (velocity - frequency * b * dt) * e;
            return true;
        }
        const float decay = damping * frequency, w = frequency * std::sqrt(1 - damping * damping),
                    e = std::exp(-decay * dt), b = (velocity + decay * d) / w, sn = std::sin(w * dt),
                    cs = std::cos(w * dt);
        value = target + e * (d * cs + b * sn);
        velocity = e * (w * (-d * sn + b * cs) - decay * (d * cs + b * sn));
        return true;
    }
};
struct Hit {
    int id;
    D2D1_RECT_F rect;
    std::wstring tip;
};
class App {
  public:
    HWND hwnd = nullptr;
    Page page = Page::Dashboard;
    services::Preferences preferences;
    services::Snapshot catalog;
    services::SessionBook sessions;
    std::unique_ptr<services::RunnerRuntime> runner;
    services::Game runnerGame;
    bool runnerRpc = false, runnerPreview = false;
    services::RuntimeSnapshot runnerState;
    bool runtimeEnabled = true;
    std::wstring query;
    bool standardOnly = false;
    float width = 760, height = 520, dpi = 96;
    bool sizing = false;
    bool dirty = true, animating = false, persistent = true;
    services::fs::path data;
    App(bool save = true, services::fs::path root = {})
        : persistent(save),
          data(root.empty() ? (save ? services::DataDirectory() : services::DataDirectory() / L"qa")
                            : std::move(root)) {
        if (persistent) {
            services::MigrateLegacyData(data);
            preferences.Load(data);
        }
        services::InstallAssets(data);
        sessions.completedRuns = preferences.completedRuns;
        UpdateTheme();
        Check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf()));
        Check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                               IID_PPV_ARGS(wic.GetAddressOf())));
        Check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory5),
                                  reinterpret_cast<IUnknown **>(write.GetAddressOf())));
        ComPtr<IDWriteFontSetBuilder1> builder;
        Check(write->CreateFontSetBuilder(builder.GetAddressOf()));
        for (const auto *name : {L"Inter.ttf", L"fa-solid-900.ttf", L"fa-brands-400.ttf"}) {
            ComPtr<IDWriteFontFile> f;
            Check(write->CreateFontFileReference((data / L"fonts" / name).c_str(), nullptr, f.GetAddressOf()));
            Check(builder->AddFontFile(f.Get()));
        }
        ComPtr<IDWriteFontSet> set;
        Check(builder->CreateFontSet(set.GetAddressOf()));
        Check(write->CreateFontCollectionFromFontSet(set.Get(), fonts.GetAddressOf()));
        for (UINT32 i = 0; i < fonts->GetFontFamilyCount(); ++i) {
            ComPtr<IDWriteFontFamily> f;
            ComPtr<IDWriteLocalizedStrings> names;
            Check(fonts->GetFontFamily(i, f.GetAddressOf()));
            Check(f->GetFamilyNames(names.GetAddressOf()));
            UINT32 n = 0;
            Check(names->GetStringLength(0, &n));
            std::wstring name(n + 1, 0);
            Check(names->GetString(0, name.data(), n + 1));
            name.resize(n);
            if (name.find(L"Brands") != std::wstring::npos)
                brandFamily = name;
            else if (name.find(L"Font Awesome") != std::wstring::npos)
                iconFamily = name;
        }
        if (iconFamily.empty() || brandFamily.empty())
            throw std::runtime_error("FontAwesome could not be loaded.");
        Check(write->CreateTypography(typography.GetAddressOf()));
        Check(typography->AddFontFeature({DWRITE_FONT_FEATURE_TAG_TABULAR_FIGURES, 1}));
        nav.value = NavY(0);
        intro.value = 1;
        toggleAnimations[300].value = preferences.animations ? 1.0f : 0.0f;
    }
    ~App() {
        try {
            Save();
        } catch (...) {
        }
        worker.reset();
        for (auto &[id, run] : liveRuns) {
            if (reaper && run)
                reaper->Retire(std::move(run));
        }
        liveRuns.clear();
        reaper.reset();
        runner.reset();
        DropGpu();
        layouts.clear();
        formats.clear();
        fonts.Reset();
    }
    bool Light() const {
        if (preferences.preset == 4) {
            const auto c = Color(preferences.customBackground);
            return c.r * .2126f + c.g * .7152f + c.b * .0722f > .55f;
        }
        return preferences.preset == 3;
    }
    void Save() {
        preferences.completedRuns = sessions.completedRuns;
        if (persistent) {
            if (worker)
                worker->Persist(preferences.Serialize());
            else
                preferences.SaveTo(data);
        }
    }
    void StartServices() {
        worker = std::make_unique<services::Worker>(data, hwnd);
        if (!runner)
            reaper = std::make_unique<services::RunReaper>();
        if (!runner)
            worker->Search(query);
    }
    HANDLE FrameHandle() const {
        return latency;
    }
    DWORD IdleWaitMs() const {
        if (queryPending || inputFocus || !toast.empty() || (hover && !tipVisible))
            return 40;
        return runner || sessions.ActiveCount() ? 100 : INFINITE;
    }
    void Mark() {
        dirty = true;
    }
    void ProcessUpdate() {
        if (!worker)
            return;
        const auto previousGeneration = catalog.generation;
        const bool wasSearching = catalog.searching;
        catalog = worker->Take();
        if ((!catalog.searching && wasSearching) || previousGeneration != catalog.generation) {
            resultsTime = Clock::now();
            selected = -1;
            selectionSlide = {};
        }
        if (catalog.reportSerial != seenReport && !catalog.report.empty()) {
            seenReport = catalog.reportSerial;
            ShellExecuteW(hwnd, L"open", catalog.report.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        sceneDirty = dirty = true;
    }
    void Tick() {
        const auto now = Clock::now();
        const float dt = std::min(.05f, std::chrono::duration<float>(now - lastTick).count());
        lastTick = now;
        changedSearch = false;
        if (page == Page::SelectQuest && !runner && !runnerPreview) {
            const float age = std::chrono::duration<float>(now - resultsTime).count();
            changedSearch = ((catalog.searching || queryPending) && preferences.animations) ||
                            (!catalog.games.empty() && age < .48f && preferences.animations);
            const bool slide =
                selected >= 0 && selectionSlide.Step(selected * 58.0f, dt, preferences.animations, 27, 1);
            const float footerAge = std::chrono::duration<float>(now - footerTime).count();
            footerFade = preferences.animations ? Ease(footerAge / .24f) : 1;
            sceneDirty |= changedSearch || slide || footerFade < 1;
            dirty |= changedSearch || slide || footerFade < 1;
        }
        bool smoothRuntime = false;
        if (runner) {
            const auto state = runner->Snapshot();
            if (state != runnerState) {
                runnerState = state;
                sceneDirty = dirty = true;
            }
            if (runner->Closing() && hwnd)
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
            smoothRuntime =
                runnerProgress.Step(static_cast<float>(runnerState.elapsed), dt, preferences.animations, 20, 1);
            sceneDirty |= smoothRuntime;
        }
        animating = nav.Step(NavY(static_cast<int>(page)), dt, preferences.animations, 26, 1);
        animating |=
            smoothRuntime || (page == Page::SelectQuest &&
                              (changedSearch || footerFade < 1 ||
                               (selected >= 0 && std::abs(selectionSlide.value - selected * 58.0f) > .001f)));
        const float transition = static_cast<float>(std::chrono::duration<double>(now - pageTime).count() / .32);
        intro.value = preferences.animations ? Ease(transition) : 1;
        animating |= intro.value < 1;
        animating |= modeMenu.Step(modeOpen ? 1.0f : 0.0f, dt, preferences.animations, 30, .92f);
        animating |= page == Page::Beta && preferences.animations;
        if (intro.value == 1)
            transitionImage.Reset();
        bool changed = scroll.Step(scrollTarget, dt, preferences.animations, 24, .98f);
        sceneDirty |= changed;
        animating |= changed;
        for (auto &[id, s] : hoverAnimations) {
            changed = s.Step(id == hover ? 1.0f : 0.0f, dt, preferences.animations, 26, .98f);
            sceneDirty |= changed && ContentControl(id);
            animating |= changed;
        }
        for (auto &[id, s] : pressAnimations) {
            changed = s.Step(id == pressed ? 1.0f : 0.0f, dt, preferences.animations, 38, .9f);
            sceneDirty |= changed && ContentControl(id);
            animating |= changed;
        }
        changed = toggleAnimations[300].Step(preferences.animations ? 1.0f : 0.0f, dt, preferences.animations);
        sceneDirty |= changed;
        animating |= changed;
        const double sessionNow = services::SteadyNow();
        for (auto &session : sessions.items) {
            const auto found = liveRuns.find(session.uid);
            if (found == liveRuns.end())
                continue;
            const auto state = found->second->Snapshot();
            session.runtimeElapsed = state.elapsed;
            if (state.status == services::RuntimeStatus::Stopped)
                sessions.Stop(session.uid, sessionNow);
            if (state.status == services::RuntimeStatus::LaunchFailed) {
                sessions.Stop(session.uid, sessionNow);
                Notify(L"Could not start session");
            }
        }
        if (sessions.Update(sessionNow)) {
            sceneDirty = true;
            Save();
            Notify(L"Session complete");
        }
        for (auto it = liveRuns.begin(); it != liveRuns.end();) {
            const auto session = std::find_if(sessions.items.begin(), sessions.items.end(),
                                              [&](const services::Session &s) { return s.uid == it->first; });
            if (session == sessions.items.end() || session->state != services::SessionState::Running) {
                if (reaper)
                    reaper->Retire(std::move(it->second));
                it = liveRuns.erase(it);
                Save();
                sceneDirty = dirty = true;
            } else
                ++it;
        }
        for (const auto &s : sessions.items) {
            changed = page == Page::Dashboard && preferences.animations &&
                      (sessionNow - s.started < .28 || s.state != services::SessionState::Running);
            const bool active = page == Page::Dashboard && RowHovered(s.uid);
            changed |= rowHover[s.uid].Step(active ? 1.0f : 0.0f, dt, preferences.animations, 26, .98f);
            sceneDirty |= changed;
            animating |= changed;
        }
        changed = sessions.Prune(sessionNow, preferences.animations ? .28 : 0);
        sceneDirty |= changed;
        dirty |= changed;
        if (queryPending && now - searchChanged > std::chrono::milliseconds(350)) {
            queryPending = false;
            selected = -1;
            scroll = {};
            scrollTarget = 0;
            if (worker) {
                worker->Search(query, standardOnly);
                catalog = worker->Take();
            }
            sceneDirty = dirty = true;
        }
        if (hover && !tipVisible && now - hoverTime > std::chrono::milliseconds(900)) {
            tipVisible = true;
            dirty = true;
        }
        if (now - lastSecond > std::chrono::seconds(1)) {
            if (sessions.ActiveCount()) {
                sceneDirty = dirty = true;
            }
            lastSecond = now;
        }
        const bool blink =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - editTime).count() % 1000 < 500;
        if (inputFocus && blink != caretOn) {
            caretOn = blink;
            sceneDirty = dirty = true;
        }
        if (!toast.empty()) {
            float age = std::chrono::duration<float>(now - toastTime).count();
            toastAlpha = age < .2f ? Ease(age / .2f) : age > 2 ? 1 - Ease((age - 2) / .2f) : 1;
            animating |= preferences.animations && (age < .2f || age > 2);
            if (age > 2.2f) {
                toast.clear();
                dirty = true;
            }
        }
        ClampScroll();
        dirty |= animating;
    }
    void Paint(bool present = true) {
        if (!hwnd)
            return;
        if (!swap)
            CreateGpu();
        if (!ResizeGpu())
            return;
        target = context.Get();
        PrepareBrush();
        SetPalette();
        if (!PreparePage())
            return;
        target->BeginDraw();
        Render();
        const HRESULT result = target->EndDraw();
        if (result == D2DERR_RECREATE_TARGET) {
            DropGpu();
            dirty = true;
            return;
        }
        Check(result);
        if (present) {
            const HRESULT hr = swap->Present(1, 0);
            if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
                DropGpu();
                dirty = true;
                return;
            }
            Check(hr);
        }
        dirty = false;
    }
    void Resize() {
        if (!hwnd)
            return;
        RECT r;
        GetClientRect(hwnd, &r);
        if (r.right <= 0 || r.bottom <= 0)
            return;
        width = static_cast<float>(r.right) * 96 / dpi;
        height = static_cast<float>(r.bottom) * 96 / dpi;
        ApplyWindowCorners();
        bufferResizePending = true;
        transitionImage.Reset();
        intro = {1, 0};
        pageTime = Clock::now() - std::chrono::seconds(1);
        modeOpen = colorOpen = false;
        modeMenu.value = 0;
        ClampScroll();
        sceneDirty = dirty = true;
    }
    bool ResizeGpu() {
        if (!bufferResizePending)
            return true;
        RECT r{};
        GetClientRect(hwnd, &r);
        if (r.right <= 0 || r.bottom <= 0)
            return false;
        pageImage.Reset();
        transitionImage.Reset();
        context->SetTarget(nullptr);
        context->Flush();
        backBuffer.Reset();
        gpuContext->ClearState();
        gpuContext->Flush();
        const HRESULT hr = swap->ResizeBuffers(0, r.right, r.bottom, DXGI_FORMAT_UNKNOWN,
                                               DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
        if (FAILED(hr)) {
            DropGpu();
            CreateGpu();
        } else
            BindBuffer();
        bufferResizePending = false;
        sceneDirty = true;
        return true;
    }
    void ConfigureWindowCorners() {
        DWORD corners = 2;
        legacyCorners = FAILED(DwmSetWindowAttribute(hwnd, 33, &corners, sizeof(corners)));
        ApplyWindowCorners();
    }
    void ApplyWindowCorners() {
        if (!legacyCorners || !hwnd)
            return;
        if (IsZoomed(hwnd)) {
            SetWindowRgn(hwnd, nullptr, TRUE);
            return;
        }
        RECT rect{};
        GetWindowRect(hwnd, &rect);
        const int diameter = static_cast<int>(24 * dpi / 96);
        HRGN shape =
            CreateRoundRectRgn(0, 0, rect.right - rect.left + 1, rect.bottom - rect.top + 1, diameter, diameter);
        if (shape && !SetWindowRgn(hwnd, shape, TRUE))
            DeleteObject(shape);
    }
    void Snapshot(const wchar_t *path, float scale = 1, bool brand = false) {
        nav.value = NavY(static_cast<int>(page));
        intro.value = 1;
        footerFade = 1;
        resultsTime = Clock::now() - std::chrono::seconds(1);
        if (selected >= 0)
            selectionSlide = {static_cast<float>(selected) * 58.0f, 0};
        const UINT w = brand ? 64 : static_cast<UINT>(width * scale),
                   h = brand ? 64 : static_cast<UINT>(height * scale);
        ComPtr<IWICBitmap> bitmap;
        Check(wic->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, bitmap.GetAddressOf()));
        ComPtr<ID2D1RenderTarget> offscreen;
        Check(factory->CreateWicBitmapRenderTarget(
            bitmap.Get(),
            D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)),
            offscreen.GetAddressOf()));
        target = offscreen.Get();
        target->SetDpi(96 * scale, 96 * scale);
        PrepareBrush();
        target->BeginDraw();
        if (brand) {
            target->Clear(Color(0, 0));
            Gradient(Box(0, 0, 64, 64), 17, 0xc5b2ff, 0x9c82e5);
            Glyph(Icon::Bolt, Box(0, 0, 64, 64), 33, Color(0x282036));
        } else
            Render();
        Check(target->EndDraw());
        EncodeSnapshot(bitmap.Get(), path, w, h);
        brush.Reset();
        bitmaps.clear();
        imageLayer.Reset();
        target = nullptr;
    }
    void NativeSnapshot(const wchar_t *path) {
        nav = {NavY(static_cast<int>(page)), 0};
        intro = {1, 0};
        footerFade = 1;
        resultsTime = Clock::now() - std::chrono::seconds(1);
        if (selected >= 0)
            selectionSlide = {static_cast<float>(selected) * 58.0f, 0};
        transitionImage.Reset();
        Paint(false);
        const auto size = backBuffer->GetPixelSize();
        const auto properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), dpi, dpi);
        ComPtr<ID2D1Bitmap1> pixels;
        Check(context->CreateBitmap(size, nullptr, 0, &properties, pixels.GetAddressOf()));
        Check(pixels->CopyFromBitmap(nullptr, backBuffer.Get(), nullptr));
        D2D1_MAPPED_RECT mapped{};
        Check(pixels->Map(D2D1_MAP_OPTIONS_READ, &mapped));
        ComPtr<IWICBitmap> bitmap;
        const HRESULT result =
            wic->CreateBitmapFromMemory(size.width, size.height, GUID_WICPixelFormat32bppBGRA, mapped.pitch,
                                        mapped.pitch * size.height, mapped.bits, bitmap.GetAddressOf());
        pixels->Unmap();
        Check(result);
        EncodeSnapshot(bitmap.Get(), path, size.width, size.height);
    }
    void EncodeSnapshot(IWICBitmapSource *bitmap, const wchar_t *path, UINT w, UINT h) {
        ComPtr<IWICStream> stream;
        Check(wic->CreateStream(stream.GetAddressOf()));
        Check(stream->InitializeFromFilename(path, GENERIC_WRITE));
        ComPtr<IWICBitmapEncoder> encoder;
        Check(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.GetAddressOf()));
        Check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache));
        ComPtr<IWICBitmapFrameEncode> frame;
        Check(encoder->CreateNewFrame(frame.GetAddressOf(), nullptr));
        Check(frame->Initialize(nullptr));
        Check(frame->SetSize(w, h));
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppPBGRA;
        Check(frame->SetPixelFormat(&format));
        Check(frame->WriteSource(bitmap, nullptr));
        Check(frame->Commit());
        Check(encoder->Commit());
    }
    void MouseMove(float x, float y) {
        sceneDirty |= Contains(scrollTrack, mouse) != Contains(scrollTrack, {x, y});
        mouse = {x, y};
        if (colorDrag) {
            UpdateColorPicker(x, y);
            return;
        }
        if (scrollDragging) {
            DragScroll(y);
            return;
        }
        const int next = HitAt(mouse);
        if (next != hover) {
            hover = next;
            hoverTime = Clock::now();
            tipVisible = false;
            dirty = true;
        }
        SetCursor(LoadCursorW(nullptr, next == 40 ? IDC_IBEAM : next ? IDC_HAND : IDC_ARROW));
        TRACKMOUSEEVENT e{sizeof(e), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&e);
    }
    void MouseLeave() {
        sceneDirty |= Contains(scrollTrack, mouse);
        mouse = {-1, -1};
        hover = 0;
        tipVisible = false;
        dirty = true;
    }
    void MouseDown(float x, float y) {
        const int id = HitAt({x, y});
        if (colorOpen && (id == 360 || id == 361)) {
            colorDrag = id == 360 ? 1 : 2;
            keyboardFocus = id;
            SetCapture(hwnd);
            UpdateColorPicker(x, y);
            return;
        }
        if (colorOpen && !Contains(colorPanel, {x, y})) {
            colorOpen = false;
            dirty = true;
        }
        if (modeOpen && id != 62 && !Contains(modePanel, {x, y})) {
            modeOpen = false;
            sceneDirty = dirty = true;
        }
        if (MaxScroll() > 0 && Contains(scrollTrack, {x, y})) {
            scrollDragging = true;
            scrollGrab =
                Contains(scrollThumb, {x, y}) ? y - scrollThumb.top : (scrollThumb.bottom - scrollThumb.top) / 2;
            SetCapture(hwnd);
            DragScroll(y);
            return;
        }
        pressed = HitAt({x, y});
        FocusInput(pressed == 40 ? pressed : 0);
        keyboardFocus = 0;
        SetFocus(hwnd);
        if (pressed)
            SetCapture(hwnd);
        dirty = true;
    }
    void MouseUp(float x, float y) {
        if (colorDrag) {
            UpdateColorPicker(x, y);
            colorDrag = 0;
            ReleaseCapture();
            Save();
            return;
        }
        if (scrollDragging) {
            scrollDragging = false;
            ReleaseCapture();
            sceneDirty = dirty = true;
            return;
        }
        const int action = pressed;
        pressed = 0;
        ReleaseCapture();
        if (action && action == HitAt({x, y}))
            Activate(action);
        dirty = true;
    }
    void Wheel(int delta) {
        scrollTarget -= static_cast<float>(delta) / WHEEL_DELTA * 88;
        ClampScroll();
        tipVisible = false;
        dirty = true;
    }
    void Character(wchar_t c) {
        if (!inputFocus)
            return;
        if (c == 8) {
            if (caret == anchor && caret > 0)
                anchor = caret - 1;
            ReplaceSelection(L"");
        } else if (c >= 32 && GetKeyState(VK_CONTROL) >= 0)
            ReplaceSelection(std::wstring(1, c));
        else
            return;
        dirty = true;
    }
    std::wstring &InputValue() {
        return query;
    }
    void FocusInput(int id) {
        sceneDirty = true;
        inputFocus = id;
        caret = anchor = id ? InputValue().size() : 0;
        editTime = Clock::now();
        caretOn = true;
    }
    void ReplaceSelection(std::wstring text) {
        sceneDirty = true;
        auto &value = InputValue();
        const size_t begin = std::min(caret, anchor), end = std::max(caret, anchor), limit = 72u;
        const size_t remaining = value.size() - (end - begin);
        text.resize(std::min(text.size(), remaining < limit ? limit - remaining : 0));
        value.replace(begin, end - begin, text);
        caret = anchor = begin + text.size();
        editTime = Clock::now();
        caretOn = true;
        if (inputFocus == 40) {
            queryPending = true;
            searchChanged = Clock::now();
            scrollTarget = 0;
            catalog.games.clear();
            catalog.searching = query.size() >= 2;
        }
        dirty = true;
    }
    void Key(WPARAM key) {
        sceneDirty = true;
        if (key == VK_ESCAPE && (colorOpen || modeOpen)) {
            colorOpen = modeOpen = false;
            keyboardFocus = 0;
            dirty = true;
            return;
        }
        if (colorOpen && (keyboardFocus == 360 || keyboardFocus == 361) &&
            (key == VK_LEFT || key == VK_RIGHT || key == VK_UP || key == VK_DOWN)) {
            if (keyboardFocus == 361)
                colorHue = std::clamp(colorHue + (key == VK_LEFT ? -.025f : .025f), 0.0f, .9999f);
            else {
                colorSaturation = std::clamp(colorSaturation + (key == VK_LEFT    ? -.02f
                                                                : key == VK_RIGHT ? .02f
                                                                                  : 0),
                                             0.0f, 1.0f);
                colorValue = std::clamp(colorValue + (key == VK_DOWN ? -.02f
                                                      : key == VK_UP ? .02f
                                                                     : 0),
                                        0.0f, 1.0f);
            }
            CustomColor() = PackRgb(HSV(colorHue, colorSaturation, colorValue));
            UpdateTheme();
            Save();
            return;
        }
        const bool control = GetKeyState(VK_CONTROL) < 0;
        if (inputFocus && control && key == 'A') {
            anchor = 0;
            caret = InputValue().size();
        } else if (inputFocus && control && key == 'V') {
            if (OpenClipboard(hwnd)) {
                HANDLE clip = GetClipboardData(CF_UNICODETEXT);
                const auto *text = clip ? static_cast<const wchar_t *>(GlobalLock(clip)) : nullptr;
                if (text) {
                    std::wstring pasted;
                    for (size_t i = 0; text[i] && i < 4096 && pasted.size() < 72; ++i)
                        if (text[i] >= 32)
                            pasted.push_back(text[i]);
                    ReplaceSelection(pasted);
                    GlobalUnlock(clip);
                }
                CloseClipboard();
            }
        } else if (inputFocus && (key == VK_LEFT || key == VK_RIGHT || key == VK_HOME || key == VK_END)) {
            const bool shift = GetKeyState(VK_SHIFT) < 0;
            if (key == VK_HOME)
                caret = 0;
            else if (key == VK_END)
                caret = InputValue().size();
            else if (!shift && caret != anchor)
                caret = key == VK_LEFT ? std::min(caret, anchor) : std::max(caret, anchor);
            else if (key == VK_LEFT && caret)
                --caret;
            else if (key == VK_RIGHT && caret < InputValue().size())
                ++caret;
            if (!shift)
                anchor = caret;
            editTime = Clock::now();
            caretOn = true;
        } else if (inputFocus && key == VK_DELETE) {
            if (caret == anchor && caret < InputValue().size())
                anchor = caret + 1;
            ReplaceSelection(L"");
        } else if (key == 'F' && control && page == Page::SelectQuest) {
            FocusInput(40);
            keyboardFocus = 40;
        } else if (key == VK_ESCAPE) {
            if (inputFocus == 40) {
                query.clear();
                queryPending = true;
                searchChanged = Clock::now();
            }
            inputFocus = 0;
            keyboardFocus = 0;
        } else if (key == VK_TAB) {
            std::vector<int> ids;
            for (const auto &h : hits)
                if (h.id < 900 || h.id > 902)
                    ids.push_back(h.id);
            if (!ids.empty()) {
                auto it = std::find(ids.begin(), ids.end(), keyboardFocus);
                int i = it == ids.end() ? -1 : static_cast<int>(it - ids.begin()),
                    n = static_cast<int>(ids.size());
                i = (i + (GetKeyState(VK_SHIFT) < 0 ? n - 1 : 1) + n) % n;
                keyboardFocus = ids[i];
                FocusInput(keyboardFocus == 40 ? keyboardFocus : 0);
            }
        } else if (key == VK_RETURN) {
            if (inputFocus == 40) {
                queryPending = true;
                searchChanged = Clock::now() - std::chrono::seconds(1);
            } else if (keyboardFocus)
                Activate(keyboardFocus);
        } else if (key == VK_SPACE && !inputFocus && keyboardFocus)
            Activate(keyboardFocus);
        else if (key == VK_NEXT)
            Wheel(-WHEEL_DELTA * 3);
        else if (key == VK_PRIOR)
            Wheel(WHEEL_DELTA * 3);
        dirty = true;
    }
    int HitAt(D2D1_POINT_2F point) const {
        for (auto it = hits.rbegin(); it != hits.rend(); ++it)
            if (Contains(it->rect, point)) {
                if (colorOpen && Contains(colorPanel, point) && (it->id < 349 || it->id > 361))
                    continue;
                if (modeOpen && Contains(modePanel, point) && it->id != 64 && it->id != 65)
                    continue;
                if (it->id >= 2000 && it->id < GameControlBase) {
                    auto row = rowHover.find((it->id - 2000) / 3);
                    if (row == rowHover.end() || row->second.value < .2f)
                        continue;
                }
                return it->id;
            }
        return 0;
    }
    void Activate(int id) {
        sceneDirty = true;
        if ((runner || runnerPreview) && id == 80) {
            if (runner)
                runner->Stop();
            if (hwnd)
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
        } else if ((runner || runnerPreview) && id == 81) {
            if (worker)
                worker->Info(runnerGame, runnerRpc, runnerState.elapsed);
        } else if (id >= 10 && id <= 13) {
            const auto next = static_cast<Page>(id - 10);
            if (page != next) {
                CaptureTransition();
                transitionDirection = static_cast<int>(next) > static_cast<int>(page) ? 1 : -1;
                page = next;
                colorOpen = modeOpen = false;
                modeMenu = {};
                pageTime = Clock::now();
                if (!animating)
                    lastTick = pageTime;
                intro = {preferences.animations ? 0.0f : 1.0f, 0};
                scroll = {};
                scrollTarget = 0;
                inputFocus = 0;
            }
        } else if (id == 40)
            FocusInput(40);
        else if (id == 41) {
            query.clear();
            queryPending = true;
            searchChanged = Clock::now() - std::chrono::seconds(1);
            FocusInput(40);
        } else if (id == 42 || id == 43 || id == 44) {
            if (catalog.searching || queryPending)
                return;
            size_t nextPage = catalog.page;
            if (id == 42) {
                standardOnly = !standardOnly;
                nextPage = 0;
            } else if (id == 43 && nextPage > 0)
                --nextPage;
            else if (id == 44 && (nextPage + 1) * 100 < catalog.totalGames)
                ++nextPage;
            else
                return;
            selected = -1;
            scroll = {};
            scrollTarget = 0;
            if (worker) {
                worker->Search(query, standardOnly, nextPage);
                catalog = worker->Take();
            }
        } else if (id >= GameControlBase && id < GameControlBase + 100) {
            const int next = id - GameControlBase;
            if (next < static_cast<int>(catalog.games.size()) && next != selected) {
                previousFooter = selected >= 0 ? catalog.games[selected].name : L"";
                footerTime = Clock::now();
                footerFade = preferences.animations ? 0.0f : 1.0f;
                if (selected < 0)
                    selectionSlide = {static_cast<float>(next) * 58.0f, 0};
                selected = next;
                if (worker) {
                    worker->Executables(catalog.games[selected]);
                    catalog = worker->Take();
                }
                lastTick = Clock::now();
            }
        } else if (id == 60 && selected >= 0 && selected < static_cast<int>(catalog.games.size())) {
            const auto &game = catalog.games[selected];
            auto active =
                std::find_if(sessions.items.begin(), sessions.items.end(), [&](const services::Session &s) {
                    return s.game.SameApplication(game) && s.state == services::SessionState::Running;
                });
            if (active != sessions.items.end()) {
                StopSession(active->uid);
            } else if (ModeAvailable(game, rpcMode)) {
                try {
                    auto run = runtimeEnabled
                                   ? std::make_unique<services::ParentRun>(game, rpcMode, preferences, data)
                                   : nullptr;
                    if (run && !reaper)
                        reaper = std::make_unique<services::RunReaper>();
                    const int uid = sessions.Start(game, rpcMode, services::SteadyNow());
                    if (uid) {
                        if (run) {
                            sessions.items.back().runtimeElapsed = 0;
                            liveRuns.emplace(uid, std::move(run));
                        }
                        Activate(10);
                    }
                } catch (const std::exception &) {
                    Notify(L"Could not start session");
                }
            }
            modeOpen = false;
        } else if (id == 61)
            Activate(11);
        else if (id == 62)
            modeOpen = !modeOpen;
        else if (id == 63 && worker && selected >= 0 && selected < static_cast<int>(catalog.games.size())) {
            const auto &game = catalog.games[selected];
            double elapsed = 0;
            bool mode = rpcMode;
            for (const auto &s : sessions.items)
                if (s.game.SameApplication(game) && s.state == services::SessionState::Running) {
                    elapsed = s.Elapsed(services::SteadyNow());
                    mode = s.rpc;
                }
            worker->Info(game, mode, elapsed);
        } else if (id == 64 || id == 65) {
            rpcMode = id == 65;
            modeOpen = false;
        } else if (id == 300) {
            preferences.animations = !preferences.animations;
            Save();
        } else if (id >= 303 && id <= 304) {
            preferences.theme = id - 303;
            preferences.preset = id == 303 ? 0 : 3;
            UpdateTheme();
            Save();
        } else if (id >= 320 && id <= 324) {
            preferences.accent = id - 320;
            colorOpen = false;
            UpdateTheme();
            Save();
        } else if (id == 325) {
            preferences.accent = 5;
            colorChannel = 2;
            LoadColorPicker();
            colorOpen = true;
            UpdateTheme();
            Save();
        } else if (id >= 330 && id <= 334) {
            preferences.preset = id - 330;
            preferences.theme = id == 333 ? 1 : 0;
            colorOpen = false;
            if (id == 334) {
                colorChannel = 0;
                LoadColorPicker();
                colorOpen = true;
            }
            UpdateTheme();
            Save();
        } else if (id >= 340 && id <= 341) {
            colorChannel = id - 340;
            LoadColorPicker();
            colorOpen = true;
        } else if (id == 343) {
            preferences.customBackground = 0x15151c;
            preferences.customSurface = 0x22222c;
            LoadColorPicker();
            UpdateTheme();
            Save();
        } else if (id == 349)
            colorOpen = false;
        else if (id >= 350 && id <= 357) {
            CustomColor() = ColorSamples()[id - 350];
            LoadColorPicker();
            UpdateTheme();
            Save();
        } else if (id >= 2000) {
            int uid = (id - 2000) / 3, kind = (id - 2000) % 3;
            auto it = std::find_if(sessions.items.begin(), sessions.items.end(),
                                   [&](const services::Session &s) { return s.uid == uid; });
            if (it != sessions.items.end()) {
                if (kind == 0 && worker)
                    worker->Info(it->game, it->rpc, it->Elapsed(services::SteadyNow()));
                else if (kind == 1) {
                    StopSession(uid);
                    keyboardFocus = 0;
                } else if (kind == 2) {
                    if (it->rpc && worker)
                        worker->Info(it->game, true, it->Elapsed(services::SteadyNow()));
                    else {
                        auto found = liveRuns.find(uid);
                        if (found != liveRuns.end())
                            found->second->Show();
                    }
                }
            }
        } else if (id == 700)
            ShellExecuteW(hwnd, L"open", L"https://github.com/osk4rrv/discord-quest-completer", nullptr, nullptr, SW_SHOWNORMAL);
        else if (id == 900)
            ShowWindow(hwnd, SW_MINIMIZE);
        else if (id == 902)
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        ClampScroll();
        dirty = true;
    }
    void UpdateTheme() {
        sceneDirty = true;
        if (hwnd) {
            BOOL dark = !Light();
            DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
        }
        dirty = true;
    }
    bool ModeAvailable(const services::Game &game, bool rpc) const {
        if (rpc)
            return services::ApplicationIdValid(game.discordApplicationId) &&
                   !std::any_of(sessions.items.begin(), sessions.items.end(), [](const services::Session &s) {
                       return s.rpc && s.state == services::SessionState::Running;
                   });
        return !game.standardRequiresSteam && !services::StandardPath(game).empty();
    }
    std::wstring SessionStatus(int uid) const {
        const auto found = liveRuns.find(uid);
        if (found == liveRuns.end())
            return {};
        const auto state = found->second->Snapshot();
        return services::RuntimeLabel(state.status) + (state.error ? L" · " + std::to_wstring(state.error) : L"");
    }
    void StopSession(int uid) {
        auto found = liveRuns.find(uid);
        if (found != liveRuns.end()) {
            const auto state = found->second->Snapshot();
            for (auto &s : sessions.items)
                if (s.uid == uid)
                    s.runtimeElapsed = state.elapsed;
            found->second->Stop();
        }
        sessions.Stop(uid, services::SteadyNow());
        Save();
    }
    void RunnerFixture(bool rpc) {
        runnerPreview = true;
        runnerRpc = rpc;
        width = 440;
        height = 354;
        runnerGame = {620, L"Portal 2", {}, data / L"art" / L"620-icon.jpg"};
        runnerGame.discordApplicationId = "359508941782122496";
        runnerGame.windowsExecutables = {L"portal2.exe"};
        runnerState = {rpc ? services::RuntimeStatus::Active : services::RuntimeStatus::ProcessRunning, 0, 323};
        runnerProgress = {323, 0};
    }
    void Fixture(bool many = false) {
        sceneDirty = true;
        const double now = services::SteadyNow();
        sessions.completedRuns = 6;
        sessions.Start({620, L"Portal 2", {}, data / L"art" / L"620-icon.jpg"}, false, now - 684);
        sessions.Start({730, L"Counter-Strike 2", {}, data / L"art" / L"730-icon.jpg"}, false, now - 223);
        if (many)
            for (int i = 0; i < 12; ++i)
                sessions.Start({10000 + i, L"Session " + std::to_wstring(i + 3), {}, {}}, i % 2 == 0, now - 200);
    }
    void HoverFirstRow() {
        sceneDirty = true;
        mouse = {X() + W() - 160, 321};
        if (!sessions.items.empty())
            rowHover[sessions.items.front().uid].value = 1;
    }
    void ShowModeForQa() {
        modeOpen = true;
        modeMenu.value = 1;
    }
    float ScrollOffset() const {
        return scroll.value;
    }
    unsigned RasterCount() const {
        return pageRasterizations;
    }
    bool HasControl(int id) const {
        return std::any_of(hits.begin(), hits.end(), [&](const Hit &hit) { return hit.id == id; });
    }
    void ResizeStress() {
        sizing = true;
        for (int i = 0; i < 100; ++i) {
            dpi = std::array<float, 5>{96, 120, 144, 168, 192}[i % 5];
            const int w = static_cast<int>((760 + i % 9 * 33) * dpi / 96),
                      h = static_cast<int>((520 + i % 5 * 22) * dpi / 96);
            SetWindowPos(hwnd, nullptr, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            Activate(10 + i % 4);
            Tick();
            Paint(false);
            if (!hwnd || !backBuffer || !pageImage)
                throw std::runtime_error("Resize interrupted the graphics device.");
            RECT rect{};
            GetClientRect(hwnd, &rect);
            const auto pixels = backBuffer->GetPixelSize(), scene = pageImage->GetPixelSize();
            if (pixels.width != static_cast<UINT>(rect.right) || pixels.height != static_cast<UINT>(rect.bottom) ||
                scene.width != pixels.width || scene.height != pixels.height)
                throw std::runtime_error("GPU viewport does not match the physical client pixels.");
        }
        sizing = false;
    }
    void CancelDrag() {
        scrollDragging = false;
        colorDrag = 0;
        pressed = 0;
    }
    services::Json Benchmark(bool present = false) {
        Fixture(true);
        preferences.animations = true;
        std::vector<double> timings;
        std::vector<double> intervals;
        Paint(false);
        if (present) {
            ShowWindow(hwnd, SW_SHOWNORMAL);
            UpdateWindow(hwnd);
            Paint();
        }
        auto lastFrame = Clock::now();
        for (int i = 0; i < (present ? 240 : 360); ++i) {
            if (present) {
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
                if (!hwnd)
                    break;
                WaitForSingleObject(FrameHandle(), 100);
            }
            const int segment = present ? 48 : 72;
            if (i % segment == 0)
                Activate(10 + (i / segment) % 4);
            // Deterministic animation sampling, including cached outgoing-page transitions.
            if (!present)
                pageTime = Clock::now() - std::chrono::milliseconds((i % segment) * 4);
            MouseMove(33, NavY((i / segment) % 4) + 20);
            Tick();
            const auto start = Clock::now();
            Paint(present);
            const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
            timings.push_back(ms);
            const auto finished = Clock::now();
            if (i >= 12)
                intervals.push_back(std::chrono::duration<double, std::milli>(finished - lastFrame).count());
            lastFrame = finished;
        }
        gpuContext->Flush();
        std::sort(timings.begin(), timings.end());
        double total = 0;
        for (double value : timings)
            total += value;
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC desc{};
        Check(gpu.As(&dxgi));
        Check(dxgi->GetAdapter(adapter.GetAddressOf()));
        Check(adapter->GetDesc(&desc));
        services::Json report{{"frames", timings.size()},
                              {"pageRasterizations", pageRasterizations},
                              {"renderer", services::Utf8(desc.Description)},
                              {"paintMeanMs", total / timings.size()},
                              {"paintP95Ms", timings[timings.size() * 95 / 100]},
                              {"paintWorstMs", timings.back()},
                              {"scope", present ? "Native presentation including DXGI wait"
                                                : "CPU draw submission; excludes monitor presentation"}};
        if (present && !intervals.empty()) {
            double sum = 0;
            for (double interval : intervals)
                sum += interval;
            std::sort(intervals.begin(), intervals.end());
            DEVMODEW mode{};
            mode.dmSize = sizeof(mode);
            EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode);
            report["presentedFPS"] = 1000 * intervals.size() / sum;
            report["intervalP95Ms"] = intervals[intervals.size() * 95 / 100];
            report["displayHz"] = mode.dmDisplayFrequency;
        }
        return report;
    }

  private:
    ComPtr<ID2D1Factory1> factory;
    ComPtr<IDWriteFactory5> write;
    ComPtr<IDWriteFontCollection1> fonts;
    ComPtr<IDWriteTypography> typography;
    ComPtr<IDWriteRenderingParams> renderingParams;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<ID3D11Device> gpu;
    ComPtr<ID3D11DeviceContext> gpuContext;
    ComPtr<ID2D1Device> device;
    ComPtr<ID2D1DeviceContext> context;
    ComPtr<IDXGISwapChain2> swap;
    ComPtr<ID2D1Bitmap1> backBuffer;
    ComPtr<ID2D1Bitmap1> transitionImage;
    ComPtr<ID2D1Bitmap1> pageImage;
    ComPtr<ID2D1Layer> imageLayer;
    std::map<unsigned, ComPtr<ID2D1RadialGradientBrush>> glowBrushes;
    HANDLE latency = nullptr;
    ComPtr<ID2D1SolidColorBrush> brush;
    ID2D1RenderTarget *target = nullptr;
    std::map<std::wstring, ComPtr<ID2D1Bitmap>> bitmaps;
    std::map<uint64_t, ComPtr<ID2D1RoundedRectangleGeometry>> imageMasks;
    std::map<int, ComPtr<IDWriteTextFormat>> formats;
    std::map<std::wstring, ComPtr<IDWriteTextLayout>> layouts;
    std::vector<Hit> hits;
    std::vector<Hit> pageHits;
    bool sceneDirty = true;
    unsigned pageRasterizations = 0;
    std::map<int, Spring> hoverAnimations, toggleAnimations, pressAnimations;
    std::map<int, Spring> rowHover;
    std::map<int, D2D1_RECT_F> rowBounds;
    Spring nav, intro, scroll;
    Spring modeMenu;
    Spring selectionSlide, runnerProgress;
    bool changedSearch = false;
    float footerFade = 1;
    std::wstring previousFooter;
    Clock::time_point resultsTime = Clock::now() - std::chrono::seconds(1),
                      footerTime = Clock::now() - std::chrono::seconds(1);
    bool rpcMode = false, modeOpen = false, colorOpen = false, bufferResizePending = false;
    int colorChannel = 2, colorDrag = 0, transitionDirection = 1;
    float colorHue = 0, colorSaturation = 0, colorValue = 1;
    D2D1_RECT_F modePanel{}, colorPanel{}, saturationRect{}, hueRect{};
    float scrollTarget = 0;
    int hover = 0, pressed = 0, keyboardFocus = 0, inputFocus = 0, selected = -1;
    bool tipVisible = false, queryPending = false;
    bool scrollDragging = false, legacyCorners = false;
    D2D1_RECT_F scrollTrack{}, scrollThumb{};
    float scrollGrab = 0, toastAlpha = 0;
    unsigned seenReport = 0;
    bool caretOn = true;
    size_t caret = 0, anchor = 0;
    D2D1_POINT_2F mouse{};
    Palette p = Palette::Make(false);
    std::wstring iconFamily, brandFamily, toast;
    Clock::time_point lastTick = Clock::now(), lastSecond = Clock::now(), hoverTime = Clock::now(),
                      toastTime = Clock::now(), searchChanged = Clock::now(), editTime = Clock::now(),
                      pageTime = Clock::now() - std::chrono::seconds(1);
    std::unique_ptr<services::Worker> worker;
    std::map<int, std::unique_ptr<services::ParentRun>> liveRuns;
    std::unique_ptr<services::RunReaper> reaper;
    static bool ContentControl(int id) {
        return (id >= 40 && id <= 44) || (id >= 60 && id <= 81) || (id >= 100 && id < 700) || id >= 2000;
    }
    static float NavY(int i) {
        return 84.0f + i * 48.0f + (i >= 2 ? 16.0f : 0.0f);
    }
    float X() const {
        return Sidebar + std::max(24.0f, (width - Sidebar - 1000) / 2);
    }
    float W() const {
        return std::min(1000.0f, width - Sidebar - 48);
    }
    void Notify(std::wstring s) {
        toast = std::move(s);
        toastTime = Clock::now();
        dirty = true;
    }
    float MaxScroll() const {
        if (page == Page::SelectQuest) {
            const float top = !catalog.notice.empty() && !catalog.searching && !queryPending ? 213.0f : 201.0f;
            return std::max(0.0f, static_cast<float>(catalog.games.size()) * 58 - (height - top - 79));
        }
        if (page == Page::Dashboard)
            return std::max(0.0f, static_cast<float>(sessions.items.size()) * 66 - (height - 329));
        return 0;
    }
    void ClampScroll() {
        scrollTarget = std::clamp(scrollTarget, 0.0f, MaxScroll());
    }
    void PrepareBrush() {
        if (!brush)
            Check(target->CreateSolidColorBrush(p.text, brush.GetAddressOf()));
    }
    void DropGpu() {
        sceneDirty = true;
        bitmaps.clear();
        brush.Reset();
        transitionImage.Reset();
        pageImage.Reset();
        imageLayer.Reset();
        glowBrushes.clear();
        if (context)
            context->SetTarget(nullptr);
        backBuffer.Reset();
        swap.Reset();
        context.Reset();
        device.Reset();
        gpuContext.Reset();
        gpu.Reset();
        target = nullptr;
        if (latency) {
            CloseHandle(latency);
            latency = nullptr;
        }
    }
    void BindBuffer() {
        ComPtr<IDXGISurface> surface;
        Check(swap->GetBuffer(0, IID_PPV_ARGS(surface.GetAddressOf())));
        const auto properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), dpi, dpi);
        Check(context->CreateBitmapFromDxgiSurface(surface.Get(), &properties, backBuffer.GetAddressOf()));
        context->SetTarget(backBuffer.Get());
        context->SetDpi(dpi, dpi);
        renderingParams.Reset();
        if (SUCCEEDED(write->CreateMonitorRenderingParams(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST),
                                                          renderingParams.GetAddressOf())))
            context->SetTextRenderingParams(renderingParams.Get());
    }
    void CaptureTransition() {
        if (!preferences.animations || !context || !pageImage)
            return;
        ComPtr<ID2D1Bitmap1> snapshot;
        const auto properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), dpi,
            dpi);
        if (FAILED(context->CreateBitmap(pageImage->GetPixelSize(), nullptr, 0, &properties,
                                         snapshot.GetAddressOf())))
            return;
        context->SetTarget(snapshot.Get());
        target = context.Get();
        target->BeginDraw();
        target->Clear(p.bg);
        DrawComposition();
        const HRESULT hr = target->EndDraw();
        context->SetTarget(backBuffer.Get());
        if (SUCCEEDED(hr))
            transitionImage = std::move(snapshot);
        else
            transitionImage.Reset();
    }
    void CreateGpu() {
        D3D_FEATURE_LEVEL level;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                       D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                                       gpu.GetAddressOf(), &level, gpuContext.GetAddressOf());
        if (FAILED(hr))
            Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                    nullptr, 0, D3D11_SDK_VERSION, gpu.GetAddressOf(), &level,
                                    gpuContext.GetAddressOf()));
        ComPtr<IDXGIDevice> dxgi;
        Check(gpu.As(&dxgi));
        Check(factory->CreateDevice(dxgi.Get(), device.GetAddressOf()));
        Check(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, context.GetAddressOf()));
        ComPtr<IDXGIAdapter> adapter;
        Check(dxgi->GetAdapter(adapter.GetAddressOf()));
        ComPtr<IDXGIFactory2> df;
        Check(adapter->GetParent(IID_PPV_ARGS(df.GetAddressOf())));
        DXGI_SWAP_CHAIN_DESC1 d{};
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount = 2;
        d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        d.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        ComPtr<IDXGISwapChain1> created;
        Check(df->CreateSwapChainForHwnd(gpu.Get(), hwnd, &d, nullptr, nullptr, created.GetAddressOf()));
        Check(created.As(&swap));
        Check(swap->SetMaximumFrameLatency(1));
        latency = swap->GetFrameLatencyWaitableObject();
        df->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
        BindBuffer();
    }
#include "drawing.inc"
    ID2D1Bitmap *Bitmap(const services::fs::path &file) {
        const auto key = file.wstring();
        auto found = bitmaps.find(key);
        if (found != bitmaps.end())
            return found->second.Get();
        if (!services::fs::exists(file))
            return nullptr;
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICFormatConverter> converter;
        if (FAILED(wic->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ,
                                                  WICDecodeMetadataCacheOnLoad, decoder.GetAddressOf())) ||
            FAILED(decoder->GetFrame(0, frame.GetAddressOf())) ||
            FAILED(wic->CreateFormatConverter(converter.GetAddressOf())))
            return nullptr;
        UINT imageWidth = 0, imageHeight = 0;
        if (FAILED(frame->GetSize(&imageWidth, &imageHeight)) || !imageWidth || !imageHeight)
            return nullptr;
        ComPtr<IWICBitmapScaler> scaler;
        IWICBitmapSource *source = frame.Get();
        if (std::max(imageWidth, imageHeight) > 512) {
            float scale = 512.0f / std::max(imageWidth, imageHeight);
            if (FAILED(wic->CreateBitmapScaler(scaler.GetAddressOf())) ||
                FAILED(scaler->Initialize(frame.Get(), std::max(1u, static_cast<UINT>(imageWidth * scale)),
                                          std::max(1u, static_cast<UINT>(imageHeight * scale)),
                                          WICBitmapInterpolationModeFant)))
                return nullptr;
            source = scaler.Get();
        }
        if (FAILED(converter->Initialize(source, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr,
                                         0, WICBitmapPaletteTypeCustom)))
            return nullptr;
        ComPtr<ID2D1Bitmap> bitmap;
        if (FAILED(target->CreateBitmapFromWicBitmap(converter.Get(), nullptr, bitmap.GetAddressOf())))
            return nullptr;
        return bitmaps.emplace(key, std::move(bitmap)).first->second.Get();
    }
    void Image(const services::fs::path &file, D2D1_RECT_F r, float radius) {
        auto *b = Bitmap(file);
        if (!b)
            return;
        const auto s = b->GetSize();
        const float ratio = (r.right - r.left) / (r.bottom - r.top);
        float sw = s.width, sh = s.height;
        if (s.width / s.height > ratio)
            sw = sh * ratio;
        else
            sh = sw / ratio;
        const auto source = Box((s.width - sw) / 2, (s.height - sh) / 2, sw, sh);
        const uint64_t maskKey = (static_cast<uint64_t>((r.right - r.left) * 4) << 32) |
                                 (static_cast<uint64_t>((r.bottom - r.top) * 4) << 16) |
                                 static_cast<uint64_t>(radius * 4);
        auto &mask = imageMasks[maskKey];
        if (!mask)
            Check(factory->CreateRoundedRectangleGeometry(
                D2D1::RoundedRect(Box(0, 0, r.right - r.left, r.bottom - r.top), radius, radius),
                mask.GetAddressOf()));
        if (!imageLayer)
            Check(target->CreateLayer(nullptr, imageLayer.GetAddressOf()));
        target->PushLayer(D2D1::LayerParameters(r, mask.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                D2D1::Matrix3x2F::Translation(r.left, r.top)),
                          imageLayer.Get());
        target->DrawBitmap(b, r, 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, source);
        target->PopLayer();
    }
#include "interface.inc"
    void SetPalette() {
        p = Palette::Make(Light());
        if (preferences.preset == 1 && !Light()) {
            p.bg = Color(0x19191d);
            p.sidebar = Color(0x131316);
            p.surface = Color(0x222227);
            p.raised = Color(0x2b2b32);
            p.border = Color(0x37373f);
        } else if (preferences.preset == 2) {
            p.bg = Color(0x101722);
            p.sidebar = Color(0x0c121c);
            p.surface = Color(0x172232);
            p.raised = Color(0x203047);
            p.border = Color(0x2d3b51);
        } else if (preferences.preset == 4) {
            p.bg = Color(preferences.customBackground);
            p.sidebar = Mix(p.bg, Color(0), .13f);
            p.surface = Color(preferences.customSurface);
            const float luminance = p.surface.r * .2126f + p.surface.g * .7152f + p.surface.b * .0722f;
            const auto onSurface = Color(luminance > .55f ? 0x24242c : 0xededf2);
            p.raised = Mix(p.surface, onSurface, .055f);
            p.border = Mix(p.surface, onSurface, .12f);
        }
        p.accent = Color(preferences.accent == 5 ? preferences.customAccent : Accents[preferences.accent]);
        p.accentSoft = Mix(p.surface, p.accent, Light() ? .13f : .14f);
    }
    void DrawPage() {
        scrollTrack = scrollThumb = {};
        if (runner || runnerPreview) {
            DrawRunner();
            return;
        }
        switch (page) {
        case Page::Dashboard:
            DrawDashboard();
            break;
        case Page::SelectQuest:
            DrawSelect();
            break;
        case Page::Settings:
            DrawSettings();
            break;
        case Page::Beta:
            DrawBeta();
            break;
        }
    }
    bool PreparePage() {
        if (!sceneDirty && pageImage)
            return true;
        if (!pageImage) {
            const auto size = backBuffer->GetPixelSize();
            const auto properties = D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
                dpi, dpi);
            Check(context->CreateBitmap(size, nullptr, 0, &properties, pageImage.GetAddressOf()));
        }
        context->SetTarget(pageImage.Get());
        target->BeginDraw();
        target->Clear(p.bg);
        target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
        target->SetTransform(D2D1::Matrix3x2F::Identity());
        hits.clear();
        DrawPage();
        pageHits = hits;
        const HRESULT result = target->EndDraw();
        context->SetTarget(backBuffer.Get());
        if (result == D2DERR_RECREATE_TARGET) {
            DropGpu();
            dirty = true;
            return false;
        }
        Check(result);
        sceneDirty = false;
        ++pageRasterizations;
        return true;
    }
    void DrawComposition() {
        const float t = std::clamp(intro.value, 0.0f, 1.0f);
        if (t < 1 && transitionImage) {
            target->SetTransform(D2D1::Matrix3x2F::Translation(0, -transitionDirection * t * 10));
            target->DrawBitmap(transitionImage.Get(), nullptr, 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, nullptr);
        }
        target->SetTransform(D2D1::Matrix3x2F::Translation(0, transitionDirection * (1 - t) * 14));
        target->DrawBitmap(pageImage.Get(), nullptr, transitionImage ? t : 1,
                           t < 1 ? D2D1_BITMAP_INTERPOLATION_MODE_LINEAR
                                 : D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR,
                           nullptr);
        target->SetTransform(D2D1::Matrix3x2F::Identity());
    }
    void Render() {
        SetPalette();
        hits.clear();
        target->SetTransform(D2D1::Matrix3x2F::Identity());
        target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
        target->Clear(p.bg);
        DrawChrome();
        const float opacity = std::clamp(intro.value, 0.0f, 1.0f),
                    shift = transitionDirection * (1 - opacity) * 14;
        const bool fade = opacity < .999f;
        const float edge = runner || runnerPreview ? 0 : Sidebar + 1;
        target->PushAxisAlignedClip(Box(edge, 38, width - edge, height - 38), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        auto start = hits.size();
        if (target == context.Get() && pageImage) {
            DrawComposition();
            hits.insert(hits.end(), pageHits.begin(), pageHits.end());
        } else {
            if (fade) {
                target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
                target->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr,
                                                        D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                        D2D1::Matrix3x2F::Identity(), opacity),
                                  nullptr);
            }
            target->SetTransform(D2D1::Matrix3x2F::Translation(0, shift));
            DrawPage();
            target->SetTransform(D2D1::Matrix3x2F::Identity());
            if (fade)
                target->PopLayer();
        }
        for (size_t i = start; i < hits.size(); ++i) {
            hits[i].rect.top += shift;
            hits[i].rect.bottom += shift;
        }
        target->PopAxisAlignedClip();
        target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
        if (!runner && !runnerPreview)
            DrawBetaMotion();
        DrawOverlay();
    }
};

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    auto *app = reinterpret_cast<App *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        app = static_cast<App *>(reinterpret_cast<CREATESTRUCTW *>(lp)->lpCreateParams);
        app->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (!app)
        return DefWindowProcW(hwnd, message, wp, lp);
    try {
        switch (message) {
        case WM_NCCALCSIZE:
            return 0;
        case WM_NCPAINT:
            return 0;
        case WM_NCACTIVATE:
            return TRUE;
        case WM_NCLBUTTONDBLCLK:
            if (wp == HTCAPTION)
                return 0;
            break;
        case WM_SYSCOMMAND:
            if ((wp & 0xfff0) == SC_MAXIMIZE)
                return 0;
            break;
        case WM_NCHITTEST: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &pt);
            RECT r;
            GetClientRect(hwnd, &r);
            int e = static_cast<int>(7 * app->dpi / 96);
            if (!IsZoomed(hwnd)) {
                bool l = pt.x < e, ri = pt.x >= r.right - e, t = pt.y < e, b = pt.y >= r.bottom - e;
                if (t && l)
                    return HTTOPLEFT;
                if (t && ri)
                    return HTTOPRIGHT;
                if (b && l)
                    return HTBOTTOMLEFT;
                if (b && ri)
                    return HTBOTTOMRIGHT;
                if (t)
                    return HTTOP;
                if (b)
                    return HTBOTTOM;
                if (l)
                    return HTLEFT;
                if (ri)
                    return HTRIGHT;
            }
            if (pt.y * 96 / app->dpi < 36 && pt.x * 96 / app->dpi < app->width - 72)
                return HTCAPTION;
            return HTCLIENT;
        }
        case WM_GETMINMAXINFO: {
            auto *info = reinterpret_cast<MINMAXINFO *>(lp);
            const bool runner = app->runner || app->runnerPreview;
            info->ptMinTrackSize = {static_cast<LONG>((runner ? 440 : 760) * app->dpi / 96),
                                    static_cast<LONG>((runner ? 354 : 520) * app->dpi / 96)};
            MONITORINFO mi{sizeof(mi)};
            if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
                info->ptMaxPosition = {mi.rcWork.left - mi.rcMonitor.left, mi.rcWork.top - mi.rcMonitor.top};
                info->ptMaxSize = {mi.rcWork.right - mi.rcWork.left, mi.rcWork.bottom - mi.rcWork.top};
            }
            return 0;
        }
        case WM_SIZE:
            if (wp == SIZE_MAXIMIZED) {
                ShowWindow(hwnd, SW_RESTORE);
                return 0;
            }
            if (wp != SIZE_MINIMIZED)
                app->Resize();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_ENTERSIZEMOVE:
            app->sizing = true;
            SetTimer(hwnd, 201, 16, nullptr);
            return 0;
        case WM_EXITSIZEMOVE:
            app->sizing = false;
            KillTimer(hwnd, 201);
            app->Resize();
            return 0;
        case WM_TIMER:
            if (wp == 201 && app->sizing && !IsIconic(hwnd)) {
                app->Tick();
                if (app->dirty)
                    app->Paint();
            }
            return 0;
        case WM_DPICHANGED: {
            app->dpi = static_cast<float>(HIWORD(wp));
            auto *r = reinterpret_cast<RECT *>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            app->Resize();
            return 0;
        }
        case WM_SETTINGCHANGE:
            app->UpdateTheme();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            app->Mark();
            if (app->sizing) {
                app->Tick();
                app->Paint();
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case services::Updated:
            app->ProcessUpdate();
            return 0;
        case WM_MOUSEMOVE:
            app->MouseMove(GET_X_LPARAM(lp) * 96 / app->dpi, GET_Y_LPARAM(lp) * 96 / app->dpi);
            return 0;
        case WM_MOUSELEAVE:
            app->MouseLeave();
            return 0;
        case WM_LBUTTONDOWN:
            app->MouseDown(GET_X_LPARAM(lp) * 96 / app->dpi, GET_Y_LPARAM(lp) * 96 / app->dpi);
            return 0;
        case WM_LBUTTONUP:
            app->MouseUp(GET_X_LPARAM(lp) * 96 / app->dpi, GET_Y_LPARAM(lp) * 96 / app->dpi);
            return 0;
        case WM_CAPTURECHANGED:
            app->CancelDrag();
            return 0;
        case WM_MOUSEWHEEL:
            app->Wheel(GET_WHEEL_DELTA_WPARAM(wp));
            return 0;
        case WM_CHAR:
            app->Character(static_cast<wchar_t>(wp));
            return 0;
        case WM_KEYDOWN:
            app->Key(wp);
            return 0;
        case WM_DESTROY:
            if (app->runner)
                app->runner->Stop();
            app->hwnd = nullptr;
            PostQuitMessage(0);
            return 0;
        }
    } catch (const std::exception &error) {
        MessageBoxA(hwnd, error.what(), "Quest Completer", MB_OK | MB_ICONERROR);
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}
#include "runtime_tests.inc"
int SelfTest() {
    unsigned verification = 0;
    auto require = [&verification](bool ok) {
        ++verification;
        if (!ok)
            throw std::runtime_error("State verification failed at check " + std::to_string(verification) + ".");
    };
    auto games = services::ParseSearch(
        R"([{"appid":"620","name":"Portal 2","icon":"https://shared.fastly.steamstatic.com/portal.jpg"},{"appid":"","name":"Invalid item"}])",
        L"test-data");
    require(games.size() == 1 && games[0].id == 620);
    require(services::Encode(L"Counter Strike") == L"Counter%20Strike");
    require(services::ExecutableRelativePath(L"win64/game.exe") == services::fs::path(L"win64") / L"game.exe");
    require(services::ExecutableRelativePath(L"../../game.exe").empty() &&
            services::ExecutableRelativePath(L"C:/game.exe").empty());
    require(services::ExecutableName(L"con.exe").empty() && services::ExecutableName(L"game.exe:secret").empty());
    std::vector<unsigned char> frame{1, 0, 0, 0, 2, 0, 0, 0, '{'};
    require(!services::DecodeRpcFrame(frame));
    frame.push_back('}');
    require(services::DecodeRpcFrame(frame)->payload == "{}" && frame.empty());
    auto raw = services::CompactDiscordCatalog(
        R"([{"id":"363445589247131668","name":"ROBLOX","aliases":[],"executables":[{"os":"win32","name":"robloxplayerbeta.exe"}]}])");
    auto roblox = services::DiscordReferenceSearch(L"Roblox", raw, L"qa");
    require(roblox.size() == 1 && roblox.front().Valid() &&
            services::StandardExecutable(roblox.front()) == L"robloxplayerbeta.exe");
    auto epicGames = services::ParseEpic(
        R"({"data":{"Catalog":{"searchStore":{"elements":[{"id":"offer/../../test","namespace":"epic-space","title":"Example Game","offerType":"BASE_GAME","description":"A game","productSlug":null,"catalogNs":{"mappings":[{"pageType":"productHome","pageSlug":"example-game"}]},"keyImages":[{"type":"Thumbnail","url":"https://cdn1.epicgames.com/icon.png"}]},{"id":"dlc","title":"Example DLC","offerType":"DLC"}]}}}})",
        L"test-data");
    require(epicGames.size() == 1 && epicGames.front().Valid() &&
            epicGames.front().provider == services::Provider::Epic && epicGames.front().id == 0);
    require(epicGames.front().artFile.parent_path() == services::fs::path(L"test-data") / L"art");
    require(epicGames.front().storeUrl == "https://store.epicgames.com/en-US/p/example-game");
    const auto discordCatalog = services::CompactDiscordCatalog(
        R"([{"id":"123456789012345678","name":"Portal 2","aliases":[],"third_party_skus":[{"distributor":"steam","id":"620"}]},{"id":"223456789012345678","name":"Portal 2","aliases":[],"third_party_skus":[]},{"id":"323456789012345678","name":"Example Game","aliases":[],"third_party_skus":[{"distributor":"epic","id":"offer/../../test"}]},{"id":"423456789012345678","name":"A different title","aliases":[],"third_party_skus":[{"distributor":"epic","id":"EPIC-NAMESPACE"}]}])");
    services::ResolveDiscordIds(games, discordCatalog);
    services::ResolveDiscordIds(epicGames, discordCatalog);
    require(games.front().discordApplicationId == "123456789012345678" &&
            epicGames.front().discordApplicationId == "323456789012345678");
    auto namespaceOnly = epicGames;
    namespaceOnly.front().catalogId = "different-offer";
    namespaceOnly.front().catalogNamespace = "epic-namespace";
    namespaceOnly.front().discordApplicationId.clear();
    services::ResolveDiscordIds(namespaceOnly, discordCatalog);
    require(namespaceOnly.front().discordApplicationId == "423456789012345678" &&
            namespaceOnly.front().discordMatch == "Epic namespace in Discord catalog");
    auto ambiguous = games;
    ambiguous.front().id = 999;
    ambiguous.front().discordApplicationId.clear();
    services::ResolveDiscordIds(ambiguous, discordCatalog);
    require(ambiguous.front().discordApplicationId.empty());
    services::Game portalSteam{620, L"Portal 2", {}, {}};
    portalSteam.discordApplicationId = "123456789012345678";
    auto portalEpic = epicGames.front();
    portalEpic.name = L"Portal 2®";
    portalEpic.discordApplicationId = portalSteam.discordApplicationId;
    services::Game edition{104600, L"Portal 2 - The Final Hours", {}, {}};
    services::Game sequel{3, L"Portal 3", {}, {}};
    auto ordered = services::RankGames({sequel, edition, portalEpic, portalSteam}, L"portal 2");
    require(ordered.size() == 2 && ordered[0].id == 620 && ordered[1].id == 104600);
    auto deluxe = portalEpic;
    deluxe.name = L"Portal 2 Deluxe Edition";
    require(services::RankGames({deluxe, portalSteam}, L"Portal 2").size() == 1);
    const auto deluxeMatch = services::RankGames({portalSteam, deluxe}, L"Portal 2 Deluxe Edition");
    require(deluxeMatch.size() == 1 && deluxeMatch.front().name == deluxe.name);
    auto typo = services::RankGames({portalSteam}, L"Potal 2");
    require(typo.size() == 1);
    auto aliasGame = portalSteam;
    aliasGame.searchAliases = {L"P2"};
    require(services::RankGames({aliasGame}, L"P2").size() == 1);
    auto unrelated = portalEpic;
    unrelated.discordApplicationId = "999456789012345678";
    require(services::RankGames({portalSteam, unrelated}, L"Portal 2").size() == 2);
    services::SessionBook crossStore;
    require(crossStore.Start(portalSteam, false, 100) > 0 && crossStore.IsRunning(portalEpic) &&
            crossStore.Start(portalEpic, false, 101) == 0);
    services::Cancellation cancel;
    services::Internet firstHandle(WinHttpOpen(L"QuestCompleter test", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    services::Internet secondHandle(WinHttpOpen(L"QuestCompleter test", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    require(firstHandle.h && secondHandle.h);
    {
        services::RequestGuard firstGuard(&cancel, firstHandle), secondGuard(&cancel, secondHandle);
        cancel.Stop();
        cancel.Stop();
        require(cancel.active.size() == 2 && cancel.closed.size() == 2);
    }
    require(!firstHandle.h && !secondHandle.h && cancel.active.empty() && cancel.closed.empty());
    services::SessionBook mixed;
    require(mixed.Start(games.front(), false, 100) > 0 && mixed.Start(epicGames.front(), false, 100) > 0 &&
            mixed.ActiveCount() == 2 && mixed.Start(epicGames.front(), false, 101) == 0);
    services::Preferences roundTrip;
    roundTrip.FromJson({{"version", 4}, {"preset", 4}, {"accent", 1}, {"customAccent", 0xabcdef}});
    require(roundTrip.preset == 4 && roundTrip.accent == 5 && roundTrip.customAccent == 0xabcdef);
    roundTrip.preset = 3;
    roundTrip.FromJson(services::Json::parse(roundTrip.Serialize()));
    require(roundTrip.preset == 3 && roundTrip.accent == 5 && roundTrip.customAccent == 0xabcdef);
    roundTrip.FromJson({{"version", 4}, {"preset", 5}, {"accent", 2}});
    require(roundTrip.preset == 0 && roundTrip.accent == 2);
    services::SessionBook book;
    auto game = games.front();
    int first = book.Start(game, false, 1000);
    require(first > 0 && book.ActiveCount() == 1 && book.completedRuns == 0);
    require(book.Start(game, true, 1001) == 0);
    require(!book.Update(1899.999) && book.completedRuns == 0);
    require(book.Update(1900) && book.completedRuns == 1 && book.SavedSeconds() == 900);
    require(!book.Update(2000) && book.completedRuns == 1);
    int second = book.Start(game, false, 2100);
    require(book.Stop(second, 2200) && book.completedRuns == 1 && book.ActiveCount() == 0);
    book.Start(game, false, 2300);
    require(book.Update(3200) && book.completedRuns == 2 && book.SavedSeconds() == 1800);
    int boundary = book.Start(game, false, 3300);
    require(book.Stop(boundary, 4200) && book.completedRuns == 3);
    require(!book.Stop(boundary, 4201) && !book.Update(4201) && book.completedRuns == 3);
    services::Game dangerous{1, L"<script>alert('x')</script>", {}, {}};
    const auto report = services::InfoHtml(dangerous, false, 100, services::Json{});
    require(report.find("<script>") == std::string::npos && report.find("&lt;script&gt;") != std::string::npos);
    Spring s;
    for (int i = 0; i < 240; ++i)
        s.Step(1, 1.0f / 144, true);
    require(std::abs(s.value - 1) < .001f);
    App app(false);
    app.runtimeEnabled = false;
    const auto launch = services::ParseSteamExecutables(
        R"({"status":"success","data":{"366320":{"common":{"gameid":"366320"},"config":{"installdir":"Seasons after Fall","launch":{"0":{"config":{"oslist":"windows"},"executable":"Seasons after Fall.exe"},"1":{"config":{"oslist":"macos"},"executable":"wrong.exe"},"2":{"executable":"../../bad.exe"},"3":{"executable":"steam2ea://launchgame/366320"},"4":{"executable":"Launcher.exe"},"5":{"executable":"CON.exe"}}}}}})",
        "366320");
    require(launch.at("executables").size() == 1);
    services::Game seasons{366320, L"Seasons after Fall", {}, {}};
    seasons.windowsExecutables = {L"gameguard.des"};
    services::ApplyExecutableRecord(seasons, launch);
    require(seasons.windowsExecutables.size() == 1 &&
            services::StandardPath(seasons) ==
                services::fs::path(L"Seasons after Fall") / L"Seasons after Fall.exe" &&
            !seasons.discordExecutableRule && app.ModeAvailable(seasons, false));
    const auto recovered = services::RuntimeGame(services::RuntimeGameJson(seasons));
    require(recovered.windowsExecutables == seasons.windowsExecutables &&
            recovered.windowsExecutableSource == seasons.windowsExecutableSource);
    const auto uriOnly = services::ParseSteamExecutables(
        R"({"status":"success","data":{"4080220":{"config":{"installdir":"EA SPORTS FC 27","launch":{"0":{"executable":"steam2ea://launchgame/4080220?platform=steam"}}}}}})",
        "4080220");
    require(uriOnly.at("executables").empty() && !services::CatalogText(uriOnly, "reason").empty());
    services::Game fc27{4080220, L"EA Sports FC 27", {}, {}};
    services::ApplyExecutableRecord(fc27, uriOnly);
    require(!app.ModeAvailable(fc27, false));
    const std::string tick(1, char(96));
    const auto directory = services::ParseCommunityExecutables(
        "| Example Game | " + tick + "..\\Game\\Real.exe" + tick + " |\n" + "| Bad | " + tick +
        "..\\..\\outside.exe" + tick + " |\n" + "| Placeholder | " + tick + "..\\version-xxxxxxxx\\game.exe" +
        tick + " |\n" + "| Mac | " + tick + "game.app" + tick + " |\n");
    require(directory.at("games").size() == 1);
    const auto uniqueCatalog = services::CompactDiscordCatalog(
        R"([{"id":"123456789012345678","name":"Example Game","aliases":[],"executables":[]}])");
    auto example = services::DiscordReferenceSearch(L"Example Game", uniqueCatalog, app.data);
    services::ApplyCommunityExecutables(example.front(), uniqueCatalog, directory);
    require(services::StandardPath(example.front()) == services::fs::path(L"Game") / L"Real.exe" &&
            !example.front().discordExecutableRule);
    auto ambiguousCatalog = uniqueCatalog;
    ambiguousCatalog.push_back(
        {{"id", "223456789012345678"}, {"name", "Example Game"}, {"aliases", services::Json::array()}});
    auto ambiguousGame = example.front();
    ambiguousGame.windowsExecutables.clear();
    services::ApplyCommunityExecutables(ambiguousGame, ambiguousCatalog, directory);
    require(ambiguousGame.windowsExecutables.empty());
    seasons.standardRequiresSteam = true;
    require(!app.ModeAvailable(seasons, false));
    require(roblox.front().discordExecutableRule);
    auto mirrored = raw;
    mirrored.front()["catalogSourceUrl"] = "https://markterence.github.io/discord-quest-completer/detectable.json";
    const auto mirroredRoblox = services::DiscordReferenceSearch(L"Roblox", mirrored, app.data);
    require(mirroredRoblox.front().windowsExecutableSource == "Discord detectable catalog (community mirror)" &&
            mirroredRoblox.front().windowsExecutableSourceUrl ==
                "https://markterence.github.io/discord-quest-completer/detectable.json");
    services::Json expanded = services::Json::array();
    for (int i = 0; i < 125; ++i)
        expanded.push_back({{"id", std::to_string(100000000000000000ull + i)},
                            {"name", "Browse Game " + std::to_string(i)},
                            {"executables", services::Json::array({{{"os", "win32"}, {"name", "browse.exe"}}})},
                            {"aliases", services::Json::array()}});
    auto browsable = services::DiscordReferenceSearch(L"", expanded, app.data);
    require(browsable.size() == 125 && services::RankGames(browsable, L"Browse Game").size() == 125);
    size_t browseTotal = 0;
    const auto firstPage = services::PageCatalogGames(browsable, false, 0, &browseTotal);
    const auto lastPage = services::PageCatalogGames(browsable, false, 1);
    require(browseTotal == 125 && firstPage.size() == 100 && lastPage.size() == 25 &&
            !firstPage.back().SameApplication(lastPage.front()) &&
            services::PageCatalogGames(browsable, false, 999).front().SameApplication(lastPage.front()));
    auto filteredBrowse = browsable;
    filteredBrowse[0].windowsExecutables.clear();
    filteredBrowse[1].standardRequiresSteam = true;
    require(services::PageCatalogGames(filteredBrowse, true, 1, &browseTotal).size() == 23 &&
            browseTotal == 123 && services::PageCatalogGames({}, true, 999, &browseTotal).empty() &&
            browseTotal == 0);
    std::vector<services::Game> batch{example.front(), ambiguousGame};
    for (auto &item : batch)
        item.windowsExecutables.clear();
    services::ApplyCommunityExecutableBatch(batch, ambiguousCatalog, directory);
    require(batch[0].windowsExecutables.empty() && batch[1].windowsExecutables.empty());
    services::ApplyCommunityExecutableBatch(batch, uniqueCatalog, directory);
    require(!services::StandardPath(batch[0]).empty());
    auto curatedCatalog = services::Json::array({
        {{"id", "1531874756096295054"}, {"name", "EA SPORTS FC 27"},
         {"skus", services::Json::array({{{"distributor", "steam"}, {"id", "4080220"}}})}},
        {{"id", "123456789012345678"}, {"name", "Enginefall"}}});
    auto curatedGames = services::DiscordReferenceSearch(L"", curatedCatalog, app.data);
    services::ApplyCuratedExecutableBatch(curatedGames, curatedCatalog, services::BundledQuestPaths());
    require(curatedGames.size() == 2);
    const auto curatedFc = std::find_if(curatedGames.begin(), curatedGames.end(), [](const services::Game &g) {
        return g.discordApplicationId == "1531874756096295054";
    });
    require(curatedFc != curatedGames.end() && app.ModeAvailable(*curatedFc, false) &&
            curatedFc->steamInstallDirectory == L"EA SPORTS FC 27" &&
            services::StandardPath(*curatedFc) == services::fs::path(L"fc27.exe") &&
            services::DiscordReferenceSearch(L"fc27", curatedCatalog, app.data).size() == 1 &&
            services::ExecutableRelativePath(L"version-{hash}/game.exe").empty());
    curatedGames[0].steamApplicationId = "123";
    for (auto &g : curatedGames) { g.windowsExecutables.clear(); g.steamInstallDirectory.clear(); }
    services::ApplyCuratedExecutableBatch(curatedGames, curatedCatalog, services::BundledQuestPaths());
    require(curatedGames[0].steamInstallDirectory.empty());
    app.catalog.games.assign(browsable.begin(), browsable.begin() + 100);
    app.Activate(GameControlBase + 99);
    app.Activate(64);
    app.Activate(60);
    require(app.sessions.items.back().game.name == browsable[99].name && app.page == Page::Dashboard);
    app.StopSession(app.sessions.items.back().uid);
    app.sessions.items.clear();
    app.Activate(12);
    require(app.page == Page::Settings);
    app.Activate(13);
    require(app.page == Page::Beta);
    app.Activate(304);
    require(app.Light());
    app.query = L"Portal";
    app.FocusInput(40);
    app.Key(VK_HOME);
    app.Character(L'X');
    require(app.query == L"XPortal");
    app.Key(VK_DELETE);
    require(app.query == L"Xortal");
    app.Character(8);
    require(app.query == L"ortal");
    app.Activate(11);
    app.catalog.games = games;
    app.Activate(GameControlBase);
    app.Activate(65);
    app.Activate(60);
    require(app.page == Page::Dashboard && app.sessions.ActiveCount() == 1 && app.sessions.items.front().rpc);
    app.Activate(60);
    require(app.sessions.ActiveCount() == 0 && app.sessions.completedRuns == 0);
    app.Activate(12);
    app.Activate(325);
    app.Activate(351);
    require(app.preferences.accent == 5 && app.preferences.customAccent == 0x94b5dc);
    for (int id = 330; id <= 334; ++id) {
        app.Activate(id);
        require(app.preferences.accent == 5 && app.preferences.customAccent == 0x94b5dc);
    }
    app.Activate(334);
    app.Activate(350);
    require(app.preferences.preset == 4 && app.preferences.customBackground == 0x151518 &&
            app.preferences.customAccent == 0x94b5dc);
    app.Activate(343);
    require(app.preferences.customAccent == 0x94b5dc);
    app.Activate(321);
    require(app.preferences.accent == 1 && app.preferences.preset == 4);
    for (int i = 0; i < 300; ++i) {
        s.Step(i % 30 < 15 ? 244.0f : 84.0f, 1.0f / 144, true, 26, 1);
        require(std::isfinite(s.value) && std::isfinite(s.velocity));
    }
    return 0;
}
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(init))
        return 1;
    int result = 0;
    try {
        int argc = 0;
        wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (!argv)
            throw std::runtime_error("Command line is unavailable.");
        std::vector<std::wstring> args(argv, argv + argc);
        LocalFree(argv);
        bool test = false, windowTest = false, apiTest = false, brand = false, fixture = false, many = false,
             hoverRow = false, benchmark = false, infoTest = false, frameTest = false, nativeShot = false,
             install = false, customTheme = false, customAccent = false, selectFirst = false, showMode = false,
             resizeTest = false, runtimeTest = false, rpcProbe = false, rpcDiagnose = false,
             resolveExecutables = false, reportRpc = false, standardDiagnose = false, localFolderDiagnose = false;
        unsigned diagnosticSeconds = 2;
        unsigned prepareSeconds = 0;
        std::wstring screenshot, runnerConfig, runtimeKey;
        services::Json sessionConfig;
        for (size_t i = 1; i + 1 < args.size(); ++i) {
            if (args[i] == L"--session-runner")
                runnerConfig = args[++i];
            else if (args[i] == L"--runtime-key")
                runtimeKey = args[++i];
        }
        if (!runnerConfig.empty())
            sessionConfig = services::Json::parse(services::Read(runnerConfig));
        float scale = 1;
        for (auto &s : args) {
            test |= s == L"--self-test";
            windowTest |= s == L"--window-test";
            apiTest |= s == L"--api-test";
            fixture |= s == L"--fixture";
            many |= s == L"--fixture-list";
            hoverRow |= s == L"--hover-run";
            benchmark |= s == L"--benchmark";
            infoTest |= s == L"--info-test";
            frameTest |= s == L"--frame-test";
            nativeShot |= s == L"--native-shot";
            install |= s == L"--install";
            customTheme |= s == L"--custom-theme";
            customAccent |= s == L"--custom-accent";
            selectFirst |= s == L"--select-first";
            showMode |= s == L"--mode-menu";
            resizeTest |= s == L"--resize-test";
            runtimeTest |= s == L"--runtime-test";
            rpcProbe |= s == L"--rpc-probe";
            rpcDiagnose |= s == L"--rpc-diagnose";
            standardDiagnose |= s == L"--standard-diagnose";
            localFolderDiagnose |= s == L"--local-folder";
            resolveExecutables |= s == L"--resolve-executables";
        }
        bool headless = test || rpcProbe || rpcDiagnose || standardDiagnose || resolveExecutables || runtimeTest || windowTest ||
                        apiTest || benchmark || infoTest || frameTest || resizeTest || fixture || many ||
                        hoverRow || customTheme || customAccent || showMode || selectFirst;
        for (auto &s : args)
            headless |= s == L"--screenshot" || s == L"--brand";
        App app(!headless && runnerConfig.empty(),
                runnerConfig.empty()
                    ? services::fs::path{}
                    : services::fs::path(services::Wide(sessionConfig.at("data").get<std::string>())));
        if (!runnerConfig.empty()) {
            app.preferences.FromJson(sessionConfig.at("preferences"));
            app.runnerGame = services::RuntimeGame(sessionConfig.at("game"));
            app.runnerRpc = sessionConfig.value("rpc", false);
            app.width = 440;
            app.height = 354;
            if (!app.runnerGame.Valid() ||
                (app.runnerRpc && !services::ApplicationIdValid(app.runnerGame.discordApplicationId)))
                throw std::runtime_error("Session metadata is invalid.");
            app.runner = std::make_unique<services::RunnerRuntime>(runtimeKey, app.runnerRpc,
                app.runnerGame.discordApplicationId, services::Wide(sessionConfig.value("testPipe", std::string{})),
                app.data / L"info" / services::Wide(app.runnerGame.discordApplicationId + ".rpc.json"));
        }
        services::Provider diagnosticProvider = services::Provider::All;
        for (size_t i = 1; i < args.size(); ++i) {
            if (args[i] == L"--screenshot" && i + 1 < args.size())
                screenshot = args[++i];
            else if (args[i] == L"--brand" && i + 1 < args.size()) {
                screenshot = args[++i];
                brand = true;
            } else if (args[i] == L"--scale" && i + 1 < args.size())
                scale = std::clamp(std::stof(args[++i]), 1.0f, 3.0f);
            else if (args[i] == L"--light")
                app.Activate(304);
            else if (args[i] == L"--compact") {
                app.width = 760;
                app.height = 520;
            } else if (args[i] == L"--wide") {
                app.width = 1040;
                app.height = 700;
            } else if (args[i] == L"--theme" && i + 1 < args.size()) {
                const auto theme = args[++i];
                app.Activate(theme == L"graphite"                     ? 331
                             : theme == L"midnight"                   ? 332
                             : theme == L"pearl" || theme == L"white" ? 333
                                                                      : 330);
            } else if (args[i] == L"--provider" && i + 1 < args.size()) {
                const auto provider = args[++i];
                diagnosticProvider = provider == L"epic"    ? services::Provider::Epic
                                     : provider == L"steam" ? services::Provider::Steam
                                     : provider == L"discord" ? services::Provider::Discord
                                                            : services::Provider::All;
            } else if (args[i] == L"--view" && i + 1 < args.size()) {
                auto v = args[++i];
                reportRpc = v == L"rpc";
                if (v == L"runner" || v == L"runner-rpc")
                    app.RunnerFixture(v == L"runner-rpc");
                if (v == L"rpc")
                    app.Activate(65);
                app.page = v == L"select" || v == L"quests" ? Page::SelectQuest
                           : v == L"rpc"                    ? Page::SelectQuest
                           : v == L"settings"               ? Page::Settings
                           : v == L"beta"                   ? Page::Beta
                                                            : Page::Dashboard;
            } else if (args[i] == L"--diagnostic-seconds" && i + 1 < args.size())
                diagnosticSeconds = std::clamp(static_cast<unsigned>(std::stoul(args[++i])), 1u, 300u);
            else if (args[i] == L"--prepare-seconds" && i + 1 < args.size())
                prepareSeconds = std::clamp(static_cast<unsigned>(std::stoul(args[++i])), 0u, 60u);
            else if (args[i] == L"--standard-only")
                app.standardOnly = true;
            else if (args[i] == L"--search" && i + 1 < args.size())
                app.query = args[++i];
        }
        if (fixture || many)
            app.Fixture(many);
        if (hoverRow)
            app.HoverFirstRow();
        if (apiTest || infoTest || !app.query.empty() || (headless && app.page == Page::SelectQuest)) {
            services::Json discordCatalog;
            app.catalog.games =
                services::SearchCatalog(app.query, diagnosticProvider, app.data,
                                        nullptr, discordCatalog, app.catalog.notice, app.standardOnly, 0,
                                        &app.catalog.totalGames);
            if (apiTest && app.catalog.games.empty())
                throw std::runtime_error("No games returned by the live catalog.");
            for (size_t i = 0; i < std::min(size_t(8), app.catalog.games.size()); ++i) {
                auto &g = app.catalog.games[i];
                if (!g.artUrl.empty() && !services::fs::exists(g.artFile))
                    try { services::Save(g.artFile, services::Get(g.artUrl)); } catch (...) { }
            }
            if (app.runnerPreview && !app.catalog.games.empty())
                app.runnerGame = app.catalog.games.front();
        }
        if (selectFirst && !app.catalog.games.empty())
            app.Activate(GameControlBase);
        if (resolveExecutables && !app.catalog.games.empty()) {
            const auto catalog = services::LoadDiscordCatalog(app.data);
            services::ResolveExecutableMetadata(app.catalog.games.front(), app.data, nullptr, catalog);
        }
        if (showMode)
            app.ShowModeForQa();
        if (customTheme) {
            app.Activate(12);
            app.Activate(334);
        }
        if (customAccent) {
            app.Activate(12);
            app.Activate(325);
        }
        if (install) {
            const auto message = services::Utf8(app.data.wstring()) + "\n";
            DWORD n;
            WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), message.data(), static_cast<DWORD>(message.size()), &n,
                      nullptr);
        } else if (test)
            result = SelfTest();
        else if (runtimeTest)
            result = RuntimeTest(app.data);
        else if (rpcProbe) {
            if (app.catalog.games.empty())
                throw std::runtime_error("Search for a game to probe its Discord ID.");
            const auto output =
                services::ProbeDiscordIpc(app.catalog.games.front().discordApplicationId).dump(2) + "\n";
            DWORD count;
            WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), output.data(), static_cast<DWORD>(output.size()), &count,
                      nullptr);
        } else if (rpcDiagnose || standardDiagnose) {
            if (app.catalog.games.empty())
                throw std::runtime_error("Search for a game to diagnose its session.");
            const auto &game = app.catalog.games.front();
            if (standardDiagnose && !app.ModeAvailable(game, false))
                throw std::runtime_error("Standard is unavailable for this game.");
            services::RuntimeSnapshot observed;
            {
                services::ParentRun run(game, !standardDiagnose, app.preferences, app.data, true, {},
                    localFolderDiagnose ? std::optional<services::fs::path>(services::fs::path{}) : std::nullopt,
                    prepareSeconds);
                const auto until = GetTickCount64() + (diagnosticSeconds + prepareSeconds + 10ull) * 1000;
                while (GetTickCount64() < until) {
                    observed = run.Snapshot();
                    if (observed.elapsed >= diagnosticSeconds || observed.status == services::RuntimeStatus::Error ||
                        observed.status == services::RuntimeStatus::Stopped ||
                        observed.status == services::RuntimeStatus::LaunchFailed)
                        break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
                run.Stop();
            }
            auto diagnostic = standardDiagnose
                ? services::Json{{"applicationId", game.discordApplicationId}, {"mode", "Standard"},
                                 {"questProgressAvailable", false}, {"requestedSeconds", diagnosticSeconds}}
                : services::Json::parse(
                    services::Read(app.data / L"info" / services::Wide(game.discordApplicationId + ".rpc.json")));
            diagnostic["observedStatus"] = services::Utf8(services::RuntimeLabel(observed.status));
            diagnostic["observedLocalElapsed"] = observed.elapsed;
            diagnostic["prepareSeconds"] = prepareSeconds;
            try {
                diagnostic["process"] = services::Json::parse(services::Read(
                    app.data / L"info" / services::Wide(game.discordApplicationId + ".process.json")));
            } catch (...) { }
            if (observed.status == services::RuntimeStatus::LaunchFailed)
                try {
                    diagnostic["launchError"] = services::Json::parse(services::Read(
                        app.data / L"info" / services::Wide(game.discordApplicationId + ".launch.json")));
                    diagnostic.erase("process");
                } catch (...) { }
            const auto output = diagnostic.dump(2) + "\n";
            DWORD count;
            WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), output.data(), static_cast<DWORD>(output.size()), &count,
                      nullptr);
        } else if (apiTest) {
            services::Json output{{"provider", diagnosticProvider == services::Provider::All    ? "Combined"
                                               : diagnosticProvider == services::Provider::Epic ? "Epic"
                                               : diagnosticProvider == services::Provider::Discord ? "Discord"
                                                                                                : "Steam"},
                                  {"count", app.catalog.games.size()},
                                  {"total", app.catalog.totalGames},
                                  {"standardOnly", app.standardOnly},
                                  {"notice", services::Utf8(app.catalog.notice)},
                                  {"games", services::Json::array()}};
            for (const auto &game : app.catalog.games)
                output["games"].push_back(
                    {{"name", services::Utf8(game.name)},
                     {"source", game.provider == services::Provider::Discord ? "Discord"
                                : game.provider == services::Provider::Epic  ? "Epic"
                                                                             : "Steam"},
                     {"catalogId",
                      game.provider == services::Provider::Steam ? std::to_string(game.id) : game.catalogId},
                     {"catalogNamespace", game.catalogNamespace},
                     {"discordApplicationId", game.discordApplicationId},
                     {"matchedBy", game.discordMatch},
                     {"executableSource", game.windowsExecutableSource},
                     {"executableSourceUrl", game.windowsExecutableSourceUrl},
                     {"discordExecutableRule", game.discordExecutableRule},
                     {"steamApplicationId", game.steamApplicationId},
                     {"steamInstallDirectory", services::Utf8(game.steamInstallDirectory)},
                     {"standardUnavailableReason", game.standardUnavailableReason},
                     {"standardAvailable", app.ModeAvailable(game, false)},
                     {"windowsExecutables", services::Json::array()},
                     {"referenceOnly", game.referenceOnly},
                     {"artworkCached", services::fs::exists(game.artFile)}});
            for (size_t i = 0; i < app.catalog.games.size(); ++i)
                for (const auto &exe : app.catalog.games[i].windowsExecutables)
                    output["games"][i]["windowsExecutables"].push_back(services::Utf8(exe));
            std::string message = output.dump(2) + "\n";
            DWORD n;
            WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), message.data(), static_cast<DWORD>(message.size()), &n,
                      nullptr);
        } else if (infoTest) {
            std::string failure;
            const auto file =
                services::GenerateInfo(app.catalog.games.front(), reportRpc, 123, app.data, nullptr, &failure);
            if (services::Read(file).find("<h2>Identifiers</h2>") == std::string::npos ||
                (app.catalog.games.front().provider == services::Provider::Steam &&
                 services::Read(file).find("<h2>About the game</h2>") == std::string::npos))
                throw std::runtime_error("API game information was not included in the report: " + failure);
            const std::string output = services::Utf8(file.wstring()) + "\n";
            DWORD n;
            WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), output.data(), static_cast<DWORD>(output.size()), &n,
                      nullptr);
        } else if (!screenshot.empty() && !nativeShot)
            app.Snapshot(screenshot.c_str(), scale, brand);
        else {
            SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            WNDCLASSEXW wc{sizeof(wc)};
            wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
            wc.lpfnWndProc = WindowProc;
            wc.hInstance = instance;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.lpszClassName = WindowClass;
            const auto appIcon = app.data / L"assets" / L"quest.ico";
            wc.hIcon =
                static_cast<HICON>(LoadImageW(nullptr, appIcon.c_str(), IMAGE_ICON, 32, 32, LR_LOADFROMFILE));
            wc.hIconSm =
                static_cast<HICON>(LoadImageW(nullptr, appIcon.c_str(), IMAGE_ICON, 16, 16, LR_LOADFROMFILE));
            Check(RegisterClassExW(&wc) ? S_OK : HRESULT_FROM_WIN32(GetLastError()));
            app.dpi = nativeShot ? 96 * scale : static_cast<float>(GetDpiForSystem());
            MONITORINFO mi{sizeof(mi)};
            GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
            int w = static_cast<int>(app.width * app.dpi / 96), h = static_cast<int>(app.height * app.dpi / 96);
            if (!nativeShot) {
                w = std::min<LONG>(w, mi.rcWork.right - mi.rcWork.left - 24);
                h = std::min<LONG>(h, mi.rcWork.bottom - mi.rcWork.top - 24);
            }
            int x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - w) / 2,
                y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - h) / 2;
            HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, WindowClass, L"Quest Completer",
                                        WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_SYSMENU, x, y, w, h,
                                        nullptr, nullptr, instance, &app);
            if (!hwnd)
                throw std::runtime_error("Could not create application window.");
            if (app.runner) {
                app.runner->Window(hwnd);
                SetWindowTextW(hwnd, (app.runnerGame.name + L" · Session").c_str());
            }
            if (const HMENU menu = GetSystemMenu(hwnd, FALSE))
                DeleteMenu(menu, SC_MAXIMIZE, MF_BYCOMMAND);
            app.UpdateTheme();
            app.ConfigureWindowCorners();
            if (nativeShot && !screenshot.empty()) {
                app.NativeSnapshot(screenshot.c_str());
                DestroyWindow(hwnd);
            } else if (benchmark || frameTest) {
                const auto output = app.Benchmark(frameTest).dump(2) + "\n";
                DWORD n;
                WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), output.data(), static_cast<DWORD>(output.size()), &n,
                          nullptr);
                DestroyWindow(hwnd);
            } else if (resizeTest) {
                app.ResizeStress();
                DestroyWindow(hwnd);
            } else if (windowTest) {
                if ((GetWindowLongPtrW(hwnd, GWL_STYLE) & (WS_CAPTION | WS_MAXIMIZEBOX)) != 0)
                    throw std::runtime_error("Native caption is still enabled.");
                SendMessageW(hwnd, WM_SYSCOMMAND, SC_MAXIMIZE, 0);
                SendMessageW(hwnd, WM_NCLBUTTONDBLCLK, HTCAPTION, 0);
                if (IsZoomed(hwnd))
                    throw std::runtime_error("Window maximization is still enabled.");
                app.preferences.animations = false;
                app.Paint(false);
                const unsigned initialRasterCount = app.RasterCount();
                app.Paint(false);
                if (app.RasterCount() != initialRasterCount)
                    throw std::runtime_error("Static page was unnecessarily rasterized.");
                for (int id = 10; id <= 13; ++id) {
                    app.Activate(id);
                    app.Tick();
                    app.Paint(false);
                }
                app.Activate(304);
                app.Paint(false);
                app.Activate(11);
                app.FocusInput(40);
                app.Paint(false);
                const unsigned beforeEdit = app.RasterCount();
                app.Character(L'P');
                app.Paint(false);
                if (app.RasterCount() <= beforeEdit)
                    throw std::runtime_error("Text editing did not invalidate the cached page.");
                app.dpi = 144;
                SetWindowPos(hwnd, nullptr, 0, 0, 1140, 780, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                app.Resize();
                app.Paint(false);
                app.Activate(10);
                app.Fixture(true);
                app.Tick();
                app.Paint(false);
                app.MouseDown(app.width - 30, 330);
                app.MouseMove(app.width - 30, app.height - 44);
                app.MouseUp(app.width - 30, app.height - 44);
                if (app.ScrollOffset() <= 0)
                    throw std::runtime_error("Scrollbar dragging failed.");
                app.Paint(false);
                app.sessions.items.clear();
                app.Activate(11);
                app.query = L"Unavailable";
                app.catalog.searching = false;
                app.catalog.games = {{99999999, L"Unavailable game", {}, {}}};
                app.Activate(GameControlBase);
                app.Activate(64);
                app.Paint(false);
                if (app.HasControl(60))
                    throw std::runtime_error("Unavailable Standard is clickable.");
                app.Activate(65);
                app.Paint(false);
                if (app.HasControl(60))
                    throw std::runtime_error("Unavailable RPC is clickable.");
                app.catalog.games.front().discordApplicationId = "123456789012345678";
                app.Activate(65);
                app.Paint(false);
                if (!app.HasControl(60))
                    throw std::runtime_error("Available RPC is disabled.");
                DestroyWindow(hwnd);
            } else {
                app.StartServices();
                app.Paint(false);
                const bool hiddenRunner =
                    app.runner && std::find(args.begin(), args.end(), L"--runner-hidden") != args.end();
                if (!hiddenRunner)
                    ShowWindow(hwnd, show == SW_HIDE ? SW_SHOWNORMAL : show);
                UpdateWindow(hwnd);
                bool running = true, frameReady = true;
                while (running) {
                    MSG msg{};
                    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                        if (msg.message == WM_QUIT) {
                            result = static_cast<int>(msg.wParam);
                            running = false;
                            break;
                        }
                        TranslateMessage(&msg);
                        DispatchMessageW(&msg);
                    }
                    if (!running)
                        break;
                    app.Tick();
                    HANDLE frame = app.FrameHandle();
                    if (app.dirty && !IsIconic(hwnd)) {
                        if (!frameReady && frame) {
                            const DWORD ready =
                                MsgWaitForMultipleObjectsEx(1, &frame, 100, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
                            if (ready == WAIT_OBJECT_0)
                                frameReady = true;
                            else
                                continue;
                        }
                        app.Paint();
                        frameReady = false;
                        frame = app.FrameHandle();
                    }
                    const DWORD count = app.animating && frame && !IsIconic(hwnd) ? 1u : 0u;
                    const DWORD event = MsgWaitForMultipleObjectsEx(count, count ? &frame : nullptr,
                                                                    count ? 100 : app.IdleWaitMs(), QS_ALLINPUT,
                                                                    MWMO_INPUTAVAILABLE);
                    if (count && event == WAIT_OBJECT_0)
                        frameReady = true;
                }
            }
        }
    } catch (const std::exception &error) {
        std::string message = std::string(error.what()) + "\n";
        DWORD n;
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), message.data(), static_cast<DWORD>(message.size()), &n, nullptr);
        if (std::wstring(GetCommandLineW()).find(L"--") == std::wstring::npos)
            MessageBoxA(nullptr, error.what(), "Quest Completer", MB_OK | MB_ICONERROR);
        result = 1;
    }
    CoUninitialize();
    return result;
}
