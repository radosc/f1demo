// mvedump: standalone test tool for the MVE decoder (not part of the game build).
// Decodes an Interplay MVE file, writes every Nth frame (plus the first and the last) as PNG, a contact sheet
// of evenly spaced frames, and the audio as a WAV file, then prints the stream parameters.
//
// Usage: mvedump <file.mve> <output prefix> [--every N] [--sheet N] [--raw FILE]
//   --every N   write frame PNGs for every Nth frame (default 10; 0 = only first and last)
//   --sheet N   number of frames on the half-size contact sheet <prefix>_sheet.png (default 20; 0 = none)
//   --raw FILE  also write every frame as raw data: width*height palette indices followed by the palette as
//               256 little-endian 0xAARRGGBB words (the layout of ffmpeg's "-f rawvideo -pix_fmt pal8")
#include "media/mve.h"
#include "core/png.h"

#include <filesystem>

using namespace fo;

namespace {

bool readFile(const std::string& path, Bytes& out)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(size > 0 ? (size_t)size : 0);
    bool ok = out.empty() || fread(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    return ok;
}

bool writeFile(const std::string& path, const u8* data, size_t size)
{
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, size, f) == size;
    fclose(f);
    return ok;
}

bool writeFramePng(const std::string& path, int w, int h, const std::vector<u8>& pixels, const u32* palette)
{
    Image img;
    img.allocIndexed(w, h);
    if (!pixels.empty()) memcpy(img.pixels.data(), pixels.data(), img.pixels.size());
    img.palette.assign(palette, palette + 256);
    Bytes png = pngEncode(img, 6);
    return writeFile(path, png.data(), png.size());
}

bool writeWav(const std::string& path, const std::vector<i16>& samples, int rate, int channels)
{
    u32 dataBytes = (u32)(samples.size() * 2);
    Bytes out(44 + dataBytes);
    u8* p = out.data();
    memcpy(p, "RIFF", 4);
    wrLE32(p + 4, 36 + dataBytes);
    memcpy(p + 8, "WAVEfmt ", 8);
    wrLE32(p + 16, 16);
    wrLE16(p + 20, 1);  // PCM
    wrLE16(p + 22, (u16)channels);
    wrLE32(p + 24, (u32)rate);
    wrLE32(p + 28, (u32)(rate * channels * 2));
    wrLE16(p + 32, (u16)(channels * 2));
    wrLE16(p + 34, 16);
    memcpy(p + 36, "data", 4);
    wrLE32(p + 40, dataBytes);
    for (size_t i = 0; i < samples.size(); i++) wrLE16(p + 44 + i * 2, (u16)samples[i]);
    return writeFile(path, out.data(), out.size());
}

