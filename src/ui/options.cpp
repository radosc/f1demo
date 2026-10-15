// Options menu (opbase + opbtnon/opbtnoff), the preferences screen (prefback with the demo's pref* buttons and
// sliders), the SAVE GAME / LOAD GAME screens with the description box (lsgbox), and the stand-alone LOAD GAME
// screen of the main menu.
//
// The demo has no full-screen load/save background (lsgame.frm) and its preferences background is a plain
// panel, so those two screens are composed from the demo's panel and button art.
#include "ui/screens.h"
#include "ui/uikit.h"
#include "audio/sound.h"
#include "core/fs.h"
#include "core/png.h"
#include "game/game.h"
#include "game/msg.h"
#include "game/settings.h"
#include "gfx/palette.h"

#include <windows.h>

namespace fo {

namespace {

constexpr u32 kLabel = 0xFF948C18u;     // colorTable[18979]
constexpr u32 kLabelDark = 0xFF736318u; // colorTable[14723]
constexpr u32 kSelected = 0xFFFFFF5Au;  // colorTable[32747]
constexpr u32 kListText = 0xFF00FF00u;  // colorTable[992]
constexpr u32 kListDim = 0xFF00A400u;
constexpr u32 kEdgeOuter = 0xFF1C1C1Cu; // bevel colours of the demo's pref buttons
constexpr u32 kEdgeLight = 0xFF714924u;
constexpr u32 kEdgeDark = 0xFF49241Cu;
constexpr u32 kInsetFill = 0xFF040C00u; // lsgbox text field
constexpr u32 kPanelFill = 0xFF383020u; // prefback base colour (fallback when the art is missing)
constexpr u32 kDim = 0xA0000000u;

// prefback.frm (468x375), centered; used by the preferences and the load/save screens.
constexpr float kPanelW = 468, kPanelH = 375;
constexpr float kPanelX = 86, kPanelY = 52;

constexpr int kMaxDescription = 29;   // LOAD_SAVE_DESCRIPTION_LENGTH - 1
constexpr int kDescriptionWidth = 206; // fits the slot list column

std::string msgOr(const char* file, int num, const char* fallback)
{
    std::string s = num >= 0 ? gameMsg(file, num) : std::string();
    return s.empty() || s == "Error" ? std::string(fallback) : s;
}

// Line height like the original's text_height() (glyph height + line spacing).
int textHeight(int fontId)
{
    Font* f = font(fontId);
    return f ? f->lineHeight() : 10;
}

std::string clipText(int fontId, std::string s, int width)
{
    while (!s.empty() && ui::textWidth(fontId, s) > width) s.pop_back();
    return s;
}

float artWOr(const char* name, float fallback) { return ui::artW(name) > 0 ? (float)ui::artW(name) : fallback; }
float artHOr(const char* name, float fallback) { return ui::artH(name) > 0 ? (float)ui::artH(name) : fallback; }

void dimScreen() { ui::fill(-ui::originX(), -ui::originY(), (float)g_app.uiW, (float)g_app.uiH, kDim); }

void bevel(float x, float y, float w, float h, bool raised)
{
    u32 tl = raised ? kEdgeLight : kEdgeDark, br = raised ? kEdgeDark : kEdgeLight;
    ui::outline(x, y, w, h, kEdgeOuter);
    ui::fill(x + 1, y + 1, w - 2, 1, tl);
    ui::fill(x + 1, y + 1, 1, h - 2, tl);
    ui::fill(x + 1, y + h - 2, w - 2, 1, br);
    ui::fill(x + w - 2, y + 1, 1, h - 2, br);
}

void panel(float x, float y)
{
    if (ui::art("prefback")) ui::draw("prefback", x, y);
    else ui::fill(x, y, kPanelW, kPanelH, kPanelFill);
    bevel(x, y, kPanelW, kPanelH, true);
}

void inset(float x, float y, float w, float h)
{
    ui::fill(x, y, w, h, kInsetFill);
    bevel(x, y, w, h, false);
}

// Little red button with a label to its right (like the original DONE/CANCEL pairs).
bool redButton(const std::string& label, float x, float y, int hotkey)
{
    bool hit = ui::button("lilredup", "lilreddn", x, y, hotkey, ui::SND_SMALL);
    ui::text(103, label, x + 22, y - 1, kLabel);
    return hit;
}

// Pushes the preferences into the game, including the port's display options.
void applySettingsLive()
{
    g_settings.apply();
    if (g_app.fullscreen != g_settings.fullscreen) g_app.toggleFullscreen();
    if (g_app.vsync != g_settings.vsync) {
        g_app.vsync = g_settings.vsync;
        g_renderer.setVsync(g_app.vsync);
    }
}

// Screens that describe their interface in one function run by both passes.
class OptPanel : public Screen {
public:
    void update(float dt) override
    {
        m_time += dt;
        {
            ui::Pass pass(false);
            layout();
        }
        if (!done) afterLogic();
    }
    void drawUI() override
    {
        // the game screen below sets its world cursor while drawing; ours is drawn later and wins
        g_app.setCursor("stdarrow", 0, 0);
        ui::Pass pass(true);
        layout();
    }

protected:
    virtual void layout() = 0;
    // Runs after the logic pass: the place for modal sub-screens (never opened from inside a pass).
    virtual void afterLogic() {}
    // Modal windows swallow every click.
    void swallowClicks()
    {
        if (!ui::drawing() && (g_app.input.pressed[0] || g_app.input.pressed[1])) ui::takeMouse();
    }
    float m_time = 0;
};

// ------------------------------------------------------------------ description box (lsgbox)
class DescriptionBox : public OptPanel {
public:
    DescriptionBox(const std::string& initial, std::string* out, bool* ok) : m_text(initial), m_out(out), m_ok(ok)
    {
        if (m_ok) *m_ok = false;
        if ((int)m_text.size() > kMaxDescription) m_text.resize(kMaxDescription);
    }
    bool transparent() const override { return true; }

protected:
    void layout() override
    {
        using namespace ui;
        float w = artWOr("lsgbox", 290), h = artHOr("lsgbox", 85);
        float x = 169, y = 116; // position of the original in 640x480
        if (art("lsgbox")) {
            draw("lsgbox", x, y);
        } else {
            fill(x, y, w, h, kPanelFill);
            bevel(x, y, w, h, true);
            inset(x + 19, y + 30, 250, 19);
        }
        std::string title = msgOr("lsgame", 130, "DESCRIPTION");
        text(103, title, x + floorf((w - textWidth(103, title)) / 2), y + 7, kLabel);
        text(103, msgOr("lsgame", 104, "DONE"), x + 56, y + 57, kLabel);
        text(103, msgOr("lsgame", 105, "CANCEL"), x + 181, y + 57, kLabel);
        if (button("lilredup", "lilreddn", x + 34, y + 58, VK_RETURN, SND_SMALL)) finish(true);
        if (button("lilredup", "lilreddn", x + 160, y + 58, VK_ESCAPE, SND_SMALL)) finish(false);

        float tx = x + 24, ty = y + 35;
        text(101, m_text, tx, ty, kListText);
        if (fmodf(m_time, 0.5f) < 0.25f) {
            // block cursor in the space after the text, like the original
            int cw = std::max(2, textWidth(101, "_") - 4);
            float cx = tx + (float)(textWidth(101, m_text + " ") - cw);
            fill(cx, ty, (float)cw, (float)(textHeight(101) - 2), kListText);
        }
        if (!drawing() && !done) typeText();
        swallowClicks();
    }

private:
    void typeText()
    {
        for (u32 c : ui::chars()) {
            if (c < 32 || c == 127 || c > 255) continue;
            if ((int)m_text.size() >= kMaxDescription) break;
            std::string t = m_text + (char)(unsigned char)c;
            if (ui::textWidth(101, t) > kDescriptionWidth) break;
            m_text = t;
        }
        if (ui::key(VK_BACK) && !m_text.empty()) m_text.pop_back();
    }
    void finish(bool ok)
    {
        if (ok) {
            if (m_out) *m_out = m_text;
            if (m_ok) *m_ok = true;
        }
        done = true;
    }
    std::string m_text;
    std::string* m_out;
    bool* m_ok;
};

// ------------------------------------------------------------------ save / load game
class SaveLoadScreen : public OptPanel {
public:
    // `standalone`: opened from the main menu - full screen, no game running.
    SaveLoadScreen(bool save, bool standalone, bool* ok) : m_save(save), m_standalone(standalone), m_ok(ok)
    {
        if (m_ok) *m_ok = false;
    }
    bool transparent() const override { return !m_standalone; }

