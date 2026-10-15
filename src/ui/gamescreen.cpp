// The in-game screen: world view, interface bar, mouse modes (move / command / target / use), action menu
// and the keyboard shortcuts from the demo manual.
#include "ui/gamescreen.h"
#include "ui/screens.h"
#include "ui/uikit.h"
#include "audio/sound.h"
#include "game/actions.h"
#include "game/anim.h"
#include "game/art.h"
#include "game/combat.h"
#include "game/critter.h"
#include "game/dialog.h"
#include "game/game.h"
#include "game/hex.h"
#include "game/item.h"
#include "game/map.h"
#include "game/msg.h"
#include "game/proto.h"
#include "game/script.h"
#include "game/worldview.h"
#include "gfx/palette.h"

#include <windows.h>

namespace fo {

namespace {

constexpr float kBarY = 380;      // interface bar (iface.frm) top in layout coordinates
constexpr float kMonX = 23, kMonY = 24, kMonW = 167, kMonH = 60;
constexpr float kItemX = 267, kItemY = 26, kItemW = 188, kItemH = 67;
constexpr int kFont = 101;

enum MouseMode { M_MOVE, M_COMMAND, M_TARGET, M_USE_ITEM, M_USE_SKILL };

// item slot actions (g_game.handMode)
enum HandAction { HA_PRIMARY = 0, HA_PRIMARY_AIMED, HA_SECONDARY, HA_SECONDARY_AIMED, HA_RELOAD, HA_COUNT };

const char* kActionIcons[ACT_COUNT][2] = {
    { "canceln", "cancelh" }, { "dropn", "droph" }, { "invenn", "invenh" }, { "lookn", "lookh" }, { "rotaten", "rotateh" },
    { "talkn", "talkh" }, { "usegetn", "usegeth" }, { "canceln", "cancelh" }, { "usegetn", "usegeth" },
};

Object* dude() { return g_world.dude; }

Object* activeItem() { return dude() ? (g_game.activeHand == 0 ? itemLeftHand(dude()) : itemRightHand(dude())) : nullptr; }

bool isWeaponSlot(Object* item) { return !item || itemType(item) == ITEM_WEAPON; }

int hitModeFor(int action)
{
    Object* item = activeItem();
    bool secondary = action == HA_SECONDARY || action == HA_SECONDARY_AIMED;
    if (!item || itemType(item) != ITEM_WEAPON) return secondary ? HIT_KICK : HIT_PUNCH;
    if (g_game.activeHand == 0) return secondary ? HIT_LEFT_SECONDARY : HIT_LEFT_PRIMARY;
    return secondary ? HIT_RIGHT_SECONDARY : HIT_RIGHT_PRIMARY;
}

bool actionAimed(int action) { return action == HA_PRIMARY_AIMED || action == HA_SECONDARY_AIMED; }

bool calledShotAllowed(int hitMode)
{
    int a = weaponAttackType(activeItem(), hitMode);
    return a == ATTACK_PUNCH || a == ATTACK_KICK || a == ATTACK_SWING || a == ATTACK_THRUST || a == ATTACK_FIRE_SINGLE;
}

// Next valid item action (right click on the item button / N key).
void cycleHandAction()
{
    int& action = g_game.handMode[g_game.activeHand];
    Object* item = activeItem();
    if (!isWeaponSlot(item)) {
        action = HA_PRIMARY;
        return;
    }
    for (int guard = 0; guard < HA_COUNT; guard++) {
        action = (action + 1) % HA_COUNT;
        switch (action) {
        case HA_PRIMARY: return;
        case HA_PRIMARY_AIMED:
            if (calledShotAllowed(hitModeFor(HA_PRIMARY))) return;
            break;
        case HA_SECONDARY:
        case HA_SECONDARY_AIMED: {
            int hm = hitModeFor(HA_SECONDARY);
            int a = weaponAttackType(item, hm);
            if (item && (a == ATTACK_NONE || a == weaponAttackType(item, hitModeFor(HA_PRIMARY)))) break;
            if (action == HA_SECONDARY) return;
            if (calledShotAllowed(hm)) return;
            break;
        }
        case HA_RELOAD:
            if (item && weaponUsesAmmo(item)) return;
            break;
        }
    }
    action = HA_PRIMARY;
}

const char* attackIcon(int attackType)
{
    switch (attackType) {
    case ATTACK_PUNCH: return "punch";
    case ATTACK_KICK: return "kick";
    case ATTACK_SWING: return "swing";
    case ATTACK_THRUST: return "thrust";
    case ATTACK_THROW: return "throw";
    case ATTACK_FIRE_SINGLE: return "single";
    case ATTACK_FIRE_BURST:
    case ATTACK_FLAME: return "burst";
    default: return nullptr;
    }
}

std::string dateTimeString()
{
    static const char* kMonths[12] = { "JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC" };
    static const int kDays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    // the game starts on December 5, 2161
    int day = gameDay() + 4, month = 11, year = 2161;
    while (day >= kDays[month]) {
        day -= kDays[month];
        if (++month == 12) {
            month = 0;
            year++;
        }
    }
    return strFormat("%s %d, %d %04d", kMonths[month], day + 1, year, gameHour());
}

class GameScreenImpl : public Screen {
public:
    void onEnter() override
    {
        g_onGameOver = [] { g_app.runModal(makeMessageBox({ "The demo is over.", "Thank you for playing." })); };
        g_app.onDevCommand = [this](const std::string& cmd) { devCommand(cmd); };
    }
    void onExit() override { g_app.onDevCommand = nullptr; }
    bool showsWorld() const override { return true; }

