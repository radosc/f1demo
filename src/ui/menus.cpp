// Main menu, character selection and the generic message box.
#include "ui/screens.h"
#include "ui/uikit.h"
#include "core/fs.h"
#include "core/json.h"
#include "audio/sound.h"
#include "game/critter.h"
#include "game/game.h"
#include "game/msg.h"

#include <windows.h>

namespace fo {

namespace {

constexpr u32 kMenuLabel = 0xFFA59C18u;   // colorTable[21091]
constexpr u32 kMenuSmall = 0xFFA5B5A5u;   // colorTable[21204]
constexpr u32 kBoxText = 0xFFFF9442u;     // colorTable[32328]
constexpr u32 kBoxButton = 0xFF948C18u;   // colorTable[18979]

std::string startMapName()
{
    std::string text;
    if (g_vfs.readText("manifest.json", text)) {
        Json j = Json::parse(text);
        std::string m = j["startMap"].str("");
        if (!m.empty()) return m;
    }
    return "junkdemo";
}

bool startNewGame()
{
    soundStopMusic();
    if (!gameEnterMap(startMapName())) {
        g_app.runModal(makeMessageBox({ "The start map could not be loaded." }));
        return false;
    }
    g_app.replaceAll(makeGameScreen());
    return true;
}

// ------------------------------------------------------------------ message box
class MessageBoxScreen : public Screen {
public:
    MessageBoxScreen(std::vector<std::string> lines, bool yesNo, bool* yes) : m_lines(std::move(lines)), m_yesNo(yesNo), m_yes(yes) {}
    bool transparent() const override { return true; }
    void update(float) override
    {
        ui::Pass p(false);
        layout();
    }
    void drawUI() override
    {
        ui::Pass p(true);
        layout();
    }

private:
    void layout()
    {
        using namespace ui;
        // wrap lines to the box width; pick the large box when they do not fit the medium one
        std::vector<std::string> body;
        bool large = false;
        for (int pass = 0; pass < 2; pass++) {
            body.clear();
            int w = (pass == 0 ? 215 : 302) - 26;
            for (auto& l : m_lines) {
                auto wr = wrap(101, l, w);
                if (wr.empty()) body.push_back("");
                for (auto& x : wr) body.push_back(x);
            }
            large = pass == 1;
            if ((int)body.size() <= 5) break;
        }
        const char* art = large ? "lgdialog" : "medialog";
        float bw = (float)artW(art), bh = (float)artH(art);
        float x = floorf((640 - bw) / 2), y = floorf((380 - bh) / 2);
        draw(art, x, y);
        int lh = lineHeight(101);
        int lines = large ? 6 : 5;
        float ty = y + (large ? 27 : 23);
        ty = ty + lines * lh / 2 - (float)((body.size() + 1) * lh / 2);
        for (auto& l : body) {
            text(101, l, x + bw / 2, ty, kBoxText, T_CENTER);
            ty += (float)lh;
        }
        float doneY = y + (large ? 98 : 81);
        std::string yesText = gameMsg("dbox", m_yesNo ? 101 : 100);
        if (yesText.empty()) yesText = m_yesNo ? "YES" : "DONE";
        if (!m_yesNo) {
            float bx = x + (bw - 90) / 2;
            draw("donebox", bx, doneY);
            text(103, yesText, bx + 35, doneY + 3, kBoxButton);
            if (button("lilredup", "lilreddn", bx + 13, doneY + 4, VK_RETURN, SND_SMALL) || key(VK_ESCAPE)) finish(true);
        } else {
            std::string noText = gameMsg("dbox", 102);
            if (noText.empty()) noText = "NO";
            float bx1 = x + bw / 2 - 100, bx2 = x + bw / 2 + 5;
            draw("donebox", bx1, doneY);
            draw("donebox", bx2, doneY);
            text(103, yesText, bx1 + 35, doneY + 3, kBoxButton);
            text(103, noText, bx2 + 35, doneY + 3, kBoxButton);
            if (button("lilredup", "lilreddn", bx1 + 13, doneY + 4, 'Y', SND_SMALL) || key(VK_RETURN)) finish(true);
            if (button("lilredup", "lilreddn", bx2 + 13, doneY + 4, 'N', SND_SMALL) || key(VK_ESCAPE)) finish(false);
        }
    }
    void finish(bool yes)
    {
        if (m_yes) *m_yes = yes;
        done = true;
    }
    std::vector<std::string> m_lines;
    bool m_yesNo;
    bool* m_yes;
};

// ------------------------------------------------------------------ character selection
struct PremadeInfo {
    std::string id;     // file stem
    std::string name;
    std::vector<std::string> bio;
    std::string face;   // interface art name
};

std::vector<PremadeInfo> loadPremades()
{
    std::vector<PremadeInfo> list;
    std::vector<std::string> order = { "combat", "stealth", "diplomat" };
    std::set<std::string> seen;
    auto add = [&](const std::string& id) {
        if (seen.count(id)) return;
        std::string text;
        if (!g_vfs.readText("premade/" + id + ".json", text)) return;
        Json j = Json::parse(text);
        // templates (blank = character creation, player/demo = engine defaults) are not offered;
        // a mod can hide a character with "selectable": false
        if (id == "blank" || id == "player" || id == "demo") return;
        if (j["name"].str("").empty() || !j["selectable"].b(true)) return;
        seen.insert(id);
        PremadeInfo p;
        p.id = id;
        p.name = j["name"].str(id);
        p.face = j["face"].str(id);
        std::string bio;
        if (g_vfs.readText("premade/" + j["bio"].str(""), bio)) {
            for (auto& line : strSplit(strReplaceAll(bio, "\r", ""), '\n')) p.bio.push_back(line);
        }
        list.push_back(p);
    };
    for (auto& id : order) add(id);
    for (auto& f : g_vfs.list("premade")) {
        if (strEndsWith(f, ".json")) add(pathStem(f));
    }
    return list;
}

class CharSelectScreen : public Screen {
public:
    void onEnter() override
    {
        m_premades = loadPremades();
        select(0);
    }
    void update(float) override
    {
        ui::Pass p(false);
        layout();
    }
    void drawUI() override
    {
        ui::Pass p(true);
        layout();
    }

private:
    void select(int i)
    {
        if (m_premades.empty()) return;
        m_index = (i + (int)m_premades.size()) % (int)m_premades.size();
        gameResetState();
        gameNewPlayer(m_premades[m_index].id);
    }

