#include "gfx/sprite.h"
#include "gfx/palette.h"
#include "core/fs.h"
#include "core/json.h"

namespace fo {

SpriteCache g_sprites;

bool loadPng(const std::string& vpath, Image& out)
{
    Bytes b;
    if (!g_vfs.read(vpath, b)) return false;
    std::string err;
    if (!pngDecode(b.data(), b.size(), out, &err)) {
        LOGE("PNG %s: %s", vpath.c_str(), err.c_str());
        return false;
    }
    return true;
}

// Indexed images whose palette differs from the game palette are converted to RGBA so mods can
// ship art made with any palette.
static void normalizeImage(Image& img)
{
    if (!img.indexed) return;
    const u32* base = g_palette.base();
    bool same = img.palette.size() >= 256;
    for (int i = 1; same && i < 256; i++) {
        if ((img.palette[i] & 0x00FFFFFFu) != (base[i] & 0x00FFFFFFu)) same = false;
    }
    if (same) return;
    Image rgba;
    rgba.allocRGBA(img.width, img.height);
    for (size_t i = 0; i < img.pixels.size(); i++) {
        u8 idx = img.pixels[i];
        u32 c = idx < img.palette.size() ? img.palette[idx] : 0;
        u8* p = &rgba.pixels[i * 4];
        p[0] = (u8)(c >> 16);
        p[1] = (u8)(c >> 8);
        p[2] = (u8)c;
        p[3] = idx == 0 && (c >> 24) == 255 ? 0 : (u8)(c >> 24); // index 0 stays transparent
    }
    img = std::move(rgba);
}

bool Sprite::opaqueAt(int dir, int f, int x, int y) const
{
    const SheetFrame& fr = meta.frame(dir, f);
    if (x < 0 || y < 0 || x >= fr.w || y >= fr.h) return false;
    if (image.pixels.empty()) return true;
    int px = fr.x + x, py = fr.y + y;
    if (px >= image.width || py >= image.height) return false;
    if (image.indexed) return image.row(py)[px] != 0;
    return image.row(py)[px * 4 + 3] > 16;
}

Sprite* SpriteCache::get(const std::string& vpathIn)
{
    std::string vpath = pathNormalize(vpathIn);
    auto it = m_sprites.find(vpath);
    if (it != m_sprites.end()) {
        it->second->lastUse = m_frame;
        return it->second.get();
    }
    if (m_missing.count(vpath)) return nullptr;
    std::string text;
    if (!g_vfs.readText(vpath + ".json", text)) {
        m_missing.insert(vpath);
        return nullptr;
    }
    auto s = std::make_unique<Sprite>();
    s->path = vpath;
    std::string err;
    Json j = Json::parse(text, &err);
    if (!err.empty() || !s->meta.fromJson(j)) {
        LOGE("Bad sprite metadata %s.json %s", vpath.c_str(), err.c_str());
        m_missing.insert(vpath);
        return nullptr;
    }
    if (!loadPng(vpath + ".png", s->image)) {
        LOGE("Missing sprite image %s.png", vpath.c_str());
        m_missing.insert(vpath);
        return nullptr;
    }
    normalizeImage(s->image);
    s->lastUse = m_frame;
    Sprite* raw = s.get();
    m_sprites[vpath] = std::move(s);
    return raw;
}

TexturePtr SpriteCache::texture(Sprite* s, int canvasScale, int& outScale)
{
    if (!s) return nullptr;
    s->lastUse = m_frame;
    if (m_useHD && canvasScale > 1) {
        if (!s->hdLoaded) {
            s->hdLoaded = true;
            std::string dir = pathDir(s->path), name = pathFile(s->path);
            Image hd;
            if (loadPng(dir + "/hd/" + name + ".png", hd) && s->meta.sheetW > 0) {
                int scale = hd.width / s->meta.sheetW;
                if (scale >= 2 && hd.width == s->meta.sheetW * scale && hd.height == s->meta.sheetH * scale) {
                    normalizeImage(hd);
                    s->texHD = g_renderer.createTexture(hd);
                    s->hdScale = scale;
                } else {
                    LOGW("HD sprite %s has size %dx%d which is not a multiple of %dx%d", s->path.c_str(), hd.width, hd.height,
                        s->meta.sheetW, s->meta.sheetH);
                }
            }
        }
        if (s->texHD) {
            outScale = s->hdScale;
            return s->texHD;
        }
    }
    if (!s->tex1x) s->tex1x = g_renderer.createTexture(s->image);
    outScale = 1;
    return s->tex1x;
}

size_t SpriteCache::gpuBytes() const
{
    size_t n = 0;
    for (auto& kv : m_sprites) {
        if (kv.second->tex1x) n += kv.second->tex1x->bytes;
        if (kv.second->texHD) n += kv.second->texHD->bytes;
    }
    return n;
}

void SpriteCache::trim(size_t maxBytes)
{
    size_t total = gpuBytes();
    if (total <= maxBytes) return;
    std::vector<Sprite*> list;
    for (auto& kv : m_sprites) list.push_back(kv.second.get());
    std::sort(list.begin(), list.end(), [](Sprite* a, Sprite* b) { return a->lastUse < b->lastUse; });
    for (Sprite* s : list) {
        if (total <= maxBytes * 3 / 4) break;
        if (s->lastUse + 2 >= m_frame) continue; // in use right now
        if (s->texHD) {
            total -= s->texHD->bytes;
            s->texHD.reset();
            s->hdLoaded = false;
        }
        if (s->tex1x) {
            total -= s->tex1x->bytes;
            s->tex1x.reset();
        }
    }
}

void SpriteCache::clear()
{
    m_sprites.clear();
    m_missing.clear();
}

} // namespace fo