    void update(float dt) override
    {
        m_time += dt;
        updateCamera(dt);
        if (!dude()) return;
        {
            ui::Pass pass(false);
            layoutBar();
        }
        if (!m_pendingScreen.empty()) {
            std::string s = m_pendingScreen;
            m_pendingScreen.clear();
            openScreen(s);
            return;
        }
        handleKeys();
        if (done || !dude()) return;
        handleWorldMouse(dt);
        gameUpdate(dt);
        if (done || !dude()) return;
        UiRequest req;
        while (actionsPollUi(req)) {
            Object* t = g_world.byUid(req.target);
            if (!t) continue;
            g_app.runModal(makeInventoryScreen(req.kind == UIREQ_STEAL ? INV_STEAL : INV_LOOT, t));
            if (objIsContainer(t) && t->frame != 0) objClose(t);
        }
        if (done || !dude()) return;
        if (dude() && dude()->isDead() && !m_deathShown) {
            m_deathShown = true;
            m_deathTimer = 2.5f;
            Object* d = dude();
            LOGI("Player died: tile %d flags %08X fid %08X frame %d", d->tile, d->flags, d->fid, d->frame);
        }
        if (m_deathShown) {
            m_deathTimer -= dt;
            if (m_deathTimer <= 0) {
                g_app.runModal(makeMessageBox({ "You have died.", "", "Your adventure in the wasteland is over." }));
                endSession();
                return;
            }
        }
    }

    void drawUI() override
    {
        ui::Pass pass(true);
        if (g_app.top() == this) drawWorldCursor();
        layoutBar();
        if (m_debug) {
            ui::text(kFont,
                strFormat("map %s  hex %d  zoom %.2f  tex %dx  objs %d  %s", g_game.mapName.c_str(), g_view.hoverHex, g_app.worldZoom, g_app.worldTex,
                    (int)g_world.count(), dateTimeString().c_str()),
                4 - ui::originX(), 4 - ui::originY(), ui::kGreen, ui::T_SHADOW);
        }
        if (g_game.fade > 0) ui::fill(-ui::originX(), -ui::originY(), (float)g_app.uiW, (float)g_app.uiH, ((u32)(g_game.fade * 255) << 24));
    }

private:
    // ------------------------------------------------------------------ developer commands (tests / modding aid)
    static Object* findObject(const std::string& spec)
    {
        Object* d = dude();
        Object* best = nullptr;
        int bestD = 1 << 30;
        bool byPid = strStartsWith(spec, "0x");
        int pid = byPid ? (int)strtoul(spec.c_str(), nullptr, 16) : -1;
        g_world.forEach([&](Object* o) {
            if (o == d || o->tile < 0) return;
            bool match = byPid ? o->pid == pid : (o->script && strIEquals(o->script->name(), spec));
            if (!match) return;
            int dist = d ? hex::distance(d->tile, o->tile) : 0;
            if (dist < bestD) bestD = dist, best = o;
        });
        return best;
    }

    void devCommand(const std::string& line)
    {
        auto parts = strSplit(line, ' ');
        if (parts.empty() || !dude()) return;
        const std::string& c = parts[0];
        std::string arg = parts.size() > 1 ? parts[1] : std::string();
        Object* d = dude();
        Object* o = arg.empty() ? nullptr : findObject(arg);
        LOGI("dev: %s -> %s", line.c_str(), o ? strFormat("uid %u pid %08X tile %d", o->uid, o->pid, o->tile).c_str() : "-");
        if (c == "use" && o) {
            if (o->isItem()) actionPickup(d, o);
            else actionUse(d, o);
        } else if (c == "look" && o) {
            actionLook(d, o, true);
        } else if (c == "talk" && o) {
            actionTalk(d, o);
        } else if (c == "attack" && o) {
            if (!inCombat()) combatStart(d, o);
            combatAttack(d, o, hitModeFor(g_game.handMode[g_game.activeHand]), HIT_UNCALLED);
        } else if (c == "kill" && o) {
            critterKill(o, ANIM_FALL_BACK, true);
        } else if (c == "goto" && o) {
            int best = -1, bestD = 1 << 30;
            for (int r = 0; r < 6; r++) {
                int t = hex::inDirection(o->tile, r, 1);
                if (g_world.blockingAt(d, t, d->elevation)) continue;
                int dist = hex::distance(d->tile, t);
                if (dist < bestD) bestD = dist, best = t;
            }
            if (best >= 0) actionMoveTo(d, best, true);
        } else if (c == "walk" && !arg.empty()) {
            actionMoveTo(d, atoi(arg.c_str()), false);
        } else if (c == "tp" && !arg.empty()) {
            g_world.place(d, atoi(arg.c_str()), d->elevation);
            g_view.centerOn(d->tile);
        } else if (c == "give" && !arg.empty()) {
            int pid = (int)strtoul(arg.c_str(), nullptr, 16);
            int n = parts.size() > 2 ? std::max(1, atoi(parts[2].c_str())) : 1;
            if (g_protos.get(pid)) g_world.addToInventory(d, g_world.create(pid), n);
        } else if (c == "equip" && !arg.empty()) {
            int pid = (int)strtoul(arg.c_str(), nullptr, 16);
            if (Object* it = g_world.findInInventory(d, pid)) {
                itemEquip(d, it, itemType(it) == ITEM_ARMOR ? 2 : g_game.activeHand);
                critterUpdateArt(d);
            }
        } else if (c == "pick") {
            Object* p = pickObject(g_app.input.worldX, g_app.input.worldY);
            LOGI("dev pick: %s", p ? strFormat("%s pid %08X tile %d", g_protos.name(p->pid).c_str(), p->pid, p->tile).c_str() : "nothing");
        } else if (c == "inv") {
            for (auto& e : d->inventory) LOGI("dev inv: %s x%d (%08X)", g_protos.name(e.obj->pid).c_str(), e.count, e.obj->pid);
        } else if (c == "xp" && !arg.empty()) {
            playerAddExperience(atoi(arg.c_str()));
        } else if (c == "cursor") {
            g_app.setCursorVisible(arg != "off");
        } else if (c == "scroll" && parts.size() > 2) {
            g_view.scroll((float)atof(parts[1].c_str()), (float)atof(parts[2].c_str()));
        } else if (c == "center") {
            g_view.centerOn(o ? o->tile : d->tile);
        }
    }

