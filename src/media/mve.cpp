// Interplay MVE decoder, written from the public format descriptions on the MultimediaWiki ("Interplay MVE",
// "Interplay Video", "Interplay DPCM") and checked against the Fallout demo movies.
//
// File layout: a 26-byte header, then chunks { u16 size, u16 type, opcodes... }, each opcode being
// { u16 size, u8 type, u8 version, payload }. All values are little-endian.
//
// Video (8-bit): the frame is a grid of 8x8 blocks. Opcode 0x0F carries the decoding map (one 4-bit block code
// per block, low nibble first) and opcode 0x11 the block data stream (after a 14-byte header). The player keeps
// two buffers: a frame is drawn into the buffer that holds the frame from two frames ago (after an optional swap
// requested by bit 0 of the header flags), so "unchanged" blocks keep that older content, while the other buffer
// holds the most recently displayed frame. Opcode 0x07 shows the drawn buffer.
#include "media/mve.h"

namespace fo {

namespace {

const u8 kSignature[20] = {'I', 'n', 't', 'e', 'r', 'p', 'l', 'a', 'y', ' ',
                           'M', 'V', 'E', ' ', 'F', 'i', 'l', 'e', 0x1A, 0x00};
const size_t kFileHeaderSize = 26;  // signature + three words (0x001A, 0x0100, 0x1133)
const u64 kDefaultFrameUs = 66728;  // 8341 * 8 us, the usual Interplay timer (~15 fps)
const int kMaxBlocks = 256;         // per dimension (2048 pixels); real files use at most 80x60

enum : u8 {
    kOpEndOfStream = 0x00,
    kOpEndOfChunk = 0x01,
    kOpCreateTimer = 0x02,
    kOpInitAudio = 0x03,
    kOpStartAudio = 0x04,
    kOpInitVideo = 0x05,
    kOpVideoData06 = 0x06,  // older video format (not supported)
    kOpShowFrame = 0x07,
    kOpAudioData = 0x08,
    kOpAudioSilence = 0x09,
    kOpVideoMode = 0x0A,
    kOpGradient = 0x0B,
    kOpPalette = 0x0C,
    kOpPalettePacked = 0x0D,
    kOpSkipMap = 0x0E,      // used with format 0x10 only
    kOpDecodingMap = 0x0F,
    kOpVideoData10 = 0x10,  // newer video format (not supported)
    kOpVideoData = 0x11,
};

// Interplay DPCM delta table.
const i16 kDpcmDelta[256] = {
    0,      1,      2,      3,      4,      5,      6,      7,      8,      9,      10,     11,     12,     13,
    14,     15,     16,     17,     18,     19,     20,     21,     22,     23,     24,     25,     26,     27,
    28,     29,     30,     31,     32,     33,     34,     35,     36,     37,     38,     39,     40,     41,
    42,     43,     47,     51,     56,     61,     66,     72,     79,     86,     94,     102,    112,    122,
    133,    145,    158,    173,    189,    206,    225,    245,    267,    292,    318,    348,    379,    414,
    452,    493,    538,    587,    640,    699,    763,    832,    908,    991,    1081,   1180,   1288,   1405,
    1534,   1673,   1826,   1993,   2175,   2373,   2590,   2826,   3084,   3365,   3672,   4008,   4373,   4772,
    5208,   5683,   6202,   6767,   7385,   8059,   8794,   9597,   10472,  11428,  12471,  13609,  14851,  16206,
    17685,  19298,  21060,  22981,  25078,  27367,  29864,  32589,  -29973, -26728, -23186, -19322, -15105, -10503,
    -5481,  -1,     1,      1,      5481,   10503,  15105,  19322,  23186,  26728,  29973,  -32589, -29864, -27367,
    -25078, -22981, -21060, -19298, -17685, -16206, -14851, -13609, -12471, -11428, -10472, -9597,  -8794,  -8059,
    -7385,  -6767,  -6202,  -5683,  -5208,  -4772,  -4373,  -4008,  -3672,  -3365,  -3084,  -2826,  -2590,  -2373,
    -2175,  -1993,  -1826,  -1673,  -1534,  -1405,  -1288,  -1180,  -1081,  -991,   -908,   -832,   -763,   -699,
    -640,   -587,   -538,   -493,   -452,   -414,   -379,   -348,   -318,   -292,   -267,   -245,   -225,   -206,
    -189,   -173,   -158,   -145,   -133,   -122,   -112,   -102,   -94,    -86,    -79,    -72,    -66,    -61,
    -56,    -51,    -47,    -43,    -42,    -41,    -40,    -39,    -38,    -37,    -36,    -35,    -34,    -33,
    -32,    -31,    -30,    -29,    -28,    -27,    -26,    -25,    -24,    -23,    -22,    -21,    -20,    -19,
    -18,    -17,    -16,    -15,    -14,    -13,    -12,    -11,    -10,    -9,     -8,     -7,     -6,     -5,
    -4,     -3,     -2,     -1,
};

// Block data stream; reading past the end yields zeros and sets `underrun`.
struct ByteStream {
    const u8* p;
    const u8* end;
    bool underrun = false;
    ByteStream(const u8* data, size_t size) : p(data), end(data + size) {}
    u8 byte()
    {
        if (p < end) return *p++;
        underrun = true;
        return 0;
    }
    u32 le16() { u32 lo = byte(); u32 hi = byte(); return lo | (hi << 8); }
    u32 le32() { u32 lo = le16(); u32 hi = le16(); return lo | (hi << 16); }
    u64 le64() { u64 lo = le32(); u64 hi = le32(); return lo | (hi << 32); }
    size_t left() const { return (size_t)(end - p); }
};

inline void fillRect(u8* dst, int stride, int w, int h, u8 c)
{
    for (int y = 0; y < h; y++) memset(dst + (ptrdiff_t)y * stride, c, (size_t)w);
}

// Two-colour pattern over cols x rows cells of cw x ch pixels: one bit per cell, consumed from the least
// significant bit, cells in row-major order; a set bit selects c1.
void pattern1(u8* dst, int stride, int cols, int rows, int cw, int ch, u8 c0, u8 c1, u64 bits)
{
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++, bits >>= 1)
            fillRect(dst + (ptrdiff_t)r * ch * stride + c * cw, stride, cw, ch, (bits & 1) ? c1 : c0);
}

// Four-colour pattern: two bits per cell (least significant first), indexing c[0..3].
void pattern2(u8* dst, int stride, int cols, int rows, int cw, int ch, const u8* c, u64 bits)
{
    for (int r = 0; r < rows; r++)
        for (int x = 0; x < cols; x++, bits >>= 2)
            fillRect(dst + (ptrdiff_t)r * ch * stride + x * cw, stride, cw, ch, c[bits & 3]);
}

// Frame period set by a timer opcode (rate * subdivision microseconds); 0 if implausible (corrupt data).
inline u64 timerPeriodUs(const u8* p)
{
    u64 us = (u64)rdLE32(p) * rdLE16(p + 4);
    return us <= 10000000 ? us : 0;
}

inline u32 vgaColor(u8 r, u8 g, u8 b)
{
    auto x = [](u8 v) { v &= 63; return (u32)((v << 2) | (v >> 4)); };
    return 0xFF000000u | (x(r) << 16) | (x(g) << 8) | x(b);
}

} // namespace