    void onEnter() override
    {
        refresh();
        m_cursor = 0;
        if (m_standalone) {
            for (int i = kSaveSlots - 1; i >= 0; i--)
                if (m_slots[i].used) m_cursor = i;
        } else {
            m_cursor = clampv(gameLastSaveSlot(), 0, kSaveSlots - 1);
        }
    }
    void onExit() override { m_thumbs.clear(); }

protected:
    void layout() override
    {
        using namespace ui;
        if (m_standalone) {
            letterbox(kBlack);
            fill(0, 0, 640, 480, kBlack);
        } else {
            dimScreen();
        }
        const float px = kPanelX, py = kPanelY;
        panel(px, py);
        text(103, m_save ? msgOr("lsgame", 102, "SAVE GAME") : msgOr("lsgame", 100, "LOAD GAME"), px + 20, py + 10, kLabel);

        // slot list: "[   SAVE GAME 01:   ]" and the description below
        const float lx = px + 14, ly = py + 34, lw = 222, lh = 318;
        inset(lx, ly, lw, lh);
        int th = textHeight(101);
        std::string word = m_save ? msgOr("lsgame", 109, "SAVE GAME") : msgOr("lsgame", 110, "LOAD GAME");
        std::string empty = "       " + msgOr("lsgame", 111, "- EMPTY -");
        for (int i = 0; i < kSaveSlots; i++) {
            float sy = py + 40 + 31.0f * i;
            const SaveSlotInfo& s = m_slots[i];
            u32 c = i == m_cursor ? kSelected : kListText;
            text(101, strFormat("[   %s %.2d:   ]", word.c_str(), i + 1), lx + 8, sy, c);
            text(101, clipText(101, s.used ? s.description : empty, (int)lw - 16), lx + 8, sy + th, c);
            if (area(lx + 2, sy - 3, lw - 4, 31, 0, SND_NONE)) clickSlot(i);
        }
        if (!drawing() && g_app.input.wheel && hovered(lx, ly, lw, lh)) moveCursor(g_app.input.wheel > 0 ? -1 : 1);
        if (key(VK_UP)) moveCursor(-1);
        if (key(VK_DOWN)) moveCursor(1);
        if (key(VK_HOME)) m_cursor = 0;
        if (key(VK_END)) m_cursor = kSaveSlots - 1;

        // preview and details of the selected slot
        const SaveSlotInfo& cur = m_slots[m_cursor];
        const float vx = px + 246, vy = py + 34, vw = 208, vh = 124;
        inset(vx, vy, vw, vh);
        if (drawing()) drawThumbnail(cur, vx + 2, vy + 2, vw - 4, vh - 4);
        const float ix = px + 246, iy = py + 166, iw = 208, ih = 150;
        inset(ix, iy, iw, ih);
        float ty = iy + 8;
        auto info = [&](const std::string& s, u32 color) {
            if (s.empty()) return;
            for (auto& l : wrap(101, s, (int)iw - 16)) {
                if (ty + th > iy + ih - 4) return;
                text(101, l, ix + 8, ty, color);
                ty += (float)th;
            }
        };
        if (cur.used) {
            info(cur.playerName, kListText);
            info(cur.gameDate, kListText);
            info(cur.mapName, kListText);
            ty += (float)th;
            info(cur.savedAt, kListDim);
        } else {
            info(msgOr("lsgame", 114, "Empty."), kListText);
        }

        if (redButton(msgOr("lsgame", 104, "DONE"), px + 262, py + 334, VK_RETURN)) m_confirm = true;
        if (redButton(msgOr("lsgame", 105, "CANCEL"), px + 362, py + 334, VK_ESCAPE)) done = true;
        swallowClicks();
    }