    // ------------------------------------------------------------------ camera
    void updateCamera(float dt)
    {
        Input& in = g_app.input;
        float speed = 600.0f * dt;
        float dx = 0, dy = 0;
        if (in.keyDown[VK_LEFT]) dx -= speed;
        if (in.keyDown[VK_RIGHT]) dx += speed;
        if (in.keyDown[VK_UP]) dy -= speed;
        if (in.keyDown[VK_DOWN]) dy += speed;
        m_scrollCursor = nullptr;
        if (in.mouseInWindow && in.focused && !m_menuOpen) {
            int mx = in.mouseX, my = in.mouseY;
            int ww = g_renderer.width(), wh = g_renderer.height();
            int edge = std::max(2, ww / 300);
            bool l = mx <= edge, r = mx >= ww - 1 - edge, t = my <= edge, b = my >= wh - 1 - edge;
            if (l) dx -= speed;
            if (r) dx += speed;
            if (t) dy -= speed;
            if (b) dy += speed;
            if (t && l) m_scrollCursor = "scrnwest";
            else if (t && r) m_scrollCursor = "scrneast";
            else if (b && l) m_scrollCursor = "scrswest";
            else if (b && r) m_scrollCursor = "scrseast";
            else if (t) m_scrollCursor = "scrnorth";
            else if (b) m_scrollCursor = "scrsouth";
            else if (l) m_scrollCursor = "scrwest";
            else if (r) m_scrollCursor = "screast";
        }
        if (dx || dy) g_view.scroll(dx, dy);
        if (in.wheel && !overBar()) {
            g_app.zoomUser = clampv(g_app.zoomUser * (in.wheel > 0 ? 1.15f : 1.0f / 1.15f), 0.5f, 2.5f);
            g_app.onResize(g_renderer.width(), g_renderer.height());
        }
    }

    bool overBar() const
    {
        float mx = ui::mouseX(), my = ui::mouseY();
        return my >= kBarY && mx >= 0 && mx < 640;
    }

    bool playerCanAct() const
    {
        if (!dude() || dude()->isDead() || g_game.uiDisabled) return false;
        if (inCombat() && !combatIsPlayerTurn()) return false;
        return true;
    }

