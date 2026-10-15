// Small modal panels: skilldex, help screen, called shot picker.
#include "ui/screens.h"
#include "ui/uikit.h"
#include "game/anim.h"
#include "game/art.h"
#include "game/combat.h"
#include "game/critter.h"
#include "game/game.h"
#include "game/msg.h"

#include <windows.h>

namespace fo {

namespace {

constexpr u32 kLabel = 0xFF948C18u;     // colorTable[18979]
constexpr u32 kLabelDark = 0xFF736318u; // colorTable[14723]

// Base class for panels that run their layout in both passes.
class Panel : public Screen {
public:
    void update(float dt) override
    {
        m_dt = dt;
        ui::Pass p(false);
        layout();
    }
    void drawUI() override
    {
        ui::Pass p(true);
        layout();
    }

protected:
    virtual void layout() = 0;
    float m_dt = 0;
};

// ------------------------------------------------------------------ skilldex
class SkilldexScreen : public Panel {
public:
    explicit SkilldexScreen(int* out) : m_out(out)
    {
        if (m_out) *m_out = -1;
    }
    bool transparent() const override { return true; }

protected:
    void layout() override
    {
        using namespace ui;
        static const int kSkills[8] = { SKILL_SNEAK, SKILL_LOCKPICK, SKILL_STEAL, SKILL_TRAPS, SKILL_FIRST_AID, SKILL_DOCTOR, SKILL_SCIENCE, SKILL_REPAIR };
        float bw = (float)artW("skldxbox"), bh = (float)artH("skldxbox");
        float x = 640 - bw - 4, y = 480 - 100 - 1 - bh - 6;
        draw("skldxbox", x, y);
        text(103, gameMsg("skilldex", 100), x + 55, y + 14, kLabel);
        Object* d = g_world.dude;
        int lh = lineHeight(103);
        for (int i = 0; i < 8; i++) {
            int value = d ? std::max(0, critterSkill(d, kSkills[i])) : 0;
            float vy = y + 48 + 36.0f * i;
            drawPart("bignum", 14 * (value / 100 % 10), 0, 14, 24, x + 110, vy);
            drawPart("bignum", 14 * (value / 10 % 10), 0, 14, 24, x + 124, vy);
            drawPart("bignum", 14 * (value % 10), 0, 14, 24, x + 138, vy);
            float by = y + 45 + 36.0f * i;
            bool pressed = hovered(x + 15, by, (float)artW("skldxoff"), (float)artH("skldxoff")) && g_app.input.down[0];
            if (button("skldxon", "skldxoff", x + 15, by, '1' + i, SND_LARGE)) choose(kSkills[i]);
            std::string name = gameMsg("skilldex", 102 + i);
            float nx = x + 15 + std::max(0.0f, (artW("skldxoff") - textWidth(103, name)) / 2.0f) + 1;
            float ny = by + (artH("skldxoff") - lh) / 2 + 1;
            text(103, name, nx, ny, pressed ? kLabelDark : kLabel);
        }
        text(103, gameMsg("skilldex", 101), x + 72, y + 337, kLabel);
        if (button("lilredup", "lilreddn", x + 48, y + 338, VK_ESCAPE, SND_SMALL)) done = true;
        if (!drawing() && g_app.input.pressed[1]) done = true;
    }

private:
    void choose(int skill)
    {
        if (m_out) *m_out = skill;
        done = true;
    }
    int* m_out;
};

// ------------------------------------------------------------------ help
class HelpScreen : public Panel {
protected:
    void layout() override
    {
        using namespace ui;
        letterbox(kBlack);
        draw("helpscrn", 0, 0);
        if (!drawing()) {
            Input& in = g_app.input;
            bool anyKey = false;
            for (int k = 0; k < 256; k++)
                if (in.keyPressed[k]) anyKey = true;
            if (in.pressed[0] || in.pressed[1] || anyKey) done = true;
            takeMouse();
        }
    }
};

// ------------------------------------------------------------------ called shot
class CalledShotScreen : public Panel {
public:
    CalledShotScreen(Object* target, int hitMode, int* out) : m_target(target ? target->uid : 0), m_hitMode(hitMode), m_out(out)
    {
        if (m_out) *m_out = -1;
    }
    bool transparent() const override { return true; }

protected:
    void layout() override
    {
        using namespace ui;
        static const int kTy[4] = { 122, 188, 252, 316 };
        static const int kLeft[4] = { HIT_HEAD, HIT_EYES, HIT_RIGHT_ARM, HIT_RIGHT_LEG };
        static const int kRight[4] = { HIT_TORSO, HIT_GROIN, HIT_LEFT_ARM, HIT_LEFT_LEG };
        Object* t = g_world.byUid(m_target);
        Object* d = g_world.dude;
        if (!t || !d) {
            done = true;
            return;
        }
        float x = 108, y = 20;
        draw("called", x, y);
        // the target's called shot picture
        int fid = makeFid(OBJ_CRITTER, fidIndex(t->fid), ANIM_CALLED_SHOT_PIC, 0, 0);
        if (Sprite* s = g_art.sprite(fid)) drawSprite(s, 0, 0, x + 128, y + 31);
        for (int side = 0; side < 2; side++) {
            for (int i = 0; i < 4; i++) {
                int loc = side == 0 ? kLeft[i] : kRight[i];
                int chance = clampv(combatToHit(d, d->tile, t, m_hitMode, loc), 0, 95);
                float ry = y + kTy[i] - 86;
                float bx = side == 0 ? x + 33 : x + 303;
                bool hot = hovered(bx, y + kTy[i] - 82, 88, 10);
                text(101, strFormat("%d%%", chance), side == 0 ? x + 33 : x + 373, ry, kGreen);
                std::string name = gameMsg("combat", 1000 + loc);
                if (name.empty() || name == "Error") name = locationName(loc);
                if (side == 0) text(101, name, x + 74, ry, hot ? kYellow : kGreen);
                else text(101, name, x + 351, ry, hot ? kYellow : kGreen, T_RIGHT);
                if (area(bx, y + kTy[i] - 82, 88, 10, 0, SND_SMALL)) choose(loc);
            }
        }
        if (button("lilredup", "lilreddn", x + 170, y + 268, VK_ESCAPE, SND_SMALL)) done = true;
        if (!drawing() && g_app.input.pressed[1]) done = true;
    }

private:
    static const char* locationName(int loc)
    {
        static const char* kNames[HIT_LOCATION_COUNT] = { "Head", "Left Arm", "Right Arm", "Torso", "Right Leg", "Left Leg", "Eyes", "Groin", "Uncalled" };
        return loc >= 0 && loc < HIT_LOCATION_COUNT ? kNames[loc] : "";
    }
    void choose(int loc)
    {
        if (m_out) *m_out = loc;
        done = true;
    }
    u32 m_target;
    int m_hitMode;
    int* m_out;
};

} // namespace

std::unique_ptr<Screen> makeSkilldexScreen(int* skill) { return std::make_unique<SkilldexScreen>(skill); }
std::unique_ptr<Screen> makeHelpScreen() { return std::make_unique<HelpScreen>(); }
std::unique_ptr<Screen> makeCalledShotScreen(Object* target, int hitMode, int* location)
{
    return std::make_unique<CalledShotScreen>(target, hitMode, location);
}

} // namespace fo