MveDecoder::MveDecoder()
{
    for (auto& c : palette_) c = 0xFF000000u;
}

void MveDecoder::close()
{
    data_.clear();
    data_.shrink_to_fit();
    scanBlocksW_ = scanBlocksH_ = 0;
    scanTimerUs_ = 0;
    frameCount_ = 0;
    audioRate_ = audioChannels_ = 0;
    audio16_ = audioCompressed_ = false;
    resetState();
}

bool MveDecoder::open(Bytes data, std::string* error)
{
    close();
    auto fail = [&](const char* msg) {
        if (error) *error = msg;
        close();
        return false;
    };
    if (data.size() < kFileHeaderSize || memcmp(data.data(), kSignature, sizeof(kSignature)) != 0)
        return fail("not an Interplay MVE file");
    data_ = std::move(data);

    // Pre-scan the chunk structure for the stream parameters and the number of displayed frames.
    bool truecolor = false, haveVideoData = false, otherVideo = false;
    size_t pos = kFileHeaderSize;
    bool done = false;
    const u8* const file = data_.data();
    while (!done && pos + 4 <= data_.size()) {
        const size_t start = pos + 4;
        const size_t end = std::min(start + rdLE16(file + pos), data_.size());
        pos = start + rdLE16(file + pos);  // next chunk (a truncated last chunk is scanned as far as it goes)
        for (size_t p = start; p + 4 <= end;) {
            size_t len = rdLE16(file + p);
            u8 type = file[p + 2], version = file[p + 3];
            const u8* d = file + p + 4;
            if (p + 4 + len > end) break;
            p += 4 + len;
            if (type == kOpEndOfStream) { done = true; break; }
            if (type == kOpEndOfChunk) break;
            switch (type) {
            case kOpCreateTimer:
                if (!scanTimerUs_ && len >= 6) scanTimerUs_ = timerPeriodUs(d);
                break;
            case kOpInitAudio:
                if (!audioChannels_) parseAudioInit(version, d, len);
                break;
            case kOpInitVideo:
                if (!scanBlocksW_ && len >= 4) {
                    scanBlocksW_ = rdLE16(d);
                    scanBlocksH_ = rdLE16(d + 2);
                    if (version >= 2 && len >= 8 && rdLE16(d + 6)) truecolor = true;
                }
                break;
            case kOpShowFrame: frameCount_++; break;
            case kOpVideoData: haveVideoData = true; break;
            case kOpVideoData06:
            case kOpVideoData10: otherVideo = true; break;
            default: break;
            }
        }
    }
    if (scanBlocksW_ <= 0 || scanBlocksH_ <= 0 || scanBlocksW_ > kMaxBlocks || scanBlocksH_ > kMaxBlocks)
        return fail("MVE file has no (valid) video stream");
    if (truecolor) return fail("16-bit MVE video is not supported");
    if (otherVideo && !haveVideoData) return fail("unsupported MVE video format (opcode 0x06/0x10)");
    resetState();
    return true;
}

