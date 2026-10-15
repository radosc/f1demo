#include "game/perks.h"
#include "core/fs.h"
#include "core/json.h"
#include "game/critter.h"
#include "game/game.h"
#include "game/msg.h"

namespace fo {

namespace {

PerkRule g_rules[PERK_COUNT];
bool g_loaded = false;

int statByName(const Json& j)
{
    if (j.isNumber()) return j.toInt(-1);
    static const std::pair<const char*, int> kNames[] = { { "maxHitPoints", STAT_MAX_HP }, { "actionPoints", STAT_MAX_AP },
        { "armorClass", STAT_AC }, { "meleeDamage", STAT_MELEE_DMG }, { "carryWeight", STAT_CARRY_WEIGHT },
        { "sequence", STAT_SEQUENCE }, { "healingRate", STAT_HEAL_RATE }, { "criticalChance", STAT_CRIT_CHANCE },
        { "damageResistance", STAT_DR_NORMAL }, { "radiationResistance", STAT_RAD_RESIST },
        { "poisonResistance", STAT_POISON_RESIST } };
    for (auto& n : kNames)
        if (strIEquals(j.str(), n.first)) return n.second;
    return -1;
}

int skillByName(const std::string& name)
{
    static const char* const kNames[SKILL_COUNT] = { "smallGuns", "bigGuns", "energyWeapons", "unarmed", "meleeWeapons", "throwing",
        "firstAid", "doctor", "sneak", "lockpick", "steal", "traps", "science", "repair", "speech", "barter", "gambling", "outdoorsman" };
    for (int i = 0; i < SKILL_COUNT; i++)
        if (strIEquals(name, kNames[i])) return i;
    return -1;
}

void load()
{
    if (g_loaded) return;
    g_loaded = true;
    std::string text;
    if (!g_vfs.readText("rules/perks.json", text)) {
        LOGW("rules/perks.json missing - no perks are offered at level ups");
        return;
    }
    std::string err;
    Json j = Json::parse(text, &err);
    if (!err.empty()) {
        LOGW("rules/perks.json: %s", err.c_str());
        return;
    }
    static const char* const kSpecial[7] = { "ST", "PE", "EN", "CH", "IN", "AG", "LK" };
    for (auto& p : j["perks"].items()) {
        int id = p["id"].toInt(-1);
        if (id < 0 || id >= PERK_COUNT) continue;
        PerkRule& r = g_rules[id];
        r.ranks = std::max(0, p["ranks"].toInt(0));
        r.level = p["level"].toInt(1);
        for (int s = 0; s < 7; s++) r.special[s] = p["requires"][kSpecial[s]].toInt(0);
        if (p.has("skill")) {
            r.skill = skillByName(p["skill"]["name"].str());
            r.skillValue = p["skill"]["value"].toInt(0);
        }
        if (p.has("bonus")) {
            r.bonusStat = statByName(p["bonus"]["stat"]);
            r.bonusPerRank = p["bonus"]["perRank"].toInt(0);
        }
        r.name = p["name"].str();
        r.description = p["description"].str();
    }
}

} // namespace

const PerkRule& perkRule(int perk)
{
    static const PerkRule kNone;
    load();
    return perk >= 0 && perk < PERK_COUNT ? g_rules[perk] : kNone;
}

bool perkAvailable(int perk)
{
    const PerkRule& r = perkRule(perk);
    Object* d = g_world.dude;
    if (!d || r.ranks <= 0 || perkLevel(perk) >= r.ranks || g_game.player.level < r.level) return false;
    for (int s = 0; s < 7; s++)
        if (critterStat(d, s) < r.special[s]) return false;
    return r.skill < 0 || critterSkill(d, r.skill) >= r.skillValue;
}

int perkStatBonus(int stat)
{
    load();
    int v = 0;
    for (int p = 0; p < PERK_COUNT; p++)
        if (g_rules[p].bonusStat == stat) v += g_rules[p].bonusPerRank * perkLevel(p);
    return v;
}

bool perkLevelGrants(int level) { return level > 1 && level % (hasTrait(TRAIT_SKILLED) ? 4 : 3) == 0; }

std::string perkDescription(int perk)
{
    const PerkRule& r = perkRule(perk);
    if (!r.description.empty()) return r.description;
    return gameMsg("perk", 201 + perk);
}

} // namespace fo
