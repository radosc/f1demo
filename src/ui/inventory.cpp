// Inventory screens: player inventory, looting containers/bodies, stealing, and picking an item to use on
// something. Layout follows the original windows (invbox / loot / use art).
#include "ui/screens.h"
#include "ui/uikit.h"
#include "audio/sound.h"
#include "game/actions.h"
#include "game/anim.h"
#include "game/art.h"
#include "game/combat.h"
#include "game/critter.h"
#include "game/game.h"
#include "game/hex.h"
#include "game/item.h"
#include "game/msg.h"
#include "game/proto.h"
#include "game/script.h"

#include <windows.h>

namespace fo {

namespace {

constexpr float kWinX = 80, kWinY = 0;
constexpr float kSlotW = 64, kSlotH = 48;
constexpr int kSlots = 6;
constexpr float kLeftListX = 46, kListY = 35, kRightListX = 424;
constexpr float kArmorX = 154, kArmorY = 183, kLeftHandX = 154, kRightHandX = 245, kHandY = 286, kBigW = 90, kBigH = 61;
constexpr float kBodyW = 60, kBodyH = 100;
constexpr float kSummaryX = 297, kSummaryY = 44;
constexpr int kFont = 101;

enum SlotKind { SK_NONE, SK_LEFT_LIST, SK_RIGHT_LIST, SK_ARMOR, SK_LEFT_HAND, SK_RIGHT_HAND };
enum InvAction { IA_CANCEL, IA_LOOK, IA_USE, IA_DROP, IA_UNLOAD };

const char* kIcons[5][2] = { { "canceln", "cancelh" }, { "lookn", "lookh" }, { "usegetn", "usegeth" }, { "dropn", "droph" }, { "canceln", "cancelh" } };

std::string invMsg(int n, const char* fb)
{
    std::string s = gameMsg("inventry", n);
    return s.empty() || s == "Error" ? std::string(fb) : s;
}

struct Entry {
    Object* obj = nullptr;
    int count = 1;
};

// Items listed in a scroller: the player's list hides equipped items.
std::vector<Entry> listItems(Object* owner, bool hideEquipped)
{
    std::vector<Entry> v;
    if (!owner) return v;
    for (auto it = owner->inventory.rbegin(); it != owner->inventory.rend(); ++it) {
        if (hideEquipped && (it->obj->flags & OF_EQUIPPED)) continue;
        v.push_back({ it->obj, it->count });
    }
    return v;
}

Sprite* invSprite(Object* item)
{
    const Proto* p = item ? item->proto() : nullptr;
    if (!p) return nullptr;
    if (Sprite* s = g_art.sprite(p->invFid)) return s;
    return g_art.sprite(item->fid);
}

void drawItemFit(Object* item, float x, float y, float w, float h, float alpha = 1.0f)
{
    Sprite* s = invSprite(item);
    if (!s) return;
    const SheetFrame& f = s->frame(0, 0);
    float k = std::min(1.0f, std::min(w / f.w, h / f.h));
    float dw = f.w * k, dh = f.h * k;
    DrawParams p;
    p.alphaMul = alpha;
    ui::drawSpriteScaled(s, 0, 0, floorf(x + (w - dw) / 2), floorf(y + (h - dh) / 2), dw, dh, p);
}

class InventoryScreen : public Screen {
public:
    InventoryScreen(int mode, Object* target, u32* chosen) : m_mode(mode), m_target(target ? target->uid : 0), m_chosen(chosen)
    {
        if (m_chosen) *m_chosen = 0;
    }

    void onEnter() override { m_dude = g_world.dude ? g_world.dude->uid : 0; }

    bool transparent() const override { return true; }

    void update(float dt) override
    {
        m_time += dt;
        if (!dude()) {
            done = true;
            return;
        }
        if (m_quantity.active) {
            ui::Pass p(false);
            layoutQuantity();
            return;
        }
        m_pendingAction = -1;
        {
            ui::Pass p(false);
            layout();
        }
        handleMouse();
    }

