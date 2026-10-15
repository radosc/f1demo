#include "ui/app.h"
#include "audio/sound.h"
#include "core/fs.h"
#include "core/png.h"
#include "game/game.h"
#include "game/worldview.h"
#include "gfx/palette.h"
#include "gfx/sprite.h"

#include <windows.h>
#include <windowsx.h>

namespace fo {

App g_app;

void Input::beginFrame()
{
    for (int i = 0; i < 3; i++) pressed[i] = released[i] = false;
    memset(keyPressed, 0, sizeof(keyPressed));
    chars.clear();
    wheel = 0;
    doubleClick = false;
}

static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CLOSE:
        if (!g_app.onClose || !g_app.onClose()) g_app.requestQuit();
        return 0;
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) g_app.onResize(LOWORD(lp), HIWORD(lp));
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN && (lp & (1 << 29))) {
            g_app.toggleFullscreen();
            return 0;
        }
        if (wp == VK_F4 && (lp & (1 << 29))) {
            g_app.requestQuit();
            return 0;
        }
        g_app.onKey((int)wp, true);
        return 0;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        g_app.onKey((int)wp, false);
        return 0;
    case WM_CHAR:
        g_app.onChar((u32)wp);
        return 0;
    case WM_LBUTTONDOWN: SetCapture(hwnd); g_app.onMouseButton(0, true); return 0;
    case WM_LBUTTONUP: ReleaseCapture(); g_app.onMouseButton(0, false); return 0;
    case WM_RBUTTONDOWN: g_app.onMouseButton(1, true); return 0;
    case WM_RBUTTONUP: g_app.onMouseButton(1, false); return 0;
    case WM_MBUTTONDOWN: g_app.onMouseButton(2, true); return 0;
    case WM_MBUTTONUP: g_app.onMouseButton(2, false); return 0;
    case WM_MOUSEMOVE: {
        if (!g_app.input.mouseInWindow) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
        }
        g_app.input.mouseInWindow = true;
        g_app.onMouseMove(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    }
    case WM_MOUSELEAVE:
        g_app.input.mouseInWindow = false;
        return 0;
    case WM_ACTIVATE:
        g_app.input.focused = LOWORD(wp) != WA_INACTIVE;
        if (!g_app.input.focused) {
            memset(g_app.input.keyDown, 0, sizeof(g_app.input.keyDown));
            for (bool& b : g_app.input.down) b = false;
        }
        return 0;
    case WM_MOUSEWHEEL: g_app.onWheel(GET_WHEEL_DELTA_WPARAM(wp)); return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            SetCursor(nullptr); // the game draws its own cursor
            return TRUE;
        }
        break;
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool App::argFlag(const std::string& f) const
{
    for (auto& a : args)
        if (a == f) return true;
    return false;
}

std::string App::argValue(const std::string& f, const std::string& def) const
{
    for (size_t i = 0; i + 1 < args.size(); i++)
        if (args[i] == f) return args[i + 1];
    return def;
}

bool App::init(const std::vector<std::string>& a)
{
    args = a;
    vsync = !argFlag("--no-vsync");
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszClassName = L"FalloutDemoHD";
    RegisterClassExW(&wc);

    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    int w = std::max(1024, sw * 3 / 4), h = std::max(720, sh * 3 / 4);
    if (!argValue("--width").empty()) w = atoi(argValue("--width").c_str());
    if (!argValue("--height").empty()) h = atoi(argValue("--height").c_str());
    RECT r = { 0, 0, w, h };
    DWORD style = WS_OVERLAPPEDWINDOW;
    AdjustWindowRect(&r, style, FALSE);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Fallout Demo HD", style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        LOGE("CreateWindow failed");
        return false;
    }
    m_hwnd = hwnd;
    RECT cr;
    GetClientRect(hwnd, &cr);
    m_winW = std::max(1L, cr.right - cr.left);
    m_winH = std::max(1L, cr.bottom - cr.top);
    if (!g_renderer.init(hwnd, m_winW, m_winH, vsync)) {
        MessageBoxW(hwnd, L"Could not initialize Direct3D 11.", L"Fallout Demo HD", MB_ICONERROR);
        return false;
    }
    if (argFlag("--hidden")) {
        // used by automated screenshots
    } else {
        ShowWindow(hwnd, SW_SHOW);
    }
    if (argFlag("--fullscreen")) toggleFullscreen();
    m_screenshotPath = argValue("--screenshot");
    if (!m_screenshotPath.empty()) m_screenshotFrames = atoi(argValue("--frames", "60").c_str());
    updateCanvases();
    m_lastTime = timeSeconds();
    m_fixedFps = atoi(argValue("--fixed-fps", "0").c_str());
    // --record <file.mp4> [--record-fps 30] [--ffmpeg <path>]
    std::string record = argValue("--record");
    if (!record.empty() && !m_recorder.start(record, m_winW, m_winH, atoi(argValue("--record-fps", "30").c_str()), argValue("--ffmpeg", "ffmpeg")))
        LOGE("Recording could not start");
    return true;
}

