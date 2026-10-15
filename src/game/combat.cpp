// Turn based combat: sequence, action points, to-hit, damage, criticals, reactions and the combat AI.
// Rules follow the original game; tables that modders may want to change live in data (critical hits in
// data/rules/critical.json, AI packets in data/data/ai.txt, taunts in combatai.msg).
#include "game/combat.h"
#include "game/actions.h"
#include "game/anim.h"
#include "game/art.h"
#include "game/critter.h"
#include "game/dialog.h"
#include "game/game.h"
#include "game/hex.h"
#include "game/item.h"
#include "game/msg.h"
#include "game/path.h"
#include "game/proto.h"
#include "game/script.h"
#include "game/worldview.h"
#include "audio/sound.h"
#include "core/fs.h"
#include "core/json.h"
#include "gfx/palette.h"

namespace fo {

extern bool g_combatLog;

namespace {

constexpr u32 PID_IMMORTAL = 0x01000008; // never critically hit, never damaged (original hard-coded critter)

// critter maneuver bits
enum : int { MAN_ENGAGING = 0x01, MAN_DISENGAGING = 0x02, MAN_FLEEING = 0x04 };

// attack subtypes
enum AttackKind { AK_NONE, AK_UNARMED, AK_MELEE, AK_THROW, AK_RANGED };

enum Roll { ROLL_CRIT_FAIL = 0, ROLL_FAIL = 1, ROLL_SUCCESS = 2, ROLL_CRIT_SUCCESS = 3 };

enum BadShot { BS_OK, BS_NO_AMMO, BS_OUT_OF_RANGE, BS_NOT_ENOUGH_AP, BS_DEAD, BS_AIM_BLOCKED, BS_ARM_CRIPPLED, BS_BOTH_ARMS_CRIPPLED };

int hitLocationPenalty[HIT_LOCATION_COUNT] = { -40, -30, -30, 0, -20, -20, -60, -30, 0 };

// ------------------------------------------------------------------ data: AI packets
struct AiPacket {
    std::string name;
    int maxDist = 15, minToHit = 1, minHp = 0, aggression = 50;
    u32 hurtTooMuch = 0;
    int secondaryFreq = 1, calledFreq = 1;
    int font = 101, color = 0, outlineColor = 0, chance = 0;
    int runStart = 0, moveStart = 0, attackStart = 0, missStart = 0;
    int hitStart[8] = {};
    int lastMsg = 0;
};
std::map<int, AiPacket> g_packets;
bool g_packetsLoaded = false;

void loadPackets()
{
    if (g_packetsLoaded) return;
    g_packetsLoaded = true;
    std::string text;
    if (!g_vfs.readText("data/ai.txt", text)) return;
    AiPacket cur;
    int num = -1;
    auto flush = [&] {
        if (num >= 0) g_packets[num] = cur;
        cur = AiPacket();
        num = -1;
    };
    for (auto raw : strSplit(text, '\n')) {
        std::string line = raw;
        size_t sc = line.find(';');
        if (sc != std::string::npos) line = line.substr(0, sc);
        line = strTrim(line);
        if (line.empty()) continue;
        if (line[0] == '[') {
            flush();
            cur.name = line.substr(1, line.find(']') - 1);
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = strLower(strTrim(line.substr(0, eq)));
        std::string val = strTrim(line.substr(eq + 1));
        int v = atoi(val.c_str());
        if (key == "packet_num") num = v;
        else if (key == "max_dist") cur.maxDist = v;
        else if (key == "min_to_hit") cur.minToHit = v;
        else if (key == "min_hp") cur.minHp = v;
        else if (key == "aggression") cur.aggression = v;
        else if (key == "secondary_freq") cur.secondaryFreq = std::max(1, v);
        else if (key == "called_freq") cur.calledFreq = std::max(1, v);
        else if (key == "font") cur.font = v;
        else if (key == "color") cur.color = v;
        else if (key == "outline_color") cur.outlineColor = v;
        else if (key == "chance") cur.chance = v;
        else if (key == "run_start") cur.runStart = v;
        else if (key == "move_start") cur.moveStart = v;
        else if (key == "attack_start") cur.attackStart = v;
        else if (key == "miss_start") cur.missStart = v;
        else if (key == "hit_head_start") cur.hitStart[HIT_HEAD] = v;
        else if (key == "hit_left_arm_start") cur.hitStart[HIT_LEFT_ARM] = v;
        else if (key == "hit_right_arm_start") cur.hitStart[HIT_RIGHT_ARM] = v;
        else if (key == "hit_torso_start") cur.hitStart[HIT_TORSO] = v;
        else if (key == "hit_right_leg_start") cur.hitStart[HIT_RIGHT_LEG] = v;
        else if (key == "hit_left_leg_start") cur.hitStart[HIT_LEFT_LEG] = v;
        else if (key == "hit_eyes_start") cur.hitStart[HIT_EYES] = v;
        else if (key == "hit_groin_start") cur.hitStart[HIT_GROIN] = v;
        else if (key == "last_msg") cur.lastMsg = v;
        else if (key == "hurt_too_much") {
            for (auto part : strSplit(val, ',')) {
                std::string p = strLower(strTrim(part));
                if (p == "blind") cur.hurtTooMuch |= DAM_BLIND;
                else if (p == "crippled") cur.hurtTooMuch |= DAM_CRIP_LEG_ANY | DAM_CRIP_ARM_ANY;
                else if (p == "crippled_legs") cur.hurtTooMuch |= DAM_CRIP_LEG_ANY;
                else if (p == "crippled_arms") cur.hurtTooMuch |= DAM_CRIP_ARM_ANY;
            }
        }
    }
    flush();
    LOGI("Loaded %d AI packets", (int)g_packets.size());
}

const AiPacket& packetOf(Object* c)
{
    loadPackets();
    static AiPacket fallback;
    auto it = g_packets.find(c ? c->critter.aiPacket : 0);
    return it != g_packets.end() ? it->second : fallback;
}

// ------------------------------------------------------------------ data: critical hits
struct CritEffect {
    int multiplier = 2;
    u32 flags = 0;
    int checkStat = -1, checkMod = 0;
    u32 checkFlags = 0;
    int message = 0, checkMessage = 0;
};
struct CritTables {
    bool loaded = false;
    std::vector<std::array<std::array<CritEffect, 6>, HIT_LOCATION_COUNT>> kill; // [killType][loc][level]
    std::array<std::array<CritEffect, 6>, HIT_LOCATION_COUNT> player;
} g_crit;

void loadCritTables()
{
    if (g_crit.loaded) return;
    g_crit.loaded = true;
    std::string text;
    if (!g_vfs.readText("rules/critical.json", text)) {
        LOGW("rules/critical.json missing - critical hits only double damage");
        return;
    }
    Json j = Json::parse(text);
    std::map<std::string, u32> flagBits;
    for (auto& m : j["flagNames"].members()) flagBits[m.first] = (u32)m.second.i();
    std::vector<std::string> statNames;
    for (auto& s : j["statNames"].items()) statNames.push_back(s.str());
    std::vector<std::string> locs;
    for (auto& s : j["hitLocations"].items()) locs.push_back(s.str());
    for (int l = 0; l < (int)locs.size() && l < HIT_LOCATION_COUNT; l++) hitLocationPenalty[l] = j["hitLocationPenalty"][locs[l]].toInt(hitLocationPenalty[l]);
    auto flagsOf = [&](const Json& arr) {
        u32 f = 0;
        for (auto& n : arr.items()) f |= flagBits[n.str()];
        return f;
    };
    auto readTable = [&](const Json& t, std::array<std::array<CritEffect, 6>, HIT_LOCATION_COUNT>& out) {
        for (int l = 0; l < HIT_LOCATION_COUNT && l < (int)locs.size(); l++) {
            const Json& levels = t[locs[l]];
            for (int lv = 0; lv < 6 && lv < (int)levels.size(); lv++) {
                const Json& e = levels[lv];
                CritEffect c;
                c.multiplier = e["damageMultiplier"].toInt(2);
                c.flags = flagsOf(e["flags"]);
                std::string st = e["check"]["stat"].str("");
                for (int s = 0; s < (int)statNames.size(); s++)
                    if (statNames[s] == st) c.checkStat = s;
                c.checkMod = e["check"]["modifier"].toInt(0);
                c.checkFlags = flagsOf(e["check"]["flags"]);
                c.message = e["message"].toInt(0);
                c.checkMessage = e["checkFailMessage"].toInt(0);
                out[l][lv] = c;
            }
        }
    };
    for (auto& kt : j["killTypes"].items()) {
        std::array<std::array<CritEffect, 6>, HIT_LOCATION_COUNT> table;
        readTable(j["tables"][kt.str()], table);
        g_crit.kill.push_back(table);
    }
    readTable(j["player"], g_crit.player);
}

// critical failure table [weapon critical failure type][effect]
const u32 kCritFail[7][5] = {
    { 0, DAM_LOSE_TURN, DAM_LOSE_TURN, DAM_HURT_SELF | DAM_KNOCKED_DOWN, DAM_CRIP_RANDOM },
    { 0, DAM_LOSE_TURN, DAM_DROP, DAM_RANDOM_HIT, DAM_HIT_SELF },
    { 0, DAM_LOSE_AMMO, DAM_DROP, DAM_RANDOM_HIT, DAM_DESTROY },
    { DAM_LOSE_TURN, DAM_LOSE_TURN | DAM_LOSE_AMMO, DAM_DROP | DAM_LOSE_TURN, DAM_RANDOM_HIT, DAM_EXPLODE | DAM_LOSE_TURN },
    { DAM_DUD, DAM_DROP, DAM_DROP | DAM_HURT_SELF, DAM_RANDOM_HIT, DAM_EXPLODE },
    { DAM_LOSE_TURN, DAM_DUD, DAM_DESTROY, DAM_RANDOM_HIT, DAM_EXPLODE | DAM_LOSE_TURN | DAM_KNOCKED_DOWN },
    { 0, DAM_LOSE_TURN, DAM_RANDOM_HIT, DAM_DESTROY, DAM_EXPLODE | DAM_LOSE_TURN | DAM_ON_FIRE },
};

// ------------------------------------------------------------------ combat state
struct ScriptedAttack {
    bool valid = false;
    u32 attacker = 0, target = 0;
    int calledShot = 0, numAttacks = 1, bonus = 0, minDmg = 0, maxDmg = 0x7FFFFFFF;
    int attackerResults = 0, targetResults = 0;
};

enum Phase { PH_NONE, PH_START_TURN, PH_PLAYER, PH_AI, PH_END_TURN };

struct CombatState {
    bool active = false;
    std::vector<u32> list;   // critters on the combat elevation; the first `com` take part
    int com = 0;
    int index = 0;
    u32 turn = 0;
    Phase phase = PH_NONE;
    int elevation = 0;
    int freeMove = 0;
    int aiAttempts = 0;
    bool aiTaunted = false;
    int exps = 0;
    bool playerEnd = false;
    bool endRequested = false;
    float settle = 0;        // short pause between turns
    ScriptedAttack pending;  // requested by scripts, starts once no script/dialogue runs
    ScriptedAttack gcsd;     // parameters of the scripted attack for the attacker's first turn
} C;

Object* objOf(u32 uid) { return g_world.byUid(uid); }
Object* dude() { return g_world.dude; }

std::string combatText(int num, const char* fb)
{
    std::string s = gameMsg("combat", num);
    return (s.empty() || s == "Error") ? std::string(fb) : s;
}

// ------------------------------------------------------------------ rules helpers
Object* hitWith(Object* critter, int hitMode)
{
    switch (hitMode) {
    case HIT_LEFT_PRIMARY:
    case HIT_LEFT_SECONDARY: return itemLeftHand(critter);
    case HIT_RIGHT_PRIMARY:
    case HIT_RIGHT_SECONDARY: return itemRightHand(critter);
    default: return nullptr;
    }
}

int attackKindOf(int attackType)
{
    switch (attackType) {
    case ATTACK_PUNCH:
    case ATTACK_KICK: return AK_UNARMED;
    case ATTACK_SWING:
    case ATTACK_THRUST: return AK_MELEE;
    case ATTACK_THROW: return AK_THROW;
    case ATTACK_FIRE_SINGLE:
    case ATTACK_FIRE_BURST:
    case ATTACK_FLAME: return AK_RANGED;
    default: return AK_NONE;
    }
}

int attackKind(Object* critter, int hitMode) { return attackKindOf(weaponAttackType(hitWith(critter, hitMode), hitMode)); }

int attackAnimOf(Object* critter, int hitMode)
{
    if (hitMode == HIT_KICK) return ANIM_KICK_LEG;
    Object* w = hitWith(critter, hitMode);
    if (!w) return ANIM_THROW_PUNCH;
    static const int kAnim[ATTACK_TYPE_COUNT] = { ANIM_STAND, ANIM_THROW_PUNCH, ANIM_KICK_LEG, ANIM_SWING, ANIM_THRUST, ANIM_THROW_ANIM,
        ANIM_FIRE_SINGLE, ANIM_FIRE_BURST, ANIM_FIRE_CONTINUOUS };
    return kAnim[clampv(weaponAttackType(w, hitMode), 0, ATTACK_TYPE_COUNT - 1)];
}

int distanceOf(Object* a, Object* b)
{
    if (!a || !b || a->tile < 0 || b->tile < 0) return 9999;
    int d = hex::distance(a->tile, b->tile);
    if ((a->flags & OF_MULTIHEX) && d > 0) d--;
    if ((b->flags & OF_MULTIHEX) && d > 0) d--;
    return d;
}

int rollCheck(int chance, int critChance, int* howMuch)
{
    int delta = chance - g_rng.range(1, 100);
    if (howMuch) *howMuch = delta;
    bool firstDay = gameTime() < kTicksPerDay;
    if (delta < 0) {
        if (!firstDay && g_rng.range(1, 100) <= -delta / 10) return ROLL_CRIT_FAIL;
        return ROLL_FAIL;
    }
    if (!firstDay && g_rng.range(1, 100) <= delta / 10 + critChance) return ROLL_CRIT_SUCCESS;
    return ROLL_SUCCESS;
}

bool canSee(Object* a, Object* b)
{
    int diff = abs(a->rotation - hex::direction(a->tile, b->tile));
    return diff == 0 || diff == 1 || diff == 5;
}

bool withinPerception(Object* a, Object* b)
{
    if (!a || !b || a->tile < 0 || b->tile < 0 || a->elevation != b->elevation) return false;
    int dist = hex::distance(a->tile, b->tile);
    int pe = critterStat(a, STAT_PE);
    int maxD;
    if (canSee(a, b)) {
        maxD = pe * 5;
        if (b->flags & OF_TRANS_GLASS) maxD /= 2;
        if (b == dude() && b->critter.sneaking) {
            // sneaking works when the sneak roll succeeds
            if (g_rng.range(1, 100) <= critterSkill(b, SKILL_SNEAK)) maxD /= 4;
        }
    } else {
        maxD = inCombat() ? pe * 2 : pe;
    }
    return dist <= maxD;
}

// True if a non-critter obstacle blocks the line of fire; `critters` counts critters in the way.
bool shotBlocked(Object* attacker, int from, int to, Object* defender, int* critters)
{
    if (critters) *critters = 0;
    if (!attacker || from < 0 || to < 0) return false;
    std::vector<int> line = hex::line(from, to);
    for (size_t i = 1; i < line.size(); i++) {
        int t = line[i];
        Object* b = g_world.shootBlockingAt(attacker, t, attacker->elevation);
        if (b && b != defender) {
            if (!b->isCritter()) return true;
            if (critters) (*critters)++;
        }
        if (t == to) break;
    }
    return false;
}

bool hitFromFront(Object* attacker, Object* defender)
{
    int diff = abs(attacker->rotation - defender->rotation);
    return diff != 0 && diff != 1 && diff != 5;
}

int toHit(Object* attacker, Object* defender, int location, int hitMode, bool checkRange)
{
    if (!attacker || !defender) return 0;
    Object* weapon = hitWith(attacker, hitMode);
    bool ranged = false;
    int acc;
    if (!weapon) {
        acc = critterSkill(attacker, SKILL_UNARMED);
    } else {
        acc = critterSkill(attacker, weaponSkill(weapon, hitMode));
        int kind = attackKindOf(weaponAttackType(weapon, hitMode));
        if (kind == AK_RANGED || kind == AK_THROW) {
            ranged = true;
            if (checkRange) {
                const Proto* wp = weapon->proto();
                int perceptionMod = (wp && wp->weapon.perk == 7) ? 4 : 2; // weapon long range perk
                int pe = critterStat(attacker, STAT_PE);
                int range = distanceOf(attacker, defender) - perceptionMod * pe;
                if (range < -2 * pe) range = -2 * pe;
                if (attacker == dude()) range -= 2 * perkLevel(PERK_SHARPSHOOTER);
                if (range >= 0 && (attacker->critter.results & DAM_BLIND)) acc -= 12 * range;
                else acc -= 4 * range;
            }
            int critters = 0;
            shotBlocked(attacker, attacker->tile, defender->tile, defender, &critters);
            acc -= 10 * critters;
        }
        if (attacker == dude() && hasTrait(TRAIT_ONE_HANDER)) acc += weaponIsTwoHanded(weapon) ? -40 : 20;
        const Proto* wp = weapon->proto();
        if (wp) {
            int st = wp->weapon.minStrength - critterStat(attacker, STAT_ST);
            if (st > 0) acc -= 20 * st;
            if (wp->weapon.perk == 8) acc += 20; // weapon accurate perk
        }
    }
    acc -= critterStat(defender, STAT_AC);
    int loc = clampv(location, 0, HIT_LOCATION_COUNT - 1);
    acc += ranged ? hitLocationPenalty[loc] : hitLocationPenalty[loc] / 2;
    if (defender->flags & OF_MULTIHEX) acc += 15;
    if (attacker == dude()) {
        int light = (int)(g_view.lightAt(defender->tile, defender->elevation) * 65536.0f);
        if (light <= 26214) acc -= 40;
        else if (light <= 39321) acc -= 25;
        else if (light <= 52428) acc -= 10;
    }
    if (C.gcsd.valid && C.gcsd.attacker == attacker->uid) acc += C.gcsd.bonus;
    if (attacker->critter.results & DAM_BLIND) acc -= 25;
    if (defender->critter.results & (DAM_KNOCKED_OUT | DAM_KNOCKED_DOWN)) acc += 40;
    if (dude() && attacker->critter.team != dude()->critter.team) {
        if (g_game.combatDifficulty == 0) acc -= 20;
        else if (g_game.combatDifficulty == 2) acc += 20;
    }
    return std::min(acc, 95);
}

int weaponDamageRoll(Object* critter, int hitMode)
{
    Object* weapon = hitWith(critter, hitMode);
    int minD, maxD, bonus = 0;
    if (weapon) {
        minD = weaponMinDamage(weapon);
        maxD = weaponMaxDamage(weapon);
        int kind = attackKindOf(weaponAttackType(weapon, hitMode));
        if (kind == AK_MELEE || kind == AK_UNARMED) bonus = critterStat(critter, STAT_MELEE_DMG);
    } else {
        minD = 1;
        maxD = critterStat(critter, STAT_MELEE_DMG) + 2;
    }
    return g_rng.range(minD, std::max(minD, maxD + bonus));
}

// ------------------------------------------------------------------ attack resolution
struct Extra {
    Object* obj = nullptr;
    int damage = 0;
    u32 flags = 0;
    int knockback = 0;
};

struct Attack {
    Object* attacker = nullptr;
    Object* defender = nullptr;
    Object* oops = nullptr;     // the intended target when another critter got hit
    Object* weapon = nullptr;
    int hitMode = HIT_PUNCH;
    int location = HIT_TORSO;
    int tile = -1;
    u32 attackerFlags = 0, defenderFlags = 0;
    int attackerDamage = 0, defenderDamage = 0;
    int knockback = 0;
    int ammoUsed = 0;
    int critMessage = 0;
    std::vector<Extra> extras;
};

void computeDamage(Attack& a, Object* critter, int rounds, int multiplier, int* damageOut, u32* flagsOut, int* knockbackOut)
{
    *damageOut = 0;
    if (!critter || !critter->isCritter()) return;
    int damageType = a.weapon ? weaponDamageType(a.weapon) : DMG_NORMAL;
    int dt = 0, dr = 0;
    if (!(*flagsOut & DAM_BYPASS) || damageType == DMG_EMP) {
        const Proto* wp = a.weapon ? a.weapon->proto() : nullptr;
        dt = (wp && wp->weapon.perk == 9) ? 0 : critterStat(critter, STAT_DT_NORMAL + damageType); // weapon penetrate perk
        dr = critterStat(critter, STAT_DR_NORMAL + damageType);
        if (a.attacker == dude() && hasTrait(TRAIT_FINESSE)) dr += 30;
    }
    int bonusRanged = (a.attacker == dude() && attackKind(a.attacker, a.hitMode) == AK_RANGED) ? 2 * perkLevel(PERK_BONUS_RANGED_DAMAGE) : 0;
    int diffMult = 100;
    if (dude() && a.attacker->critter.team != dude()->critter.team) {
        if (g_game.combatDifficulty == 0) diffMult = 75;
        else if (g_game.combatDifficulty == 2) diffMult = 125;
    }
    for (int r = 0; r < rounds; r++) {
        int d = weaponDamageRoll(a.attacker, a.hitMode) + bonusRanged;
        d = d * multiplier / 2;
        d = d * diffMult / 100;
        d -= dt;
        if (d > 0) {
            d -= d * dr / 100;
            if (d > 0) *damageOut += d;
        }
    }
    if (knockbackOut && !(critter->flags & OF_MULTIHEX)) {
        int kind = attackKind(a.attacker, a.hitMode);
        if (damageType == DMG_EXPLOSION || !a.weapon || kind == AK_MELEE) {
            const Proto* wp = a.weapon ? a.weapon->proto() : nullptr;
            *knockbackOut = (wp && wp->weapon.perk == 10) ? *damageOut / 5 : *damageOut / 10; // weapon knockback perk
        }
    }
}

void computeDefenderDamage(Attack& a, int rounds, int multiplier)
{
    if (a.attackerFlags & DAM_HIT) computeDamage(a, a.defender, rounds, multiplier, &a.defenderDamage, &a.defenderFlags, &a.knockback);
    else computeDamage(a, a.attacker, rounds, multiplier, &a.attackerDamage, &a.attackerFlags, nullptr);
}

void randomCripple(u32& flags)
{
    flags &= ~DAM_CRIP_RANDOM;
    static const u32 kLimbs[4] = { DAM_CRIP_LEG_LEFT, DAM_CRIP_LEG_RIGHT, DAM_CRIP_ARM_LEFT, DAM_CRIP_ARM_RIGHT };
    flags |= kLimbs[g_rng.range(0, 3)];
}

int critSuccess(Attack& a)
{
    Object* d = a.defender;
    if (d && (u32)d->pid == PID_IMMORTAL) return 2;
    a.attackerFlags |= DAM_CRITICAL;
    loadCritTables();
    int roll = g_rng.range(1, 100) + critterStat(a.attacker, STAT_BETTER_CRITS);
    int level = roll <= 20 ? 0 : roll <= 45 ? 1 : roll <= 70 ? 2 : roll <= 90 ? 3 : roll <= 100 ? 4 : 5;
    const CritEffect* e = nullptr;
    int loc = clampv(a.location, 0, HIT_LOCATION_COUNT - 1);
    if (d == dude()) {
        e = &g_crit.player[loc][level];
    } else if (!g_crit.kill.empty()) {
        const Proto* p = d ? d->proto() : nullptr;
        int kt = clampv(p ? p->killType : 0, 0, (int)g_crit.kill.size() - 1);
        e = &g_crit.kill[kt][loc][level];
    }
    if (!e) return 4;
    a.defenderFlags |= e->flags;
    a.critMessage = e->message;
    if (e->checkStat >= 0 && d) {
        if (!critterStatRoll(d, e->checkStat, e->checkMod, nullptr)) {
            a.defenderFlags |= e->checkFlags;
            a.critMessage = e->checkMessage;
        }
    }
    if (a.defenderFlags & DAM_CRIP_RANDOM) randomCripple(a.defenderFlags);
    return e->multiplier;
}

Object* randomTarget(Attack& a)
{
    std::vector<Object*> candidates;
    for (u32 uid : C.list) {
        Object* o = objOf(uid);
        if (!o || o == a.attacker || o == a.defender || !critterIsActive(o)) continue;
        if (distanceOf(a.attacker, o) > weaponRange(a.attacker, a.weapon, a.hitMode)) continue;
        candidates.push_back(o);
    }
    if (candidates.empty()) return nullptr;
    return candidates[g_rng.range(0, (int)candidates.size() - 1)];
}

void critFailure(Attack& a)
{
    a.attackerFlags &= ~DAM_HIT;
    if ((u32)a.attacker->pid == PID_IMMORTAL) return;
    int kind = attackKind(a.attacker, a.hitMode);
    const Proto* wp = a.weapon ? a.weapon->proto() : nullptr;
    int table = wp ? clampv(wp->weapon.criticalFail, 0, 6) : 0;
    int roll = g_rng.range(1, 100) - 5 * (critterStat(a.attacker, STAT_LK) - 5);
    int effect = roll <= 20 ? 0 : roll <= 50 ? 1 : roll <= 75 ? 2 : roll <= 95 ? 3 : 4;
    u32 flags = kCritFail[table][effect];
    if (!flags) return;
    a.attackerFlags |= DAM_CRITICAL | flags;
    if (a.attackerFlags & DAM_HIT_SELF) computeDamage(a, a.attacker, kind == AK_RANGED ? std::max(1, a.ammoUsed) : 1, 2, &a.attackerDamage, &a.attackerFlags, nullptr);
    else if (a.attackerFlags & DAM_EXPLODE) computeDamage(a, a.attacker, 1, 2, &a.attackerDamage, &a.attackerFlags, nullptr);
    if (a.attackerFlags & DAM_HURT_SELF) a.attackerDamage += g_rng.range(1, 5);
    if (a.attackerFlags & DAM_LOSE_TURN) a.attacker->critter.ap = 0;
    if (a.attackerFlags & DAM_LOSE_AMMO) {
        if (kind == AK_RANGED && a.weapon) a.ammoUsed = a.weapon->ammo;
        else a.attackerFlags &= ~DAM_LOSE_AMMO;
    }
    if (a.attackerFlags & DAM_CRIP_RANDOM) randomCripple(a.attackerFlags);
    if (a.attackerFlags & DAM_RANDOM_HIT) {
        Object* t = randomTarget(a);
        if (t) {
            a.oops = a.defender;
            a.defender = t;
            a.attackerFlags |= DAM_HIT;
            a.attackerFlags &= ~DAM_CRITICAL;
            a.location = HIT_TORSO;
            computeDamage(a, t, kind == AK_RANGED ? std::max(1, a.ammoUsed) : 1, 2, &a.defenderDamage, &a.defenderFlags, &a.knockback);
            a.tile = t->tile;
        }
    }
}

// A missed shot may hit something else along the line.
bool rangedMiss(Attack& a)
{
    int range = weaponRange(a.attacker, a.weapon, a.hitMode);
    int end = hex::beyond(a.attacker->tile, a.defender->tile, range);
    std::vector<int> line = hex::line(a.attacker->tile, end, range + 2);
    for (size_t i = 1; i < line.size(); i++) {
        Object* b = g_world.shootBlockingAt(a.attacker, line[i], a.attacker->elevation);
        if (!b || b == a.defender) continue;
        if (!b->isCritter()) return false;
        int chance = b->isDead() ? 5 : toHit(a.attacker, b, a.location, a.hitMode, true) / 3;
        if (g_rng.range(1, 100) <= chance) {
            a.oops = a.defender;
            a.defender = b;
            a.tile = b->tile;
            a.attackerFlags |= DAM_HIT;
            a.location = HIT_TORSO;
            computeDamage(a, b, 1, 2, &a.defenderDamage, &a.defenderFlags, &a.knockback);
            return true;
        }
    }
    return false;
}

void addExtra(Attack& a, Object* o, int damage, u32 flags, int knockback, bool accumulate)
{
    for (auto& e : a.extras) {
        if (e.obj == o) {
            if (accumulate) {
                e.damage += damage;
                e.flags |= flags;
                e.knockback += knockback;
            }
            return;
        }
    }
    if (a.extras.size() >= 6) return;
    a.extras.push_back({ o, damage, flags, knockback });
}

int shootAlongPath(Attack& a, int endTile, int rounds, int anim)
{
    int hitMain = 0;
    std::vector<int> line = hex::line(a.attacker->tile, endTile, 60);
    std::set<Object*> seen;
    for (size_t i = 1; i < line.size(); i++) {
        if ((rounds <= 0 && anim != ANIM_FIRE_CONTINUOUS) || a.extras.size() >= 6) break;
        Object* b = g_world.shootBlockingAt(a.attacker, line[i], a.attacker->elevation);
        if (!b || seen.count(b)) continue;
        seen.insert(b);
        if (!b->isCritter()) break;
        int acc = toHit(a.attacker, b, HIT_TORSO, a.hitMode, true);
        if (anim == ANIM_FIRE_CONTINUOUS) rounds = 1;
        int hits = 0;
        while (rounds > 0 && g_rng.range(1, 100) <= acc) {
            rounds--;
            hits++;
        }
        if (hits == 0) continue;
        if (b == a.defender) {
            hitMain += hits;
        } else {
            Attack tmp = a;
            tmp.extras.clear();
            int dmg = 0, kb = 0;
            u32 fl = 0;
            computeDamage(tmp, b, hits, 2, &dmg, &fl, &kb);
            addExtra(a, b, dmg, fl, kb, anim == ANIM_FIRE_BURST);
        }
    }
    if (anim == ANIM_FIRE_CONTINUOUS) hitMain = 0;
    return hitMain;
}

int computeSpray(Attack& a, int accuracy, int& hitMain, int& roundsSpent, int anim)
{
    hitMain = 0;
    const Proto* wp = a.weapon ? a.weapon->proto() : nullptr;
    int ammo = a.weapon ? a.weapon->ammo : 0;
    int burst = wp ? std::max(1, wp->weapon.burstRounds) : 1;
    if (burst < ammo || ammo <= 0) ammo = burst;
    roundsSpent = ammo;
    int roll = rollCheck(accuracy, critterStat(a.attacker, STAT_CRIT_CHANCE), nullptr);
    if (roll == ROLL_CRIT_FAIL) return roll;
    if (roll == ROLL_CRIT_SUCCESS) accuracy += 20;
    int left, center, right, mainRounds;
    if (anim == ANIM_FIRE_BURST) {
        center = std::max(1, ammo / 3);
        left = ammo / 3;
        right = ammo - center - left;
        mainRounds = center / 2;
        if (mainRounds == 0) {
            mainRounds = 1;
            center -= 1;
        }
    } else {
        left = mainRounds = center = right = 1;
    }
    for (int i = 0; i < mainRounds; i++)
        if (rollCheck(accuracy, 0, nullptr) >= ROLL_SUCCESS) hitMain++;
    if (hitMain == 0 && rangedMiss(a)) hitMain = 1;
    int range = weaponRange(a.attacker, a.weapon, a.hitMode);
    hitMain += shootAlongPath(a, hex::beyond(a.attacker->tile, a.defender->tile, range), center - hitMain, anim);
    int centerTile = distanceOf(a.attacker, a.defender) <= 3 ? hex::beyond(a.attacker->tile, a.defender->tile, 3) : a.defender->tile;
    int rot = hex::direction(centerTile, a.attacker->tile);
    int leftTile = hex::inDirection(centerTile, (rot + 1) % 6, 1);
    int rightTile = hex::inDirection(centerTile, (rot + 5) % 6, 1);
    hitMain += shootAlongPath(a, hex::beyond(a.attacker->tile, leftTile, range), left, anim);
    hitMain += shootAlongPath(a, hex::beyond(a.attacker->tile, rightTile, range), right, anim);
    if (roll == ROLL_FAIL && (hitMain > 0 || !a.extras.empty())) roll = ROLL_SUCCESS;
    else if (roll >= ROLL_SUCCESS && hitMain == 0 && a.extras.empty()) roll = ROLL_FAIL;
    return roll;
}

// Explosion damage on everything within the blast radius around the hit (grenades, explosive weapons).
void explosionExtras(Attack& a, bool fromAttacker, bool grenade)
{
    Object* center = fromAttacker ? a.attacker : ((a.attackerFlags & DAM_HIT) ? a.defender : nullptr);
    int origin = center ? center->tile : a.tile;
    if (origin < 0) return;
    int radius = grenade ? 2 : 3;
    for (Object* o : g_world.objectsInRadius(origin, a.attacker->elevation, radius)) {
        if (a.extras.size() >= 6) break;
        if (!o->isCritter() || o->isDead() || o == a.defender) continue;
        if (shotBlocked(o, o->tile, origin, nullptr, nullptr)) continue;
        if (o == a.attacker) {
            u32 savedHit = a.attackerFlags & DAM_HIT;
            a.attackerFlags &= ~DAM_HIT;
            computeDamage(a, a.attacker, 1, 2, &a.attackerDamage, &a.attackerFlags, nullptr);
            a.attackerFlags |= savedHit | DAM_BACKWASH;
            continue;
        }
        Attack tmp = a;
        int dmg = 0, kb = 0;
        u32 fl = 0;
        computeDamage(tmp, o, 1, 2, &dmg, &fl, &kb);
        addExtra(a, o, dmg, fl, kb, false);
    }
}

void deathChecks(Attack& a)
{
    auto check = [](Object* o, int damage, u32& flags) {
        if (!o || !o->isCritter() || (u32)o->pid == PID_IMMORTAL) return;
        if (damage > 0 && o->critter.hp - damage <= 0) flags |= DAM_DEAD;
    };
    check(a.attacker, a.attackerDamage, a.attackerFlags);
    check(a.defender, a.defenderDamage, a.defenderFlags);
    for (auto& e : a.extras) check(e.obj, e.damage, e.flags);
}

int computeAttack(Attack& a)
{
    int range = weaponRange(a.attacker, a.weapon, a.hitMode);
    if (range < distanceOf(a.attacker, a.defender)) return -1;
    int anim = attackAnimOf(a.attacker, a.hitMode);
    int chance = toHit(a.attacker, a.defender, a.location, a.hitMode, true);
    int damageType = a.weapon ? weaponDamageType(a.weapon) : DMG_NORMAL;
    bool grenade = anim == ANIM_THROW_ANIM && (damageType == DMG_EXPLOSION || damageType == DMG_PLASMA || damageType == DMG_EMP);
    if (a.location == HIT_UNCALLED) a.location = HIT_TORSO;
    int kind = attackKind(a.attacker, a.hitMode);
    int hitMain = 1, multiplier = 2, roundsSpent = 1, roll;
    if (anim == ANIM_FIRE_BURST || anim == ANIM_FIRE_CONTINUOUS) {
        roll = computeSpray(a, chance, hitMain, roundsSpent, anim);
    } else {
        roll = rollCheck(chance, critterStat(a.attacker, STAT_CRIT_CHANCE) - hitLocationPenalty[a.location], nullptr);
    }
    if (roll == ROLL_FAIL && a.attacker == dude() && hasTrait(TRAIT_JINXED) && g_rng.range(0, 1) == 1) roll = ROLL_CRIT_FAIL;
    if (kind == AK_RANGED) a.ammoUsed = roundsSpent;
    else if (a.weapon && weaponUsesAmmo(a.weapon)) a.ammoUsed = 1;
    switch (roll) {
    case ROLL_CRIT_SUCCESS:
        multiplier = critSuccess(a);
        // fallthrough
    case ROLL_SUCCESS:
        a.attackerFlags |= DAM_HIT;
        computeDefenderDamage(a, hitMain, multiplier);
        break;
    case ROLL_FAIL:
        if (kind == AK_RANGED || kind == AK_THROW) rangedMiss(a);
        break;
    case ROLL_CRIT_FAIL: critFailure(a); break;
    }
    if ((kind == AK_RANGED || kind == AK_THROW) && !(a.attackerFlags & (DAM_HIT | DAM_CRITICAL))) {
        // the projectile lands somewhere
        if (grenade) {
            int d = std::max(1, g_rng.range(1, std::max(1, distanceOf(a.attacker, a.defender) / 2)));
            a.tile = hex::inDirection(a.defender->tile, g_rng.range(0, 5), d);
        } else {
            a.tile = hex::beyond(a.attacker->tile, a.defender->tile, range);
        }
    }
    if (a.tile < 0) a.tile = a.defender->tile;
    if (damageType == DMG_EXPLOSION || grenade) {
        if ((a.attackerFlags & DAM_HIT) || !(a.attackerFlags & DAM_CRITICAL)) explosionExtras(a, false, grenade);
    } else if (a.attackerFlags & DAM_EXPLODE) {
        explosionExtras(a, true, grenade);
    }
    deathChecks(a);
    return 0;
}

// ------------------------------------------------------------------ sounds
std::string weaponSfx(char effect, Object* weapon, int hitMode, Object* target)
{
    if (!weapon) return std::string();
    const Proto* p = weapon->proto();
    char code = p && p->weapon.soundCode ? (char)tolower(p->weapon.soundCode) : 'a';
    int mode = (effect == 'r' || effect == 'o') ? 1 : ((hitMode == HIT_LEFT_PRIMARY || hitMode == HIT_RIGHT_PRIMARY || hitMode == HIT_PUNCH) ? 1 : 2);
    char material = 'x';
    int dt = p ? p->weapon.damageType : 0;
    if (effect == 'h' && target && dt != DMG_EXPLOSION && dt != DMG_PLASMA && dt != DMG_EMP) {
        int mat = -1;
        if (target->type() != OBJ_CRITTER) {
            if (const Proto* tp = target->proto()) mat = tp->material;
        }
        switch (mat) {
        case MAT_GLASS:
        case MAT_METAL:
        case MAT_PLASTIC: material = 'm'; break;
        case MAT_WOOD: material = 'w'; break;
        case MAT_DIRT:
        case MAT_STONE:
        case MAT_CEMENT: material = 's'; break;
        default: material = 'f'; break;
        }
    }
    std::string name = strFormat("w%c%c%d%cxx1", effect, code, mode, material);
    if (!soundExists(name)) name = strFormat("w%c%c1%cxx1", effect, code, material);
    return name;
}

// Critter sound: <base><code1><code2>; humans share hmxxxx / hfxxxx in the demo data.
std::string critterSfx(Object* c, int anim, int extra)
{
    if (!c || !c->isCritter()) return std::string();
    std::string base = strLower(g_art.name(OBJ_CRITTER, fidIndex(c->fid)));
    if (base.size() >= 6) base = base.substr(0, 6);
    if (base.size() == 6 && (strStartsWith(base, "hm") || strStartsWith(base, "hf"))) base = base.substr(0, 2) + "xxxx";
    char c1, c2;
    if (!artCritterCode(anim, fidWeapon(c->fid), c1, c2)) return std::string();
    if (anim == ANIM_FALL_FRONT || anim == ANIM_FALL_BACK) {
        if (extra == 1) c1 = 'y';       // passing out
        else if (extra == 2) c1 = 'z';  // dying
    } else if ((anim == ANIM_THROW_PUNCH || anim == ANIM_KICK_LEG) && extra == 3) {
        c1 = 'z';                       // contact
    }
    return base + c1 + c2;
}

// ------------------------------------------------------------------ messages
std::string nameFor(Object* o)
{
    if (!o) return std::string();
    if (o == dude()) return combatText(506, "You");
    return o->isCritter() ? critterName(o) : g_protos.name(o->pid);
}

std::string flagsText(u32 flags)
{
    static const std::pair<u32, int> kFlagMsgs[] = { { DAM_KNOCKED_OUT, 200 }, { DAM_KNOCKED_DOWN, 201 }, { DAM_CRIP_LEG_LEFT, 202 },
        { DAM_CRIP_LEG_RIGHT, 203 }, { DAM_CRIP_ARM_LEFT, 204 }, { DAM_CRIP_ARM_RIGHT, 205 }, { DAM_BLIND, 206 }, { DAM_ON_FIRE, 210 },
        { DAM_DEAD, 207 } };
    static const char* kFallback[] = { "knocked out", "knocked down", "crippled left leg", "crippled right leg", "crippled left arm",
        "crippled right arm", "blinded", "on fire", "killed" };
    std::string out;
    int i = 0;
    for (auto& f : kFlagMsgs) {
        if (flags & f.first) {
            if (f.first == DAM_KNOCKED_DOWN && (flags & DAM_KNOCKED_OUT)) {
                i++;
                continue;
            }
            out += combatText(518, " and ") + combatText(f.second, kFallback[i]);
        }
        i++;
    }
    return out;
}

std::string hitLine(Object* o, int damage, u32 flags, int location, bool critical, bool showLocation)
{
    std::string name = nameFor(o);
    std::string verb = o == dude() ? combatText(507, "were") : combatText(508, "was");
    std::string loc = combatText(100 + clampv(location, 0, 8), "torso");
    std::string f;
    if (damage <= 0) f = combatText(showLocation ? (critical ? 525 : 526) : (critical ? 528 : 527), "%s %s hit for no damage");
    else if (damage == 1) f = combatText(showLocation ? (critical ? 521 : 522) : (critical ? 524 : 523), "%s %s hit for 1 hit point");
    else f = combatText(showLocation ? (critical ? 511 : 512) : (critical ? 520 : 513), "%s %s hit for %d hit points");
    std::string line;
    if (showLocation) line = strFormat(f.c_str(), name.c_str(), verb.c_str(), loc.c_str(), damage);
    else line = strFormat(f.c_str(), name.c_str(), verb.c_str(), damage);
    return line + flagsText(flags) + ".";
}

void displayAttack(const Attack& a)
{
    if (a.attacker == dude() && a.weapon) {
        const Proto* wp = a.weapon->proto();
        if (wp && wp->weapon.minStrength > critterStat(dude(), STAT_ST)) displayMessage("You are not strong enough to use this weapon properly.");
    }
    if (a.attackerFlags & DAM_HIT) {
        if (a.oops && a.oops != a.defender) {
            std::string f = combatText(509, "Oops! %s %s hit instead of %s!");
            std::string verb = a.defender == dude() ? combatText(507, "were") : combatText(508, "was");
            displayMessage(strFormat(f.c_str(), nameFor(a.defender).c_str(), verb.c_str(), nameFor(a.oops).c_str()));
        }
        bool showLoc = a.location != HIT_TORSO && a.location != HIT_UNCALLED;
        displayMessage(hitLine(a.defender, a.defenderDamage, a.defenderFlags, a.location, (a.attackerFlags & DAM_CRITICAL) != 0, showLoc || (a.attackerFlags & DAM_CRITICAL)));
    } else {
        std::string f = combatText((a.attackerFlags & DAM_CRITICAL) ? 514 : 515, "%s missed");
        displayMessage(strFormat(f.c_str(), nameFor(a.attacker).c_str()) + flagsText(a.attackerFlags & ~DAM_DEAD) + ".");
    }
    if (a.attackerDamage > 0 || (a.attackerFlags & DAM_DEAD)) displayMessage(hitLine(a.attacker, a.attackerDamage, a.attackerFlags & ~DAM_HIT, HIT_TORSO, false, false));
    for (auto& e : a.extras) {
        std::string f = combatText(517, "Nearby %s hit for %d hit points");
        displayMessage(strFormat(f.c_str(), nameFor(e.obj).c_str(), e.damage) + flagsText(e.flags) + ".");
    }
}

// combatai.msg taunts
enum TauntType { TAUNT_RUN, TAUNT_MOVE, TAUNT_ATTACK, TAUNT_MISS, TAUNT_HIT };

void taunt(Object* c, int type, int location = HIT_TORSO)
{
    if (!c || c == dude() || !c->isCritter() || c->tile < 0) return;
    const AiPacket& p = packetOf(c);
    if (g_rng.range(1, 100) > p.chance) return;
    int start = 0, end = 0;
    switch (type) {
    case TAUNT_RUN: start = p.runStart; end = p.moveStart; break;
    case TAUNT_MOVE: start = p.moveStart; end = p.attackStart; break;
    case TAUNT_ATTACK: start = p.attackStart; end = p.missStart; break;
    case TAUNT_MISS: start = p.missStart; end = p.hitStart[HIT_HEAD]; break;
    case TAUNT_HIT: {
        int l = clampv(location, 0, 7);
        start = p.hitStart[l];
        end = l < 7 ? p.hitStart[l + 1] : p.lastMsg + 1;
        break;
    }
    }
    if (end <= start) return;
    int num = g_rng.range(start, end - 1);
    const MessageEntry* e = messages("text/" + g_language + "/game/combatai.msg").find(num);
    if (!e || e->text.empty()) return;
    u32 color = p.color > 0 ? (g_palette.argb(p.color) | 0xFF000000u) : 0xFFFFFF55u;
    g_view.addFloatText(c, e->text, color, p.font);
}

// ------------------------------------------------------------------ damage application & reactions
void setResults(Object* c, u32 flags)
{
    if (!c || !c->isCritter() || (u32)c->pid == PID_IMMORTAL) return;
    if (flags & DAM_DEAD) {
        queueRemove(c, EV_KNOCKOUT);
    } else if (flags & DAM_KNOCKED_OUT) {
        queueRemove(c, EV_KNOCKOUT);
        queueAdd(10 * (35 - 3 * critterStat(c, STAT_EN)), c, EV_KNOCKOUT);
    }
    c->critter.results |= flags & (DAM_KNOCKED_OUT | DAM_KNOCKED_DOWN | DAM_CRIP | DAM_DEAD | DAM_LOSE_TURN);
}

int combatRating(Object* o)
{
    if (!o || !o->isCritter() || (o->critter.results & (DAM_DEAD | DAM_KNOCKED_OUT))) return 0;
    int r = critterStat(o, STAT_MELEE_DMG);
    for (Object* w : { itemRightHand(o), itemLeftHand(o) })
        if (w && itemType(w) == ITEM_WEAPON) r = std::max(r, weaponMaxDamage(w));
    return r + critterStat(o, STAT_AC);
}

void setWhoHitMe(Object* c, Object* who)
{
    if (!c || !c->isCritter()) return;
    if (who && !who->isCritter()) return;
    if (!who || who->critter.team != c->critter.team || !critterStatRoll(c, STAT_IN, -1, nullptr)) {
        c->critter.whoHitMe = who ? who->uid : 0;
        if (who == dude()) c->critter.reaction = std::max(-100, c->critter.reaction - 3);
    }
}

void checkRetaliation(Object* c, Object* candidate)
{
    if (combatRating(candidate) > combatRating(objOf(c->critter.whoHitMe))) setWhoHitMe(c, candidate);
}

void notifyOnlookers(Object* c)
{
    for (u32 uid : C.list) {
        Object* o = objOf(uid);
        if (o && o != c && withinPerception(o, c)) o->critter.maneuver |= MAN_ENGAGING;
    }
}

void giveKillExperience(Object* victim)
{
    Object* killer = objOf(victim->critter.whoHitMe);
    if (!killer || !dude()) return;
    if (killer != dude() && killer->critter.team != dude()->critter.team) return;
    const Proto* p = victim->proto();
    if (p) g_game.player.kills[clampv(p->killType, 0, 15)]++;
    int xp = p ? p->experience : 0;
    if (xp <= 0) return;
    if (inCombat()) C.exps += xp;
    else playerAddExperience(xp);
}

void damageObject(Object* o, int damage, bool scriptDamageProc)
{
    if (!o || !o->isCritter() || (u32)o->pid == PID_IMMORTAL || damage <= 0) return;
    critterAdjustHp(o, -damage);
    o->critter.damageLastTurn += damage;
    if (scriptDamageProc && o->script) g_scripts.execObject(o, SP_DAMAGE, objOf(o->critter.whoHitMe), nullptr);
    if (o->critter.results & DAM_DEAD) {
        if (o->script) {
            bool overrides = g_scripts.execObject(o, SP_DESTROY, objOf(o->critter.whoHitMe), nullptr);
            if (!overrides) giveKillExperience(o);
        } else {
            giveKillExperience(o);
        }
    }
}

void applyDamage(Attack& a)
{
    Object* att = a.attacker;
    bool attackerCritter = att && att->isCritter();
    if (attackerCritter && !att->isDead()) {
        setResults(att, a.attackerFlags);
        damageObject(att, a.attackerDamage, a.defender != a.oops);
    }
    if (a.oops && a.oops != a.defender) notifyOnlookers(a.oops);
    Object* def = a.defender;
    if (def && def->isCritter() && !def->isDead()) {
        setResults(def, a.defenderFlags);
        if (attackerCritter) {
            if (def->critter.results & (DAM_DEAD | DAM_KNOCKED_OUT)) setWhoHitMe(def, att);
            else if (def == a.oops || def->critter.team != att->critter.team) checkRetaliation(def, att);
        }
        damageObject(def, a.defenderDamage, a.defender != a.oops);
        notifyOnlookers(def);
        if (a.defenderDamage >= 0 && (a.attackerFlags & DAM_HIT) && att && att->script)
            g_scripts.exec(att->script, SP_COMBAT, nullptr, def, 2);
    }
    for (auto& e : a.extras) {
        Object* o = e.obj;
        if (!o->isCritter() || o->isDead()) continue;
        setResults(o, e.flags);
        if (attackerCritter) {
            if (o->critter.results & (DAM_DEAD | DAM_KNOCKED_OUT)) setWhoHitMe(o, att);
            else if (o->critter.team != att->critter.team) checkRetaliation(o, att);
        }
        damageObject(o, e.damage, a.defender != a.oops);
        notifyOnlookers(o);
    }
}

int checkDeathAnim(Object* o, int anim, int minViolence, bool front)
{
    if (g_game.violenceLevel >= minViolence && animArtExists(o, anim)) return anim;
    if (front) return ANIM_FALL_BACK;
    return animArtExists(o, ANIM_FALL_FRONT) ? ANIM_FALL_BACK : ANIM_FALL_FRONT;
}

int pickDeath(Object* attacker, Object* defender, int damage, int damageType, int attackAnim, bool front)
{
    static const int kDeath2[DMG_TYPE_COUNT] = { ANIM_BIG_HOLE, ANIM_CHARRED_BODY, ANIM_CHARRED_BODY, ANIM_ELECTRIFY, ANIM_ELECTRIFY,
        ANIM_ELECTRIFY, ANIM_FALL_BACK };
    static const int kDeath3[DMG_TYPE_COUNT] = { ANIM_EXPLODED_TO_NOTHING, ANIM_BURNED_TO_NOTHING, ANIM_BURNED_TO_NOTHING,
        ANIM_MELTED_TO_NOTHING, ANIM_ELECTRIFIED_TO_NOTHING, ANIM_ELECTRIFIED_TO_NOTHING, ANIM_EXPLODED_TO_NOTHING };
    bool bloody = attacker == dude() && hasTrait(TRAIT_BLOODY_MESS);
    int dt = clampv(damageType, 0, DMG_TYPE_COUNT - 1);
    int anim;
    if (attackAnim == ANIM_THROW_PUNCH || attackAnim == ANIM_KICK_LEG || attackAnim == ANIM_THRUST || attackAnim == ANIM_SWING ||
        attackAnim == ANIM_THROW_ANIM) {
        anim = (g_game.violenceLevel == 3 && bloody) ? ANIM_BIG_HOLE : ANIM_FALL_BACK;
    } else if (attackAnim == ANIM_FIRE_SINGLE && dt == DMG_NORMAL) {
        anim = (g_game.violenceLevel == 3 && (bloody || damage >= 45)) ? ANIM_BIG_HOLE : ANIM_FALL_BACK;
    } else if (g_game.violenceLevel > 2 && (bloody || damage >= 45)) {
        anim = kDeath3[dt];
        if (checkDeathAnim(defender, anim, 3, front) != anim) anim = kDeath2[dt];
    } else if (g_game.violenceLevel > 1 && (bloody || damage >= 15)) {
        anim = kDeath2[dt];
    } else {
        anim = ANIM_FALL_BACK;
    }
    if (!front && anim == ANIM_FALL_BACK) anim = ANIM_FALL_FRONT;
    return checkDeathAnim(defender, anim, 0, front);
}

// Makes a dead critter a corpse: flat, not blocking, drops nothing (inventory stays lootable).
void finishCorpse(Object* o, int anim)
{
    if (!o) return;
    o->flags |= OF_NO_BLOCK | OF_FLAT;
    o->outline = 0;
    if (anim >= ANIM_ELECTRIFIED_TO_NOTHING && anim <= ANIM_EXPLODED_TO_NOTHING) {
        // nothing left of the body: items fall to the floor
        auto items = o->inventory;
        for (auto& e : items) {
            Object* it = g_world.removeFromInventory(o, e.obj, e.count);
            if (it) g_world.place(it, o->tile, o->elevation);
        }
    }
}

// Adds the defender's reaction (hit / knock down / death) to the sequence under construction.
void addReaction(Object* def, int damage, u32 flags, Object* weapon, bool front, int knockback, int kbRot, int attackAnim, Object* attacker, int delay)
{
    if (!def || !def->isCritter()) return;
    u32 du = def->uid;
    // results were already applied: "prone" means lying on the ground before this hit
    int curAnim = fidAnim(def->fid);
    bool wasDown = (curAnim >= ANIM_FALL_BACK && curAnim <= ANIM_FALL_FRONT_BLOOD) || (curAnim >= ANIM_FIRST_SF_DEATH && curAnim <= ANIM_LAST_SF_DEATH);
    if (wasDown && !(flags & DAM_DEAD)) return;
    if (flags & DAM_DEAD) {
        int anim = wasDown ? -1 : pickDeath(attacker, def, damage, weapon ? weaponDamageType(weapon) : DMG_NORMAL, attackAnim, front);
        if (anim >= 0) {
            if (knockback > 0 && (anim == ANIM_FALL_FRONT || anim == ANIM_FALL_BACK)) {
                int dest = hex::inDirection(def->tile, kbRot, std::min(knockback, 3));
                animPlaySfx(def, critterSfx(def, anim, 2), delay);
                animMoveStraight(def, dest, def->elevation, anim, 0);
            } else {
                animPlaySfx(def, critterSfx(def, anim, 2), delay);
                animAnimate(def, anim, 0);
            }
        }
        int finalAnim = anim;
        animCallback(def, [du, finalAnim] {
            if (Object* o = g_world.byUid(du)) finishCorpse(o, finalAnim);
        }, -1, true);
        return;
    }
    if (flags & (DAM_KNOCKED_OUT | DAM_KNOCKED_DOWN)) {
        int anim = front ? ANIM_FALL_BACK : ANIM_FALL_FRONT;
        animPlaySfx(def, critterSfx(def, anim, 1), delay);
        if (knockback > 0) animMoveStraight(def, hex::inDirection(def->tile, kbRot, std::min(knockback, 3)), def->elevation, anim, 0);
        else animAnimate(def, anim, 0);
        return;
    }
    if (knockback > 0) {
        int anim = front ? ANIM_FALL_BACK : ANIM_FALL_FRONT;
        animMoveStraight(def, hex::inDirection(def->tile, kbRot, std::min(knockback, 3)), def->elevation, anim, delay);
        animAnimate(def, anim == ANIM_FALL_BACK ? ANIM_BACK_TO_STANDING : ANIM_PRONE_TO_STANDING, -1);
        return;
    }
    int anim = (front || !animArtExists(def, ANIM_HIT_FROM_BACK)) ? ANIM_HIT_FROM_FRONT : ANIM_HIT_FROM_BACK;
    animPlaySfx(def, critterSfx(def, anim, 0), delay);
    animAnimate(def, anim, 0);
}

void handleWeaponEffects(Object* weaponOwner, Object* weapon, u32 flags)
{
    if (!weapon || !weaponOwner) return;
    if (flags & DAM_DESTROY) {
        Object* w = g_world.removeFromInventory(weaponOwner, weapon, 1);
        if (w) g_world.destroy(w);
        critterUpdateArt(weaponOwner);
    } else if (flags & (DAM_DROP | DAM_EXPLODE)) {
        Object* w = g_world.removeFromInventory(weaponOwner, weapon, 1);
        if (w) {
            if (flags & DAM_EXPLODE) g_world.destroy(w);
            else g_world.place(w, weaponOwner->tile, weaponOwner->elevation);
        }
        critterUpdateArt(weaponOwner);
    }
}

// Runs the attack animation; damage is applied when the blow lands (the attack's action frame).
void animateAttack(std::shared_ptr<Attack> ap)
{
    Attack& a = *ap;
    Object* att = a.attacker;
    int anim = attackAnimOf(att, a.hitMode);
    if (!animArtExists(att, anim)) anim = ANIM_THROW_PUNCH;
    int fid = animFid(att, anim);
    Sprite* s = g_art.sprite(fid);
    int actionFrame = s ? std::max(1, s->meta.actionFrame) : 1;
    bool melee = anim == ANIM_THROW_PUNCH || anim == ANIM_KICK_LEG || anim == ANIM_SWING || anim == ANIM_THRUST;
    std::string sfxAttack = melee && (anim == ANIM_THROW_PUNCH || anim == ANIM_KICK_LEG) ? critterSfx(att, anim, 0) : weaponSfx('a', a.weapon, a.hitMode, nullptr);
    std::string sfxHit = (anim == ANIM_THROW_PUNCH || anim == ANIM_KICK_LEG) ? critterSfx(att, anim, 3) : weaponSfx('h', a.weapon, a.hitMode, a.defender);

    animStop(att);
    if (a.defender && a.defender != att) animStop(a.defender);
    animBegin(ANIMSEQ_REPLACE | ANIMSEQ_COMBAT);
    if (a.defender && a.defender->tile >= 0 && a.defender != att) animRotateToTile(att, a.defender->tile);
    taunt(att, TAUNT_ATTACK);
    animPlaySfx(att, sfxAttack, 0);
    animAnimate(att, anim, -1);
    animCallback(att, [ap, sfxHit, anim] {
        Attack& at = *ap;
        if (at.attackerFlags & DAM_HIT) soundPlaySfxAt(sfxHit, at.defender);
        bool attackerProne = false;
        applyDamage(at);
        displayAttack(at);
        // reactions (separate sequences so they play alongside the end of the attack)
        animBegin(ANIMSEQ_COMBAT);
        if (at.attackerFlags & DAM_HIT) {
            if (at.defender && at.defender->isCritter()) {
                bool front = hitFromFront(at.attacker, at.defender);
                addReaction(at.defender, at.defenderDamage, at.defenderFlags, at.weapon, front, at.knockback,
                    hex::direction(at.attacker->tile, at.defender->tile), anim, at.attacker, 0);
                if (!(at.defenderFlags & DAM_DEAD)) taunt(at.attacker, TAUNT_HIT, at.location);
            }
        } else {
            if (at.defender && at.defender->isCritter() && !critterIsProne(at.defender) && critterIsActive(at.defender) &&
                (anim == ANIM_THROW_PUNCH || anim == ANIM_KICK_LEG || anim == ANIM_SWING || anim == ANIM_THRUST) && animArtExists(at.defender, ANIM_DODGE)) {
                animPlaySfx(at.defender, critterSfx(at.defender, ANIM_DODGE, 0), 0);
                animAnimate(at.defender, ANIM_DODGE, 0);
            }
            taunt(at.attacker, TAUNT_MISS);
        }
        for (auto& e : at.extras) {
            if (!e.obj->isCritter()) continue;
            bool front = hitFromFront(at.attacker, e.obj);
            addReaction(e.obj, e.damage, e.flags, at.weapon, front, e.knockback, hex::direction(at.attacker->tile, e.obj->tile), anim, at.attacker, 0);
        }
        if ((at.attackerFlags & (DAM_KNOCKED_DOWN | DAM_KNOCKED_OUT | DAM_DEAD)) || at.attackerDamage > 0) {
            addReaction(at.attacker, at.attackerDamage, at.attackerFlags, at.weapon, true, 0, 0, anim, at.attacker, 0);
            attackerProne = true;
        }
        animEnd();
        (void)attackerProne;
        handleWeaponEffects(at.attacker, at.weapon, at.attackerFlags);
    }, actionFrame, true);
    animEnd();
}

// ------------------------------------------------------------------ bad shot checks
int checkBadShot(Object* attacker, Object* defender, int hitMode, bool aiming)
{
    if (!defender || defender->isDead()) return BS_DEAD;
    Object* weapon = hitWith(attacker, hitMode);
    if (weapon) {
        if ((attacker->critter.results & DAM_CRIP_ARM_LEFT) && (attacker->critter.results & DAM_CRIP_ARM_RIGHT)) return BS_BOTH_ARMS_CRIPPLED;
        if ((attacker->critter.results & DAM_CRIP_ARM_ANY) && weaponIsTwoHanded(weapon)) return BS_ARM_CRIPPLED;
    }
    int ap = weaponApCost(attacker, weapon, hitMode, aiming);
    int available = attacker->critter.ap;
    if (ap > available) return BS_NOT_ENOUGH_AP;
    if (weaponRange(attacker, weapon, hitMode) < distanceOf(attacker, defender)) return BS_OUT_OF_RANGE;
    int kind = attackKind(attacker, hitMode);
    if (weapon && weaponUsesAmmo(weapon) && weapon->ammo < std::max(1, weaponAmmoPerShot(weapon, hitMode))) return BS_NO_AMMO;
    if ((kind == AK_RANGED || kind == AK_THROW) && shotBlocked(attacker, attacker->tile, defender->tile, defender, nullptr)) return BS_AIM_BLOCKED;
    return BS_OK;
}

void reportBadShot(int bad, Object* attacker, int hitMode, bool aiming)
{
    switch (bad) {
    case BS_NOT_ENOUGH_AP: {
        std::string f = combatText(503, "You need %d action points.");
        displayMessage(strFormat(f.c_str(), weaponApCost(attacker, hitWith(attacker, hitMode), hitMode, aiming)));
        break;
    }
    case BS_OUT_OF_RANGE: displayMessage(combatText(505, "Target out of range.")); break;
    case BS_NO_AMMO:
        displayMessage(combatText(504, "Out of ammo."));
        soundPlaySfx(weaponSfx('o', hitWith(attacker, hitMode), hitMode, nullptr));
        break;
    case BS_AIM_BLOCKED: displayMessage(combatText(529, "Your aim is blocked.")); break;
    case BS_ARM_CRIPPLED: displayMessage(combatText(531, "You cannot use two-handed weapons with a crippled arm.")); break;
    case BS_BOTH_ARMS_CRIPPLED: displayMessage(combatText(530, "You cannot use weapons with both arms crippled.")); break;
    case BS_DEAD: displayMessage(combatText(502, "You can't attack.")); break;
    default: break;
    }
}

// Resolves and animates one attack. Returns false if the attack could not be made.
bool doAttack(Object* attacker, Object* defender, int hitMode, int location)
{
    bool aiming = location != HIT_UNCALLED && location != HIT_TORSO;
    int bad = checkBadShot(attacker, defender, hitMode, aiming);
    if (bad != BS_OK) {
        if (attacker == dude()) reportBadShot(bad, attacker, hitMode, aiming);
        return false;
    }
    if (hitMode == HIT_PUNCH && g_rng.range(1, 4) == 1 && animArtExists(attacker, ANIM_KICK_LEG)) hitMode = HIT_KICK;
    auto a = std::make_shared<Attack>();
    a->attacker = attacker;
    a->defender = defender;
    a->hitMode = hitMode;
    a->location = location;
    a->weapon = hitWith(attacker, hitMode);
    if (computeAttack(*a) == -1) {
        if (attacker == dude()) reportBadShot(BS_OUT_OF_RANGE, attacker, hitMode, aiming);
        return false;
    }
    if (C.gcsd.valid && C.gcsd.attacker == attacker->uid) {
        a->defenderDamage = clampv(a->defenderDamage, C.gcsd.minDmg, std::max(C.gcsd.minDmg, C.gcsd.maxDmg));
        if (C.gcsd.attackerResults == C.gcsd.targetResults && C.gcsd.targetResults) a->defenderFlags = (u32)C.gcsd.targetResults;
    }
    int ap = weaponApCost(attacker, a->weapon, a->hitMode, aiming);
    attacker->critter.ap = std::max(0, attacker->critter.ap - ap);
    if (a->weapon && a->ammoUsed > 0) a->weapon->ammo = std::max(0, a->weapon->ammo - a->ammoUsed);
    if (attacker == dude()) setWhoHitMe(attacker, defender);
    animateAttack(a);
    return true;
}

// ------------------------------------------------------------------ combat flow
std::vector<Object*> comList()
{
    std::vector<Object*> v;
    for (int i = 0; i < C.com && i < (int)C.list.size(); i++)
        if (Object* o = objOf(C.list[i])) v.push_back(o);
    return v;
}

bool inList(u32 uid, int from, int to)
{
    for (int i = from; i < to && i < (int)C.list.size(); i++)
        if (C.list[i] == uid) return true;
    return false;
}

Object* dangerSource(Object* c)
{
    Object* whoHitMe = objOf(c->critter.whoHitMe);
    std::vector<Object*> targets;
    if (whoHitMe && whoHitMe != c) {
        if (!whoHitMe->isDead()) return whoHitMe;
        // avenge: nearest member of the killer's team
        if (whoHitMe->critter.team != c->critter.team) {
            Object* best = nullptr;
            int bestD = 1 << 30;
            for (u32 uid : C.list) {
                Object* o = objOf(uid);
                if (!o || o == c || o->isDead() || o->critter.team != whoHitMe->critter.team) continue;
                int d = distanceOf(c, o);
                if (d < bestD) bestD = d, best = o;
            }
            if (best) targets.push_back(best);
        }
    }
    // critters attacking me, enemies of my team mates, and enemies attacking my team
    Object* attackingMe = nullptr;
    Object* hittingFriend = nullptr;
    Object* enemyOfTeam = nullptr;
    std::vector<Object*> sorted;
    for (u32 uid : C.list)
        if (Object* o = objOf(uid)) sorted.push_back(o);
    std::sort(sorted.begin(), sorted.end(), [c](Object* a, Object* b) { return distanceOf(c, a) < distanceOf(c, b); });
    int team = c->critter.team;
    for (Object* o : sorted) {
        if (o == c) continue;
        if (!attackingMe && !o->isDead() && o->critter.whoHitMe == c->uid) attackingMe = o;
        if (!hittingFriend && o->critter.team == team) {
            Object* w = objOf(o->critter.whoHitMe);
            if (w && w != c && w->critter.team != team && !w->isDead()) hittingFriend = w;
        }
        if (!enemyOfTeam && o->critter.team != team && !o->isDead()) {
            Object* w = objOf(o->critter.whoHitMe);
            if (w && w->critter.team == team) enemyOfTeam = o;
        }
    }
    for (Object* o : { attackingMe, hittingFriend, enemyOfTeam })
        if (o) targets.push_back(o);
    std::sort(targets.begin(), targets.end(), [c](Object* a, Object* b) { return distanceOf(c, a) < distanceOf(c, b); });
    for (Object* t : targets)
        if (withinPerception(c, t)) return t;
    return nullptr;
}

bool wantToJoin(Object* c)
{
    if (!c || c->isHidden() || !dude() || c->elevation != dude()->elevation) return false;
    if (c->critter.results & (DAM_DEAD | DAM_KNOCKED_OUT)) return false;
    if (c->critter.damageLastTurn > 0) return true;
    if (c->script) g_scripts.exec(c->script, SP_COMBAT, nullptr, nullptr, 5);
    if (c->critter.maneuver & MAN_ENGAGING) return true;
    if (c->critter.maneuver & (MAN_DISENGAGING | MAN_FLEEING)) return false;
    return dangerSource(c) != nullptr;
}

bool wantToStop(Object* c)
{
    if (c->critter.maneuver & (MAN_DISENGAGING | MAN_FLEEING)) return true;
    if (c->critter.results & (DAM_KNOCKED_OUT | DAM_DEAD)) return true;
    Object* danger = dangerSource(c);
    if (!danger || !withinPerception(c, danger)) return true;
    return c->critter.ap == critterStat(c, STAT_MAX_AP);
}

bool shouldEnd()
{
    if (C.com <= 1) return true;
    Object* d = dude();
    if (!d || !inList(d->uid, 0, C.com)) return true;
    int team = d->critter.team;
    for (Object* c : comList()) {
        if (c->critter.team != team) return false;
        Object* w = objOf(c->critter.whoHitMe);
        if (w && w->critter.team == team) return false;
    }
    return true;
}

void endCombatNow()
{
    for (u32 uid : C.list) {
        Object* o = objOf(uid);
        if (!o) continue;
        o->critter.damageLastTurn = 0;
        o->critter.maneuver = 0;
        o->critter.ap = 0;
        o->critter.whoHitMe = 0;
        o->outline = 0;
    }
    C.active = false;
    C.phase = PH_NONE;
    C.list.clear();
    C.com = 0;
    C.turn = 0;
    C.gcsd = ScriptedAttack();
    if (Object* d = dude()) {
        d->critter.ap = critterStat(d, STAT_MAX_AP);
        if (C.exps > 0 && !d->isDead()) {
            std::string prefix = gameMsg("proto", 622 + g_rng.range(0, 3));
            std::string f = gameMsg("proto", 621);
            if (f.empty() || f == "Error") f = "%s you earn %d exp. points.";
            displayMessage(strFormat(f.c_str(), prefix.c_str(), C.exps));
            playerAddExperience(C.exps);
        }
    }
    C.exps = 0;
    g_game.uiDisabled = false;
    g_scripts.mapUpdate();
}

// Movement in combat costs action points (free bonus-move points first).
void installStepHook()
{
    g_animCombatStep = [](Object* o) -> bool {
        if (!C.active || !o) return true;
        if (o == g_world.dude && C.freeMove > 0) C.freeMove--;
        else if (o->critter.ap > 0) o->critter.ap--;
        return o->critter.ap > 0 || (o == g_world.dude && C.freeMove > 0);
    };
}

void beginCombat(Object* attacker, Object* defender)
{
    Object* d = dude();
    if (!d) return;
    installStepHook();
    animStopAll();
    C = CombatState{ true };
    C.pending = ScriptedAttack();
    C.elevation = d->elevation;
    // every critter on the elevation is listed; only the combatants take turns
    std::vector<Object*> all = g_world.critters(C.elevation);
    std::vector<Object*> front;
    for (Object* o : { attacker, defender, d })
        if (o && std::find(front.begin(), front.end(), o) == front.end()) front.push_back(o);
    for (Object* o : front) C.list.push_back(o->uid);
    C.com = (int)front.size();
    for (Object* o : all) {
        if (std::find(front.begin(), front.end(), o) != front.end()) continue;
        C.list.push_back(o->uid);
    }
    for (u32 uid : C.list) {
        Object* o = objOf(uid);
        o->critter.maneuver &= MAN_ENGAGING;
        o->critter.damageLastTurn = 0;
        o->critter.whoHitMe = 0;
        o->critter.ap = 0;
    }
    if (attacker && defender) {
        setWhoHitMe(attacker, defender);
        setWhoHitMe(defender, attacker);
    }
    C.index = 0;
    C.phase = PH_START_TURN;
    LOGI("Combat started (%d combatants, %d critters)", C.com, (int)C.list.size());
}

// Between rounds: critters that want to join are added, the dead removed, order by sequence.
void nextRound()
{
    std::vector<u32> com, non;
    for (int i = 0; i < (int)C.list.size(); i++) (i < C.com ? com : non).push_back(C.list[i]);
    for (auto it = non.begin(); it != non.end();) {
        Object* o = objOf(*it);
        if (o && wantToJoin(o)) {
            o->critter.maneuver = 0;
            com.push_back(*it);
            it = non.erase(it);
        } else {
            ++it;
        }
    }
    std::vector<u32> keep;
    for (u32 uid : com) {
        Object* o = objOf(uid);
        if (!o || o->isDead()) {
            if (o) non.push_back(uid);
            continue;
        }
        if (o != dude() && ((o->critter.results & DAM_KNOCKED_OUT) || o->critter.maneuver == MAN_DISENGAGING)) {
            o->critter.maneuver &= ~MAN_ENGAGING;
            non.push_back(uid);
            continue;
        }
        keep.push_back(uid);
    }
    std::stable_sort(keep.begin(), keep.end(), [](u32 a, u32 b) {
        Object* oa = objOf(a);
        Object* ob = objOf(b);
        int sa = critterStat(oa, STAT_SEQUENCE), sb = critterStat(ob, STAT_SEQUENCE);
        if (sa != sb) return sa > sb;
        return critterStat(oa, STAT_LK) > critterStat(ob, STAT_LK);
    });
    C.list = keep;
    C.com = (int)keep.size();
    for (u32 uid : non) C.list.push_back(uid);
    C.index = 0;
    g_game.time += 5 * kTicksPerSecond;
    queueProcess();
}

void endTurn()
{
    Object* c = objOf(C.turn);
    if (c) {
        c->critter.damageLastTurn = 0;
        c->outline = 0;
    }
    if (C.gcsd.valid && c && C.gcsd.attacker == c->uid) C.gcsd = ScriptedAttack();
    C.freeMove = 0;
    C.playerEnd = false;
    C.turn = 0;
    C.index++;
    C.phase = PH_START_TURN;
    C.settle = 0.15f;
}

void standUp(Object* c)
{
    int anim = fidAnim(c->fid) == ANIM_FALL_BACK ? ANIM_BACK_TO_STANDING : ANIM_PRONE_TO_STANDING;
    c->critter.results &= ~DAM_KNOCKED_DOWN;
    c->critter.ap = std::max(0, c->critter.ap - 3);
    animBegin(ANIMSEQ_REPLACE | ANIMSEQ_COMBAT);
    animAnimate(c, anim, 0);
    animEnd();
}

void startTurn()
{
    if (C.endRequested) {
        C.endRequested = false;
        endCombatNow();
        return;
    }
    Object* d = dude();
    if (!d || d->isDead()) {
        endCombatNow();
        return;
    }
    if (C.index >= C.com) {
        if (shouldEnd()) {
            endCombatNow();
            return;
        }
        nextRound();
        if (C.com == 0) {
            endCombatNow();
            return;
        }
    }
    Object* c = objOf(C.list[C.index]);
    if (!c || c->isDead() || c->elevation != C.elevation) {
        C.index++;
        return;
    }
    C.turn = c->uid;
    C.aiAttempts = 0;
    C.aiTaunted = false;
    if (g_combatLog) LOGI("[combat] turn %d/%d: %s (uid %u) tile %d ap %d hp %d", C.index, C.com, critterName(c).c_str(), c->uid, c->tile, critterStat(c, STAT_MAX_AP), c->critter.hp);
    if (c->critter.results & (DAM_KNOCKED_OUT | DAM_DEAD | DAM_LOSE_TURN)) {
        c->critter.results &= ~DAM_LOSE_TURN;
        endTurn();
        return;
    }
    c->critter.ap = critterStat(c, STAT_MAX_AP);
    if (c == d) C.freeMove = 2 * perkLevel(PERK_BONUS_MOVE);
    bool overrides = false;
    if (c->script) overrides = g_scripts.exec(c->script, SP_COMBAT, nullptr, nullptr, 4);
    if (!C.active) return; // the script ended combat
    if (overrides) {
        C.phase = PH_END_TURN;
        return;
    }
    if (critterIsProne(c)) standUp(c);
    if (c == d) {
        g_game.uiDisabled = false;
        C.phase = PH_PLAYER;
    } else {
        c->outline = 0xFFFF2020u;
        C.phase = PH_AI;
    }
}

// ------------------------------------------------------------------ AI
bool aiRunAway(Object* c)
{
    const AiPacket& p = packetOf(c);
    Object* d = dude();
    if (!d) return false;
    if (distanceOf(c, d) >= p.maxDist) {
        c->critter.maneuver |= MAN_DISENGAGING;
        return false;
    }
    c->critter.maneuver |= MAN_FLEEING;
    int rot = hex::direction(d->tile, c->tile);
    for (int ap = c->critter.ap; ap > 0; ap--) {
        for (int r : { rot, (rot + 1) % 6, (rot + 5) % 6 }) {
            int dest = hex::inDirection(c->tile, r, ap);
            if (!findPath(c, c->tile, dest, c->elevation, false).empty()) {
                taunt(c, TAUNT_RUN);
                animBegin(ANIMSEQ_REPLACE | ANIMSEQ_COMBAT);
                animMoveToTile(c, dest, c->elevation, c->critter.ap, true);
                animEnd();
                return true;
            }
        }
    }
    return false;
}

bool aiMoveCloser(Object* c, Object* target)
{
    if (distanceOf(c, target) <= 1 || c->critter.ap <= 0) return false;
    if (!C.aiTaunted) {
        taunt(c, TAUNT_MOVE);
        C.aiTaunted = true;
    }
    int before = c->tile;
    animBegin(ANIMSEQ_REPLACE | ANIMSEQ_COMBAT);
    animMoveToObject(c, target, c->critter.ap, false);
    if (animEnd() != 0) return false;
    (void)before;
    return true;
}

Object* aiBestWeapon(Object* c)
{
    Object* best = nullptr;
    int bestScore = -1;
    for (auto& e : c->inventory) {
        Object* w = e.obj;
        if (itemType(w) != ITEM_WEAPON) continue;
        const Proto* p = w->proto();
        if (!p) continue;
        if (p->weapon.minStrength > critterStat(c, STAT_ST) + 2) continue;
        if (weaponUsesAmmo(w) && w->ammo <= 0) {
            bool haveAmmo = false;
            for (auto& a : c->inventory)
                if (weaponCanReloadWith(w, a.obj)) haveAmmo = true;
            if (!haveAmmo) continue;
        }
        int kind = attackKindOf(weaponAttackType(w, HIT_RIGHT_PRIMARY));
        int score = p->weapon.maxDamage * 2 + p->weapon.minDamage + (kind == AK_RANGED ? 10 : 0);
        if (score > bestScore) bestScore = score, best = w;
    }
    return best;
}

int aiPickHitMode(Object* c, Object* weapon)
{
    if (!weapon) return HIT_PUNCH;
    const AiPacket& p = packetOf(c);
    int a2 = weaponAttackType(weapon, HIT_RIGHT_SECONDARY);
    if (a2 != ATTACK_NONE && g_rng.range(1, p.secondaryFreq) == 1 && weaponApCost(c, weapon, HIT_RIGHT_SECONDARY, false) <= c->critter.ap)
        return HIT_RIGHT_SECONDARY;
    return HIT_RIGHT_PRIMARY;
}

int aiCalledShot(Object* c, Object* target, int hitMode)
{
    const AiPacket& p = packetOf(c);
    if (weaponApCost(c, hitWith(c, hitMode), hitMode, true) > c->critter.ap) return HIT_TORSO;
    int a = weaponAttackType(hitWith(c, hitMode), hitMode);
    if (a == ATTACK_FIRE_BURST || a == ATTACK_FLAME || a == ATTACK_THROW) return HIT_TORSO;
    if (g_rng.range(1, p.calledFreq) != 1) return HIT_TORSO;
    int minIq = g_game.combatDifficulty == 0 ? 7 : g_game.combatDifficulty == 2 ? 3 : 5;
    if (critterStat(c, STAT_IN) < minIq) return HIT_TORSO;
    int loc = g_rng.range(0, 8);
    if (toHit(c, target, loc, hitMode, true) < p.minToHit) return HIT_TORSO;
    return loc;
}

bool aiUseDrugs(Object* c)
{
    int maxHp = critterStat(c, STAT_MAX_HP);
    if (c->critter.hp >= maxHp / 2 || c->critter.ap < 2) return false;
    for (auto& e : c->inventory) {
        const Proto* p = e.obj->proto();
        if (!p || p->itemType != ITEM_DRUG) continue;
        bool heals = false;
        for (int i = 0; i < 3; i++)
            if (p->drug.stat[i] == STAT_CURRENT_HP && p->drug.amount[i] > 0) heals = true;
        if (!heals) continue;
        Object* item = e.obj;
        if (drugTake(c, item)) {
            Object* used = g_world.removeFromInventory(c, item, 1);
            if (used) g_world.destroy(used);
            c->critter.ap -= 2;
            return true;
        }
    }
    return false;
}

// One AI decision. Returns true if an action was started (wait for animations), false to end the turn.
bool aiStep(Object* c)
{
    if (++C.aiAttempts > 10) return false;
    if (c->critter.results & (DAM_KNOCKED_OUT | DAM_DEAD | DAM_LOSE_TURN)) return false;
    const AiPacket& p = packetOf(c);
    Object* target = nullptr;
    if (C.gcsd.valid && C.gcsd.attacker == c->uid) target = objOf(C.gcsd.target);
    if ((c->critter.maneuver & MAN_FLEEING) || (c->critter.results & p.hurtTooMuch) || c->critter.hp < p.minHp) {
        if (C.aiAttempts == 1) aiRunAway(c);
        return false;
    }
    if (!target) {
        if (C.aiAttempts == 1 && aiUseDrugs(c)) return true;
        target = dangerSource(c);
    }
    if (!target || target->isDead()) {
        Object* w = objOf(c->critter.whoHitMe);
        if (w && !w->isDead() && c->critter.damageLastTurn > 0 && C.aiAttempts == 1) aiRunAway(c);
        return false;
    }
    setWhoHitMe(c, target);
    Object* weapon = itemRightHand(c);
    if (weapon && itemType(weapon) != ITEM_WEAPON) weapon = nullptr;
    if (!weapon) {
        if (Object* best = aiBestWeapon(c)) {
            itemEquip(c, best, 1);
            critterUpdateArt(c);
            weapon = best;
        }
    }
    int hitMode = aiPickHitMode(c, weapon);
    int bad = checkBadShot(c, target, hitMode, false);
    if (g_combatLog)
        LOGI("[combat]   ai %s -> %s dist %d ap %d weapon %s bad %d", critterName(c).c_str(), critterName(target).c_str(), distanceOf(c, target),
            c->critter.ap, weapon ? itemName(weapon).c_str() : "none", bad);
    switch (bad) {
    case BS_OK: {
        int chance = toHit(c, target, HIT_UNCALLED, hitMode, true);
        if (chance >= p.minToHit) {
            if (c->critter.maneuver & MAN_FLEEING) return false;
            int loc = aiCalledShot(c, target, hitMode);
            return doAttack(c, target, hitMode, loc);
        }
        if (toHit(c, target, HIT_UNCALLED, hitMode, false) < p.minToHit) {
            aiRunAway(c);
            return false;
        }
        if (!aiMoveCloser(c, target)) {
            aiRunAway(c);
            return false;
        }
        return true;
    }
    case BS_NO_AMMO: {
        if (weapon && c->critter.ap >= 2 && weaponReloadFromInventory(c, weapon) > 0) {
            soundPlaySfxAt(weaponSfx('r', weapon, hitMode, nullptr), c);
            c->critter.ap -= 2;
            return true;
        }
        if (weapon) {
            itemUnequip(c, weapon);
            critterUpdateArt(c);
        }
        return c->critter.ap > 0;
    }
    case BS_OUT_OF_RANGE:
        if (toHit(c, target, HIT_UNCALLED, hitMode, false) < p.minToHit) {
            aiRunAway(c);
            return false;
        }
        return aiMoveCloser(c, target);
    case BS_AIM_BLOCKED: return aiMoveCloser(c, target);
    case BS_NOT_ENOUGH_AP:
    case BS_ARM_CRIPPLED:
    case BS_BOTH_ARMS_CRIPPLED:
    default: return false;
    }
}

void startScripted()
{
    ScriptedAttack s = C.pending;
    C.pending = ScriptedAttack();
    Object* a = objOf(s.attacker);
    Object* t = objOf(s.target);
    if (!a || !t || !critterIsActive(a) || !critterIsActive(t)) return;
    beginCombat(a, t);
    C.gcsd = s;
    C.gcsd.valid = true;
}

bool anyCombatAnim() { return animBusyAny(true); }

} // namespace

// ------------------------------------------------------------------ public API
bool g_combatLog = false;

bool combatActive() { return C.active; }

Object* combatWhoseTurn() { return C.active ? objOf(C.turn) : nullptr; }

void combatStart(Object* attacker, Object* target)
{
    if (C.active) {
        // join an already running combat
        for (Object* o : { attacker, target }) {
            if (!o) continue;
            auto it = std::find(C.list.begin(), C.list.end(), o->uid);
            if (it == C.list.end()) C.list.push_back(o->uid);
            o->critter.maneuver |= MAN_ENGAGING;
        }
        if (attacker && target) setWhoHitMe(target, attacker);
        return;
    }
    if (g_dialog.active()) return;
    beginCombat(attacker, target);
    // the player starting combat gets the first turn right away
    if (attacker == g_world.dude) startTurn();
}

bool combatEnd(bool force)
{
    if (!C.active) {
        if (force) C.pending = ScriptedAttack();
        return true;
    }
    if (force) {
        endCombatNow();
        return true;
    }
    return combatTryEnd();
}

bool combatTryEnd()
{
    if (!C.active) return true;
    Object* d = dude();
    if (!d) return true;
    int team = d->critter.team;
    for (int i = 0; i < (int)C.list.size(); i++) {
        Object* o = objOf(C.list[i]);
        if (!o || o == d || o->isDead()) continue;
        Object* w = objOf(o->critter.whoHitMe);
        bool hostile = o->critter.team != team || (w && w->critter.team == o->critter.team);
        if (!hostile) continue;
        bool blocks = i < C.com ? !wantToStop(o) : wantToJoin(o);
        if (blocks && w) {
            displayMessage(combatText(519, "Combat cannot end with nearby hostile creatures."));
            return false;
        }
    }
    C.endRequested = true;
    if (combatIsPlayerTurn()) C.playerEnd = true;
    return true;
}

void combatEndPlayerTurn()
{
    if (combatIsPlayerTurn()) C.playerEnd = true;
}

bool combatIsPlayerTurn() { return C.active && C.phase == PH_PLAYER && g_world.dude && C.turn == g_world.dude->uid; }

int combatFreeMove() { return C.active ? C.freeMove : 0; }

bool combatUseAp(Object* critter, int ap)
{
    if (!C.active || !critter) return true;
    if (critter->critter.ap >= ap) {
        critter->critter.ap -= ap;
        return true;
    }
    if (critter == g_world.dude) displayMessage(gameMsg("proto", 700).empty() || gameMsg("proto", 700) == "Error" ? "You don't have enough action points." : gameMsg("proto", 700));
    return false;
}

int combatToHit(Object* attacker, int tile, Object* target, int hitMode, int location, bool checkRange)
{
    (void)tile;
    if (!attacker || !target) return 0;
    return toHit(attacker, target, location, hitMode, checkRange);
}

bool combatAttack(Object* attacker, Object* target, int hitMode, int location)
{
    if (!attacker || !target || attacker == target) return false;
    if (!target->isCritter()) return false;
    if (anyCombatAnim() && attacker == g_world.dude) return false;
    if (!C.active) {
        combatStart(attacker, target);
        if (!C.active) return false;
    }
    if (attacker == g_world.dude && !combatIsPlayerTurn()) return false;
    if (attacker != g_world.dude) target->critter.maneuver |= MAN_ENGAGING;
    return doAttack(attacker, target, hitMode, location);
}

void combatScriptAttack(Object* attacker, Object* target, int calledShot, int numAttacks, int bonus, int minDmg, int maxDmg, int attackerResults,
    int targetResults)
{
    if (!attacker || !target) return;
    if (C.active) {
        if (!(attacker->critter.maneuver & MAN_ENGAGING)) {
            attacker->critter.maneuver |= MAN_ENGAGING;
            attacker->critter.whoHitMe = target->uid;
        }
        if (std::find(C.list.begin(), C.list.end(), attacker->uid) == C.list.end()) C.list.push_back(attacker->uid);
        return;
    }
    ScriptedAttack s;
    s.valid = true;
    s.attacker = attacker->uid;
    s.target = target->uid;
    s.calledShot = calledShot;
    s.numAttacks = numAttacks;
    s.bonus = bonus;
    s.minDmg = minDmg;
    s.maxDmg = maxDmg;
    s.attackerResults = attackerResults;
    s.targetResults = targetResults;
    C.pending = s;
}

void combatUpdate(float dt)
{
    if (!C.active) {
        if (C.pending.valid && !g_dialog.active() && g_scripts.current() == nullptr) startScripted();
        if (!C.active) return;
    }
    Object* d = dude();
    if (!d) {
        endCombatNow();
        return;
    }
    if (C.settle > 0) {
        C.settle -= dt;
        return;
    }
    switch (C.phase) {
    case PH_START_TURN:
        if (anyCombatAnim()) return;
        startTurn();
        break;
    case PH_PLAYER: {
        bool out = d->critter.ap <= 0 && C.freeMove <= 0;
        if (d->isDead() || (d->critter.results & (DAM_KNOCKED_OUT | DAM_LOSE_TURN))) out = true;
        if ((C.playerEnd || out) && !anyCombatAnim() && !animBusy(d)) {
            g_game.uiDisabled = true;
            endTurn();
        }
        break;
    }
    case PH_AI: {
        Object* c = objOf(C.turn);
        if (!c || c->isDead()) {
            endTurn();
            break;
        }
        if (anyCombatAnim() || animBusy(c)) return;
        if (!aiStep(c)) C.phase = PH_END_TURN;
        break;
    }
    case PH_END_TURN:
        if (anyCombatAnim()) return;
        endTurn();
        break;
    default: break;
    }
}

void combatExplosion(int tile, int elevation, int minDmg, int maxDmg, Object* source)
{
    (void)source;
    if (!hex::valid(tile)) return;
    soundPlaySfx("wae1xxx1");
    for (Object* o : g_world.objectsInRadius(tile, elevation, 2)) {
        if (!o->isCritter() || o->isDead()) continue;
        int dmg = g_rng.range(minDmg, std::max(minDmg, maxDmg));
        int dr = critterStat(o, STAT_DR_EXPLOSION), dtv = critterStat(o, STAT_DT_EXPLOSION);
        dmg = std::max(0, dmg - dtv);
        dmg -= dmg * dr / 100;
        critterApplyDamage(o, dmg, nullptr, 0, DMG_EXPLOSION);
    }
}

void critterApplyDamage(Object* target, int amount, Object* source, u32 resultFlags, int damageType)
{
    if (!target || !target->isCritter() || target->isDead() || (u32)target->pid == PID_IMMORTAL) return;
    (void)damageType;
    u32 flags = resultFlags & ~DAM_BYPASS;
    if (amount > 0 && target->critter.hp - amount <= 0) flags |= DAM_DEAD;
    if (source) setWhoHitMe(target, source);
    setResults(target, flags);
    damageObject(target, amount, true);
    if (target == g_world.dude) displayMessage(strFormat(combatText(516, "You were hit for %d hit points").c_str(), amount) + flagsText(flags) + ".");
    animBegin(ANIMSEQ_COMBAT);
    addReaction(target, amount, flags, nullptr, true, 0, 0, ANIM_THROW_PUNCH, source ? source : target, 0);
    animEnd();
}

void critterKill(Object* critter, int deathAnim, bool animate)
{
    if (!critter || !critter->isCritter() || critter->isDead()) return;
    critter->critter.results |= DAM_DEAD;
    critter->critter.hp = 0;
    queueRemove(critter, EV_KNOCKOUT);
    if (animate && deathAnim >= ANIM_FIRST_DEATH && deathAnim <= ANIM_LAST_DEATH && animArtExists(critter, deathAnim)) {
        u32 cu = critter->uid;
        animBegin(ANIMSEQ_REPLACE | ANIMSEQ_NO_STAND);
        animPlaySfx(critter, critterSfx(critter, deathAnim, 2), 0);
        animAnimate(critter, deathAnim, 0);
        animCallback(critter, [cu, deathAnim] {
            if (Object* o = g_world.byUid(cu)) finishCorpse(o, deathAnim);
        }, -1, true);
        animEnd();
        return;
    }
    int anim = deathAnim;
    if (anim < 0 || anim > ANIM_LAST_SF_DEATH) anim = ANIM_FALL_BACK_SF;
    if (anim < ANIM_FIRST_SF_DEATH) anim = anim - ANIM_FIRST_DEATH + ANIM_FIRST_SF_DEATH;
    if (g_game.violenceLevel < 3 && (anim == ANIM_BIG_HOLE_SF || anim == ANIM_CHUNKS_OF_FLESH_SF || anim == ANIM_EXPLODED_TO_NOTHING_SF)) anim = ANIM_FALL_BACK_SF;
    if (!animArtExists(critter, anim)) anim = ANIM_FALL_BACK_SF;
    animStop(critter);
    critter->fid = animFid(critter, anim);
    critter->frame = 0;
    critter->offX = critter->offY = 0;
    finishCorpse(critter, anim);
}

void critterKnockOut(Object* critter, int turns)
{
    if (!critter || !critter->isCritter() || critter->isDead()) return;
    critter->critter.results |= DAM_KNOCKED_OUT | DAM_KNOCKED_DOWN;
    queueRemove(critter, EV_KNOCKOUT);
    queueAdd(std::max(1, turns) * 50, critter, EV_KNOCKOUT);
    animBegin(ANIMSEQ_REPLACE | ANIMSEQ_NO_STAND);
    animAnimate(critter, ANIM_FALL_BACK, 0);
    animEnd();
}

void critterWakeUp(Object* critter)
{
    if (!critter || !critter->isCritter() || critter->isDead()) return;
    critter->critter.results &= ~DAM_KNOCKED_OUT;
    if (critter == g_world.dude) displayMessage("You regain consciousness.");
    if (!inCombat()) {
        int anim = fidAnim(critter->fid) == ANIM_FALL_BACK ? ANIM_BACK_TO_STANDING : ANIM_PRONE_TO_STANDING;
        critter->critter.results &= ~DAM_KNOCKED_DOWN;
        animBegin(ANIMSEQ_REPLACE);
        animAnimate(critter, anim, 0);
        animEnd();
    }
}

bool critterIsProne(Object* c) { return c && (c->critter.results & (DAM_DEAD | DAM_KNOCKED_DOWN | DAM_KNOCKED_OUT)) != 0; }

std::string combatMsg(int num) { return gameMsg("combat", num); }


} // namespace fo
