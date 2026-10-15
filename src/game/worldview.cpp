#include "game/worldview.h"
#include "game/settings.h"
#include "game/art.h"
#include "game/critter.h"
#include "game/game.h"
#include "game/hex.h"
#include "game/map.h"
#include "gfx/font.h"
#include "gfx/sprite.h"

namespace fo {

WorldView g_view;

// ------------------------------------------------------------------ light map
static std::vector<int> g_lightMap;     // per hex intensity for the current elevation
static int g_lightElevation = -1;
static u64 g_lightHash = 0;

static void rebuildLight(int elevation)
{
    // hash of all light sources to avoid recomputing every frame
    u64 h = 1469598103934665603ull + (u64)elevation;
    std::vector<Object*> lights;
    g_world.forEach([&](Object* o) {
        if (o->tile < 0 || o->elevation != elevation || o->lightIntensity <= 0 || o->lightRadius <= 0) return;
        if (o->flags & OF_HIDDEN) return;
        lights.push_back(o);
        h = (h ^ (u64)o->tile) * 1099511628211ull;
        h = (h ^ (u64)o->lightIntensity) * 1099511628211ull;
        h = (h ^ (u64)o->lightRadius) * 1099511628211ull;
    });
    if (h == g_lightHash && elevation == g_lightElevation && !g_lightMap.empty()) return;
    g_lightHash = h;
    g_lightElevation = elevation;
    g_lightMap.assign(kHexes, 0);
    for (Object* o : lights) {
        int r = std::min(o->lightRadius, 8);
        int cx, cy;
        hex::toWorld(o->tile, cx, cy);
        // visit hexes in a bounding area (light radius in hexes ~ 32px horizontally per hex)
        int row = o->tile / kHexW, col = o->tile % kHexW;
        for (int dr = -r - 1; dr <= r + 1; dr++) {
            for (int dc = -2 * r - 2; dc <= 2 * r + 2; dc++) {
                int rr = row + dr, cc = col + dc;
                if (rr < 0 || rr >= kHexH || cc < 0 || cc >= kHexW) continue;
                int t = rr * kHexW + cc;
                int d = hex::distance(o->tile, t);
                if (d > r) continue;
                int add = o->lightIntensity - o->lightIntensity * d / (r + 1);
                g_lightMap[t] = std::min(g_lightMap[t] + add, 65536 * 2);
            }
        }
    }
}

float WorldView::lightAt(int tile, int elevation) const
{
    // the Night Vision perk brightens the ambient light by 10% per rank (drawing and to-hit alike)
    int ambient = std::min(65536, g_game.lightLevel + perkLevel(PERK_NIGHT_VISION) * 6553);
    int v = ambient;
    if (elevation == g_lightElevation && hex::valid(tile) && !g_lightMap.empty()) v = std::max(ambient, std::min(g_lightMap[tile] + ambient / 2, 65536));
    return std::min(1.0f, (float)v / 65536.0f);
}

// ------------------------------------------------------------------ camera
void WorldView::centerOn(int tile)
{
    if (!hex::valid(tile)) return;
    int x, y;
    hex::center(tile, x, y);
    camX = (float)x;
    camY = (float)y;
}

void WorldView::scroll(float dx, float dy)
{
    camX += dx;
    camY += dy;
    clampCamera();
}

void WorldView::clampCamera()
{
    // keep the camera inside the hex grid's bounding box
    int x0, y0, x1, y1, x2, y2, x3, y3;
    hex::center(0, x0, y0);
    hex::center(kHexW - 1, x1, y1);
    hex::center(kHexes - kHexW, x2, y2);
    hex::center(kHexes - 1, x3, y3);
    float minX = (float)std::min(std::min(x0, x1), std::min(x2, x3));
    float maxX = (float)std::max(std::max(x0, x1), std::max(x2, x3));
    float minY = (float)std::min(std::min(y0, y1), std::min(y2, y3));
    float maxY = (float)std::max(std::max(y0, y1), std::max(y2, y3));
    camX = clampv(camX, minX, maxX);
    camY = clampv(camY, minY, maxY);
}

int WorldView::hexAt(float cx, float cy) const
{
    return hex::fromWorld((int)floorf(toWorldX(cx)), (int)floorf(toWorldY(cy)));
}

// ------------------------------------------------------------------ helpers
static Sprite* spriteFor(const Object* o) { return g_art.sprite(o->fid); }

bool WorldView::objectRect(const Object* o, float& x, float& y, float& w, float& h) const
{
    if (o->tile < 0) return false;
    Sprite* s = spriteFor(o);
    if (!s) return false;
    const SheetFrame& f = s->frame(o->rotation, o->frame);
    int cx, cy;
    hex::center(o->tile, cx, cy);
    int dir = s->meta.dirCount > 1 ? o->rotation : 0;
    float ax = (float)(cx + s->meta.shiftX[dir] + o->offX);
    float ay = (float)(cy + s->meta.shiftY[dir] + o->offY);
    x = toCanvasX(ax - (float)(f.w / 2));
    y = toCanvasY(ay - (float)(f.h - 1));
    w = (float)f.w;
    h = (float)f.h;
    return true;
}

bool WorldView::eggCovers(const Object* o) const
{
    Object* dude = g_world.dude;
    if (!eggEnabled || !dude || dude->tile < 0 || (dude->flags & OF_HIDDEN)) return false;
    int t = fidType(o->fid);
    if (t != OBJ_SCENERY && t != OBJ_WALL) return false;
    if (o->flags & OF_TRANS_ANY) return false;
    const Proto* p = o->proto();
    u32 ext = p ? p->extFlags : 0;
    bool v;
    if ((ext & PXF_WALL_EAST_WEST) || (ext & PXF_WALL_WEST_CORNER)) {
        v = hex::inFrontOf(o->tile, dude->tile);
        if (v && hex::toRightOf(o->tile, dude->tile) && (o->flags & OF_WALL_TRANS_END)) v = false;
    } else if (ext & PXF_WALL_NORTH_CORNER) {
        v = hex::inFrontOf(o->tile, dude->tile) || hex::toRightOf(dude->tile, o->tile);
    } else if (ext & PXF_WALL_SOUTH_CORNER) {
        v = hex::inFrontOf(o->tile, dude->tile) && hex::toRightOf(dude->tile, o->tile);
    } else {
        v = hex::toRightOf(dude->tile, o->tile);
        if (v && hex::inFrontOf(dude->tile, o->tile) && (o->flags & OF_WALL_TRANS_END)) v = false;
    }
    return v;
}

void WorldView::drawObject(Object* o, float light)
{
    Sprite* s = spriteFor(o);
    if (!s) return;
    float x, y, w, h;
    if (!objectRect(o, x, y, w, h)) return;
    if (x > canvasW || y > canvasH || x + w < 0 || y + h < 0) return;
    int texScale = 1;
    TexturePtr tex = g_sprites.texture(s, this->texScale, texScale);
    if (!tex) return;
    const SheetFrame& f = s->frame(o->rotation, o->frame);
    DrawParams p;
    p.light = light;
    p.texScaleDiv = std::max(1, texScale / this->texScale);
    p.egg = eggCovers(o);
    if (o->flags & OF_TRANS_ANY) {
        if (o->flags & OF_TRANS_RED) p.color = 0xFFFF6060u, p.alphaMul = 0.6f;
        else if (o->flags & OF_TRANS_WALL) p.alphaMul = 0.5f;
        else if (o->flags & OF_TRANS_GLASS) p.color = 0xFFC0E0FFu, p.alphaMul = 0.5f;
        else if (o->flags & OF_TRANS_STEAM) p.alphaMul = 0.4f;
        else if (o->flags & OF_TRANS_ENERGY) p.color = 0xFFFFFF80u, p.alphaMul = 0.6f;
    }
    float s1 = (float)this->texScale;
    float ts = (float)texScale;
    if (o->outline) {
        // outline: the silhouette in the outline colour shifted one game pixel in 8 directions, sprite on top
        DrawParams q = p;
        q.flatColor = o->outline;
        q.egg = false;
        q.light = 1.0f;
        static const int kDirs[8][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 }, { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, 1 } };
        for (auto& d : kDirs)
            g_renderer.draw(tex, f.x * ts, f.y * ts, f.w * ts, f.h * ts, (x + d[0]) * s1, (y + d[1]) * s1, w * s1, h * s1, q);
    }
    g_renderer.draw(tex, f.x * ts, f.y * ts, f.w * ts, f.h * ts, x * s1, y * s1, w * s1, h * s1, p);
}