    // ------------------------------------------------------------------ keyboard
    void handleKeys()
    {
        Input& in = g_app.input;
        if ((in.ctrl && (in.key('Q') || in.key('X'))) || in.key(VK_F10)) {
            quitToMenu();
            return;
        }
        if (in.key(VK_F12)) {
            g_app.saveScreenshot("");
            displayMessage(gameMsg("misc", 3));
        }
        if (in.ctrl && in.key('D')) m_debug = !m_debug;
        if (in.key(VK_HOME)) g_view.centerOn(dude()->tile);
        if (in.key(VK_F2)) soundSetVolumes(std::max(0.0f, soundMasterVolume() - 0.1f), soundSfxVolume(), soundMusicVolume());
        if (in.key(VK_F3)) soundSetVolumes(std::min(1.0f, soundMasterVolume() + 0.1f), soundSfxVolume(), soundMusicVolume());
        if (in.key(VK_OEM_MINUS) || in.key(VK_SUBTRACT)) g_palette.setBrightness(std::max(0.5f, g_palette.brightness() - 0.05f));
        if (in.key(VK_OEM_PLUS) || in.key(VK_ADD)) g_palette.setBrightness(std::min(1.5f, g_palette.brightness() + 0.05f));
        if (in.key(VK_OEM_2) && in.shift) displayMessage(dateTimeString());
        if (in.key(VK_F1)) {
            openScreen("help");
            return;
        }
        if (in.key(VK_F6)) {
            Image shot;
            if (g_renderer.captureBackBuffer(shot)) gameCaptureThumbnail(shot);
            if (gameSave(gameLastSaveSlot(), "Quick save")) displayMessage(gameMsg("misc", 5));
            else displayMessage("The game could not be saved now.");
        }
        if (in.key(VK_F9)) {
            if (gameLoad(gameLastSaveSlot())) displayMessage(gameMsg("misc", 4));
            else displayMessage("There is no quick save to load.");
            return;
        }
        if (in.ctrl) return;
        static const std::pair<int, const char*> kScreenKeys[] = { { 'C', "character" }, { 'I', "inventory" }, { 'P', "pipboy" },
            { 'S', "skilldex" }, { VK_TAB, "automap" }, { 'O', "options" } };
        for (auto& k : kScreenKeys) {
            if (in.key(k.first)) {
                openScreen(k.second);
                return;
            }
        }
        if (in.key(VK_ESCAPE)) {
            if (m_menuOpen) m_menuOpen = false;
            else if (m_mode != M_MOVE && m_mode != M_COMMAND) setMode(M_MOVE);
            else if (inCombat()) combatTryEnd();
            else {
                openScreen("options");
                return;
            }
        }
        if (!playerCanAct()) return;
        if (in.key('B')) toggleHands();
        if (in.key('N')) cycleHandAction();
        if (in.key('M')) setMode(m_mode == M_MOVE ? M_COMMAND : M_MOVE);
        if (in.key('A')) startTargeting();
        if (in.key(VK_OEM_COMMA)) {
            if (!animBusy(dude())) critterStand(dude(), (dude()->rotation + 5) % 6);
        }
        if (in.key(VK_OEM_PERIOD)) actionRotate(dude());
        if (inCombat()) {
            if (in.key(VK_SPACE)) combatEndPlayerTurn();
            else if (in.key(VK_RETURN)) combatTryEnd();
        }
        static const int kSkillKeys[8] = { SKILL_SNEAK, SKILL_LOCKPICK, SKILL_STEAL, SKILL_TRAPS, SKILL_FIRST_AID, SKILL_DOCTOR, SKILL_SCIENCE, SKILL_REPAIR };
        for (int i = 0; i < 8; i++)
            if (in.key('1' + i)) useSkill(kSkillKeys[i]);
    }

    void quitToMenu()
    {
        bool yes = false;
        g_app.runModal(makeMessageBox({ gameMsg("misc", 0) }, true, &yes));
        if (!yes) return;
        endSession();
    }

    // Ends the game session and returns to the main menu (this screen is finished afterwards).
    void endSession()
    {
        done = true;
        gameEnd();
        g_app.replaceAll(makeMainMenu());
    }

    void openScreen(const std::string& name)
    {
        if (m_menuOpen) return;
        if (name == "inventory") {
            if (inCombat() && !combatIsPlayerTurn()) return;
            g_app.runModal(makeInventoryScreen(INV_NORMAL, nullptr));
            critterUpdateArt(dude());
        } else if (name == "character") {
            g_app.runModal(makeCharacterScreen(false));
        } else if (name == "pipboy") {
            if (inCombat()) {
                displayMessage(gameMsg("misc", 7));
                return;
            }
            g_app.runModal(makePipboyScreen());
        } else if (name == "skilldex") {
            if (inCombat() && !combatIsPlayerTurn()) return;
            int skill = -1;
            g_app.runModal(makeSkilldexScreen(&skill));
            if (skill >= 0) useSkill(skill);
        } else if (name == "automap") {
            g_app.runModal(makeAutomapScreen());
        } else if (name == "options") {
            Image shot;
            if (g_renderer.captureBackBuffer(shot)) gameCaptureThumbnail(shot);
            bool quit = false;
            g_app.runModal(makeOptionsScreen(&quit));
            if (quit) endSession();
        } else if (name == "help") {
            g_app.runModal(makeHelpScreen());
        }
    }

    void setMode(int mode)
    {
        m_mode = mode;
        m_menuOpen = false;
        m_pressObj = 0;
        if (mode != M_USE_ITEM) m_useItem = 0;
        if (mode != M_USE_SKILL) m_useSkill = -1;
    }

    void useSkill(int skill)
    {
        if (!playerCanAct()) return;
        if (skill == SKILL_SNEAK) {
            actionUseSkill(dude(), SKILL_SNEAK, nullptr);
            return;
        }
        m_useSkill = skill;
        m_mode = M_USE_SKILL;
    }

    void toggleHands()
    {
        Object* d = dude();
        int oldCode = fidWeapon(d->fid);
        g_game.activeHand ^= 1;
        critterUpdateArt(d);
        int newCode = fidWeapon(d->fid);
        if (oldCode != newCode && !animBusy(d) && d->tile >= 0) {
            // put the old weapon away and take out the new one (art switches in between)
            d->fid = makeFid(OBJ_CRITTER, fidIndex(d->fid), ANIM_STAND, oldCode);
            animBegin(ANIMSEQ_REPLACE);
            if (oldCode) animAnimate(d, ANIM_PUT_AWAY, 0);
            u32 du = d->uid;
            animCallback(d, [du] {
                if (Object* o = g_world.byUid(du)) critterStand(o, o->rotation), o->fid = makeFid(OBJ_CRITTER, fidIndex(o->fid), ANIM_STAND, 0);
            });
            if (newCode) animTakeOut(d, newCode, -1);
            animCallback(d, [du] {
                if (Object* o = g_world.byUid(du)) critterUpdateArt(o);
            });
            animEnd();
        }
        if (m_mode == M_TARGET) setMode(M_MOVE);
    }

