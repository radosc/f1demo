// Game launcher: locates data, runs the first-time conversion, starts the app.
#include "core/base.h"
#include "core/fs.h"
#include "convert/convert.h"
#include "audio/sound.h"
#include "game/art.h"
#include "game/combat.h"
#include "game/game.h"
#include "game/script.h"
#include "game/settings.h"
#include "gfx/palette.h"
#include "ui/app.h"
#include "ui/screens.h"

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>

#include <atomic>
#include <mutex>
#include <thread>

namespace fo {

// Runs the first-start conversion on a worker thread while a small window shows its progress
// (the sprite upscaling takes a minute or two).
static bool convertWithProgress(const ConvertOptions& base)
{
    struct Shared {
        std::mutex lock;
        std::string stage;
        float progress = 0;
        std::atomic<bool> finished { false };
        bool ok = false;
    } st;
    ConvertOptions opt = base;
    opt.onProgress = [&st](const std::string& stage, float p) {
        std::lock_guard<std::mutex> guard(st.lock);
        st.stage = stage;
        st.progress = p;
    };
    std::thread worker([&] {
        st.ok = convertGameData(opt);
        st.finished = true;
    });

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_PROGRESS_CLASS };
    InitCommonControlsEx(&icc);
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_APPSTARTING);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"FalloutDemoHDConvert";
    RegisterClassExW(&wc);
    UINT dpi = GetDpiForSystem();
    auto px = [dpi](int v) { return MulDiv(v, (int)dpi, 96); };
    DWORD style = WS_POPUP | WS_CAPTION;
    RECT r = { 0, 0, px(460), px(124) };
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, dpi);
    int w = r.right - r.left, h = r.bottom - r.top;
    HWND win = CreateWindowExW(0, wc.lpszClassName, L"Fallout Demo HD", style, (GetSystemMetrics(SM_CXSCREEN) - w) / 2,
        (GetSystemMetrics(SM_CYSCREEN) - h) / 2, w, h, nullptr, nullptr, inst, nullptr);
    NONCLIENTMETRICSW ncm = {};
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi);
    HFONT font = CreateFontIndirectW(&ncm.lfMessageFont);
    HWND info = CreateWindowExW(0, L"STATIC",
        L"First start: converting the demo data and drawing the high-resolution sprites. This happens only once.", WS_CHILD | WS_VISIBLE,
        px(16), px(12), px(428), px(40), win, nullptr, inst, nullptr);
    HWND bar = CreateWindowExW(0, PROGRESS_CLASSW, nullptr, WS_CHILD | WS_VISIBLE, px(16), px(58), px(428), px(20), win, nullptr, inst, nullptr);
    HWND stage = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, px(16), px(90), px(428), px(22), win, nullptr, inst, nullptr);
    for (HWND c : { info, stage }) SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageW(bar, PBM_SETRANGE32, 0, 1000);
    // the game window stays hidden meanwhile
    HWND game = (HWND)g_app.hwnd();
    bool gameVisible = game && IsWindowVisible(game);
    if (gameVisible) ShowWindow(game, SW_HIDE);
    ShowWindow(win, g_app.argFlag("--hidden") ? SW_SHOWNOACTIVATE : SW_SHOW); // automated runs must not take the focus

    // stage -> position of the stage's start on the bar, and its label (the art stage takes most of the time)
    static const struct {
        const char* stage;
        float start, end;
        const wchar_t* label;
    } kStages[] = {
        { "text", 0.00f, 0.01f, L"Texts" }, { "scripts", 0.01f, 0.02f, L"Scripts" }, { "misc", 0.02f, 0.05f, L"Fonts and characters" },
        { "protos", 0.05f, 0.06f, L"Object prototypes" }, { "maps", 0.06f, 0.08f, L"Maps" }, { "sound", 0.08f, 0.12f, L"Sounds and music" },
        { "art", 0.12f, 1.00f, L"Sprites" }, { "done", 1.00f, 1.00f, L"Done" },
    };
    std::wstring shownLabel;
    int shownPos = -1;
    while (!st.finished) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        std::string cur;
        float p;
        {
            std::lock_guard<std::mutex> guard(st.lock);
            cur = st.stage;
            p = st.progress;
        }
        for (auto& s : kStages) {
            if (cur != s.stage) continue;
            int pos = (int)((s.start + (s.end - s.start) * std::min(1.0f, p)) * 1000);
            std::wstring label = s.label;
            if (cur == "art") label += L" (" + std::to_wstring((int)(p * 100)) + L"%)";
            if (pos != shownPos) SendMessageW(bar, PBM_SETPOS, (WPARAM)(shownPos = pos), 0);
            if (label != shownLabel) SetWindowTextW(stage, (shownLabel = label).c_str());
        }
        Sleep(30);
    }
    worker.join();
    DestroyWindow(win);
    DeleteObject(font);
    UnregisterClassW(wc.lpszClassName, inst);
    if (gameVisible) ShowWindow(game, SW_SHOW);
    return st.ok;
}

static std::string findDataDir()
{
    std::string exe = exeDir();
    const char* candidates[] = { "data", "../data", "../../data" };
    for (auto* c : candidates) {
        std::string d = pathJoin(exe, c);
        if (fileExists(pathJoin(d, "manifest.json"))) return d;
    }
    return {};
}

static std::string findDat()
{
    std::string exe = exeDir();
    const char* candidates[] = { "Falldemo.dat", "../Falldemo.dat", "../../Falldemo.dat", "falldemo/Falldemo.dat" };
    for (auto* c : candidates) {
        std::string d = pathJoin(exe, c);
        if (fileExists(d)) return d;
    }
    return {};
}