// ------------------------------------------------------------------ passes
void WorldView::drawFloor(int e)
{
    if (!g_map.hasElevation[e]) return;
    float s1 = (float)texScale;
    float viewL = camX - canvasW * 0.5f - 96, viewR = camX + canvasW * 0.5f + 96;
    float viewT = camY - canvasH * 0.5f - 64, viewB = camY + canvasH * 0.5f + 64;
    for (int sq = 0; sq < kSquares; sq++) {
        int id = g_map.floor[e][sq];
        if (id <= 1) continue; // 0/1 = no tile
        int wx, wy;
        hex::squareToWorld(sq, wx, wy);
        if (wx > viewR || wy > viewB || wx + 80 < viewL || wy + 36 < viewT) continue;
        Sprite* s = g_art.sprite(makeFid(OBJ_TILE, id));
        if (!s) continue;
        int ts = 1;
        TexturePtr tex = g_sprites.texture(s, texScale, ts);
        if (!tex) continue;
        const SheetFrame& f = s->frame(0, 0);
        DrawParams p;
        p.texScaleDiv = std::max(1, ts / texScale);
        // per-corner lighting from the hexes under the tile corners
        int t0 = hex::fromWorld(wx + 16, wy + 13);
        float l = lightAt(t0, e);
        p.light = l;
        int tl = hex::fromWorld(wx + 2, wy + 18), tr = hex::fromWorld(wx + 64, wy + 2);
        int br = hex::fromWorld(wx + 78, wy + 18), bl = hex::fromWorld(wx + 16, wy + 34);
        p.lightCorners[0] = lightAt(tl >= 0 ? tl : t0, e);
        p.lightCorners[1] = lightAt(tr >= 0 ? tr : t0, e);
        p.lightCorners[2] = lightAt(br >= 0 ? br : t0, e);
        p.lightCorners[3] = lightAt(bl >= 0 ? bl : t0, e);
        float cx = toCanvasX((float)wx), cy = toCanvasY((float)wy);
        g_renderer.draw(tex, f.x * (float)ts, f.y * (float)ts, f.w * (float)ts, f.h * (float)ts, cx * s1, cy * s1, f.w * s1, f.h * s1, p);
    }
}