    void afterLogic() override
    {
        if (!m_confirm) return;
        m_confirm = false;
        if (m_save) saveSlot();
        else loadSlot();
    }

private:
    void refresh()
    {
        m_slots.assign((size_t)kSaveSlots, SaveSlotInfo());
        for (int i = 0; i < kSaveSlots; i++) m_slots[i].slot = i;
        for (auto& s : gameListSaves())
            if (s.slot >= 0 && s.slot < kSaveSlots) m_slots[s.slot] = s;
        m_thumbs.clear();
    }

    void moveCursor(int d)
    {
        m_cursor = clampv(m_cursor + d, 0, kSaveSlots - 1);
        m_lastClick = -1;
    }

    // Single click selects, a second click on the same slot confirms.
    void clickSlot(int i)
    {
        if (i == m_cursor && i == m_lastClick && g_app.time - m_lastClickTime < 0.5) {
            soundPlaySfx("ib1p1xx1");
            m_lastClick = -1;
            m_confirm = true;
            return;
        }
        m_cursor = i;
        m_lastClick = i;
        m_lastClickTime = g_app.time;
    }

    void saveSlot()
    {
        SaveSlotInfo s = m_slots[m_cursor];
        if (s.used) {
            bool yes = false;
            g_app.runModal(makeMessageBox({ msgOr("lsgame", 131, "Save game already exists, overwrite?") }, true, &yes));
            if (!yes) return;
        }
        std::string description = s.used ? s.description : std::string();
        bool ok = false;
        g_app.runModal(std::make_unique<DescriptionBox>(description, &description, &ok));
        if (!ok) return;
        if (gameSave(s.slot, description)) {
            if (m_ok) *m_ok = true;
            done = true;
            return;
        }
        soundPlaySfx("iisxxxx1");
        g_app.runModal(makeMessageBox({ msgOr("lsgame", 132, "Error saving game!"), msgOr("lsgame", 133, "Unable to save game.") }));
        refresh();
    }