    void startTargeting()
    {
        if (!playerCanAct()) return;
        m_mode = M_TARGET;
    }

    // Left click on the item button.
    void useActiveItem()
    {
        if (!playerCanAct()) return;
        Object* item = activeItem();
        int action = g_game.handMode[g_game.activeHand];
        if (isWeaponSlot(item)) {
            if (action == HA_RELOAD && item) {
                if (!combatUseAp(dude(), 2)) return;
                if (weaponReloadFromInventory(dude(), item) > 0) {
                    const Proto* p = item->proto();
                    char code = p && p->weapon.soundCode ? (char)tolower(p->weapon.soundCode) : 'a';
                    soundPlaySfx(strFormat("wr%c1xxx1", code));
                } else {
                    displayMessage("You have no ammunition for that weapon.");
                }
                cycleHandAction();
                return;
            }
            startTargeting();
            return;
        }
        const Proto* p = item->proto();
        if (p && (p->extFlags & PXF_CAN_USE_ON)) {
            m_useItem = item->uid;
            m_mode = M_USE_ITEM;
        } else if (p && ((p->extFlags & PXF_CAN_USE) || p->itemType == ITEM_DRUG)) {
            if (actionUseItem(dude(), item)) critterUpdateArt(dude());
        } else {
            displayMessage("That does nothing.");
        }
    }

    // ------------------------------------------------------------------ world mouse
    void handleWorldMouse(float dt)
    {
        Input& in = g_app.input;
        Object* d = dude();
        float wx = in.worldX, wy = in.worldY;
        bool overUi = overBar() || ui::mouseTaken();
        int hex = overUi ? -1 : g_view.hexAt(wx, wy);

        // stationary mouse detection (hover look)
        if (fabsf(in.uiX - m_lastMX) > 0.5f || fabsf(in.uiY - m_lastMY) > 0.5f) {
            m_still = 0;
            m_lastMX = in.uiX;
            m_lastMY = in.uiY;
            m_showPick = false;
        } else {
            m_still += dt;
        }

        g_view.hoverHex = (m_mode == M_MOVE && !overUi && !m_scrollCursor && in.mouseInWindow) ? hex : -1;
        g_view.hoverHexColor = 0xFFFF2020u;
        if (m_mode == M_MOVE && hex >= 0 && g_world.blockingAt(d, hex, d->elevation)) g_view.hoverHexColor = 0xFFFFFF40u;

        // action menu (click and hold in command mode)
        if (m_menuOpen) {
            float dy = in.uiY - m_menuAnchorY;
            if (fabsf(dy) > 10) {
                m_menuSel = clampv(m_menuSel + (dy > 0 ? 1 : -1), 0, (int)m_menuItems.size() - 1);
                m_menuAnchorY = in.uiY;
            }
            if (in.released[0] || !in.down[0]) {
                m_menuOpen = false;
                Object* t = g_world.byUid(m_menuTarget);
                if (t && m_menuSel >= 0 && m_menuSel < (int)m_menuItems.size()) doAction(m_menuItems[m_menuSel], t);
            }
            return;
        }

        if (overUi) {
            m_pressObj = 0;
            return;
        }

        // right click: toggle modes
        if (in.pressed[1]) {
            if (m_mode == M_MOVE) setMode(M_COMMAND);
            else setMode(M_MOVE);
            return;
        }

        Object* under = nullptr;
        if (m_mode != M_MOVE) under = pickObject(wx, wy);
        m_hover = under ? under->uid : 0;

        if (m_mode == M_COMMAND) {
            if (under && m_still >= 0.25f && !m_showPick) {
                m_showPick = true;
                if (m_lastLooked != under->uid && playerCanAct()) {
                    m_lastLooked = under->uid;
                    actionLook(d, under, false);
                }
            }
            if (!under) m_lastLooked = 0;
            if (in.pressed[0] && under) {
                m_pressObj = under->uid;
                m_pressTime = 0;
            }
            if (m_pressObj) {
                m_pressTime += dt;
                Object* t = g_world.byUid(m_pressObj);
                if (!t) {
                    m_pressObj = 0;
                } else if (in.released[0]) {
                    m_pressObj = 0;
                    if (playerCanAct()) doAction(defaultActionFor(t), t);
                } else if (m_pressTime > 0.35f && in.down[0] && playerCanAct()) {
                    m_pressObj = 0;
                    m_menuItems = actionMenuFor(t);
                    if (!m_menuItems.empty()) {
                        m_menuOpen = true;
                        m_menuTarget = t->uid;
                        m_menuSel = 0;
                        m_menuX = in.uiX - ui::originX();
                        m_menuY = in.uiY - ui::originY();
                        m_menuAnchorY = in.uiY;
                    }
                }
            }
            return;
        }

        if (!in.pressed[0] || !playerCanAct()) return;

        switch (m_mode) {
        case M_MOVE:
            if (hex >= 0) actionMoveTo(d, hex, in.shift || in.doubleClick);
            break;
        case M_TARGET:
            if (under && under->isCritter() && under != d) attack(under);
            break;
        case M_USE_ITEM: {
            Object* item = g_world.byUid(m_useItem);
            if (under && item) actionUseItemOn(d, item, under);
            setMode(M_MOVE);
            break;
        }
        case M_USE_SKILL:
            if (under) actionUseSkill(d, m_useSkill, under);
            setMode(M_MOVE);
            break;
        default: break;
        }
    }

