#include "game/anim.h"
#include "game/art.h"
#include "game/game.h"
#include "game/hex.h"
#include "game/path.h"
#include "audio/sound.h"

namespace fo {

std::function<bool(Object*)> g_animCombatStep;
std::function<void(Object*)> g_animOnTileChanged;
float g_animSpeed = 1.0f;

namespace {

enum Kind {
    K_MOVE_TILE, K_MOVE_OBJ, K_MOVE_STRAIGHT, K_ANIMATE, K_ANIMATE_REV, K_ANIMATE_HIDE, K_ANIMATE_FOREVER,
    K_ROTATE_TILE, K_ROTATE_CW, K_ROTATE_CCW, K_HIDE, K_CALLBACK, K_SET_FID, K_TAKE_OUT, K_SFX, K_SET_FLAG,
    K_UNSET_FLAG, K_SET_LIGHT,
};

struct Desc {
    Kind kind = K_CALLBACK;
    u32 obj = 0;
    u32 target = 0;
    int tile = -1, elev = 0;
    int anim = 0;
    int ap = -1;
    bool run = false;
    int delay = -1;
    std::function<void()> fn;
    bool forced = false;
    int fid = -1;
    u32 flag = 0;
    std::string sfx;
    int value = 0;
};

struct Seq {
    int id = 0;
    int flags = 0;
    std::vector<Desc> descs;
    std::vector<u8> state; // 0 = pending, 1 = running, 2 = done
    bool ended = false;
};

struct Runner {
    int seq = 0;
    size_t desc = 0;
    u32 obj = 0;
    Kind kind = K_ANIMATE;
    int fid = -1;
    std::vector<int> rotations;
    size_t pathPos = 0;
    int destTile = -1;
    std::vector<int> straight;
    float acc = 0;
    int tpf = 100;
    bool started = false;
    bool done = false;
    bool stopMove = false;
    int framesDone = 0;   // frames played (for descriptions with a positive frame delay)
};

const int kOffTile[2][6] = { { 16, 32, 16, -16, -32, -16 }, { -12, 0, 12, 12, 0, -12 } };

std::vector<std::unique_ptr<Seq>> g_seqs;
std::vector<Runner> g_runners;
std::unique_ptr<Seq> g_building;
int g_nextSeq = 1;

Sprite* artOf(int fid) { return g_art.sprite(fid); }

void frameHot(int fid, int frame, int rot, int& x, int& y)
{
    x = y = 0;
    Sprite* s = artOf(fid);
    if (!s) return;
    const SheetFrame& f = s->frame(rot, frame);
    x = f.ox;
    y = f.oy;
}

void artShift(int fid, int rot, int& x, int& y)
{
    x = y = 0;
    Sprite* s = artOf(fid);
    if (!s) return;
    int d = s->meta.dirCount > 1 ? rot : 0;
    x = s->meta.shiftX[d];
    y = s->meta.shiftY[d];
}

int frameCount(int fid)
{
    Sprite* s = artOf(fid);
    return s ? std::max(1, s->meta.framesPerDir) : 1;
}

bool critterProne(Object* o)
{
    return o->isCritter() && (o->critter.results & (DAM_DEAD | DAM_KNOCKED_DOWN | DAM_KNOCKED_OUT));
}

void tileChanged(Object* o)
{
    if (g_animOnTileChanged) g_animOnTileChanged(o);
}

} // namespace

int animFrameTime(Object* obj, int fid)
{
    Sprite* s = artOf(fid);
    int fps = s ? std::max(1, s->meta.fps) : 10;
    (void)obj;
    float ms = 1000.0f / (float)fps / std::max(0.1f, g_animSpeed);
    return std::max(1, (int)ms);
}

int animFid(Object* obj, int anim)
{
    if (!obj) return -1;
    int type = fidType(obj->fid);
    if (type != OBJ_CRITTER) return obj->fid;
    int weapon = fidWeapon(obj->fid);
    int fid = makeFid(OBJ_CRITTER, fidIndex(obj->fid), anim, weapon);
    if (artOf(fid)) return fid;
    // fall back to the unarmed version
    int unarmed = makeFid(OBJ_CRITTER, fidIndex(obj->fid), anim, 0);
    if (artOf(unarmed)) return unarmed;
    if (anim == ANIM_RUNNING) return animFid(obj, ANIM_WALK);
    return fid;
}

bool animArtExists(Object* obj, int anim) { return artOf(animFid(obj, anim)) != nullptr; }

void critterStand(Object* obj, int rotation)
{
    if (!obj) return;
    obj->rotation = ((rotation % 6) + 6) % 6;
    int x = 0, y = 0;
    int weapon = fidWeapon(obj->fid);
    if (weapon != 0) {
        int takeOut = makeFid(OBJ_CRITTER, fidIndex(obj->fid), ANIM_TAKE_OUT, weapon);
        if (Sprite* s = artOf(takeOut)) {
            for (int f = 0; f < s->meta.framesPerDir; f++) {
                const SheetFrame& fr = s->frame(obj->rotation, f);
                x += fr.ox;
                y += fr.oy;
            }
            int sx, sy;
            artShift(makeFid(OBJ_CRITTER, fidIndex(obj->fid), ANIM_STAND, 0), obj->rotation, sx, sy);
            x += sx;
            y += sy;
        }
    }
    int anim = fidAnim(obj->fid) == ANIM_FIRE_DANCE ? ANIM_FIRE_DANCE : ANIM_STAND;
    int fid = makeFid(fidType(obj->fid), fidIndex(obj->fid), anim, weapon);
    if (!artOf(fid)) fid = makeFid(fidType(obj->fid), fidIndex(obj->fid), anim, 0), x = y = 0;
    obj->fid = fid;
    obj->frame = 0;
    obj->offX = x;
    obj->offY = y;
}

// ------------------------------------------------------------------ builder
static Desc& addDesc(Kind k, Object* obj, int delay)
{
    if (!g_building) animBegin();
    Desc d;
    d.kind = k;
    d.obj = obj ? obj->uid : 0;
    d.delay = delay;
    g_building->descs.push_back(d);
    return g_building->descs.back();
}

void animBegin(int flags)
{
    g_building = std::make_unique<Seq>();
    g_building->flags = flags;
}

void animClear() { g_building.reset(); }

void animMoveToTile(Object* obj, int tile, int elevation, int ap, bool run, int delay)
{
    Desc& d = addDesc(K_MOVE_TILE, obj, delay);
    d.tile = tile;
    d.elev = elevation;
    d.ap = ap;
    d.run = run;
}

void animMoveToObject(Object* obj, Object* target, int ap, bool run, int delay)
{
    Desc& d = addDesc(K_MOVE_OBJ, obj, delay);
    d.target = target ? target->uid : 0;
    d.ap = ap;
    d.run = run;
}

void animMoveStraight(Object* obj, int tile, int elevation, int anim, int delay)
{
    Desc& d = addDesc(K_MOVE_STRAIGHT, obj, delay);
    d.tile = tile;
    d.elev = elevation;
    d.anim = anim;
}

void animAnimate(Object* obj, int anim, int delay) { addDesc(K_ANIMATE, obj, delay).anim = anim; }
void animAnimateReverse(Object* obj, int anim, int delay) { addDesc(K_ANIMATE_REV, obj, delay).anim = anim; }
void animAnimateAndHide(Object* obj, int anim, int delay) { addDesc(K_ANIMATE_HIDE, obj, delay).anim = anim; }
void animAnimateForever(Object* obj, int anim, int delay) { addDesc(K_ANIMATE_FOREVER, obj, delay).anim = anim; }
void animRotateToTile(Object* obj, int tile) { addDesc(K_ROTATE_TILE, obj, -1).tile = tile; }
void animRotateClockwise(Object* obj) { addDesc(K_ROTATE_CW, obj, -1); }
void animRotateCounterClockwise(Object* obj) { addDesc(K_ROTATE_CCW, obj, -1); }
void animHide(Object* obj, int delay) { addDesc(K_HIDE, obj, delay); }

void animCallback(Object* obj, std::function<void()> fn, int delay, bool forced)
{
    Desc& d = addDesc(K_CALLBACK, obj, delay);
    d.fn = std::move(fn);
    d.forced = forced;
}

void animSetFid(Object* obj, int fid, int delay) { addDesc(K_SET_FID, obj, delay).fid = fid; }
void animTakeOut(Object* obj, int weaponCode, int delay) { addDesc(K_TAKE_OUT, obj, delay).value = weaponCode; }
void animPlaySfx(Object* obj, const std::string& sfx, int delay) { addDesc(K_SFX, obj, delay).sfx = sfx; }
void animSetFlag(Object* obj, u32 flag, int delay) { addDesc(K_SET_FLAG, obj, delay).flag = flag; }
void animUnsetFlag(Object* obj, u32 flag, int delay) { addDesc(K_UNSET_FLAG, obj, delay).flag = flag; }
void animSetLight(Object* obj, int radius, int delay) { addDesc(K_SET_LIGHT, obj, delay).value = radius; }

static void endSeq(Seq& s, bool aborted);

int animEnd()
{
    if (!g_building || g_building->descs.empty()) {
        g_building.reset();
        return -1;
    }
    std::unique_ptr<Seq> s = std::move(g_building);
    s->id = g_nextSeq++;
    s->state.assign(s->descs.size(), 0);
    if (s->flags & ANIMSEQ_REPLACE) {
        // stop running sequences of the objects in this sequence (unless uninterruptible)
        std::set<u32> objs;
        for (auto& d : s->descs)
            if (d.obj) objs.insert(d.obj);
        for (u32 o : objs) {
            for (auto& other : g_seqs) {
                if (other->ended || (other->flags & ANIMSEQ_UNINTERRUPTIBLE)) continue;
                bool uses = false;
                for (auto& d : other->descs)
                    if (d.obj == o) uses = true;
                if (uses) endSeq(*other, true);
            }
        }
    }
    g_seqs.push_back(std::move(s));
    return 0;
}

// ------------------------------------------------------------------ execution
static void endSeq(Seq& s, bool aborted)
{
    if (s.ended) return;
    s.ended = true;
    // remove runners of this sequence
    g_runners.erase(std::remove_if(g_runners.begin(), g_runners.end(), [&](const Runner& r) { return r.seq == s.id; }), g_runners.end());
    // forced callbacks still run
    for (size_t i = 0; i < s.descs.size(); i++) {
        Desc& d = s.descs[i];
        if (s.state[i] != 2 && d.kind == K_CALLBACK && d.forced && d.fn) {
            s.state[i] = 2;
            d.fn();
        }
    }
    (void)aborted;
    // return critters to their stand pose
    if (!(s.flags & ANIMSEQ_NO_STAND)) {
        std::set<u32> done;
        for (auto& d : s.descs) {
            if (!d.obj || done.count(d.obj) || d.kind == K_CALLBACK) continue;
            done.insert(d.obj);
            Object* o = g_world.byUid(d.obj);
            if (!o || o->tile < 0 || fidType(o->fid) != OBJ_CRITTER || critterProne(o) || (o->flags & OF_HIDDEN)) continue;
            int anim = fidAnim(o->fid);
            if (anim >= ANIM_FIRST_DEATH && anim <= ANIM_LAST_SF_DEATH) continue;
            critterStand(o, o->rotation);
        }
    }
}

static bool startMove(Runner& r, Object* o, const Desc& d)
{
    int dest = d.tile;
    bool dropLast = false;
    if (d.kind == K_MOVE_OBJ) {
        Object* t = g_world.byUid(d.target);
        if (!t || t->tile < 0) return false;
        dest = t->tile;
        dropLast = true;
    }
    if (!hex::valid(dest) || o->tile < 0) return false;
    std::vector<int> rots = findPath(o, o->tile, dest, o->elevation, true);
    if (dropLast && !rots.empty()) {
        // stop next to the target if its hex is occupied
        if (g_world.blockingAt(o, dest, o->elevation)) rots.pop_back();
    } else if (!rots.empty() && g_world.blockingAt(o, dest, o->elevation)) {
        rots.pop_back();
    }
    if (d.ap >= 0 && (int)rots.size() > d.ap) rots.resize((size_t)d.ap);
    if (rots.empty()) return false;
    r.rotations = std::move(rots);
    r.pathPos = 0;
    r.destTile = dest;
    int anim = d.run ? ANIM_RUNNING : ANIM_WALK;
    r.fid = animFid(o, anim);
    r.tpf = animFrameTime(o, r.fid);
    return true;
}

static void finishRunner(Runner& r) { r.done = true; }

static void stepMove(Runner& r, Object* o)
{
    if (!r.started) {
        r.started = true;
        g_world.place(o, o->tile, o->elevation); // resets offsets
        o->frame = 0;
        o->rotation = r.rotations[0];
        o->fid = r.fid;
    } else {
        o->frame = (o->frame + 1) % frameCount(o->fid);
    }
    int hx, hy;
    frameHot(o->fid, o->frame, o->rotation, hx, hy);
    o->offX += hx;
    o->offY += hy;
    int rot = r.rotations[r.pathPos];
    int x = kOffTile[0][rot], y = kOffTile[1][rot];
    bool crossed = (x > 0 && x <= o->offX) || (x < 0 && x >= o->offX) || (y > 0 && y <= o->offY) || (y < 0 && y >= o->offY);
    if (!crossed) return;
    int remX = o->offX - x, remY = o->offY - y;
    int next = hex::inDirection(o->tile, rot, 1);
    Object* blocker = g_world.blockingAt(o, next, o->elevation);
    if (blocker && blocker != o) {
        extern void objUseDoor(Object * user, Object * door, bool passThrough);
        if (canUseDoor(o, blocker)) {
            objUseDoor(o, blocker, true);
        } else {
            std::vector<int> rots = findPath(o, o->tile, r.destTile, o->elevation, true);
            if (!rots.empty() && g_world.blockingAt(o, r.destTile, o->elevation)) rots.pop_back();
            if (rots.empty()) {
                finishRunner(r);
                return;
            }
            r.rotations = std::move(rots);
            r.pathPos = 0;
            g_world.place(o, o->tile, o->elevation);
            o->frame = 0;
            o->rotation = r.rotations[0];
            return;
        }
    }
    int oldTile = o->tile;
    g_world.place(o, next, o->elevation);
    if (oldTile != next) tileChanged(o);
    if (o->destroyed || o->tile != next) {
        finishRunner(r);
        return;
    }
    bool outOfAp = false;
    if (inCombat() && o->isCritter() && g_animCombatStep) outOfAp = !g_animCombatStep(o);
    r.pathPos++;
    if (r.pathPos >= r.rotations.size() || outOfAp || r.stopMove) {
        finishRunner(r);
        return;
    }
    o->rotation = r.rotations[r.pathPos];
    o->offX = remX;
    o->offY = remY;
}

static void stepStraight(Runner& r, Object* o, bool noAnim)
{
    if (!r.started) {
        r.started = true;
        if (!noAnim) {
            o->fid = r.fid;
            o->frame = 0;
        }
    } else if (!noAnim) {
        o->frame = (o->frame + 1) % frameCount(o->fid);
    }
    if (r.pathPos < r.straight.size()) {
        int oldTile = o->tile;
        g_world.place(o, r.straight[r.pathPos], o->elevation);
        if (oldTile != o->tile) tileChanged(o);
        r.pathPos++;
    }
    if (r.pathPos >= r.straight.size()) finishRunner(r);
}

static void stepAnimate(Runner& r, Object* o)
{
    bool reverse = r.kind == K_ANIMATE_REV;
    bool forever = r.kind == K_ANIMATE_FOREVER;
    if (!r.started) {
        r.started = true;
        if (o->fid != r.fid) {
            int sx, sy;
            artShift(o->fid, o->rotation, sx, sy);
            o->fid = r.fid;
            o->frame = reverse ? frameCount(o->fid) - 1 : 0;
            int hx, hy;
            frameHot(o->fid, o->frame, o->rotation, hx, hy);
            if (fidType(o->fid) == OBJ_CRITTER) {
                o->offX += sx + hx;
                o->offY += sy + hy;
            }
            return;
        }
    }
    int count = frameCount(o->fid);
    if (!reverse) {
        if (!forever && o->frame >= count - 1) {
            if (r.kind == K_ANIMATE_HIDE) o->flags |= OF_HIDDEN;
            finishRunner(r);
            return;
        }
        o->frame = (o->frame + 1) % count;
        r.framesDone++;
        if (fidType(o->fid) == OBJ_CRITTER) {
            int hx, hy;
            frameHot(o->fid, o->frame, o->rotation, hx, hy);
            o->offX += hx;
            o->offY += hy;
        }
    } else {
        if (forever || o->frame != 0) {
            int hx, hy;
            frameHot(o->fid, o->frame, o->rotation, hx, hy);
            o->frame = o->frame > 0 ? o->frame - 1 : count - 1;
            r.framesDone++;
            if (fidType(o->fid) == OBJ_CRITTER) {
                o->offX -= hx;
                o->offY -= hy;
            }
        } else {
            finishRunner(r);
        }
    }
}

// Starts description `i` of sequence `s`. Returns true if it completed instantly.
static bool startDesc(Seq& s, size_t i)
{
    Desc& d = s.descs[i];
    Object* o = g_world.byUid(d.obj);
    if (d.kind != K_CALLBACK && (!o || o->destroyed)) return true;
    switch (d.kind) {
    case K_MOVE_TILE:
    case K_MOVE_OBJ: {
        Runner r;
        r.seq = s.id;
        r.desc = i;
        r.obj = d.obj;
        r.kind = d.kind;
        if (!startMove(r, o, d)) return true;
        g_runners.push_back(std::move(r));
        return false;
    }
    case K_MOVE_STRAIGHT: {
        Runner r;
        r.seq = s.id;
        r.desc = i;
        r.obj = d.obj;
        r.kind = d.kind;
        Object* blocker = nullptr;
        r.straight = straightPath(o, o->tile, d.tile, o->elevation, &blocker);
        if (r.straight.empty()) return true;
        r.fid = d.anim >= 0 ? animFid(o, d.anim) : o->fid;
        r.tpf = animFrameTime(o, r.fid);
        g_runners.push_back(std::move(r));
        return false;
    }
    case K_ANIMATE:
    case K_ANIMATE_REV:
    case K_ANIMATE_HIDE:
    case K_ANIMATE_FOREVER: {
        int fid = fidType(o->fid) == OBJ_CRITTER ? animFid(o, d.anim) : o->fid;
        if (!artOf(fid)) {
            if (d.kind == K_ANIMATE_HIDE) o->flags |= OF_HIDDEN;
            return true;
        }
        Runner r;
        r.seq = s.id;
        r.desc = i;
        r.obj = d.obj;
        r.kind = d.kind;
        r.fid = fid;
        r.tpf = animFrameTime(o, fid);
        // other running in-place animations of the same object end now
        for (auto& other : g_runners)
            if (other.obj == d.obj && (other.kind == K_ANIMATE_FOREVER)) other.done = true;
        g_runners.push_back(std::move(r));
        return false;
    }
    case K_ROTATE_TILE:
        if (!critterProne(o) && hex::valid(d.tile) && d.tile != o->tile) critterStand(o, hex::direction(o->tile, d.tile));
        return true;
    case K_ROTATE_CW:
        o->rotation = (o->rotation + 1) % 6;
        return true;
    case K_ROTATE_CCW:
        o->rotation = (o->rotation + 5) % 6;
        return true;
    case K_HIDE:
        o->flags |= OF_HIDDEN;
        return true;
    case K_CALLBACK:
        if (d.fn) d.fn();
        return true;
    case K_SET_FID:
        o->fid = d.fid;
        o->frame = 0;
        return true;
    case K_TAKE_OUT: {
        int fid = makeFid(OBJ_CRITTER, fidIndex(o->fid), ANIM_TAKE_OUT, d.value);
        if (!artOf(fid)) {
            o->fid = makeFid(OBJ_CRITTER, fidIndex(o->fid), ANIM_STAND, d.value);
            return true;
        }
        Runner r;
        r.seq = s.id;
        r.desc = i;
        r.obj = d.obj;
        r.kind = K_ANIMATE;
        r.fid = fid;
        r.tpf = animFrameTime(o, fid);
        g_runners.push_back(std::move(r));
        return false;
    }
    case K_SFX:
        soundPlaySfxAt(d.sfx, o);
        return true;
    case K_SET_FLAG:
        o->flags |= d.flag;
        return true;
    case K_UNSET_FLAG:
        o->flags &= ~d.flag;
        return true;
    case K_SET_LIGHT:
        o->lightRadius = d.value;
        return true;
    }
    return true;
}

void animUpdate(float dt)
{
    float ms = dt * 1000.0f;
    // tick runners
    for (size_t i = 0; i < g_runners.size(); i++) {
        Runner& r = g_runners[i];
        if (r.done) continue;
        Object* o = g_world.byUid(r.obj);
        if (!o || o->destroyed || o->tile < 0) {
            r.done = true;
            continue;
        }
        r.acc += ms;
        if (!r.started) r.acc = std::max(r.acc, (float)r.tpf);
        int steps = 0;
        while (r.acc >= r.tpf && !r.done && steps < 4) {
            r.acc -= r.tpf;
            steps++;
            switch (r.kind) {
            case K_MOVE_TILE:
            case K_MOVE_OBJ: stepMove(r, o); break;
            case K_MOVE_STRAIGHT: stepStraight(r, o, false); break;
            default: stepAnimate(r, o); break;
            }
        }
        if (r.acc > r.tpf * 4) r.acc = 0;
    }
    // completions
    for (auto& r : g_runners) {
        if (!r.done) continue;
        for (auto& s : g_seqs)
            if (s->id == r.seq && r.desc < s->state.size()) s->state[r.desc] = 2;
    }
    g_runners.erase(std::remove_if(g_runners.begin(), g_runners.end(), [](const Runner& r) { return r.done; }), g_runners.end());
    // advance sequences
    for (size_t si = 0; si < g_seqs.size(); si++) {
        Seq& s = *g_seqs[si];
        if (s.ended) continue;
        bool progressed = true;
        int guard = 0;
        while (progressed && !s.ended && guard++ < 64) {
            progressed = false;
            for (size_t i = 0; i < s.descs.size(); i++) {
                if (s.state[i] != 0) continue;
                bool prevDone = true;
                for (size_t k = 0; k < i; k++)
                    if (s.state[k] != 2) prevDone = false;
                int delay = s.descs[i].delay;
                if (delay < 0 && !prevDone) break;
                if (delay > 0 && !prevDone) {
                    // positive delay: start once the previous description has played `delay` frames
                    bool ready = i > 0 && s.state[i - 1] == 2;
                    if (!ready && i > 0 && s.state[i - 1] == 1) {
                        for (auto& r : g_runners)
                            if (r.seq == s.id && r.desc == i - 1 && r.framesDone >= delay) ready = true;
                    }
                    if (!ready) break;
                }
                s.state[i] = 1;
                if (startDesc(s, i)) s.state[i] = 2;
                progressed = true;
                break;
            }
        }
        if (s.ended) continue;
        bool all = std::all_of(s.state.begin(), s.state.end(), [](u8 v) { return v == 2; });
        if (all) endSeq(s, false);
    }
    g_seqs.erase(std::remove_if(g_seqs.begin(), g_seqs.end(), [](const std::unique_ptr<Seq>& s) { return s->ended; }), g_seqs.end());
}

bool animBusy(Object* obj)
{
    if (!obj) return false;
    for (auto& s : g_seqs) {
        if (s->ended) continue;
        for (size_t i = 0; i < s->descs.size(); i++)
            if (s->descs[i].obj == obj->uid && s->state[i] != 2) return true;
    }
    return false;
}

bool animBusyAny(bool combatOnly)
{
    for (auto& s : g_seqs) {
        if (s->ended) continue;
        if (combatOnly && !(s->flags & ANIMSEQ_COMBAT)) continue;
        return true;
    }
    return false;
}

void animStop(Object* obj)
{
    if (!obj) return;
    for (auto& s : g_seqs) {
        if (s->ended) continue;
        for (auto& d : s->descs) {
            if (d.obj == obj->uid) {
                endSeq(*s, true);
                break;
            }
        }
    }
}

void animStopAll()
{
    for (auto& s : g_seqs) {
        if (s->ended) continue;
        s->ended = true; // no stand / forced callbacks when tearing down a map
    }
    g_seqs.clear();
    g_runners.clear();
    g_building.reset();
}

} // namespace fo
