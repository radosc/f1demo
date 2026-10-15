#include "gfx/frm.h"

namespace fo {

bool frmDecode(const Bytes& data, Frm& out)
{
    if (data.size() < 62) return false;
    BeReader r(data);
    r.u32v(); // version
    out.fps = r.u16v();
    if (out.fps == 0) out.fps = 10;
    out.actionFrame = r.u16v();
    out.framesPerDir = r.u16v();
    for (int i = 0; i < 6; i++) out.shiftX[i] = r.i16v();
    for (int i = 0; i < 6; i++) out.shiftY[i] = r.i16v();
    u32 offs[6];
    for (int i = 0; i < 6; i++) offs[i] = r.u32v();
    r.u32v(); // frame area size
    if (out.framesPerDir <= 0) return false;
    bool allSame = true;
    for (int i = 1; i < 6; i++)
        if (offs[i] != offs[0]) allSame = false;
    out.dirCount = allSame ? 1 : 6;
    for (int d = 0; d < out.dirCount; d++) {
        out.dirs[d].clear();
        size_t p = 62 + (size_t)offs[d];
        for (int f = 0; f < out.framesPerDir; f++) {
            if (p + 12 > data.size()) return false;
            FrmFrame fr;
            fr.w = rdBE16(&data[p]);
            fr.h = rdBE16(&data[p + 2]);
            u32 size = rdBE32(&data[p + 4]);
            fr.ox = rdBE16s(&data[p + 8]);
            fr.oy = rdBE16s(&data[p + 10]);
            p += 12;
            if (p + size > data.size()) return false;
            fr.px.assign(data.begin() + p, data.begin() + p + std::min<size_t>(size, (size_t)fr.w * fr.h));
            fr.px.resize((size_t)fr.w * fr.h, 0);
            p += size;
            out.dirs[d].push_back(std::move(fr));
        }
    }
    return true;
}

bool frmMergeDirectional(Frm* parts[6], Frm& out)
{
    Frm* first = nullptr;
    for (int i = 0; i < 6; i++)
        if (parts[i]) { first = parts[i]; break; }
    if (!first) return false;
    out = Frm();
    out.fps = first->fps;
    out.actionFrame = first->actionFrame;
    out.framesPerDir = first->framesPerDir;
    out.dirCount = 6;
    for (int d = 0; d < 6; d++) {
        Frm* src = parts[d] ? parts[d] : first;
        out.shiftX[d] = src->shiftX[parts[d] ? d : 0];
        out.shiftY[d] = src->shiftY[parts[d] ? d : 0];
        out.dirs[d] = src->dirs[0];
        if ((int)out.dirs[d].size() < out.framesPerDir) out.dirs[d].resize(out.framesPerDir);
        out.dirs[d].resize(out.framesPerDir);
    }
    return true;
}

Json SpriteMeta::toJson() const
{
    Json j = Json::object();
    j.set("fps", fps);
    j.set("actionFrame", actionFrame);
    j.set("framesPerDirection", framesPerDir);
    j.set("directions", dirCount);
    Json sh = Json::array();
    for (int d = 0; d < 6; d++) {
        Json p = Json::array();
        p.push(shiftX[d]);
        p.push(shiftY[d]);
        sh.push(p);
    }
    j.set("shift", sh);
    Json sz = Json::array();
    sz.push(sheetW);
    sz.push(sheetH);
    j.set("sheetSize", sz);
    Json dirs = Json::array();
    for (int d = 0; d < dirCount; d++) {
        Json row = Json::array();
        for (int f = 0; f < framesPerDir; f++) {
            const SheetFrame& sf = frames[(size_t)d * framesPerDir + f];
            Json fr = Json::array();
            fr.push(sf.x);
            fr.push(sf.y);
            fr.push(sf.w);
            fr.push(sf.h);
            fr.push(sf.ox);
            fr.push(sf.oy);
            row.push(fr);
        }
        dirs.push(row);
    }
    j.set("frames", dirs);
    return j;
}

bool SpriteMeta::fromJson(const Json& j)
{
    fps = j["fps"].toInt(10);
    if (fps <= 0) fps = 10;
    actionFrame = j["actionFrame"].toInt(0);
    framesPerDir = j["framesPerDirection"].toInt(1);
    dirCount = j["directions"].toInt(1);
    if (dirCount != 1 && dirCount != 6) dirCount = 1;
    for (int d = 0; d < 6; d++) {
        shiftX[d] = j["shift"][d][(size_t)0].toInt();
        shiftY[d] = j["shift"][d][(size_t)1].toInt();
    }
    sheetW = j["sheetSize"][(size_t)0].toInt();
    sheetH = j["sheetSize"][(size_t)1].toInt();
    frames.clear();
    const Json& dirs = j["frames"];
    for (int d = 0; d < dirCount; d++) {
        for (int f = 0; f < framesPerDir; f++) {
            const Json& fr = dirs[(size_t)d][(size_t)f];
            SheetFrame sf;
            sf.x = fr[(size_t)0].toInt();
            sf.y = fr[(size_t)1].toInt();
            sf.w = fr[(size_t)2].toInt();
            sf.h = fr[(size_t)3].toInt();
            sf.ox = fr[(size_t)4].toInt();
            sf.oy = fr[(size_t)5].toInt();
            frames.push_back(sf);
        }
    }
    return framesPerDir > 0 && !frames.empty() && sheetW > 0 && sheetH > 0;
}

void frmToSheet(const Frm& frm, Image& sheet, SpriteMeta& meta, int pad)
{
    meta = SpriteMeta();
    meta.fps = frm.fps;
    meta.actionFrame = frm.actionFrame;
    meta.framesPerDir = frm.framesPerDir;
    meta.dirCount = frm.dirCount;
    for (int d = 0; d < 6; d++) {
        meta.shiftX[d] = frm.shiftX[d];
        meta.shiftY[d] = frm.shiftY[d];
    }
    const int kMaxRow = 3000; // keeps 4x sheets below D3D11's 16K texture limit
    int x = pad, y = pad, rowH = 0, sheetW = 0;
    meta.frames.resize((size_t)frm.dirCount * frm.framesPerDir);
    for (int d = 0; d < frm.dirCount; d++) {
        if (d > 0) {
            y += rowH + pad;
            x = pad;
            rowH = 0;
        }
        for (int f = 0; f < frm.framesPerDir; f++) {
            const FrmFrame& fr = frm.dirs[d][f];
            if (x > pad && x + fr.w + pad > kMaxRow) {
                y += rowH + pad;
                x = pad;
                rowH = 0;
            }
            SheetFrame& sf = meta.frames[(size_t)d * frm.framesPerDir + f];
            sf.x = x;
            sf.y = y;
            sf.w = fr.w;
            sf.h = fr.h;
            sf.ox = fr.ox;
            sf.oy = fr.oy;
            x += std::max(fr.w, 1) + pad;
            rowH = std::max(rowH, fr.h);
            sheetW = std::max(sheetW, x);
        }
    }
    int sheetH = y + rowH + pad;
    meta.sheetW = sheetW;
    meta.sheetH = sheetH;
    sheet.allocIndexed(sheetW, sheetH);
    for (int d = 0; d < frm.dirCount; d++) {
        for (int f = 0; f < frm.framesPerDir; f++) {
            const FrmFrame& fr = frm.dirs[d][f];
            const SheetFrame& sf = meta.frames[(size_t)d * frm.framesPerDir + f];
            for (int yy = 0; yy < fr.h; yy++) memcpy(sheet.row(sf.y + yy) + sf.x, fr.px.data() + (size_t)yy * fr.w, (size_t)fr.w);
        }
    }
}

Image sheetCrop(const Image& sheet, const SheetFrame& f, int scale)
{
    Image out;
    out.indexed = sheet.indexed;
    out.palette = sheet.palette;
    out.width = f.w * scale;
    out.height = f.h * scale;
    int bpp = sheet.indexed ? 1 : 4;
    out.pixels.resize((size_t)out.width * out.height * bpp);
    for (int y = 0; y < out.height; y++) {
        memcpy(out.row(y), sheet.row(f.y * scale + y) + (size_t)f.x * scale * bpp, (size_t)out.width * bpp);
    }
    return out;
}

} // namespace fo