    Object* pickObject(float wx, float wy)
    {
        // critters and items first (they stand on scenery), then everything else
        Object* o = g_view.pick(wx, wy, (1u << OBJ_CRITTER) | (1u << OBJ_ITEM));
        if (!o) o = g_view.pick(wx, wy, (1u << OBJ_SCENERY) | (1u << OBJ_WALL));
        return o;
    }

    void attack(Object* target)
    {
        int action = g_game.handMode[g_game.activeHand];
        int hitMode = hitModeFor(action);
        int location = HIT_UNCALLED;
        if (actionAimed(action)) {
            int loc = -1;
            g_app.runModal(makeCalledShotScreen(target, hitMode, &loc));
            if (loc < 0) return;
            location = loc;
        }
        if (!inCombat()) combatStart(dude(), target);
        combatAttack(dude(), target, hitMode, location);
    }

    void doAction(int action, Object* t)
    {
        Object* d = dude();
        switch (action) {
        case ACT_LOOK: actionLook(d, t, true); break;
        case ACT_TALK: actionTalk(d, t); break;
        case ACT_USE:
            if (t->isItem()) actionPickup(d, t);
            else actionUse(d, t);
            break;
        case ACT_ROTATE: actionRotate(d); break;
        case ACT_INVENTORY: {
            u32 chosen = 0;
            g_app.runModal(makeInventoryScreen(INV_USE_ON, t, &chosen));
            Object* item = g_world.byUid(chosen);
            if (item) actionUseItemOn(d, item, t);
            break;
        }
        case ACT_SKILL: {
            int skill = -1;
            g_app.runModal(makeSkilldexScreen(&skill));
            if (skill == SKILL_SNEAK) actionUseSkill(d, skill, nullptr);
            else if (skill >= 0) actionUseSkill(d, skill, t);
            break;
        }
        default: break;
        }
    }

    // ------------------------------------------------------------------ cursor & action icons
    void drawWorldCursor()
    {
        if (m_scrollCursor) {
            g_app.setCursor(m_scrollCursor, 0, 0);
            return;
        }
        if (overBar()) {
            if (ui::mouseIn(kMonX, kBarY + kMonY, kMonW, kMonH)) g_app.setCursor(ui::mouseY() < kBarY + kMonY + kMonH / 2 ? "suparrow" : "sdnarrow", 5, 0);
            return;
        }
        if (inCombat() && !combatIsPlayerTurn()) {
            g_app.setCursor("wait2", 6, 7, (int)(m_time * 10) % 6);
            return;
        }
        float mx = ui::mouseX(), my = ui::mouseY();
        switch (m_mode) {
        case M_MOVE: g_app.setCursor("blank", 0, 0); break;
        case M_COMMAND: {
            g_app.setCursor("actarrow", 0, 0);
            if (m_menuOpen) {
                drawActionMenu();
                g_app.setCursor("blank", 0, 0);
            } else if (m_showPick) {
                Object* t = g_world.byUid(m_hover);
                int a = defaultActionFor(t);
                if (a >= 0 && a < ACT_COUNT) {
                    float x = mx + ui::artW("actarrow"), y = my;
                    if (x + 40 > 640 + ui::originX()) {
                        x = mx - ui::artW("actarrom") - 40;
                        g_app.setCursor("actarrom", ui::artW("actarrom") - 1, 0);
                    }
                    if (y + 40 > kBarY) y = kBarY - 40;
                    ui::draw(kActionIcons[a][0], x, y);
                }
            }
            break;
        }
        case M_TARGET: {
            g_app.setCursor("crsshair", 10, 10);
            Object* t = g_world.byUid(m_hover);
            if (t && t->isCritter() && t != dude()) {
                int action = g_game.handMode[g_game.activeHand];
                int chance = combatToHit(dude(), dude()->tile, t, hitModeFor(action), HIT_UNCALLED);
                ui::draw("acttohit", mx + 12, my - 10);
                ui::text(kFont, strFormat("%d%%", clampv(chance, 0, 95)), mx + 20, my - 6, ui::kGreen, ui::T_SHADOW);
            }
            break;
        }
        case M_USE_ITEM:
        case M_USE_SKILL: g_app.setCursor("crossuse", 10, 10); break;
        }
    }

