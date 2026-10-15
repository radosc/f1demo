// Player and critter interactions with the world, modelled on the original action / object-use rules.
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
#include "audio/sound.h"

namespace fo {

namespace {

constexpr int kTalkRange = 9;
constexpr u32 DOOR_OPEN_STATE = 0x01; // in Object::dataFlags

std::vector<UiRequest> g_uiRequests;
// first aid / doctor uses: game time of the last three uses
int g_skillUses[SKILL_COUNT][3] = {};

Object* dude() { return g_world.dude; }

std::string protoMsg(int num, const char* fallback)
{
    const MessageEntry* e = messages("text/" + g_language + "/game/proto.msg").find(num);
    return e ? e->text : std::string(fallback);
}

std::string skillMsg(int num, const char* fallback)
{
    const MessageEntry* e = messages("text/" + g_language + "/game/skill.msg").find(num);
    return e ? e->text : std::string(fallback);
}

std::string fmt1(const std::string& f, const std::string& a)
{
    std::string out;
    size_t p = f.find("%s");
    if (p == std::string::npos) return f;
    return f.substr(0, p) + a + f.substr(p + 2);
}

std::string fmtD(const std::string& f, int v)
{
    size_t p = f.find("%d");
    if (p == std::string::npos) return f;
    return f.substr(0, p) + std::to_string(v) + f.substr(p + 2);
}

void say(Object* who, const std::string& text)
{
    if (who == dude() && !text.empty()) displayMessage(text);
}

int distanceBetween(Object* a, Object* b)
{
    if (!a || !b || a->tile < 0 || b->tile < 0) return 9999;
    int d = hex::distance(a->tile, b->tile);
    if (b->flags & OF_MULTIHEX) d = std::max(0, d - 1);
    if (a->flags & OF_MULTIHEX) d = std::max(0, d - 1);
    return d;
}

bool nextTo(Object* a, Object* b) { return distanceBetween(a, b) <= 1; }

std::string nameOf(Object* o)
{
    if (!o) return std::string();
    if (o->isCritter()) return critterName(o);
    if (o->isItem()) return itemName(o);
    return g_protos.name(o->pid);
}

const Proto* sceneryProto(Object* o)
{
    if (!o || o->type() != OBJ_SCENERY) return nullptr;
    return o->proto();
}

bool lineBlocked(Object* from, Object* to)
{
    std::vector<int> line = hex::line(from->tile, to->tile);
    for (size_t i = 1; i < line.size(); i++) {
        if (line[i] == to->tile) return false;
        Object* b = g_world.shootBlockingAt(from, line[i], from->elevation);
        if (b && b != to && !b->isCritter()) return true;
    }
    return false;
}

// Walk next to `target` as the first step of an action (runs when far away and outside combat).
void approach(Object* who, Object* target, bool allowRun = true)
{
    if (!who || !target || who == target || nextTo(who, target)) return;
    if (inCombat()) {
        animMoveToObject(who, target, who->critter.ap, false);
    } else {
        bool run = allowRun && who == dude() && distanceBetween(who, target) >= 5 && (!who->critter.sneaking || perkLevel(PERK_SILENT_RUNNING));
        animMoveToObject(who, target, -1, run);
    }
}

// Puts the weapon away for "magic hands" animations and takes it out again afterwards.
int putAwayWeapon(Object* who)
{
    int weapon = fidWeapon(who->fid);
    if (weapon != 0) animAnimate(who, ANIM_PUT_AWAY, -1);
    return weapon;
}

void takeOutWeapon(Object* who, int weapon)
{
    if (weapon != 0) animTakeOut(who, weapon, -1);
}

int handsAnim(Object* target)
{
    if (target->isCritter() && critterIsProne(target)) return ANIM_MAGIC_HANDS_GROUND;
    if (target->isItem() || target->isFlat()) return ANIM_MAGIC_HANDS_GROUND;
    return ANIM_MAGIC_HANDS_MIDDLE;
}

bool isNextToOrAbort(Object* who, Object* target)
{
    if (nextTo(who, target)) return true;
    if (who == dude()) displayMessage("You cannot get there.");
    return false;
}

// Aborts the rest of the sequence when the walk did not end next to the target (register_object_must_call).
void mustBeNextTo(Object* who, Object* target)
{
    u32 tu = target->uid, wu = who->uid;
    animCallback(who, [tu, wu] {
        Object* t = g_world.byUid(tu);
        Object* w = g_world.byUid(wu);
        if (t && w && !isNextToOrAbort(w, t)) animStop(w);
    });
}

// ------------------------------------------------------------------ use / pickup / loot callbacks
void doPickup(Object* who, Object* item)
{
    if (!who || !item || item->owner || item->tile < 0) return;
    if (scriptHandled(item, SP_PICKUP, who, item)) return;
    int count = 1;
    if (item->pid == PID_CAPS) count = std::max(1, item->ammo > 0 ? item->ammo : 1);
    int newWeight = inventoryWeight(who) + itemWeight(item) * count;
    if (who == dude() && newWeight > critterCarryLimit(who)) {
        displayMessage("You cannot pick that up. You are at your maximum weight capacity.");
        return;
    }
    g_world.addToInventory(who, item, count);
}

void doLoot(Object* who, Object* container, bool steal)
{
    if (!who || !container) return;
    if (who != dude()) return;
    if (container->isCritter()) {
        if (steal) {
            actionsRequestUi(UIREQ_STEAL, container);
            return;
        }
        if (!container->isDead() && !container->isKnockedOut()) {
            say(who, "You cannot do that.");
            return;
        }
        actionsRequestUi(UIREQ_LOOT, container);
        return;
    }
    if (objIsLocked(container)) {
        soundPlaySfxAt(sfxOpenName(container, 2), container);
        say(who, protoMsg(487, "It is locked."));
        return;
    }
    if (scriptHandled(container, SP_USE, who, container)) return;
    if (container->frame == 0) {
        animBegin(ANIMSEQ_NONE);
        animPlaySfx(container, sfxOpenName(container, 0), 0);
        animAnimate(container, ANIM_STAND, 0);
        animEnd();
    }
    actionsRequestUi(UIREQ_LOOT, container);
}

void doUseScenery(Object* who, Object* target)
{
    const Proto* p = sceneryProto(target);
    if (!p) return;
    if (p->sceneryType == SCEN_DOOR) {
        objUseDoor(who, target, false);
        return;
    }
    if (scriptHandled(target, SP_USE, who, target)) return;
    // stairs / ladders / elevators lead to other maps, which the demo does not have
    if (who == dude()) displayMessage(fmt1(protoMsg(480, "You see: %s."), nameOf(target)));
}

int skillUseSlot(int skill)
{
    for (int s = 0; s < 3; s++)
        if (g_skillUses[skill][s] == 0) return s;
    int hours = (gameTime() - g_skillUses[skill][0]) / kTicksPerHour;
    return hours <= 24 ? -1 : 2;
}

void skillUseAdd(int skill)
{
    int slot = skillUseSlot(skill);
    if (slot < 0) return;
    if (g_skillUses[skill][slot] != 0)
        for (int i = 0; i < slot; i++) g_skillUses[skill][i] = g_skillUses[skill][i + 1];
    g_skillUses[skill][slot] = std::max(1, gameTime());
}

int rollSkill(Object* who, int skill, int critMod, int* howMuch)
{
    int delta = critterSkill(who, skill) - g_rng.range(1, 100);
    if (howMuch) *howMuch = delta;
    bool firstDay = gameTime() < kTicksPerDay;
    int crit = critterStat(who, STAT_CRIT_CHANCE) + critMod;
    if (delta < 0) return (!firstDay && g_rng.range(1, 100) <= -delta / 10) ? 0 : 1;
    return (!firstDay && g_rng.range(1, 100) <= delta / 10 + crit) ? 3 : 2;
}

void skillExperience(Object* who, int skill, int successes)
{
    static const int kExp[SKILL_COUNT] = { 0, 0, 0, 0, 0, 0, 25, 50, 0, 25, 25, 25, 0, 0, 0, 0, 0, 100 };
    if (who != dude() || successes <= 0 || kExp[skill] == 0) return;
    int before = g_game.player.experience;
    playerAddExperience(kExp[skill] * successes);
    int gained = g_game.player.experience - before;
    if (gained > 0) displayMessage(fmtD(skillMsg(505, "You earn %d XP for honing your skills."), gained));
}

// Engine default for a skill used on a target (when no script handled it).
void skillDefault(Object* who, Object* target, int skill, int critMod)
{
    switch (skill) {
    case SKILL_FIRST_AID:
    case SKILL_DOCTOR: {
        if (skillUseSlot(skill) < 0) {
            say(who, skillMsg(590 + g_rng.range(0, 2), "You've taxed your ability with that skill. Wait a while."));
            return;
        }
        if (!target->isCritter()) return;
        if (target->isDead()) {
            say(who, skillMsg(512 + g_rng.range(0, 2), "You can't heal the dead."));
            return;
        }
        int hp = target->critter.hp, maxHp = critterStat(target, STAT_MAX_HP);
        bool crippled = (target->critter.results & DAM_CRIP) != 0;
        if (hp >= maxHp && !(skill == SKILL_DOCTOR && crippled)) {
            if (target == dude()) say(who, skillMsg(501, "You look healthy already."));
            else say(who, fmt1(skillMsg(502, "%s looks healthy already."), nameOf(target)));
            return;
        }
        int healer = perkLevel(PERK_HEALER);
        int successes = 0, attempts = 1;
        const Proto* tp = target->proto();
        bool robot = tp && tp->bodyType == 2;
        if (skill == SKILL_DOCTOR && crippled) {
            static const u32 kFlags[5] = { DAM_BLIND, DAM_CRIP_ARM_LEFT, DAM_CRIP_ARM_RIGHT, DAM_CRIP_LEG_RIGHT, DAM_CRIP_LEG_LEFT };
            for (int i = 0; i < 5; i++) {
                if (!(target->critter.results & kFlags[i])) continue;
                attempts++;
                int r = rollSkill(who, skill, critMod, nullptr);
                std::string part = skillMsg(530 + i, "limb");
                if (r >= 2) {
                    target->critter.results &= ~kFlags[i];
                    say(who, fmt1(skillMsg(target == dude() ? 520 : 521, "You heal the %s."), part));
                    successes++;
                    skillUseAdd(skill);
                } else {
                    say(who, fmt1(skillMsg(target == dude() ? 525 : 526, "You fail to heal the %s."), part));
                }
            }
        }
        int r = robot ? 1 : rollSkill(who, skill, critMod, nullptr);
        if (r >= 2 && hp < maxHp) {
            int heal = skill == SKILL_FIRST_AID ? g_rng.range(2 * healer + 1, 5 * healer + 5) : g_rng.range(2 * healer + 4, 5 * healer + 10);
            critterAdjustHp(target, heal);
            say(who, fmtD(skillMsg(500, "You heal %d hit points."), std::min(heal, maxHp - hp)));
            successes++;
            if (skill == SKILL_FIRST_AID || successes == 1) skillUseAdd(skill);
        } else if (hp < maxHp) {
            say(who, skillMsg(503, "You fail to do any healing."));
        }
        if (who == dude()) {
            g_game.time += (skill == SKILL_FIRST_AID ? 1800 : 3600 * attempts) * kTicksPerSecond;
            queueProcess();
        }
        skillExperience(who, skill, skill == SKILL_FIRST_AID ? std::min(successes, 1) : successes);
        g_scripts.mapUpdate();
        return;
    }
    case SKILL_SNEAK:
    case SKILL_LOCKPICK:
        // lock picking is scripted (door / container scripts); without a script nothing happens
        if (skill == SKILL_LOCKPICK && objIsLocked(target)) say(who, skillMsg(550, "You fail to pick the lock."));
        return;
    case SKILL_STEAL:
        doLoot(who, target, true);
        return;
    case SKILL_TRAPS: say(who, skillMsg(551, "You fail to find any traps.")); return;
    case SKILL_SCIENCE: say(who, skillMsg(552, "You fail to learn anything.")); return;
    case SKILL_REPAIR: say(who, skillMsg(553, "You cannot repair that.")); return;
    default: return;
    }
}

void doUseSkillOn(Object* who, Object* target, int skill)
{
    if (objIsJammed(target)) {
        say(who, gameMsg("misc", 2001).empty() ? "The lock is jammed." : gameMsg("misc", 2001));
        return;
    }
    if (scriptHandled(target, SP_USE_SKILL_ON, who, nullptr, -9999, skill)) return;
    skillDefault(who, target, skill, 0);
}

// Default "use item on" when no script handled it.
int defaultUseItemOn(Object* who, Object* target, Object* item)
{
    int type = itemType(item);
    if (type == ITEM_DRUG) {
        if (!target->isCritter()) {
            say(who, protoMsg(582, "That does nothing."));
            return -1;
        }
        if (target->isDead()) {
            say(who, protoMsg(583 + g_rng.range(0, 3), "That won't work on the dead."));
            return -1;
        }
        bool ok = drugTake(target, item);
        if (who == dude() && target != dude()) {
            std::string f = protoMsg(581, "You use the %s on %s.");
            size_t p = f.find("%s");
            if (p != std::string::npos) f = f.substr(0, p) + nameOf(item) + f.substr(p + 2);
            displayMessage(fmt1(f, nameOf(target)));
        }
        return ok ? 1 : -1;
    }
    say(who, protoMsg(582, "That does nothing."));
    return -1;
}

void doUseItemOn(Object* who, Object* target, Object* item)
{
    if (!item || !target) return;
    // first aid kits and doctor's bags work through the skills
    int skill = -1;
    std::string n = strLower(g_protos.name(item->pid));
    if (n.find("first aid") != std::string::npos) skill = SKILL_FIRST_AID;
    else if (n.find("doctor") != std::string::npos) skill = SKILL_DOCTOR;
    int rc;
    if (skill >= 0) {
        if (inCombat()) {
            say(who, protoMsg(902, "You cannot do that in combat."));
            return;
        }
        skillDefault(who, target, skill, 20);
        rc = g_rng.range(1, 10) == 1 ? 1 : 0;
        if (rc == 1) say(who, protoMsg(skill == SKILL_DOCTOR ? 900 : 901, "The supplies run out."));
    } else {
        bool handled = false;
        if (item->script) handled = scriptHandled(item, SP_USE_OBJ_ON, who, target);
        if (!handled && target->script) handled = scriptHandled(target, SP_USE_OBJ_ON, who, item);
        rc = handled ? 0 : defaultUseItemOn(who, target, item);
    }
    if (rc == 1) {
        // the item was used up
        if (Object* owner = item->owner) {
            Object* used = g_world.removeFromInventory(owner, item, 1);
            if (used) g_world.destroy(used);
            if (owner->isCritter()) critterUpdateArt(owner);
        } else {
            g_world.destroy(item);
        }
    }
    g_scripts.mapUpdate();
}

} // namespace

// ------------------------------------------------------------------ public API
bool scriptHandled(Object* target, int proc, Object* source, Object* targetObj, int param, int actionBeingUsed)
{
    if (!target || !target->script) return false;
    return g_scripts.exec(target->script, proc, source, targetObj ? targetObj : target, param, actionBeingUsed);
}

void actionsRequestUi(int kind, Object* target)
{
    UiRequest r;
    r.kind = kind;
    r.target = target ? target->uid : 0;
    g_uiRequests.push_back(r);
}

bool actionsPollUi(UiRequest& out)
{
    if (g_uiRequests.empty()) return false;
    out = g_uiRequests.front();
    g_uiRequests.erase(g_uiRequests.begin());
    return true;
}

bool canTalkTo(Object* target) { return target && target->isCritter() && critterIsActive(target); }

int defaultActionFor(Object* target)
{
    if (!target) return ACT_NONE;
    switch (target->type()) {
    case OBJ_ITEM: return ACT_USE;
    case OBJ_CRITTER:
        if (target == dude()) return ACT_ROTATE;
        if (canTalkTo(target)) return inCombat() ? ACT_LOOK : ACT_TALK;
        return ACT_USE;
    case OBJ_SCENERY: {
        const Proto* p = target->proto();
        return p && (p->extFlags & PXF_CAN_USE) ? ACT_USE : ACT_LOOK;
    }
    case OBJ_WALL: return ACT_LOOK;
    default: return ACT_NONE;
    }
}

std::vector<int> actionMenuFor(Object* target)
{
    std::vector<int> v;
    if (!target) return v;
    const Proto* p = target->proto();
    switch (target->type()) {
    case OBJ_ITEM:
        v.push_back(ACT_USE);
        v.push_back(ACT_LOOK);
        if (objIsContainer(target)) {
            v.push_back(ACT_INVENTORY);
            v.push_back(ACT_SKILL);
        }
        break;
    case OBJ_CRITTER:
        if (target == dude()) {
            v.push_back(ACT_ROTATE);
        } else if (canTalkTo(target)) {
            if (!inCombat()) v.push_back(ACT_TALK);
        } else {
            v.push_back(ACT_USE);
        }
        v.push_back(ACT_LOOK);
        v.push_back(ACT_INVENTORY);
        v.push_back(ACT_SKILL);
        break;
    case OBJ_SCENERY:
        if (p && (p->extFlags & PXF_CAN_USE)) v.push_back(ACT_USE);
        v.push_back(ACT_LOOK);
        v.push_back(ACT_INVENTORY);
        v.push_back(ACT_SKILL);
        break;
    case OBJ_WALL:
        v.push_back(ACT_LOOK);
        if (p && (p->extFlags & PXF_CAN_USE)) v.push_back(ACT_INVENTORY);
        break;
    default: break;
    }
    v.push_back(ACT_CANCEL);
    return v;
}

void actionLook(Object* who, Object* target, bool detailed)
{
    if (!target || who != dude() || target->type() == OBJ_TILE) return;
    if (detailed) {
        // examine: description_p_proc, else the proto description
        if (!scriptHandled(target, SP_DESCRIPTION, who, target)) {
            std::string d = g_protos.description(target->pid);
            if (d.empty() || d == "<None>") d = protoMsg(493, "You see nothing out of the ordinary.");
            if (!(target->isCritter() && target->isDead())) displayMessage(d);
        }
        if (target->isCritter()) {
            int hp = target->critter.hp, maxHp = std::max(1, critterStat(target, STAT_MAX_HP));
            int level = target->isDead() ? 500 : hp < maxHp / 4 ? 501 : hp < maxHp / 2 ? 502 : hp < maxHp ? 503 : 504;
            std::string state = protoMsg(level, "");
            if (target == dude()) {
                displayMessage(fmt1(protoMsg(520, "You look %s."), state));
                displayMessage(strFormat("%s has %d/%d hps.", nameOf(target).c_str(), hp, maxHp));
            } else if (!target->isDead()) {
                int g = critterGender(target);
                std::string pron = protoMsg(g == GENDER_FEMALE ? 523 : 522, g == GENDER_FEMALE ? "She" : "He");
                std::string f = protoMsg(521, "%s looks %s.");
                size_t p = f.find("%s");
                if (p != std::string::npos) f = f.substr(0, p) + pron + f.substr(p + 2);
                displayMessage(fmt1(f, state));
                if (perkLevel(PERK_AWARENESS)) {
                    // Awareness: exact hit points and the weapon in hand
                    const Proto* tp = target->proto();
                    std::string it = (!tp || tp->bodyType == 0) ? pron : protoMsg(524, "It");
                    displayMessage(strFormat("%s has %d/%d hps.", it.c_str(), hp, maxHp));
                    Object* w = itemRightHand(target);
                    if (!w || itemType(w) != ITEM_WEAPON) w = itemLeftHand(target);
                    if (w && itemType(w) == ITEM_WEAPON) displayMessage(strFormat("%s is holding %s.", it.c_str(), g_protos.name(w->pid).c_str()));
                }
            }
        } else if (target->isItem() && itemType(target) == ITEM_WEAPON && weaponUsesAmmo(target)) {
            const Proto* p = target->proto();
            std::string ammoName = target->ammoPid >= 0 ? g_protos.name(target->ammoPid) : std::string("ammo");
            displayMessage(strFormat("It has %d/%d shots of %s.", target->ammo, p ? p->weapon.maxAmmo : 0, ammoName.c_str()));
        }
        return;
    }
    if (scriptHandled(target, SP_LOOK_AT, who, target)) return;
    int num = (target->isCritter() && target->isDead()) ? 491 + g_rng.range(0, 1) : 490;
    displayMessage(fmt1(protoMsg(num, "You see: %s."), nameOf(target)));
}

void actionTalk(Object* who, Object* target)
{
    if (!who || !target || who != dude() || !target->isCritter() || target == who) return;
    if (!canTalkTo(target)) {
        actionLook(who, target, false);
        return;
    }
    int anim = fidAnim(who->fid);
    if (anim == ANIM_WALK || anim == ANIM_RUNNING) animStop(who);
    animBegin(ANIMSEQ_REPLACE);
    if (inCombat()) {
        animMoveToObject(who, target, who->critter.ap, false);
    } else if (distanceBetween(who, target) >= kTalkRange || lineBlocked(who, target)) {
        animMoveToObject(who, target, -1, true);
    }
    u32 tu = target->uid, wu = who->uid;
    animCallback(who, [tu, wu] {
        Object* t = g_world.byUid(tu);
        Object* w = g_world.byUid(wu);
        if (!t || !w || distanceBetween(w, t) > kTalkRange || lineBlocked(w, t)) return;
        gameRequestDialog(t);
    });
    animEnd();
}

void actionUse(Object* who, Object* target)
{
    if (!who || !target) return;
    if (target->isItem()) {
        actionPickup(who, target);
        return;
    }
    if (target->isCritter()) {
        if (target == who) return;
        // using a critter means looting it (dead or unconscious)
        animBegin(ANIMSEQ_REPLACE);
        approach(who, target);
        mustBeNextTo(who, target);
        u32 tu = target->uid, wu = who->uid;
        int weapon = putAwayWeapon(who);
        animAnimate(who, ANIM_MAGIC_HANDS_GROUND, -1);
        animCallback(who, [tu, wu] {
            Object* t = g_world.byUid(tu);
            Object* w = g_world.byUid(wu);
            if (!t || !w || !isNextToOrAbort(w, t)) return;
            if (!combatUseAp(w, 3)) return;
            doLoot(w, t, false);
        });
        takeOutWeapon(who, weapon);
        animEnd();
        return;
    }
    const Proto* p = sceneryProto(target);
    if (!p) {
        actionLook(who, target, false);
        return;
    }
    animBegin(ANIMSEQ_REPLACE);
    approach(who, target);
    mustBeNextTo(who, target);
    u32 tu = target->uid, wu = who->uid;
    int weapon = putAwayWeapon(who);
    if (p->sceneryType != SCEN_STAIRS) animAnimate(who, handsAnim(target), -1);
    animCallback(who, [tu, wu] {
        Object* t = g_world.byUid(tu);
        Object* w = g_world.byUid(wu);
        if (!t || !w || !nextTo(w, t)) return;
        if (!combatUseAp(w, 3)) return;
        doUseScenery(w, t);
        g_scripts.mapUpdate();
    });
    takeOutWeapon(who, weapon);
    animEnd();
}

void actionPickup(Object* who, Object* item)
{
    if (!who || !item || !item->isItem() || item->owner) return;
    if (objIsContainer(item)) {
        // containers are searched instead of picked up
        animBegin(ANIMSEQ_REPLACE);
        approach(who, item);
        mustBeNextTo(who, item);
        int weapon = putAwayWeapon(who);
        animAnimate(who, handsAnim(item), -1);
        u32 iu = item->uid, wu = who->uid;
        animCallback(who, [iu, wu] {
            Object* i = g_world.byUid(iu);
            Object* w = g_world.byUid(wu);
            if (!i || !w || !isNextToOrAbort(w, i)) return;
            if (!combatUseAp(w, 3)) return;
            doLoot(w, i, false);
        });
        takeOutWeapon(who, weapon);
        animEnd();
        return;
    }
    animBegin(ANIMSEQ_REPLACE);
    approach(who, item);
    mustBeNextTo(who, item);
    int weapon = putAwayWeapon(who);
    animAnimate(who, handsAnim(item), -1);
    u32 iu = item->uid, wu = who->uid;
    animCallback(who, [iu, wu] {
        Object* i = g_world.byUid(iu);
        Object* w = g_world.byUid(wu);
        if (!i || !w || !nextTo(w, i)) return;
        if (!combatUseAp(w, 3)) return;
        doPickup(w, i);
    });
    takeOutWeapon(who, weapon);
    animEnd();
}

void actionDrop(Object* who, Object* item, int count)
{
    if (!who || !item || item->owner != who || who->tile < 0) return;
    if (scriptHandled(item, SP_DROP, who, item)) return;
    InvItem* e = g_world.invEntry(who, item);
    if (!e) return;
    bool equipped = (item->flags & OF_EQUIPPED) != 0;
    Object* dropped = g_world.removeFromInventory(who, item, std::min(count, e->count));
    if (!dropped) return;
    if (dropped->pid == PID_CAPS) dropped->ammo = std::min(count, std::max(1, count));
    g_world.place(dropped, who->tile, who->elevation);
    if (equipped && who->isCritter()) critterUpdateArt(who);
}

void actionUseItemOn(Object* who, Object* item, Object* target)
{
    if (!who || !item || !target) return;
    if (target == who) {
        // using an item on yourself happens immediately
        if (!combatUseAp(who, 2)) return;
        doUseItemOn(who, target, item);
        return;
    }
    animBegin(ANIMSEQ_REPLACE);
    approach(who, target);
    mustBeNextTo(who, target);
    int weapon = putAwayWeapon(who);
    animAnimate(who, handsAnim(target), -1);
    u32 iu = item->uid, tu = target->uid, wu = who->uid;
    animCallback(who, [iu, tu, wu] {
        Object* i = g_world.byUid(iu);
        Object* t = g_world.byUid(tu);
        Object* w = g_world.byUid(wu);
        if (!i || !t || !w || !isNextToOrAbort(w, t)) return;
        if (!combatUseAp(w, 2)) return;
        doUseItemOn(w, t, i);
    });
    takeOutWeapon(who, weapon);
    animEnd();
}

void actionUseSkill(Object* who, int skill, Object* target)
{
    if (!who || skill < 0 || skill >= SKILL_COUNT) return;
    if (skill == SKILL_SNEAK) {
        who->critter.sneaking = !who->critter.sneaking;
        if (who == dude()) displayMessage(who->critter.sneaking ? "You are now sneaking." : "You stop sneaking.");
        return;
    }
    if (!target) return;
    switch (skill) {
    case SKILL_FIRST_AID:
    case SKILL_DOCTOR:
        if (inCombat()) {
            say(who, protoMsg(902, "You cannot do that in combat."));
            return;
        }
        if (!target->isCritter()) return;
        break;
    case SKILL_LOCKPICK:
        if (inCombat()) {
            say(who, protoMsg(902, "You cannot do that in combat."));
            return;
        }
        if (!target->isItem() && !target->isScenery()) return;
        break;
    case SKILL_STEAL:
        if (inCombat()) {
            say(who, protoMsg(902, "You cannot do that in combat."));
            return;
        }
        if (!target->isItem() && !target->isCritter()) return;
        if (target == who) {
            say(who, "You cannot steal from yourself.");
            return;
        }
        break;
    case SKILL_TRAPS:
        if (target->isCritter()) {
            say(who, "You cannot use that skill on a critter.");
            return;
        }
        break;
    case SKILL_SCIENCE:
    case SKILL_REPAIR:
        if (target->isCritter()) {
            say(who, "You cannot use that skill on a critter.");
            return;
        }
        break;
    default: break;
    }
    if (target == who) {
        doUseSkillOn(who, target, skill);
        return;
    }
    animBegin(ANIMSEQ_REPLACE);
    approach(who, target);
    mustBeNextTo(who, target);
    int weapon = putAwayWeapon(who);
    animAnimate(who, handsAnim(target), -1);
    u32 tu = target->uid, wu = who->uid;
    animCallback(who, [tu, wu, skill] {
        Object* t = g_world.byUid(tu);
        Object* w = g_world.byUid(wu);
        if (!t || !w || !isNextToOrAbort(w, t)) return;
        doUseSkillOn(w, t, skill);
    });
    takeOutWeapon(who, weapon);
    animEnd();
}

bool actionUseItem(Object* who, Object* item)
{
    if (!who || !item) return false;
    if (scriptHandled(item, SP_USE, who, item)) return false;
    if (itemType(item) == ITEM_DRUG) {
        if (!drugTake(who, item)) return false;
        Object* used = g_world.removeFromInventory(item->owner ? item->owner : who, item, 1);
        if (used) g_world.destroy(used);
        return true;
    }
    say(who, protoMsg(582, "That does nothing."));
    return false;
}

void actionMoveTo(Object* who, int tile, bool run)
{
    if (!who || !hex::valid(tile) || who->tile < 0) return;
    if (run && who->critter.sneaking && !(who == dude() && perkLevel(PERK_SILENT_RUNNING))) run = false;
    animBegin(ANIMSEQ_REPLACE);
    animMoveToTile(who, tile, who->elevation, inCombat() ? who->critter.ap + combatFreeMove() : -1, run);
    animEnd();
}

void actionRotate(Object* who)
{
    if (!who || !who->isCritter() || animBusy(who) || critterIsProne(who)) return;
    critterStand(who, (who->rotation + 1) % 6);
}

// ------------------------------------------------------------------ doors & containers
bool objIsDoor(Object* o)
{
    const Proto* p = sceneryProto(o);
    return p && p->sceneryType == SCEN_DOOR;
}

bool objIsContainer(Object* o)
{
    const Proto* p = o && o->isItem() ? o->proto() : nullptr;
    return p && p->itemType == ITEM_CONTAINER;
}

bool objIsOpen(Object* o)
{
    if (!o) return false;
    if (objIsDoor(o)) return (o->dataFlags & DOOR_OPEN_STATE) != 0;
    return o->frame != 0;
}

bool objIsLocked(Object* o) { return o && (objIsDoor(o) || objIsContainer(o)) && (o->dataFlags & LOCK_FLAG_LOCKED); }
bool objIsJammed(Object* o) { return o && (o->dataFlags & LOCK_FLAG_JAMMED); }

void objSetLocked(Object* o, bool locked)
{
    if (!o) return;
    if (locked) o->dataFlags |= LOCK_FLAG_LOCKED;
    else o->dataFlags &= ~LOCK_FLAG_LOCKED;
}

static void doorSettle(Object* door)
{
    Sprite* s = g_art.sprite(door->fid);
    int count = s ? std::max(1, s->meta.framesPerDir) : 1;
    if (door->dataFlags & DOOR_OPEN_STATE) {
        door->flags |= OF_OPEN_DOOR;
        door->frame = count - 1;
    } else {
        door->flags &= ~OF_OPEN_DOOR;
        door->frame = 0;
    }
    door->offX = door->offY = 0;
}

void objUseDoor(Object* user, Object* door, bool passThrough)
{
    if (!door) return;
    if (objIsLocked(door)) soundPlaySfxAt(sfxOpenName(door, 2), door);
    if (user && scriptHandled(door, SP_USE, user, door)) return;
    if (objIsLocked(door)) {
        if (user == dude()) displayMessage(protoMsg(487, "It is locked."));
        return;
    }
    u32 du = door->uid;
    bool open = (door->dataFlags & DOOR_OPEN_STATE) != 0;
    if (open && !passThrough) {
        // closing: something standing in the doorway keeps it open
        for (Object* o : g_world.at(door->tile, door->elevation)) {
            if (o != door && !(o->flags & OF_HIDDEN) && (o->isCritter() || o->isItem())) {
                if (user == dude()) displayMessage("Something is blocking the door.");
                return;
            }
        }
    }
    animBegin(ANIMSEQ_NONE | ANIMSEQ_NO_STAND);
    if (!open) {
        if (!passThrough) {
            animCallback(door, [du] {
                if (Object* d = g_world.byUid(du)) d->dataFlags |= DOOR_OPEN_STATE;
            });
        }
        // passable as soon as it starts opening
        animSetFlag(door, OF_OPEN_DOOR, 0);
        animPlaySfx(door, sfxOpenName(door, 0), -1);
        animAnimate(door, ANIM_STAND, -1);
        if (passThrough) {
            animPlaySfx(door, sfxOpenName(door, 1), -1);
            animAnimateReverse(door, ANIM_STAND, -1);
        }
    } else {
        animCallback(door, [du] {
            if (Object* d = g_world.byUid(du)) d->dataFlags &= ~DOOR_OPEN_STATE;
        });
        animPlaySfx(door, sfxOpenName(door, 1), -1);
        animAnimateReverse(door, ANIM_STAND, -1);
    }
    animCallback(door, [du] {
        if (Object* d = g_world.byUid(du)) doorSettle(d);
    }, -1, true);
    animEnd();
}

void objOpen(Object* o)
{
    if (!o) return;
    if (objIsDoor(o)) {
        if (!(o->dataFlags & DOOR_OPEN_STATE)) objUseDoor(nullptr, o, false);
        return;
    }
    if (o->frame == 0) {
        animBegin(ANIMSEQ_NONE);
        animAnimate(o, ANIM_STAND, 0);
        animEnd();
    }
}

void objClose(Object* o)
{
    if (!o) return;
    if (objIsDoor(o)) {
        if (o->dataFlags & DOOR_OPEN_STATE) objUseDoor(nullptr, o, false);
        return;
    }
    if (o->frame != 0) {
        animBegin(ANIMSEQ_NONE);
        animAnimateReverse(o, ANIM_STAND, 0);
        animEnd();
    }
}

std::string sfxOpenName(Object* o, int action)
{
    // The demo ships a handful of door/container sounds; mods can add more with the release naming
    // scheme (S<action>DOORS<id> / I<action>CNTNR<id>), which is tried first.
    static const char kAction[5] = { 'O', 'C', 'L', 'N', 'U' };
    if (!o || action < 0 || action > 4) return std::string();
    std::string release;
    const Proto* p = o->proto();
    char id = p && p->soundId ? (char)p->soundId : 'A';
    if (o->type() == OBJ_SCENERY) release = strFormat("S%cDOORS%c", kAction[action], id);
    else release = strFormat("I%cCNTNR%c", kAction[action], id);
    if (soundExists(release)) return release;
    std::string art = strLower(g_art.name(fidType(o->fid), fidIndex(o->fid)));
    bool fridge = art.find("fridg") != std::string::npos || art.find("refr") != std::string::npos;
    if (action == 0) return fridge ? "srefopx1" : "scwdoxx1";
    if (action == 1) return fridge ? "srefclx1" : "scwdcxx1";
    return std::string();
}

// ------------------------------------------------------------------ drugs
static void drugApplyStats(Object* c, const int* stats, const int* amounts)
{
    for (int i = 0; i < 3; i++) {
        int stat = stats[i], amount = amounts[i];
        if (stat < 0 || amount == 0) continue;
        switch (stat) {
        case STAT_CURRENT_HP: critterAdjustHp(c, amount); break;
        case STAT_CURRENT_POISON: c->critter.poison = std::max(0, c->critter.poison + amount); break;
        case STAT_CURRENT_RAD: c->critter.radiation = std::max(0, c->critter.radiation + amount); break;
        case STAT_AGE:
        case STAT_GENDER: break; // not meaningful for drugs (beta data uses these slots inconsistently)
        default:
            if (stat < STAT_SAVEABLE_COUNT) critterSetBonusStat(c, stat, critterBonusStat(c, stat) + amount);
            break;
        }
    }
}

bool drugTake(Object* critter, Object* item)
{
    if (!critter || !item || !critter->isCritter() || critter->isDead()) return false;
    const Proto* p = item->proto();
    if (!p || p->itemType != ITEM_DRUG) return false;
    drugApplyStats(critter, p->drug.stat, p->drug.amount);
    if (p->drug.duration1 > 0) queueAdd(p->drug.duration1 * kTicksPerMinute, critter, EV_DRUG, item->pid, 1);
    if (p->drug.duration2 > 0) queueAdd(p->drug.duration2 * kTicksPerMinute, critter, EV_DRUG, item->pid, 2);
    return true;
}

void drugProcessEvent(const GameEvent& e)
{
    Object* c = g_world.byUid(e.obj);
    const Proto* p = g_protos.get(e.data[0]);
    if (!c || !p || c->isDead()) return;
    if (e.type == EV_DRUG) drugApplyStats(c, p->drug.stat, e.data[1] == 1 ? p->drug.amount1 : p->drug.amount2);
}

} // namespace fo