void MveDecoder::rewind()
{
    resetState();
}

void MveDecoder::resetState()
{
    chunkPos_ = opPos_ = chunkEnd_ = kFileHeaderSize;
    ended_ = data_.empty();
    for (auto& c : palette_) c = 0xFF000000u;
    paletteDirty_ = true;
    paletteChanged_ = false;
    timerUs_ = scanTimerUs_;
    frameIndex_ = -1;
    audio_.clear();
    map_.clear();
    cur_ = 0;
    blocksW_ = blocksH_ = -1;
    initVideo(scanBlocksW_, scanBlocksH_);
    stats_ = MveStats();
}

void MveDecoder::initVideo(int blocksW, int blocksH)
{
    if (blocksW == blocksW_ && blocksH == blocksH_) return;
    blocksW_ = std::max(blocksW, 0);
    blocksH_ = std::max(blocksH, 0);
    width_ = blocksW_ * 8;
    height_ = blocksH_ * 8;
    for (auto& b : buf_) b.assign((size_t)width_ * height_, 0);
    cur_ = 0;
}

double MveDecoder::frameDuration() const
{
    return (double)(timerUs_ ? timerUs_ : kDefaultFrameUs) / 1e6;
}

std::vector<i16> MveDecoder::takeAudio()
{
    std::vector<i16> out;
    out.swap(audio_);
    return out;
}