    void drawActionMenu()
    {
        float x = m_menuX, y = m_menuY;
        float iconX = x + ui::artW("actarrow");
        float h = 40.0f * m_menuItems.size();
        if (y + h > kBarY) y = std::max(0.0f, kBarY - h);
        ui::draw("actarrow", x, m_menuY);
        for (int i = 0; i < (int)m_menuItems.size(); i++) {
            int a = m_menuItems[i];
            ui::draw(kActionIcons[a][i == m_menuSel ? 1 : 0], iconX, y + 40.0f * i);
            if (a == ACT_SKILL) {
                ui::fill(iconX + 4, y + 40.0f * i + 4, 32, 32, 0xFF202018u);
                ui::text(kFont, "SKILL", iconX + 20, y + 40.0f * i + 15, i == m_menuSel ? ui::kYellow : ui::kGreen, ui::T_CENTER | ui::T_SHADOW);
            }
        }
    }

    // ------------------------------------------------------------------ interface bar
    void layoutBar()
    {
        using namespace ui;
        Object* d = dude();
        if (!d) return;
        draw("iface", 0, kBarY);

        drawMonitor();

        if (button("invbutup", "invbutdn", 211, kBarY + 41, 0, SND_SMALL)) m_pendingScreen = "inventory";
        if (button("optiup", "optidn", 210, kBarY + 62, 0, SND_SMALL)) m_pendingScreen = "options";
        if (button("bigredup", "bigreddn", 218, kBarY + 6, 0, SND_SMALL) && playerCanAct()) toggleHands();
        if (button("bigredup", "bigreddn", 523, kBarY + 7, 0, SND_SMALL)) m_pendingScreen = "skilldex";
        if (button("mapup", "mapdn", 526, kBarY + 40, 0, SND_SMALL)) m_pendingScreen = "automap";
        if (button("chaup", "chadn", 526, kBarY + 59, 0, SND_SMALL)) m_pendingScreen = "character";
        if (button("pipup", "pipdn", 526, kBarY + 78, 0, SND_SMALL)) m_pendingScreen = "pipboy";

        drawItemSlot();

        // hit points & armor class counters
        int maxHp = std::max(1, critterStat(d, STAT_MAX_HP));
        int hp = d->critter.hp;
        int color = hp < maxHp / 4 ? 240 : hp < maxHp / 2 ? 120 : 0;
        drawCounter(473, kBarY + 40, hp, color);
        drawCounter(473, kBarY + 75, critterAC(d), 0);

        // action point lights: lit only during the player's combat turn (the demo art has them all lit)
        {
            int ap = combatIsPlayerTurn() ? d->critter.ap : 0;
            int freeMove = combatIsPlayerTurn() ? combatFreeMove() : 0;
            DrawParams off;
            off.light = 0.22f;
            for (int i = 0; i < 10; i++) {
                float lx = 316 + 9.0f * i, ly = kBarY + 14;
                if (i < ap) draw("hlgrn", lx, ly);
                else if (i < ap + freeMove) draw("hlyel", lx, ly);
                else draw("hlgrn", lx, ly, 0, off);
            }
        }

        // combat panel (end turn / end combat)
        bool combat = inCombat();
        float target = combat ? 4.0f : 0.0f;
        if (!drawing()) {
            if (m_panel < target) m_panel = std::min(target, m_panel + g_app.dt * 10);
            if (m_panel > target) m_panel = std::max(target, m_panel - g_app.dt * 10);
        }
        draw("endanim", 580, kBarY + 38, (int)m_panel);
        if (combat && m_panel >= 4.0f) {
            bool mine = combatIsPlayerTurn();
            draw(mine ? "endltgrn" : "endltred", 580, kBarY + 38);
            if (button("endturnu", "endturnd", 590, kBarY + 43, 0, SND_MEDIUM, mine)) combatEndPlayerTurn();
            if (button("endcmbtu", "endcmbtd", 590, kBarY + 65, 0, SND_MEDIUM, mine)) combatTryEnd();
        }
        // the bar itself swallows clicks
        if (!drawing() && overBar() && (g_app.input.pressed[0] || g_app.input.pressed[1])) takeMouse();
    }

    void drawCounter(float x, float y, int value, int colorOffset)
    {
        value = clampv(value, -999, 999);
        int v = abs(value);
        // sign, hundreds, tens, ones from numbers.frm (9x17 digits; colour bands at offsets 0/120/240)
        ui::drawPart("numbers", value >= 0 ? 114 : 108, 0, 6, 17, x, y);
        ui::drawPart("numbers", colorOffset + 9 * (v / 100 % 10), 0, 9, 17, x + 6, y);
        ui::drawPart("numbers", colorOffset + 9 * (v / 10 % 10), 0, 9, 17, x + 15, y);
        ui::drawPart("numbers", colorOffset + 9 * (v % 10), 0, 9, 17, x + 24, y);
    }

