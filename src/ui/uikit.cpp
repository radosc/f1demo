#include "ui/uikit.h"
#include "audio/sound.h"

namespace fo {
namespace ui {

static bool s_draw = false;
static u64 s_active = 0;      // widget holding the left mouse button
static int s_takenFrame = -1; // frame in which a widget took the mouse

Pass::Pass(bool draw) : prev(s_draw) { s_draw = draw; }

Pass::~Pass()
{
    // a press that ended outside every widget releases the capture
    if (!s_draw && g_app.input.released[0]) s_active = 0;
    s_draw = prev;
}

bool drawing() { return s_draw; }

float originX() { return (float)g_app.uiOffsetX; }
float originY() { return (float)g_app.uiOffsetY; }
float texScale() { return (float)g_app.uiTex; }
float mouseX() { return g_app.input.uiX - originX(); }
float mouseY() { return g_app.input.uiY - originY(); }

bool mouseIn(float x, float y, float w, float h)
{
    if (!g_app.input.mouseInWindow) return false;
    float mx = mouseX(), my = mouseY();
    return mx >= x && my >= y && mx < x + w && my < y + h;
}

bool mouseTaken() { return s_takenFrame == g_app.frameNo(); }
void takeMouse() { s_takenFrame = g_app.frameNo(); }

Sprite* art(const std::string& name) { return g_sprites.get("art/intrface/" + name); }

int artW(const std::string& name, int frame)
{
    Sprite* s = art(name);
    return s ? s->frame(0, frame).w : 0;
}

int artH(const std::string& name, int frame)
{
    Sprite* s = art(name);
    return s ? s->frame(0, frame).h : 0;
}

void drawSpriteScaled(Sprite* s, int dir, int frame, float x, float y, float w, float h, const DrawParams& p)
{
    if (!s_draw || !s) return;
    float S = texScale();
    int ts = 1;
    TexturePtr tex = g_sprites.texture(s, (int)S, ts);
    if (!tex) return;
    const SheetFrame& f = s->frame(dir, frame);
    DrawParams q = p;
    if (q.texScaleDiv <= 1) q.texScaleDiv = std::max(1, ts / std::max(1, (int)S));
    g_renderer.draw(tex, (float)f.x * ts, (float)f.y * ts, (float)f.w * ts, (float)f.h * ts, (originX() + x) * S, (originY() + y) * S, w * S, h * S, q);
}

void drawSprite(Sprite* s, int dir, int frame, float x, float y, const DrawParams& p)
{
    if (!s_draw || !s) return;
    const SheetFrame& f = s->frame(dir, frame);
    drawSpriteScaled(s, dir, frame, x, y, (float)f.w, (float)f.h, p);
}

void draw(const std::string& name, float x, float y, int frame, const DrawParams& p)
{
    if (!s_draw) return;
    drawSprite(art(name), 0, frame, x, y, p);
}

void drawPart(const std::string& name, int sx, int sy, int sw, int sh, float x, float y, const DrawParams& p)
{
    if (!s_draw) return;
    Sprite* s = art(name);
    if (!s || sw <= 0 || sh <= 0) return;
    float S = texScale();
    int ts = 1;
    TexturePtr tex = g_sprites.texture(s, (int)S, ts);
    if (!tex) return;
    const SheetFrame& f = s->frame(0, 0);
    DrawParams q = p;
    if (q.texScaleDiv <= 1) q.texScaleDiv = std::max(1, ts / std::max(1, (int)S));
    g_renderer.draw(tex, (float)(f.x + sx) * ts, (float)(f.y + sy) * ts, (float)sw * ts, (float)sh * ts, (originX() + x) * S, (originY() + y) * S, sw * S,
        sh * S, q);
}

void fill(float x, float y, float w, float h, u32 argb)
{
    if (!s_draw) return;
    float S = texScale();
    g_renderer.fillRect((originX() + x) * S, (originY() + y) * S, w * S, h * S, argb);
}

void outline(float x, float y, float w, float h, u32 argb)
{
    fill(x, y, w, 1, argb);
    fill(x, y + h - 1, w, 1, argb);
    fill(x, y, 1, h, argb);
    fill(x + w - 1, y, 1, h, argb);
}

void letterbox(u32 argb)
{
    if (!s_draw) return;
    float S = texScale();
    float ox = originX(), oy = originY();
    float W = (float)g_app.uiW, H = (float)g_app.uiH;
    if (ox > 0) {
        g_renderer.fillRect(0, 0, ox * S, H * S, argb);
        g_renderer.fillRect((ox + 640) * S, 0, (W - ox - 640) * S, H * S, argb);
    }
    if (oy > 0) g_renderer.fillRect(ox * S, 0, 640 * S, oy * S, argb);
    if (oy + 480 < H) g_renderer.fillRect(ox * S, (oy + 480) * S, 640 * S, (H - oy - 480) * S, argb);
}

void text(int fontId, const std::string& s, float x, float y, u32 color, u32 flags, float alpha)
{
    if (!s_draw || s.empty()) return;
    Font* f = font(fontId);
    if (!f) return;
    if (flags & (T_CENTER | T_RIGHT)) {
        float w = (float)f->textWidth(s);
        x -= (flags & T_CENTER) ? floorf(w / 2) : w;
    }
    float S = texScale();
    if (flags & T_SHADOW) f->draw(s, (originX() + x + 1) * S, (originY() + y + 1) * S, S, 0xFF000000u, alpha);
    f->draw(s, (originX() + x) * S, (originY() + y) * S, S, color, alpha);
}

int textWidth(int fontId, const std::string& s)
{
    Font* f = font(fontId);
    return f ? f->textWidth(s) : 0;
}

int lineHeight(int fontId)
{
    Font* f = font(fontId);
    return f ? f->height() : 10;
}

std::vector<std::string> wrap(int fontId, const std::string& s, int width)
{
    Font* f = font(fontId);
    if (!f) return { s };
    return f->wrap(s, width);
}

float textBox(int fontId, const std::string& s, float x, float y, int width, u32 color, float maxY, int skipLines)
{
    int lh = lineHeight(fontId);
    int n = 0;
    for (auto& line : wrap(fontId, s, width)) {
        if (n++ < skipLines) continue;
        if (y + lh > maxY) break;
        text(fontId, line, x, y, color);
        y += (float)lh;
    }
    return y;
}

void playClick(int sound, bool press)
{
    if (sound == SND_NONE) return;
    soundPlaySfx(strFormat(press ? "butin%d" : "butout%d", sound));
}

static u64 widgetId(const void* p, float x, float y)
{
    u64 h = (u64)(uintptr_t)p * 0x9E3779B97F4A7C15ull;
    h ^= (u64)(i64)(x * 8) * 0xC2B2AE3D27D4EB4Full;
    h ^= (u64)(i64)(y * 8) * 0x165667B19E3779F9ull;
    return h | 1;
}

static bool clickLogic(u64 id, bool inside, int hotkey, int sound)
{
    Input& in = g_app.input;
    bool clicked = false;
    if (in.pressed[0] && inside && !mouseTaken()) {
        s_active = id;
        takeMouse();
        playClick(sound, true);
    }
    if (s_active == id) {
        takeMouse();
        if (in.released[0]) {
            s_active = 0;
            if (inside) {
                clicked = true;
                playClick(sound, false);
            }
        }
    }
    if (hotkey && in.key(hotkey)) {
        clicked = true;
        playClick(sound, true);
    }
    return clicked;
}

bool button(const char* up, const char* down, float x, float y, int hotkey, int sound, bool enabled)
{
    Sprite* su = art(up);
    float w = su ? (float)su->frame(0, 0).w : 16.0f, h = su ? (float)su->frame(0, 0).h : 16.0f;
    u64 id = widgetId(up, x, y);
    bool inside = mouseIn(x, y, w, h);
    if (!s_draw) {
        if (!enabled) return false;
        return clickLogic(id, inside, hotkey, sound);
    }
    bool pressedLook = enabled && s_active == id && g_app.input.down[0] && inside;
    DrawParams p;
    if (!enabled) p.light = 0.5f;
    if (pressedLook && down) draw(down, x, y, 0, p);
    else drawSprite(su, 0, 0, x, y, p);
    return false;
}

bool area(float x, float y, float w, float h, int hotkey, int sound)
{
    if (s_draw) return false;
    u64 id = widgetId(nullptr, x, y) ^ ((u64)(i64)(w * 16) << 20) ^ ((u64)(i64)(h * 16) << 40);
    return clickLogic(id, mouseIn(x, y, w, h), hotkey, sound);
}

bool hovered(float x, float y, float w, float h) { return mouseIn(x, y, w, h); }

bool rightClicked(float x, float y, float w, float h)
{
    if (s_draw) return false;
    if (g_app.input.pressed[1] && mouseIn(x, y, w, h) && !mouseTaken()) {
        takeMouse();
        return true;
    }
    return false;
}

bool key(int vk) { return !s_draw && g_app.input.key(vk); }

const std::vector<u32>& chars()
{
    static const std::vector<u32> kNone;
    return s_draw ? kNone : g_app.input.chars;
}

} // namespace ui
} // namespace fo
