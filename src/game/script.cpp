#include "game/script.h"
#include "game/game.h"
#include "game/hex.h"
#include "game/map.h"
#include "game/scriptnames.h"
#include "core/fs.h"

namespace fo {

ScriptSystem g_scripts;

Object* ScriptInstance::owner() const { return g_world.byUid(ownerUid); }
std::string ScriptInstance::name() const { return g_scriptNames.name(index); }

ScriptInstance* scriptOf(Vm& vm) { return (ScriptInstance*)vm.userData; }

void ScriptSystem::clear()
{
    for (auto& s : m_scripts) {
        if (Object* o = s->owner()) o->script = nullptr;
    }
    m_scripts.clear();
    m_mapScript = nullptr;
    m_stack.clear();
    m_heartbeatIndex = 0;
}

std::shared_ptr<ProgramData> ScriptSystem::program(const std::string& name)
{
    std::string key = strLower(name);
    auto it = m_programs.find(key);
    if (it != m_programs.end()) return it->second;
    Bytes b;
    if (!g_vfs.read("scripts/" + key + ".int", b)) {
        LOGW("Script program scripts/%s.int not found", key.c_str());
        m_programs[key] = nullptr;
        return nullptr;
    }
    auto p = std::make_shared<ProgramData>();
    std::string err;
    if (!p->parse(key, std::move(b), &err)) {
        LOGE("Script %s: %s", key.c_str(), err.c_str());
        m_programs[key] = nullptr;
        return nullptr;
    }
    m_programs[key] = p;
    return p;
}

ScriptInstance* ScriptSystem::attach(Object* obj, int scriptIndex)
{
    if (!obj || scriptIndex < 0) return nullptr;
    if (obj->script) detach(obj);
    auto s = std::make_unique<ScriptInstance>();
    s->index = scriptIndex;
    s->type = obj->isCritter() ? SCRIPT_CRITTER : SCRIPT_ITEM;
    s->ownerUid = obj->uid;
    const ScriptListEntry* e = g_scriptNames.get(scriptIndex);
    if (e && e->localVars > 0) s->localVars.assign((size_t)e->localVars, 0);
    obj->script = s.get();
    obj->scriptIndex = scriptIndex;
    ScriptInstance* raw = s.get();
    m_scripts.push_back(std::move(s));
    return raw;
}

void ScriptSystem::detach(Object* obj)
{
    if (!obj || !obj->script) return;
    ScriptInstance* s = obj->script;
    obj->script = nullptr;
    for (auto& p : m_stack)
        if (p == s) return; // currently executing: keep it alive, it is orphaned
    m_scripts.erase(std::remove_if(m_scripts.begin(), m_scripts.end(), [&](const std::unique_ptr<ScriptInstance>& x) { return x.get() == s; }),
        m_scripts.end());
}

void ScriptSystem::attachAll()
{
    g_world.forEach([&](Object* o) {
        if (o->scriptIndex >= 0 && !o->script) attach(o, o->scriptIndex);
        // inventory items with scripts
        std::function<void(Object*)> visit = [&](Object* c) {
            for (auto& it : c->inventory) {
                if (it.obj->scriptIndex >= 0 && !it.obj->script) attach(it.obj, it.obj->scriptIndex);
                visit(it.obj);
            }
        };
        visit(o);
    });
    if (g_map.scriptIndex >= 0) {
        // hidden helper object owning the map script (like the original)
        Object* holder = g_world.create(0x05000000 | 0, makeFid(OBJ_MISC, 12));
        holder->flags |= OF_HIDDEN | OF_NO_SAVE | OF_LIGHT_THRU;
        g_world.place(holder, 1, 0);
        m_mapScript = attach(holder, g_map.scriptIndex);
        if (m_mapScript) m_mapScript->type = SCRIPT_SYSTEM;
    }
    for (auto& sp : g_map.spatials) {
        Object* holder = g_world.create(0x05000000 | 0, makeFid(OBJ_MISC, 12));
        holder->flags |= OF_HIDDEN | OF_NO_SAVE | OF_LIGHT_THRU | OF_NO_BLOCK | OF_FLAT;
        g_world.place(holder, sp.tile, sp.elevation);
        sp.script = attach(holder, sp.scriptIndex);
        if (sp.script) {
            sp.script->type = SCRIPT_SPATIAL;
            sp.script->spatial = &sp;
        }
    }
}

bool ScriptSystem::load(ScriptInstance* s)
{
    if (s->loaded) return true;
    s->loaded = true;
    std::string nm = s->name();
    auto prog = program(nm);
    if (!prog) {
        s->disabled = true;
        return false;
    }
    s->vm = std::make_unique<Vm>(prog);
    s->vm->userData = s;
    for (int i = 0; i < SP_COUNT; i++) s->procs[i] = prog->findProc(scriptProcName(i));
    return true;
}

bool ScriptSystem::exec(ScriptInstance* s, int proc, Object* source, Object* target, int fixedParam, int actionBeingUsed)
{
    if (!s || s->disabled) return false;
    if (m_stack.size() > 32) {
        LOGW("Script recursion too deep, skipping %s", s->name().c_str());
        return false;
    }
    bool firstRun = !s->loaded;
    if (!load(s)) return false;
    if (s->vm->failed()) {
        s->disabled = true;
        return false;
    }
    s->action = proc;
    s->overrides = false;
    s->source = source ? source->uid : 0;
    Object* owner = s->owner();
    s->target = target ? target->uid : (owner ? owner->uid : 0);
    if (fixedParam != -9999) s->fixedParam = fixedParam;
    s->actionBeingUsed = actionBeingUsed;
    m_stack.push_back(s);
    bool ok;
    if (firstRun) {
        // The first execution runs the program from the top: globals init + "start" with this action.
        ok = s->vm->runInit();
    } else {
        int idx = (proc >= 0 && proc < SP_COUNT) ? s->procs[proc] : -1;
        if (idx <= 0) idx = s->procs[SP_START] >= 0 ? s->procs[SP_START] : 1;
        ok = s->vm->callProc(idx);
    }
    m_stack.pop_back();
    if (!ok && s->vm->failed()) {
        LOGE("Script %s disabled after an error", s->name().c_str());
        s->disabled = true;
    }
    s->source = 0;
    return s->overrides;
}

bool ScriptSystem::execObject(Object* obj, int proc, Object* source, Object* target, int fixedParam, int actionBeingUsed)
{
    if (!obj || !obj->script) return false;
    return exec(obj->script, proc, source, target, fixedParam, actionBeingUsed);
}

std::vector<ScriptInstance*> ScriptSystem::all() const
{
    std::vector<ScriptInstance*> v;
    for (auto& s : m_scripts) v.push_back(s.get());
    return v;
}

void ScriptSystem::mapEnter(bool fresh)
{
    bool savedSpatials = m_spatialsEnabled;
    m_spatialsEnabled = false;
    // map script first (fixed param = 1 when entering a fresh map, like the original)
    if (m_mapScript) exec(m_mapScript, SP_MAP_ENTER, nullptr, nullptr, fresh ? 1 : 0);
    loadAll();
    for (ScriptInstance* s : all()) {
        if (s == m_mapScript || s->disabled || !s->vm) continue;
        if (s->procs[SP_MAP_ENTER] > 0) exec(s, SP_MAP_ENTER, nullptr, nullptr, fresh ? 1 : 0);
    }
    m_spatialsEnabled = savedSpatials;
    mapUpdate();
}

void ScriptSystem::loadAll()
{
    for (ScriptInstance* s : all()) {
        if (s->disabled) continue;
        exec(s, SP_START);
    }
}

void ScriptSystem::mapUpdate()
{
    bool savedSpatials = m_spatialsEnabled;
    m_spatialsEnabled = false;
    if (m_mapScript) exec(m_mapScript, SP_MAP_UPDATE);
    for (ScriptInstance* s : all()) {
        if (s == m_mapScript || s->disabled || !s->vm) continue;
        if (s->procs[SP_MAP_UPDATE] > 0) exec(s, SP_MAP_UPDATE);
    }
    m_spatialsEnabled = savedSpatials;
}

void ScriptSystem::mapExit()
{
    for (ScriptInstance* s : all()) {
        if (s->disabled || !s->vm) continue;
        if (s->procs[SP_MAP_EXIT] > 0) exec(s, SP_MAP_EXIT);
    }
}

void ScriptSystem::heartbeat()
{
    std::vector<ScriptInstance*> critters;
    for (auto& s : m_scripts)
        if (s->type == SCRIPT_CRITTER && !s->disabled) critters.push_back(s.get());
    if (critters.empty()) return;
    m_heartbeatIndex = (m_heartbeatIndex + 1) % critters.size();
    ScriptInstance* s = critters[m_heartbeatIndex];
    Object* o = s->owner();
    if (!o || o->tile < 0 || o->elevation != g_game.elevation) return;
    exec(s, inCombat() ? SP_COMBAT : SP_CRITTER);
}

void ScriptSystem::checkSpatials(Object* mover, int tile, int elevation)
{
    if (!mover || !m_spatialsEnabled || (mover->flags & (OF_HIDDEN | OF_FLAT)) || tile < 10) return;
    m_spatialsEnabled = false;
    for (auto& sp : g_map.spatials) {
        if (!sp.script || sp.elevation != elevation) continue;
        bool inside = sp.tile == tile || (sp.radius > 0 && hex::distance(sp.tile, tile) <= sp.radius);
        if (inside) exec(sp.script, SP_SPATIAL, mover, nullptr);
    }
    m_spatialsEnabled = true;
}

} // namespace fo
