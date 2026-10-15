// Save games: <user dir>/saves/slotNN/save.json (+ thumbnail.png).
// The whole dynamic state is stored as readable JSON: objects with their runtime uids (scripts keep object
// handles in variables), script local variables, timed events, player data and global/map variables.
// Map tiles are not saved; they are reloaded from the map file.
#include "game/game.h"
#include "core/fs.h"
#include "core/json.h"
#include "core/png.h"
#include "game/anim.h"
#include "game/combat.h"
#include "game/critter.h"
#include "game/dialog.h"
#include "game/hex.h"
#include "game/item.h"
#include "game/map.h"
#include "game/proto.h"
#include "game/script.h"
#include "game/scriptnames.h"
#include "game/worldview.h"
#include "ui/app.h"

#include <ctime>

namespace fo {

namespace {

Image g_thumbnail;
int g_lastSlot = 0;

std::string slotDir(int slot) { return pathJoin(pathJoin(g_app.userDir.empty() ? exeDir() : g_app.userDir, "saves"), strFormat("slot%02d", slot + 1)); }

std::string gameDateString()
{
    static const char* kMonths[12] = { "JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC" };
    static const int kDays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int day = gameDay() + 4, month = 11, year = 2161;
    while (day >= kDays[month]) {
        day -= kDays[month];
        if (++month == 12) {
            month = 0;
            year++;
        }
    }
    return strFormat("%s %d, %d %04d", kMonths[month], day + 1, year, gameHour());
}

std::string nowString()
{
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
    return buf;
}

Json intArray(const int* v, int n)
{
    Json a = Json::array();
    for (int i = 0; i < n; i++) a.push(v[i]);
    return a;
}

Json intVector(const std::vector<int>& v)
{
    Json a = Json::array();
    for (int x : v) a.push(x);
    return a;
}

// Identifies a script instance across save/load: by owner uid, or as the map / a spatial script.
Json scriptKey(ScriptInstance* s)
{
    Json k = Json::object();
    if (s == g_scripts.mapScript()) {
        k.set("kind", "map");
    } else if (s->spatial) {
        int idx = -1;
        for (int i = 0; i < (int)g_map.spatials.size(); i++)
            if (&g_map.spatials[i] == s->spatial) idx = i;
        k.set("kind", "spatial");
        k.set("index", idx);
    } else {
        k.set("kind", "object");
        k.set("owner", (i64)s->ownerUid);
    }
    return k;
}

ScriptInstance* findScript(const Json& k)
{
    std::string kind = k["kind"].str();
    if (kind == "map") return g_scripts.mapScript();
    if (kind == "spatial") {
        int idx = k["index"].toInt(-1);
        if (idx >= 0 && idx < (int)g_map.spatials.size()) return g_map.spatials[idx].script;
        return nullptr;
    }
    Object* o = g_world.byUid((u32)k["owner"].i(0));
    return o ? o->script : nullptr;
}

void writeThumbnail(const std::string& dir)
{
    if (g_thumbnail.width <= 0) return;
    Bytes png = pngEncode(g_thumbnail, 6);
    fileWrite(pathJoin(dir, "thumbnail.png"), png.data(), png.size());
}

} // namespace

// Called by the game screen right before opening the options menu (the screen still shows the world).
void gameCaptureThumbnail(const Image& screen)
{
    // 4:3 crop of the window, box-filtered down to 160x120
    const int tw = 160, th = 120;
    if (screen.width <= 0 || screen.height <= 0 || screen.indexed) return;
    int cw = std::min(screen.width, screen.height * 4 / 3), ch = cw * 3 / 4;
    int ox = (screen.width - cw) / 2, oy = (screen.height - ch) / 2;
    g_thumbnail.allocRGBA(tw, th);
    for (int y = 0; y < th; y++) {
        for (int x = 0; x < tw; x++) {
            int x0 = ox + x * cw / tw, x1 = std::max(x0 + 1, ox + (x + 1) * cw / tw);
            int y0 = oy + y * ch / th, y1 = std::max(y0 + 1, oy + (y + 1) * ch / th);
            u32 acc[3] = {}, n = 0;
            for (int sy = y0; sy < y1; sy++)
                for (int sx = x0; sx < x1; sx++) {
                    const u8* p = screen.row(sy) + sx * 4;
                    acc[0] += p[0];
                    acc[1] += p[1];
                    acc[2] += p[2];
                    n++;
                }
            u8* d = g_thumbnail.row(y) + x * 4;
            for (int k = 0; k < 3; k++) d[k] = (u8)(acc[k] / std::max(1u, n));
            d[3] = 255;
        }
    }
}

std::vector<SaveSlotInfo> gameListSaves()
{
    std::vector<SaveSlotInfo> v;
    for (int i = 0; i < kSaveSlots; i++) {
        SaveSlotInfo s;
        s.slot = i;
        std::string dir = slotDir(i);
        Bytes data;
        if (fileRead(pathJoin(dir, "save.json"), data)) {
            Json j = Json::parse(std::string(data.begin(), data.end()));
            if (j["format"].str() == "fo1hd-save") {
                const Json& info = j["info"];
                s.used = true;
                s.description = info["description"].str();
                s.playerName = info["playerName"].str();
                s.mapName = info["mapName"].str();
                s.gameDate = info["gameDate"].str();
                s.savedAt = info["savedAt"].str();
                std::string thumb = pathJoin(dir, "thumbnail.png");
                if (fileExists(thumb)) s.thumbnail = thumb;
            }
        }
        v.push_back(s);
    }
    return v;
}

int gameLastSaveSlot() { return g_lastSlot; }

bool gameSave(int slot, const std::string& description)
{
    Object* dude = g_world.dude;
    if (slot < 0 || slot >= kSaveSlots || !g_game.running || !dude) return false;
    if (inCombat() || g_dialog.active()) {
        LOGW("Cannot save during combat or dialogue");
        return false;
    }
    Json root = Json::object();
    root.set("format", "fo1hd-save");
    root.set("version", 1);

    Json info = Json::object();
    info.set("description", description);
    info.set("playerName", g_game.player.name);
    info.set("mapName", g_game.mapName);
    info.set("gameDate", gameDateString());
    info.set("savedAt", nowString());
    root.set("info", info);

    Json game = Json::object();
    game.set("time", g_game.time);
    game.set("elevation", g_game.elevation);
    game.set("activeHand", g_game.activeHand);
    game.set("handMode", intArray(g_game.handMode, 2));
    game.set("lightLevel", g_game.lightLevel);
    game.set("globalVars", intVector(g_game.globalVars));
    game.set("mapVars", intVector(g_game.mapVars));
    Json log = Json::array();
    int first = std::max(0, (int)g_game.log.size() - 50);
    for (int i = first; i < (int)g_game.log.size(); i++) {
        Json l = Json::object();
        l.set("text", g_game.log[i].text);
        l.set("color", (i64)g_game.log[i].color);
        log.push(l);
    }
    game.set("log", log);
    root.set("game", game);

    const PlayerData& pl = g_game.player;
    Json player = Json::object();
    player.set("name", pl.name);
    player.set("tags", intArray(pl.tags, 4));
    player.set("traits", intArray(pl.traits, 2));
    player.set("perks", intArray(pl.perks, PERK_COUNT));
    player.set("level", pl.level);
    player.set("experience", pl.experience);
    player.set("unspentSkillPoints", pl.unspentSkillPoints);
    player.set("karma", pl.karma);
    player.set("reputation", pl.reputation);
    player.set("levelUpPending", pl.levelUpPending);
    player.set("freePerks", pl.freePerks);
    player.set("healDay", pl.healDay);
    player.set("premade", pl.premade);
    player.set("bodyType", pl.bodyType);
    player.set("kills", intArray(pl.kills, 16));
    root.set("player", player);
    if (const Proto* pp = g_protos.get(dude->pid)) root.set("playerProto", pp->toJson(pl.name));

    Json map = Json::object();
    map.set("name", g_map.name);
    root.set("map", map);
    root.set("dude", (i64)dude->uid);

    Json objects = Json::array();
    g_world.forEach([&](Object* o) {
        if (o->owner || (o->flags & OF_NO_SAVE)) return;
        objects.push(objectToJson(o, true, true));
    });
    root.set("objects", objects);

    Json scripts = Json::array();
    for (ScriptInstance* s : g_scripts.all()) {
        if (!s || s->localVars.empty()) continue;
        Json e = Json::object();
        e.set("key", scriptKey(s));
        e.set("localVars", intVector(s->localVars));
        scripts.push(e);
    }
    root.set("scripts", scripts);

    Json events = Json::array();
    for (const GameEvent& ev : queueEvents()) {
        Json e = Json::object();
        e.set("time", ev.time);
        e.set("type", ev.type);
        e.set("obj", (i64)ev.obj);
        e.set("data", intArray(ev.data, 4));
        events.push(e);
    }
    root.set("events", events);

    std::string dir = slotDir(slot);
    dirCreate(dir);
    std::string text = root.dump(1);
    if (!fileWrite(pathJoin(dir, "save.json"), text.data(), text.size())) {
        LOGE("Could not write save game to %s", dir.c_str());
        return false;
    }
    writeThumbnail(dir);
    g_lastSlot = slot;
    LOGI("Game saved to slot %d (%d objects)", slot + 1, (int)objects.size());
    return true;
}

bool gameLoad(int slot)
{
    if (slot < 0 || slot >= kSaveSlots) return false;
    Bytes data;
    std::string dir = slotDir(slot);
    if (!fileRead(pathJoin(dir, "save.json"), data)) return false;
    std::string err;
    Json root = Json::parse(std::string(data.begin(), data.end()), &err);
    if (!err.empty() || root["format"].str() != "fo1hd-save") {
        LOGE("Save game %d is damaged: %s", slot + 1, err.c_str());
        return false;
    }

    // tear down the current session
    if (g_dialog.inGameDialog()) g_dialog.end();
    if (g_map.loaded()) gameEnd();
    g_world.clear();
    g_view.floats.clear();

    // player prototype first (the player object refers to it)
    const Json& player = root["player"];
    PlayerData pl;
    pl.name = player["name"].str("None");
    for (int i = 0; i < 4; i++) pl.tags[i] = player["tags"][(size_t)i].toInt(-1);
    for (int i = 0; i < 2; i++) pl.traits[i] = player["traits"][(size_t)i].toInt(-1);
    for (int i = 0; i < PERK_COUNT; i++) pl.perks[i] = player["perks"][(size_t)i].toInt(0);
    pl.level = player["level"].toInt(1);
    pl.experience = player["experience"].toInt(0);
    pl.unspentSkillPoints = player["unspentSkillPoints"].toInt(0);
    pl.karma = player["karma"].toInt(0);
    pl.reputation = player["reputation"].toInt(0);
    pl.levelUpPending = player["levelUpPending"].b(false);
    pl.freePerks = player["freePerks"].toInt(0);
    pl.healDay = player["healDay"].toInt(-1);
    pl.premade = player["premade"].str();
    pl.bodyType = player["bodyType"].toInt(2);
    for (int i = 0; i < 16; i++) pl.kills[i] = player["kills"][(size_t)i].toInt(0);
    g_game.player = pl;
    if (root.has("playerProto")) {
        Proto p;
        if (p.fromJson(root["playerProto"])) g_protos.put(p);
    }

    // map tiles and metadata (objects come from the save)
    std::string mapName = root["map"]["name"].str();
    if (!g_map.load(mapName, &err, false)) {
        LOGE("Save game %d: map %s: %s", slot + 1, mapName.c_str(), err.c_str());
        return false;
    }
    g_game.mapName = g_map.name;

    const Json& game = root["game"];
    g_game.time = game["time"].toInt(0);
    g_game.elevation = game["elevation"].toInt(0);
    g_game.activeHand = game["activeHand"].toInt(1);
    for (int i = 0; i < 2; i++) g_game.handMode[i] = game["handMode"][(size_t)i].toInt(0);
    g_game.lightLevel = game["lightLevel"].toInt(65536);
    g_game.globalVars.clear();
    for (auto& v : game["globalVars"].items()) g_game.globalVars.push_back(v.toInt());
    g_game.mapVars.clear();
    for (auto& v : game["mapVars"].items()) g_game.mapVars.push_back(v.toInt());
    g_game.log.clear();
    for (auto& l : game["log"].items()) g_game.log.push_back({ l["text"].str(), (u32)l["color"].i(0xFF00FF00) });
    g_game.uiDisabled = false;
    g_game.fade = 0;
    g_game.pendingMap.clear();
    g_game.pendingDialog = 0;

    // objects (uids are restored so script handles stay valid)
    int count = 0;
    for (auto& oj : root["objects"].items()) {
        std::string oerr;
        Object* o = objectFromJson(g_world, oj, &oerr);
        if (!o) {
            LOGW("Save game object skipped: %s", oerr.c_str());
            continue;
        }
        int tile = oj["tile"].toInt(-1);
        if (hex::valid(tile)) g_world.place(o, tile, oj["elevation"].toInt(0));
        count++;
    }
    g_world.dude = g_world.byUid((u32)root["dude"].i(0));
    Object* dude = g_world.dude;
    if (!dude) {
        LOGE("Save game %d has no player object", slot + 1);
        return false;
    }
    dude->flags |= OF_NO_REMOVE;
    critterUpdateArt(dude);
    critterStand(dude, dude->rotation);
    g_world.forEach([](Object* o) {
        if (o->isCritter() && o != g_world.dude && !o->isDead()) critterUpdateArt(o);
    });

    // scripts + their local variables
    g_scripts.attachAll();
    int dudeScript = g_scriptNames.indexOf("obj_dude");
    if (dudeScript >= 0 && !dude->script) g_scripts.attach(dude, dudeScript);
    for (auto& e : root["scripts"].items()) {
        ScriptInstance* s = findScript(e["key"]);
        if (!s) continue;
        s->localVars.clear();
        for (auto& v : e["localVars"].items()) s->localVars.push_back(v.toInt());
    }

    // timed events
    queueEvents().clear();
    for (auto& e : root["events"].items()) {
        GameEvent ev;
        ev.time = e["time"].toInt(0);
        ev.type = e["type"].toInt(0);
        ev.obj = (u32)e["obj"].i(0);
        for (int i = 0; i < 4; i++) ev.data[i] = e["data"][(size_t)i].toInt(0);
        queueEvents().push_back(ev);
    }
    std::stable_sort(queueEvents().begin(), queueEvents().end(), [](const GameEvent& a, const GameEvent& b) { return a.time < b.time; });

    gameSetElevation(dude->elevation);
    g_view.centerOn(dude->tile);
    g_map.updateRoofVisibility(dude->tile, dude->elevation);

    g_animOnTileChanged = [](Object* o) {
        if (!o || o->tile < 0) return;
        g_scripts.checkSpatials(o, o->tile, o->elevation);
        if (o == g_world.dude) g_map.updateRoofVisibility(o->tile, o->elevation);
    };
    // map scripts run their map-enter procedure like the original does after loading
    g_game.mapSaved = true;
    g_game.loadingGame = true;
    g_game.running = true;
    g_scripts.mapEnter(false);
    g_game.loadingGame = false;
    g_game.lastMapUpdate = g_game.time;
    g_lastSlot = slot;
    LOGI("Loaded save game %d (%d objects)", slot + 1, count);
    return true;
}

} // namespace fo
