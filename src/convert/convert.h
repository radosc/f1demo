// Converts the original FALLDEMO.DAT into the open, moddable `data/` tree.
#pragma once
#include "core/base.h"

namespace fo {

struct ConvertOptions {
    std::string datPath;
    std::string outDir;
    int hdScale = 4;        // 0 = skip HD sprites, else 2 or 4
    // HD style: 1 = smooth edge-directed RGBA (default; sprites with palette-animated colours stay pixel style),
    // 0 = pixel-art magnification that keeps the palette (crisp, blockier).
    int hdStyle = 1;
    bool art = true;
    bool text = true;
    bool protos = true;
    bool maps = true;
    bool scripts = true;
    bool sound = true;
    bool misc = true;
    std::function<void(const std::string& stage, float progress)> onProgress;
};

bool convertGameData(const ConvertOptions& opt);

} // namespace fo
