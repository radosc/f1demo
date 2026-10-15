// Application shell: window, input, frame loop, screen stack and canvases.
#pragma once
#include "core/base.h"
#include "gfx/renderer.h"
#include "ui/recorder.h"

namespace fo {

struct Input {
    int mouseX = 0, mouseY = 0;          // window pixels
    float uiX = 0, uiY = 0;              // UI canvas coordinates (game px)
    float worldX = 0, worldY = 0;        // world canvas coordinates (game px)
    bool down[3] = {}, pressed[3] = {}, released[3] = {};
    bool doubleClick = false;
    int wheel = 0;
    bool keyDown[256] = {};
    bool keyPressed[256] = {};
    std::vector<u32> chars;
    bool shift = false, ctrl = false, alt = false;
    bool mouseInWindow = false;
    bool focused = false;
    void beginFrame();
    bool key(int vk) const { return vk >= 0 && vk < 256 && keyPressed[vk]; }
};

class Screen {
public:
    virtual ~Screen() = default;
    virtual void onEnter() {}
    virtual void onExit() {}
    virtual void update(float dt) { (void)dt; }
    // Called when this screen (or one above it that is transparent) wants the world drawn.
    virtual bool showsWorld() const { return false; }
    // Screens below a transparent screen keep being drawn.
    virtual bool transparent() const { return false; }
    virtual void drawUI() {}
    bool done = false;
    int result = 0;
};

class App {
public:
    bool init(const std::vector<std::string>& args);
    void shutdown();
    int run();
    // One iteration: messages, update top screen, render all. Returns false if the app should quit.
    bool frame();
    // Pushes a screen and runs frames until it is done; returns its result.
    int runModal(std::unique_ptr<Screen> s);
    void push(std::unique_ptr<Screen> s);
    void replaceAll(std::unique_ptr<Screen> s);
    Screen* top() const { return m_screens.empty() ? nullptr : m_screens.back().get(); }
    void requestQuit() { m_quit = true; }
    bool quitting() const { return m_quit; }

    Input input;
    float dt = 0;
    double time = 0;

    // canvases
    int uiW = 640, uiH = 480;           // UI canvas in game px
    float uiScale = 1.0f;               // window px per UI game px
    int uiTex = 1;                      // UI texel scale
    int worldW = 640, worldH = 480;
    float worldZoom = 2.0f;             // window px per world game px
    int worldTex = 2;
    float zoomUser = 1.0f;              // user zoom multiplier
    bool toolUi = false;                // tools (map editor): finer interface scale with more room than 640x480
    int uiOffsetX = 0, uiOffsetY = 0;   // 640x480 layout origin inside the UI canvas (centered)

    bool fullscreen = false;
    bool vsync = true;
    std::string dataDir, modsDir, userDir;
    std::vector<std::string> args;
    bool argFlag(const std::string& f) const;
    std::string argValue(const std::string& f, const std::string& def = "") const;
    void toggleFullscreen();
    void* hwnd() const { return m_hwnd; }
    void saveScreenshot(const std::string& path);
    void setCursorVisible(bool v);
    // Mouse cursor: interface art (art/intrface/<name>) with its hot spot in art pixels.
    // Reset to the standard arrow every frame before the top screen's update().
    void setCursor(const std::string& art, int hotX = 0, int hotY = 0, int frame = 0);
    int frameNo() const { return m_frameNo; }
    // Developer commands ("dev <command>" lines of --auto scripts); set by the game screen.
    std::function<void(const std::string&)> onDevCommand;
    // Closing the window: returns true when the active screen handles it (e.g. asks to save first).
    std::function<bool()> onClose;
    int screenCount() const { return (int)m_screens.size(); }

    // internal (window procedure)
    void onResize(int w, int h);
    void onKey(int vk, bool down);
    void onChar(u32 c);
    void onMouseButton(int b, bool down);
    void onMouseMove(int x, int y);
    void onWheel(int delta);

private:
    void updateCanvases();
    void render();
    void runAutomation();
    bool m_autoParsed = false;
    std::vector<std::pair<int, std::string>> m_auto;
    int m_autoRelease = 0, m_autoReleaseFrame = -1;
    bool m_autoCtrl = false, m_autoShift = false; // modifiers of this frame's scripted key
    float m_glideFrom[2] = {}, m_glideTo[2] = {};   // scripted mouse glide (window px)
    int m_glideStart = 0, m_glideFrames = 0;
    Recorder m_recorder;
    int m_fixedFps = 0; // --fixed-fps: constant time step for scripted runs
    std::string m_autoShot;
    void* m_hwnd = nullptr;
    std::vector<std::unique_ptr<Screen>> m_screens;
    std::vector<std::unique_ptr<Screen>> m_dead;
    std::unique_ptr<RenderTarget> m_worldRt, m_uiRt;
    bool m_quit = false;
    double m_lastTime = 0;
    double m_lastClickTime = 0;
    int m_lastClickX = 0, m_lastClickY = 0;
    int m_winW = 1280, m_winH = 720;
    long m_savedStyle = 0;
    int m_savedRect[4] = {};
    int m_screenshotFrames = -1;
    std::string m_screenshotPath;
    int m_frameNo = 0;
    int m_modalDepth = 0;
    std::string m_cursor = "stdarrow";
    int m_cursorHot[2] = {};
    int m_cursorFrame = 0;
    bool m_cursorVisible = true;
};

extern App g_app;

} // namespace fo