void App::shutdown()
{
    m_screens.clear();
    m_dead.clear();
    m_worldRt.reset();
    m_uiRt.reset();
    g_sprites.clear();
    g_renderer.shutdown();
    if (m_hwnd) DestroyWindow((HWND)m_hwnd);
    m_hwnd = nullptr;
}

void App::updateCanvases()
{
    // UI: scale so the 640x480 layout fits the window
    uiScale = std::min((float)m_winW / 640.0f, (float)m_winH / 480.0f);
    if (toolUi) {
        uiScale = std::max(1.0f, floorf((float)m_winH / 720.0f));
    } else if (uiScale > 1.0f && !argFlag("--smooth-ui")) {
        // prefer crisp steps of 1/4 when large
        uiScale = std::max(1.0f, floorf(uiScale * 4.0f) / 4.0f);
    }
    uiScale = std::max(0.5f, uiScale);
    uiW = (int)ceilf((float)m_winW / uiScale);
    uiH = (int)ceilf((float)m_winH / uiScale);
    uiOffsetX = (uiW - 640) / 2;
    uiOffsetY = uiH - 480;
    uiTex = uiScale > 2.0f ? 4 : (uiScale > 1.0f ? 2 : 1);

    worldZoom = std::max(1.0f, uiScale * zoomUser);
    worldW = (int)ceilf((float)m_winW / worldZoom);
    worldH = (int)ceilf((float)m_winH / worldZoom);
    worldTex = worldZoom > 2.0f ? 4 : (worldZoom > 1.0f ? 2 : 1);
    if (argFlag("--no-hd")) worldTex = uiTex = 1;

    int rtW = worldW * worldTex, rtH = worldH * worldTex;
    if (!m_worldRt || m_worldRt->width != rtW || m_worldRt->height != rtH) m_worldRt = g_renderer.createRenderTarget(rtW, rtH, true);
    int uW = uiW * uiTex, uH = uiH * uiTex;
    if (!m_uiRt || m_uiRt->width != uW || m_uiRt->height != uH) m_uiRt = g_renderer.createRenderTarget(uW, uH, true);
    g_view.canvasW = worldW;
    g_view.canvasH = worldH;
    g_view.texScale = worldTex;
}

void App::onResize(int w, int h)
{
    if (w <= 0 || h <= 0) return;
    m_winW = w;
    m_winH = h;
    g_renderer.resize(w, h);
    updateCanvases();
}

void App::onKey(int vk, bool down)
{
    if (vk < 0 || vk >= 256) return;
    if (down && !input.keyDown[vk]) input.keyPressed[vk] = true;
    if (down) input.keyPressed[vk] = true; // auto-repeat also counts as a press
    input.keyDown[vk] = down;
    input.shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    input.ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    input.alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
}

void App::onChar(u32 c) { input.chars.push_back(c); }

void App::onMouseButton(int b, bool down)
{
    if (down && !input.down[b]) {
        input.pressed[b] = true;
        if (b == 0) {
            double now = timeSeconds();
            if (now - m_lastClickTime < 0.35 && abs(input.mouseX - m_lastClickX) < 6 && abs(input.mouseY - m_lastClickY) < 6)
                input.doubleClick = true;
            m_lastClickTime = now;
            m_lastClickX = input.mouseX;
            m_lastClickY = input.mouseY;
        }
    }
    if (!down && input.down[b]) input.released[b] = true;
    input.down[b] = down;
}

