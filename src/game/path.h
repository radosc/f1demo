// Hex grid A* pathfinding (Fallout semantics: 2000 node limit, turn penalty outside combat).
#pragma once
#include "game/object.h"

namespace fo {

// Returns rotations (one per step) from `from` to `to`. Empty if no path.
// If `allowBlockedTarget` is false and the target hex is blocked, fails immediately.
std::vector<int> findPath(Object* obj, int from, int to, int elevation, bool allowBlockedTarget = true, int maxNodes = 2000);
// Tiles along a rotation path (excluding the start tile).
std::vector<int> pathTiles(int from, const std::vector<int>& rotations);
// Straight line path (for knockback/flight) - stops at blockers.
std::vector<int> straightPath(Object* obj, int from, int to, int elevation, Object** blocker);
// Can `critter` open this door while walking (NPCs only, unlocked doors)?
bool canUseDoor(Object* critter, Object* door);

} // namespace fo
