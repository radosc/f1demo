#include "game/item.h"
#include "game/critter.h"
#include "game/game.h"
#include "game/msg.h"
#include "game/proto.h"

namespace fo {

static const int kAttackSkill[ATTACK_TYPE_COUNT] = { -1, SKILL_UNARMED, SKILL_UNARMED, SKILL_MELEE, SKILL_MELEE, SKILL_THROWING,
    SKILL_SMALL_GUNS, SKILL_SMALL_GUNS, SKILL_SMALL_GUNS };

static Object* findFlag(Object* critter, u32 flag)
{
    if (!critter) return nullptr;
    for (auto& e : critter->inventory)
        if (e.obj->flags & flag) return e.obj;
    return nullptr;
}

Object* itemWorn(Object* critter) { return findFlag(critter, OF_WORN); }
Object* itemRightHand(Object* critter) { return findFlag(critter, OF_RIGHT_HAND); }
Object* itemLeftHand(Object* critter) { return findFlag(critter, OF_LEFT_HAND); }

const Proto* itemProto(const Object* item) { return item ? g_protos.get(item->pid) : nullptr; }

int itemType(const Object* item)
{
    const Proto* p = itemProto(item);
    return (p && p->type() == OBJ_ITEM) ? p->itemType : -1;
}

std::string itemName(const Object* item) { return item ? g_protos.name(item->pid) : std::string(); }
std::string itemDescription(const Object* item) { return item ? g_protos.description(item->pid) : std::string(); }

int itemWeight(const Object* item)
{
    const Proto* p = itemProto(item);
    if (!p) return 0;
    int w = p->weight;
    if (p->itemType == ITEM_CONTAINER) {
        w += inventoryWeight(item);
    } else if (p->itemType == ITEM_WEAPON && item->ammo > 0 && item->ammoPid >= 0) {
        const Proto* ap = g_protos.get(item->ammoPid);
        if (ap && ap->ammo.quantity > 0) w += ap->weight * ((item->ammo - 1) / ap->ammo.quantity + 1);
    }
    return w;
}

int inventoryWeight(const Object* container)
{
    if (!container) return 0;
    int w = 0;
    for (auto& e : container->inventory) {
        const Proto* p = itemProto(e.obj);
        if (p && p->itemType == ITEM_AMMO && p->ammo.quantity > 0) {
            // whole boxes plus the partially filled top box
            int full = e.count - 1;
            w += p->weight * full + p->weight * ((e.obj->ammo + p->ammo.quantity - 1) / p->ammo.quantity);
        } else {
            w += itemWeight(e.obj) * e.count;
        }
    }
    return w;
}

int itemCost(const Object* item)
{
    const Proto* p = itemProto(item);
    if (!p) return 0;
    int c = p->cost;
    if (p->itemType == ITEM_CONTAINER) {
        for (auto& e : item->inventory) c += itemCost(e.obj) * e.count;
    } else if (p->itemType == ITEM_WEAPON && item->ammo > 0 && item->ammoPid >= 0) {
        const Proto* ap = g_protos.get(item->ammoPid);
        if (ap && ap->ammo.quantity > 0) c += ap->cost * item->ammo / ap->ammo.quantity;
    } else if (p->itemType == ITEM_AMMO && p->ammo.quantity > 0) {
        c = p->cost * item->ammo / p->ammo.quantity;
    }
    return c;
}

int itemSize(const Object* item)
{
    const Proto* p = itemProto(item);
    return p ? p->size : 0;
}

int critterCarryLimit(Object* critter) { return critterStat(critter, STAT_CARRY_WEIGHT); }

void critterUpdateArt(Object* c)
{
    if (!c || !c->isCritter()) return;
    int base = fidIndex(c->fid);
    int weaponCode = fidWeapon(c->fid);
    if (c == g_world.dude) {
        int gender = critterGender(c);
        const Proto* ap = itemProto(itemWorn(c));
        int armorFid = ap ? (gender == GENDER_FEMALE ? ap->armor.femaleFid : ap->armor.maleFid) : -1;
        if (armorFid >= 0) base = fidIndex(armorFid);
        else base = gender == GENDER_FEMALE ? 4 : 11; // hfjmps / hmjmps vault suits
        Object* w = g_game.activeHand == 0 ? itemLeftHand(c) : itemRightHand(c);
        weaponCode = weaponAnimCode(w);
    } else {
        Object* w = itemRightHand(c);
        if (!w) w = itemLeftHand(c);
        if (w) weaponCode = weaponAnimCode(w);
    }
    c->fid = makeFid(OBJ_CRITTER, base, fidAnim(c->fid), weaponCode, 0);
}

bool itemEquip(Object* critter, Object* item, int slot)
{
    if (!critter || !item || item->owner != critter) return false;
    const Proto* p = itemProto(item);
    if (!p) return false;
    u32 flag = slot == 0 ? OF_LEFT_HAND : slot == 1 ? OF_RIGHT_HAND : OF_WORN;
    if (slot == 2 && p->itemType != ITEM_ARMOR) return false;
    if (Object* cur = findFlag(critter, flag)) cur->flags &= ~flag;
    // stacked items: equip one unit
    InvItem* e = g_world.invEntry(critter, item);
    if (e && e->count > 1) {
        Object* one = g_world.removeFromInventory(critter, item, 1);
        critter->inventory.push_back({ one, 1 });
        one->owner = critter;
        item = one;
    }
    if (slot != 2) {
        // two handed weapons occupy both hands in the original only for art; keep simple: one slot
        item->flags &= ~OF_EQUIPPED;
    }
    item->flags |= flag;
    critterUpdateArt(critter);
    return true;
}

void itemUnequip(Object* critter, Object* item)
{
    if (!item) return;
    item->flags &= ~OF_EQUIPPED;
    // merge back into an identical stack
    if (critter && item->owner == critter) {
        for (size_t i = 0; i < critter->inventory.size(); i++) {
            InvItem& e = critter->inventory[i];
            if (e.obj == item) continue;
            if (e.obj->pid == item->pid && !(e.obj->flags & OF_EQUIPPED) && e.obj->ammo == item->ammo && e.obj->ammoPid == item->ammoPid &&
                e.obj->charges == item->charges && item->inventory.empty() && !item->script) {
                InvItem* mine = g_world.invEntry(critter, item);
                int n = mine ? mine->count : 1;
                g_world.removeFromInventory(critter, item, n);
                e.count += n;
                g_world.destroy(item);
                break;
            }
        }
    }
    critterUpdateArt(critter);
}

static int attackIndex(const Object* weapon, int hitMode)
{
    const Proto* p = itemProto(weapon);
    if (!p) return ATTACK_PUNCH;
    bool primary = hitMode == HIT_LEFT_PRIMARY || hitMode == HIT_RIGHT_PRIMARY;
    return primary ? (int)(p->extFlags & 0xF) : (int)((p->extFlags >> 4) & 0xF);
}

int weaponAttackType(const Object* weapon, int hitMode)
{
    if (!weapon || hitMode == HIT_PUNCH) return ATTACK_PUNCH;
    if (hitMode == HIT_KICK) return ATTACK_KICK;
    int a = attackIndex(weapon, hitMode);
    return clampv(a, 0, ATTACK_TYPE_COUNT - 1);
}

int weaponSkill(const Object* weapon, int hitMode)
{
    if (!weapon || hitMode == HIT_PUNCH || hitMode == HIT_KICK) return SKILL_UNARMED;
    int a = weaponAttackType(weapon, hitMode);
    int skill = kAttackSkill[a];
    if (skill < 0) return SKILL_UNARMED;
    if (skill == SKILL_SMALL_GUNS) {
        const Proto* p = itemProto(weapon);
        int dt = p ? p->weapon.damageType : 0;
        if (dt == DMG_LASER || dt == DMG_PLASMA || dt == DMG_ELECTRICAL) skill = SKILL_ENERGY_WEAPONS;
        else if (p && (p->extFlags & PXF_BIG_GUN)) skill = SKILL_BIG_GUNS;
    }
    return skill;
}

int weaponApCost(Object* critter, const Object* weapon, int hitMode, bool aimed)
{
    int ap;
    const Proto* p = itemProto(weapon);
    if (weapon && p && p->itemType != ITEM_WEAPON) return 2;
    if (!weapon || hitMode == HIT_PUNCH || hitMode == HIT_KICK) {
        ap = 3;
    } else {
        bool primary = hitMode == HIT_LEFT_PRIMARY || hitMode == HIT_RIGHT_PRIMARY;
        ap = primary ? p->weapon.ap1 : p->weapon.ap2;
        if (critter == g_world.dude && hasTrait(TRAIT_FAST_SHOT)) ap -= 1;
    }
    if (critter == g_world.dude) {
        int a = weaponAttackType(weapon, hitMode);
        bool melee = a == ATTACK_PUNCH || a == ATTACK_KICK || a == ATTACK_SWING || a == ATTACK_THRUST;
        bool ranged = a == ATTACK_FIRE_SINGLE || a == ATTACK_FIRE_BURST || a == ATTACK_FLAME;
        if (perkLevel(PERK_BONUS_HTH_ATTACKS) && melee) ap -= 1;
        if (perkLevel(PERK_BONUS_RATE_OF_FIRE) && ranged) ap -= 1;
    }
    if (aimed) ap += 1;
    return std::max(ap, 1);
}

int weaponRange(Object* critter, const Object* weapon, int hitMode)
{
    if (!weapon || hitMode == HIT_PUNCH || hitMode == HIT_KICK) return 1;
    const Proto* p = itemProto(weapon);
    if (!p) return 1;
    bool primary = hitMode == HIT_LEFT_PRIMARY || hitMode == HIT_RIGHT_PRIMARY;
    int range = primary ? p->weapon.range1 : p->weapon.range2;
    if (weaponAttackType(weapon, hitMode) == ATTACK_THROW) {
        int maxRange = 3 * critterStat(critter, STAT_ST);
        range = std::min(range, maxRange);
    }
    return std::max(range, 1);
}

int weaponMinDamage(const Object* w)
{
    const Proto* p = itemProto(w);
    return p ? p->weapon.minDamage : 1;
}

int weaponMaxDamage(const Object* w)
{
    const Proto* p = itemProto(w);
    return p ? p->weapon.maxDamage : 2;
}

int weaponDamageType(const Object* w)
{
    const Proto* p = itemProto(w);
    return p ? p->weapon.damageType : DMG_NORMAL;
}

int weaponAnimCode(const Object* w)
{
    const Proto* p = itemProto(w);
    if (!p || p->itemType != ITEM_WEAPON) return WANIM_NONE;
    return clampv(p->weapon.animCode, 0, WANIM_COUNT - 1);
}

bool weaponIsRanged(const Object* w, int hitMode)
{
    int a = weaponAttackType(w, hitMode);
    return a == ATTACK_FIRE_SINGLE || a == ATTACK_FIRE_BURST || a == ATTACK_FLAME || a == ATTACK_THROW;
}

bool weaponUsesAmmo(const Object* w)
{
    const Proto* p = itemProto(w);
    return p && p->itemType == ITEM_WEAPON && p->weapon.maxAmmo > 0;
}

int weaponAmmoPerShot(const Object* w, int hitMode)
{
    if (!weaponUsesAmmo(w)) return 0;
    if (weaponAttackType(w, hitMode) == ATTACK_FIRE_BURST) {
        const Proto* p = itemProto(w);
        return std::max(1, p->weapon.burstRounds);
    }
    return 1;
}

bool weaponCanReloadWith(const Object* weapon, const Object* ammo)
{
    const Proto* wp = itemProto(weapon);
    const Proto* ap = itemProto(ammo);
    if (!wp || !ap || wp->itemType != ITEM_WEAPON || ap->itemType != ITEM_AMMO) return false;
    if (wp->weapon.caliber != ap->ammo.caliber) return false;
    if (weapon->ammo != 0 && weapon->ammoPid != ammo->pid) return false;
    return true;
}

int weaponReload(Object* weapon, Object* ammo)
{
    if (!weaponCanReloadWith(weapon, ammo)) return -1;
    const Proto* wp = itemProto(weapon);
    int cap = wp->weapon.maxAmmo;
    int need = cap - weapon->ammo;
    if (need <= 0) return 0;
    int moved = std::min(need, ammo->ammo);
    weapon->ammo += moved;
    weapon->ammoPid = ammo->pid;
    ammo->ammo -= moved;
    return moved;
}

bool weaponIsTwoHanded(const Object* w)
{
    const Proto* p = itemProto(w);
    return p && (p->extFlags & PXF_TWO_HANDED) != 0;
}

bool weaponIsThrown(const Object* w, int hitMode) { return weaponAttackType(w, hitMode) == ATTACK_THROW; }

int capsCount(const Object* critter) { return g_world.inventoryCount(critter, PID_CAPS); }

void capsAdjust(Object* critter, int delta)
{
    if (!critter || delta == 0) return;
    if (delta > 0) {
        Object* caps = g_world.findInInventory(critter, PID_CAPS);
        if (caps && caps->owner == critter) {
            if (InvItem* e = g_world.invEntry(critter, caps)) e->count += delta;
        } else {
            Object* c = g_world.create(PID_CAPS);
            g_world.addToInventory(critter, c, delta);
        }
        return;
    }
    int remove = -delta;
    while (remove > 0) {
        Object* caps = g_world.findInInventory(critter, PID_CAPS);
        if (!caps || !caps->owner) break;
        InvItem* e = g_world.invEntry(caps->owner, caps);
        if (!e) break;
        int n = std::min(remove, e->count);
        Object* removed = g_world.removeFromInventory(caps->owner, caps, n);
        g_world.destroy(removed);
        remove -= n;
    }
}

int weaponReloadFromInventory(Object* critter, Object* weapon)
{
    if (!critter || !weapon) return 0;
    const Proto* wp = itemProto(weapon);
    if (!wp || wp->itemType != ITEM_WEAPON || wp->weapon.maxAmmo <= 0) return 0;
    int total = 0;
    for (int guard = 0; guard < 100 && weapon->ammo < wp->weapon.maxAmmo; guard++) {
        Object* ammo = nullptr;
        for (auto& e : critter->inventory) {
            if (e.obj != weapon && weaponCanReloadWith(weapon, e.obj)) {
                ammo = e.obj;
                break;
            }
        }
        if (!ammo) break;
        const Proto* ap = itemProto(ammo);
        if (ammo->ammo <= 0) ammo->ammo = ap ? ap->ammo.quantity : 0;
        int moved = weaponReload(weapon, ammo);
        if (moved <= 0) break;
        total += moved;
        if (ammo->ammo <= 0) {
            // box used up: the next box in the stack is full
            InvItem* e = g_world.invEntry(critter, ammo);
            if (e && e->count > 1) {
                e->count--;
                ammo->ammo = ap ? ap->ammo.quantity : 0;
            } else {
                Object* gone = g_world.removeFromInventory(critter, ammo, 1);
                if (gone) g_world.destroy(gone);
            }
        }
    }
    return total;
}

} // namespace fo