void WorldView::drawObjects(int e)
{
    m_drawList.clear();
    float viewL = camX - canvasW * 0.5f - 400, viewR = camX + canvasW * 0.5f + 400;
    float viewT = camY - canvasH * 0.5f - 300, viewB = camY + canvasH * 0.5f + 400;
    g_world.forEach([&](Object* o) {
        if (o->tile < 0 || o->elevation != e || (o->flags & OF_HIDDEN)) return;
        int x, y;
        hex::center(o->tile, x, y);
        if (x < viewL || x > viewR || y < viewT || y > viewB) return;
        m_drawList.push_back(o);
    });
    std::stable_sort(m_drawList.begin(), m_drawList.end(), [](const Object* a, const Object* b) {
        bool fa = a->isFlat(), fb = b->isFlat();
        if (fa != fb) return fa;
        if (a->tile != b->tile) return a->tile < b->tile;
        return a->uid < b->uid;
    });
    for (Object* o : m_drawList) {
        drawObject(o, lightAt(o->tile, e));
        if (markSeen && !(o->flags & OF_SEEN)) {
            float x, y, w, h;
            if (objectRect(o, x, y, w, h) && x + w > 0 && y + h > 0 && x < canvasW && y < canvasH) o->flags |= OF_SEEN;
        }
    }
}