void MveDecoder::parseAudioInit(int version, const u8* p, size_t n)
{
    if (n < 6) return;
    u16 flags = rdLE16(p + 2);  // only bits 0..2 are meaningful, the rest is garbage in real files
    audioRate_ = rdLE16(p + 4);
    audioChannels_ = (flags & 1) ? 2 : 1;
    audio16_ = (flags & 2) != 0;
    audioCompressed_ = version >= 1 && (flags & 4) != 0;
    if (audioRate_ <= 0) audioChannels_ = 0;
}

bool MveDecoder::enterNextChunk()
{
    if (chunkPos_ + 4 > data_.size()) return false;
    size_t len = rdLE16(data_.data() + chunkPos_);
    opPos_ = chunkPos_ + 4;
    chunkEnd_ = std::min(opPos_ + len, data_.size());
    chunkPos_ = opPos_ + len;
    return true;
}

void MveDecoder::setPaletteEntry(int index, u8 r, u8 g, u8 b)
{
    if (index < 0 || index > 255) return;
    palette_[index] = vgaColor(r, g, b);
    paletteDirty_ = true;
}

bool MveDecoder::nextFrame()
{
    if (data_.empty() || ended_) return false;
    for (;;) {
        if (opPos_ + 4 > chunkEnd_) {
            if (!enterNextChunk()) {
                ended_ = true;
                return false;
            }
            continue;
        }
        const u8* h = data_.data() + opPos_;
        size_t len = rdLE16(h);
        u8 type = h[2], version = h[3];
        const u8* p = h + 4;
        if (opPos_ + 4 + len > chunkEnd_) {  // truncated file
            opPos_ = chunkEnd_;
            continue;
        }
        opPos_ += 4 + len;

        switch (type) {
        case kOpEndOfStream:
            ended_ = true;
            return false;
        case kOpEndOfChunk:
            opPos_ = chunkEnd_;
            break;
        case kOpCreateTimer:
            if (len >= 6 && timerPeriodUs(p)) timerUs_ = timerPeriodUs(p);
            break;
        case kOpInitAudio:
            parseAudioInit(version, p, len);
            break;
        case kOpInitVideo:
            if (len >= 4) {
                if (version >= 2 && len >= 8 && rdLE16(p + 6)) {
                    warn(stats_.unsupported, "16-bit video");
                    break;
                }
                initVideo(std::min<int>(rdLE16(p), kMaxBlocks), std::min<int>(rdLE16(p + 2), kMaxBlocks));
            }
            break;
        case kOpShowFrame:
            // The palette range in the payload names the entries to install; the entries themselves were already
            // stored by the palette opcodes, so they are applied as soon as they arrive.
            frameIndex_++;
            paletteChanged_ = paletteDirty_;
            paletteDirty_ = false;
            return true;
        case kOpAudioData:
        case kOpAudioSilence:
            decodeAudio(p, len, type == kOpAudioSilence);
            break;
        case kOpGradient:
            if (len >= 6) {
                // two ramps of 6-bit colours: red rows x blue columns, then red rows x green columns
                for (int ramp = 0; ramp < 2; ramp++) {
                    int base = p[ramp * 3], rows = p[ramp * 3 + 1], cols = p[ramp * 3 + 2];
                    for (int r = 0; r < rows; r++)
                        for (int c = 0; c < cols; c++) {
                            u8 red = (u8)(rows > 1 ? 63 * r / (rows - 1) : 0);
                            u8 other = (u8)(cols > 1 ? 39 * c / (cols - 1) : 0);
                            setPaletteEntry(base + r * cols + c, red, ramp ? other : 0, ramp ? 0 : other);
                        }
                }
            }
            break;
        case kOpPalette:
            if (len >= 4) {
                int start = rdLE16(p), count = rdLE16(p + 2);
                for (int i = 0; i < count && 4 + (size_t)i * 3 + 3 <= len; i++)
                    setPaletteEntry(start + i, p[4 + i * 3], p[5 + i * 3], p[6 + i * 3]);
            }
            break;
        case kOpPalettePacked: {
            // 32 groups of 8 entries: a mask byte (bit 0 = first entry), then one RGB triplet per set bit
            size_t q = 0;
            for (int g = 0; g < 32 && q < len; g++) {
                u8 mask = p[q++];
                for (int b = 0; b < 8; b++) {
                    if (!(mask & (1 << b))) continue;
                    if (q + 3 > len) break;
                    setPaletteEntry(g * 8 + b, p[q], p[q + 1], p[q + 2]);
                    q += 3;
                }
            }
            break;
        }
        case kOpDecodingMap:
            map_.assign(p, p + len);
            break;
        case kOpVideoData:
            if (len >= 14) {
                // header: frame number, previous frame number, x, y, width, height (blocks), flags
                if (rdLE16(p + 12) & 1) cur_ ^= 1;
                decodeVideo(p + 14, len - 14);
            }
            break;
        case kOpVideoData06:
        case kOpVideoData10:
            warn(stats_.unsupported, "video format 0x06/0x10");
            break;
        default:  // 0x04 start audio, 0x0A video mode, 0x0E skip map, 0x12..0x15: nothing to do
            break;
        }
    }
}