    void loadSlot()
    {
        SaveSlotInfo s = m_slots[m_cursor];
        if (!s.used) return;
        if (gameLoad(s.slot)) {
            if (m_ok) *m_ok = true;
            done = true;
            return;
        }
        soundPlaySfx("iisxxxx1");
        g_app.runModal(makeMessageBox({ msgOr("lsgame", 134, "Error loading game!"), msgOr("lsgame", 135, "Unable to load game.") }));
        refresh();
    }

    TexturePtr thumbnail(const SaveSlotInfo& s)
    {
        auto it = m_thumbs.find(s.slot);
        if (it != m_thumbs.end()) return it->second;
        TexturePtr tex;
        Bytes data;
        Image img;
        if (!s.thumbnail.empty() && fileRead(s.thumbnail, data) && pngDecode(data.data(), data.size(), img) && img.width > 0 && img.height > 0) {
            if (img.indexed) {
                // thumbnails carry their own palette: expand to RGBA
                Image rgba;
                rgba.allocRGBA(img.width, img.height);
                for (size_t i = 0; i < img.pixels.size() && i * 4 + 3 < rgba.pixels.size(); i++) {
                    u8 idx = img.pixels[i];
                    u32 c = idx < img.palette.size() ? img.palette[idx] : 0xFF000000u;
                    u8* p = &rgba.pixels[i * 4];
                    p[0] = (u8)(c >> 16);
                    p[1] = (u8)(c >> 8);
                    p[2] = (u8)c;
                    p[3] = (u8)(c >> 24);
                }
                img = std::move(rgba);
            }
            tex = g_renderer.createTexture(img);
        }
        m_thumbs[s.slot] = tex;
        return tex;
    }

    void drawThumbnail(const SaveSlotInfo& s, float x, float y, float w, float h)
    {
        if (!s.used) return;
        TexturePtr t = thumbnail(s);
        if (!t || t->width <= 0 || t->height <= 0) return;
        float k = std::min(w / (float)t->width, h / (float)t->height);
        float dw = floorf((float)t->width * k), dh = floorf((float)t->height * k);
        float dx = x + floorf((w - dw) / 2), dy = y + floorf((h - dh) / 2);
        float S = ui::texScale();
        g_renderer.draw(t, 0, 0, (float)t->width, (float)t->height, (ui::originX() + dx) * S, (ui::originY() + dy) * S, dw * S, dh * S);
    }

