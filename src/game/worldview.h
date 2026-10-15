// Renders the isometric world (floor, objects, roofs, overlays) into the world canvas and handles picking.
#pragma once
#include "game/object.h"
#include "gfx/renderer.h"

namespace fo {

struct FloatingText {
    u32 objUid = 0;
    std::vector<std::string> lines;
    u32 color = 0xFFFFFFFFu;
    float timeLeft = 0;
    int fontId = 101;
};

class WorldView {
public:
    // camera center in world pixels
    float camX = 0, camY = 0;
    // canvas size in game pixels and texel scale (1, 2 or 4)
    int canvasW = 640, canvasH = 380;
    int texScale = 1;
    bool showRoofs = true;
    bool showHexGrid = false;
    bool eggEnabled = true;
    bool markSeen = true;         // flag drawn walls as seen (automap); off in the map editor
    int hoverHex = -1;            // hex under the mouse (for the hex cursor)
    u32 hoverHexColor = 0;        // 0 = hidden
    std::vector<int> pathPreview; // tiles to highlight
    std::vector<FloatingText> floats;

    void centerOn(int tile);
    void centerOnWorld(float x, float y) { camX = x; camY = y; }
    void scroll(float dx, float dy);
    void clampCamera();

    // world <-> canvas (game pixel) coordinates
    float toCanvasX(float wx) const { return wx - camX + canvasW * 0.5f; }
    float toCanvasY(float wy) const { return wy - camY + canvasH * 0.5f; }
    float toWorldX(float cx) const { return cx + camX - canvasW * 0.5f; }
    float toWorldY(float cy) const { return cy + camY - canvasH * 0.5f; }

    // Draws into the currently bound canvas (renderer.beginCanvas already called).
    void draw(float dt);
    // Topmost object under the canvas point (pixel accurate). `filter` limits types (bitmask 1<<type).
    Object* pick(float canvasX, float canvasY, u32 typeMask = 0xFFFFFFFFu, bool includeDude = true) const;
    int hexAt(float canvasX, float canvasY) const;
    // Screen-space bounding box (canvas pixels) of an object's current frame; false if no art.
    bool objectRect(const Object* o, float& x, float& y, float& w, float& h) const;
    void addFloatText(Object* obj, const std::string& text, u32 color, int fontId = 101);

    float lightAt(int tile, int elevation) const;

private:
    void drawFloor(int elevation);
    void drawObjects(int elevation);
    void drawRoofs(int elevation);
    void drawOverlays(int elevation, float dt);
    void drawObject(Object* o, float light);
    bool eggCovers(const Object* o) const;
    mutable std::vector<Object*> m_drawList;
};

extern WorldView g_view;

} // namespace fo
