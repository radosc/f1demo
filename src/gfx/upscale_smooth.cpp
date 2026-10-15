// Edge-directed smooth magnification for the HD sprites.
//
// Each 2x step fills the new pixels in two passes, in the spirit of Super-xBR:
//   1. cell centres (odd, odd) from the 4x4 source neighbourhood, interpolating along whichever diagonal
//      varies least (sharp 4-tap filter), blended by the relative edge strength;
//   2. edge midpoints (odd, even) / (even, odd) with the same rule on the 45-degree rotated lattice formed by
//      the source pixels and the centres from pass 1.
// The result is clamped to the range of the four nearest known pixels (no ringing). Everything runs on
// premultiplied RGBA so silhouettes get anti-aliased edges without dark fringes.
#include "gfx/upscale.h"

namespace fo {

namespace {

struct Px {
    float r = 0, g = 0, b = 0, a = 0;
};

inline Px operator+(Px x, Px y) { return { x.r + y.r, x.g + y.g, x.b + y.b, x.a + y.a }; }
inline Px operator*(Px x, float k) { return { x.r * k, x.g * k, x.b * k, x.a * k }; }

// Feature used for edge detection: premultiplied luma plus a share of alpha (so silhouettes count as edges).
inline float feat(const Px& p) { return 0.299f * p.r + 0.587f * p.g + 0.114f * p.b + 0.35f * p.a; }

struct Grid {
    int w = 0, h = 0;
    std::vector<Px> px;
    Px at(int x, int y) const
    {
        if (x < 0 || y < 0 || x >= w || y >= h) return Px();
        return px[(size_t)y * w + x];
    }
    Px& ref(int x, int y) { return px[(size_t)y * w + x]; }
};

constexpr float kSharp = 0.129633f; // 4-tap filter -k, 0.5+k, 0.5+k, -k

// n: 4x4 neighbourhood, row-major; the new pixel lies in the middle of n[5], n[6], n[9], n[10].
Px interpolate(const Px n[16], float sharp)
{
    float f[16];
    for (int i = 0; i < 16; i++) f[i] = feat(n[i]);
    auto d = [&](int a, int b) { return fabsf(f[a] - f[b]); };
    // variation along the main diagonal (n0-n5-n10-n15) and the anti diagonal (n3-n6-n9-n12)
    float varMain = d(0, 5) + d(10, 15) + 4.0f * d(5, 10) + 2.0f * (d(1, 6) + d(6, 11) + d(4, 9) + d(9, 14)) + d(2, 7) + d(8, 13);
    float varAnti = d(3, 6) + d(9, 12) + 4.0f * d(6, 9) + 2.0f * (d(2, 5) + d(5, 8) + d(7, 10) + d(10, 13)) + d(1, 4) + d(11, 14);
    Px cMain = (n[0] + n[15]) * -sharp + (n[5] + n[10]) * (0.5f + sharp);
    Px cAnti = (n[3] + n[12]) * -sharp + (n[6] + n[9]) * (0.5f + sharp);
    float s = (varAnti - varMain) / (varAnti + varMain + 1e-4f); // > 0: smooth along the main diagonal
    float t = clampv((s + 0.2f) / 0.4f, 0.0f, 1.0f);
    t = t * t * (3.0f - 2.0f * t);
    Px c = cMain * t + cAnti * (1.0f - t);
    // anti-ringing: stay within the four nearest known pixels
    const Px& p5 = n[5];
    const Px& p6 = n[6];
    const Px& p9 = n[9];
    const Px& p10 = n[10];
    auto clampc = [](float v, float a, float b, float c2, float e) {
        float lo = std::min(std::min(a, b), std::min(c2, e)), hi = std::max(std::max(a, b), std::max(c2, e));
        return clampv(v, lo, hi);
    };
    c.r = clampc(c.r, p5.r, p6.r, p9.r, p10.r);
    c.g = clampc(c.g, p5.g, p6.g, p9.g, p10.g);
    c.b = clampc(c.b, p5.b, p6.b, p9.b, p10.b);
    c.a = clampc(c.a, p5.a, p6.a, p9.a, p10.a);
    // premultiplied colour can never exceed alpha
    c.r = std::min(c.r, c.a);
    c.g = std::min(c.g, c.a);
    c.b = std::min(c.b, c.a);
    return c;
}

Grid smooth2x(const Grid& src)
{
    Grid out;
    out.w = src.w * 2;
    out.h = src.h * 2;
    out.px.assign((size_t)out.w * out.h, Px());
    // source pixels
    for (int y = 0; y < src.h; y++)
        for (int x = 0; x < src.w; x++) out.ref(2 * x, 2 * y) = src.at(x, y);
    // pass 1: cell centres
    Px n[16];
    for (int y = 0; y < src.h; y++) {
        for (int x = 0; x < src.w; x++) {
            for (int j = 0; j < 4; j++)
                for (int i = 0; i < 4; i++) n[j * 4 + i] = src.at(x + i - 1, y + j - 1);
            out.ref(2 * x + 1, 2 * y + 1) = interpolate(n, kSharp);
        }
    }
    // pass 2: edge midpoints on the rotated lattice; offset of lattice point (i, j) is (i+j-3, i-j)
    // (all lattice neighbours have even x+y: source pixels or pass-1 centres; outside = transparent)
    auto known = [&](int x, int y) -> Px { return out.at(x, y); };
    for (int y = 0; y < out.h; y++) {
        for (int x = ((y & 1) ? 0 : 1); x < out.w; x += 2) {
            for (int j = 0; j < 4; j++)
                for (int i = 0; i < 4; i++) n[j * 4 + i] = known(x + (i + j - 3), y + (i - j));
            out.ref(x, y) = interpolate(n, kSharp * 0.8f);
        }
    }
    return out;
}

// Unsharp mask (3x3 gaussian) limited to the local 3x3 range, so it restores crispness without halos.
void sharpen(Grid& g, float amount)
{
    Grid src = g;
    static const float k[3] = { 0.25f, 0.5f, 0.25f };
    for (int y = 0; y < g.h; y++) {
        for (int x = 0; x < g.w; x++) {
            Px blur;
            Px lo{ 1e9f, 1e9f, 1e9f, 1e9f }, hi{ -1e9f, -1e9f, -1e9f, -1e9f };
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    Px p = src.at(x + dx, y + dy);
                    blur = blur + p * (k[dx + 1] * k[dy + 1]);
                    lo = { std::min(lo.r, p.r), std::min(lo.g, p.g), std::min(lo.b, p.b), std::min(lo.a, p.a) };
                    hi = { std::max(hi.r, p.r), std::max(hi.g, p.g), std::max(hi.b, p.b), std::max(hi.a, p.a) };
                }
            }
            Px c = src.at(x, y);
            Px r = c + (c + blur * -1.0f) * amount;
            r.r = clampv(r.r, lo.r, hi.r);
            r.g = clampv(r.g, lo.g, hi.g);
            r.b = clampv(r.b, lo.b, hi.b);
            r.a = c.a; // keep the silhouette's anti-aliasing as is
            r.r = std::min(r.r, r.a);
            r.g = std::min(r.g, r.a);
            r.b = std::min(r.b, r.a);
            g.ref(x, y) = r;
        }
    }
}

} // namespace

