// Perk rules from data/rules/perks.json: ranks, requirements, stat bonuses and descriptions.
#pragma once
#include "game/defs.h"

namespace fo {

struct PerkRule {
    int ranks = 0;       // 0: never offered
    int level = 1;       // lowest character level
    int special[7] = {}; // lowest ST..LK
    int skill = -1, skillValue = 0;
    int bonusStat = -1, bonusPerRank = 0;
    std::string name;        // empty: perk.msg
    std::string description; // empty: perk.msg
};

const PerkRule& perkRule(int perk);
// Whether the player can take (another rank of) the perk now.
bool perkAvailable(int perk);
// Raise of a derived stat from the player's perks.
int perkStatBonus(int stat);
// New levels that bring a perk: every 3rd, every 4th with the Skilled trait.
bool perkLevelGrants(int level);
// perks.json description, else the perk.msg text (the demo's perk.msg only has placeholder names).
std::string perkDescription(int perk);

} // namespace fo
