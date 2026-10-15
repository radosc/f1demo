#include "game/object.h"
#include "game/critter.h"
#include "game/hex.h"
#include "game/scriptnames.h"
#include "game/proto.h"

namespace fo {

World g_world;
const std::vector<Object*> World::kEmpty;

void World::clear()
{
    m_objects.clear();
    m_byUid.clear();
    m_tiles.clear();
    m_dead.clear();
    dude = nullptr;
}

void World::clearExcept(Object* keep)
{
    if (!keep) {
        clear();
        return;
    }
    std::set<Object*> keepSet;
    std::function<void(Object*)> add = [&](Object* o) {
        keepSet.insert(o);
        for (auto& it : o->inventory) add(it.obj);
    };
    add(keep);
    unplace(keep);
    std::vector<std::unique_ptr<Object>> kept;
    for (auto& o : m_objects)
        if (keepSet.count(o.get())) kept.push_back(std::move(o));
    m_objects = std::move(kept);
    m_byUid.clear();
    for (auto& o : m_objects) m_byUid[o->uid] = o.get();
    m_tiles.clear();
    m_dead.clear();
    dude = keep;
}

Object* World::create(int pid, int fid)
{
    auto o = std::make_unique<Object>();
    o->uid = m_nextUid++;
    o->pid = pid;
    const Proto* p = g_protos.get(pid);
    if (p) {
        o->fid = fid >= 0 ? fid : p->fid;
        o->flags = p->flags;
        o->lightRadius = p->lightDistance;
        o->lightIntensity = p->lightIntensity;
        if (p->sid >= 0) o->scriptIndex = p->sid & 0xFFFFFF;
        switch (pidType(pid)) {
        case OBJ_ITEM:
            switch (p->itemType) {
            case ITEM_WEAPON:
                o->ammo = p->weapon.maxAmmo;
                o->ammoPid = p->weapon.ammoPid;
                break;
            case ITEM_AMMO: o->ammo = p->ammo.quantity; break;
            case ITEM_MISC: o->charges = p->misc.charges; break;
            case ITEM_KEY: o->keyCode = p->key.keyCode; break;
            case ITEM_CONTAINER: o->dataFlags = 0; break;
            }
            break;
        case OBJ_SCENERY:
            switch (p->sceneryType) {
            case SCEN_DOOR: o->dataFlags = (u32)p->sceneryData[0]; break;
            case SCEN_STAIRS:
                o->destMap = p->sceneryData[0];
                o->destTile = p->sceneryData[1];
                break;
            case SCEN_ELEVATOR:
                o->elevatorType = p->sceneryData[0];
                o->elevatorLevel = p->sceneryData[1];
                break;
            case SCEN_LADDER_UP:
            case SCEN_LADDER_DOWN: o->destTile = p->sceneryData[0]; break;
            }
            break;
        case OBJ_CRITTER:
            o->critter.aiPacket = p->aiPacket;
            o->critter.team = p->team;
            break;
        }
    } else {
        o->fid = fid;
        if (pid != -1 && pid != 0x05000000) LOGW("Creating object with unknown proto %08X", pid);
    }
    Object* raw = o.get();
    m_byUid[raw->uid] = raw;
    m_objects.push_back(std::move(o));
    if (raw->isCritter() && p) {
        raw->critter.hp = critterStat(raw, STAT_MAX_HP);
        raw->critter.ap = critterStat(raw, STAT_MAX_AP);
    }
    return raw;
}

void World::destroy(Object* obj)
{
    if (!obj || obj->destroyed) return;
    for (auto& it : obj->inventory) destroy(it.obj);
    obj->inventory.clear();
    if (obj->owner) {
        auto& inv = obj->owner->inventory;
        inv.erase(std::remove_if(inv.begin(), inv.end(), [&](const InvItem& i) { return i.obj == obj; }), inv.end());
        obj->owner = nullptr;
    }
    unplace(obj);
    obj->destroyed = true;
    if (obj == dude) dude = nullptr;
    m_dead.push_back(obj);
}

void World::collectGarbage()
{
    if (m_dead.empty()) return;
    std::set<Object*> dead(m_dead.begin(), m_dead.end());
    m_dead.clear();
    for (Object* o : dead) m_byUid.erase(o->uid);
    m_objects.erase(std::remove_if(m_objects.begin(), m_objects.end(), [&](const std::unique_ptr<Object>& o) { return dead.count(o.get()) != 0; }),
        m_objects.end());
}

Object* World::byUid(u32 uid) const
{
    if (!uid) return nullptr;
    auto it = m_byUid.find(uid);
    return (it == m_byUid.end() || it->second->destroyed) ? nullptr : it->second;
}

void World::rekey(Object* o, u32 newUid)
{
    if (!o || !newUid || o->uid == newUid) return;
    m_byUid.erase(o->uid);
    o->uid = newUid;
    m_byUid[newUid] = o;
    if (m_nextUid <= newUid) m_nextUid = newUid + 1;
}

Object* World::byMapId(int id) const
{
    for (auto& o : m_objects)
        if (o->mapId == id && !o->destroyed) return o.get();
    return nullptr;
}

void World::place(Object* obj, int tile, int elevation)
{
    if (!obj) return;
    unplace(obj);
    if (!hex::valid(tile) || elevation < 0 || elevation >= kElevations) return;
    obj->tile = tile;
    obj->elevation = elevation;
    obj->offX = obj->offY = 0;
    m_tiles[elevation * kHexes + tile].push_back(obj);
}

void World::unplace(Object* obj)
{
    if (!obj || obj->tile < 0) return;
    auto it = m_tiles.find(obj->elevation * kHexes + obj->tile);
    if (it != m_tiles.end()) {
        auto& v = it->second;
        v.erase(std::remove(v.begin(), v.end(), obj), v.end());
        if (v.empty()) m_tiles.erase(it);
    }
    obj->tile = -1;
}

void World::moveTo(Object* obj, int tile, int elevation) { place(obj, tile, elevation); }

const std::vector<Object*>& World::at(int tile, int elevation) const
{
    auto it = m_tiles.find(elevation * kHexes + tile);
    return it == m_tiles.end() ? kEmpty : it->second;
}

static bool blocksType(const Object* o)
{
    int t = fidType(o->fid);
    return t == OBJ_CRITTER || t == OBJ_SCENERY || t == OBJ_WALL;
}

Object* World::blockingAt(const Object* except, int tile, int elevation) const
{
    if (!hex::valid(tile)) return nullptr;
    for (Object* o : at(tile, elevation)) {
        if (o == except || (o->flags & (OF_HIDDEN | OF_NO_BLOCK))) continue;
        if (blocksType(o)) return o;
    }
    for (int r = 0; r < 6; r++) {
        int n = hex::inDirection(tile, r, 1);
        if (n == tile) continue;
        for (Object* o : at(n, elevation)) {
            if (!(o->flags & OF_MULTIHEX) || o == except || (o->flags & (OF_HIDDEN | OF_NO_BLOCK))) continue;
            if (blocksType(o)) return o;
        }
    }
    return nullptr;
}

Object* World::shootBlockingAt(const Object* except, int tile, int elevation) const
{
    if (!hex::valid(tile)) return nullptr;
    for (Object* o : at(tile, elevation)) {
        if (o == except || (o->flags & OF_HIDDEN) || (o->flags & OF_SHOOT_THRU)) continue;
        if (blocksType(o)) {
            if (o->isCritter() && o->isDead()) continue;
            return o;
        }
    }
    for (int r = 0; r < 6; r++) {
        int n = hex::inDirection(tile, r, 1);
        if (n == tile) continue;
        for (Object* o : at(n, elevation)) {
            if (!(o->flags & OF_MULTIHEX) || o == except || (o->flags & (OF_HIDDEN | OF_SHOOT_THRU))) continue;
            if (blocksType(o)) return o;
        }
    }
    return nullptr;
}

bool World::sightBlockingAt(int tile, int elevation) const
{
    for (Object* o : at(tile, elevation)) {
        if ((o->flags & (OF_HIDDEN | OF_LIGHT_THRU))) continue;
        int t = fidType(o->fid);
        if (t == OBJ_SCENERY || t == OBJ_WALL) return true;
    }
    return false;
}

Object* World::critterAt(int tile, int elevation, bool includeDead) const
{
    for (Object* o : at(tile, elevation)) {
        if (!o->isCritter() || o->isHidden()) continue;
        if (!includeDead && o->isDead()) continue;
        return o;
    }
    return nullptr;
}

std::vector<Object*> World::critters(int elevation) const
{
    std::vector<Object*> out;
    for (auto& o : m_objects) {
        if (o->destroyed || !o->isCritter() || o->tile < 0) continue;
        if (elevation >= 0 && o->elevation != elevation) continue;
        out.push_back(o.get());
    }
    return out;
}

std::vector<Object*> World::objectsInRadius(int tile, int elevation, int radius) const
{
    std::vector<Object*> out;
    for (auto& o : m_objects) {
        if (o->destroyed || o->tile < 0 || o->elevation != elevation) continue;
        if (hex::distance(tile, o->tile) <= radius) out.push_back(o.get());
    }
    return out;
}

static bool stackable(const Object* a, const Object* b)
{
    if (a->pid != b->pid) return false;
    if (a->script || b->script || a->scriptIndex != b->scriptIndex) return false;
    if (!a->inventory.empty() || !b->inventory.empty()) return false;
    if ((a->flags & OF_EQUIPPED) || (b->flags & OF_EQUIPPED)) return false;
    const Proto* p = a->proto();
    if (p && p->type() == OBJ_ITEM) {
        if (p->itemType == ITEM_WEAPON && (a->ammo != b->ammo || a->ammoPid != b->ammoPid)) return false;
        if (p->itemType == ITEM_AMMO && a->ammo != b->ammo) return false;
        if (p->itemType == ITEM_MISC && a->charges != b->charges) return false;
        if (p->itemType == ITEM_KEY && a->keyCode != b->keyCode) return false;
        if (p->itemType == ITEM_CONTAINER) return false;
    }
    return true;
}

void World::addToInventory(Object* container, Object* item, int count)
{
    if (!container || !item || count <= 0) return;
    if (item->owner) removeFromInventory(item->owner, item, INT32_MAX);
    unplace(item);
    for (auto& e : container->inventory) {
        if (e.obj != item && stackable(e.obj, item)) {
            e.count += count;
            destroy(item);
            return;
        }
        if (e.obj == item) {
            e.count += count;
            return;
        }
    }
    item->owner = container;
    item->flags &= ~OF_EQUIPPED;
    container->inventory.push_back({ item, count });
}

Object* World::removeFromInventory(Object* container, Object* item, int count)
{
    if (!container || !item) return nullptr;
    for (size_t i = 0; i < container->inventory.size(); i++) {
        InvItem& e = container->inventory[i];
        if (e.obj != item) continue;
        if (count >= e.count) {
            container->inventory.erase(container->inventory.begin() + (long long)i);
            item->owner = nullptr;
            item->flags &= ~OF_EQUIPPED;
            return item;
        }
        // split the stack
        e.count -= count;
        Object* copy = create(item->pid, item->fid);
        copy->flags = item->flags & ~OF_EQUIPPED;
        copy->ammo = item->ammo;
        copy->ammoPid = item->ammoPid;
        copy->charges = item->charges;
        copy->keyCode = item->keyCode;
        copy->scriptIndex = item->scriptIndex;
        return copy;
    }
    return nullptr;
}

int World::inventoryCount(const Object* container, int pid) const
{
    if (!container) return 0;
    int n = 0;
    for (auto& e : container->inventory) {
        if (e.obj->pid == pid) n += e.count;
        // items inside carried containers count as carried (like the original)
        if (!e.obj->inventory.empty()) n += inventoryCount(e.obj, pid);
    }
    return n;
}

Object* World::findInInventory(const Object* container, int pid) const
{
    if (!container) return nullptr;
    for (auto& e : container->inventory) {
        if (e.obj->pid == pid) return e.obj;
        if (Object* inner = findInInventory(e.obj, pid)) return inner;
    }
    return nullptr;
}

InvItem* World::invEntry(Object* container, Object* item)
{
    if (!container) return nullptr;
    for (auto& e : container->inventory)
        if (e.obj == item) return &e;
    return nullptr;
}

// ------------------------------------------------------------------ JSON
Json objectToJson(const Object* o, bool includePosition, bool runtime)
{
    Json j = Json::object();
    j.set("pid", jsonHex((u32)o->pid));
    if (o->mapId >= 0) j.set("id", o->mapId);
    if (includePosition) {
        j.set("tile", o->tile);
        j.set("elevation", o->elevation);
    }
    j.set("rotation", o->rotation);
    const Proto* p = o->proto();
    if (!p || p->fid != o->fid) j.set("fid", jsonHex((u32)o->fid));
    if (o->frame) j.set("frame", o->frame);
    j.set("flags", flagsToJson(o->flags, kObjectFlagNames));
    if (o->lightRadius || o->lightIntensity) {
        Json l = Json::array();
        l.push(o->lightRadius);
        l.push(o->lightIntensity);
        j.set("light", l);
    }
    if (o->scriptIndex >= 0) j.set("script", g_scriptNames.name(o->scriptIndex));
    if (runtime) j.set("uid", (i64)o->uid);
    switch (o->type()) {
    case OBJ_CRITTER: {
        Json c = Json::object();
        c.set("hp", o->critter.hp);
        c.set("ap", o->critter.ap);
        c.set("aiPacket", o->critter.aiPacket);
        c.set("team", o->critter.team);
        if (o->critter.results) c.set("results", (i64)o->critter.results);
        if (o->critter.radiation) c.set("radiation", o->critter.radiation);
        if (o->critter.poison) c.set("poison", o->critter.poison);
        if (o->critter.reaction) c.set("reaction", o->critter.reaction);
        if (runtime && o->critter.whoHitMe) c.set("whoHitMe", (i64)o->critter.whoHitMe);
        j.set("critter", c);
        break;
    }
    case OBJ_ITEM:
        if (p) {
            switch (p->itemType) {
            case ITEM_WEAPON: {
                Json w = Json::object();
                w.set("ammo", o->ammo);
                w.set("ammoPid", o->ammoPid < 0 ? Json(-1) : Json(jsonHex((u32)o->ammoPid)));
                j.set("weapon", w);
                break;
            }
            case ITEM_AMMO: j.set("ammo", o->ammo); break;
            case ITEM_MISC: j.set("charges", o->charges); break;
            case ITEM_KEY: j.set("keyCode", o->keyCode); break;
            case ITEM_CONTAINER:
                if (o->dataFlags) j.set("lock", flagsToJson(o->dataFlags, kLockFlagNames));
                break;
            }
        }
        break;
    case OBJ_SCENERY:
        if (p) {
            switch (p->sceneryType) {
            case SCEN_DOOR: {
                Json d = Json::object();
                d.set("openFlags", flagsToJson(o->dataFlags, kLockFlagNames));
                j.set("door", d);
                break;
            }
            case SCEN_STAIRS: {
                Json d = Json::object();
                d.set("destMap", o->destMap);
                d.set("destTile", o->destTile);
                j.set("stairs", d);
                break;
            }
            case SCEN_ELEVATOR: {
                Json d = Json::object();
                d.set("elevatorType", o->elevatorType);
                d.set("level", o->elevatorLevel);
                j.set("elevator", d);
                break;
            }
            case SCEN_LADDER_UP:
            case SCEN_LADDER_DOWN: {
                Json d = Json::object();
                d.set("destTile", o->destTile);
                j.set("ladder", d);
                break;
            }
            }
        }
        break;
    case OBJ_MISC:
        if (o->pid >= 0x5000010 && o->pid <= 0x5000017) {
            Json e = Json::object();
            e.set("map", o->destMap);
            e.set("tile", o->destTile);
            e.set("elevation", o->destElevation);
            e.set("rotation", o->destRotation);
            j.set("exitGrid", e);
        }
        break;
    }
    if (!o->inventory.empty()) {
        Json inv = Json::array();
        for (auto& e : o->inventory) {
            Json it = Json::object();
            it.set("count", e.count);
            it.set("object", objectToJson(e.obj, false, runtime));
            inv.push(it);
        }
        j.set("inventory", inv);
    }
    return j;
}

Object* objectFromJson(World& w, const Json& j, std::string* error)
{
    int pid = (int)j["pid"].i(-1);
    if (pid == -1) {
        if (error) *error = "object without pid";
        return nullptr;
    }
    int fid = j.has("fid") ? (int)j["fid"].i(-1) : -1;
    Object* o = w.create(pid, fid);
    if (j.has("id")) {
        o->mapId = j["id"].toInt(-1);
        w.reserveMapIds(o->mapId);
    }
    if (j.has("uid")) {
        // restore saved uid (save games keep script references stable)
        u32 saved = (u32)j["uid"].i(0);
        if (saved) w.rekey(o, saved);
    }
    o->rotation = clampv(j["rotation"].toInt(0), 0, 5);
    o->frame = j["frame"].toInt(0);
    if (j.has("flags")) o->flags = flagsFromJson(j["flags"], kObjectFlagNames);
    if (j.has("light")) {
        o->lightRadius = j["light"][(size_t)0].toInt();
        o->lightIntensity = j["light"][(size_t)1].toInt();
    }
    if (j.has("script")) {
        const Json& s = j["script"];
        if (s.isNull()) o->scriptIndex = -1;
        else {
            int idx = g_scriptNames.indexOf(s.str());
            if (idx < 0) LOGW("Unknown script '%s' on object %08X", s.str().c_str(), pid);
            o->scriptIndex = idx;
        }
    }
    const Proto* p = o->proto();
    if (o->isCritter()) {
        const Json& c = j["critter"];
        if (c.has("hp")) o->critter.hp = c["hp"].toInt();
        if (c.has("ap")) o->critter.ap = c["ap"].toInt();
        if (c.has("aiPacket")) o->critter.aiPacket = c["aiPacket"].toInt();
        if (c.has("team")) o->critter.team = c["team"].toInt();
        o->critter.results = (u32)c["results"].i(0);
        o->critter.radiation = c["radiation"].toInt();
        o->critter.poison = c["poison"].toInt();
        o->critter.reaction = c["reaction"].toInt();
        o->critter.whoHitMe = (u32)c["whoHitMe"].i(0);
    } else if (o->isItem() && p) {
        switch (p->itemType) {
        case ITEM_WEAPON:
            if (j.has("weapon")) {
                o->ammo = j["weapon"]["ammo"].toInt(o->ammo);
                o->ammoPid = (int)j["weapon"]["ammoPid"].i(o->ammoPid);
            }
            break;
        case ITEM_AMMO:
            if (j.has("ammo")) o->ammo = j["ammo"].toInt(o->ammo);
            break;
        case ITEM_MISC:
            if (j.has("charges")) o->charges = j["charges"].toInt(o->charges);
            break;
        case ITEM_KEY:
            if (j.has("keyCode")) o->keyCode = j["keyCode"].toInt(o->keyCode);
            break;
        case ITEM_CONTAINER:
            if (j.has("lock")) o->dataFlags = flagsFromJson(j["lock"], kLockFlagNames);
            break;
        }
    } else if (o->isScenery() && p) {
        switch (p->sceneryType) {
        case SCEN_DOOR:
            if (j.has("door")) o->dataFlags = flagsFromJson(j["door"]["openFlags"], kLockFlagNames);
            break;
        case SCEN_STAIRS:
            if (j.has("stairs")) {
                o->destMap = j["stairs"]["destMap"].toInt(o->destMap);
                o->destTile = j["stairs"]["destTile"].toInt(o->destTile);
            }
            break;
        case SCEN_ELEVATOR:
            if (j.has("elevator")) {
                o->elevatorType = j["elevator"]["elevatorType"].toInt(o->elevatorType);
                o->elevatorLevel = j["elevator"]["level"].toInt(o->elevatorLevel);
            }
            break;
        case SCEN_LADDER_UP:
        case SCEN_LADDER_DOWN:
            if (j.has("ladder")) o->destTile = j["ladder"]["destTile"].toInt(o->destTile);
            break;
        }
    } else if (o->type() == OBJ_MISC && j.has("exitGrid")) {
        const Json& e = j["exitGrid"];
        o->destMap = e["map"].toInt(-1);
        o->destTile = e["tile"].toInt(-1);
        o->destElevation = e["elevation"].toInt(0);
        o->destRotation = e["rotation"].toInt(0);
    }
    for (auto& it : j["inventory"].items()) {
        Object* child = objectFromJson(w, it["object"], error);
        if (!child) continue;
        int count = std::max(1, it["count"].toInt(1));
        u32 equip = child->flags & OF_EQUIPPED;
        w.addToInventory(o, child, count);
        // keep equipped flags from the data (addToInventory clears them)
        if (InvItem* e = w.invEntry(o, child)) e->obj->flags |= equip;
    }
    return o;
}

} // namespace fo