    void drawUI() override
    {
        if (!dude()) return;
        ui::Pass p(true);
        layout();
        if (m_quantity.active) layoutQuantity();
        drawCursor();
    }

private:
    Object* dude() const { return g_world.byUid(m_dude); }
    Object* target() const { return g_world.byUid(m_target); }
    bool twoSided() const { return m_mode == INV_LOOT || m_mode == INV_STEAL; }
    const char* background() const { return twoSided() ? "loot" : (m_mode == INV_USE_ON ? "use" : "invbox"); }

    // ------------------------------------------------------------------ layout (both passes)
    void layout()
    {
        using namespace ui;
        Object* d = dude();
        draw(background(), kWinX, kWinY);
        float X = kWinX, Y = kWinY;

        // player list
        auto left = listItems(d, true);
        m_scrollL = clampv(m_scrollL, 0, std::max(0, (int)left.size() - kSlots));
        drawList(left, m_scrollL, X + kLeftListX, Y + kListY, SK_LEFT_LIST);
        if (button("invupout", "invupin", X + 128, Y + 39, VK_UP, SND_SMALL, m_scrollL > 0)) m_scrollL--;
        if (button("invdnout", "invdnin", X + 128, Y + 62, VK_DOWN, SND_SMALL, m_scrollL + kSlots < (int)left.size())) m_scrollL++;
        drawBody(d, X + 176, Y + 37);

        if (twoSided()) {
            Object* t = target();
            if (!t) {
                done = true;
                return;
            }
            auto right = listItems(t, false);
            m_scrollR = clampv(m_scrollR, 0, std::max(0, (int)right.size() - kSlots));
            drawList(right, m_scrollR, X + kRightListX, Y + kListY, SK_RIGHT_LIST);
            if (button("invupout", "invupin", X + 379, Y + 39, 0, SND_SMALL, m_scrollR > 0)) m_scrollR--;
            if (button("invdnout", "invdnin", X + 379, Y + 62, 0, SND_SMALL, m_scrollR + kSlots < (int)right.size())) m_scrollR++;
            drawBody(t, X + 297, Y + 37);
            if (button("lilredup", "lilreddn", X + 288, Y + 328, VK_ESCAPE, SND_SMALL)) close();
        } else if (m_mode == INV_USE_ON) {
            if (button("lilredup", "lilreddn", X + 233, Y + 328, VK_ESCAPE, SND_SMALL)) close();
        } else {
            drawEquipSlot(itemWorn(d), X + kArmorX, Y + kArmorY, SK_ARMOR);
            drawEquipSlot(itemLeftHand(d), X + kLeftHandX, Y + kHandY, SK_LEFT_HAND);
            drawEquipSlot(itemRightHand(d), X + kRightHandX, Y + kHandY, SK_RIGHT_HAND);
            drawSummary(d, X + kSummaryX, Y + kSummaryY);
            if (button("lilredup", "lilreddn", X + 437, Y + 329, VK_ESCAPE, SND_SMALL)) close();
            if (key('I')) close();
        }
        if (!drawing() && g_app.input.pressed[1] && !m_menu.open) {
            m_arrow = !m_arrow;
            takeMouse();
        }
    }

    void close()
    {
        if (!m_drag.item) done = true;
    }

    void drawList(const std::vector<Entry>& items, int scroll, float x, float y, int kind)
    {
        for (int i = 0; i < kSlots; i++) {
            int idx = scroll + i;
            float sy = y + kSlotH * i;
            if (idx >= (int)items.size()) continue;
            const Entry& e = items[idx];
            bool dragged = m_drag.item == e.obj->uid && m_drag.from == kind;
            if (ui::drawing()) {
                drawItemFit(e.obj, x + 4, sy + 4, kSlotW - 8, kSlotH - 8, dragged ? 0.35f : 1.0f);
                if (e.count > 1) ui::text(kFont, strFormat("x%d", e.count), x + 4, sy + 4, ui::kGreen);
            }
        }
    }

    void drawEquipSlot(Object* item, float x, float y, int kind)
    {
        if (!item || !ui::drawing()) return;
        bool dragged = m_drag.item == item->uid && m_drag.from == kind;
        drawItemFit(item, x + 4, y + 4, kBigW - 8, kBigH - 8, dragged ? 0.35f : 1.0f);
    }

