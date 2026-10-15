// Sprite sheets loaded from data/art (PNG + JSON), with optional HD textures.
#pragma once
#include "gfx/frm.h"
#include "gfx/renderer.h"

namespace fo {

struct Sprite {
    std::string path;      // virtual path without extension: "art/critters/hmjmpsaa"
    SpriteMeta meta;
    Image image;           // 1x indexed sheet kept for pixel hit testing
    TexturePtr tex1x;
    TexturePtr texHD;
    int hdScale = 0;       // HD texels per 1x pixel (0 = none)
    bool hdLoaded = false;
    u64 lastUse = 0;

    int frameCount() const { return meta.framesPerDir; }
    const SheetFrame& frame(int dir, int f) const { return meta.frame(dir, f); }
    // true if the 1x pixel at (x, y) inside the frame is opaque
    bool opaqueAt(int dir, int f, int x, int y) const;
};

class SpriteCache {
public:
    // Returns nullptr if the art does not exist (cached negative results).
    Sprite* get(const std::string& vpath);
    // Picks the texture for drawing at `canvasScale` texels per game pixel.
    // `outScale` receives that texture's texels per game pixel.
    TexturePtr texture(Sprite* s, int canvasScale, int& outScale);
    void setUseHD(bool on) { m_useHD = on; }
    bool useHD() const { return m_useHD; }
    void beginFrame() { m_frame++; }
    void trim(size_t maxBytes); // drop least recently used GPU textures above budget
    void clear();
    size_t gpuBytes() const;

private:
    std::unordered_map<std::string, std::unique_ptr<Sprite>> m_sprites;
    std::set<std::string> m_missing;
    bool m_useHD = true;
    u64 m_frame = 0;
};

extern SpriteCache g_sprites;

// Loads a PNG through the VFS.
bool loadPng(const std::string& vpath, Image& out);

} // namespace fo