    bool m_save;
    bool m_standalone;
    bool* m_ok;
    std::vector<SaveSlotInfo> m_slots;
    int m_cursor = 0;
    bool m_confirm = false;
    int m_lastClick = -1;
    double m_lastClickTime = 0;
    std::map<int, TexturePtr> m_thumbs;
};

// ------------------------------------------------------------------ preferences
struct Choice {
    int msg;
    const char* fallback;
};

struct RadioPref {
    int titleMsg;
    const char* titleFallback;
    std::vector<Choice> choices;
    int (*get)(const Settings&);
    void (*set)(Settings&, int);
};

struct SliderPref {
    int titleMsg;
    const char* titleFallback;
    float minV, maxV;
    bool inverted; // left end = maximum
    float (*get)(const Settings&);
    void (*set)(Settings&, float);
    std::string (*valueText)(const Settings&);
};

const std::vector<RadioPref>& radioPrefs()
{
    static const std::vector<RadioPref> kPrefs = {
        { 300, "Game Difficulty", { { 402, "Easy" }, { 403, "Normal" }, { -1, "Hard" } },
            [](const Settings& s) { return s.gameDifficulty; }, [](Settings& s, int v) { s.gameDifficulty = v; } },
        { 305, "Combat Difficulty", { { 407, "Wimpy" }, { 403, "Normal" }, { 408, "Rough" } },
            [](const Settings& s) { return s.combatDifficulty; }, [](Settings& s, int v) { s.combatDifficulty = v; } },
        { 304, "Violence Level", { { 404, "None" }, { 405, "Minimal" }, { 403, "Normal" }, { 406, "Maximum Blood" } },
            [](const Settings& s) { return s.violenceLevel; }, [](Settings& s, int v) { s.violenceLevel = v; } },
        { 303, "Target Hi-Lite", { { 401, "Off" }, { 400, "On" }, { -1, "Targeting" } },
            [](const Settings& s) { return s.targetHighlight; }, [](Settings& s, int v) { s.targetHighlight = v; } },
        { -1, "Combat Messages", { { -1, "Verbose" }, { -1, "Brief" } },
            [](const Settings& s) { return s.combatMessagesVerbose ? 0 : 1; }, [](Settings& s, int v) { s.combatMessagesVerbose = v == 0; } },
        { -1, "Combat Taunts", { { 401, "Off" }, { 400, "On" } },
            [](const Settings& s) { return s.combatTaunts ? 1 : 0; }, [](Settings& s, int v) { s.combatTaunts = v != 0; } },
        { 309, "Running", { { 403, "Normal" }, { 410, "Always" } },
            [](const Settings& s) { return s.alwaysRun ? 1 : 0; }, [](Settings& s, int v) { s.alwaysRun = v != 0; } },
        { -1, "Language Filter", { { 401, "Off" }, { 400, "On" } },
            [](const Settings& s) { return s.languageFilter ? 1 : 0; }, [](Settings& s, int v) { s.languageFilter = v != 0; } },
        { 317, "Subtitles", { { 401, "Off" }, { 400, "On" } },
            [](const Settings& s) { return s.subtitles ? 1 : 0; }, [](Settings& s, int v) { s.subtitles = v != 0; } },
        { 308, "Item Hi-Lite", { { 401, "Off" }, { 400, "On" } },
            [](const Settings& s) { return s.itemHighlight ? 1 : 0; }, [](Settings& s, int v) { s.itemHighlight = v != 0; } },
        // port extras
        { -1, "HD Sprites", { { 401, "Off" }, { 400, "On" } },
            [](const Settings& s) { return s.hdSprites ? 1 : 0; }, [](Settings& s, int v) { s.hdSprites = v != 0; } },
        { -1, "Fullscreen", { { 401, "Off" }, { 400, "On" } },
            [](const Settings& s) { return s.fullscreen ? 1 : 0; }, [](Settings& s, int v) { s.fullscreen = v != 0; } },
        { -1, "VSync", { { 401, "Off" }, { 400, "On" } },
            [](const Settings& s) { return s.vsync ? 1 : 0; }, [](Settings& s, int v) { s.vsync = v != 0; } },
    };
    return kPrefs;
}

std::string volumeText(float v) { return v <= 0.005f ? msgOr("options", 401, "Off") : strFormat("%d%%", (int)lroundf(v * 100.0f)); }

const std::vector<SliderPref>& sliderPrefs()
{
    static const std::vector<SliderPref> kPrefs = {
        { -1, "Combat Speed", 0.0f, 50.0f, false, [](const Settings& s) { return (float)s.combatSpeed; },
            [](Settings& s, float v) { s.combatSpeed = clampv((int)lroundf(v), 0, 50); },
            [](const Settings& s) {
                if (s.combatSpeed <= 0) return msgOr("options", 403, "Normal");
                if (s.combatSpeed >= 50) return std::string("Fastest");
                return strFormat("+%d%%", s.combatSpeed * 2);
            } },
        { -1, "Text Delay", 1.0f, 6.0f, true, [](const Settings& s) { return s.textDelay; },
            [](Settings& s, float v) { s.textDelay = clampv(roundf(v * 10.0f) / 10.0f, 1.0f, 6.0f); },
            [](const Settings& s) { return strFormat("%.1f sec", s.textDelay); } },
        { 315, "Brightness Level", 1.0f, 1.18f, false, [](const Settings& s) { return s.brightness; },
            [](Settings& s, float v) { s.brightness = clampv(v, 1.0f, 1.18f); },
            [](const Settings& s) {
                int p = (int)lroundf((s.brightness - 1.0f) * 100.0f);
                return p <= 0 ? msgOr("options", 403, "Normal") : strFormat("+%d%%", p);
            } },
        { 316, "Mouse Sensitivity", 1.0f, 2.5f, false, [](const Settings& s) { return s.mouseSensitivity; },
            [](Settings& s, float v) { s.mouseSensitivity = clampv(roundf(v * 10.0f) / 10.0f, 1.0f, 2.5f); },
            [](const Settings& s) { return strFormat("%.1fx", s.mouseSensitivity); } },
        { 311, "Master Audio Volume", 0.0f, 1.0f, false, [](const Settings& s) { return s.masterVolume; },
            [](Settings& s, float v) { s.masterVolume = clampv(v, 0.0f, 1.0f); }, [](const Settings& s) { return volumeText(s.masterVolume); } },
        { 312, "Music Volume", 0.0f, 1.0f, false, [](const Settings& s) { return s.musicVolume; },
            [](Settings& s, float v) { s.musicVolume = clampv(v, 0.0f, 1.0f); }, [](const Settings& s) { return volumeText(s.musicVolume); } },
        { 313, "Sound Effects Volume", 0.0f, 1.0f, false, [](const Settings& s) { return s.sfxVolume; },
            [](Settings& s, float v) { s.sfxVolume = clampv(v, 0.0f, 1.0f); }, [](const Settings& s) { return volumeText(s.sfxVolume); } },
        { 314, "Speech Volume", 0.0f, 1.0f, false, [](const Settings& s) { return s.speechVolume; },
            [](Settings& s, float v) { s.speechVolume = clampv(v, 0.0f, 1.0f); }, [](const Settings& s) { return volumeText(s.speechVolume); } },
    };
    return kPrefs;
}

// Placement of the switches inside the panel: pref index, label x, buttons x, y.
struct RadioPlace {
    int pref;
    float labelX, buttonsX, y;
};
const RadioPlace kRadioPlaces[] = {
    { 0, 16, 130, 32 }, { 1, 16, 130, 53 }, { 2, 16, 130, 74 }, { 3, 16, 130, 95 },
    { 4, 16, 130, 116 }, { 5, 268, 365, 116 },
    { 6, 16, 130, 137 }, { 7, 268, 365, 137 },
    { 8, 16, 130, 158 }, { 9, 268, 365, 158 },
    { 10, 16, 90, 327 }, { 11, 172, 240, 327 }, { 12, 322, 365, 327 },
};

// Option buttons of the demo's preference art: small, medium and big; [0] raised, [1] pressed in.
const char* const kOptionArt[3][2] = { { "prefon", "prefoff" }, { "pmedon", "pmedoff" }, { "pbigon", "pbigoff" } };
const float kOptionArtW[3] = { 34, 61, 105 };
constexpr float kOptionH = 17;

class PrefsScreen : public OptPanel {
public:
    void onEnter() override
    {
        // show the real window state (it may come from the command line)
        g_settings.fullscreen = g_app.fullscreen;
        g_settings.vsync = g_app.vsync;
        m_backup = g_settings;
    }
    bool transparent() const override { return true; }

protected:
    void layout() override
    {
        using namespace ui;
        dimScreen();
        const float px = kPanelX, py = kPanelY;
        panel(px, py);
        std::string t1 = msgOr("options", 200, "GAME PREFERENCES"), t2 = msgOr("options", 201, "SYSTEM PREFERENCES");
        text(103, t1, px + floorf((kPanelW - textWidth(103, t1)) / 2), py + 8, kLabel);
        text(103, t2, px + floorf((kPanelW - textWidth(103, t2)) / 2), py + 182, kLabel);

        const auto& radios = radioPrefs();
        for (const RadioPlace& r : kRadioPlaces)
            if (r.pref < (int)radios.size()) radio(radios[r.pref], px + r.labelX, px + r.buttonsX, py + r.y);

        const auto& sliders = sliderPrefs();
        for (int i = 0; i < (int)sliders.size(); i++) slider(i, sliders[i], px + (i < 4 ? 16.0f : 246.0f), py + 204 + 30.0f * (i % 4));

        if (redButton(msgOr("options", -1, "DEFAULT"), px + 77, py + 352, 0)) {
            g_settings = Settings();
            applySettingsLive();
        }
        if (redButton(msgOr("dbox", 100, "DONE"), px + 208, py + 352, VK_RETURN)) {
            applySettingsLive();
            settingsSave();
            done = true;
        }
        if (redButton(msgOr("dbox", 103, "CANCEL"), px + 312, py + 352, VK_ESCAPE)) {
            g_settings = m_backup;
            applySettingsLive();
            done = true;
        }
        swallowClicks();
    }

private:
    // A row of option buttons; the selected one is pressed in.
    void radio(const RadioPref& p, float labelX, float x, float y)
    {
        using namespace ui;
        text(101, msgOr("options", p.titleMsg, p.titleFallback), labelX, y + 4, kLabel);
        int n = (int)p.choices.size();
        std::vector<std::string> labels(n);
        std::vector<int> sizes(n);
        int widest = 0;
        for (int i = 0; i < n; i++) {
            labels[i] = msgOr("options", p.choices[i].msg, p.choices[i].fallback);
            int need = textWidth(101, labels[i]) + 4, s = 0;
            while (s < 2 && need > (int)artWOr(kOptionArt[s][0], kOptionArtW[s])) s++;
            sizes[i] = s;
            widest = std::max(widest, s);
        }
        if (widest < 2)
            for (int& s : sizes) s = widest; // uniform rows unless a big button is needed
        int cur = p.get(g_settings);
        for (int i = 0; i < n; i++) {
            const char* up = kOptionArt[sizes[i]][0];
            const char* down = kOptionArt[sizes[i]][1];
            float w = artWOr(up, kOptionArtW[sizes[i]]);
            bool selected = cur == i;
            bool held = hovered(x, y, w, kOptionH) && g_app.input.down[0];
            if (area(x, y, w, kOptionH, 0, SND_SMALL) && !selected) {
                p.set(g_settings, i);
                applySettingsLive();
            }
            if (art(up)) draw(selected || held ? down : up, x, y);
            else bevel(x, y, w, kOptionH, !(selected || held));
            text(101, labels[i], x + floorf((w - textWidth(101, labels[i])) / 2), y + 4, selected ? kSelected : kLabel);
            x += w + 3;
        }
    }

