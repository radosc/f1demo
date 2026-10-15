#include "game/proto.h"
#include "game/msg.h"
#include "game/scriptnames.h"
#include "core/fs.h"

namespace fo {

ProtoDb g_protos;

static Json intArray(const int* v, int n)
{
    Json a = Json::array();
    for (int i = 0; i < n; i++) a.push(v[i]);
    return a;
}

static void readIntArray(const Json& j, int* v, int n)
{
    if (!j.isArray()) return;
    for (int i = 0; i < n && i < (int)j.size(); i++) v[i] = j[(size_t)i].toInt();
}

static Json fidJson(int fid) { return fid < 0 ? Json(-1) : Json(jsonHex((u32)fid)); }

static Json damageTable(const int* v)
{
    Json o = Json::object();
    for (int i = 0; i < DMG_TYPE_COUNT; i++) o.set(kDamageTypeNames[i], v[i]);
    return o;
}

static void readDamageTable(const Json& j, int* v)
{
    if (j.isArray()) { readIntArray(j, v, DMG_TYPE_COUNT); return; }
    for (int i = 0; i < DMG_TYPE_COUNT; i++)
        if (j.has(kDamageTypeNames[i])) v[i] = j[kDamageTypeNames[i]].toInt();
}

static Json statTable(const int* v, bool skipZero)
{
    Json o = Json::object();
    for (int i = 0; i < STAT_SAVEABLE_COUNT; i++)
        if (!skipZero || v[i]) o.set(kStatNames[i], v[i]);
    return o;
}

static void readStatTable(const Json& j, int* v)
{
    if (j.isArray()) { readIntArray(j, v, STAT_SAVEABLE_COUNT); return; }
    for (auto& kv : j.members()) {
        int idx = nameIndex(kStatNames, STAT_SAVEABLE_COUNT, kv.first, -1);
        if (idx >= 0) v[idx] = kv.second.toInt();
    }
}

Json Proto::toJson(const std::string& nameComment) const
{
    Json j = Json::object();
    j.set("pid", jsonHex((u32)pid));
    if (!nameComment.empty()) j.set("name", nameComment);
    j.set("messageId", messageId);
    j.set("fid", fidJson(fid));
    j.set("flags", flagsToJson(flags, kObjectFlagNames));
    int t = type();
    u32 ext = extFlags;
    if (t == OBJ_ITEM && itemType == ITEM_WEAPON) ext &= ~0xFFu; // attack modes stored in "weapon"
    j.set("extFlags", flagsToJson(ext, kProtoExtFlagNames));
    if (t != OBJ_TILE) {
        j.set("lightDistance", lightDistance);
        j.set("lightIntensity", lightIntensity);
    }
    if (t != OBJ_MISC) {
        if (sid >= 0) {
            j.set("script", g_scriptNames.name(sid & 0xFFFFFF));
            j.set("scriptType", (sid >> 24) & 0xFF);
        } else {
            j.set("script", Json());
        }
    }
    switch (t) {
    case OBJ_ITEM: {
        j.set("itemType", kItemTypeNames[clampv(itemType, 0, ITEM_TYPE_COUNT - 1)]);
        j.set("material", kMaterialNames[clampv(material, 0, MAT_COUNT - 1)]);
        j.set("size", size);
        j.set("weight", weight);
        j.set("cost", cost);
        j.set("inventoryFid", fidJson(invFid));
        j.set("soundId", soundId);
        Json d = Json::object();
        switch (itemType) {
        case ITEM_ARMOR:
            d.set("armorClass", armor.ac);
            d.set("damageResistance", damageTable(armor.dr));
            d.set("damageThreshold", damageTable(armor.dt));
            d.set("perk", armor.perk);
            d.set("maleFid", fidJson(armor.maleFid));
            d.set("femaleFid", fidJson(armor.femaleFid));
            j.set("armor", d);
            break;
        case ITEM_CONTAINER:
            d.set("maxSize", container.maxSize);
            d.set("openFlags", (i64)container.openFlags);
            j.set("container", d);
            break;
        case ITEM_DRUG:
            d.set("stats", intArray(drug.stat, 3));
            d.set("amount", intArray(drug.amount, 3));
            d.set("duration1", drug.duration1);
            d.set("amount1", intArray(drug.amount1, 3));
            d.set("duration2", drug.duration2);
            d.set("amount2", intArray(drug.amount2, 3));
            d.set("addictionChance", drug.addictionChance);
            d.set("withdrawalEffect", drug.withdrawalEffect);
            d.set("withdrawalOnset", drug.withdrawalOnset);
            j.set("drug", d);
            break;
        case ITEM_WEAPON: {
            Json attacks = Json::array();
            attacks.push(kAttackTypeNames[clampv(primaryAttack(), 0, ATTACK_TYPE_COUNT - 1)]);
            attacks.push(kAttackTypeNames[clampv(secondaryAttack(), 0, ATTACK_TYPE_COUNT - 1)]);
            d.set("attacks", attacks);
            d.set("animation", weapon.animCode);
            Json dmg = Json::array();
            dmg.push(weapon.minDamage);
            dmg.push(weapon.maxDamage);
            d.set("damage", dmg);
            d.set("damageType", kDamageTypeNames[clampv(weapon.damageType, 0, DMG_TYPE_COUNT - 1)]);
            Json range = Json::array();
            range.push(weapon.range1);
            range.push(weapon.range2);
            d.set("range", range);
            Json ap = Json::array();
            ap.push(weapon.ap1);
            ap.push(weapon.ap2);
            d.set("apCost", ap);
            d.set("projectilePid", weapon.projectilePid < 0 ? Json(-1) : Json(jsonHex((u32)weapon.projectilePid)));
            d.set("minStrength", weapon.minStrength);
            d.set("criticalFail", weapon.criticalFail);
            d.set("perk", weapon.perk);
            d.set("burstRounds", weapon.burstRounds);
            d.set("caliber", weapon.caliber);
            d.set("ammoPid", weapon.ammoPid < 0 ? Json(-1) : Json(jsonHex((u32)weapon.ammoPid)));
            d.set("maxAmmo", weapon.maxAmmo);
            d.set("soundCode", std::string(1, (char)(weapon.soundCode ? weapon.soundCode : ' ')));
            j.set("weapon", d);
            break;
        }
        case ITEM_AMMO:
            d.set("caliber", ammo.caliber);
            d.set("quantity", ammo.quantity);
            d.set("acModifier", ammo.acMod);
            d.set("drModifier", ammo.drMod);
            d.set("damageMultiplier", ammo.dmgMult);
            d.set("damageDivisor", ammo.dmgDiv);
            j.set("ammo", d);
            break;
        case ITEM_MISC:
            d.set("powerPid", misc.powerPid < 0 ? Json(-1) : Json(jsonHex((u32)misc.powerPid)));
            d.set("powerType", misc.powerType);
            d.set("charges", misc.charges);
            j.set("misc", d);
            break;
        case ITEM_KEY:
            d.set("keyCode", key.keyCode);
            j.set("key", d);
            break;
        }
        break;
    }
    case OBJ_CRITTER: {
        Json c = Json::object();
        c.set("headFid", fidJson(headFid));
        c.set("aiPacket", aiPacket);
        c.set("team", team);
        c.set("flags", flagsToJson(critterFlags, kCritterFlagNames));
        c.set("stats", statTable(baseStats, false));
        c.set("bonusStats", statTable(bonusStats, true));
        Json sk = Json::object();
        for (int i = 0; i < SKILL_COUNT; i++) sk.set(kSkillNames[i], skills[i]);
        c.set("skills", sk);
        c.set("bodyType", bodyType);
        c.set("experience", experience);
        c.set("killType", killType);
        j.set("critter", c);
        break;
    }
    case OBJ_SCENERY: {
        j.set("sceneryType", kSceneryTypeNames[clampv(sceneryType, 0, SCEN_TYPE_COUNT - 1)]);
        j.set("material", kMaterialNames[clampv(material, 0, MAT_COUNT - 1)]);
        j.set("soundId", soundId);
        Json d = Json::object();
        switch (sceneryType) {
        case SCEN_DOOR:
            d.set("openFlags", flagsToJson((u32)sceneryData[0], kLockFlagNames));
            d.set("keyCode", sceneryData[1]);
            break;
        case SCEN_STAIRS:
            d.set("destMap", sceneryData[0]);
            d.set("destTile", sceneryData[1]);
            break;
        case SCEN_ELEVATOR:
            d.set("elevatorType", sceneryData[0]);
            d.set("level", sceneryData[1]);
            break;
        case SCEN_LADDER_UP:
        case SCEN_LADDER_DOWN:
            d.set("destTile", sceneryData[0]);
            break;
        default:
            d.set("data", sceneryData[0]);
            break;
        }
        j.set(kSceneryTypeNames[clampv(sceneryType, 0, SCEN_TYPE_COUNT - 1)], d);
        break;
    }
    case OBJ_WALL:
    case OBJ_TILE:
        j.set("material", kMaterialNames[clampv(material, 0, MAT_COUNT - 1)]);
        break;
    default:
        break;
    }
    return j;
}

static int pidFromJson(const Json& j) { return j.isNull() ? -1 : (int)j.i(-1); }

bool Proto::fromJson(const Json& j)
{
    if (!j.isObject()) return false;
    pid = (int)j["pid"].i(-1);
    if (pid < 0) return false;
    messageId = j["messageId"].toInt(0);
    fid = (int)j["fid"].i(-1);
    flags = flagsFromJson(j["flags"], kObjectFlagNames);
    extFlags = flagsFromJson(j["extFlags"], kProtoExtFlagNames);
    lightDistance = j["lightDistance"].toInt(0);
    lightIntensity = j["lightIntensity"].toInt(0);
    sid = -1;
    const Json& sc = j["script"];
    if (sc.isString() && !sc.str().empty()) {
        int idx = g_scriptNames.indexOf(sc.str());
        if (idx >= 0) {
            int st = j["scriptType"].toInt(type() == OBJ_CRITTER ? SCRIPT_CRITTER : SCRIPT_ITEM);
            sid = (st << 24) | idx;
        } else {
            LOGW("Proto %08X references unknown script '%s'", pid, sc.str().c_str());
        }
    } else if (sc.isNumber()) {
        sid = sc.toInt(-1);
    }
    switch (type()) {
    case OBJ_ITEM: {
        itemType = enumFromJson(j["itemType"], kItemTypeNames, ITEM_TYPE_COUNT, 0);
        material = enumFromJson(j["material"], kMaterialNames, MAT_COUNT, 0);
        size = j["size"].toInt();
        weight = j["weight"].toInt();
        cost = j["cost"].toInt();
        invFid = (int)j["inventoryFid"].i(-1);
        soundId = j["soundId"].toInt();
        switch (itemType) {
        case ITEM_ARMOR: {
            const Json& d = j["armor"];
            armor.ac = d["armorClass"].toInt();
            readDamageTable(d["damageResistance"], armor.dr);
            readDamageTable(d["damageThreshold"], armor.dt);
            armor.perk = d["perk"].toInt(-1);
            armor.maleFid = (int)d["maleFid"].i(-1);
            armor.femaleFid = (int)d["femaleFid"].i(-1);
            break;
        }
        case ITEM_CONTAINER: {
            const Json& d = j["container"];
            container.maxSize = d["maxSize"].toInt();
            container.openFlags = (u32)d["openFlags"].i(0);
            break;
        }
        case ITEM_DRUG: {
            const Json& d = j["drug"];
            readIntArray(d["stats"], drug.stat, 3);
            readIntArray(d["amount"], drug.amount, 3);
            drug.duration1 = d["duration1"].toInt();
            readIntArray(d["amount1"], drug.amount1, 3);
            drug.duration2 = d["duration2"].toInt();
            readIntArray(d["amount2"], drug.amount2, 3);
            drug.addictionChance = d["addictionChance"].toInt();
            drug.withdrawalEffect = d["withdrawalEffect"].toInt();
            drug.withdrawalOnset = d["withdrawalOnset"].toInt();
            break;
        }
        case ITEM_WEAPON: {
            const Json& d = j["weapon"];
            int a0 = enumFromJson(d["attacks"][(size_t)0], kAttackTypeNames, ATTACK_TYPE_COUNT, 0);
            int a1 = enumFromJson(d["attacks"][(size_t)1], kAttackTypeNames, ATTACK_TYPE_COUNT, 0);
            extFlags = (extFlags & ~0xFFu) | (u32)(a0 & 0xF) | ((u32)(a1 & 0xF) << 4);
            weapon.animCode = d["animation"].toInt();
            weapon.minDamage = d["damage"][(size_t)0].toInt();
            weapon.maxDamage = d["damage"][(size_t)1].toInt();
            weapon.damageType = enumFromJson(d["damageType"], kDamageTypeNames, DMG_TYPE_COUNT, 0);
            weapon.range1 = d["range"][(size_t)0].toInt();
            weapon.range2 = d["range"][(size_t)1].toInt();
            weapon.ap1 = d["apCost"][(size_t)0].toInt();
            weapon.ap2 = d["apCost"][(size_t)1].toInt();
            weapon.projectilePid = pidFromJson(d["projectilePid"]);
            weapon.minStrength = d["minStrength"].toInt();
            weapon.criticalFail = d["criticalFail"].toInt();
            weapon.perk = d["perk"].toInt(-1);
            weapon.burstRounds = d["burstRounds"].toInt();
            weapon.caliber = d["caliber"].toInt();
            weapon.ammoPid = pidFromJson(d["ammoPid"]);
            weapon.maxAmmo = d["maxAmmo"].toInt();
            std::string sc2 = d["soundCode"].str();
            weapon.soundCode = sc2.empty() || sc2[0] == ' ' ? 0 : (u8)sc2[0];
            break;
        }
        case ITEM_AMMO: {
            const Json& d = j["ammo"];
            ammo.caliber = d["caliber"].toInt();
            ammo.quantity = d["quantity"].toInt();
            ammo.acMod = d["acModifier"].toInt();
            ammo.drMod = d["drModifier"].toInt();
            ammo.dmgMult = d["damageMultiplier"].toInt(1);
            ammo.dmgDiv = d["damageDivisor"].toInt(1);
            break;
        }
        case ITEM_MISC: {
            const Json& d = j["misc"];
            misc.powerPid = pidFromJson(d["powerPid"]);
            misc.powerType = d["powerType"].toInt();
            misc.charges = d["charges"].toInt();
            break;
        }
        case ITEM_KEY:
            key.keyCode = j["key"]["keyCode"].toInt(-1);
            break;
        }
        break;
    }
    case OBJ_CRITTER: {
        const Json& c = j["critter"];
        headFid = (int)c["headFid"].i(-1);
        aiPacket = c["aiPacket"].toInt();
        team = c["team"].toInt();
        critterFlags = flagsFromJson(c["flags"], kCritterFlagNames);
        readStatTable(c["stats"], baseStats);
        readStatTable(c["bonusStats"], bonusStats);
        const Json& sk = c["skills"];
        if (sk.isArray()) readIntArray(sk, skills, SKILL_COUNT);
        else
            for (auto& kv : sk.members()) {
                int idx = nameIndex(kSkillNames, SKILL_COUNT, kv.first, -1);
                if (idx >= 0) skills[idx] = kv.second.toInt();
            }
        bodyType = c["bodyType"].toInt();
        experience = c["experience"].toInt();
        killType = c["killType"].toInt();
        break;
    }
    case OBJ_SCENERY: {
        sceneryType = enumFromJson(j["sceneryType"], kSceneryTypeNames, SCEN_TYPE_COUNT, SCEN_GENERIC);
        material = enumFromJson(j["material"], kMaterialNames, MAT_COUNT, 0);
        soundId = j["soundId"].toInt();
        const Json& d = j[kSceneryTypeNames[clampv(sceneryType, 0, SCEN_TYPE_COUNT - 1)]];
        switch (sceneryType) {
        case SCEN_DOOR:
            sceneryData[0] = (int)flagsFromJson(d["openFlags"], kLockFlagNames);
            sceneryData[1] = d["keyCode"].toInt(-1);
            break;
        case SCEN_STAIRS:
            sceneryData[0] = d["destMap"].toInt(-1);
            sceneryData[1] = d["destTile"].toInt(-1);
            break;
        case SCEN_ELEVATOR:
            sceneryData[0] = d["elevatorType"].toInt(-1);
            sceneryData[1] = d["level"].toInt(-1);
            break;
        case SCEN_LADDER_UP:
        case SCEN_LADDER_DOWN:
            sceneryData[0] = d["destTile"].toInt(-1);
            break;
        default:
            sceneryData[0] = d["data"].toInt(0);
            break;
        }
        break;
    }
    case OBJ_WALL:
    case OBJ_TILE:
        material = enumFromJson(j["material"], kMaterialNames, MAT_COUNT, 0);
        break;
    default:
        break;
    }
    return true;
}

Proto* ProtoDb::put(const Proto& p)
{
    auto copy = std::make_unique<Proto>(p);
    Proto* raw = copy.get();
    m_cache[p.pid] = std::move(copy);
    m_missing.erase(p.pid);
    return raw;
}

std::string ProtoDb::pathFor(int pid)
{
    return strFormat("proto/%s/%05d.json", objTypeDir(pidType(pid)), pidIndex(pid));
}

const Proto* ProtoDb::get(int pid)
{
    if (pid < 0) return nullptr;
    auto it = m_cache.find(pid);
    if (it != m_cache.end()) return it->second.get();
    if (m_missing.count(pid)) return nullptr;
    std::string text;
    if (!g_vfs.readText(pathFor(pid), text)) {
        m_missing.insert(pid);
        return nullptr;
    }
    std::string err;
    Json j = Json::parse(text, &err);
    auto p = std::make_unique<Proto>();
    if (!err.empty() || !p->fromJson(j)) {
        LOGE("Bad proto %s: %s", pathFor(pid).c_str(), err.c_str());
        m_missing.insert(pid);
        return nullptr;
    }
    p->pid = pid;
    Proto* raw = p.get();
    m_cache[pid] = std::move(p);
    return raw;
}

static const char* protoMsgFile(int type)
{
    switch (type) {
    case OBJ_ITEM: return "pro_item";
    case OBJ_CRITTER: return "pro_crit";
    case OBJ_SCENERY: return "pro_scen";
    case OBJ_WALL: return "pro_wall";
    case OBJ_TILE: return "pro_tile";
    default: return "pro_misc";
    }
}

std::string ProtoDb::name(int pid)
{
    const Proto* p = get(pid);
    if (!p) return "";
    return gameMsg(protoMsgFile(pidType(pid)), p->messageId);
}

std::string ProtoDb::description(int pid)
{
    const Proto* p = get(pid);
    if (!p) return "";
    MessageList& ml = messages("text/" + g_language + "/game/" + protoMsgFile(pidType(pid)) + ".msg");
    const MessageEntry* e = ml.find(p->messageId + 1);
    return e ? e->text : std::string();
}

} // namespace fo
