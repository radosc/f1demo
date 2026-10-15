// Global game state shared by all gameplay modules.
#pragma once
#include "game/defs.h"
#include "game/object.h"

namespace fo {

struct PlayerData {
    std::string name = "None";
    int tags[4] = { -1, -1, -1, -1 };
    int traits[2] = { -1, -1 };
    int perks[PERK_COUNT] = {};
    int level = 1;
    int experience = 0;
    int unspentSkillPoints = 0;
    int karma = 0;
    int reputation = 0;
    bool levelUpPending = false;
    int freePerks = 0;         // perks earned at level ups and not chosen yet
    int firstAidUses = 0, doctorUses = 0;
    int healDay = -1;          // day index of the last first aid/doctor uses
    std::string premade;       // file the character came from
    int bodyType = 2;
    int kills[16] = {};        // kills per critter kill type (proto killType)
};

struct MessageLine {
    std::string text;
    u32 color = 0xFF00FF00u;
};

struct GameState {
    int time = 0;               // game ticks (1/10 s) since start
    int startTimeOffset = 0;
    std::vector<int> globalVars;
    std::vector<std::string> globalVarNames;
    std::vector<int> mapVars;
    std::vector<int> localVars;
    PlayerData player;
    std::vector<MessageLine> log;
    int difficulty = 1;         // 0 easy, 1 normal, 2 hard
    int combatDifficulty = 1;
    int violenceLevel = 3;
    bool running = false;       // a game session is active
    bool uiDisabled = false;    // game_ui_disable
    std::string mapName;        // "junkdemo"
    int mapIndex = 0;
    int elevation = 0;
    int lightLevel = 65536;     // ambient light 0..65536
    int lastMapUpdate = 0;
    bool demoOver = false;
    int activeHand = 1;         // 0 = left, 1 = right (interface item slot)
    int handMode[2] = { 0, 0 }; // 0 = primary, 1 = secondary, 2 = aimed primary, 3 = reload
    float fade = 0;             // 0 = normal, 1 = black (gfade_out)
    bool mapSaved = false;      // the current map state came from a save game (metarule FIRST_RUN = false)
    bool loadingGame = false;   // a save game is being restored (metarule IS_LOADGAME)

    // requests made by scripts, carried out by gameUpdate once no script or dialogue is running
    std::string pendingMap;
    int pendingMapEntrance = -1;
    u32 pendingDialog = 0;      // speaker uid (dialogue_system_enter)
};

extern GameState g_game;

// ------------------------------------------------------------------ timed events
enum EventType : int {
    EV_SCRIPT = 0,      // timed_event_p_proc: data[0] = fixed param
    EV_KNOCKOUT = 1,    // critter recovers from knock out
    EV_DRUG = 2,        // drug effect stage: data[0] = proto pid, data[1] = stage
    EV_WITHDRAWAL = 3,
    EV_POISON = 4,
    EV_RADIATION = 5,
    EV_MAP_UPDATE = 6,
    EV_HEAL = 7,        // natural healing tick
};

struct GameEvent {
    int time = 0;
    int type = 0;
    u32 obj = 0;        // object uid (0 = none)
    int data[4] = {};
};

void queueAdd(int delayTicks, Object* obj, int type, int d0 = 0, int d1 = 0, int d2 = 0, int d3 = 0);
void queueRemove(Object* obj, int type = -1);
int queueNextTime(Object* obj, int type);
void queueProcess();
std::vector<GameEvent>& queueEvents();

// ------------------------------------------------------------------ session
bool gameLoadGlobals();
// Fresh session state for NEW GAME: clock, global variables, message log, requests (keeps preferences).
void gameResetState();
// Creates a fresh player from data/premade/<name>.json
bool gameNewPlayer(const std::string& premade);
// Loads a map and places the player (tile < 0 = map entrance). Runs map scripts.
bool gameEnterMap(const std::string& mapName, int tile = -1, int elevation = -1, int rotation = -1);
// Per-frame simulation (time, queue, script heartbeat, animations).
void gameUpdate(float dt);
void gameEnd();
void gameSetElevation(int e);
// Deferred requests (load_map, dialogue_system_enter, gfade_*).
void gameRequestMap(const std::string& mapName, int entrance = -1);
void gameRequestDialog(Object* speaker);
void gameFade(bool out);
// Starts a conversation with `speaker` right away (runs its talk procedure).
void gameTalkTo(Object* speaker);
// UI hook: called when the demo ends (metarule END_GAME / endgame opcodes) or the player dies.
extern std::function<void()> g_onGameOver;

// ------------------------------------------------------------------ save games (user/saves/slotNN)
struct SaveSlotInfo {
    int slot = 0;
    bool used = false;
    std::string description;   // player-entered comment
    std::string playerName;
    std::string mapName;
    std::string gameDate;      // e.g. "DEC 5, 2161 0721"
    std::string savedAt;       // real time stamp
    std::string thumbnail;     // absolute path of a PNG thumbnail (may be empty)
};
constexpr int kSaveSlots = 10;
std::vector<SaveSlotInfo> gameListSaves();
bool gameSave(int slot, const std::string& description);
bool gameLoad(int slot);
// Quick save / load use the slot of the last save or load (F6 / F9 in the port).
int gameLastSaveSlot();
// Screen capture used as the next save game's thumbnail (taken before the options menu opens).
void gameCaptureThumbnail(const struct Image& screen);

int gameTime();
int gameHour();                 // HHMM, e.g. 1430
int gameDay();
void displayMessage(const std::string& text, u32 color = 0xFF00FF00u);
bool inCombat();
Object* combatTurnObject();
int globalVar(int index);
void setGlobalVar(int index, int value);

} // namespace fo