    void drawBody(Object* c, float x, float y)
    {
        if (!ui::drawing() || !c) return;
        int rot = (int)(m_time / 0.5f) % 6;
        int fid = c->fid;
        if (c->isCritter()) {
            fid = makeFid(OBJ_CRITTER, fidIndex(c->fid), c->isDead() ? fidAnim(c->fid) : ANIM_STAND, fidWeapon(c->fid), 0);
            if (c->isDead()) rot = c->rotation;
        } else {
            rot = 0;
        }
        Sprite* s = g_art.sprite(fid);
        if (!s) return;
        int frame = c->isCritter() && c->isDead() ? s->frameCount() - 1 : 0;
        const SheetFrame& f = s->frame(rot, frame);
        float k = std::min(1.0f, std::min(kBodyW / f.w, kBodyH / f.h));
        float w = f.w * k, h = f.h * k;
        ui::drawSpriteScaled(s, rot, frame, floorf(x + (kBodyW - w) / 2), floorf(y + (kBodyH - h) / 2), w, h);
    }

    void drawSummary(Object* d, float x, float y)
    {
        using namespace ui;
        if (!drawing()) return;
        int lh = lineHeight(kFont);
        if (!m_info.empty()) {
            text(kFont, m_infoTitle, x, y, kGreen);
            fill(x, y + 3.0f * lh / 2, 143, 1, kGreen);
            textBox(kFont, m_info, x, y + 2.0f * lh, 143, kGreen, y + 186);
            return;
        }
        text(kFont, critterName(d), x, y, kGreen);
        fill(x, y + 3.0f * lh / 2, 143, 1, kGreen);
        float ry = y + 2.0f * lh;
        static const int kStats1[7] = { STAT_CURRENT_HP, STAT_AC, STAT_DT_NORMAL, STAT_DT_LASER, STAT_DT_FIRE, STAT_DT_PLASMA, STAT_DT_EXPLOSION };
        static const int kStats2[7] = { STAT_MAX_HP, -1, STAT_DR_NORMAL, STAT_DR_LASER, STAT_DR_FIRE, STAT_DR_PLASMA, STAT_DR_EXPLOSION };
        static const char* kAbbr[7] = { "ST", "PE", "EN", "CH", "IN", "AG", "LK" };
        static const char* kNames[7] = { "Hit Points", "Armor Class", "   Normal", "   Laser", "   Fire", "   Plasma", "   Explosion" };
        for (int i = 0; i < 7; i++) {
            text(kFont, invMsg(i, kAbbr[i]), x, ry + lh * i, kGreen);
            text(kFont, std::to_string(critterStat(d, i)), x + 24, ry + lh * i, kGreen);
            text(kFont, invMsg(7 + i, kNames[i]), x + 40, ry + lh * i, kGreen);
            std::string v;
            if (i == 0) v = strFormat("%d/%d", d->critter.hp, critterStat(d, kStats2[i]));
            else if (kStats2[i] < 0) v = strFormat("   %d", critterAC(d));
            else v = strFormat("%d/%d%%", critterStat(d, kStats1[i]), critterStat(d, kStats2[i]));
            text(kFont, v, x + 150, ry + lh * i, kGreen, T_RIGHT);
        }
        float ly = y + 18.0f * lh / 2 + 4;
        fill(x, ly, 143, 1, kGreen);
        float wy = ry + lh * 8.0f;
        Object* hands[2] = { itemLeftHand(d), itemRightHand(d) };
        int modes[2] = { HIT_LEFT_PRIMARY, HIT_RIGHT_PRIMARY };
        for (int h = 0; h < 2; h++) {
            Object* item = hands[h];
            if (!item) {
                text(kFont, invMsg(14, "None"), x, wy, kGreen);
                text(kFont, strFormat("%s 1-%d", invMsg(15, "Dmg:").c_str(), critterStat(d, STAT_MELEE_DMG) + 2), x, wy + lh, kGreen);
                wy += 4.0f * lh;
                continue;
            }
            text(kFont, itemName(item), x, wy, kGreen);
            if (itemType(item) == ITEM_WEAPON) {
                int range = weaponRange(d, item, modes[h]);
                int a = weaponAttackType(item, modes[h]);
                bool melee = a == ATTACK_SWING || a == ATTACK_THRUST || a == ATTACK_PUNCH || a == ATTACK_KICK;
                int bonus = melee ? critterStat(d, STAT_MELEE_DMG) : 0;
                std::string dmg = strFormat("%s %d-%d", invMsg(15, "Dmg:").c_str(), weaponMinDamage(item), weaponMaxDamage(item) + bonus);
                if (!melee || range > 1) dmg += strFormat("   %s %d", invMsg(16, "Rng:").c_str(), range);
                text(kFont, dmg, x, wy + lh, kGreen);
                const Proto* p = item->proto();
                if (p && p->weapon.maxAmmo > 0) {
                    std::string ammo = item->ammoPid >= 0 ? g_protos.name(item->ammoPid) : std::string();
                    text(kFont, strFormat("%s %d/%d %s", invMsg(17, "Ammo:").c_str(), item->ammo, p->weapon.maxAmmo, ammo.c_str()), x, wy + 2.0f * lh, kGreen);
                }
            } else if (itemType(item) == ITEM_ARMOR) {
                text(kFont, invMsg(18, "(Not worn)"), x, wy + lh, kGreen);
            }
            wy += 4.0f * lh;
        }
    }

