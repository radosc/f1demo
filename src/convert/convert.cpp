#include "convert/convert.h"
#include "convert/convert_internal.h"
#include "core/dat.h"
#include "core/fs.h"
#include "core/json.h"
#include "core/png.h"
#include "gfx/frm.h"
#include "gfx/palette.h"
#include "gfx/upscale.h"

#include <windows.h>

#include <atomic>
#include <mutex>
#include <thread>

namespace fo {

// ------------------------------------------------------------------ helpers
std::string convOut(const ConvContext& c, const std::string& rel) { return pathJoin(c.opt.outDir, rel); }

bool convWrite(const ConvContext& c, const std::string& rel, const Bytes& data)
{
    return fileWrite(convOut(c, rel), data.data(), data.size());
}

bool convWriteText(const ConvContext& c, const std::string& rel, const std::string& text)
{
    return fileWrite(convOut(c, rel), text);
}

// Data files that belong to the engine rather than the demo (src/res, embedded as RCDATA resources by
// resource.rc): game rules the demo's executable holds in code, written out so that they can be modded.
static void writeEngineFiles(const ConvContext& c)
{
    static const struct {
        const char* resource;
        const char* path;
    } kFiles[] = {
        { "RULES_CRITICAL", "rules/critical.json" },
        { "RULES_PERKS", "rules/perks.json" },
    };
    for (auto& f : kFiles) {
        HRSRC res = FindResourceA(nullptr, f.resource, MAKEINTRESOURCEA(10)); // RT_RCDATA
        HGLOBAL mem = res ? LoadResource(nullptr, res) : nullptr;
        const u8* data = mem ? (const u8*)LockResource(mem) : nullptr;
        if (!data) {
            LOGE("Embedded file %s is missing from the executable", f.path);
            continue;
        }
        convWrite(c, f.path, Bytes(data, data + SizeofResource(nullptr, res)));
    }
}

// Wraps the 1x/HD image with the Fallout palette for PNG output.
static Image withPalette(Image img)
{
    img.palette.assign(g_palette.base(), g_palette.base() + 256);
    img.palette[0] = 0x00000000u;
    return img;
}

// ------------------------------------------------------------------ HD upscaling strategies
// Floor/roof tiles tile on an isometric lattice; upscale a neighbourhood so interiors stay seamless,
// then cut the exact (nearest-neighbour) diamond silhouette.
static Image upscaleTile(const Image& tile, int scale, const int luma[256])
{
    const int lat[2][2] = { { 48, -12 }, { 32, 24 } };
    int margin = 96;
    Image canvas;
    canvas.allocIndexed(tile.width + margin * 2, tile.height + margin * 2);
    for (int i = -2; i <= 2; i++) {
        for (int j = -2; j <= 2; j++) {
            int ox = margin + i * lat[0][0] + j * lat[1][0];
            int oy = margin + i * lat[0][1] + j * lat[1][1];
            for (int y = 0; y < tile.height; y++) {
                int cy = oy + y;
                if (cy < 0 || cy >= canvas.height) continue;
                for (int x = 0; x < tile.width; x++) {
                    int cx = ox + x;
                    if (cx < 0 || cx >= canvas.width) continue;
                    u8 v = tile.row(y)[x];
                    if (v) canvas.row(cy)[cx] = v;
                }
            }
        }
    }
    Image big = upscaleIndexed(canvas, scale, luma);
    Image out;
    out.allocIndexed(tile.width * scale, tile.height * scale);
    for (int y = 0; y < out.height; y++) {
        const u8* srcRow = big.row(margin * scale + y);
        const u8* maskRow = tile.row(y / scale);
        u8* d = out.row(y);
        for (int x = 0; x < out.width; x++) {
            u8 m = maskRow[x / scale];
            if (!m) continue;
            u8 v = srcRow[margin * scale + x];
            d[x] = v ? v : m;
        }
    }
    return out;
}

// MMPX colours but the original silhouette (used for walls that must join their neighbours exactly).
static Image upscaleKeepMask(const Image& img, int scale, const int luma[256])
{
    Image big = upscaleIndexed(img, scale, luma);
    for (int y = 0; y < big.height; y++) {
        const u8* maskRow = img.row(y / scale);
        u8* d = big.row(y);
        for (int x = 0; x < big.width; x++) {
            u8 m = maskRow[x / scale];
            if (!m) d[x] = 0;
            else if (!d[x]) d[x] = m;
        }
    }
    return big;
}

// ------------------------------------------------------------------ smooth (RGBA) variants
static bool usesCyclingColors(const Image& sheet)
{
    for (u8 v : sheet.pixels)
        if (v >= 229 && v <= 254) return true;
    return false;
}

// RGBA copy of an indexed image whose colours are extended outward (fully opaque) by `passes` pixels, so
// smoothing never mixes in transparency where the final silhouette is hard.
static Image opaqueExtended(const Image& indexed, int passes)
{
    Image rgba = indexedToRGBA(indexed, g_palette.base());
    bleedAlpha(rgba, passes);
    for (size_t i = 0; i < rgba.pixels.size(); i += 4) {
        const u8* p = &rgba.pixels[i];
        bool colored = p[0] | p[1] | p[2];
        if (rgba.pixels[i + 3] == 0 && colored) rgba.pixels[i + 3] = 255;
    }
    return rgba;
}

// Floor/roof tile: smooth-scale the tile surrounded by copies of itself on the isometric lattice (plus colour
// extension for irregular edges) so the interior has no seams, then cut the exact diamond silhouette.
static Image smoothTile(const Image& tile, int scale)
{
    const int lat[2][2] = { { 48, -12 }, { 32, 24 } };
    const int margin = 16;
    Image canvas;
    canvas.allocIndexed(tile.width + margin * 2, tile.height + margin * 2);
    for (int i = -1; i <= 1; i++) {
        for (int j = -1; j <= 1; j++) {
            int ox = margin + i * lat[0][0] + j * lat[1][0];
            int oy = margin + i * lat[0][1] + j * lat[1][1];
            for (int y = 0; y < tile.height; y++) {
                int cy = oy + y;
                if (cy < 0 || cy >= canvas.height) continue;
                for (int x = 0; x < tile.width; x++) {
                    int cx = ox + x;
                    if (cx < 0 || cx >= canvas.width) continue;
                    u8 v = tile.row(y)[x];
                    if (v) canvas.row(cy)[cx] = v;
                }
            }
        }
    }
    Image big = upscaleSmooth(opaqueExtended(canvas, 6), scale);
    Image out;
    out.allocRGBA(tile.width * scale, tile.height * scale);
    for (int y = 0; y < out.height; y++) {
        const u8* src = big.row(margin * scale + y) + (size_t)margin * scale * 4;
        const u8* mask = tile.row(y / scale);
        u8* d = out.row(y);
        for (int x = 0; x < out.width; x++) {
            u8 m = mask[x / scale];
            if (!m) continue;
            const u8* p = src + x * 4;
            if (p[3] == 0) {
                u32 c = g_palette.base()[m];
                d[x * 4] = (u8)(c >> 16), d[x * 4 + 1] = (u8)(c >> 8), d[x * 4 + 2] = (u8)c;
            } else {
                d[x * 4] = p[0], d[x * 4 + 1] = p[1], d[x * 4 + 2] = p[2];
            }
            d[x * 4 + 3] = 255;
        }
    }
    return out;
}

// Smooth colours with the original (nearest neighbour) silhouette: walls must join their neighbours exactly.
static Image smoothKeepMask(const Image& img, int scale)
{
    Image big = upscaleSmooth(opaqueExtended(img, 6), scale);
    for (int y = 0; y < big.height; y++) {
        const u8* mask = img.row(y / scale);
        u8* d = big.row(y);
        for (int x = 0; x < big.width; x++) d[x * 4 + 3] = mask[x / scale] ? 255 : 0;
    }
    return big;
}

static Image upscaleSheetSmooth(const Image& sheet, const SpriteMeta& meta, int scale, int mode)
{
    Image out;
    out.allocRGBA(sheet.width * scale, sheet.height * scale);
    const int pad = 3;
    for (auto& f : meta.frames) {
        if (f.w <= 0 || f.h <= 0) continue;
        Image crop;
        crop.allocIndexed(f.w + pad * 2, f.h + pad * 2);
        for (int y = 0; y < f.h; y++) memcpy(crop.row(y + pad) + pad, sheet.row(f.y + y) + f.x, (size_t)f.w);
        Image up;
        if (mode == 1) up = smoothTile(crop, scale);
        else if (mode == 2) up = smoothKeepMask(crop, scale);
        else up = upscaleSmooth(indexedToRGBA(crop, g_palette.base()), scale);
        for (int y = 0; y < f.h * scale; y++) {
            memcpy(out.row(f.y * scale + y) + (size_t)f.x * scale * 4, up.row(y + pad * scale) + (size_t)pad * scale * 4, (size_t)f.w * scale * 4);
        }
    }
    bleedAlpha(out, 2 * scale);
    return out;
}

// Upscales every frame separately (so frames never bleed into each other) and reassembles the sheet.
static Image upscaleSheet(const Image& sheet, const SpriteMeta& meta, int scale, const int luma[256], int mode)
{
    Image out;
    out.allocIndexed(sheet.width * scale, sheet.height * scale);
    const int pad = 3;
    for (auto& f : meta.frames) {
        if (f.w <= 0 || f.h <= 0) continue;
        // crop with a transparent margin so edge rules see "outside"
        Image crop;
        crop.allocIndexed(f.w + pad * 2, f.h + pad * 2);
        for (int y = 0; y < f.h; y++) memcpy(crop.row(y + pad) + pad, sheet.row(f.y + y) + f.x, (size_t)f.w);
        Image up;
        if (mode == 1) up = upscaleTile(crop, scale, luma);
        else if (mode == 2) up = upscaleKeepMask(crop, scale, luma);
        else up = upscaleIndexed(crop, scale, luma);
        for (int y = 0; y < f.h * scale; y++) {
            memcpy(out.row(f.y * scale + y) + (size_t)f.x * scale, up.row(y + pad * scale) + (size_t)pad * scale, (size_t)f.w * scale);
        }
    }
    return out;
}

// ------------------------------------------------------------------ art
static bool convertArt(ConvContext& c)
{
    struct Job {
        std::string base;      // "art/critters/hmjmpsaa"
        std::vector<const DatEntry*> parts; // single .frm or 6 directional parts (index = direction)
        bool directional = false;
    };
    std::map<std::string, Job> jobs;
    for (auto& e : c.dat.entries()) {
        if (!strStartsWith(e.path, "art/")) {
            continue;
        }
        std::string ext = pathExt(e.path);
        std::string base = e.path.substr(0, e.path.size() - ext.size());
        if (ext == ".frm") {
            Job& j = jobs[base];
            j.base = base;
            j.parts.assign(1, &e);
        } else if (ext.size() == 4 && ext[1] == 'f' && ext[2] == 'r' && ext[3] >= '0' && ext[3] <= '5') {
            Job& j = jobs[base];
            j.base = base;
            if (!j.directional && !j.parts.empty()) continue; // prefer plain .frm
            j.directional = true;
            j.parts.resize(6, nullptr);
            j.parts[ext[3] - '0'] = &e;
        } else if (ext == ".lst") {
            Bytes b;
            c.dat.read(e, b);
            convWrite(c, e.path, b);
        } else if (ext == ".mve") {
            Bytes b;
            c.dat.read(e, b);
            convWrite(c, "movies/" + pathFile(e.path), b);
        }
    }

    int luma[256];
    buildLumaTable(g_palette.base(), luma);

    std::vector<Job*> list;
    for (auto& kv : jobs) list.push_back(&kv.second);
    std::atomic<size_t> next{ 0 };
    std::atomic<int> done{ 0 };
    std::mutex datMutex;
    std::atomic<bool> failed{ false };
    auto worker = [&]() {
        for (;;) {
            size_t idx = next++;
            if (idx >= list.size()) break;
            Job& job = *list[idx];
            Frm frm;
            bool ok = false;
            if (!job.directional) {
                Bytes data;
                {
                    std::lock_guard<std::mutex> lock(datMutex);
                    c.dat.read(*job.parts[0], data);
                }
                ok = frmDecode(data, frm);
            } else {
                Frm parts[6];
                Frm* ptrs[6] = {};
                for (int d = 0; d < 6; d++) {
                    if (!job.parts[d]) continue;
                    Bytes data;
                    {
                        std::lock_guard<std::mutex> lock(datMutex);
                        c.dat.read(*job.parts[d], data);
                    }
                    if (frmDecode(data, parts[d])) ptrs[d] = &parts[d];
                }
                ok = frmMergeDirectional(ptrs, frm);
            }
            if (!ok) {
                LOGW("Failed to decode %s", job.base.c_str());
                done++;
                continue;
            }
            Image sheet;
            SpriteMeta meta;
            frmToSheet(frm, sheet, meta, 4);
            std::string cat = pathFile(pathDir(job.base));
            std::string name = pathFile(job.base);
            std::string dir = "art/" + cat;
            convWrite(c, dir + "/" + name + ".png", pngEncode(withPalette(sheet), 9));
            convWriteText(c, dir + "/" + name + ".json", meta.toJson().dump(-1));
            if (c.opt.hdScale >= 2) {
                int mode = 0;
                if (cat == "tiles") mode = 1;
                else if (cat == "walls") mode = 2;
                // floor/roof tiles are noise textures: smoothing makes them painterly and turns their dark
                // edge pixels into visible seams, so they keep the crisp palette-preserving scaler
                if (c.opt.hdStyle == 1 && mode != 1 && !usesCyclingColors(sheet)) {
                    Image hd = upscaleSheetSmooth(sheet, meta, c.opt.hdScale, mode);
                    convWrite(c, dir + "/hd/" + name + ".png", pngEncode(hd, 6));
                } else {
                    Image hd = upscaleSheet(sheet, meta, c.opt.hdScale, luma, mode);
                    convWrite(c, dir + "/hd/" + name + ".png", pngEncode(withPalette(hd), 6));
                }
            }
            int n = ++done;
            if (c.opt.onProgress && (n % 16) == 0) c.opt.onProgress("art", (float)n / (float)list.size());
        }
    };
    unsigned threads = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < threads; t++) pool.emplace_back(worker);
    for (auto& t : pool) t.join();
    LOGI("Converted %d sprites", (int)list.size());
    return !failed;
}

