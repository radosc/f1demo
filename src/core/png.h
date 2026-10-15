// PNG codec supporting paletted (index-preserving) and RGBA images.
#pragma once
#include "core/base.h"

namespace fo {

struct Image {
    int width = 0;
    int height = 0;
    bool indexed = false;     // true: `pixels` holds palette indices (1 byte/px)
    std::vector<u8> pixels;   // indexed: w*h bytes; otherwise RGBA w*h*4 bytes
    std::vector<u32> palette; // indexed only: 0xAARRGGBB entries (alpha from tRNS)

    u8* row(int y) { return pixels.data() + (size_t)y * width * (indexed ? 1 : 4); }
    const u8* row(int y) const { return pixels.data() + (size_t)y * width * (indexed ? 1 : 4); }
    void allocIndexed(int w, int h) { width = w; height = h; indexed = true; pixels.assign((size_t)w * h, 0); }
    void allocRGBA(int w, int h) { width = w; height = h; indexed = false; pixels.assign((size_t)w * h * 4, 0); }
};

// Decodes a PNG. Paletted 1/2/4/8-bit images stay indexed; everything else becomes RGBA8.
bool pngDecode(const u8* data, size_t size, Image& out, std::string* error = nullptr);
// Encodes indexed (with palette + tRNS) or RGBA images.
Bytes pngEncode(const Image& img, int level = 6);

} // namespace fo
