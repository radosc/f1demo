// Automap: the top-down map window (automap.frm) showing the walls (and in high detail the scenery) of the
// current map elevation, the player's position and - with a working motion sensor in hand - the critters.
#include "ui/screens.h"
#include "ui/uikit.h"
#include "audio/sound.h"
#include "game/game.h"
#include "game/item.h"
#include "game/map.h"
#include "game/msg.h"

#include <windows.h>

namespace fo {

namespace {

// Window of the original (519x480, centered on the 640x480 screen).
constexpr float kWinX = 60, kWinY = 0;
// Hex (col, row) -> window pixel: x = 449 - 2 * col, y = 8 + 2 * row (two pixels per hex, every other line).
constexpr int kMapX0 = 449, kMapY0 = 8;
// Level arrows printed on the demo's automap art (top right corner).
constexpr float kArrowX = 490, kArrowUpY = 38, kArrowDnY = 63, kArrowW = 21, kArrowH = 20;

constexpr int PID_STAIRS = 0x02000031;
constexpr int PID_BLOCK_HEX_INVISO = 0x02000158;
constexpr int PID_MOTION_SENSOR = 0x0000003B;

constexpr u32 kWallColor = 0xFF00FF00u;    // colorTable[992]
constexpr u32 kSceneryColor = 0xFF007B00u; // colorTable[480]
constexpr u32 kStairsColor = 0xFFFF9442u;  // colorTable[32328]
constexpr u32 kCritterColor = 0xFFFF0000u; // colorTable[31744]
constexpr u32 kDudeColor = 0xFFFF0000u;

// Higher value wins when several objects share a hex (walls are never painted over by scenery).
enum Mark : u8 { MK_NONE = 0, MK_SCENERY, MK_WALL, MK_STAIRS, MK_CRITTER, MK_COUNT };

bool s_highDetail = false; // the HI/LO switch (kept for the session like the original's autoflags)

std::string msgOr(const char* file, int num, const char* fallback)
{
    std::string s = gameMsg(file, num);
    return s.empty() || s == "Error" ? std::string(fallback) : s;
}

// Display name of the current map: the map's own name, map.msg ("NAME.MAP" -> entry + 100) or the file name.
std::string mapTitle()
{
    if (!g_map.displayName.empty() && g_map.displayName != g_map.name) return g_map.displayName;
    std::string file = strUpper(g_map.name) + ".MAP";
    MessageList& ml = messages("text/" + g_language + "/game/map.msg");
    for (auto& kv : ml.entries()) {
        if (kv.first >= 100 || !strIEquals(strTrim(kv.second.text), file)) continue;
        if (const MessageEntry* e = ml.find(kv.first + 100)) return strTrim(e->text);
    }
    return strUpper(g_map.name);
}

std::string clipText(int fontId, std::string s, int width)
{
    while (!s.empty() && ui::textWidth(fontId, s) > width) s.pop_back();
    return s;
}

class AutomapScreen : public Screen {
public:
    void onEnter() override
    {
        Object* d = g_world.dude;
        m_elev = d && d->tile >= 0 ? d->elevation : g_game.elevation;
        // Honour "seen" flags once the engine tracks them; until then the whole elevation is shown.
        m_useSeen = false;
        g_world.forEach([&](Object* o) {
            if (o->flags & OF_SEEN) m_useSeen = true;
        });
    }

    bool transparent() const override { return true; }

    void update(float dt) override
    {
        m_time += dt;
        m_scannerRequest = false;
        {
            ui::Pass pass(false);
            layout();
        }
        if (m_scannerRequest && !done) useScanner();
    }

    void drawUI() override
    {
        // the game screen below sets its world cursor while drawing; ours is drawn later and wins
        g_app.setCursor("stdarrow", 0, 0);
        ui::Pass pass(true);
        layout();
    }

private:
    void layout()
    {
        using namespace ui;
        if (art("automap")) draw("automap", kWinX, kWinY);
        else fill(kWinX, kWinY, 519, 480, 0xFF202020u);
        if (drawing()) drawMap();

        std::string level = msgOr("lsgame", 129, "Level") + strFormat(" %d", m_elev + 1);
        text(101, clipText(101, mapTitle(), 240), kWinX + 150, kWinY + 380, kWallColor);
        text(101, clipText(101, level, 240), kWinX + 150, kWinY + 396, kWallColor);

        if (button("lilredup", "lilreddn", kWinX + 111, kWinY + 454, 'S', SND_SMALL)) m_scannerRequest = true;
        if (button("lilredup", "lilreddn", kWinX + 277, kWinY + 454, VK_ESCAPE, SND_SMALL)) done = true;

        // HI/LO detail switch: lever up = high details
        const char* sw = s_highDetail ? "autoup" : "autodwn";
        float sw_w = artW(sw) > 0 ? (float)artW(sw) : 42.0f, sw_h = artH(sw) > 0 ? (float)artH(sw) : 74.0f;
        draw(sw, kWinX + 457, kWinY + 340);
        if (area(kWinX + 457, kWinY + 340, sw_w, sw_h, 0, SND_MEDIUM)) s_highDetail = !s_highDetail;
        if (key('H')) s_highDetail = true;
        if (key('L')) s_highDetail = false;

        // level arrows (part of the background art)
        if (area(kWinX + kArrowX, kWinY + kArrowUpY, kArrowW, kArrowH, VK_PRIOR, SND_SMALL)) changeLevel(-1);
        if (area(kWinX + kArrowX, kWinY + kArrowDnY, kArrowW, kArrowH, VK_NEXT, SND_SMALL)) changeLevel(1);
        if (drawing() && g_app.input.down[0]) {
            if (hovered(kWinX + kArrowX, kWinY + kArrowUpY, kArrowW, kArrowH)) fill(kWinX + kArrowX, kWinY + kArrowUpY, kArrowW, kArrowH, 0x60000000u);
            if (hovered(kWinX + kArrowX, kWinY + kArrowDnY, kArrowW, kArrowH)) fill(kWinX + kArrowX, kWinY + kArrowDnY, kArrowW, kArrowH, 0x60000000u);
        }

        if (key(VK_TAB) || key(VK_RETURN) || key('A')) done = true;
        // the window is modal: clicks never reach the world below
        if (!drawing() && (g_app.input.pressed[0] || g_app.input.pressed[1])) takeMouse();
    }