void App::onMouseMove(int x, int y)
{
    input.mouseX = x;
    input.mouseY = y;
}

void App::onWheel(int delta) { input.wheel += delta / WHEEL_DELTA; }

void App::setCursorVisible(bool v) { m_cursorVisible = v; }

// Scripted input for automated tests: --auto "30:click 520 190;60:key T;90:shot name.png"
// Coordinates are 640x480 layout coordinates. Keys: letters/digits, or esc/enter/space/tab/left/right/up/down/f1..f12.
void App::runAutomation()
{
    if (!m_autoParsed) {
        m_autoParsed = true;
        for (auto& cmd : strSplit(argValue("--auto"), ';')) {
            std::string c = strTrim(cmd);
            size_t colon = c.find(':');
            if (colon == std::string::npos) continue;
            m_auto.push_back({ atoi(c.substr(0, colon).c_str()), strTrim(c.substr(colon + 1)) });
        }
    }
    m_autoCtrl = m_autoShift = false;
    if (m_glideFrames > 0) {
        // smooth scripted mouse movement
        float t = std::min(1.0f, (float)(m_frameNo - m_glideStart + 1) / (float)m_glideFrames);
        float s = t * t * (3 - 2 * t);
        input.mouseX = (int)lrintf(m_glideFrom[0] + (m_glideTo[0] - m_glideFrom[0]) * s);
        input.mouseY = (int)lrintf(m_glideFrom[1] + (m_glideTo[1] - m_glideFrom[1]) * s);
        input.mouseInWindow = input.focused = true;
        if (t >= 1.0f) m_glideFrames = 0;
    }
    if (m_autoRelease && m_autoReleaseFrame == m_frameNo) {
        int b = m_autoRelease - 1;
        input.down[b] = false;
        input.released[b] = true;
        m_autoRelease = 0;
    }
    for (auto& [frameNo, cmd] : m_auto) {
        // each scripted event fires once (nested modal loops reuse the outer frame number)
        if (frameNo < 0 || frameNo > m_frameNo) continue;
        frameNo = -1;
        auto parts = strSplit(cmd, ' ');
        if (parts.empty()) continue;
        auto toWin = [&](float lx, float ly) {
            input.mouseX = (int)((uiOffsetX + lx) * uiScale);
            input.mouseY = (int)((uiOffsetY + ly) * uiScale);
            input.mouseInWindow = true;
            input.focused = true;
        };
        if ((parts[0] == "click" || parts[0] == "rclick" || parts[0] == "move" || parts[0] == "down") && parts.size() >= 3) {
            toWin((float)atof(parts[1].c_str()), (float)atof(parts[2].c_str()));
            int b = parts[0] == "rclick" ? 1 : 0;
            if (parts[0] == "click" || parts[0] == "rclick" || parts[0] == "down") {
                input.pressed[b] = true;
                input.down[b] = true;
            }
            if (parts[0] == "click" || parts[0] == "rclick") {
                m_autoRelease = b + 1;
                m_autoReleaseFrame = m_frameNo + 1;
            }
        } else if (parts[0] == "glide" && parts.size() >= 4) {
            // glide x y frames: moves the mouse there smoothly
            m_glideFrom[0] = (float)input.mouseX;
            m_glideFrom[1] = (float)input.mouseY;
            m_glideTo[0] = (uiOffsetX + (float)atof(parts[1].c_str())) * uiScale;
            m_glideTo[1] = (uiOffsetY + (float)atof(parts[2].c_str())) * uiScale;
            m_glideStart = m_frameNo;
            m_glideFrames = std::max(1, atoi(parts[3].c_str()));
        } else if (parts[0] == "wheel" && parts.size() >= 2) {
            input.wheel += atoi(parts[1].c_str());
        } else if (parts[0] == "up") {
            input.down[0] = false;
            input.released[0] = true;
        } else if (parts[0] == "key" && parts.size() >= 2) {
            static const std::pair<const char*, int> kNamed[] = { { "esc", VK_ESCAPE }, { "enter", VK_RETURN }, { "space", VK_SPACE },
                { "tab", VK_TAB }, { "left", VK_LEFT }, { "right", VK_RIGHT }, { "up", VK_UP }, { "down", VK_DOWN }, { "del", VK_DELETE },
                { "back", VK_BACK }, { "pgup", VK_PRIOR }, { "pgdn", VK_NEXT }, { "[", VK_OEM_4 }, { "]", VK_OEM_6 },
                { "home", VK_HOME } };
            std::string k = strLower(parts[1]);
            // modifiers: key ctrl+s, key shift+r
            for (;;) {
                if (strStartsWith(k, "ctrl+")) m_autoCtrl = true, k = k.substr(5);
                else if (strStartsWith(k, "shift+")) m_autoShift = true, k = k.substr(6);
                else break;
            }
            int vk = 0;
            for (auto& n : kNamed)
                if (k == n.first) vk = n.second;
            if (!vk && k.size() >= 2 && k[0] == 'f') vk = VK_F1 + atoi(k.c_str() + 1) - 1;
            if (!vk && k.size() == 1) vk = toupper((unsigned char)k[0]);
            if (vk > 0 && vk < 256) input.keyPressed[vk] = true;
        } else if (parts[0] == "type" && cmd.size() > 5) {
            for (char ch : cmd.substr(5)) input.chars.push_back((u8)ch);
        } else if (parts[0] == "shot" && parts.size() >= 2) {
            m_autoShot = parts[1];
        } else if (parts[0] == "dev" && onDevCommand) {
            onDevCommand(strTrim(cmd.substr(3)));
        }
    }
}

