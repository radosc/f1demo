#include "game/path.h"
#include "game/game.h"
#include "game/hex.h"
#include "game/proto.h"

#include <queue>

namespace fo {

static int idist(int x1, int y1, int x2, int y2)
{
    int dx = abs(x2 - x1), dy = abs(y2 - y1);
    int dm = std::min(dx, dy);
    return dx + dy - dm / 2;
}

bool canUseDoor(Object* critter, Object* door)
{
    if (!critter || !door || critter == g_world.dude || !critter->isCritter()) return false;
    const Proto* cp = g_protos.get(critter->pid);
    if (cp && cp->bodyType != 0 && cp->bodyType != 2) return false; // bipeds / robots only
    if (fidType(door->fid) != OBJ_SCENERY) return false;
    const Proto* dp = door->proto();
    if (!dp || dp->sceneryType != SCEN_DOOR) return false;
    if (door->dataFlags & LOCK_FLAG_LOCKED) return false;
    return true;
}

std::vector<int> findPath(Object* obj, int from, int to, int elevation, bool allowBlockedTarget, int maxNodes)
{
    std::vector<int> result;
    if (!hex::valid(from) || !hex::valid(to) || from == to) return result;
    if (!allowBlockedTarget && g_world.blockingAt(obj, to, elevation)) return result;
    bool notInCombat = !inCombat();
    int tx, ty;
    hex::toWorld(to, tx, ty);

    struct Node {
        int tile;
        int from;
        int rot;
        int h;
        int g;
    };
    std::vector<Node> nodes;
    nodes.reserve(2048);
    std::unordered_map<int, int> seen; // tile -> node index (closed or open)
    struct Open {
        int f;
        int idx;
        int order;
        bool operator<(const Open& o) const { return f != o.f ? f > o.f : order > o.order; }
    };
    std::priority_queue<Open> open;
    int fx, fy;
    hex::toWorld(from, fx, fy);
    nodes.push_back({ from, -1, 0, idist(fx, fy, tx, ty), 0 });
    seen[from] = 0;
    open.push({ nodes[0].h, 0, 0 });
    int order = 1;
    int found = -1;
    int expanded = 0;
    while (!open.empty()) {
        Open cur = open.top();
        open.pop();
        Node n = nodes[cur.idx];
        if (n.tile == to) {
            found = cur.idx;
            break;
        }
        if (++expanded >= maxNodes) break;
        for (int r = 0; r < 6; r++) {
            int t = hex::inDirection(n.tile, r, 1);
            if (t == n.tile || seen.count(t)) continue;
            if (t != to) {
                Object* b = g_world.blockingAt(obj, t, elevation);
                if (b && !canUseDoor(obj, b)) continue;
            }
            int x, y;
            hex::toWorld(t, x, y);
            Node nn;
            nn.tile = t;
            nn.from = cur.idx;
            nn.rot = r;
            nn.h = idist(x, y, tx, ty);
            nn.g = n.g + 50 + ((notInCombat && n.rot != r && cur.idx != 0) ? 10 : 0);
            seen[t] = (int)nodes.size();
            nodes.push_back(nn);
            open.push({ nn.g + nn.h, (int)nodes.size() - 1, order++ });
        }
    }
    if (found < 0) return result;
    for (int i = found; nodes[i].from >= 0; i = nodes[i].from) result.push_back(nodes[i].rot);
    std::reverse(result.begin(), result.end());
    if (result.size() > 800) result.resize(800);
    return result;
}

std::vector<int> pathTiles(int from, const std::vector<int>& rotations)
{
    std::vector<int> tiles;
    int t = from;
    for (int r : rotations) {
        t = hex::inDirection(t, r, 1);
        tiles.push_back(t);
    }
    return tiles;
}

std::vector<int> straightPath(Object* obj, int from, int to, int elevation, Object** blocker)
{
    if (blocker) *blocker = nullptr;
    std::vector<int> line = hex::line(from, to);
    std::vector<int> out;
    for (size_t i = 1; i < line.size(); i++) {
        Object* b = g_world.blockingAt(obj, line[i], elevation);
        if (b) {
            if (blocker) *blocker = b;
            break;
        }
        out.push_back(line[i]);
    }
    return out;
}

} // namespace fo