// ------------------------------------------------------------------ sound
static Bytes makeWav(const u8* pcm, size_t size, int rate, int bits, int channels)
{
    Bytes w(44 + size);
    memcpy(&w[0], "RIFF", 4);
    wrLE32(&w[4], (u32)(36 + size));
    memcpy(&w[8], "WAVEfmt ", 8);
    wrLE32(&w[16], 16);
    wrLE16(&w[20], 1);
    wrLE16(&w[22], (u16)channels);
    wrLE32(&w[24], (u32)rate);
    wrLE32(&w[28], (u32)(rate * channels * bits / 8));
    wrLE16(&w[32], (u16)(channels * bits / 8));
    wrLE16(&w[34], (u16)bits);
    memcpy(&w[36], "data", 4);
    wrLE32(&w[40], (u32)size);
    if (size) memcpy(&w[44], pcm, size);
    return w;
}

static bool convertSound(ConvContext& c)
{
    for (auto& e : c.dat.entries()) {
        if (!strStartsWith(e.path, "sound/")) continue;
        std::string ext = pathExt(e.path);
        Bytes b;
        if (!c.dat.read(e, b)) continue;
        std::string stem = pathStem(e.path);
        if (ext == ".snd") {
            // raw 8-bit unsigned mono PCM @ 22050 Hz
            convWrite(c, "sound/sfx/" + stem + ".wav", makeWav(b.data(), b.size(), 22050, 8, 1));
        } else if (ext == ".mus") {
            // raw 16-bit signed little-endian mono PCM @ 22050 Hz
            convWrite(c, "sound/music/" + stem + ".wav", makeWav(b.data(), b.size() & ~(size_t)1, 22050, 16, 1));
        } else {
            convWrite(c, e.path, b);
        }
    }
    return true;
}