    void changeLevel(int dir)
    {
        for (int e = m_elev + dir; e >= 0 && e < kElevations; e += dir) {
            if (g_map.hasElevation[e]) {
                m_elev = e;
                return;
            }
        }
    }

    void drawMap()
    {
        Object* d = g_world.dude;
        m_grid.assign((size_t)kHexes, (u8)MK_NONE);
        g_world.forEach([&](Object* o) {
            if (o->tile < 0 || o->tile >= kHexes || o->elevation != m_elev || o == d || o->isHidden()) return;
            u8 mark = MK_NONE;
            if (o->isCritter()) {
                if (m_scanner && !o->isDead()) mark = MK_CRITTER;
            } else if (!m_useSeen || (o->flags & OF_SEEN)) {
                if (o->pid == PID_STAIRS) mark = MK_STAIRS;
                else if (o->isWall()) mark = MK_WALL;
                else if (o->isScenery() && s_highDetail && o->pid != PID_BLOCK_HEX_INVISO) mark = MK_SCENERY;
            }
            u8& cell = m_grid[(size_t)o->tile];
            if (mark > cell) cell = mark;
        });
        static const u32 kColors[MK_COUNT] = { 0, kSceneryColor, kWallColor, kStairsColor, kCritterColor };
        // horizontal runs of equal marks become one quad (columns run right to left on screen)
        for (int row = 0; row < kHexH; row++) {
            const u8* line = &m_grid[(size_t)row * kHexW];
            float y = kWinY + kMapY0 + 2.0f * row;
            int col = kHexW - 1;
            while (col >= 0) {
                u8 m = line[col];
                if (m == MK_NONE) {
                    col--;
                    continue;
                }
                int end = col;
                while (end > 0 && line[end - 1] == m) end--;
                ui::fill(kWinX + kMapX0 - 2.0f * col, y, 2.0f * (col - end + 1), 1, kColors[m]);
                col = end - 1;
            }
        }
        // the player: a blinking cross
        if (d && d->tile >= 0 && d->elevation == m_elev && fmodf(m_time, 0.8f) < 0.55f) {
            float x = kWinX + kMapX0 - 2.0f * (d->tile % kHexW), y = kWinY + kMapY0 + 2.0f * (d->tile / kHexW);
            ui::fill(x - 1, y, 3, 1, kDudeColor);
            ui::fill(x, y - 1, 1, 3, kDudeColor);
        }
    }

    // SCANNER: shows the critters while a motion sensor with charges is held in a hand.
    void useScanner()
    {
        Object* d = g_world.dude;
        if (!d) return;
        if (d->tile >= 0) m_elev = d->elevation;
        if (m_scanner) return;
        Object* sensor = nullptr;
        for (Object* it : { itemLeftHand(d), itemRightHand(d) })
            if (it && it->pid == PID_MOTION_SENSOR) sensor = it;
        if (sensor) {
            const Proto* p = sensor->proto();
            bool usesCharges = p && p->itemType == ITEM_MISC && p->misc.charges > 0;
            if (!usesCharges || sensor->charges > 0) {
                if (usesCharges) sensor->charges--;
                m_scanner = true;
                return;
            }
        }
        soundPlaySfx("iisxxxx1");
        std::string msg = sensor ? msgOr("misc", 18, "The motion sensor has no charges remaining.")
                                 : msgOr("misc", 17, "The motion sensor is not installed.");
        g_app.runModal(makeMessageBox({ msg }));
    }

    int m_elev = 0;
    bool m_useSeen = false;
    bool m_scanner = false;
    bool m_scannerRequest = false;
    float m_time = 0;
    std::vector<u8> m_grid;
};

} // namespace

std::unique_ptr<Screen> makeAutomapScreen() { return std::make_unique<AutomapScreen>(); }

} // namespace fo