void MveDecoder::decodeAudio(const u8* p, size_t n, bool silence)
{
    if (!audioChannels_ || n < 6) return;
    if (!(rdLE16(p + 2) & 1)) return;  // stream mask: only stream 0 is decoded
    const int channels = audioChannels_;
    const bool wide = audio16_ || audioCompressed_;
    size_t total = rdLE16(p + 4);  // size of the decoded PCM in bytes
    if (wide) total /= 2;
    total -= total % (size_t)channels;  // whole sample frames only, keeps the channels interleaved correctly
    const u8* d = p + 6;
    const size_t dn = n - 6;
    const size_t start = audio_.size();
    if (!silence) {
        if (audioCompressed_) {
            // per channel an initial 16-bit predictor (also the first output sample), then one delta index per
            // sample, channels interleaved; the predictor saturates to 16 bits
            int pred[2] = {0, 0};
            size_t q = 0;
            for (int c = 0; c < channels && audio_.size() - start < total && q + 2 <= dn; c++, q += 2) {
                pred[c] = (i16)rdLE16(d + q);
                audio_.push_back((i16)pred[c]);
            }
            int c = 0;
            while (audio_.size() - start < total && q < dn) {
                pred[c] = clampv(pred[c] + kDpcmDelta[d[q++]], -32768, 32767);
                audio_.push_back((i16)pred[c]);
                if (channels == 2) c ^= 1;
            }
        } else if (audio16_) {
            size_t count = std::min(total, dn / 2);
            for (size_t i = 0; i < count; i++) audio_.push_back((i16)rdLE16(d + i * 2));
        } else {
            size_t count = std::min(total, dn);
            for (size_t i = 0; i < count; i++) audio_.push_back((i16)(((int)d[i] - 128) * 256));
        }
    }
    // silence frames, or a truncated data frame: pad so audio stays in step with the video
    if (audio_.size() - start < total) audio_.resize(start + total, 0);
}

void MveDecoder::copyBlock(u8* dst, const u8* srcBuf, ptrdiff_t srcOff)
{
    // Offsets are linear buffer positions (like the original player's pointer arithmetic), so a vector that
    // crosses the left/right edge wraps to the neighbouring row; only positions outside the buffer are rejected.
    const ptrdiff_t stride = width_;
    if (srcOff < 0 || srcOff + 7 * stride + 8 > stride * height_) {
        warn(stats_.badVectors, "motion vector outside the frame");
        return;
    }
    const u8* src = srcBuf + srcOff;
    for (int y = 0; y < 8; y++) memmove(dst + y * stride, src + y * stride, 8);
}

