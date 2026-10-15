// Premade characters (.GCD) and bitmap fonts (.FON / .AAF) -> JSON + PNG.
#include "convert/convert_internal.h"
#include "core/fs.h"
#include "core/json.h"
#include "core/png.h"
#include "gfx/upscale.h"

namespace fo {

bool convertPremade(ConvContext& c)
{
    for (auto& e : c.dat.entries()) {
        if (!strStartsWith(e.path, "premade/")) continue;
        Bytes b;
        if (!c.dat.read(e, b)) continue;
        std::string stem = pathStem(e.path);
        if (pathExt(e.path) == ".bio") {
            convWrite(c, "premade/" + stem + ".bio", b);
            continue;
        }
        if (pathExt(e.path) != ".gcd") continue;
        BeReader r(b);
        Json j = Json::object();
        j.set("flags", r.i32v());
        int base[STAT_SAVEABLE_COUNT], bonus[STAT_SAVEABLE_COUNT], skills[SKILL_COUNT];
        for (int& v : base) v = r.i32v();
        for (int& v : bonus) v = r.i32v();
        for (int& v : skills) v = r.i32v();
        int body = r.i32v();
        bool release = b.size() >= 428; // release format adds experience + kill type + character points
        if (release) {
            r.i32v();
            r.i32v();
        }
        std::string name = r.str(32);
        int tagCount = release ? 4 : 3;
        Json tags = Json::array();
        for (int i = 0; i < tagCount; i++) {
            int t = r.i32v();
            if (t >= 0 && t < SKILL_COUNT) tags.push(kSkillNames[t]);
        }
        Json traits = Json::array();
        for (int i = 0; i < 2; i++) {
            int t = r.i32v();
            if (t >= 0 && t < TRAIT_COUNT) traits.push(kTraitNames[t]);
        }
        Json out = Json::object();
        out.set("name", name == "None" ? std::string() : name);
        out.set("bio", stem + ".bio");
        Json st = Json::object();
        for (int i = 0; i < STAT_SAVEABLE_COUNT; i++) st.set(kStatNames[i], base[i]);
        out.set("stats", st);
        Json bs = Json::object();
        for (int i = 0; i < STAT_SAVEABLE_COUNT; i++)
            if (bonus[i]) bs.set(kStatNames[i], bonus[i]);
        out.set("bonusStats", bs);
        Json sk = Json::object();
        for (int i = 0; i < SKILL_COUNT; i++) sk.set(kSkillNames[i], skills[i]);
        out.set("skills", sk);
        out.set("tagSkills", tags);
        out.set("traits", traits);
        out.set("bodyType", body);
        convWriteText(c, "premade/" + stem + ".json", out.dump());
    }
    return true;
}

// Coverage levels: FON glyphs are binary (255), AAF glyphs use 0..9 intensity levels.
static Image fontPaletteImage(Image img, bool aaf)
{
    img.palette.assign(256, 0x00FFFFFFu);
    for (int i = 1; i < 256; i++) {
        int a = aaf ? std::min(255, i * 255 / 9) : 255;
        img.palette[i] = ((u32)a << 24) | 0x00FFFFFFu;
    }
    return img;
}

static void writeFont(ConvContext& c, const std::string& stem, bool aaf, int height, int spacing, int wordSpacing, int lineSpacing,
    const std::vector<std::array<int, 3>>& glyphInfo, const std::vector<Image>& glyphImages)
{
    // pack glyphs into rows of 16
    const int pad = 3;
    int cellW = 1, cellH = height;
    for (auto& g : glyphImages) {
        cellW = std::max(cellW, g.width);
        cellH = std::max(cellH, g.height);
    }
    int cols = 16, rows = ((int)glyphImages.size() + cols - 1) / cols;
    Image atlas;
    atlas.allocIndexed(cols * (cellW + pad) + pad, rows * (cellH + pad) + pad);
    Json glyphs = Json::array();
    for (size_t i = 0; i < glyphImages.size(); i++) {
        int gx = pad + (int)(i % cols) * (cellW + pad);
        int gy = pad + (int)(i / cols) * (cellH + pad);
        const Image& g = glyphImages[i];
        for (int y = 0; y < g.height; y++) memcpy(atlas.row(gy + y) + gx, g.row(y), (size_t)g.width);
        Json gj = Json::array();
        gj.push(gx);
        gj.push(gy);
        gj.push(glyphInfo[i][0]); // advance width
        gj.push(g.height);
        gj.push(glyphInfo[i][1]); // y offset from line top
        glyphs.push(gj);
    }
    Json j = Json::object();
    j.set("type", aaf ? "aaf" : "fon");
    j.set("height", height);
    j.set("spacing", spacing);
    j.set("wordSpacing", wordSpacing);
    j.set("lineSpacing", lineSpacing);
    Json sz = Json::array();
    sz.push(atlas.width);
    sz.push(atlas.height);
    j.set("sheetSize", sz);
    j.set("glyphs", glyphs);
    convWriteText(c, "fonts/" + stem + ".json", j.dump(-1));
    convWrite(c, "fonts/" + stem + ".png", pngEncode(fontPaletteImage(atlas, aaf), 9));
    if (c.opt.hdScale >= 2) {
        int luma[256];
        for (int i = 0; i < 256; i++) luma[i] = 2000 - i * 100;
        luma[0] = 100000;
        // upscale each glyph cell separately to avoid bleeding
        Image hd;
        hd.allocIndexed(atlas.width * c.opt.hdScale, atlas.height * c.opt.hdScale);
        int s = c.opt.hdScale;
        for (size_t i = 0; i < glyphImages.size(); i++) {
            const Image& g = glyphImages[i];
            if (g.width <= 0 || g.height <= 0) continue;
            Image crop;
            crop.allocIndexed(g.width + 4, g.height + 4);
            for (int y = 0; y < g.height; y++) memcpy(crop.row(y + 2) + 2, g.row(y), (size_t)g.width);
            Image up = upscaleIndexed(crop, s, luma);
            int gx = pad + (int)(i % cols) * (cellW + pad);
            int gy = pad + (int)(i / cols) * (cellH + pad);
            for (int y = 0; y < g.height * s; y++) memcpy(hd.row(gy * s + y) + (size_t)gx * s, up.row(y + 2 * s) + 2 * s, (size_t)g.width * s);
        }
        convWrite(c, "fonts/hd/" + stem + ".png", pngEncode(fontPaletteImage(hd, aaf), 9));
    }
}

bool convertFonts(ConvContext& c)
{
    for (auto& e : c.dat.entries()) {
        if (e.path.find('/') != std::string::npos) continue;
        std::string ext = pathExt(e.path);
        Bytes b;
        if (ext == ".fon") {
            if (!c.dat.read(e, b) || b.size() < 20) continue;
            int num = (int)rdLE32(&b[0]);
            int height = (int)rdLE32(&b[4]);
            int spacing = (int)rdLE32(&b[8]);
            size_t infoPos = 20;
            size_t dataPos = infoPos + (size_t)num * 8;
            if (num <= 0 || num > 256 || dataPos > b.size()) continue;
            std::vector<std::array<int, 3>> info;
            std::vector<Image> imgs;
            for (int i = 0; i < 256; i++) {
                Image g;
                if (i >= num) {
                    g.allocIndexed(0, height);
                    info.push_back({ 0, 0, 0 });
                    imgs.push_back(g);
                    continue;
                }
                int w = (int)rdLE32(&b[infoPos + i * 8]);
                int off = (int)rdLE32(&b[infoPos + i * 8 + 4]);
                g.allocIndexed(w, height);
                int stride = (w + 7) >> 3;
                for (int y = 0; y < height; y++) {
                    for (int x = 0; x < w; x++) {
                        size_t p = dataPos + (size_t)off + (size_t)y * stride + (x >> 3);
                        if (p < b.size() && (b[p] & (0x80 >> (x & 7)))) g.row(y)[x] = 255;
                    }
                }
                info.push_back({ w, 0, 0 });
                imgs.push_back(g);
            }
            // FON fonts use the width of ' ' as word spacing
            int wordSpacing = num > ' ' ? info[' '][0] : height / 2;
            writeFont(c, pathStem(e.path) + "_fon", false, height, spacing, wordSpacing, 0, info, imgs);
        } else if (ext == ".aaf") {
            if (!c.dat.read(e, b) || b.size() < 2060 || memcmp(b.data(), "AAFF", 4) != 0) continue;
            int maxH = rdBE16s(&b[4]);
            int letterSp = rdBE16s(&b[6]);
            int wordSp = rdBE16s(&b[8]);
            int lineSp = rdBE16s(&b[10]);
            std::vector<std::array<int, 3>> info;
            std::vector<Image> imgs;
            for (int i = 0; i < 256; i++) {
                const u8* g = &b[12 + i * 8];
                int w = rdBE16s(g), h = rdBE16s(g + 2);
                u32 off = rdBE32(g + 4);
                Image img;
                img.allocIndexed(std::max(0, w), std::max(0, h));
                for (int y = 0; y < h; y++)
                    for (int x = 0; x < w; x++) {
                        size_t p = 2060 + off + (size_t)y * w + x;
                        img.row(y)[x] = p < b.size() ? std::min<u8>(b[p], 9) : 0;
                    }
                info.push_back({ i == ' ' ? wordSp : w, maxH - h, 0 });
                imgs.push_back(img);
            }
            writeFont(c, pathStem(e.path) + "_aaf", true, maxH, letterSp, wordSp, lineSp, info, imgs);
        }
    }
    return true;
}

} // namespace fo
