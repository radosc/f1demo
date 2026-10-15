#include "game/critter.h"
#include "game/game.h"
#include "game/item.h"
#include "game/msg.h"
#include "game/perks.h"
#include "game/proto.h"

namespace fo {

struct StatRange { int minV, maxV; };
static const StatRange kStatRanges[STAT_COUNT] = {
    { 1, 10 }, { 1, 10 }, { 1, 10 }, { 1, 10 }, { 1, 10 }, { 1, 10 }, { 1, 10 },
    { 0, INT32_MAX }, { 0, INT32_MAX }, { 0, INT32_MAX }, { 0, INT32_MAX }, { 0, INT32_MAX }, { 0, INT32_MAX },
    { 0, INT32_MAX }, { 0, INT32_MAX }, { 0, 100 }, { -60, 100 },
    { 0, 100 }, { 0, 100 }, { 0, 100 }, { 0, 100 }, { 0, 100 }, { 0, 100 }, { 0, 100 },
    { 0, 90 }, { 0, 90 }, { 0, 90 }, { 0, 90 }, { 0, 90 }, { 0, 100 }, { 0, 90 },
    { 0, 100 }, { 0, 100 }, { 16, 35 }, { 0, 1 },
    { 0, 2000 }, { 0, 2000 }, { 0, 2000 },
};

int statMin(int stat) { return (stat >= 0 && stat < STAT_COUNT) ? kStatRanges[stat].minV : 0; }
int statMax(int stat) { return (stat >= 0 && stat < STAT_COUNT) ? kStatRanges[stat].maxV : 0; }

Proto* critterProto(Object* c)
{
    if (!c) return nullptr;
    return g_protos.getMutable(c->pid);
}

bool skillTagged(int skill)
{
    for (int t : g_game.player.tags)
        if (t == skill && skill >= 0) return true;
    return false;
}

bool hasTrait(int trait)
{
    return trait >= 0 && (g_game.player.traits[0] == trait || g_game.player.traits[1] == trait);
}

int perkLevel(int perk) { return (perk >= 0 && perk < PERK_COUNT) ? g_game.player.perks[perk] : 0; }

int critterBaseStatDirect(Object* c, int stat)
{
    Proto* p = critterProto(c);
    if (!p || stat < 0 || stat >= STAT_SAVEABLE_COUNT) return 0;
    return p->baseStats[stat];
}

static int traitAdjustStat(int stat)
{
    Object* dude = g_world.dude;
    int m = 0;
    bool night = gameHour() < 600 || gameHour() >= 1800;
    switch (stat) {
    case STAT_ST:
        if (hasTrait(TRAIT_GIFTED)) m += 1;
        if (hasTrait(TRAIT_BRUISER)) m += 2;
        break;
    case STAT_PE:
        if (hasTrait(TRAIT_GIFTED)) m += 1;
        if (hasTrait(TRAIT_NIGHT_PERSON)) m += night ? 1 : -1;
        break;
    case STAT_EN:
    case STAT_CH:
    case STAT_LK:
        if (hasTrait(TRAIT_GIFTED)) m += 1;
        break;
    case STAT_IN:
        if (hasTrait(TRAIT_GIFTED)) m += 1;
        if (hasTrait(TRAIT_NIGHT_PERSON)) m += night ? 1 : -1;
        break;
    case STAT_AG:
        if (hasTrait(TRAIT_GIFTED)) m += 1;
        if (hasTrait(TRAIT_SMALL_FRAME)) m += 1;
        break;
    case STAT_MAX_AP:
        if (hasTrait(TRAIT_BRUISER)) m -= 2;
        break;
    case STAT_AC:
        if (hasTrait(TRAIT_KAMIKAZE)) m -= critterBaseStatDirect(dude, STAT_AC);
        break;
    case STAT_MELEE_DMG:
        if (hasTrait(TRAIT_HEAVY_HANDED)) m += 4;
        break;
    case STAT_CARRY_WEIGHT:
        if (hasTrait(TRAIT_SMALL_FRAME)) m -= 10 * critterBaseStatDirect(dude, STAT_ST);
        break;
    case STAT_SEQUENCE:
        if (hasTrait(TRAIT_KAMIKAZE)) m += 5;
        break;
    case STAT_HEAL_RATE:
        if (hasTrait(TRAIT_FAST_METABOLISM)) m += 2;
        break;
    case STAT_CRIT_CHANCE:
        if (hasTrait(TRAIT_FINESSE)) m += 10;
        break;
    case STAT_BETTER_CRITS:
        if (hasTrait(TRAIT_HEAVY_HANDED)) m -= 30;
        break;
    case STAT_RAD_RESIST:
        if (hasTrait(TRAIT_FAST_METABOLISM)) m -= critterBaseStatDirect(dude, STAT_RAD_RESIST);
        break;
    case STAT_POISON_RESIST:
        if (hasTrait(TRAIT_FAST_METABOLISM)) m -= critterBaseStatDirect(dude, STAT_POISON_RESIST);
        break;
    }
    return m;
}

int critterBaseStat(Object* c, int stat)
{
    int v = critterBaseStatDirect(c, stat);
    if (c && c == g_world.dude) v += traitAdjustStat(stat);
    return v;
}

int critterBonusStat(Object* c, int stat)
{
    Proto* p = critterProto(c);
    if (!p || stat < 0 || stat >= STAT_SAVEABLE_COUNT) return 0;
    return p->bonusStats[stat];
}

void critterSetBaseStat(Object* c, int stat, int value)
{
    Proto* p = critterProto(c);
    if (!p || stat < 0 || stat >= STAT_SAVEABLE_COUNT) return;
    p->baseStats[stat] = value;
    if (stat < STAT_PRIMARY_COUNT) critterRecalcDerived(c);
}

void critterSetBonusStat(Object* c, int stat, int value)
{
    Proto* p = critterProto(c);
    if (!p || stat < 0 || stat >= STAT_SAVEABLE_COUNT) return;
    p->bonusStats[stat] = value;
}

void critterRecalcDerived(Object* c)
{
    Proto* p = critterProto(c);
    if (!p) return;
    int st = critterStat(c, STAT_ST), pe = critterStat(c, STAT_PE), en = critterStat(c, STAT_EN);
    int ag = critterStat(c, STAT_AG), lk = critterStat(c, STAT_LK);
    p->baseStats[STAT_MAX_HP] = critterBaseStat(c, STAT_ST) + critterBaseStat(c, STAT_EN) * 2 + 15;
    p->baseStats[STAT_MAX_AP] = ag / 2 + 5;
    p->baseStats[STAT_AC] = ag;
    p->baseStats[STAT_MELEE_DMG] = std::max(st - 5, 1);
    p->baseStats[STAT_CARRY_WEIGHT] = 25 * st + 25;
    p->baseStats[STAT_SEQUENCE] = 2 * pe;
    p->baseStats[STAT_HEAL_RATE] = std::max(en / 3, 1);
    p->baseStats[STAT_CRIT_CHANCE] = lk;
    p->baseStats[STAT_BETTER_CRITS] = 0;
    p->baseStats[STAT_RAD_RESIST] = 2 * en;
    p->baseStats[STAT_POISON_RESIST] = 5 * en;
}

int critterStat(Object* c, int stat)
{
    if (!c) return 0;
    if (stat >= 0 && stat < STAT_SAVEABLE_COUNT) {
        int v = critterBaseStat(c, stat) + critterBonusStat(c, stat);
        switch (stat) {
        case STAT_PE:
            if (c->critter.results & DAM_BLIND) v -= 5;
            break;
        case STAT_AC:
            if (inCombat() && combatTurnObject() != c) v += c->critter.ap;
            if (Object* armor = itemWorn(c)) {
                if (const Proto* ap = armor->proto()) v += ap->armor.ac;
            }
            break;
        case STAT_AGE:
            v += gameTime() / (365 * kTicksPerDay);
            break;
        default:
            if (stat >= STAT_DT_NORMAL && stat <= STAT_DR_EXPLOSION) {
                if (Object* armor = itemWorn(c)) {
                    if (const Proto* ap = armor->proto()) {
                        if (stat <= STAT_DT_EXPLOSION) v += ap->armor.dt[stat - STAT_DT_NORMAL];
                        else v += ap->armor.dr[stat - STAT_DR_NORMAL];
                    }
                }
            }
            break;
        }
        if (c == g_world.dude) v += perkStatBonus(stat); // rules/perks.json
        return clampv(v, kStatRanges[stat].minV, kStatRanges[stat].maxV);
    }
    switch (stat) {
    case STAT_CURRENT_HP: return c->critter.hp;
    case STAT_CURRENT_POISON: return c->critter.poison;
    case STAT_CURRENT_RAD: return c->critter.radiation;
    default: return 0;
    }
}

bool critterStatRoll(Object* c, int stat, int modifier, int* howMuch)
{
    int value = critterStat(c, stat) + modifier;
    int roll = g_rng.range(1, 10);
    if (howMuch) *howMuch = value - roll;
    return roll <= value;
}

// ------------------------------------------------------------------ skills
struct SkillInfo {
    int base;
    int statMod;
    int stat1, stat2;
    int pointsMod;
};
static const SkillInfo kSkills[SKILL_COUNT] = {
    { 35, 1, STAT_AG, -1, 1 }, { 10, 1, STAT_AG, -1, 1 }, { 10, 1, STAT_AG, -1, 1 },
    { 65, 1, STAT_AG, STAT_ST, 1 }, { 55, 1, STAT_AG, STAT_ST, 1 }, { 40, 1, STAT_AG, -1, 1 },
    { 30, 1, STAT_PE, STAT_IN, 1 }, { 15, 1, STAT_PE, STAT_IN, 1 }, { 25, 1, STAT_AG, -1, 1 },
    { 20, 1, STAT_PE, STAT_AG, 1 }, { 20, 1, STAT_AG, -1, 1 }, { 20, 1, STAT_PE, STAT_AG, 1 },
    { 25, 2, STAT_IN, -1, 1 }, { 20, 1, STAT_IN, -1, 1 }, { 25, 2, STAT_CH, -1, 1 },
    { 20, 2, STAT_CH, -1, 1 }, { 20, 3, STAT_LK, -1, 1 }, { 5, 1, STAT_EN, STAT_IN, 1 },
};

int skillBaseValue(int skill) { return (skill >= 0 && skill < SKILL_COUNT) ? kSkills[skill].base : 0; }

int critterSkillPoints(Object* c, int skill)
{
    Proto* p = critterProto(c);
    if (!p || skill < 0 || skill >= SKILL_COUNT) return 0;
    return p->skills[skill];
}

void critterSetSkillPoints(Object* c, int skill, int points)
{
    Proto* p = critterProto(c);
    if (!p || skill < 0 || skill >= SKILL_COUNT) return;
    p->skills[skill] = std::max(0, points);
}

static int traitAdjustSkill(int skill)
{
    int m = 0;
    if (hasTrait(TRAIT_GIFTED)) m -= 10;
    if (hasTrait(TRAIT_SKILLED)) m += 10;
    if (hasTrait(TRAIT_GOOD_NATURED)) {
        if (skill <= SKILL_THROWING) m -= 10;
        else if (skill == SKILL_FIRST_AID || skill == SKILL_DOCTOR || skill == SKILL_SPEECH || skill == SKILL_BARTER) m += 15;
    }
    return m;
}

static int perkAdjustSkill(int skill)
{
    int m = 0;
    switch (skill) {
    case SKILL_FIRST_AID:
    case SKILL_DOCTOR: m += perkLevel(PERK_HEALER) * 0; break; // healer affects healed amount, not skill
    case SKILL_SCIENCE:
    case SKILL_REPAIR: break;
    case SKILL_OUTDOORSMAN: m += perkLevel(PERK_SURVIVALIST) * 25; break;
    case SKILL_BARTER: m += perkLevel(PERK_MASTER_TRADER) * 25; break;
    case SKILL_SNEAK: break;
    default: break;
    }
    return m;
}

int critterSkill(Object* c, int skill)
{
    if (!c || skill < 0 || skill >= SKILL_COUNT) return -5;
    int points = critterSkillPoints(c, skill);
    const SkillInfo& s = kSkills[skill];
    int bonus;
    if (s.stat2 >= 0) bonus = (critterStat(c, s.stat1) + critterStat(c, s.stat2)) * s.statMod / 2;
    else bonus = critterStat(c, s.stat1) * s.statMod;
    int v = s.base + bonus + points * s.pointsMod;
    if (c == g_world.dude) {
        if (skillTagged(skill)) v += 20 + points * s.pointsMod;
        v += traitAdjustSkill(skill);
        v += perkAdjustSkill(skill);
        if (skill >= SKILL_FIRST_AID) {
            if (g_game.difficulty == 0) v += 20;
            else if (g_game.difficulty == 2) v -= 10;
        }
    }
    return std::min(v, 300);
}

int skillCostToRaise(Object* c, int skill)
{
    (void)c;
    (void)skill;
    return 1; // Fallout 1: each skill point raises a skill by 1% (tagged by 2%)
}

// ------------------------------------------------------------------ hit points & misc
int critterMaxHp(Object* c) { return critterStat(c, STAT_MAX_HP); }

void critterAdjustHp(Object* c, int delta)
{
    if (!c || !c->isCritter()) return;
    c->critter.hp = clampv(c->critter.hp + delta, -9999, critterMaxHp(c));
}

int critterAC(Object* c) { return critterStat(c, STAT_AC); }
int critterDT(Object* c, int dt) { return critterStat(c, STAT_DT_NORMAL + clampv(dt, 0, 6)); }
int critterDR(Object* c, int dt) { return critterStat(c, STAT_DR_NORMAL + clampv(dt, 0, 6)); }

int critterGender(Object* c) { return critterStat(c, STAT_GENDER); }

std::string critterName(Object* c)
{
    if (!c) return "";
    if (c == g_world.dude) return g_game.player.name;
    return g_protos.name(c->pid);
}

bool critterIsActive(Object* c)
{
    return c && c->isCritter() && !(c->critter.results & (DAM_DEAD | DAM_KNOCKED_OUT | DAM_LOSE_TURN)) && !c->destroyed;
}

bool critterIsHostileTo(Object* a, Object* b)
{
    if (!a || !b || a == b) return false;
    if (a->critter.team == b->critter.team) return false;
    return a->critter.whoHitMe == b->uid || b->critter.whoHitMe == a->uid;
}

// ------------------------------------------------------------------ experience
int expForLevel(int level)
{
    static const int kTable[21] = { 0, 1, 3, 6, 10, 15, 21, 28, 36, 45, 55, 66, 78, 91, 105, 120, 136, 153, 171, 190, 210 };
    int idx = level - 1;
    if (idx < 0) return 0;
    if (idx >= 21) return INT32_MAX;
    return 1000 * kTable[idx];
}

int playerSkillPointsPerLevel()
{
    Object* d = g_world.dude;
    int pts = critterBaseStat(d, STAT_IN) * 2 + 5;
    if (hasTrait(TRAIT_SKILLED)) pts += 5;
    if (perkLevel(PERK_EDUCATED)) pts += perkLevel(PERK_EDUCATED) * 2;
    return pts;
}

void playerAddExperience(int xp)
{
    PlayerData& pl = g_game.player;
    pl.experience = std::max(0, pl.experience + xp);
    while (pl.level < 21 && pl.experience >= expForLevel(pl.level + 1)) {
        pl.level++;
        displayMessage(gameMsg("stat", 600));
        Object* d = g_world.dude;
        int hp = critterBaseStat(d, STAT_EN) / 2 + 2;
        critterSetBonusStat(d, STAT_MAX_HP, critterBonusStat(d, STAT_MAX_HP) + hp);
        critterAdjustHp(d, hp);
        pl.unspentSkillPoints += playerSkillPointsPerLevel();
        pl.levelUpPending = true;
        if (perkLevelGrants(pl.level)) pl.freePerks++;
    }
}

} // namespace fo
