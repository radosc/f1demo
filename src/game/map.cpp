#include "game/map.h"
#include "game/game.h"
#include "game/hex.h"
#include "game/scriptnames.h"
#include "core/fs.h"

namespace fo {

GameMap g_map;

void GameMap::clear()
{
    name.clear();
    displayName.clear();
    scriptIndex = -1;
    spatials.clear();
    for (int e = 0; e < kElevations; e++) {
        hasElevation[e] = false;
        floor[e].assign(kSquares, 1);
        roof[e].assign(kSquares, 1);
        roofHidden[e].assign(kSquares, 0);
    }
}

bool GameMap::load(const std::string& mapName, std::string* error, bool withObjects)
{
    clear();
    std::string text;
    std::string path = "maps/" + strLower(mapName) + ".json";
    if (!g_vfs.readText(path, text)) {
        if (error) *error = "map file not found: " + path;
        return false;
    }
    std::string err;
    Json j = Json::parse(text, &err);
    if (!err.empty()) {
        if (error) *error = path + ": " + err;
        return false;
    }
    name = strLower(mapName);
    displayName = j["displayName"].str(name);
    enterTile = j["entrance"]["tile"].toInt(20100);
    enterElevation = j["entrance"]["elevation"].toInt(0);
    enterRotation = j["entrance"]["rotation"].toInt(0);
    darkness = j["darkness"].toInt(0);
    scriptIndex = j["script"].isString() ? g_scriptNames.indexOf(j["script"].str()) : -1;
    g_game.mapVars.clear();
    for (auto& v : j["mapVars"].items()) g_game.mapVars.push_back(v.toInt());
    g_game.localVars.clear();
    for (auto& v : j["localVars"].items()) g_game.localVars.push_back(v.toInt());

    const Json& elevs = j["elevations"];
    for (int e = 0; e < kElevations; e++) {
        const Json& el = elevs[(size_t)e];
        if (!el.isObject()) continue;
        hasElevation[e] = true;
        const Json& f = el["floor"];
        const Json& r = el["roof"];
        for (int i = 0; i < kSquares && i < (int)f.size(); i++) floor[e][i] = (u16)f[(size_t)i].toInt(1);
        for (int i = 0; i < kSquares && i < (int)r.size(); i++) roof[e][i] = (u16)r[(size_t)i].toInt(1);
    }

    for (auto& s : j["spatials"].items()) {
        MapSpatial sp;
        sp.scriptIndex = g_scriptNames.indexOf(s["script"].str());
        sp.tile = s["tile"].toInt(-1);
        sp.elevation = s["elevation"].toInt(0);
        sp.radius = s["radius"].toInt(0);
        if (sp.scriptIndex < 0) {
            LOGW("Spatial script '%s' unknown", s["script"].str().c_str());
            continue;
        }
        spatials.push_back(sp);
    }

    int created = 0;
    for (auto& oj : j["objects"].items()) {
        if (!withObjects) break;
        std::string oerr;
        Object* o = objectFromJson(g_world, oj, &oerr);
        if (!o) {
            LOGW("Map object skipped: %s", oerr.c_str());
            continue;
        }
        int tile = oj["tile"].toInt(-1);
        int elev = oj["elevation"].toInt(0);
        if (hex::valid(tile)) g_world.place(o, tile, elev);
        created++;
    }
    LOGI("Loaded map %s: %d objects, %d spatials", name.c_str(), created, (int)spatials.size());
    return true;
}

int GameMap::floorAt(int e, int sq) const
{
    if (e < 0 || e >= kElevations || sq < 0 || sq >= kSquares || floor[e].empty()) return 1;
    return floor[e][sq];
}

int GameMap::roofAt(int e, int sq) const
{
    if (e < 0 || e >= kElevations || sq < 0 || sq >= kSquares || roof[e].empty()) return 1;
    return roof[e][sq];
}

void GameMap::floodRoof(int e, int start, u8 value)
{
    std::vector<int> stack;
    stack.push_back(start);
    while (!stack.empty()) {
        int sq = stack.back();
        stack.pop_back();
        if (sq < 0 || sq >= kSquares) continue;
        if (roof[e][sq] <= 1) continue; // 1 = blank tile
        if (roofHidden[e][sq] == value) continue;
        roofHidden[e][sq] = value;
        int x = sq % kSquareW, y = sq / kSquareW;
        if (x > 0) stack.push_back(sq - 1);
        if (x < kSquareW - 1) stack.push_back(sq + 1);
        if (y > 0) stack.push_back(sq - kSquareW);
        if (y < kSquareH - 1) stack.push_back(sq + kSquareW);
    }
}

void GameMap::updateRoofVisibility(int tile, int e)
{
    if (e < 0 || e >= kElevations || roof[e].empty() || !hex::valid(tile)) return;
    int sq = hex::squareOfTile(tile);
    bool under = roof[e][sq] > 1;
    // reveal everything first, then hide the region the player stands under
    static int lastSq[kElevations] = { -1, -1, -1 };
    if (lastSq[e] == sq) return;
    lastSq[e] = sq;
    std::fill(roofHidden[e].begin(), roofHidden[e].end(), (u8)0);
    if (under) floodRoof(e, sq, 1);
}

Json GameMap::toJson(const Json& source) const
{
    Json j = source.isObject() ? source : Json::object();
    j.set("format", "fo1hd-map");
    if (!j.has("version")) j.set("version", 1);
    j.set("name", name);
    j.set("displayName", displayName.empty() ? name : displayName);
    Json entrance = Json::object();
    entrance.set("tile", enterTile);
    entrance.set("elevation", enterElevation);
    entrance.set("rotation", enterRotation);
    j.set("entrance", entrance);
    j.set("script", scriptIndex >= 0 ? Json(g_scriptNames.name(scriptIndex)) : Json());
    j.set("darkness", darkness);
    Json mapVars = Json::array(), localVars = Json::array();
    for (int v : g_game.mapVars) mapVars.push(v);
    for (int v : g_game.localVars) localVars.push(v);
    j.set("mapVars", mapVars);
    j.set("localVars", localVars);
    Json elevations = Json::array();
    for (int e = 0; e < kElevations; e++) {
        if (!hasElevation[e]) {
            elevations.push(Json());
            continue;
        }
        Json el = Json::object(), f = Json::array(), r = Json::array();
        for (int i = 0; i < kSquares; i++) {
            f.push((int)floor[e][i]);
            r.push((int)roof[e][i]);
        }
        el.set("floor", f);
        el.set("roof", r);
        elevations.push(el);
    }
    j.set("elevations", elevations);
    Json sp = Json::array();
    for (auto& s : spatials) {
        Json o = Json::object();
        o.set("script", g_scriptNames.name(s.scriptIndex));
        o.set("tile", s.tile);
        o.set("elevation", s.elevation);
        o.set("radius", s.radius);
        sp.push(o);
    }
    j.set("spatials", sp);
    // the prototype's name after the pid, as a comment for readers (like the converter writes it)
    std::function<Json(const Json&)> named = [&](const Json& src) {
        Json out = Json::object();
        out.set("pid", src["pid"]);
        std::string protoName = g_protos.name((int)src["pid"].i(-1));
        if (!protoName.empty()) out.set("name", protoName);
        for (auto& kv : src.members()) {
            if (kv.first == "pid") continue;
            if (kv.first != "inventory") {
                out.set(kv.first, kv.second);
                continue;
            }
            Json inv = Json::array();
            for (auto& e : kv.second.items()) {
                Json entry = e;
                entry.set("object", named(e["object"]));
                inv.push(entry);
            }
            out.set("inventory", inv);
        }
        return out;
    };
    Json objects = Json::array();
    g_world.forEach([&](Object* o) {
        if (o->tile < 0 || o->owner || o == g_world.dude) return;
        objects.push(named(objectToJson(o, true, false)));
    });
    j.set("objects", objects);
    return j;
}

void GameMap::createBlank(const std::string& mapName, int groundTile)
{
    clear();
    name = strLower(mapName);
    displayName = mapName;
    enterTile = 20100;
    enterElevation = 0;
    enterRotation = 0;
    hasElevation[0] = true;
    floor[0].assign(kSquares, (u16)groundTile);
    g_game.mapVars.clear();
    g_game.localVars.clear();
}

Json GameMap::saveState() const
{
    Json j = Json::object();
    j.set("name", name);
    Json mv = Json::array();
    for (int v : g_game.mapVars) mv.push(v);
    j.set("mapVars", mv);
    return j;
}

} // namespace fo