    // Label, value and a track with a draggable knob.
    void slider(int index, const SliderPref& p, float x, float y)
    {
        using namespace ui;
        text(101, msgOr("options", p.titleMsg, p.titleFallback), x, y, kLabel);
        text(101, p.valueText(g_settings), x + 200, y, kLabel, T_RIGHT);
        float tw = artWOr("preftrck", 162), kw = artWOr("prefsldr", 22), kh = artHOr("prefsldr", 17);
        float tx = x + 19, ty = y + 17;
        float range = p.maxV - p.minV;
        if (!drawing()) {
            Input& in = g_app.input;
            if (in.pressed[0] && !mouseTaken() && mouseIn(tx - kw / 2, ty - 8, tw + kw, 22)) {
                m_drag = index;
                takeMouse();
                playClick(SND_SMALL, true);
            }
            if (m_drag == index) {
                takeMouse();
                float t = clampv((mouseX() - tx) / (tw - 1), 0.0f, 1.0f);
                if (p.inverted) t = 1.0f - t;
                float before = p.get(g_settings);
                p.set(g_settings, p.minV + t * range);
                if (p.get(g_settings) != before) applySettingsLive();
                if (!in.down[0]) {
                    m_drag = -1;
                    playClick(SND_SMALL, false);
                }
            }
            return;
        }
        float t = range > 0 ? clampv((p.get(g_settings) - p.minV) / range, 0.0f, 1.0f) : 0.0f;
        if (p.inverted) t = 1.0f - t;
        if (art("preftrck")) draw("preftrck", tx, ty);
        else fill(tx, ty + 2, tw, 2, kEdgeOuter);
        float kx = floorf(tx + t * (tw - 1) - kw / 2), ky = floorf(ty + 3 - kh / 2);
        if (art("prefsldr")) draw("prefsldr", kx, ky);
        else bevel(kx, ky, kw, kh, true);
    }

