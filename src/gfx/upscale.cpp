#include "gfx/upscale.h"

namespace fo {

void buildLumaTable(const u32* argb, int luma[256])
{
    for (int i = 0; i < 256; i++) {
        u32 c = argb[i];
        int r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
        luma[i] = r + g + b + 1;
    }
    luma[0] = 766 * 256; // transparent: treat as brightest
}

Image upscaleNearest(const Image& src, int f)
{
    Image out;
    out.indexed = src.indexed;
    out.palette = src.palette;
    out.width = src.width * f;
    out.height = src.height * f;
    int bpp = src.indexed ? 1 : 4;
    out.pixels.resize((size_t)out.width * out.height * bpp);
    for (int y = 0; y < out.height; y++) {
        const u8* s = src.row(y / f);
        u8* d = out.row(y);
        for (int x = 0; x < out.width; x++) memcpy(d + x * bpp, s + (x / f) * bpp, bpp);
    }
    return out;
}

namespace {
inline bool anyEq3(int b, int a0, int a1, int a2) { return b == a0 || b == a1 || b == a2; }
inline bool allEq2(int b, int a0, int a1) { return b == a0 && b == a1; }
inline bool allEq3(int b, int a0, int a1, int a2) { return b == a0 && b == a1 && b == a2; }
inline bool allEq4(int b, int a0, int a1, int a2, int a3) { return b == a0 && b == a1 && b == a2 && b == a3; }
inline bool noneEq2(int b, int a0, int a1) { return b != a0 && b != a1; }
inline bool noneEq4(int b, int a0, int a1, int a2, int a3) { return b != a0 && b != a1 && b != a2 && b != a3; }
} // namespace

Image upscaleIndexed2x(const Image& srcImg, const int luma[256])
{
    const int w = srcImg.width, h = srcImg.height;
    Image out;
    out.indexed = true;
    out.palette = srcImg.palette;
    out.width = w * 2;
    out.height = h * 2;
    out.pixels.assign((size_t)out.width * out.height, 0);
    const u8* sp = srcImg.pixels.data();
    auto src = [&](int x, int y) -> int {
        x = x < 0 ? 0 : (x >= w ? w - 1 : x);
        y = y < 0 ? 0 : (y >= h ? h - 1 : y);
        return sp[(size_t)y * w + x];
    };
    for (int y = 0; y < h; y++) {
        u8* d0 = out.pixels.data() + (size_t)(2 * y) * out.width;
        u8* d1 = d0 + out.width;
        for (int x = 0; x < w; x++) {
            int A = src(x - 1, y - 1), B = src(x, y - 1), C = src(x + 1, y - 1);
            int D = src(x - 1, y), E = src(x, y), F = src(x + 1, y);
            int G = src(x - 1, y + 1), H = src(x, y + 1), I = src(x + 1, y + 1);
            int J = E, K = E, L = E, M = E;
            if (A != E || B != E || C != E || D != E || F != E || G != E || H != E || I != E) {
                int P = src(x, y - 2), S = src(x, y + 2);
                int Q = src(x - 2, y), R = src(x + 2, y);
                int Bl = luma[B], Dl = luma[D], El = luma[E], Fl = luma[F], Hl = luma[H];

                // 1:1 slope rules
                if ((D == B && D != H && D != F) && (El >= Dl || E == A) && anyEq3(E, A, C, G) && ((El < Dl) || A != D || E != P || E != Q)) J = D;
                if ((B == F && B != D && B != H) && (El >= Bl || E == C) && anyEq3(E, A, C, I) && ((El < Bl) || C != B || E != P || E != R)) K = B;
                if ((H == D && H != F && H != B) && (El >= Hl || E == G) && anyEq3(E, A, G, I) && ((El < Hl) || G != H || E != S || E != Q)) L = H;
                if ((F == H && F != B && F != D) && (El >= Fl || E == I) && anyEq3(E, C, G, I) && ((El < Fl) || I != H || E != R || E != S)) M = F;

                // Intersection rules
                if ((E != F && allEq4(E, C, I, D, Q) && allEq2(F, B, H)) && (F != src(x + 3, y))) K = M = F;
                if ((E != D && allEq4(E, A, G, F, R) && allEq2(D, B, H)) && (D != src(x - 3, y))) J = L = D;
                if ((E != H && allEq4(E, G, I, B, P) && allEq2(H, D, F)) && (H != src(x, y + 3))) L = M = H;
                if ((E != B && allEq4(E, A, C, H, S) && allEq2(B, D, F)) && (B != src(x, y - 3))) J = K = B;
                if (Bl < El && allEq4(E, G, H, I, S) && noneEq4(E, A, D, C, F)) J = K = B;
                if (Hl < El && allEq4(E, A, B, C, P) && noneEq4(E, D, G, I, F)) L = M = H;
                if (Fl < El && allEq4(E, A, D, G, Q) && noneEq4(E, B, C, I, H)) K = M = F;
                if (Dl < El && allEq4(E, C, F, I, R) && noneEq4(E, B, A, G, H)) J = L = D;

                // 2:1 slope rules
                if (H != B) {
                    if (H != A && H != E && H != C) {
                        if (allEq3(H, G, F, R) && noneEq2(H, D, src(x + 2, y - 1))) L = M;
                        if (allEq3(H, I, D, Q) && noneEq2(H, F, src(x - 2, y - 1))) M = L;
                    }
                    if (B != I && B != G && B != E) {
                        if (allEq3(B, A, F, R) && noneEq2(B, D, src(x + 2, y + 1))) J = K;
                        if (allEq3(B, C, D, Q) && noneEq2(B, F, src(x - 2, y + 1))) K = J;
                    }
                }
                if (F != D) {
                    if (D != I && D != E && D != C) {
                        if (allEq3(D, A, H, S) && noneEq2(D, B, src(x + 1, y + 2))) J = L;
                        if (allEq3(D, G, B, P) && noneEq2(D, H, src(x + 1, y - 2))) L = J;
                    }
                    if (F != E && F != A && F != G) {
                        if (allEq3(F, C, H, S) && noneEq2(F, B, src(x - 1, y + 2))) K = M;
                        if (allEq3(F, I, B, P) && noneEq2(F, H, src(x - 1, y - 2))) M = K;
                    }
                }
            }
            d0[2 * x] = (u8)J;
            d0[2 * x + 1] = (u8)K;
            d1[2 * x] = (u8)L;
            d1[2 * x + 1] = (u8)M;
        }
    }
    return out;
}

Image upscaleIndexed(const Image& src, int factor, const int luma[256])
{
    if (factor <= 1) return src;
    Image cur = upscaleIndexed2x(src, luma);
    for (int f = 4; f <= factor; f *= 2) cur = upscaleIndexed2x(cur, luma);
    return cur;
}

} // namespace fo