void App::setCursor(const std::string& art, int hotX, int hotY, int frame)
{
    m_cursor = art;
    m_cursorHot[0] = hotX;
    m_cursorHot[1] = hotY;
    m_cursorFrame = frame;
}

void App::toggleFullscreen()
{
    HWND hwnd = (HWND)m_hwnd;
    if (!fullscreen) {
        m_savedStyle = GetWindowLongW(hwnd, GWL_STYLE);
        RECT r;
        GetWindowRect(hwnd, &r);
        m_savedRect[0] = r.left;
        m_savedRect[1] = r.top;
        m_savedRect[2] = r.right - r.left;
        m_savedRect[3] = r.bottom - r.top;
        HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfoW(mon, &mi);
        SetWindowLongW(hwnd, GWL_STYLE, (LONG)(m_savedStyle & ~(WS_CAPTION | WS_THICKFRAME)));
        SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
            SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
        fullscreen = true;
    } else {
        SetWindowLongW(hwnd, GWL_STYLE, m_savedStyle);
        SetWindowPos(hwnd, nullptr, m_savedRect[0], m_savedRect[1], m_savedRect[2], m_savedRect[3], SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOOWNERZORDER);
        fullscreen = false;
    }
}

void App::push(std::unique_ptr<Screen> s)
{
    Screen* raw = s.get();
    m_screens.push_back(std::move(s));
    raw->onEnter();
}

void App::replaceAll(std::unique_ptr<Screen> s)
{
    while (!m_screens.empty()) {
        m_screens.back()->onExit();
        m_dead.push_back(std::move(m_screens.back()));
        m_screens.pop_back();
    }
    push(std::move(s));
}

int App::runModal(std::unique_ptr<Screen> s)
{
    Screen* raw = s.get();
    push(std::move(s));
    m_modalDepth++;
    while (!raw->done && !m_quit) {
        if (!frame()) break;
    }
    m_modalDepth--;
    // the input that closed the modal screen must not reach the screen below it
    input.beginFrame();
    int result = raw->result;
    // pop it (it may already have been removed by frame())
    for (size_t i = 0; i < m_screens.size(); i++) {
        if (m_screens[i].get() == raw) {
            m_screens[i]->onExit();
            m_dead.push_back(std::move(m_screens[i]));
            m_screens.erase(m_screens.begin() + (long long)i);
            break;
        }
    }
    return result;
}

