#include "game/hex.h"

namespace fo {
namespace hex {

static int floorDiv(int a, int b) { return a >= 0 ? a / b : (a + 1) / b - 1; }

// Sub-hex mask (32x16) deciding which neighbouring hex a pixel belongs to.
static const unsigned char* tileMask()
{
    static unsigned char mask[512];
    static bool built = false;
    if (built) return mask;
    int v11 = 0, v12 = 0, v13;
    do {
        v13 = 64;
        do {
            mask[v12++] = v13 > v11;
            v13 -= 4;
        } while (v13);
        do {
            mask[v12++] = v13 > v11 ? 2 : 0;
            v13 += 4;
        } while (v13 != 64);
        v11 += 16;
    } while (v11 != 64);
    v11 = 0;
    do {
        v13 = 0;
        do {
            mask[v12++] = 0;
            v13++;
        } while (v13 < 32);
        v11++;
    } while (v11 < 8);
    v11 = 0;
    do {
        v13 = 0;
        do {
            mask[v12++] = v13 > v11 ? 0 : 3;
            v13 += 4;
        } while (v13 != 64);
        v13 = 64;
        do {
            mask[v12++] = v13 > v11 ? 0 : 4;
            v13 -= 4;
        } while (v13);
        v11 += 16;
    } while (v11 != 64);
    built = true;
    return mask;
}

void toWorld(int tile, int& x, int& y)
{
    int c = kHexW - 1 - tile % kHexW; // screen column (increases to the right)
    int r = tile / kHexW;
    x = 48 * (c >> 1) + ((c & 1) ? 32 : 0) + 16 * r;
    y = -12 * (c >> 1) + 12 * r;
}

void center(int tile, int& x, int& y)
{
    toWorld(tile, x, y);
    x += 16;
    y += 8;
}

int fromWorld(int screenX, int screenY)
{
    int v2 = screenY;
    int v3 = floorDiv(v2, 12);
    int v4 = screenX - 16 * v3;
    int v5 = v2 - 12 * v3;
    int v6 = floorDiv(v4, 64);
    int v7 = v6 + v3;
    int v8 = v4 - v6 * 64;
    int v9 = 2 * v6;
    if (v8 >= 32) {
        v8 -= 32;
        v9++;
    }
    int row = v7;
    int col = v9;
    switch (tileMask()[32 * v5 + v8]) {
    case 2:
        col++;
        if (col & 1) row--;
        break;
    case 1:
        row--;
        break;
    case 3:
        col--;
        if (!(col & 1)) row++;
        break;
    case 4:
        row++;
        break;
    default:
        break;
    }
    int c = kHexW - 1 - col;
    if (c >= 0 && c < kHexW && row >= 0 && row < kHexH) return kHexW * row + c;
    return -1;
}

void squareToWorld(int sq, int& x, int& y)
{
    int c = kSquareW - 1 - sq % kSquareW;
    int r = sq / kSquareW;
    x = 48 * c + 32 * r - 16;
    y = -12 * c + 24 * r - 2;
}

void roofToWorld(int sq, int& x, int& y)
{
    squareToWorld(sq, x, y);
    y -= 96;
}

static void squareXY(int screenX, int screenY, int& cx, int& cy)
{
    int v4 = screenX - (-16);
    int v5 = screenY - (-2) - 12;
    int v6 = 3 * v4 - 4 * v5;
    cx = v6 >= 0 ? v6 / 192 : (v6 + 1) / 192 - 1;
    int v8 = 4 * v5 + v4;
    cy = v8 >= 0 ? v8 / 128 : (v8 + 1) / 128 - 1;
    cx = kSquareW - 1 - cx;
}

int squareFromWorld(int x, int y)
{
    int cx, cy;
    squareXY(x, y, cx, cy);
    if (cx >= 0 && cx < kSquareW && cy >= 0 && cy < kSquareH) return cx + kSquareW * cy;
    return -1;
}

int roofFromWorld(int x, int y) { return squareFromWorld(x, y + 96); }

static const int kDirTile[2][6] = {
    { -1, kHexW - 1, kHexW, kHexW + 1, 1, -kHexW },
    { -kHexW - 1, -1, kHexW, 1, 1 - kHexW, -kHexW },
};

bool onEdge(int tile)
{
    if (!valid(tile)) return false;
    if (tile < kHexW || tile >= kHexes - kHexW) return true;
    int m = tile % kHexW;
    return m == 0 || m == kHexW - 1;
}

int neighbor(int tile, int dir)
{
    if (!valid(tile) || onEdge(tile)) return tile;
    return tile + kDirTile[(tile % kHexW) & 1][((dir % 6) + 6) % 6];
}

int inDirection(int tile, int dir, int dist)
{
    int t = tile;
    for (int i = 0; i < dist; i++) {
        if (onEdge(t)) break;
        t += kDirTile[(t % kHexW) & 1][((dir % 6) + 6) % 6];
    }
    return t;
}

int direction(int from, int to)
{
    int x1, y1, x2, y2;
    toWorld(from, x1, y1);
    toWorld(to, x2, y2);
    int dy = y2 - y1;
    int dx = x2 - x1;
    if (dx != 0) {
        int a = (int)trunc(atan2((double)-dy, (double)dx) * 180.0 * 0.3183098862851122);
        int v = 360 - (a + 180) - 90;
        if (v < 0) v += 360;
        v /= 60;
        if (v >= ROT_COUNT) v = ROT_NW;
        return v;
    }
    return dy < 0 ? ROT_NE : ROT_SE;
}

int distance(int a, int b)
{
    if (!valid(a) || !valid(b)) return 9999;
    int t = a, d = 0;
    while (t != b && d < 1000) {
        t += kDirTile[(t % kHexW) & 1][direction(t, b)];
        d++;
    }
    return d;
}

bool inFrontOf(int a, int b)
{
    int x1, y1, x2, y2;
    toWorld(a, x1, y1);
    toWorld(b, x2, y2);
    return (double)(x2 - x1) <= (double)(y2 - y1) * -4.0;
}

bool toRightOf(int a, int b)
{
    int x1, y1, x2, y2;
    toWorld(a, x1, y1);
    toWorld(b, x2, y2);
    return (double)(x2 - x1) <= (double)(y2 - y1) * 1.3333333333333335;
}

std::vector<int> line(int from, int to, int cap)
{
    std::vector<int> tiles;
    if (cap <= 1 || !valid(from) || !valid(to)) return tiles;
    int fx, fy, tx, ty;
    center(from, fx, fy);
    center(to, tx, ty);
    tiles.push_back(from);
    int dx = tx - fx, dy = ty - fy;
    int stepX = dx > 0 ? 1 : dx < 0 ? -1 : 0;
    int stepY = dy > 0 ? 1 : dy < 0 ? -1 : 0;
    int ax = 2 * abs(dx), ay = 2 * abs(dy);
    int x = fx, y = fy;
    auto add = [&](int t) -> bool {
        if (t == to) {
            if (tiles.back() != t) tiles.push_back(t);
            return true;
        }
        if (t != -1 && t != tiles.back() && (tiles.size() < 2 || t != tiles[tiles.size() - 2])) {
            tiles.push_back(t);
            if ((int)tiles.size() >= cap) return true;
        }
        return false;
    };
    if (ax <= ay) {
        int m = ax - ay / 2;
        while (true) {
            if (add(fromWorld(x, y))) break;
            if (y == ty) break;
            if (m >= 0) {
                x += stepX;
                m -= ay;
            }
            m += ax;
            y += stepY;
        }
    } else {
        int m = ay - ax / 2;
        while (true) {
            if (add(fromWorld(x, y))) break;
            if (x == tx) break;
            if (m >= 0) {
                y += stepY;
                m -= ax;
            }
            m += ay;
            x += stepX;
        }
    }
    return tiles;
}

int beyond(int from, int to, int dist)
{
    if (dist <= 0 || from == to || !valid(from) || !valid(to)) return from;
    int fx, fy, tx, ty;
    center(from, fx, fy);
    center(to, tx, ty);
    int dx = tx - fx, dy = ty - fy;
    int ax = 2 * abs(dx), ay = 2 * abs(dy);
    int sx = dx > 0 ? 1 : dx < 0 ? -1 : 0;
    int sy = dy > 0 ? 1 : dy < 0 ? -1 : 0;
    int prev = from, x = fx, y = fy, n = 0;
    if (ax > ay) {
        int middle = ay - ax / 2;
        for (int guard = 0; guard < 100000; guard++) {
            int t = fromWorld(x, y);
            if (t == -1) return prev;
            if (t != prev) {
                n++;
                if (n == dist || onEdge(t)) return t;
                prev = t;
            }
            if (middle >= 0) {
                middle -= ax;
                y += sy;
            }
            middle += ay;
            x += sx;
        }
    } else {
        int middle = ax - ay / 2;
        for (int guard = 0; guard < 100000; guard++) {
            int t = fromWorld(x, y);
            if (t == -1) return prev;
            if (t != prev) {
                n++;
                if (n == dist || onEdge(t)) return t;
                prev = t;
            }
            if (middle >= 0) {
                middle -= ay;
                x += sx;
            }
            middle += ax;
            y += sy;
        }
    }
    return prev;
}

} // namespace hex
} // namespace fo
