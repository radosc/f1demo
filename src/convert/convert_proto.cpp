// Beta (demo) PRO binaries -> JSON prototypes.
#include "convert/convert_internal.h"
#include "core/fs.h"
#include "core/json.h"
#include "game/msg.h"
#include "game/proto.h"
#include "game/scriptnames.h"

namespace fo {

static bool parseBetaProto(const Bytes& data, int type, Proto& p)
{
    BeReader r(data);
    p.pid = r.i32v();
    p.messageId = r.i32v();
    p.fid = r.i32v();
    switch (type) {
    case OBJ_ITEM: {
        p.lightDistance = r.i32v();
        p.lightIntensity = r.i32v();
        p.flags = r.u32v();
        p.extFlags = r.u32v();
        p.sid = r.i32v();
        p.itemType = r.i32v();
        p.material = r.i32v();
        p.size = r.i32v();
        p.weight = r.i32v();
        p.cost = r.i32v();
        p.invFid = r.i32v();
        switch (p.itemType) {
        case ITEM_ARMOR:
            p.armor.ac = r.i32v();
            for (int i = 0; i < 7; i++) p.armor.dr[i] = r.i32v();
            for (int i = 0; i < 7; i++) p.armor.dt[i] = r.i32v();
            p.armor.perk = r.i32v();
            p.armor.maleFid = r.i32v();
            p.armor.femaleFid = r.i32v();
            break;
        case ITEM_CONTAINER:
            p.container.maxSize = r.i32v();
            p.container.openFlags = r.u32v();
            break;
        case ITEM_DRUG: {
            // beta layout: stat, amount, duration1, amount1, duration2, amount2, addiction, withdrawal effect, onset
            p.drug.stat[0] = r.i32v();
            p.drug.amount[0] = r.i32v();
            p.drug.duration1 = r.i32v();
            p.drug.amount1[0] = r.i32v();
            p.drug.duration2 = r.i32v();
            p.drug.amount2[0] = r.i32v();
            p.drug.addictionChance = r.i32v();
            p.drug.withdrawalEffect = r.i32v();
            p.drug.withdrawalOnset = r.i32v();
            break;
        }
        case ITEM_WEAPON:
            p.weapon.animCode = r.i32v();
            p.weapon.minDamage = r.i32v();
            p.weapon.maxDamage = r.i32v();
            p.weapon.damageType = r.i32v();
            p.weapon.range1 = r.i32v();
            p.weapon.range2 = r.i32v();
            p.weapon.projectilePid = r.i32v();
            p.weapon.minStrength = r.i32v();
            p.weapon.ap1 = r.i32v();
            p.weapon.ap2 = r.i32v();
            p.weapon.criticalFail = r.i32v();
            p.weapon.perk = r.i32v();
            p.weapon.burstRounds = r.i32v();
            p.weapon.caliber = r.i32v();
            p.weapon.ammoPid = r.i32v();
            p.weapon.maxAmmo = r.i32v();
            p.weapon.soundCode = r.u8v();
            break;
        case ITEM_AMMO:
            p.ammo.caliber = r.i32v();
            p.ammo.quantity = r.i32v();
            p.ammo.acMod = r.i32v();
            p.ammo.drMod = r.i32v();
            p.ammo.dmgMult = r.i32v();
            p.ammo.dmgDiv = r.i32v();
            break;
        case ITEM_MISC:
            p.misc.powerPid = r.i32v();
            p.misc.powerType = r.i32v();
            p.misc.charges = r.i32v();
            if (p.misc.charges == (int)0xCCCCCCCC) p.misc.charges = 0;
            if (p.misc.powerType == (int)0xCCCCCCCC) p.misc.powerType = 0;
            break;
        case ITEM_KEY:
            p.key.keyCode = r.i32v();
            break;
        }
        break;
    }
    case OBJ_CRITTER:
        p.lightDistance = r.i32v();
        p.lightIntensity = r.i32v();
        p.flags = r.u32v();
        p.extFlags = r.u32v();
        p.sid = r.i32v();
        p.headFid = r.i32v();
        p.aiPacket = r.i32v();
        p.team = r.i32v();
        p.critterFlags = r.u32v();
        for (int i = 0; i < STAT_SAVEABLE_COUNT; i++) p.baseStats[i] = r.i32v();
        for (int i = 0; i < STAT_SAVEABLE_COUNT; i++) p.bonusStats[i] = r.i32v();
        for (int i = 0; i < SKILL_COUNT; i++) p.skills[i] = r.i32v();
        p.bodyType = r.i32v();
        if (r.can(8)) {
            p.experience = r.i32v();
            p.killType = r.i32v();
        } else {
            p.killType = -1; // the demo's format predates these fields; convertProtos derives the kill type
        }
        break;
    case OBJ_SCENERY:
        p.lightDistance = r.i32v();
        p.lightIntensity = r.i32v();
        p.flags = r.u32v();
        p.extFlags = r.u32v();
        p.sid = r.i32v();
        p.sceneryType = r.i32v();
        p.material = r.i32v();
        switch (p.sceneryType) {
        case SCEN_DOOR:
        case SCEN_STAIRS:
        case SCEN_ELEVATOR:
            p.sceneryData[0] = r.i32v();
            p.sceneryData[1] = r.i32v();
            break;
        default:
            p.sceneryData[0] = r.i32v();
            break;
        }
        for (int& v : p.sceneryData)
            if (v == (int)0xCCCCCCCC) v = -1;
        break;
    case OBJ_WALL:
        p.lightDistance = r.i32v();
        p.lightIntensity = r.i32v();
        p.flags = r.u32v();
        p.extFlags = r.u32v();
        p.sid = r.i32v();
        p.material = r.i32v();
        break;
    case OBJ_TILE:
        p.flags = r.u32v();
        p.extFlags = r.u32v();
        p.sid = r.i32v();
        p.material = r.i32v();
        break;
    case OBJ_MISC:
        p.lightDistance = r.i32v();
        p.lightIntensity = r.i32v();
        p.flags = r.u32v();
        p.extFlags = r.u32v();
        break;
    default:
        return false;
    }
    return !r.bad;
}

// The demo's critter prototypes have no kill type, which selects the critical hit table and the kill
// counter, so it is derived from the critter's art name (critters.lst line).
static int killTypeFromArt(const std::string& art)
{
    static const struct {
        const char* art;
        int killType;
    } kTypes[] = {
        { "nachld", 2 }, { "mamtnt", 3 }, { "mamtn2", 3 }, { "malieu", 3 }, { "naghul", 4 }, { "naglow", 4 }, { "mabrom", 5 },
        { "mascrp", 6 }, { "mamrat", 7 }, { "masrat", 7 }, { "masphn", 8 }, { "mathng", 9 }, { "mahand", 10 }, { "maddog", 11 },
        { "maclaw", 13 },
    };
    for (auto& t : kTypes)
        if (art == t.art) return t.killType;
    // human (h) and townspeople (n) art: the second letter is the gender (m, f, a = any)
    if (art.size() >= 2 && (art[0] == 'h' || art[0] == 'n') && art[1] == 'f') return 1;
    return 0;
}

bool convertProtos(ConvContext& c)
{
    std::vector<std::string> critterArt;
    Bytes artList;
    if (c.dat.read("art/critters/critters.lst", artList)) {
        for (auto& line : strSplit(std::string(artList.begin(), artList.end()), '\n')) {
            std::string a = strTrim(line);
            critterArt.push_back(strLower(a.substr(0, a.find(','))));
        }
    }

    // script names are needed to write readable script references
    Bytes lst;
    if (c.dat.read("scripts/scripts.lst", lst)) g_scriptNames.loadFrom(std::string(lst.begin(), lst.end()));

    static const char* kDatDirs[6] = { "items", "critters", "scenery", "walls", "tiles", "misc" };
    static const char* kMsg[6] = { "pro_item", "pro_crit", "pro_scen", "pro_wall", "pro_tile", "pro_misc" };
    int total = 0;
    for (int type = 0; type < 6; type++) {
        MessageList names;
        Bytes msgData;
        if (c.dat.read(std::string("text/game/") + kMsg[type] + ".msg", msgData)) names.parse(std::string(msgData.begin(), msgData.end()));
        Bytes list;
        std::string dir = std::string("proto/") + kDatDirs[type];
        if (!c.dat.read(dir + "/" + kDatDirs[type] + ".lst", list)) continue;
        auto lines = strSplit(std::string(list.begin(), list.end()), '\n');
        for (size_t i = 0; i < lines.size(); i++) {
            std::string fn = strTrim(lines[i]);
            size_t sp = fn.find_first_of(" \t;");
            if (sp != std::string::npos) fn = fn.substr(0, sp);
            if (fn.empty()) continue;
            Bytes data;
            if (!c.dat.read(dir + "/" + fn, data)) continue;
            Proto p;
            if (!parseBetaProto(data, type, p)) {
                LOGW("Could not parse %s/%s", dir.c_str(), fn.c_str());
                continue;
            }
            int expectedPid = (type << 24) | (int)(i + 1);
            if (p.pid != expectedPid) {
                LOGW("Proto %s/%s: pid %08X does not match list position %08X", dir.c_str(), fn.c_str(), p.pid, expectedPid);
                p.pid = expectedPid;
            }
            if (p.killType < 0) {
                size_t art = (size_t)(p.fid & 0xFFF);
                p.killType = art < critterArt.size() ? killTypeFromArt(critterArt[art]) : 0;
            }
            std::string name = names.has(p.messageId) ? names.get(p.messageId) : std::string();
            convWriteText(c, ProtoDb::pathFor(p.pid), p.toJson(name).dump());
            c.protos[p.pid] = p;
            total++;
        }
    }
    LOGI("Converted %d protos", total);
    return true;
}

} // namespace fo
