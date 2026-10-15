#include "game/game.h"
#include "game/anim.h"
#include "game/art.h"
#include "game/combat.h"
#include "game/critter.h"
#include "game/hex.h"
#include "game/item.h"
#include "game/map.h"
#include "game/msg.h"
#include "game/proto.h"
#include "game/script.h"
#include "game/scriptnames.h"
#include "game/worldview.h"
#include "core/fs.h"

namespace fo {

GameState g_game;

static const int kStartTime = (7 * 60 * 60 + 21 * 60) * 10; // 7:21

int gameTime() { return g_game.time; }

int gameHour()
{
    int t = g_game.time + kStartTime;
    return 100 * ((t / 600) / 60 % 24) + (t / 600) % 60;
}

int gameDay() { return (g_game.time + kStartTime) / kTicksPerDay; }

void displayMessage(const std::string& text, u32 color)
{
    if (text.empty()) return;
    g_game.log.push_back({ text, color });
    if (g_game.log.size() > 200) g_game.log.erase(g_game.log.begin(), g_game.log.begin() + 50);
    LOGI("[msg] %s", text.c_str());
}

int globalVar(int index)
{
    if (index < 0 || index >= (int)g_game.globalVars.size()) return 0;
    return g_game.globalVars[index];
}

void setGlobalVar(int index, int value)
{
    if (index < 0) return;
    if (index >= (int)g_game.globalVars.size()) g_game.globalVars.resize((size_t)index + 1, 0);
    g_game.globalVars[index] = value;
}

// ------------------------------------------------------------------ queue
static std::vector<GameEvent> g_events;

std::vector<GameEvent>& queueEvents() { return g_events; }

void queueAdd(int delay, Object* obj, int type, int d0, int d1, int d2, int d3)
{
    GameEvent e;
    e.time = g_game.time + std::max(0, delay);
    e.type = type;
    e.obj = obj ? obj->uid : 0;
    e.data[0] = d0;
    e.data[1] = d1;
    e.data[2] = d2;
    e.data[3] = d3;
    // keep sorted by time (stable for equal times)
    auto it = std::upper_bound(g_events.begin(), g_events.end(), e, [](const GameEvent& a, const GameEvent& b) { return a.time < b.time; });
    g_events.insert(it, e);
}

void queueRemove(Object* obj, int type)
{
    u32 uid = obj ? obj->uid : 0;
    g_events.erase(std::remove_if(g_events.begin(), g_events.end(), [&](const GameEvent& e) { return e.obj == uid && (type < 0 || e.type == type); }),
        g_events.end());
}

int queueNextTime(Object* obj, int type)
{
    u32 uid = obj ? obj->uid : 0;
    for (auto& e : g_events)
        if (e.obj == uid && e.type == type) return e.time;
    return -1;
}

void drugProcessEvent(const GameEvent& e); // item use module

void queueProcess()
{
    int guard = 0;
    while (!g_events.empty() && g_events.front().time <= g_game.time && guard++ < 1000) {
        GameEvent e = g_events.front();
        g_events.erase(g_events.begin());
        Object* o = g_world.byUid(e.obj);
        switch (e.type) {
        case EV_SCRIPT:
            if (o && o->script) g_scripts.exec(o->script, SP_TIMED, nullptr, nullptr, e.data[0]);
            break;
        case EV_KNOCKOUT:
            if (o && o->isCritter() && !o->isDead()) critterWakeUp(o);
            break;
        case EV_DRUG:
        case EV_WITHDRAWAL:
            drugProcessEvent(e);
            break;
        case EV_MAP_UPDATE:
            g_scripts.mapUpdate();
            break;
        default:
            break;
        }
    }
}

void gameResetState()
{
    int difficulty = g_game.difficulty, combatDifficulty = g_game.combatDifficulty, violence = g_game.violenceLevel;
    g_game = GameState();
    g_game.difficulty = difficulty;
    g_game.combatDifficulty = combatDifficulty;
    g_game.violenceLevel = violence;
    g_events.clear();
    gameLoadGlobals();
}

// ------------------------------------------------------------------ globals
bool gameLoadGlobals()
{
    g_game.globalVars.clear();
    g_game.globalVarNames.clear();
    std::string text;
    if (!g_vfs.readText("data/vault13.gam", text)) {
        LOGW("data/vault13.gam missing - global variables start at 0");
        g_game.globalVars.assign(512, 0);
        return false;
    }
    bool inGlobals = false;
    for (auto& rawLine : strSplit(text, '\n')) {
        std::string line = rawLine;
        size_t c = line.find("//");
        if (c != std::string::npos) line = line.substr(0, c);
        line = strTrim(line);
        if (line.empty()) continue;
        if (strStartsWith(line, "GAME_GLOBAL_VARS:")) {
            inGlobals = true;
            continue;
        }
        if (strStartsWith(line, "MAP_GLOBAL_VARS:")) {
            inGlobals = false;
            continue;
        }
        if (!inGlobals) continue;
        size_t eq = line.find(":=");
        if (eq == std::string::npos) continue;
        std::string name = strTrim(line.substr(0, eq));
        int value = atoi(line.c_str() + eq + 2);
        g_game.globalVarNames.push_back(name);
        g_game.globalVars.push_back(value);
    }
    LOGI("Loaded %d global variables", (int)g_game.globalVars.size());
    return true;
}

// ------------------------------------------------------------------ player
static Proto g_dudeProtoTemplate;

bool gameNewPlayer(const std::string& premade)
{
    std::string text;
    Json j;
    if (g_vfs.readText("premade/" + premade + ".json", text)) j = Json::parse(text);
    else LOGW("premade/%s.json missing - using defaults", premade.c_str());
    PlayerData pl;
    pl.premade = premade;
    pl.name = j["name"].str("None");
    if (pl.name.empty()) pl.name = "None";
    int ti = 0;
    for (auto& t : j["tagSkills"].items()) {
        int s = nameIndex(kSkillNames, SKILL_COUNT, t.str(), -1);
        if (s >= 0 && ti < 4) pl.tags[ti++] = s;
    }
    int tr = 0;
    for (auto& t : j["traits"].items()) {
        int s = nameIndex(kTraitNames, TRAIT_COUNT, t.str(), -1);
        if (s >= 0 && tr < 2) pl.traits[tr++] = s;
    }
    pl.bodyType = j["bodyType"].toInt(2);
    g_game.player = pl;

    // player prototype (pid 0x01000000) lives only in memory
    Proto p;
    p.pid = 0x01000000;
    p.messageId = 0;
    p.fid = makeFid(OBJ_CRITTER, 11);
    p.flags = OF_LIGHT_THRU;
    p.extFlags = PXF_CAN_LOOK | PXF_CAN_TALK;
    p.team = 0;
    p.aiPacket = 0;
    for (int i = 0; i < STAT_SAVEABLE_COUNT; i++) {
        p.baseStats[i] = j["stats"][kStatNames[i]].toInt(i < STAT_PRIMARY_COUNT ? 5 : 0);
        p.bonusStats[i] = j["bonusStats"][kStatNames[i]].toInt(0);
    }
    for (int i = 0; i < SKILL_COUNT; i++) p.skills[i] = j["skills"][kSkillNames[i]].toInt(0);
    g_protos.put(p);

    if (g_world.dude) {
        g_world.destroy(g_world.dude);
        g_world.collectGarbage();
    }
    Object* dude = g_world.create(0x01000000, p.fid);
    dude->flags |= OF_NO_REMOVE | OF_LIGHT_THRU;
    dude->critter.team = 0;
    dude->critter.hp = critterStat(dude, STAT_MAX_HP);
    dude->critter.ap = critterStat(dude, STAT_MAX_AP);
    dude->lightRadius = 4;
    dude->lightIntensity = 65536 / 4; // a little light around the player in the dark (like the original)
    dude->flags |= OF_LIGHTING;
    g_world.dude = dude;
    critterUpdateArt(dude);
    // obj_dude script
    int sidx = g_scriptNames.indexOf("obj_dude");
    if (sidx >= 0) dude->scriptIndex = sidx;
    return true;
}

void gameSetElevation(int e)
{
    if (e < 0 || e >= kElevations) return;
    g_game.elevation = e;
}

// Called for every hex a moving object enters: spatial script triggers, roofs over the player.
static void installMovementHooks()
{
    g_animOnTileChanged = [](Object* o) {
        if (!o || o->tile < 0) return;
        g_scripts.checkSpatials(o, o->tile, o->elevation);
        if (o == g_world.dude) g_map.updateRoofVisibility(o->tile, o->elevation);
    };
}

bool gameEnterMap(const std::string& mapName, int tile, int elevation, int rotation)
{
    installMovementHooks();
    Object* dude = g_world.dude;
    if (g_map.loaded()) g_scripts.mapExit();
    animStopAll();
    combatEnd(true);
    g_scripts.clear();
    g_world.clearExcept(dude);
    g_events.erase(std::remove_if(g_events.begin(), g_events.end(), [&](const GameEvent& e) { return e.obj != 0 && (!dude || e.obj != dude->uid); }),
        g_events.end());
    g_view.floats.clear();
    std::string err;
    if (!g_map.load(mapName, &err)) {
        LOGE("Map load failed: %s", err.c_str());
        return false;
    }
    g_game.mapName = g_map.name;
    g_game.mapSaved = false;
    g_game.lightLevel = 65536;
    if (dude) {
        int t = tile >= 0 ? tile : g_map.enterTile;
        int e = elevation >= 0 ? elevation : g_map.enterElevation;
        int r = rotation >= 0 ? rotation : g_map.enterRotation;
        g_world.place(dude, t, e);
        dude->rotation = r;
        dude->frame = 0;
        gameSetElevation(e);
        g_view.centerOn(t);
        g_map.updateRoofVisibility(t, e);
    }
    g_scripts.attachAll();
    if (dude && dude->scriptIndex >= 0 && !dude->script) g_scripts.attach(dude, dude->scriptIndex);
    g_scripts.mapEnter();
    queueAdd(600, nullptr, EV_MAP_UPDATE);
    g_game.lastMapUpdate = g_game.time;
    g_game.running = true;
    return true;
}

void gameEnd()
{
    animStopAll();
    combatEnd(true);
    g_scripts.clear();
    g_world.clear();
    g_map.clear();
    g_events.clear();
    g_game.running = false;
}

std::function<void()> g_onGameOver;

void gameRequestMap(const std::string& mapName, int entrance)
{
    g_game.pendingMap = mapName;
    g_game.pendingMapEntrance = entrance;
}

void gameRequestDialog(Object* speaker) { g_game.pendingDialog = speaker ? speaker->uid : 0; }

void gameFade(bool out) { g_game.fade = out ? 1.0f : 0.0f; }

void gameTalkTo(Object* speaker)
{
    if (!speaker || !speaker->script || !g_world.dude) return;
    if (speaker->isCritter() && !critterIsActive(speaker)) return;
    if (inCombat()) return;
    g_scripts.execObject(speaker, SP_TALK, g_world.dude, nullptr, 0);
}

static void processRequests()
{
    if (g_game.pendingDialog) {
        Object* speaker = g_world.byUid(g_game.pendingDialog);
        g_game.pendingDialog = 0;
        if (speaker) gameTalkTo(speaker);
    }
    if (!g_game.pendingMap.empty()) {
        std::string name = g_game.pendingMap;
        g_game.pendingMap.clear();
        if (!gameEnterMap(name)) LOGE("load_map: could not load %s", name.c_str());
    }
    if (g_game.demoOver) {
        g_game.demoOver = false;
        if (g_onGameOver) g_onGameOver();
    }
}

void gameUpdate(float dt)
{
    static float timeAcc = 0, mapUpdateAcc = 0;
    if (!g_game.running) return;
    processRequests();
    if (!g_game.running) return;
    if (!inCombat()) {
        timeAcc += dt;
        while (timeAcc >= 0.1f) {
            timeAcc -= 0.1f;
            g_game.time++;
        }
    }
    queueProcess();
    // natural healing: each new day the player heals by the healing rate
    if (Object* d = g_world.dude) {
        int day = gameDay();
        if (g_game.player.healDay < 0) g_game.player.healDay = day;
        if (day > g_game.player.healDay && !inCombat()) {
            if (!d->isDead() && d->critter.hp < critterStat(d, STAT_MAX_HP)) critterAdjustHp(d, critterStat(d, STAT_HEAL_RATE) * (day - g_game.player.healDay));
            g_game.player.healDay = day;
        }
    }
    // periodic map update script (every 30 s real time like the original background process)
    mapUpdateAcc += dt;
    if (mapUpdateAcc >= 30.0f) {
        mapUpdateAcc = 0;
        g_scripts.mapUpdate();
    }
    if (!inCombat()) g_scripts.heartbeat();
    animUpdate(dt);
    combatUpdate(dt);
}

bool inCombat() { return combatActive(); }
Object* combatTurnObject() { return combatWhoseTurn(); }

} // namespace fo