void MveDecoder::decodeVideo(const u8* data, size_t size)
{
    if (buf_[0].empty()) return;
    stats_.framesDecoded++;
    const int stride = width_;
    u8* const frame = buf_[cur_].data();          // frame being built (starts as the frame from two frames ago)
    const u8* const prev = buf_[cur_ ^ 1].data(); // most recently displayed frame
    ByteStream s(data, size);
    const size_t nibbles = map_.size() * 2;
    size_t block = 0;
    u8 c[8];
    for (int by = 0; by < blocksH_; by++) {
        for (int bx = 0; bx < blocksW_; bx++, block++) {
            // blocks missing from the map are left unchanged
            int code = block < nibbles ? (map_[block >> 1] >> ((block & 1) * 4)) & 15 : 1;
            const ptrdiff_t off = (ptrdiff_t)by * 8 * stride + bx * 8;
            u8* dst = frame + off;
            switch (code) {
            case 0x0:  // copy the block from the previous frame
                copyBlock(dst, prev, off);
                break;
            case 0x1:  // unchanged (keeps the content from two frames ago)
                break;
            case 0x2: {  // copy from below/right in the frame being built (not yet overwritten = two frames ago)
                int b = s.byte(), x, y;
                if (b < 56) {
                    x = 8 + b % 7;
                    y = b / 7;
                } else {
                    x = -14 + (b - 56) % 29;
                    y = 8 + (b - 56) / 29;
                }
                copyBlock(dst, frame, off + (ptrdiff_t)y * stride + x);
                break;
            }
            case 0x3: {  // copy from above/left in the frame being built (already decoded), mirrored 0x2 vector
                int b = s.byte(), x, y;
                if (b < 56) {
                    x = -(8 + b % 7);
                    y = -(b / 7);
                } else {
                    x = -(-14 + (b - 56) % 29);
                    y = -(8 + (b - 56) / 29);
                }
                copyBlock(dst, frame, off + (ptrdiff_t)y * stride + x);
                break;
            }
            case 0x4: {  // copy from the previous frame, vector -8..7 in each nibble (low = x, high = y)
                int b = s.byte();
                copyBlock(dst, prev, off + (ptrdiff_t)((b >> 4) - 8) * stride + ((b & 15) - 8));
                break;
            }
            case 0x5: {  // copy from the previous frame, signed byte vector (x, y)
                int x = s.byte(), y = s.byte();
                if (x > 127) x -= 256;
                if (y > 127) y -= 256;
                copyBlock(dst, prev, off + (ptrdiff_t)y * stride + x);
                break;
            }
            case 0x6:  // undefined for 8-bit video (never produced by the encoder); leave the block unchanged
                break;
            case 0x7:  // two colours
                c[0] = s.byte();
                c[1] = s.byte();
                if (c[0] <= c[1]) {
                    pattern1(dst, stride, 8, 8, 1, 1, c[0], c[1], s.le64());  // one bit per pixel
                } else {
                    pattern1(dst, stride, 4, 4, 2, 2, c[0], c[1], s.le16());  // one bit per 2x2 pixels
                }
                break;
            case 0x8:  // two colours per quadrant or per half
                c[0] = s.byte();
                c[1] = s.byte();
                if (c[0] <= c[1]) {
                    // quadrants in the order top-left, bottom-left, top-right, bottom-right
                    for (int q = 0; q < 4; q++) {
                        if (q) {
                            c[0] = s.byte();
                            c[1] = s.byte();
                        }
                        u8* qd = dst + (q & 1) * 4 * stride + (q >> 1) * 4;
                        pattern1(qd, stride, 4, 4, 1, 1, c[0], c[1], s.le16());
                    }
                } else {
                    u64 bitsA = s.le32();
                    c[2] = s.byte();
                    c[3] = s.byte();
                    u64 bitsB = s.le32();
                    if (c[2] <= c[3]) {  // left / right halves
                        pattern1(dst, stride, 4, 8, 1, 1, c[0], c[1], bitsA);
                        pattern1(dst + 4, stride, 4, 8, 1, 1, c[2], c[3], bitsB);
                    } else {  // top / bottom halves
                        pattern1(dst, stride, 8, 4, 1, 1, c[0], c[1], bitsA);
                        pattern1(dst + 4 * stride, stride, 8, 4, 1, 1, c[2], c[3], bitsB);
                    }
                }
                break;
            case 0x9:  // four colours, pattern resolution chosen by the colour ordering
                for (int i = 0; i < 4; i++) c[i] = s.byte();
                if (c[0] <= c[1]) {
                    if (c[2] <= c[3]) {  // 1x1 pixels, 16 bytes
                        u64 top = s.le64();
                        pattern2(dst, stride, 8, 4, 1, 1, c, top);
                        pattern2(dst + 4 * stride, stride, 8, 4, 1, 1, c, s.le64());
                    } else {  // 2x2 pixels, 4 bytes
                        pattern2(dst, stride, 4, 4, 2, 2, c, s.le32());
                    }
                } else {
                    if (c[2] <= c[3]) {  // 2x1 pixels (2 wide, 1 high), 8 bytes
                        pattern2(dst, stride, 4, 8, 2, 1, c, s.le64());
                    } else {  // 1x2 pixels (1 wide, 2 high), 8 bytes
                        pattern2(dst, stride, 8, 4, 1, 2, c, s.le64());
                    }
                }
                break;
            case 0xA:  // four colours per quadrant or per half
                for (int i = 0; i < 4; i++) c[i] = s.byte();
                if (c[0] <= c[1]) {
                    for (int q = 0; q < 4; q++) {
                        if (q)
                            for (int i = 0; i < 4; i++) c[i] = s.byte();
                        u8* qd = dst + (q & 1) * 4 * stride + (q >> 1) * 4;
                        pattern2(qd, stride, 4, 4, 1, 1, c, s.le32());
                    }
                } else {
                    u64 bitsA = s.le64();
                    for (int i = 4; i < 8; i++) c[i] = s.byte();
                    u64 bitsB = s.le64();
                    if (c[4] <= c[5]) {  // left / right halves
                        pattern2(dst, stride, 4, 8, 1, 1, c, bitsA);
                        pattern2(dst + 4, stride, 4, 8, 1, 1, c + 4, bitsB);
                    } else {  // top / bottom halves
                        pattern2(dst, stride, 8, 4, 1, 1, c, bitsA);
                        pattern2(dst + 4 * stride, stride, 8, 4, 1, 1, c + 4, bitsB);
                    }
                }
                break;
            case 0xB:  // raw 8x8 pixels
                for (int y = 0; y < 8; y++)
                    for (int x = 0; x < 8; x++) dst[y * stride + x] = s.byte();
                break;
            case 0xC:  // raw 4x4 grid of 2x2 pixels
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 4; x++) fillRect(dst + y * 2 * stride + x * 2, stride, 2, 2, s.byte());
                break;
            case 0xD:  // raw 2x2 grid of 4x4 pixels (top-left, top-right, bottom-left, bottom-right)
                for (int y = 0; y < 2; y++)
                    for (int x = 0; x < 2; x++) fillRect(dst + y * 4 * stride + x * 4, stride, 4, 4, s.byte());
                break;
            case 0xE:  // solid colour
                fillRect(dst, stride, 8, 8, s.byte());
                break;
            case 0xF:  // checkerboard of two colours, top-left pixel is the first
                c[0] = s.byte();
                c[1] = s.byte();
                for (int y = 0; y < 8; y++)
                    for (int x = 0; x < 8; x++) dst[y * stride + x] = c[(x + y) & 1];
                break;
            }
        }
    }
    if (s.underrun) warn(stats_.streamUnderruns, "block data ends early");
    else if (s.left() > 1) warn(stats_.streamLeftovers, "unused block data");  // one byte is padding to even size
}

void MveDecoder::warn(int& counter, const char* what)
{
    if (counter++ == 0) LOGW("MVE: %s (first at frame %d)", what, frameIndex_ + 1);
}

} // namespace fo
