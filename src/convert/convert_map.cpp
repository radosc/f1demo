// Binary MAP (beta v16 / release v19) -> JSON map.
#include "convert/convert_internal.h"
#include "core/fs.h"
#include "core/json.h"
#include "game/msg.h"
#include "game/scriptnames.h"

namespace fo {

namespace {

struct RawScript {
    u32 sid = 0;
    int tile = -1, radius = -1, time = 0;
    int flags = 0;
    int index = -1; // 0-based scripts.lst index
    int oid = -1;
    int localVarOffset = -1, localVarCount = 0;
};

struct MapReader {
    ConvContext& c;
    BeReader r;
    int version = 0;
    std::unordered_map<u32, RawScript> scripts; // by sid
    MessageList protoNames[6];

    std::string protoName(int pid)
    {
        int t = pidType(pid);
        auto it = c.protos.find(pid);
        if (t < 0 || t > 5 || it == c.protos.end()) return {};
        return protoNames[t].has(it->second.messageId) ? protoNames[t].get(it->second.messageId) : std::string();
    }

    bool readScripts()
    {
        for (int type = 0; type < 5; type++) {
            int count = r.i32v();
            if (count <= 0) continue;
            int extents = (count + 15) / 16;
            for (int x = 0; x < extents; x++) {
                for (int k = 0; k < 16; k++) {
                    RawScript s;
                    s.sid = r.u32v();
                    r.i32v(); // next
                    int stype = (int)(s.sid >> 24);
                    if (stype == SCRIPT_SPATIAL) {
                        s.tile = r.i32v();
                        s.radius = r.i32v();
                    } else if (stype == SCRIPT_TIMED) {
                        s.time = r.i32v();
                    }
                    if (version >= 19) {
                        s.flags = r.i32v();
                        s.index = r.i32v();
                        r.i32v(); // program pointer
                        s.oid = r.i32v();
                        s.localVarOffset = r.i32v();
                        s.localVarCount = r.i32v();
                        r.skip(8 * 4);
                    } else {
                        // beta records: 24 ints after the type specific part
                        s.flags = r.i32v();
                        s.index = r.i32v();
                        r.i32v(); // program pointer
                        s.oid = r.i32v();
                        s.localVarOffset = r.i32v();
                        s.localVarCount = r.i32v();
                        r.skip(18 * 4);
                    }
                    if (k < count - x * 16 && s.sid != 0xCCCCCCCC) scripts[s.sid] = s;
                }
                r.i32v(); // extent length
                r.i32v(); // next extent
            }
        }
        return !r.bad;
    }

