#include "game/defs.h"

namespace fo {

const char* objTypeDir(int type)
{
    static const char* dirs[OBJ_TYPE_COUNT] = { "items", "critters", "scenery", "walls", "tiles", "misc",
                                                "intrface", "inven", "heads", "backgrnd", "skilldex" };
    return (type >= 0 && type < OBJ_TYPE_COUNT) ? dirs[type] : "misc";
}

const char* scriptProcName(int proc)
{
    static const char* names[SP_COUNT] = { "no_p_proc", "start", "spatial_p_proc", "description_p_proc",
        "pickup_p_proc", "drop_p_proc", "use_p_proc", "use_obj_on_p_proc", "use_skill_on_p_proc",
        "use_ad_on_p_proc", "use_disad_on_p_proc", "talk_p_proc", "critter_p_proc", "combat_p_proc",
        "damage_p_proc", "map_enter_p_proc", "map_exit_p_proc", "create_p_proc", "destroy_p_proc",
        "barter_init_p_proc", "barter_p_proc", "look_at_p_proc", "timed_event_p_proc", "map_update_p_proc" };
    return (proc >= 0 && proc < SP_COUNT) ? names[proc] : "";
}

const FlagName kObjectFlagNames[] = {
    { OF_HIDDEN, "hidden" }, { OF_NO_SAVE, "noSave" }, { OF_FLAT, "flat" }, { OF_NO_BLOCK, "noBlock" },
    { OF_LIGHTING, "lighting" }, { OF_NO_REMOVE, "noRemove" }, { OF_MULTIHEX, "multiHex" },
    { OF_NO_HIGHLIGHT, "noHighlight" }, { OF_USED, "used" }, { OF_TRANS_RED, "transRed" },
    { OF_TRANS_NONE, "transNone" }, { OF_TRANS_WALL, "transWall" }, { OF_TRANS_GLASS, "transGlass" },
    { OF_TRANS_STEAM, "transSteam" }, { OF_TRANS_ENERGY, "transEnergy" }, { OF_LEFT_HAND, "leftHand" },
    { OF_RIGHT_HAND, "rightHand" }, { OF_WORN, "worn" }, { OF_WALL_TRANS_END, "wallTransEnd" },
    { OF_LIGHT_THRU, "lightThru" }, { OF_SEEN, "seen" }, { OF_SHOOT_THRU, "shootThru" }, { 0, nullptr }
};

const FlagName kProtoExtFlagNames[] = {
    { PXF_BIG_GUN, "bigGun" }, { PXF_TWO_HANDED, "twoHanded" }, { PXF_CAN_USE, "canUse" },
    { PXF_CAN_USE_ON, "canUseOn" }, { PXF_CAN_LOOK, "canLook" }, { PXF_CAN_TALK, "canTalk" },
    { PXF_CAN_PICKUP, "canPickUp" }, { PXF_WALL_EAST_WEST, "wallEastWest" }, { PXF_WALL_NORTH_CORNER, "wallNorthCorner" },
    { PXF_WALL_SOUTH_CORNER, "wallSouthCorner" }, { PXF_WALL_EAST_CORNER, "wallEastCorner" },
    { PXF_WALL_WEST_CORNER, "wallWestCorner" }, { 0, nullptr }
};

const FlagName kCritterFlagNames[] = {
    { CF_BARTER, "barter" }, { CF_NO_STEAL, "noSteal" }, { CF_NO_DROP, "noDrop" }, { CF_NO_LIMBS, "noLimbs" },
    { CF_NO_AGE, "noAge" }, { CF_NO_HEAL, "noHeal" }, { CF_INVULNERABLE, "invulnerable" }, { CF_FLAT, "flat" },
    { CF_SPECIAL_DEATH, "specialDeath" }, { CF_LONG_LIMBS, "longLimbs" }, { CF_NO_KNOCKBACK, "noKnockback" },
    { 0, nullptr }
};

const FlagName kLockFlagNames[] = { { LOCK_FLAG_LOCKED, "locked" }, { LOCK_FLAG_JAMMED, "jammed" }, { 0, nullptr } };

Json flagsToJson(u32 flags, const FlagName* table)
{
    Json arr = Json::array();
    u32 known = 0;
    for (const FlagName* f = table; f->name; f++) {
        if (flags & f->bit) {
            arr.push(f->name);
            known |= f->bit;
        }
    }
    u32 rest = flags & ~known;
    for (int b = 0; b < 32; b++)
        if (rest & (1u << b)) arr.push(jsonHex(1u << b));
    return arr;
}

u32 flagsFromJson(const Json& j, const FlagName* table)
{
    if (j.isNumber()) return (u32)j.i();
    u32 v = 0;
    if (j.isString()) return (u32)j.i();
    for (auto& item : j.items()) {
        std::string s = item.str();
        bool found = false;
        for (const FlagName* f = table; f->name; f++) {
            if (strIEquals(s, f->name)) {
                v |= f->bit;
                found = true;
                break;
            }
        }
        if (!found) v |= (u32)item.i();
    }
    return v;
}

const char* const kItemTypeNames[ITEM_TYPE_COUNT] = { "armor", "container", "drug", "weapon", "ammo", "misc", "key" };
const char* const kSceneryTypeNames[SCEN_TYPE_COUNT] = { "door", "stairs", "elevator", "ladderUp", "ladderDown", "generic" };
const char* const kDamageTypeNames[DMG_TYPE_COUNT] = { "normal", "laser", "fire", "plasma", "electrical", "emp", "explosion" };
const char* const kMaterialNames[MAT_COUNT] = { "glass", "metal", "plastic", "wood", "dirt", "stone", "cement", "leather" };
const char* const kStatNames[STAT_COUNT] = {
    "strength", "perception", "endurance", "charisma", "intelligence", "agility", "luck",
    "maxHitPoints", "maxActionPoints", "armorClass", "unarmedDamage", "meleeDamage", "carryWeight", "sequence",
    "healingRate", "criticalChance", "betterCriticals",
    "dtNormal", "dtLaser", "dtFire", "dtPlasma", "dtElectrical", "dtEmp", "dtExplosion",
    "drNormal", "drLaser", "drFire", "drPlasma", "drElectrical", "drEmp", "drExplosion",
    "radiationResistance", "poisonResistance", "age", "gender",
    "currentHitPoints", "currentPoison", "currentRadiation"
};
const char* const kSkillNames[SKILL_COUNT] = {
    "smallGuns", "bigGuns", "energyWeapons", "unarmed", "meleeWeapons", "throwing", "firstAid", "doctor", "sneak",
    "lockpick", "steal", "traps", "science", "repair", "speech", "barter", "gambling", "outdoorsman"
};
const char* const kTraitNames[TRAIT_COUNT] = {
    "fastMetabolism", "bruiser", "smallFrame", "oneHander", "finesse", "kamikaze", "heavyHanded", "fastShot",
    "bloodyMess", "jinxed", "goodNatured", "chemReliant", "chemResistant", "nightPerson", "skilled", "gifted"
};
const char* const kAttackTypeNames[ATTACK_TYPE_COUNT] = {
    "none", "punch", "kick", "swing", "thrust", "throw", "fireSingle", "fireBurst", "flame"
};

int nameIndex(const char* const* names, int count, const std::string& name, int def)
{
    for (int i = 0; i < count; i++)
        if (strIEquals(name, names[i])) return i;
    return def;
}

int enumFromJson(const Json& j, const char* const* names, int count, int def)
{
    if (j.isNumber()) return j.toInt(def);
    if (j.isString()) {
        int idx = nameIndex(names, count, j.str(), -1);
        if (idx >= 0) return idx;
        return j.toInt(def);
    }
    return def;
}

} // namespace fo
