// Current map: tile layers, spatial triggers and loading from data/maps/<name>.json
#pragma once
#include "game/object.h"

namespace fo {

struct ScriptInstance;

struct MapSpatial {
    int scriptIndex = -1;
    int tile = -1;
    int elevation = 0;
    int radius = 0;
    ScriptInstance* script = nullptr;
};

class GameMap {
public:
    std::string name;
    std::string displayName;
    int enterTile = 20100, enterElevation = 0, enterRotation = 0;
    int scriptIndex = -1;
    int darkness = 0;
    bool hasElevation[kElevations] = {};
    std::vector<u16> floor[kElevations];
    std::vector<u16> roof[kElevations];
    std::vector<u8> roofHidden[kElevations]; // 1 = hidden (player under this roof region)
    std::vector<MapSpatial> spatials;

    void clear();
    // Loads tiles + objects into g_world (does not run scripts).
    // `withObjects` = false loads only tiles/spatials/metadata (save games bring their own objects).
    bool load(const std::string& mapName, std::string* error = nullptr, bool withObjects = true);
    bool loaded() const { return !name.empty(); }

    int floorAt(int elevation, int square) const;
    int roofAt(int elevation, int square) const;
    // Hides the connected roof region above `tile` (when the player walks under it).
    void updateRoofVisibility(int tile, int elevation);

    // Save game support (tiles are reloaded from data; only dynamic parts are saved)
    Json saveState() const;

    // Map editor: the whole map (tiles, entrance, spatials and the objects in g_world) as map JSON.
    // Fields of `source` (the loaded file) that the engine does not use are kept.
    Json toJson(const Json& source) const;
    // Map editor: an empty map with elevation 0 covered by `groundTile`.
    void createBlank(const std::string& mapName, int groundTile);

private:
    void floodRoof(int elevation, int sq, u8 value);
};

extern GameMap g_map;

} // namespace fo
