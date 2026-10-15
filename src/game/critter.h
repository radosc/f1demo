// Critter rules: SPECIAL, derived stats, skills, traits, perks, hit points, experience.
#pragma once
#include "game/object.h"

namespace fo {

int critterStat(Object* c, int stat);          // effective value (stat_level)
int critterBaseStat(Object* c, int stat);      // base incl. trait adjustments
int critterBaseStatDirect(Object* c, int stat);
int critterBonusStat(Object* c, int stat);
void critterSetBaseStat(Object* c, int stat, int value);
void critterSetBonusStat(Object* c, int stat, int value);
void critterRecalcDerived(Object* c);
int statMin(int stat);
int statMax(int stat);
// d10 roll against a primary stat
bool critterStatRoll(Object* c, int stat, int modifier, int* howMuch);

int critterSkill(Object* c, int skill);
int critterSkillPoints(Object* c, int skill);
void critterSetSkillPoints(Object* c, int skill, int points);
int skillBaseValue(int skill);
int skillCostToRaise(Object* c, int skill); // skill points per +1 (tagged skills rise twice as fast)

bool skillTagged(int skill);
bool hasTrait(int trait);
int perkLevel(int perk);

int critterMaxHp(Object* c);
void critterAdjustHp(Object* c, int delta);
int critterAC(Object* c);
int critterDT(Object* c, int damageType);
int critterDR(Object* c, int damageType);

int critterGender(Object* c);
std::string critterName(Object* c);
bool critterIsActive(Object* c);   // alive and not knocked out
bool critterIsHostileTo(Object* a, Object* b);

// Player progression
int expForLevel(int level);        // experience needed to reach `level`
void playerAddExperience(int xp);
int playerSkillPointsPerLevel();

// Mutable proto for the critter (player has a unique proto).
Proto* critterProto(Object* c);

} // namespace fo