    Json readObject(int depth)
    {
        Json o = Json::object();
        int id = r.i32v();
        int tile = r.i32v();
        r.i32v(); // x
        r.i32v(); // y
        r.i32v(); // sx
        r.i32v(); // sy
        int frame = r.i32v();
        int rot = r.i32v();
        int fid = r.i32v();
        u32 flags = r.u32v();
        int elev = r.i32v();
        int pid = r.i32v();
        r.i32v(); // cid
        int lightDist = r.i32v();
        int lightInt = r.i32v();
        r.i32v(); // field_74
        int sid = r.i32v();
        if (version >= 19) r.i32v(); // field_80 (script index cache)
        int invLen = r.i32v();
        r.i32v(); // capacity
        r.i32v(); // items pointer
        int type = pidType(pid);

        o.set("pid", jsonHex((u32)pid));
        std::string nm = protoName(pid);
        if (!nm.empty()) o.set("name", nm);
        if (id != -1 && id != (int)0xCCCCCCCC) o.set("id", id);
        if (depth == 0) {
            o.set("tile", tile);
            o.set("elevation", elev);
        }
        o.set("rotation", rot);
        auto protoIt = c.protos.find(pid);
        if (protoIt == c.protos.end() || protoIt->second.fid != fid) o.set("fid", jsonHex((u32)fid));
        if (frame) o.set("frame", frame);
        o.set("flags", flagsToJson(flags, kObjectFlagNames));
        if (lightDist || lightInt) {
            Json l = Json::array();
            l.push(lightDist);
            l.push(lightInt);
            o.set("light", l);
        }
        if (sid != -1) {
            auto it = scripts.find((u32)sid);
            if (it != scripts.end()) {
                o.set("script", g_scriptNames.name(it->second.index));
            } else {
                LOGW("Object %08X references unknown script %08X", pid, sid);
            }
        }

        if (type == OBJ_CRITTER) {
            Json cr = Json::object();
            int reaction = r.i32v();
            int damageLastTurn = r.i32v();
            int maneuver = r.i32v();
            int ap = r.i32v();
            int results = r.i32v();
            int aiPacket = r.i32v();
            int team = r.i32v();
            r.i32v(); // who hit me
            int hp = r.i32v();
            int rad = r.i32v();
            int poison = r.i32v();
            cr.set("hp", hp);
            cr.set("ap", ap);
            cr.set("aiPacket", aiPacket);
            cr.set("team", team);
            if (results) cr.set("results", results);
            if (rad) cr.set("radiation", rad);
            if (poison) cr.set("poison", poison);
            if (reaction) cr.set("reaction", reaction);
            if (maneuver) cr.set("maneuver", maneuver);
            (void)damageLastTurn;
            o.set("critter", cr);
        } else {
            int dataFlags = r.i32v();
            if (dataFlags != 0 && dataFlags != -1 && dataFlags != (int)0xCCCCCCCC) o.set("dataFlags", dataFlags);
            const Proto* proto = protoIt == c.protos.end() ? nullptr : &protoIt->second;
            if (type == OBJ_ITEM && proto) {
                switch (proto->itemType) {
                case ITEM_WEAPON: {
                    int qty = r.i32v();
                    int ammoPid = r.i32v();
                    Json w = Json::object();
                    w.set("ammo", qty);
                    w.set("ammoPid", ammoPid < 0 ? Json(-1) : Json(jsonHex((u32)ammoPid)));
                    o.set("weapon", w);
                    break;
                }
                case ITEM_AMMO: o.set("ammo", r.i32v()); break;
                case ITEM_MISC: o.set("charges", r.i32v()); break;
                case ITEM_KEY: o.set("keyCode", r.i32v()); break;
                default: break;
                }
            } else if (type == OBJ_SCENERY && proto) {
                switch (proto->sceneryType) {
                case SCEN_DOOR: {
                    Json d = Json::object();
                    d.set("openFlags", flagsToJson(r.u32v(), kLockFlagNames));
                    o.set("door", d);
                    break;
                }
                case SCEN_STAIRS: {
                    Json d = Json::object();
                    d.set("destMap", r.i32v());
                    d.set("destTile", r.i32v());
                    o.set("stairs", d);
                    break;
                }
                case SCEN_ELEVATOR: {
                    Json d = Json::object();
                    d.set("elevatorType", r.i32v());
                    d.set("level", r.i32v());
                    o.set("elevator", d);
                    break;
                }
                case SCEN_LADDER_UP:
                case SCEN_LADDER_DOWN: {
                    Json d = Json::object();
                    d.set("destTile", r.i32v());
                    o.set("ladder", d);
                    break;
                }
                default: break;
                }
            } else if (type == OBJ_MISC && pid >= 0x5000010 && pid <= 0x5000017) {
                Json e = Json::object();
                e.set("map", r.i32v());
                e.set("tile", r.i32v());
                e.set("elevation", r.i32v());
                e.set("rotation", r.i32v());
                o.set("exitGrid", e);
            } else if ((type == OBJ_ITEM || type == OBJ_SCENERY) && !proto) {
                LOGW("Map object with unknown proto %08X - data may be misaligned", pid);
            }
        }

        if (invLen > 0) {
            Json inv = Json::array();
            for (int i = 0; i < invLen && !r.bad; i++) {
                Json entry = Json::object();
                entry.set("count", r.i32v());
                entry.set("object", readObject(depth + 1));
                inv.push(entry);
            }
            o.set("inventory", inv);
        }
        return o;
    }
};

} // namespace

static bool convertOneMap(ConvContext& c, const std::string& path, const Bytes& data)
{
    MapReader m{ c, BeReader(data) };
    static const char* kMsg[6] = { "pro_item", "pro_crit", "pro_scen", "pro_wall", "pro_tile", "pro_misc" };
    for (int t = 0; t < 6; t++) {
        Bytes md;
        if (c.dat.read(std::string("text/game/") + kMsg[t] + ".msg", md)) m.protoNames[t].parse(std::string(md.begin(), md.end()));
    }
    BeReader& r = m.r;
    m.version = r.i32v();
    std::string name = strLower(r.str(16));
    int enterTile = r.i32v();
    int enterElev = r.i32v();
    int enterRot = r.i32v();
    int localCount = r.i32v();
    int scriptIndex = r.i32v();
    u32 flags = r.u32v();
    int darkness = r.i32v();
    int globalCount = r.i32v();
    int mapNumber = r.i32v();
    r.i32v(); // last visit time
    r.skip(44 * 4);
    if (m.version != 16 && m.version != 19) {
        LOGE("%s: unsupported map version %d", path.c_str(), m.version);
        return false;
    }
    Json j = Json::object();
    j.set("format", "fo1hd-map");
    j.set("version", 1);
    j.set("name", pathStem(name));
    // town names shown by the pipboy and automap (the demo's map.msg has no entry for its only map)
    if (strIEquals(pathStem(name), "junkdemo")) j.set("displayName", "Scrapheap");
    Json ent = Json::object();
    ent.set("tile", enterTile);
    ent.set("elevation", enterElev);
    ent.set("rotation", enterRot);
    j.set("entrance", ent);
    j.set("script", scriptIndex > 0 ? Json(g_scriptNames.name(scriptIndex - 1)) : Json());
    j.set("darkness", darkness);
    j.set("mapNumber", mapNumber);
    j.set("sourceVersion", m.version);

    Json gv = Json::array();
    for (int i = 0; i < globalCount; i++) gv.push(r.i32v());
    j.set("mapVars", gv);
    Json lv = Json::array();
    for (int i = 0; i < localCount; i++) lv.push(r.i32v());
    j.set("localVars", lv);

    Json elevs = Json::array();
    int flaggedTiles = 0;
    for (int e = 0; e < kElevations; e++) {
        if (flags & (2u << e)) {
            elevs.push(Json());
            continue;
        }
        Json floor = Json::array(), roof = Json::array();
        for (int i = 0; i < kSquares; i++) {
            u32 v = r.u32v();
            if ((v & 0xF000F000u) != 0) flaggedTiles++;
            floor.push((int)(v & 0xFFF));
            roof.push((int)((v >> 16) & 0xFFF));
        }
        Json el = Json::object();
        el.set("floor", floor);
        el.set("roof", roof);
        elevs.push(el);
    }
    if (flaggedTiles) LOGI("%s: %d squares had tile flag bits (dropped)", path.c_str(), flaggedTiles);
    j.set("elevations", elevs);

    if (!m.readScripts()) {
        LOGE("%s: script section unreadable", path.c_str());
        return false;
    }
    Json spatials = Json::array();
    for (auto& kv : m.scripts) {
        const RawScript& s = kv.second;
        if ((s.sid >> 24) != SCRIPT_SPATIAL) continue;
        Json sp = Json::object();
        sp.set("script", g_scriptNames.name(s.index));
        sp.set("tile", s.tile & 0x3FFFFFF);
        sp.set("elevation", (s.tile >> 29) & 7);
        sp.set("radius", s.radius);
        spatials.push(sp);
    }
    j.set("spatials", spatials);

    int total = r.i32v();
    Json objects = Json::array();
    for (int e = 0; e < kElevations && !r.bad; e++) {
        int n = r.i32v();
        for (int i = 0; i < n && !r.bad; i++) {
            Json o = m.readObject(0);
            o.set("elevation", e);
            objects.push(o);
        }
    }
    if (r.bad) {
        LOGE("%s: object section truncated", path.c_str());
        return false;
    }
    LOGI("%s: v%d, %d objects (header says %d), %d spatial scripts, script '%s'", path.c_str(), m.version, (int)objects.size(), total,
        (int)spatials.size(), j["script"].str().c_str());
    j.set("objects", objects);
    convWriteText(c, "maps/" + pathStem(name) + ".json", j.dump());
    return true;
}

bool convertMaps(ConvContext& c)
{
    bool ok = true;
    for (auto& e : c.dat.entries()) {
        if (!strStartsWith(e.path, "maps/") || pathExt(e.path) != ".map") continue;
        Bytes data;
        if (!c.dat.read(e, data)) continue;
        ok &= convertOneMap(c, e.path, data);
    }
    return ok;
}

} // namespace fo