    // ------------------------------------------------------------------ hit testing
    struct Hit {
        int kind = SK_NONE;
        Object* item = nullptr;
        int count = 0;
    };

    Hit hitTest(float mx, float my)
    {
        Hit h;
        Object* d = dude();
        float X = kWinX, Y = kWinY;
        auto inRect = [&](float x, float y, float w, float hh) { return mx >= x && my >= y && mx < x + w && my < y + hh; };
        auto listHit = [&](Object* owner, bool hideEquipped, int scroll, float lx, int kind) {
            if (!inRect(X + lx, Y + kListY, kSlotW, kSlotH * kSlots)) return false;
            h.kind = kind;
            int idx = scroll + (int)((my - Y - kListY) / kSlotH);
            auto items = listItems(owner, hideEquipped);
            if (idx >= 0 && idx < (int)items.size()) {
                h.item = items[idx].obj;
                h.count = items[idx].count;
            }
            return true;
        };
        if (listHit(d, true, m_scrollL, kLeftListX, SK_LEFT_LIST)) return h;
        if (twoSided()) {
            if (listHit(target(), false, m_scrollR, kRightListX, SK_RIGHT_LIST)) return h;
            return h;
        }
        if (m_mode == INV_NORMAL) {
            if (inRect(X + kArmorX, Y + kArmorY, kBigW, kBigH)) {
                h.kind = SK_ARMOR;
                h.item = itemWorn(d);
            } else if (inRect(X + kLeftHandX, Y + kHandY, kBigW, kBigH)) {
                h.kind = SK_LEFT_HAND;
                h.item = itemLeftHand(d);
            } else if (inRect(X + kRightHandX, Y + kHandY, kBigW, kBigH)) {
                h.kind = SK_RIGHT_HAND;
                h.item = itemRightHand(d);
            }
            if (h.item) h.count = 1;
        }
        return h;
    }