    Settings m_backup;
    int m_drag = -1;
};

// ------------------------------------------------------------------ options menu
class OptionsScreen : public OptPanel {
public:
    explicit OptionsScreen(bool* quit) : m_quit(quit)
    {
        if (m_quit) *m_quit = false;
    }
    bool transparent() const override { return true; }

protected:
    enum Action { A_NONE = -1, A_SAVE = 0, A_LOAD, A_PREFS, A_EXIT, A_DONE, A_BRIGHTER, A_DARKER };

    void layout() override
    {
        using namespace ui;
        float bw = artWOr("opbase", 164), bh = artHOr("opbase", 217);
        float x = floorf((640 - bw) / 2), y = floorf((480 - bh) / 2) - 60;
        if (art("opbase")) {
            draw("opbase", x, y);
        } else {
            fill(x, y, bw, bh, kPanelFill);
            bevel(x, y, bw, bh, true);
        }
        static const char* const kFallback[5] = { "SAVE GAME", "LOAD GAME", "PREFERENCES", "EXIT", "DONE" };
        static const int kHotkeys[5] = { 'S', 'L', 'P', 'E', 'D' };
        float w = artWOr("opbtnoff", 137), h = artHOr("opbtnoff", 34);
        float textY = (float)std::max(0, ((int)h - textHeight(103)) / 2 + 1);
        for (int i = 0; i < 5; i++) {
            float by = y + 17 + i * (h + 3);
            bool held = hovered(x + 13, by, w, h) && g_app.input.down[0];
            if (button("opbtnoff", "opbtnon", x + 13, by, kHotkeys[i], SND_LARGE)) m_action = i;
            std::string label = msgOr("options", 100 + i, kFallback[i]);
            float tx = std::max(0.0f, floorf((w - textWidth(103, label)) / 2));
            text(103, label, x + 13 + tx, by + textY, held ? kLabelDark : kLabel);
        }
        if (key(VK_ESCAPE) || key(VK_RETURN) || key('O')) m_action = A_DONE;
        if (key(VK_F10) || (g_app.input.ctrl && (key('Q') || key('X')))) m_action = A_EXIT;
        if (key(VK_OEM_PLUS) || key(VK_ADD)) m_action = A_BRIGHTER;
        if (key(VK_OEM_MINUS) || key(VK_SUBTRACT)) m_action = A_DARKER;
        swallowClicks();
    }

