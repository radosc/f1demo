// Game objects (items, critters, scenery, walls, misc) and the per-map object world.
#pragma once
#include "game/defs.h"
#include "game/proto.h"

namespace fo {

struct Object;
struct ScriptInstance;

struct InvItem {
    Object* obj = nullptr;
    int count = 1;
};

struct CritterState {
    int hp = 0;
    int ap = 0;              // current action points (combat)
    u32 results = 0;         // DAM_* flags (knocked out, crippled, dead...)
    int aiPacket = 0;
    int team = 0;
    int radiation = 0;
    int poison = 0;
    int reaction = 0;        // reaction towards the player
    int maneuver = 0;        // engaging / disengaging / fleeing
    int damageLastTurn = 0;
    u32 whoHitMe = 0;        // object handle
    int moveFreeAp = 0;      // bonus move points (combat)
    bool sneaking = false;
};

struct Object {
    u32 uid = 0;             // unique runtime handle (never reused), used by scripts/save games
    int mapId = -1;          // object id from the map file
    int pid = -1;
    int fid = -1;
    int tile = -1;           // hex index (-1 = not on the map: inventory or floating)
    int elevation = 0;
    int rotation = 0;
    int frame = 0;
    int offX = 0, offY = 0;  // animation pixel offsets relative to the hex anchor
    u32 flags = 0;
    u32 outline = 0;         // highlight colour (ARGB), 0 = none
    int lightRadius = 0;
    int lightIntensity = 0;  // 0..65536
    Object* owner = nullptr; // container holding this item
    std::vector<InvItem> inventory;

    // script
    int scriptIndex = -1;    // scripts.lst index
    ScriptInstance* script = nullptr;

    // non-critter instance data
    u32 dataFlags = 0;       // doors/containers: locked/jammed bits
    int ammo = 0;            // weapon current ammo / ammo item quantity
    int ammoPid = -1;        // weapon loaded ammo type
    int charges = 0;
    int keyCode = -1;
    int destMap = -1, destTile = -1, destElevation = 0, destRotation = 0; // exit grids / stairs / ladders
    int elevatorType = -1, elevatorLevel = -1;

    CritterState critter;

    // runtime
    bool destroyed = false;
    int animBusy = 0;        // >0 while an animation sequence runs
    float floatTextTimer = 0;

    int type() const { return pidType(pid); }
    bool isCritter() const { return type() == OBJ_CRITTER; }
    bool isItem() const { return type() == OBJ_ITEM; }
    bool isScenery() const { return type() == OBJ_SCENERY; }
    bool isWall() const { return type() == OBJ_WALL; }
    bool isDead() const { return isCritter() && (critter.results & DAM_DEAD) != 0; }
    bool isKnockedOut() const { return isCritter() && (critter.results & DAM_KNOCKED_OUT) != 0; }
    bool isFlat() const { return (flags & OF_FLAT) != 0; }
    bool isHidden() const { return (flags & OF_HIDDEN) != 0; }
    const Proto* proto() const { return g_protos.get(pid); }
    // Top-level object (walks the owner chain).
    Object* root() { Object* o = this; while (o->owner) o = o->owner; return o; }
};

class World {
public:
    void clear();
    // Removes every object except `keep` and its inventory tree (the player between maps).
    void clearExcept(Object* keep);
    // Creates an object from its prototype (not placed on the map).
    Object* create(int pid, int fid = -1);
    // Destroys the object (and its inventory). Safe to call during iteration (deferred delete).
    void destroy(Object* obj);
    void collectGarbage();

    Object* byUid(u32 uid) const;
    void rekey(Object* o, u32 newUid);
    Object* byMapId(int id) const;

    // Placement
    void place(Object* obj, int tile, int elevation);
    void unplace(Object* obj);
    void moveTo(Object* obj, int tile, int elevation);
    const std::vector<Object*>& at(int tile, int elevation) const;

    // Queries
    Object* blockingAt(const Object* except, int tile, int elevation) const;
    Object* shootBlockingAt(const Object* except, int tile, int elevation) const;
    Object* critterAt(int tile, int elevation, bool includeDead = false) const;
    bool sightBlockingAt(int tile, int elevation) const;
    template <class F> void forEach(F&& f) const
    {
        for (auto& o : m_objects)
            if (o && !o->destroyed) f(o.get());
    }
    std::vector<Object*> critters(int elevation = -1) const;
    std::vector<Object*> objectsInRadius(int tile, int elevation, int radius) const;
    size_t count() const { return m_objects.size(); }

    // Inventory
    void addToInventory(Object* container, Object* item, int count);
    // Removes up to `count` of the item stack; returns the removed object (split if needed).
    Object* removeFromInventory(Object* container, Object* item, int count);
    int inventoryCount(const Object* container, int pid) const;
    Object* findInInventory(const Object* container, int pid) const;
    InvItem* invEntry(Object* container, Object* item);

    Object* dude = nullptr;
    u32 nextUid() { return m_nextUid++; }
    int nextMapId() { return m_nextMapId++; }
    void reserveMapIds(int maxUsed) { m_nextMapId = std::max(m_nextMapId, maxUsed + 1); }

private:
    std::vector<std::unique_ptr<Object>> m_objects;
    std::unordered_map<u32, Object*> m_byUid;
    std::unordered_map<int, std::vector<Object*>> m_tiles; // key = elevation * kHexes + tile
    std::vector<Object*> m_dead;
    u32 m_nextUid = 1;
    int m_nextMapId = 100000;
    static const std::vector<Object*> kEmpty;
};

extern World g_world;

// Instance data <-> JSON (used by maps and save games)
Json objectToJson(const Object* o, bool includePosition, bool runtimeState);
Object* objectFromJson(World& w, const Json& j, std::string* error = nullptr);

} // namespace fo
