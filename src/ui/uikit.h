// Immediate-mode helpers for interface screens laid out on the original 640x480 grid.
//
// Screens describe their interface in one function that runs twice per frame: once from update() as the
// logic pass (hit testing, clicks, hotkeys - nothing is drawn) and once from drawUI() as the draw pass
// (nothing is clicked). Use ui::Pass to select the pass. Coordinates are layout pixels (0..640, 0..480).
#pragma once
#include "ui/app.h"
#include "gfx/font.h"
#include "gfx/sprite.h"

namespace fo {
namespace ui {

// Fallout palette colours used by the interface (RGB555 colour table entries converted to ARGB).
constexpr u32 kGreen = 0xFF00FF00u;       // colorTable[992]
constexpr u32 kDarkGreen = 0xFF00C500u;   // colorTable[768]
constexpr u32 kYellow = 0xFFFFFF5Au;      // colorTable[32747]
constexpr u32 kRed = 0xFFFF0000u;         // colorTable[31744]
constexpr u32 kBlue = 0xFF0000FFu;
constexpr u32 kGrey = 0xFFA5A5A5u;        // colorTable[21140]
constexpr u32 kDarkGrey = 0xFF7B7B7Bu;    // colorTable[15855]
constexpr u32 kWhite = 0xFFFFFFFFu;
constexpr u32 kBlack = 0xFF000000u;
constexpr u32 kAmber = 0xFFFFC000u;

enum Sound : int { SND_NONE = 0, SND_SMALL = 1, SND_MEDIUM = 2, SND_LARGE = 4 };

struct Pass {
    explicit Pass(bool draw);
    ~Pass();
    bool prev;
};
bool drawing();

// Layout origin inside the UI canvas (game px) and canvas texel scale.
float originX();
float originY();
float texScale();
float mouseX();
float mouseY();
bool mouseIn(float x, float y, float w, float h);
// True once a widget took the mouse this frame (screens should not treat the click as a world click).
bool mouseTaken();
void takeMouse();

Sprite* art(const std::string& name); // art/intrface/<name>
int artW(const std::string& name, int frame = 0);
int artH(const std::string& name, int frame = 0);

// Drawing (only in the draw pass)
void draw(const std::string& name, float x, float y, int frame = 0, const DrawParams& p = DrawParams());
void drawSprite(Sprite* s, int dir, int frame, float x, float y, const DrawParams& p = DrawParams());
void drawSpriteScaled(Sprite* s, int dir, int frame, float x, float y, float w, float h, const DrawParams& p = DrawParams());
// Draws the art region (sx, sy, sw, sh) (1x art pixels of frame 0) at (x, y).
void drawPart(const std::string& name, int sx, int sy, int sw, int sh, float x, float y, const DrawParams& p = DrawParams());
void fill(float x, float y, float w, float h, u32 argb);
void outline(float x, float y, float w, float h, u32 argb);
// Fills the canvas outside the 640x480 layout.
void letterbox(u32 argb);

enum TextFlags : u32 { T_SHADOW = 1, T_CENTER = 2, T_RIGHT = 4 };
void text(int fontId, const std::string& s, float x, float y, u32 color, u32 flags = 0, float alpha = 1.0f);
int textWidth(int fontId, const std::string& s);
int lineHeight(int fontId);
std::vector<std::string> wrap(int fontId, const std::string& s, int width);
// Draws wrapped text; returns the y below the last line drawn. Lines past maxY are skipped.
float textBox(int fontId, const std::string& s, float x, float y, int width, u32 color, float maxY = 1e9f, int skipLines = 0);

// Widgets
// Image button using interface art `up`/`down`. Returns true on click (logic pass) or hotkey.
bool button(const char* up, const char* down, float x, float y, int hotkey = 0, int sound = SND_SMALL, bool enabled = true);
// Invisible clickable region.
bool area(float x, float y, float w, float h, int hotkey = 0, int sound = SND_NONE);
// Right click / hover helpers
bool hovered(float x, float y, float w, float h);
bool rightClicked(float x, float y, float w, float h);
void playClick(int sound, bool press);

// Keyboard (logic pass)
bool key(int vk);
const std::vector<u32>& chars();

} // namespace ui
} // namespace fo