    void layout()
    {
        using namespace ui;
        letterbox(kBlack);
        draw("pickchar", 0, 0);
        if (!m_premades.empty()) drawCharacter();
        if (button("slu", "sld", 292, 320, VK_LEFT, SND_MEDIUM)) select(m_index - 1);
        if (button("sru", "srd", 318, 320, VK_RIGHT, SND_MEDIUM)) select(m_index + 1);
        if (button("lilredup", "lilreddn", 81, 323, 'T', SND_SMALL) || key(VK_RETURN)) {
            if (!m_premades.empty()) startNewGame();
        }
        if (button("lilredup", "lilreddn", 435, 320, 'M', SND_SMALL)) {
            bool accepted = false;
            g_app.runModal(makeCharacterScreen(true, &accepted));
            if (accepted) startNewGame();
        }
        if (button("lilredup", "lilreddn", 80, 425, 'C', SND_SMALL)) {
            gameResetState();
            gameNewPlayer("blank");
            bool accepted = false;
            g_app.runModal(makeCharacterScreen(true, &accepted));
            if (accepted) startNewGame();
            else select(m_index);
        }
        if (button("lilredup", "lilreddn", 461, 425, 'B', SND_SMALL) || key(VK_ESCAPE)) g_app.replaceAll(makeMainMenu());
    }

    void drawCharacter()
    {
        using namespace ui;
        const PremadeInfo& p = m_premades[m_index];
        Object* d = g_world.dude;
        // portrait with the monitor's scanline look
        if (Sprite* s = art(p.face)) {
            const SheetFrame& f = s->frame(0, 0);
            float x = 150 - f.w / 2.0f, y = 240.0f - f.h;
            drawSprite(s, 0, 0, floorf(x), y);
            for (int ly = 1; ly < f.h; ly += 2) fill(floorf(x), y + ly, (float)f.w, 1, 0xC0000000u);
        }
        if (!d) return;
        int lh = lineHeight(101);
        float y = 40;
        text(101, g_game.player.name, 318, y, kGreen, T_CENTER);
        y += 3.0f * lh;
        for (int s = STAT_ST; s <= STAT_LK; s++) {
            int v = critterStat(d, s);
            text(101, strFormat("%s %02d", gameMsg("stat", 100 + s).c_str(), v), 348, y, kGreen, T_RIGHT);
            text(101, "  " + gameMsg("stat", 300 + clampv(v, 1, 10)), 348, y, kGreen);
            y += (float)lh;
        }
        y += (float)lh;
        auto row = [&](const std::string& label, const std::string& value) {
            text(101, label, 365, y, kGreen, T_RIGHT);
            text(101, value, 365, y, kGreen);
            y += (float)lh;
        };
        row(gameMsg("misc", 16), strFormat(" %d/%d", d->critter.hp, critterStat(d, STAT_MAX_HP)));
        row(gameMsg("stat", 109), strFormat(" %d", critterStat(d, STAT_AC)));
        row(gameMsg("misc", 15), strFormat(" %d", critterStat(d, STAT_MAX_AP)));
        row(gameMsg("stat", 111), strFormat(" %d", critterStat(d, STAT_MELEE_DMG)));
        y += (float)lh;
        for (int t = 0; t < 4; t++) {
            int sk = g_game.player.tags[t];
            if (sk < 0) continue;
            row(gameMsg("skill", 100 + sk), strFormat(" %d%%", critterSkill(d, sk)));
        }
        for (int t = 0; t < 2; t++) {
            int tr = g_game.player.traits[t];
            if (tr < 0) continue;
            text(101, gameMsg("trait", 100 + tr), 365, y, kGreen, T_RIGHT);
            y += (float)lh;
        }
        float by = 40;
        for (auto& line : p.bio) {
            if (by > 300) break;
            text(101, line, 420, by, kGreen);
            by += (float)lh;
        }
    }