// Half-size RGB copy of a paletted frame placed into an RGBA contact sheet.
void blitHalf(Image& sheet, int ox, int oy, int w, int h, const std::vector<u8>& pixels, const u32* palette)
{
    for (int y = 0; y + 1 < h; y += 2) {
        u8* row = sheet.row(oy + y / 2) + (size_t)ox * 4;
        for (int x = 0; x + 1 < w; x += 2) {
            u32 sum[3] = {0, 0, 0};
            for (int k = 0; k < 4; k++) {
                u32 c = palette[pixels[(size_t)(y + (k >> 1)) * w + x + (k & 1)]];
                sum[0] += (c >> 16) & 255;
                sum[1] += (c >> 8) & 255;
                sum[2] += c & 255;
            }
            u8* d = row + (x / 2) * 4;
            d[0] = (u8)(sum[0] / 4);
            d[1] = (u8)(sum[1] / 4);
            d[2] = (u8)(sum[2] / 4);
            d[3] = 255;
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: mvedump <file.mve> <output prefix> [--every N] [--sheet N] [--raw FILE]\n");
        return 2;
    }
    std::string inPath = argv[1], prefix = argv[2], rawPath;
    int every = 10, sheetCount = 20;
    for (int i = 3; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--every")) every = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--sheet")) sheetCount = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--raw")) rawPath = argv[i + 1];
    }
    std::filesystem::path parent = std::filesystem::path(prefix).parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
    }

    Bytes data;
    if (!readFile(inPath, data)) {
        fprintf(stderr, "cannot read %s\n", inPath.c_str());
        return 1;
    }
    size_t fileSize = data.size();
    MveDecoder dec;
    std::string err;
    if (!dec.open(std::move(data), &err)) {
        fprintf(stderr, "open failed: %s\n", err.c_str());
        return 1;
    }
    printf("%s: %zu bytes\n", inPath.c_str(), fileSize);
    printf("video: %dx%d, %.4f s/frame (%.3f fps), %d frames in pre-scan\n", dec.width(), dec.height(),
           dec.frameDuration(), 1.0 / dec.frameDuration(), dec.frameCount());
    if (dec.hasAudio())
        printf("audio: %d Hz, %d channel(s), source %d-bit%s -> output s16 interleaved\n", dec.audioRate(),
               dec.audioChannels(), dec.audioSourceBits(), dec.audioCompressed() ? " DPCM" : " PCM");
    else
        printf("audio: none\n");

    // contact sheet slots
    std::vector<int> sheetFrames;
    if (sheetCount > 0 && dec.frameCount() > 0) {
        int n = std::min(sheetCount, dec.frameCount());
        for (int i = 0; i < n; i++) sheetFrames.push_back(n > 1 ? (int)((long long)i * (dec.frameCount() - 1) / (n - 1)) : 0);
    }
    const int cols = 5;
    const int sheetW = dec.width(), sheetH = dec.height();  // slot size (frames of another size are skipped)
    Image sheet;
    if (!sheetFrames.empty()) {
        int rows = ((int)sheetFrames.size() + cols - 1) / cols;
        sheet.allocRGBA(cols * (sheetW / 2 + 4), rows * (sheetH / 2 + 4));
    }

    FILE* raw = nullptr;
    if (!rawPath.empty() && !(raw = fopen(rawPath.c_str(), "wb"))) {
        fprintf(stderr, "cannot write %s\n", rawPath.c_str());
        return 1;
    }

    std::vector<i16> audio;
    std::vector<u8> lastFrame;
    u32 lastPalette[256];
    int frames = 0, paletteChanges = 0, written = 0;
    bool lastWritten = false;
    double t0 = timeSeconds();
    while (dec.nextFrame()) {
        std::vector<i16> a = dec.takeAudio();
        audio.insert(audio.end(), a.begin(), a.end());
        if (raw) {
            u8 pal[1024];
            for (int i = 0; i < 256; i++) wrLE32(pal + i * 4, dec.palette()[i]);
            fwrite(dec.frame().data(), 1, dec.frame().size(), raw);
            fwrite(pal, 1, sizeof(pal), raw);
        }
        if (dec.paletteChanged()) paletteChanges++;
        int idx = dec.frameIndex();
        lastWritten = false;
        if (idx == 0 || (every > 0 && idx % every == 0)) {
            writeFramePng(strFormat("%s_%04d.png", prefix.c_str(), idx), dec.width(), dec.height(), dec.frame(),
                          dec.palette());
            written++;
            lastWritten = true;
        }
        for (size_t s = 0; s < sheetFrames.size(); s++)
            if (sheetFrames[s] == idx && dec.width() == sheetW && dec.height() == sheetH)
                blitHalf(sheet, (int)(s % cols) * (sheetW / 2 + 4) + 2, (int)(s / cols) * (sheetH / 2 + 4) + 2, sheetW,
                         sheetH, dec.frame(), dec.palette());
        lastFrame = dec.frame();
        memcpy(lastPalette, dec.palette(), sizeof(lastPalette));
        frames++;
    }
    double t1 = timeSeconds();
    if (raw) fclose(raw);
    std::vector<i16> tail = dec.takeAudio();
    audio.insert(audio.end(), tail.begin(), tail.end());
    if (frames > 0 && !lastWritten) {
        writeFramePng(strFormat("%s_%04d.png", prefix.c_str(), frames - 1), dec.width(), dec.height(), lastFrame,
                      lastPalette);
        written++;
    }
    if (!sheetFrames.empty()) {
        Bytes png = pngEncode(sheet, 6);
        writeFile(prefix + "_sheet.png", png.data(), png.size());
        printf("contact sheet frames:");
        for (int f : sheetFrames) printf(" %d", f);
        printf("\n");
    }

    double videoSeconds = frames * dec.frameDuration();
    printf("decoded %d frames in %.3f s (%.2f ms/frame), %d palette change(s), %d PNG(s) written\n", frames,
           t1 - t0, frames ? (t1 - t0) * 1000.0 / frames : 0.0, paletteChanges, written);
    printf("video duration %.3f s\n", videoSeconds);
    if (dec.hasAudio()) {
        double audioSeconds = (double)audio.size() / dec.audioChannels() / dec.audioRate();
        printf("audio: %zu samples/channel = %.3f s (video %.3f s, difference %+.3f s)\n",
               audio.size() / dec.audioChannels(), audioSeconds, videoSeconds, audioSeconds - videoSeconds);
        writeWav(prefix + ".wav", audio, dec.audioRate(), dec.audioChannels());
    }
    const MveStats& st = dec.stats();
    printf("stats: decoded=%d underruns=%d leftovers=%d badVectors=%d unsupported=%d\n", st.framesDecoded,
           st.streamUnderruns, st.streamLeftovers, st.badVectors, st.unsupported);
    bool clean = st.streamUnderruns == 0 && st.streamLeftovers == 0 && st.badVectors == 0 && st.unsupported == 0 &&
                 frames == dec.frameCount();
    printf("%s\n", clean ? "CLEAN" : "PROBLEMS DETECTED");
    return clean ? 0 : 3;
}