bool App::frame()
{
    input.beginFrame();
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) m_quit = true;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (m_quit) return false;
    runAutomation();
    double now = timeSeconds();
    dt = (float)std::min(0.1, now - m_lastTime);
    m_lastTime = now;
    // recordings (and --fixed-fps test runs) advance the game by exactly one video frame per frame
    if (m_recorder.active()) dt = 1.0f / (float)m_recorder.fps();
    else if (m_fixedFps > 0) dt = 1.0f / (float)m_fixedFps;
    time += dt;

    // mouse in canvas coordinates
    input.uiX = (float)input.mouseX / uiScale;
    input.uiY = (float)input.mouseY / uiScale;
    input.worldX = (float)input.mouseX / worldZoom;
    input.worldY = (float)input.mouseY / worldZoom;
    input.shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0 || m_autoShift;
    input.ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0 || m_autoCtrl;
    input.alt = (GetKeyState(VK_MENU) & 0x8000) != 0;

    g_sprites.beginFrame();
    if (g_palette.animate((u32)(time * 1000.0))) g_renderer.updatePalette(g_palette.current());

    Screen* t = top();
    setCursor("stdarrow");
    if (t && !t->done) t->update(dt);
    soundUpdate(); // releases finished sound voices

    // pop finished non-modal screens (modal ones are popped by runModal)
    render();
    if (m_modalDepth == 0) {
        m_dead.clear();
        g_world.collectGarbage();
    }
    g_sprites.trim((size_t)1536 * 1024 * 1024);
    if (!m_autoShot.empty()) {
        saveScreenshot(m_autoShot);
        m_autoShot.clear();
    }
    m_frameNo++;
    if (m_screenshotFrames >= 0 && m_frameNo >= m_screenshotFrames) {
        saveScreenshot(m_screenshotPath);
        m_screenshotFrames = -1;
        if (argFlag("--exit-after-screenshot")) m_quit = true;
    }
    return !m_quit;
}

void App::render()
{
    g_renderer.beginFrame(0xFF000000u);
    // find the lowest screen that needs drawing
    size_t first = 0;
    for (size_t i = m_screens.size(); i-- > 0;) {
        if (!m_screens[i]->transparent()) {
            first = i;
            break;
        }
    }
    bool world = false;
    for (size_t i = first; i < m_screens.size(); i++)
        if (m_screens[i]->showsWorld()) world = true;
    if (world && m_worldRt) {
        g_renderer.beginCanvas(m_worldRt.get(), 0xFF000000u);
        g_view.draw(dt);
        g_renderer.endCanvas();
        g_renderer.blitToScreen(m_worldRt.get(), 0, 0, worldW * worldZoom, worldH * worldZoom, true);
    }
    if (m_uiRt) {
        g_renderer.beginCanvas(m_uiRt.get(), 0x00000000u);
        for (size_t i = first; i < m_screens.size(); i++) m_screens[i]->drawUI();
        if (m_cursorVisible && input.mouseInWindow && !m_cursor.empty()) {
            if (Sprite* s = g_sprites.get("art/intrface/" + m_cursor)) {
                int ts = 1;
                TexturePtr tex = g_sprites.texture(s, uiTex, ts);
                const SheetFrame& f = s->frame(0, m_cursorFrame);
                float S = (float)uiTex;
                float x = floorf(input.uiX) - m_cursorHot[0], y = floorf(input.uiY) - m_cursorHot[1];
                DrawParams cp;
                cp.texScaleDiv = std::max(1, ts / std::max(1, uiTex));
                if (tex) g_renderer.draw(tex, (float)f.x * ts, (float)f.y * ts, (float)f.w * ts, (float)f.h * ts, x * S, y * S, f.w * S, f.h * S, cp);
            }
        }
        g_renderer.endCanvas();
        g_renderer.blitToScreen(m_uiRt.get(), 0, 0, uiW * uiScale, uiH * uiScale, true);
    }
    if (m_recorder.active()) {
        Image shot;
        if (g_renderer.captureBackBuffer(shot)) m_recorder.addFrame(shot);
    }
    g_renderer.endFrame();
}

void App::saveScreenshot(const std::string& path)
{
    Image img;
    if (!g_renderer.captureBackBuffer(img)) return;
    Bytes png = pngEncode(img, 6);
    std::string p = path;
    if (p.empty()) {
        for (int i = 0; i < 10000; i++) {
            p = pathJoin(userDir.empty() ? exeDir() : userDir, strFormat("scr%05d.png", i));
            if (!fileExists(p)) break;
        }
    }
    fileWrite(p, png.data(), png.size());
    LOGI("Screenshot saved to %s", p.c_str());
}

int App::run()
{
    while (!m_quit && frame()) {
        if (m_screens.empty()) break;
    }
    m_recorder.finish();
    return 0;
}

} // namespace fo