    std::vector<PremadeInfo> m_premades;
    int m_index = 0;
};

// ------------------------------------------------------------------ main menu
class MainMenuScreen : public Screen {
public:
    void onEnter() override
    {
        // the demo's only music track plays on the menus
        soundPlayMusic("06vault", true);
        g_app.zoomUser = 1.0f;
    }
    void update(float dt) override
    {
        static bool s_startupMovies = false;
        if (!s_startupMovies) {
            // like the original: the Interplay logo and the intro before the first menu
            s_startupMovies = true;
            if (!g_app.argFlag("--hidden") && !g_app.argFlag("--skip-movies")) {
                g_app.runModal(makeMovieScreen("iplogo"));
                g_app.runModal(makeMovieScreen("intro"));
                return;
            }
        }
        // two idle minutes replay the intro
        Input& in = g_app.input;
        bool active = in.pressed[0] || in.pressed[1] || !in.chars.empty() || in.mouseX != m_lastMouseX || in.mouseY != m_lastMouseY;
        for (int k = 0; k < 256 && !active; k++) active = in.keyPressed[k];
        m_lastMouseX = in.mouseX;
        m_lastMouseY = in.mouseY;
        m_idle = active ? 0.0f : m_idle + dt;
        if (m_idle > 120.0f) {
            m_idle = 0;
            g_app.runModal(makeMovieScreen("intro"));
            return;
        }
        ui::Pass p(false);
        layout();
    }
    void drawUI() override
    {
        ui::Pass p(true);
        layout();
    }

private:
    float m_idle = 0;
    int m_lastMouseX = -1, m_lastMouseY = -1;

    void layout()
    {
        using namespace ui;
        letterbox(kBlack);
        draw("mainmenu", 0, 0);
        text(100, gameMsg("misc", 14), 15, 460, kMenuSmall);
        text(100, "FALLOUT DEMO HD 1.0", 615, 460, kMenuSmall, T_RIGHT);
        static const int kKeys[5] = { 'I', 'N', 'L', 'C', 'E' };
        for (int i = 0; i < 5; i++) {
            float by = 45.0f + 41.0f * i;
            text(104, gameMsg("misc", 9 + i), 520, by + 1, kMenuLabel, T_CENTER);
            if (button("menuup", "menudown", 425, by, kKeys[i], SND_SMALL)) choose(i);
        }
        if (key(VK_ESCAPE)) choose(4);
    }

    void choose(int i)
    {
        switch (i) {
        case 0: g_app.runModal(makeMovieScreen("intro")); break;
        case 1: g_app.replaceAll(std::make_unique<CharSelectScreen>()); break;
        case 2: {
            bool loaded = false;
            g_app.runModal(makeLoadGameScreen(&loaded));
            if (loaded) {
                soundStopMusic();
                g_app.replaceAll(makeGameScreen());
            }
            break;
        }
        case 3: g_app.runModal(makeMovieScreen("credits")); break;
        case 4: g_app.requestQuit(); break;
        }
    }
};

} // namespace

std::unique_ptr<Screen> makeMainMenu() { return std::make_unique<MainMenuScreen>(); }

std::unique_ptr<Screen> makeMessageBox(const std::vector<std::string>& lines, bool yesNo, bool* yes)
{
    return std::make_unique<MessageBoxScreen>(lines, yesNo, yes);
}

} // namespace fo