// ------------------------------------------------------------------ verbatim copies
static bool copyPrefix(ConvContext& c, const std::string& prefix, const std::string& destPrefix)
{
    for (auto& e : c.dat.entries()) {
        if (!strStartsWith(e.path, prefix)) continue;
        Bytes b;
        if (!c.dat.read(e, b)) continue;
        convWrite(c, destPrefix + e.path.substr(prefix.size()), b);
    }
    return true;
}

bool convertGameData(const ConvertOptions& opt)
{
    ConvContext c{ opt };
    if (!c.dat.open(opt.datPath)) return false;
    Bytes pal;
    if (!c.dat.read("color.pal", pal) || !g_palette.load(pal)) {
        LOGE("COLOR.PAL missing from DAT");
        return false;
    }
    dirCreate(opt.outDir);
    convWrite(c, "color.pal", pal);
    auto progress = [&](const char* stage, float p) {
        if (opt.onProgress) opt.onProgress(stage, p);
        LOGI("Convert: %s %.0f%%", stage, p * 100.0f);
    };

    if (opt.text) {
        progress("text", 0);
        copyPrefix(c, "text/", "text/english/");
    }
    if (opt.scripts) {
        progress("scripts", 0);
        copyPrefix(c, "scripts/", "scripts/");
    }
    if (opt.misc) {
        progress("misc", 0);
        // root fonts, data tables, premade characters, self-run demos
        for (auto& e : c.dat.entries()) {
            if (e.path.find('/') != std::string::npos) continue;
            std::string ext = pathExt(e.path);
            if (ext == ".fon" || ext == ".aaf") {
                Bytes b;
                c.dat.read(e, b);
                convWrite(c, "fonts/" + e.path, b);
            }
        }
        copyPrefix(c, "data/", "data/");
        copyPrefix(c, "selfrun/", "selfrun/");
        convertPremade(c);
        convertFonts(c);
        writeEngineFiles(c);
    }
    if (opt.protos) {
        progress("protos", 0);
        if (!convertProtos(c)) return false;
    }
    if (opt.maps) {
        progress("maps", 0);
        if (!convertMaps(c)) return false;
    }
    if (opt.sound) {
        progress("sound", 0);
        convertSound(c);
    }
    if (opt.art) {
        progress("art", 0);
        convertArt(c);
    }
    Json manifest = Json::object();
    manifest.set("format", "fo1hd-data");
    manifest.set("version", 1);
    manifest.set("source", pathFile(opt.datPath));
    manifest.set("hdScale", opt.hdScale);
    convWriteText(c, "manifest.json", manifest.dump());
    progress("done", 1);
    return true;
}

} // namespace fo