    // ------------------------------------------------------------------ mouse
    void handleMouse()
    {
        Input& in = g_app.input;
        float mx = ui::mouseX(), my = ui::mouseY();
        if (m_menu.open) {
            float dy = in.uiY - m_menu.anchorY;
            if (fabsf(dy) > 10) {
                m_menu.sel = clampv(m_menu.sel + (dy > 0 ? 1 : -1), 0, (int)m_menu.items.size() - 1);
                m_menu.anchorY = in.uiY;
            }
            if (in.released[0] || !in.down[0]) {
                m_menu.open = false;
                Object* item = g_world.byUid(m_menu.item);
                if (item) doAction(m_menu.items[m_menu.sel], item, m_menu.kind);
            }
            return;
        }
        if (ui::mouseTaken() && !m_drag.item) return;

        if (m_drag.item) {
            if (in.released[0] || !in.down[0]) {
                Object* item = g_world.byUid(m_drag.item);
                Hit h = hitTest(mx, my);
                int from = m_drag.from;
                m_drag = {};
                if (item) dropOn(item, from, h);
            }
            return;
        }

        if (in.pressed[0]) {
            Hit h = hitTest(mx, my);
            if (!h.item) return;
            if (m_mode == INV_USE_ON) {
                if (m_chosen) *m_chosen = h.item->uid;
                done = true;
                return;
            }
            if (m_arrow) {
                m_press = { h.item->uid, h.kind, 0.0f };
                return;
            }
            m_drag.item = h.item->uid;
            m_drag.from = h.kind;
            soundPlaySfx("butin1");
        }
        if (m_press.item) {
            m_press.time += g_app.dt;
            Object* item = g_world.byUid(m_press.item);
            if (!item) {
                m_press = {};
            } else if (in.released[0]) {
                int kind = m_press.kind;
                m_press = {};
                doAction(IA_LOOK, item, kind);
            } else if (m_press.time > 0.3f) {
                m_menu.open = true;
                m_menu.item = item->uid;
                m_menu.kind = m_press.kind;
                m_menu.items = menuFor(item, m_press.kind);
                m_menu.sel = 0;
                m_menu.x = mx;
                m_menu.y = my;
                m_menu.anchorY = in.uiY;
                m_press = {};
            }
        }
    }

    std::vector<int> menuFor(Object* item, int kind)
    {
        std::vector<int> v = { IA_LOOK };
        const Proto* p = item->proto();
        bool mine = kind != SK_RIGHT_LIST;
        if (mine && p && (p->itemType == ITEM_DRUG || (p->extFlags & (PXF_CAN_USE | PXF_CAN_USE_ON)))) v.push_back(IA_USE);
        if (mine && m_mode == INV_NORMAL) v.push_back(IA_DROP);
        if (mine && p && p->itemType == ITEM_WEAPON && item->ammo > 0 && p->weapon.maxAmmo > 0) v.push_back(IA_UNLOAD);
        v.push_back(IA_CANCEL);
        return v;
    }

    void doAction(int action, Object* item, int kind)
    {
        Object* d = dude();
        switch (action) {
        case IA_LOOK: {
            m_infoTitle = itemName(item);
            bool handled = scriptHandled(item, SP_DESCRIPTION, d, item);
            m_info = handled ? std::string() : g_protos.description(item->pid);
            if (m_info.empty()) m_info = m_infoTitle;
            if (m_mode != INV_NORMAL) displayMessage(m_info);
            break;
        }
        case IA_USE: {
            const Proto* p = item->proto();
            if (p && (p->extFlags & PXF_CAN_USE_ON) && p->itemType != ITEM_DRUG) {
                actionUseItemOn(d, item, d);
            } else if (actionUseItem(d, item)) {
                critterUpdateArt(d);
            }
            m_info.clear();
            break;
        }
        case IA_DROP: {
            InvItem* e = g_world.invEntry(d, item);
            int count = e ? e->count : 1;
            if (count > 1) {
                askQuantity(count, [this, uid = item->uid](int n) {
                    Object* it = g_world.byUid(uid);
                    if (it && dude()) actionDrop(dude(), it, n);
                });
            } else {
                actionDrop(d, item, 1);
            }
            break;
        }
        case IA_UNLOAD: {
            if (item->ammo > 0 && item->ammoPid >= 0) {
                Object* ammo = g_world.create(item->ammoPid);
                ammo->ammo = item->ammo;
                item->ammo = 0;
                g_world.addToInventory(d, ammo, 1);
            }
            break;
        }
        default: break;
        }
        (void)kind;
    }