static bool initData()
{
    g_app.dataDir = g_app.argValue("--data", findDataDir());
    if (g_app.dataDir.empty()) {
        std::string dat = findDat();
        if (dat.empty()) {
            // ask the player where the demo is
            MessageBoxW(nullptr,
                L"The game data has not been converted yet and FALLDEMO.DAT was not found next to the game.\n\n"
                L"Please select FALLDEMO.DAT from your copy of the Fallout demo.",
                L"Fallout Demo HD", MB_ICONINFORMATION);
            wchar_t file[MAX_PATH] = L"Falldemo.dat";
            OPENFILENAMEW ofn = {};
            ofn.lStructSize = sizeof(ofn);
            ofn.lpstrFilter = L"Fallout demo data (Falldemo.dat)\0Falldemo.dat;*.dat\0All files\0*.*\0";
            ofn.lpstrFile = file;
            ofn.nMaxFile = MAX_PATH;
            ofn.lpstrTitle = L"Select FALLDEMO.DAT";
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            if (GetOpenFileNameW(&ofn)) dat = wideToUtf8(file);
        }
        if (dat.empty()) {
            MessageBoxW(nullptr,
                L"Converted game data was not found and FALLDEMO.DAT could not be located.\n\n"
                L"Place FalloutDemoHD.exe next to (or below) the folder containing FALLDEMO.DAT, or run:\n"
                L"FalloutDemoHD.exe --convert <path to FALLDEMO.DAT>",
                L"Fallout Demo HD", MB_ICONERROR);
            return false;
        }
        // converted data goes next to the executable's folder (<game>in\..\data), where findDataDir looks
        std::string out = pathJoin(exeDir(), "../data");
        LOGI("First run: converting %s -> %s", dat.c_str(), out.c_str());
        ConvertOptions opt;
        opt.datPath = dat;
        opt.outDir = out;
        if (!convertWithProgress(opt)) {
            MessageBoxW(nullptr, L"Converting FALLDEMO.DAT failed - see fo1hd.log next to FalloutDemoHD.exe.", L"Fallout Demo HD", MB_ICONERROR);
            return false;
        }
        g_app.dataDir = out;
    }
    g_app.modsDir = pathJoin(pathDir(g_app.dataDir), "mods");
    g_app.userDir = pathJoin(pathDir(g_app.dataDir), "user");
    dirCreate(g_app.userDir);
    g_vfs.setBaseDir(g_app.dataDir);
    for (auto& m : g_vfs.discoverMods(g_app.modsDir)) {
        g_vfs.pushLayer(m.dir);
        LOGI("Mod enabled: %s (%s)", m.name.c_str(), m.dir.c_str());
    }
    Bytes pal;
    if (!g_vfs.read("color.pal", pal) || !g_palette.load(pal)) {
        LOGE("color.pal missing in %s", g_app.dataDir.c_str());
        return false;
    }
    g_art.load();
    return true;
}

int runGame(const std::vector<std::string>& args)
{
    if (!g_app.init(args)) return 1;
    if (!initData()) {
        g_app.shutdown();
        return 1;
    }
    g_renderer.updatePalette(g_palette.current());
    soundInit();
    if (g_app.argFlag("--mute")) soundSetMuted(true);
    settingsLoad();
    g_combatLog = g_app.argFlag("--combat-log");
    if (!g_app.argFlag("--hidden")) {
        if (g_settings.fullscreen && !g_app.fullscreen) g_app.toggleFullscreen();
        g_renderer.setVsync(g_settings.vsync);
    }
    registerGameOpcodes();
    extern void installDialogUi();
    installDialogUi();
    gameLoadGlobals();

    std::string map = g_app.argValue("--map");
    if (g_app.argFlag("--editor")) {
        // --editor [map] [--mod <folder>] [--save-as <map>]
        std::string editMap = g_app.argValue("--editor");
        if (editMap.empty() || strStartsWith(editMap, "--")) editMap = "junkdemo";
        g_app.push(makeEditorScreen(editMap, g_app.argValue("--mod", "my-maps"), g_app.argValue("--save-as")));
    } else if (!map.empty()) {
        gameNewPlayer(g_app.argValue("--premade", "combat"));
        if (!gameEnterMap(map)) {
            MessageBoxW(nullptr, L"Map could not be loaded.", L"Fallout Demo HD", MB_ICONERROR);
        }
        // developer aid: --test-talk <script name> opens a conversation with that script's owner
        std::string talk = g_app.argValue("--test-talk");
        if (!talk.empty()) {
            Object* found = nullptr;
            g_world.forEach([&](Object* o) {
                if (!found && o->script && strIEquals(o->script->name(), talk)) found = o;
            });
            if (found) gameRequestDialog(found);
            else LOGW("--test-talk: no object with script %s", talk.c_str());
        }
        // developer aid: --test-attack <script name> makes that script's owner attack the player
        std::string attackerName = g_app.argValue("--test-attack");
        if (!attackerName.empty()) {
            Object* found = nullptr;
            g_world.forEach([&](Object* o) {
                if (!found && o->isCritter() && o->script && strIEquals(o->script->name(), attackerName)) found = o;
            });
            if (found && g_world.dude) combatScriptAttack(found, g_world.dude, 0, 1, 0, 0, 0x7FFFFFFF, 0, 0);
            else LOGW("--test-attack: no critter with script %s", attackerName.c_str());
        }
        g_app.push(makeGameScreen());
    } else {
        g_app.push(makeMainMenu());
    }
    int rc = g_app.run();
    gameEnd();
    soundShutdown();
    g_app.shutdown();
    return rc;
}

} // namespace fo