void WorldView::drawRoofs(int e)
{
    if (!showRoofs || !g_map.hasElevation[e]) return;
    float s1 = (float)texScale;
    float viewL = camX - canvasW * 0.5f - 96, viewR = camX + canvasW * 0.5f + 96;
    float viewT = camY - canvasH * 0.5f - 160, viewB = camY + canvasH * 0.5f + 64;
    float ambient = std::min(1.0f, g_game.lightLevel / 65536.0f);
    for (int sq = 0; sq < kSquares; sq++) {
        int id = g_map.roof[e][sq];
        if (id <= 1 || g_map.roofHidden[e][sq]) continue;
        int wx, wy;
        hex::roofToWorld(sq, wx, wy);
        if (wx > viewR || wy > viewB || wx + 80 < viewL || wy + 36 < viewT) continue;
        Sprite* s = g_art.sprite(makeFid(OBJ_TILE, id));
        if (!s) continue;
        int ts = 1;
        TexturePtr tex = g_sprites.texture(s, texScale, ts);
        if (!tex) continue;
        const SheetFrame& f = s->frame(0, 0);
        DrawParams p;
        p.texScaleDiv = std::max(1, ts / texScale);
        p.light = ambient;
        p.egg = eggEnabled && g_world.dude != nullptr;
        float cx = toCanvasX((float)wx), cy = toCanvasY((float)wy);
        g_renderer.draw(tex, f.x * (float)ts, f.y * (float)ts, f.w * (float)ts, f.h * (float)ts, cx * s1, cy * s1, f.w * s1, f.h * s1, p);
    }
}

void WorldView::drawOverlays(int e, float dt)
{
    float s1 = (float)texScale;
    // hex cursor
    if (hoverHex >= 0 && hoverHexColor) {
        int x, y;
        hex::toWorld(hoverHex, x, y);
        float cx = toCanvasX((float)x), cy = toCanvasY((float)y);
        // outline of a 32x16 hex
        float px[6] = { 16, 32, 32, 16, 0, 0 };
        float py[6] = { 0, 4, 12, 16, 12, 4 };
        for (int i = 0; i < 6; i++) {
            int j = (i + 1) % 6;
            g_renderer.line((cx + px[i]) * s1, (cy + py[i]) * s1, (cx + px[j]) * s1, (cy + py[j]) * s1, hoverHexColor, s1);
        }
    }
    for (int t : pathPreview) {
        int x, y;
        hex::toWorld(t, x, y);
        float cx = toCanvasX((float)x + 12), cy = toCanvasY((float)y + 6);
        g_renderer.fillRect(cx * s1, cy * s1, 8 * s1, 4 * s1, 0x6000FF00u);
    }
    // floating texts
    Font* f = font(101);
    for (auto& ft : floats) ft.timeLeft -= dt;
    floats.erase(std::remove_if(floats.begin(), floats.end(), [](const FloatingText& t) { return t.timeLeft <= 0; }), floats.end());
    for (auto& ft : floats) {
        Object* o = g_world.byUid(ft.objUid);
        if (!o || o->tile < 0 || o->elevation != e) continue;
        float x, y, w, h;
        if (!objectRect(o, x, y, w, h)) continue;
        Font* ff = font(ft.fontId);
        if (!ff) ff = f;
        if (!ff) continue;
        int lineH = ff->lineHeight() + 1;
        float top = y - lineH * (float)ft.lines.size() - 4;
        float alpha = std::min(1.0f, ft.timeLeft * 2.0f);
        for (size_t i = 0; i < ft.lines.size(); i++) {
            float tw = (float)ff->textWidth(ft.lines[i]);
            float tx = x + w * 0.5f - tw * 0.5f;
            float ty = top + i * lineH;
            ff->draw(ft.lines[i], (tx + 1) * s1, (ty + 1) * s1, s1, 0xFF000000u, alpha);
            ff->draw(ft.lines[i], tx * s1, ty * s1, s1, ft.color, alpha);
        }
    }
}