    // Moves `item` dragged from slot kind `from` onto the slot under the mouse.
    void dropOn(Object* item, int from, const Hit& h)
    {
        Object* d = dude();
        Object* t = target();
        soundPlaySfx("butout1");
        switch (h.kind) {
        case SK_ARMOR:
            if (itemType(item) != ITEM_ARMOR) return;
            itemEquip(d, item, 2);
            critterUpdateArt(d);
            break;
        case SK_LEFT_HAND:
        case SK_RIGHT_HAND: {
            int slot = h.kind == SK_LEFT_HAND ? 0 : 1;
            if (from == SK_LEFT_HAND || from == SK_RIGHT_HAND) {
                if (from == h.kind) return;
                Object* other = h.item;
                itemUnequip(d, item);
                if (other) itemUnequip(d, other);
                itemEquip(d, item, slot);
                if (other) itemEquip(d, other, slot == 0 ? 1 : 0);
            } else {
                if (from == SK_ARMOR) itemUnequip(d, item);
                itemEquip(d, item, slot);
            }
            critterUpdateArt(d);
            break;
        }
        case SK_LEFT_LIST:
            if (from == SK_ARMOR || from == SK_LEFT_HAND || from == SK_RIGHT_HAND) {
                itemUnequip(d, item);
                critterUpdateArt(d);
            } else if (from == SK_RIGHT_LIST && t) {
                transfer(item, t, d, true);
            }
            break;
        case SK_RIGHT_LIST:
            if (t && from == SK_LEFT_LIST) transfer(item, d, t, false);
            break;
        default: break;
        }
    }

    // Moves items between the player and the loot target (asks for a quantity for stacks).
    void transfer(Object* item, Object* from, Object* to, bool taking)
    {
        InvItem* e = g_world.invEntry(from, item);
        if (!e) return;
        u32 iu = item->uid, fu = from->uid, tu = to->uid;
        auto move = [this, iu, fu, tu, taking](int n) {
            Object* it = g_world.byUid(iu);
            Object* f = g_world.byUid(fu);
            Object* t = g_world.byUid(tu);
            if (!it || !f || !t) return;
            if (m_mode == INV_STEAL && !stealCheck(it, taking)) return;
            if (taking && t == dude() && inventoryWeight(t) + itemWeight(it) * n > critterCarryLimit(t)) {
                displayMessage("You cannot carry that much.");
                return;
            }
            bool equipped = (it->flags & OF_EQUIPPED) != 0;
            Object* moved = g_world.removeFromInventory(f, it, n);
            if (!moved) return;
            g_world.addToInventory(t, moved, n);
            if (equipped && f->isCritter()) critterUpdateArt(f);
        };
        if (e->count > 1) askQuantity(e->count, move);
        else move(1);
    }

    bool stealCheck(Object* item, bool taking)
    {
        Object* d = dude();
        Object* t = target();
        if (!t) return false;
        int mod = 1 - m_stealCount * 1 - 4 * std::max(0, itemSize(item));
        if (t->isCritter() && hex::inFrontOf(t->tile, d->tile) && t->rotation == hex::direction(t->tile, d->tile)) mod -= 25;
        if (t->critter.results & (DAM_KNOCKED_OUT | DAM_KNOCKED_DOWN)) mod += 20;
        int chance = std::min(95, critterSkill(d, SKILL_STEAL) + mod);
        bool ok = g_rng.range(1, 100) <= chance;
        m_stealCount++;
        std::string name = itemName(item);
        if (!ok) {
            displayMessage(strFormat(gameMsg("skill", 570).c_str(), name.c_str()));
            done = true;
            if (t->isCritter() && critterIsActive(t)) combatScriptAttack(t, d, 0, 1, 0, 0, 0x7FFFFFFF, 0, 0);
            return false;
        }
        displayMessage(strFormat(gameMsg("skill", 571).c_str(), name.c_str()));
        (void)taking;
        return true;
    }

    // ------------------------------------------------------------------ quantity popup
    struct Quantity {
        bool active = false;
        int max = 1, value = 1;
        std::function<void(int)> done;
    } m_quantity;

    void askQuantity(int max, std::function<void(int)> fn)
    {
        m_quantity.active = true;
        m_quantity.max = max;
        m_quantity.value = 1;
        m_quantity.done = std::move(fn);
    }

