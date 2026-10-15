// FRM sprite decoding and conversion to sprite sheets (PNG + JSON metadata).
#pragma once
#include "core/png.h"
#include "core/json.h"

namespace fo {

struct FrmFrame {
    int w = 0, h = 0;
    int ox = 0, oy = 0; // per-frame "hot" offset applied while animating
    std::vector<u8> px;
};

struct Frm {
    int fps = 10;
    int actionFrame = 0;
    int framesPerDir = 0;
    int dirCount = 1; // 1 = same image for all 6 rotations
    int shiftX[6] = {};
    int shiftY[6] = {};
    std::vector<FrmFrame> dirs[6];
};

bool frmDecode(const Bytes& data, Frm& out);
// Merges a directional .FR0-.FR5 set (each holding one direction) into a 6-direction Frm.
bool frmMergeDirectional(Frm* parts[6], Frm& out);

// Sprite sheet layout in 1x pixel units. Frame pixels are packed per direction row.
struct SheetFrame {
    int x = 0, y = 0, w = 0, h = 0;
    int ox = 0, oy = 0;
};

struct SpriteMeta {
    int fps = 10;
    int actionFrame = 0;
    int framesPerDir = 1;
    int dirCount = 1;
    int shiftX[6] = {};
    int shiftY[6] = {};
    int sheetW = 0, sheetH = 0;
    std::vector<SheetFrame> frames; // dirCount * framesPerDir, direction-major

    const SheetFrame& frame(int dir, int f) const
    {
        int d = dirCount > 1 ? clampv(dir, 0, dirCount - 1) : 0;
        int fi = clampv(f, 0, framesPerDir - 1);
        return frames[(size_t)d * framesPerDir + fi];
    }
    Json toJson() const;
    bool fromJson(const Json& j);
};

// Packs `frm` into an indexed sheet image (palette left empty) and fills `meta`.
void frmToSheet(const Frm& frm, Image& sheet, SpriteMeta& meta, int pad = 4);
// Extracts one frame from a sheet (1x) as a standalone indexed image.
Image sheetCrop(const Image& sheet, const SheetFrame& f, int scale);

} // namespace fo
