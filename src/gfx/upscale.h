// Palette-preserving pixel art magnification (MMPX style rules).
#pragma once
#include "core/png.h"

namespace fo {

// 2x magnification of an indexed image. `luma` gives brightness per palette index
// (index 0 = transparent should be the brightest so opaque art wins at corners).
Image upscaleIndexed2x(const Image& src, const int luma[256]);
// Repeated 2x until `factor` (must be power of two, 1..8).
Image upscaleIndexed(const Image& src, int factor, const int luma[256]);
// Nearest-neighbor magnification (used for masks / reference).
Image upscaleNearest(const Image& src, int factor);
// Builds the luma table from an ARGB palette.
void buildLumaTable(const u32* argb, int luma[256]);

// RGBA (straight alpha) copy of an indexed image; index 0 becomes transparent.
Image indexedToRGBA(const Image& src, const u32* paletteArgb);
// Edge-directed smooth magnification (2, 4 or 8) of an RGBA image; alpha-aware (anti-aliased silhouettes).
Image upscaleSmooth(const Image& rgba, int factor);
// Gives fully transparent pixels the colour of their opaque neighbours so filtering shows no dark fringes.
void bleedAlpha(Image& rgba, int passes = 8);

} // namespace fo