    void afterLogic() override
    {
        int action = m_action;
        m_action = A_NONE;
        switch (action) {
        case A_SAVE:
        case A_LOAD: {
            bool ok = false;
            g_app.runModal(std::make_unique<SaveLoadScreen>(action == A_SAVE, false, &ok));
            if (ok) done = true; // the game continues (with the loaded state)
            break;
        }
        case A_PREFS: g_app.runModal(std::make_unique<PrefsScreen>()); break;
        case A_EXIT: {
            bool yes = false;
            g_app.runModal(makeMessageBox({ gameMsg("misc", 0) }, true, &yes));
            if (yes) {
                if (m_quit) *m_quit = true;
                done = true;
            }
            break;
        }
        case A_DONE:
            soundPlaySfx("ib1p1xx1");
            done = true;
            break;
        case A_BRIGHTER:
        case A_DARKER: {
            // gamma keys of the original options window (GAMMA_STEP within 1.0 .. 1.18)
            float step = action == A_BRIGHTER ? 0.01125f : -0.01125f;
            g_settings.brightness = clampv(g_settings.brightness + step, 1.0f, 1.18f);
            g_palette.setBrightness(g_settings.brightness);
            settingsSave();
            break;
        }
        default: break;
        }
    }

private:
    bool* m_quit;
    int m_action = A_NONE;
};

} // namespace

std::unique_ptr<Screen> makeOptionsScreen(bool* quitToMenu) { return std::make_unique<OptionsScreen>(quitToMenu); }

std::unique_ptr<Screen> makeLoadGameScreen(bool* loaded) { return std::make_unique<SaveLoadScreen>(false, true, loaded); }

} // namespace fo