    void layoutQuantity()
    {
        using namespace ui;
        float w = (float)artW("medialog"), h = (float)artH("medialog");
        float x = floorf((640 - w) / 2), y = floorf((380 - h) / 2);
        draw("medialog", x, y);
        text(101, "Move how many?", x + w / 2, y + 22, 0xFFFF9442u, T_CENTER);
        int v = m_quantity.value;
        float nx = x + w / 2 - 21;
        drawPart("bignum", 14 * (v / 100 % 10), 0, 14, 24, nx, y + 40);
        drawPart("bignum", 14 * (v / 10 % 10), 0, 14, 24, nx + 14, y + 40);
        drawPart("bignum", 14 * (v % 10), 0, 14, 24, nx + 28, y + 40);
        if (button("slu", "sld", nx - 30, y + 44, VK_LEFT, SND_SMALL)) m_quantity.value = std::max(1, v - 1);
        if (button("sru", "srd", nx + 50, y + 44, VK_RIGHT, SND_SMALL)) m_quantity.value = std::min(m_quantity.max, v + 1);
        text(103, "ALL", x + w - 40, y + 48, 0xFF948C18u);
        if (area(x + w - 46, y + 44, 36, 16, 'A', SND_SMALL)) m_quantity.value = m_quantity.max;
        if (!drawing()) {
            for (u32 c : chars()) {
                if (c >= '0' && c <= '9') m_quantity.value = clampv(m_quantity.value * 10 + (int)(c - '0'), 1, m_quantity.max);
            }
            if (key(VK_BACK)) m_quantity.value = std::max(1, m_quantity.value / 10);
        }
        draw("donebox", x + 20, y + 81);
        text(103, "DONE", x + 55, y + 84, 0xFF948C18u);
        draw("donebox", x + w - 110, y + 81);
        text(103, "CANCEL", x + w - 75, y + 84, 0xFF948C18u);
        if (button("lilredup", "lilreddn", x + 33, y + 85, VK_RETURN, SND_SMALL)) {
            m_quantity.active = false;
            auto fn = std::move(m_quantity.done);
            if (fn) fn(m_quantity.value);
        }
        if (button("lilredup", "lilreddn", x + w - 97, y + 85, VK_ESCAPE, SND_SMALL)) m_quantity.active = false;
    }

    // ------------------------------------------------------------------ cursor
    void drawCursor()
    {
        float mx = ui::mouseX(), my = ui::mouseY();
        if (m_drag.item) {
            g_app.setCursor("blank", 0, 0);
            if (Object* item = g_world.byUid(m_drag.item)) drawItemFit(item, mx - 28, my - 20, 56, 40);
            return;
        }
        if (m_menu.open) {
            g_app.setCursor("blank", 0, 0);
            float ix = m_menu.x + ui::artW("actarrow");
            float h = 40.0f * m_menu.items.size();
            float y = m_menu.y;
            if (y + h > 480) y = 480 - h;
            ui::draw("actarrow", m_menu.x, m_menu.y);
            for (int i = 0; i < (int)m_menu.items.size(); i++) {
                int a = m_menu.items[i];
                ui::draw(kIcons[a][i == m_menu.sel ? 1 : 0], ix, y + 40.0f * i);
                if (a == IA_UNLOAD) {
                    ui::fill(ix + 4, y + 40.0f * i + 4, 32, 32, 0xFF202018u);
                    ui::text(kFont, "UNLD", ix + 20, y + 40.0f * i + 15, i == m_menu.sel ? ui::kYellow : ui::kGreen, ui::T_CENTER);
                }
            }
            return;
        }
        if (m_arrow) g_app.setCursor("actarrow", 0, 0);
        else g_app.setCursor("hand", 0, 0);
    }

    int m_mode;
    u32 m_target;
    u32* m_chosen;
    u32 m_dude = 0;
    int m_scrollL = 0, m_scrollR = 0;
    float m_time = 0;
    bool m_arrow = false;
    int m_pendingAction = -1;
    int m_stealCount = 0;
    std::string m_info, m_infoTitle;
    struct Drag {
        u32 item = 0;
        int from = SK_NONE;
    } m_drag;
    struct Press {
        u32 item = 0;
        int kind = SK_NONE;
        float time = 0;
    } m_press;
    struct Menu {
        bool open = false;
        u32 item = 0;
        int kind = SK_NONE;
        std::vector<int> items;
        int sel = 0;
        float x = 0, y = 0, anchorY = 0;
    } m_menu;
};

} // namespace

std::unique_ptr<Screen> makeInventoryScreen(int mode, Object* target, u32* chosen)
{
    return std::make_unique<InventoryScreen>(mode, target, chosen);
}

} // namespace fo
