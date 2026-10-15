#include "core/png.h"
#include "core/deflate.h"

namespace fo {

static const u8 kPngSig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };

static int paeth(int a, int b, int c)
{
    int p = a + b - c;
    int pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

// Reverses filtering in place for one (sub)image of `h` rows with `stride` bytes and `bpp` bytes per pixel.
static bool unfilter(u8* data, size_t avail, int h, size_t stride, int bpp, std::vector<u8>& out)
{
    out.assign(stride * (size_t)h, 0);
    if ((stride + 1) * (size_t)h > avail) return false;
    const u8* src = data;
    std::vector<u8> zero(stride, 0);
    for (int y = 0; y < h; y++) {
        u8 ft = *src++;
        u8* cur = out.data() + (size_t)y * stride;
        const u8* prev = y ? out.data() + (size_t)(y - 1) * stride : zero.data();
        for (size_t x = 0; x < stride; x++) {
            int a = x >= (size_t)bpp ? cur[x - bpp] : 0;
            int b = prev[x];
            int c = x >= (size_t)bpp ? prev[x - bpp] : 0;
            int v = src[x];
            switch (ft) {
            case 0: break;
            case 1: v += a; break;
            case 2: v += b; break;
            case 3: v += (a + b) >> 1; break;
            case 4: v += paeth(a, b, c); break;
            default: return false;
            }
            cur[x] = (u8)v;
        }
        src += stride;
    }
    return true;
}

bool pngDecode(const u8* data, size_t size, Image& out, std::string* error)
{
    auto fail = [&](const char* msg) {
        if (error) *error = msg;
        return false;
    };
    if (size < 8 || memcmp(data, kPngSig, 8) != 0) return fail("not a PNG");
    size_t pos = 8;
    int w = 0, h = 0, depth = 0, ctype = 0, interlace = 0;
    std::vector<u32> pal;
    std::vector<u8> trns;
    Bytes idat;
    bool haveHeader = false;
    while (pos + 12 <= size) {
        u32 len = rdBE32(data + pos);
        const u8* type = data + pos + 4;
        const u8* body = data + pos + 8;
        if (pos + 12 + (size_t)len > size) return fail("truncated chunk");
        if (!memcmp(type, "IHDR", 4) && len >= 13) {
            w = (int)rdBE32(body);
            h = (int)rdBE32(body + 4);
            depth = body[8];
            ctype = body[9];
            interlace = body[12];
            haveHeader = true;
        } else if (!memcmp(type, "PLTE", 4)) {
            pal.clear();
            for (u32 i = 0; i + 2 < len; i += 3) pal.push_back(0xFF000000u | (body[i] << 16) | (body[i + 1] << 8) | body[i + 2]);
        } else if (!memcmp(type, "tRNS", 4)) {
            trns.assign(body, body + len);
        } else if (!memcmp(type, "IDAT", 4)) {
            idat.insert(idat.end(), body, body + len);
        } else if (!memcmp(type, "IEND", 4)) {
            break;
        }
        pos += 12 + (size_t)len;
    }
    if (!haveHeader || w <= 0 || h <= 0 || w > 32768 || h > 32768) return fail("bad header");
    int channels;
    switch (ctype) {
    case 0: channels = 1; break;
    case 2: channels = 3; break;
    case 3: channels = 1; break;
    case 4: channels = 2; break;
    case 6: channels = 4; break;
    default: return fail("unsupported color type");
    }
    if (ctype == 3 && pal.empty()) return fail("missing palette");
    int bitsPerPixel = channels * depth;
    int bpp = std::max(1, bitsPerPixel / 8);

    Bytes raw;
    if (!zlibInflate(idat.data(), idat.size(), raw)) return fail("bad zlib data");

    // Decode into a "samples" image: one row per y with packed samples as stored.
    auto sampleAt = [&](const u8* row, int x, int ch) -> int {
        if (depth == 8) return row[x * channels + ch];
        if (depth == 16) return row[(x * channels + ch) * 2]; // keep high byte
        int bitIndex = x * depth;
        int byte = row[bitIndex >> 3];
        int shift = 8 - depth - (bitIndex & 7);
        return (byte >> shift) & ((1 << depth) - 1);
    };

    bool keepIndexed = ctype == 3;
    out.width = w;
    out.height = h;
    out.indexed = keepIndexed;
    out.pixels.assign((size_t)w * h * (keepIndexed ? 1 : 4), 0);
    if (keepIndexed) {
        out.palette.assign(256, 0xFF000000u);
        for (size_t i = 0; i < pal.size() && i < 256; i++) out.palette[i] = pal[i];
        for (size_t i = 0; i < trns.size() && i < 256; i++) out.palette[i] = (out.palette[i] & 0x00FFFFFFu) | ((u32)trns[i] << 24);
    } else {
        out.palette.clear();
    }

    auto storePixel = [&](int x, int y, const u8* row, int sx) {
        if (keepIndexed) {
            out.pixels[(size_t)y * w + x] = (u8)sampleAt(row, sx, 0);
            return;
        }
        u8* d = &out.pixels[((size_t)y * w + x) * 4];
        int scale = depth < 8 ? 255 / ((1 << depth) - 1) : 1;
        switch (ctype) {
        case 0: {
            int g = sampleAt(row, sx, 0);
            int a = 255;
            if (trns.size() >= 2) {
                int tv = depth == 16 ? trns[0] : rdBE16(trns.data());
                if (g == tv) a = 0;
            }
            g *= scale;
            d[0] = d[1] = d[2] = (u8)g;
            d[3] = (u8)a;
            break;
        }
        case 2: {
            int r = sampleAt(row, sx, 0), g = sampleAt(row, sx, 1), b = sampleAt(row, sx, 2);
            int a = 255;
            if (trns.size() >= 6 && depth == 8 && r == rdBE16(trns.data()) && g == rdBE16(trns.data() + 2) && b == rdBE16(trns.data() + 4)) a = 0;
            d[0] = (u8)r; d[1] = (u8)g; d[2] = (u8)b; d[3] = (u8)a;
            break;
        }
        case 4: {
            int g = sampleAt(row, sx, 0) * scale;
            d[0] = d[1] = d[2] = (u8)g;
            d[3] = (u8)sampleAt(row, sx, 1);
            break;
        }
        case 6:
            d[0] = (u8)sampleAt(row, sx, 0);
            d[1] = (u8)sampleAt(row, sx, 1);
            d[2] = (u8)sampleAt(row, sx, 2);
            d[3] = (u8)sampleAt(row, sx, 3);
            break;
        }
    };

    std::vector<u8> plane;
    if (!interlace) {
        size_t stride = ((size_t)w * bitsPerPixel + 7) / 8;
        if (!unfilter(raw.data(), raw.size(), h, stride, bpp, plane)) return fail("bad filter data");
        for (int y = 0; y < h; y++) {
            const u8* row = plane.data() + (size_t)y * stride;
            for (int x = 0; x < w; x++) storePixel(x, y, row, x);
        }
    } else {
        static const int sx0[7] = { 0, 4, 0, 2, 0, 1, 0 };
        static const int sy0[7] = { 0, 0, 4, 0, 2, 0, 1 };
        static const int dx[7] = { 8, 8, 4, 4, 2, 2, 1 };
        static const int dy[7] = { 8, 8, 8, 4, 4, 2, 2 };
        size_t offset = 0;
        for (int pass = 0; pass < 7; pass++) {
            int pw = (w - sx0[pass] + dx[pass] - 1) / dx[pass];
            int ph = (h - sy0[pass] + dy[pass] - 1) / dy[pass];
            if (pw <= 0 || ph <= 0) continue;
            size_t stride = ((size_t)pw * bitsPerPixel + 7) / 8;
            if (offset > raw.size()) return fail("truncated interlace");
            if (!unfilter(raw.data() + offset, raw.size() - offset, ph, stride, bpp, plane)) return fail("bad filter data");
            offset += (stride + 1) * (size_t)ph;
            for (int y = 0; y < ph; y++) {
                const u8* row = plane.data() + (size_t)y * stride;
                for (int x = 0; x < pw; x++) storePixel(sx0[pass] + x * dx[pass], sy0[pass] + y * dy[pass], row, x);
            }
        }
    }
    return true;
}

static void writeChunk(Bytes& out, const char* type, const u8* body, size_t len)
{
    u8 hdr[8];
    wrBE32(hdr, (u32)len);
    memcpy(hdr + 4, type, 4);
    out.insert(out.end(), hdr, hdr + 8);
    if (len) out.insert(out.end(), body, body + len);
    u32 crc = crc32Update(0, (const u8*)type, 4);
    crc = crc32Update(crc, body, len);
    u8 c[4];
    wrBE32(c, crc);
    out.insert(out.end(), c, c + 4);
}

Bytes pngEncode(const Image& img, int level)
{
    Bytes out(kPngSig, kPngSig + 8);
    u8 ihdr[13];
    wrBE32(ihdr, (u32)img.width);
    wrBE32(ihdr + 4, (u32)img.height);
    ihdr[8] = 8;
    ihdr[9] = img.indexed ? 3 : 6;
    ihdr[10] = 0;
    ihdr[11] = 0;
    ihdr[12] = 0;
    writeChunk(out, "IHDR", ihdr, 13);

    int bpp = img.indexed ? 1 : 4;
    size_t stride = (size_t)img.width * bpp;
    if (img.indexed) {
        int n = (int)std::min<size_t>(256, std::max<size_t>(1, img.palette.size()));
        std::vector<u8> plte((size_t)n * 3);
        std::vector<u8> trns((size_t)n);
        int lastTrns = -1;
        for (int i = 0; i < n; i++) {
            u32 c = i < (int)img.palette.size() ? img.palette[i] : 0xFF000000u;
            plte[i * 3] = (u8)(c >> 16);
            plte[i * 3 + 1] = (u8)(c >> 8);
            plte[i * 3 + 2] = (u8)c;
            trns[i] = (u8)(c >> 24);
            if (trns[i] != 255) lastTrns = i;
        }
        writeChunk(out, "PLTE", plte.data(), plte.size());
        if (lastTrns >= 0) writeChunk(out, "tRNS", trns.data(), (size_t)lastTrns + 1);
    }

    // filter rows: indexed -> none; RGBA -> pick best by min sum of abs
    std::vector<u8> filtered;
    filtered.reserve((stride + 1) * (size_t)img.height);
    std::vector<u8> cand[5];
    for (auto& c : cand) c.resize(stride);
    for (int y = 0; y < img.height; y++) {
        const u8* cur = img.row(y);
        const u8* prev = y ? img.row(y - 1) : nullptr;
        if (img.indexed) {
            filtered.push_back(0);
            filtered.insert(filtered.end(), cur, cur + stride);
            continue;
        }
        int best = 0;
        u64 bestScore = ~0ull;
        for (int ft = 0; ft < 5; ft++) {
            u64 score = 0;
            for (size_t x = 0; x < stride; x++) {
                int a = x >= (size_t)bpp ? cur[x - bpp] : 0;
                int b = prev ? prev[x] : 0;
                int c = (prev && x >= (size_t)bpp) ? prev[x - bpp] : 0;
                int v = cur[x];
                switch (ft) {
                case 1: v -= a; break;
                case 2: v -= b; break;
                case 3: v -= (a + b) >> 1; break;
                case 4: v -= paeth(a, b, c); break;
                }
                cand[ft][x] = (u8)v;
                score += (u64)abs((int)(i8)(u8)v);
            }
            if (score < bestScore) {
                bestScore = score;
                best = ft;
            }
        }
        filtered.push_back((u8)best);
        filtered.insert(filtered.end(), cand[best].begin(), cand[best].end());
    }
    Bytes z = zlibDeflate(filtered.data(), filtered.size(), level);
    writeChunk(out, "IDAT", z.data(), z.size());
    writeChunk(out, "IEND", nullptr, 0);
    return out;
}

} // namespace fo
