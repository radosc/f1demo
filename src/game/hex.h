// Fallout hex grid (200x200 per elevation) and square tile grid (100x100) geometry.
// World coordinates are absolute game pixels (independent of the camera).
#pragma once
#include "game/defs.h"

namespace fo {
namespace hex {

inline bool valid(int tile) { return tile >= 0 && tile < kHexes; }
// Top-left corner of the 32x16 hex cell.
void toWorld(int tile, int& x, int& y);
// Hex center (sprite anchor point): toWorld + (16, 8).
void center(int tile, int& x, int& y);
// Hex containing world point, or -1.
int fromWorld(int x, int y);
// Square (floor) tile image position (80x36 art) / roof position (96 px higher).
void squareToWorld(int square, int& x, int& y);
void roofToWorld(int square, int& x, int& y);
int squareFromWorld(int x, int y);
int roofFromWorld(int x, int y);
inline int squareOfTile(int tile) { return ((tile / kHexW) / 2) * kSquareW + (tile % kHexW) / 2; }

int neighbor(int tile, int dir);
int inDirection(int tile, int dir, int distance);
int distance(int a, int b);
int direction(int from, int to); // rotation 0..5
bool inFrontOf(int a, int b);
bool toRightOf(int a, int b);
bool onEdge(int tile);
// Hexes along a straight line (inclusive of both ends) - used for line of fire.
std::vector<int> line(int from, int to, int maxCount = 200);
int beyond(int from, int to, int distance);

} // namespace hex
} // namespace fo