    void drawMonitor()
    {
        using namespace ui;
        float x = kMonX, y = kBarY + kMonY;
        int lh = lineHeight(kFont);
        int visible = (int)kMonH / lh;
        // wrap the most recent messages
        std::vector<std::pair<std::string, u32>> lines;
        const auto& log = g_game.log;
        int first = std::max(0, (int)log.size() - 40);
        for (int i = first; i < (int)log.size(); i++) {
            auto w = wrap(kFont, std::string("\x95") + log[i].text, (int)kMonW - 2);
            for (auto& l : w) lines.push_back({ l, log[i].color });
        }
        int maxScroll = std::max(0, (int)lines.size() - visible);
        if (!drawing()) {
            if (area(kMonX, y, kMonW, kMonH / 2)) m_logScroll++;
            if (area(kMonX, y + kMonH / 2, kMonW, kMonH / 2)) m_logScroll--;
            if (hovered(kMonX, y, kMonW, kMonH) && g_app.input.wheel) m_logScroll += g_app.input.wheel > 0 ? 1 : -1;
            if (log.size() != m_logSize) {
                m_logSize = log.size();
                m_logScroll = 0;
            }
        }
        m_logScroll = clampv(m_logScroll, 0, maxScroll);
        int start = (int)lines.size() - visible - m_logScroll;
        for (int i = 0; i < visible; i++) {
            int li = start + i;
            if (li < 0 || li >= (int)lines.size()) continue;
            text(kFont, lines[li].first, x, y + (float)(i * lh), lines[li].second);
        }
    }

    void drawItemSlot()
    {
        using namespace ui;
        Object* item = activeItem();
        float x = kItemX, y = kBarY + kItemY;
        if (!drawing()) {
            if (area(x, y, kItemW, kItemH, 0, SND_SMALL)) useActiveItem();
            if (rightClicked(x, y, kItemW, kItemH)) {
                playClick(SND_SMALL, true);
                cycleHandAction();
            }
            return;
        }
        bool pressedNow = hovered(x, y, kItemW, kItemH) && g_app.input.down[0];
        float px = pressedNow ? 1.0f : 0.0f, py = pressedNow ? -2.0f : 0.0f;
        draw(pressedNow ? "sattkbdn" : "sattkbup", x, y);
        DrawParams dim;
        if (pressedNow) dim.light = 0.8f;
        int action = g_game.handMode[g_game.activeHand];
        int ap = -1;
        if (isWeaponSlot(item)) {
            if (action == HA_RELOAD && item) {
                draw("reload", x + 181 - artW("reload") + px, y + 7 + py, 0, dim);
                ap = 2;
            } else {
                int hm = hitModeFor(action);
                if (const char* icon = attackIcon(weaponAttackType(item, hm))) draw(icon, x + 181 - artW(icon) + px, y + 7 + py, 0, dim);
                if (actionAimed(action)) draw("bullseye", x + 181 - artW("bullseye") + px, y + 60 - artH("bullseye") + py, 0, dim);
                ap = weaponApCost(dude(), item, hm, actionAimed(action));
            }
        } else {
            const Proto* p = item->proto();
            if (p && (p->extFlags & PXF_CAN_USE_ON)) draw("useon", x + 181 - artW("useon") + px, y + 7 + py, 0, dim);
            else if (p && ((p->extFlags & PXF_CAN_USE) || p->itemType == ITEM_DRUG)) draw("uset", x + 181 - artW("uset") + px, y + 7 + py, 0, dim);
            ap = 2;
        }
        if (ap >= 0 && ap < 10) {
            float bx = x + 7 + px, by = y + 60 - artH("mvepnt") + py;
            draw("mvepnt", bx, by, 0, dim);
            drawPart("mvenum", ap * 10, 0, 10, artH("mvenum"), bx + artW("mvepnt"), y + 60 - artH("mvenum") + py, dim);
        }
        fill(463, kBarY + 26, 1, 70, 0xFF282828u);
        if (item) {
            const Proto* p = item->proto();
            Sprite* s = p ? g_art.sprite(p->invFid) : nullptr;
            if (s) {
                const SheetFrame& f = s->frame(0, 0);
                float w = (float)f.w, h = (float)f.h;
                float k = std::min(1.0f, std::min((kItemW - 20) / w, (kItemH - 10) / h));
                w *= k;
                h *= k;
                drawSpriteScaled(s, 0, 0, x + (kItemW - w) / 2 + px, y + (kItemH - h) / 2 + py, w, h, dim);
            }
            // ammo bar
            int ratio = 0;
            if (p && p->itemType == ITEM_WEAPON && p->weapon.maxAmmo > 0) ratio = (int)((double)item->ammo / p->weapon.maxAmmo * 70.0);
            ratio &= ~1;
            for (int i = 0; i < ratio; i += 2) fill(463, kBarY + 26 + 70 - ratio + i, 1, 1, 0xFF64E464u);
        }
    }

    int m_mode = M_MOVE;
    u32 m_useItem = 0;
    int m_useSkill = -1;
    u32 m_hover = 0, m_lastLooked = 0;
    float m_still = 0, m_lastMX = -1, m_lastMY = -1;
    bool m_showPick = false;
    u32 m_pressObj = 0;
    float m_pressTime = 0;
    bool m_menuOpen = false;
    std::vector<int> m_menuItems;
    int m_menuSel = 0;
    u32 m_menuTarget = 0;
    float m_menuX = 0, m_menuY = 0, m_menuAnchorY = 0;
    int m_logScroll = 0;
    size_t m_logSize = 0;
    float m_panel = 0;
    float m_time = 0;
    const char* m_scrollCursor = nullptr;
    std::string m_pendingScreen;
    bool m_debug = false;
    bool m_deathShown = false;
    float m_deathTimer = 0;
};

} // namespace

std::unique_ptr<Screen> makeGameScreen() { return std::make_unique<GameScreenImpl>(); }

} // namespace fo
