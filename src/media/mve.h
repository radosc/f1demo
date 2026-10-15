// Interplay MVE movie decoder: 8-bit paletted video (opcode 0x11 block codec) plus PCM / Interplay DPCM audio.
// Self-contained; depends only on core/base.h.
//
// Playback: show frame i at i * frameDuration() after the first frame. Audio sample 0 belongs to frame 0, but
// files decode audio about a second ahead of the video (prebuffer chunks), so queue everything takeAudio()
// returns after each nextFrame() and start the audio voice when the first frame is shown. A file may end with
// up to ~1 s of trailing silence after the last frame.
#pragma once
#include "core/base.h"

namespace fo {

// Diagnostic counters (all zero for a well-formed file).
struct MveStats {
    int framesDecoded = 0;    // video data opcodes decoded
    int streamUnderruns = 0;  // frames whose block data ended before all blocks were decoded
    int streamLeftovers = 0;  // frames with more than one unused byte after the last block (one byte is padding)
    int badVectors = 0;       // motion vectors pointing outside the frame buffer (block left unchanged)
    int unsupported = 0;      // opcodes this decoder cannot handle (16-bit video, video formats 0x06/0x10)
};

class MveDecoder {
public:
    MveDecoder();

    // Takes ownership of the file bytes. Checks the header and pre-scans the chunk structure, so the video size,
    // frame rate, frame count and audio format are known right after open(). Returns false (reason in *error) for
    // non-MVE data, files without a valid video stream, 16-bit (true colour) video and the 0x06/0x10 video formats.
    bool open(Bytes data, std::string* error = nullptr);
    void close();
    bool isOpen() const { return !data_.empty(); }
    // Restarts decoding from the first frame (buffers, palette and pending audio are reset).
    void rewind();

    int width() const { return width_; }    // video size in pixels
    int height() const { return height_; }
    // Seconds per frame from the most recent timer opcode (rate * subdivision microseconds); 1/15 s if absent.
    double frameDuration() const;
    int frameCount() const { return frameCount_; }  // displayed frames in the file (from the pre-scan)
    int frameIndex() const { return frameIndex_; }  // index of the frame returned by the last nextFrame(), -1 before

    // Decodes until the next "send buffer to display" opcode. Returns false at the end of the stream.
    // After it returns true, frame() holds width*height palette indices (row-major, stride = width) and palette()
    // 256 colours as 0xFFRRGGBB (6-bit VGA components expanded to 8 bits). paletteChanged() reports whether the
    // palette changed since the previous frame (always true for the first frame). frame() is the decoder's own
    // buffer: it stays valid until the next nextFrame(), rewind(), close() or open().
    bool nextFrame();
    const std::vector<u8>& frame() const { return buf_[cur_]; }
    const u32* palette() const { return palette_; }
    bool paletteChanged() const { return paletteChanged_; }

    // Audio of stream 0, decoded while frames are decoded: interleaved signed 16-bit PCM regardless of the source
    // format (8-bit PCM is expanded, DPCM is decompressed). audioChannels() is 0 when the file has no audio.
    bool hasAudio() const { return audioChannels_ > 0; }
    int audioRate() const { return audioRate_; }
    int audioChannels() const { return audioChannels_; }
    int audioSourceBits() const { return audio16_ ? 16 : 8; }   // informational
    bool audioCompressed() const { return audioCompressed_; }   // informational (Interplay DPCM)
    std::vector<i16> takeAudio();                               // drains the PCM produced so far

    const MveStats& stats() const { return stats_; }

private:
    void resetState();
    bool enterNextChunk();
    void parseAudioInit(int version, const u8* p, size_t n);
    void initVideo(int blocksW, int blocksH);
    void decodeAudio(const u8* p, size_t n, bool silence);
    void decodeVideo(const u8* p, size_t n);
    void setPaletteEntry(int index, u8 r, u8 g, u8 b);
    void copyBlock(u8* dst, const u8* srcBuf, ptrdiff_t srcOff);
    void warn(int& counter, const char* what);  // counts an anomaly, logs its first occurrence

    Bytes data_;
    size_t chunkPos_ = 0;  // offset of the next chunk header
    size_t opPos_ = 0;     // offset of the next opcode inside the current chunk
    size_t chunkEnd_ = 0;  // end of the current chunk
    bool ended_ = true;

    // values from the pre-scan, restored by rewind()
    int scanBlocksW_ = 0, scanBlocksH_ = 0;
    u64 scanTimerUs_ = 0;

    int width_ = 0, height_ = 0;
    int blocksW_ = 0, blocksH_ = 0;
    std::vector<u8> buf_[2];  // buf_[cur_]: frame being built / displayed; buf_[cur_ ^ 1]: previous frame
    int cur_ = 0;
    std::vector<u8> map_;     // decoding map (one nibble per 8x8 block, low nibble first)
    u32 palette_[256];
    bool paletteDirty_ = true;
    bool paletteChanged_ = false;
    u64 timerUs_ = 0;
    int frameCount_ = 0;
    int frameIndex_ = -1;

    int audioRate_ = 0, audioChannels_ = 0;
    bool audio16_ = false, audioCompressed_ = false;
    std::vector<i16> audio_;

    MveStats stats_;
};

} // namespace fo