Image indexedToRGBA(const Image& src, const u32* paletteArgb)
{
    Image out;
    out.allocRGBA(src.width, src.height);
    for (size_t i = 0; i < src.pixels.size(); i++) {
        u8 idx = src.pixels[i];
        u8* p = &out.pixels[i * 4];
        if (idx == 0) continue;
        u32 c = paletteArgb[idx];
        p[0] = (u8)(c >> 16);
        p[1] = (u8)(c >> 8);
        p[2] = (u8)c;
        p[3] = 255;
    }
    return out;
}

Image upscaleSmooth(const Image& rgba, int factor)
{
    Grid g;
    g.w = rgba.width;
    g.h = rgba.height;
    g.px.resize((size_t)g.w * g.h);
    for (size_t i = 0; i < g.px.size(); i++) {
        const u8* p = &rgba.pixels[i * 4];
        float a = p[3] / 255.0f;
        g.px[i] = { p[0] / 255.0f * a, p[1] / 255.0f * a, p[2] / 255.0f * a, a };
    }
    for (int f = 1; f < factor; f *= 2) g = smooth2x(g);
    if (factor >= 2) sharpen(g, 0.65f);
    Image out;
    out.allocRGBA(g.w, g.h);
    for (size_t i = 0; i < g.px.size(); i++) {
        const Px& p = g.px[i];
        u8* d = &out.pixels[i * 4];
        float a = p.a;
        if (a < 0.03f) continue;
        float inv = 1.0f / a;
        d[0] = (u8)clampv((int)(p.r * inv * 255.0f + 0.5f), 0, 255);
        d[1] = (u8)clampv((int)(p.g * inv * 255.0f + 0.5f), 0, 255);
        d[2] = (u8)clampv((int)(p.b * inv * 255.0f + 0.5f), 0, 255);
        d[3] = (u8)clampv((int)(a * 255.0f + 0.5f), 0, 255);
    }
    return out;
}

void bleedAlpha(Image& img, int passes)
{
    if (img.indexed || img.width <= 0) return;
    const int w = img.width, h = img.height;
    std::vector<u8> filled((size_t)w * h);
    for (size_t i = 0; i < filled.size(); i++) filled[i] = img.pixels[i * 4 + 3] > 0;
    std::vector<size_t> todo;
    for (int pass = 0; pass < passes; pass++) {
        todo.clear();
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                size_t i = (size_t)y * w + x;
                if (filled[i]) continue;
                int r = 0, g = 0, b = 0, n = 0;
                for (int dy = -1; dy <= 1; dy++) {
                    for (int dx = -1; dx <= 1; dx++) {
                        int nx = x + dx, ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                        size_t j = (size_t)ny * w + nx;
                        if (!filled[j]) continue;
                        r += img.pixels[j * 4];
                        g += img.pixels[j * 4 + 1];
                        b += img.pixels[j * 4 + 2];
                        n++;
                    }
                }
                if (!n) continue;
                img.pixels[i * 4] = (u8)(r / n);
                img.pixels[i * 4 + 1] = (u8)(g / n);
                img.pixels[i * 4 + 2] = (u8)(b / n);
                todo.push_back(i);
            }
        }
        if (todo.empty()) break;
        for (size_t i : todo) filled[i] = 1;
    }
}

} // namespace fo
