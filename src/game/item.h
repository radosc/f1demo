// Item helpers: equipment slots, weights, weapons and ammo.
#pragma once
#include "game/object.h"

namespace fo {

constexpr int PID_CAPS = 0x29;        // Bag of Bottle Caps (demo data)

Object* itemWorn(Object* critter);
Object* itemRightHand(Object* critter);
Object* itemLeftHand(Object* critter);
int itemType(const Object* item);
const Proto* itemProto(const Object* item);
std::string itemName(const Object* item);
std::string itemDescription(const Object* item);

int itemWeight(const Object* item);          // single item including contents
int inventoryWeight(const Object* container);
int itemCost(const Object* item);
int itemSize(const Object* item);
int critterCarryLimit(Object* critter);

// Equipment. `slot`: 0 = left hand, 1 = right hand, 2 = armor.
bool itemEquip(Object* critter, Object* item, int slot);
void itemUnequip(Object* critter, Object* item);
void critterUpdateArt(Object* critter);   // armor/weapon dependent fid

// Weapons
enum HitMode : int {
    HIT_LEFT_PRIMARY = 0, HIT_LEFT_SECONDARY, HIT_RIGHT_PRIMARY, HIT_RIGHT_SECONDARY, HIT_PUNCH, HIT_KICK, HIT_COUNT
};
int weaponAttackType(const Object* weapon, int hitMode);   // ATTACK_*
int weaponApCost(Object* critter, const Object* weapon, int hitMode, bool aimed);
int weaponRange(Object* critter, const Object* weapon, int hitMode);
int weaponMinDamage(const Object* weapon);
int weaponMaxDamage(const Object* weapon);
int weaponDamageType(const Object* weapon);
int weaponSkill(const Object* weapon, int hitMode);          // SKILL_*
int weaponAnimCode(const Object* weapon);                      // WANIM_*
bool weaponIsRanged(const Object* weapon, int hitMode);
bool weaponUsesAmmo(const Object* weapon);
int weaponAmmoPerShot(const Object* weapon, int hitMode);
bool weaponCanReloadWith(const Object* weapon, const Object* ammo);
int weaponReload(Object* weapon, Object* ammo);              // returns rounds moved
// Reloads `weapon` from the critter's inventory (whole boxes, like the original). Returns rounds moved.
int weaponReloadFromInventory(Object* critter, Object* weapon);
bool weaponIsTwoHanded(const Object* weapon);
bool weaponIsThrown(const Object* weapon, int hitMode);

int capsCount(const Object* critter);
void capsAdjust(Object* critter, int delta);

} // namespace fo
