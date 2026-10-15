// Script instances attached to objects / the map, and event dispatch into the VM.
#pragma once
#include "game/object.h"
#include "game/vm.h"

namespace fo {

struct MapSpatial;

struct ScriptInstance {
    int index = -1;             // scripts.lst index
    int type = SCRIPT_ITEM;     // SCRIPT_*
    u32 ownerUid = 0;           // owner object (critter/item/scenery; hidden helper object for map/spatial scripts)
    MapSpatial* spatial = nullptr;
    std::unique_ptr<Vm> vm;
    int procs[SP_COUNT];
    bool loaded = false;
    bool disabled = false;

    // context of the current call
    int action = 0;
    int fixedParam = 0;
    u32 source = 0, target = 0;
    int actionBeingUsed = -1;
    bool overrides = false;
    int howMuch = 0;
    std::vector<int> localVars;

    Object* owner() const;
    std::string name() const;
};

class ScriptSystem {
public:
    void clear();
    // Creates (lazily loaded) scripts for every scripted object + map + spatials of the current map.
    void attachAll();
    ScriptInstance* attach(Object* obj, int scriptIndex);
    void detach(Object* obj);
    ScriptInstance* forObject(Object* obj) const { return obj ? obj->script : nullptr; }
    ScriptInstance* mapScript() const { return m_mapScript; }

    // Executes `proc` (script_action). Returns true if the script called script_overrides().
    bool exec(ScriptInstance* s, int proc, Object* source = nullptr, Object* target = nullptr, int fixedParam = -9999,
        int actionBeingUsed = -1);
    bool execObject(Object* obj, int proc, Object* source = nullptr, Object* target = nullptr, int fixedParam = -9999,
        int actionBeingUsed = -1);

    // Map lifecycle
    // Runs map_enter_p_proc everywhere; `fresh` = the map was not restored from a save (fixed param 1).
    void mapEnter(bool fresh = true);
    void mapUpdate();
    void mapExit();
    void loadAll();
    // Called every frame (outside combat/dialog): runs critter_p_proc for one critter in turn.
    void heartbeat();
    void checkSpatials(Object* mover, int tile, int elevation);
    void setSpatialsEnabled(bool on) { m_spatialsEnabled = on; }

    ScriptInstance* current() const { return m_stack.empty() ? nullptr : m_stack.back(); }
    std::vector<ScriptInstance*> all() const;
    // Program cache (shared bytecode); `name` without extension.
    std::shared_ptr<ProgramData> program(const std::string& name);
    void flushPrograms() { m_programs.clear(); }

private:
    bool load(ScriptInstance* s);
    std::vector<std::unique_ptr<ScriptInstance>> m_scripts;
    ScriptInstance* m_mapScript = nullptr;
    std::vector<ScriptInstance*> m_stack;
    std::unordered_map<std::string, std::shared_ptr<ProgramData>> m_programs;
    size_t m_heartbeatIndex = 0;
    bool m_spatialsEnabled = true;
};

extern ScriptSystem g_scripts;

ScriptInstance* scriptOf(Vm& vm);
void registerGameOpcodes(); // opcodes.cpp

} // namespace fo