void WorldView::addFloatText(Object* obj, const std::string& text, u32 color, int fontId)
{
    if (!obj) return;
    Font* f = font(fontId);
    FloatingText ft;
    ft.objUid = obj->uid;
    ft.color = color;
    ft.fontId = fontId;
    ft.lines = f ? f->wrap(text, 200) : std::vector<std::string>{ text };
    // display time follows the "text delay" preference (base seconds) plus a little per character
    ft.timeLeft = g_settings.textDelay + 0.03f * (float)text.size();
    // replace an existing text on the same object
    floats.erase(std::remove_if(floats.begin(), floats.end(), [&](const FloatingText& t) { return t.objUid == obj->uid; }), floats.end());
    floats.push_back(ft);
}

void WorldView::draw(float dt)
{
    int e = g_game.elevation;
    rebuildLight(e);
    // egg mask follows the player
    Object* dude = g_world.dude;
    Sprite* egg = g_sprites.get("art/intrface/egg");
    if (egg && dude && dude->tile >= 0 && eggEnabled) {
        static TexturePtr eggTex;
        static Sprite* eggSprite = nullptr;
        if (!eggTex || eggSprite != egg) {
            const SheetFrame& f = egg->frame(0, 0);
            std::vector<u8> vals((size_t)f.w * f.h);
            for (int y = 0; y < f.h; y++)
                for (int x = 0; x < f.w; x++) vals[(size_t)y * f.w + x] = egg->image.indexed ? egg->image.row(f.y + y)[f.x + x] : 0;
            eggTex = g_renderer.createTextureRaw8(f.w, f.h, vals.data());
            eggSprite = egg;
        }
        const SheetFrame& f = egg->frame(0, 0);
        int cx, cy;
        hex::center(dude->tile, cx, cy);
        float ex = toCanvasX((float)(cx + egg->meta.shiftX[0] + dude->offX - f.w / 2));
        float ey = toCanvasY((float)(cy + egg->meta.shiftY[0] + dude->offY - (f.h - 1)));
        float s1 = (float)texScale;
        g_renderer.setEgg(eggTex, ex * s1, ey * s1, f.w * s1, f.h * s1);
    } else {
        g_renderer.clearEgg();
    }
    drawFloor(e);
    drawObjects(e);
    drawRoofs(e);
    g_renderer.clearEgg();
    drawOverlays(e, dt);
}

Object* WorldView::pick(float cx, float cy, u32 typeMask, bool includeDude) const
{
    // walk the last draw list backwards (front-most first)
    for (size_t i = m_drawList.size(); i-- > 0;) {
        Object* o = m_drawList[i];
        if (o->destroyed || o->tile < 0 || (o->flags & OF_HIDDEN)) continue;
        if (!(typeMask & (1u << o->type()))) continue;
        if (!includeDude && o == g_world.dude) continue;
        float x, y, w, h;
        if (!objectRect(o, x, y, w, h)) continue;
        if (cx < x || cy < y || cx >= x + w || cy >= y + h) continue;
        Sprite* s = spriteFor(o);
        if (s && s->opaqueAt(o->rotation, o->frame, (int)(cx - x), (int)(cy - y))) return o;
    }
    return nullptr;
}

} // namespace fo
