#include "core/deflate.h"

namespace fo {

// ------------------------------------------------------------------ checksums
static u32 g_crcTable[256];
static bool g_crcInit = false;

u32 crc32Update(u32 crc, const u8* data, size_t size)
{
    if (!g_crcInit) {
        for (u32 n = 0; n < 256; n++) {
            u32 c = n;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            g_crcTable[n] = c;
        }
        g_crcInit = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < size; i++) crc = g_crcTable[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

u32 adler32(const u8* data, size_t size)
{
    u32 a = 1, b = 0;
    while (size > 0) {
        size_t n = std::min<size_t>(size, 5552);
        size -= n;
        while (n--) {
            a += *data++;
            b += a;
        }
        a %= 65521;
        b %= 65521;
    }
    return (b << 16) | a;
}

// ------------------------------------------------------------------ tables
static const u16 kLenBase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const u8 kLenExtra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const u16 kDistBase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const u8 kDistExtra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
static const u8 kClOrder[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };

static u32 reverseBits(u32 v, int n)
{
    u32 r = 0;
    for (int i = 0; i < n; i++) {
        r = (r << 1) | (v & 1);
        v >>= 1;
    }
    return r;
}

// ------------------------------------------------------------------ inflate
namespace {

struct BitReader {
    const u8* p;
    const u8* end;
    u64 buf = 0;
    int cnt = 0;
    int overrun = 0;

    void refill()
    {
        while (cnt <= 56) {
            u64 b;
            if (p < end) b = *p++;
            else { b = 0; overrun++; }
            buf |= b << cnt;
            cnt += 8;
        }
    }
    u32 peek(int n)
    {
        if (cnt < n) refill();
        return (u32)(buf & ((1ull << n) - 1));
    }
    void consume(int n)
    {
        buf >>= n;
        cnt -= n;
    }
    u32 get(int n)
    {
        if (n == 0) return 0;
        u32 v = peek(n);
        consume(n);
        return v;
    }
    bool bad() const { return overrun > 8; }
};

struct Huffman {
    enum { FAST = 10 };
    u16 fast[1 << FAST];
    u16 count[16];
    u16 symbol[320];

    bool build(const u8* lengths, int n)
    {
        memset(count, 0, sizeof(count));
        for (int i = 0; i < n; i++) count[lengths[i]]++;
        count[0] = 0;
        int left = 1;
        for (int len = 1; len <= 15; len++) {
            left <<= 1;
            left -= count[len];
            if (left < 0) return false; // over-subscribed
        }
        u16 offs[16];
        offs[1] = 0;
        for (int len = 1; len < 15; len++) offs[len + 1] = (u16)(offs[len] + count[len]);
        for (int i = 0; i < n; i++)
            if (lengths[i]) symbol[offs[lengths[i]]++] = (u16)i;
        // canonical codes -> fast table
        memset(fast, 0, sizeof(fast));
        int nextCode[16];
        int code = 0;
        nextCode[0] = 0;
        for (int len = 1; len <= 15; len++) {
            code = (code + count[len - 1]) << 1;
            nextCode[len] = code;
        }
        for (int i = 0; i < n; i++) {
            int len = lengths[i];
            if (!len) continue;
            int c = nextCode[len]++;
            if (len <= FAST) {
                u32 rev = reverseBits((u32)c, len);
                for (u32 j = rev; j < (1u << FAST); j += (1u << len)) fast[j] = (u16)((len << 9) | i);
            }
        }
        return true;
    }

    int decode(BitReader& br) const
    {
        u32 bits = br.peek(16);
        u16 f = fast[bits & ((1 << FAST) - 1)];
        if (f) {
            br.consume(f >> 9);
            return f & 511;
        }
        // slow canonical decode (bit by bit)
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= 15; len++) {
            code |= (int)br.get(1);
            int c = count[len];
            if (code - c < first) return symbol[index + (code - first)];
            index += c;
            first += c;
            first <<= 1;
            code <<= 1;
        }
        return -1;
    }
};

bool inflateBlocks(BitReader& br, Bytes& out)
{
    static Huffman fixedLit, fixedDist;
    static bool fixedBuilt = false;
    if (!fixedBuilt) {
        u8 l[288];
        for (int i = 0; i < 144; i++) l[i] = 8;
        for (int i = 144; i < 256; i++) l[i] = 9;
        for (int i = 256; i < 280; i++) l[i] = 7;
        for (int i = 280; i < 288; i++) l[i] = 8;
        fixedLit.build(l, 288);
        u8 d[30];
        for (int i = 0; i < 30; i++) d[i] = 5;
        fixedDist.build(d, 30);
        fixedBuilt = true;
    }
    Huffman dynLit, dynDist;
    bool last = false;
    while (!last) {
        last = br.get(1) != 0;
        u32 type = br.get(2);
        if (type == 0) {
            // stored: align
            br.consume(br.cnt & 7);
            u32 len = br.get(16);
            u32 nlen = br.get(16);
            if ((len ^ 0xFFFF) != nlen) return false;
            while (len > 0 && br.cnt >= 8) {
                out.push_back((u8)br.get(8));
                len--;
            }
            if ((size_t)(br.end - br.p) < len) return false;
            out.insert(out.end(), br.p, br.p + len);
            br.p += len;
            continue;
        }
        const Huffman* lit;
        const Huffman* dist;
        if (type == 1) {
            lit = &fixedLit;
            dist = &fixedDist;
        } else if (type == 2) {
            int hlit = (int)br.get(5) + 257;
            int hdist = (int)br.get(5) + 1;
            int hclen = (int)br.get(4) + 4;
            u8 clLens[19] = {};
            for (int i = 0; i < hclen; i++) clLens[kClOrder[i]] = (u8)br.get(3);
            Huffman cl;
            if (!cl.build(clLens, 19)) return false;
            u8 lens[320] = {};
            int n = 0;
            while (n < hlit + hdist) {
                int sym = cl.decode(br);
                if (sym < 0) return false;
                if (sym < 16) {
                    lens[n++] = (u8)sym;
                } else {
                    int rep = 0;
                    u8 val = 0;
                    if (sym == 16) {
                        if (n == 0) return false;
                        val = lens[n - 1];
                        rep = 3 + (int)br.get(2);
                    } else if (sym == 17) {
                        rep = 3 + (int)br.get(3);
                    } else {
                        rep = 11 + (int)br.get(7);
                    }
                    if (n + rep > hlit + hdist) return false;
                    while (rep--) lens[n++] = val;
                }
                if (br.bad()) return false;
            }
            if (!dynLit.build(lens, hlit)) return false;
            if (!dynDist.build(lens + hlit, hdist)) return false;
            lit = &dynLit;
            dist = &dynDist;
        } else {
            return false;
        }
        for (;;) {
            int sym = lit->decode(br);
            if (sym < 0 || br.bad()) return false;
            if (sym < 256) {
                out.push_back((u8)sym);
            } else if (sym == 256) {
                break;
            } else {
                sym -= 257;
                if (sym >= 29) return false;
                int len = kLenBase[sym] + (int)br.get(kLenExtra[sym]);
                int ds = dist->decode(br);
                if (ds < 0 || ds >= 30) return false;
                size_t d = kDistBase[ds] + br.get(kDistExtra[ds]);
                if (d > out.size()) return false;
                size_t from = out.size() - d;
                for (int i = 0; i < len; i++) out.push_back(out[from + i]);
            }
        }
        if (br.bad()) return false;
    }
    return true;
}

} // namespace

bool rawInflate(const u8* src, size_t size, Bytes& out, size_t sizeHint)
{
    out.clear();
    if (sizeHint) out.reserve(sizeHint);
    BitReader br{ src, src + size };
    return inflateBlocks(br, out);
}

bool zlibInflate(const u8* src, size_t size, Bytes& out, size_t sizeHint)
{
    if (size < 6) return false;
    u8 cmf = src[0], flg = src[1];
    if ((cmf & 0x0F) != 8 || ((cmf << 8) | flg) % 31 != 0) return false;
    if (flg & 0x20) return false; // preset dictionary unsupported
    out.clear();
    if (sizeHint) out.reserve(sizeHint);
    BitReader br{ src + 2, src + size };
    if (!inflateBlocks(br, out)) return false;
    return true; // adler32 not verified (PNG CRCs already guard the data)
}

// ------------------------------------------------------------------ deflate
namespace {

struct BitWriter {
    Bytes& out;
    u64 buf = 0;
    int cnt = 0;
    explicit BitWriter(Bytes& o) : out(o) {}
    void put(u32 bits, int n)
    {
        buf |= (u64)bits << cnt;
        cnt += n;
        while (cnt >= 8) {
            out.push_back((u8)buf);
            buf >>= 8;
            cnt -= 8;
        }
    }
    void align()
    {
        if (cnt > 0) {
            out.push_back((u8)buf);
            buf = 0;
            cnt = 0;
        }
    }
};

// Builds length-limited Huffman code lengths for `n` symbols.
void buildLengths(const u32* freqIn, int n, int maxLen, u8* lens)
{
    std::vector<u32> freq(freqIn, freqIn + n);
    for (;;) {
        struct Node {
            u32 f;
            int left, right; // -1 for leaves
            int sym;
        };
        std::vector<Node> nodes;
        std::vector<int> heap;
        auto cmp = [&](int a, int b) { return nodes[a].f > nodes[b].f || (nodes[a].f == nodes[b].f && a > b); };
        for (int i = 0; i < n; i++) {
            if (freq[i]) {
                nodes.push_back({ freq[i], -1, -1, i });
                heap.push_back((int)nodes.size() - 1);
            }
        }
        memset(lens, 0, (size_t)n);
        if (heap.empty()) return;
        if (heap.size() == 1) {
            lens[nodes[heap[0]].sym] = 1;
            return;
        }
        std::make_heap(heap.begin(), heap.end(), cmp);
        while (heap.size() > 1) {
            std::pop_heap(heap.begin(), heap.end(), cmp);
            int a = heap.back();
            heap.pop_back();
            std::pop_heap(heap.begin(), heap.end(), cmp);
            int b = heap.back();
            heap.pop_back();
            nodes.push_back({ nodes[a].f + nodes[b].f, a, b, -1 });
            heap.push_back((int)nodes.size() - 1);
            std::push_heap(heap.begin(), heap.end(), cmp);
        }
        // depth-first assign lengths
        int maxSeen = 0;
        std::vector<std::pair<int, int>> stack;
        stack.push_back({ heap[0], 0 });
        while (!stack.empty()) {
            auto [idx, depth] = stack.back();
            stack.pop_back();
            const Node& nd = nodes[idx];
            if (nd.left < 0) {
                lens[nd.sym] = (u8)std::max(1, depth);
                maxSeen = std::max(maxSeen, depth);
            } else {
                stack.push_back({ nd.left, depth + 1 });
                stack.push_back({ nd.right, depth + 1 });
            }
        }
        if (maxSeen <= maxLen) return;
        for (auto& f : freq)
            if (f) f = (f + 1) >> 1;
    }
}

void codesFromLengths(const u8* lens, int n, u16* codes)
{
    int count[16] = {};
    for (int i = 0; i < n; i++) count[lens[i]]++;
    count[0] = 0;
    int nextCode[16];
    int code = 0;
    nextCode[0] = 0;
    for (int len = 1; len <= 15; len++) {
        code = (code + count[len - 1]) << 1;
        nextCode[len] = code;
    }
    for (int i = 0; i < n; i++) {
        codes[i] = lens[i] ? (u16)reverseBits((u32)nextCode[lens[i]]++, lens[i]) : 0;
    }
}

struct Sym {
    u16 litlen; // 0..255 literal, 257.. length code; for matches holds length code
    u16 extraLen;
    u16 dist;   // 0 for literals, else distance code index + 1
    u16 extraDist;
};

int lengthCode(int len, int& extra, int& extraBits)
{
    for (int i = 28; i >= 0; i--) {
        if (len >= kLenBase[i]) {
            extra = len - kLenBase[i];
            extraBits = kLenExtra[i];
            return 257 + i;
        }
    }
    extra = 0;
    extraBits = 0;
    return 257;
}

int distCode(int d, int& extra, int& extraBits)
{
    for (int i = 29; i >= 0; i--) {
        if (d >= kDistBase[i]) {
            extra = d - kDistBase[i];
            extraBits = kDistExtra[i];
            return i;
        }
    }
    extra = 0;
    extraBits = 0;
    return 0;
}

void writeBlock(BitWriter& bw, const std::vector<Sym>& syms, const u8* raw, size_t rawLen, bool last)
{
    u32 litFreq[286] = {};
    u32 distFreq[30] = {};
    for (auto& s : syms) {
        litFreq[s.litlen]++;
        if (s.dist) distFreq[s.dist - 1]++;
    }
    litFreq[256]++;
    // make sure trees are complete (>= 2 codes)
    int litUsed = 0, distUsed = 0;
    for (int i = 0; i < 286; i++) litUsed += litFreq[i] != 0;
    for (int i = 0; i < 30; i++) distUsed += distFreq[i] != 0;
    if (litUsed < 2) litFreq[litFreq[0] ? 1 : 0]++;
    if (distUsed < 2) {
        if (!distFreq[0]) distFreq[0] = 1;
        if (!distFreq[1]) distFreq[1] = 1;
    }
    u8 litLens[286], distLens[30];
    buildLengths(litFreq, 286, 15, litLens);
    buildLengths(distFreq, 30, 15, distLens);
    int hlit = 286;
    while (hlit > 257 && litLens[hlit - 1] == 0) hlit--;
    int hdist = 30;
    while (hdist > 1 && distLens[hdist - 1] == 0) hdist--;

    // run-length encode the code lengths
    std::vector<u8> all;
    all.insert(all.end(), litLens, litLens + hlit);
    all.insert(all.end(), distLens, distLens + hdist);
    struct ClSym { u8 sym, extra; };
    std::vector<ClSym> cls;
    u32 clFreq[19] = {};
    for (size_t i = 0; i < all.size();) {
        u8 v = all[i];
        size_t run = 1;
        while (i + run < all.size() && all[i + run] == v) run++;
        if (v == 0 && run >= 3) {
            size_t r = std::min<size_t>(run, 138);
            if (r >= 11) cls.push_back({ 18, (u8)(r - 11) });
            else cls.push_back({ 17, (u8)(r - 3) });
            clFreq[cls.back().sym]++;
            i += r;
        } else if (v != 0 && run >= 4) {
            cls.push_back({ v, 0 });
            clFreq[v]++;
            size_t r = std::min<size_t>(run - 1, 6);
            cls.push_back({ 16, (u8)(r - 3) });
            clFreq[16]++;
            i += 1 + r;
        } else {
            cls.push_back({ v, 0 });
            clFreq[v]++;
            i++;
        }
    }
    u8 clLens[19];
    buildLengths(clFreq, 19, 7, clLens);
    int hclen = 19;
    while (hclen > 4 && clLens[kClOrder[hclen - 1]] == 0) hclen--;

    // cost estimate: dynamic vs stored
    u64 dynBits = 17 + 3 * (u64)hclen;
    for (auto& c : cls) dynBits += clLens[c.sym] + (c.sym == 16 ? 2 : c.sym == 17 ? 3 : c.sym == 18 ? 7 : 0);
    for (int i = 0; i < 286; i++) dynBits += (u64)litFreq[i] * litLens[i];
    for (auto& s : syms) {
        if (s.dist) dynBits += kLenExtra[s.litlen - 257] + distLens[s.dist - 1] + kDistExtra[s.dist - 1];
    }
    u64 storedBits = (rawLen + 5 * (rawLen / 65535 + 1)) * 8 + 8;
    if (raw && storedBits < dynBits) {
        size_t pos = 0;
        do {
            size_t n = std::min<size_t>(rawLen - pos, 65535);
            bool lastChunk = pos + n >= rawLen;
            bw.put(last && lastChunk ? 1 : 0, 1);
            bw.put(0, 2);
            bw.align();
            bw.out.push_back((u8)n);
            bw.out.push_back((u8)(n >> 8));
            bw.out.push_back((u8)~n);
            bw.out.push_back((u8)(~n >> 8));
            bw.out.insert(bw.out.end(), raw + pos, raw + pos + n);
            pos += n;
        } while (pos < rawLen);
        return;
    }

    u16 litCodes[286], distCodes[30], clCodes[19];
    codesFromLengths(litLens, 286, litCodes);
    codesFromLengths(distLens, 30, distCodes);
    codesFromLengths(clLens, 19, clCodes);
    bw.put(last ? 1 : 0, 1);
    bw.put(2, 2);
    bw.put((u32)(hlit - 257), 5);
    bw.put((u32)(hdist - 1), 5);
    bw.put((u32)(hclen - 4), 4);
    for (int i = 0; i < hclen; i++) bw.put(clLens[kClOrder[i]], 3);
    for (auto& c : cls) {
        bw.put(clCodes[c.sym], clLens[c.sym]);
        if (c.sym == 16) bw.put(c.extra, 2);
        else if (c.sym == 17) bw.put(c.extra, 3);
        else if (c.sym == 18) bw.put(c.extra, 7);
    }
    for (auto& s : syms) {
        bw.put(litCodes[s.litlen], litLens[s.litlen]);
        if (s.dist) {
            int lc = s.litlen - 257;
            if (kLenExtra[lc]) bw.put(s.extraLen, kLenExtra[lc]);
            int dc = s.dist - 1;
            bw.put(distCodes[dc], distLens[dc]);
            if (kDistExtra[dc]) bw.put(s.extraDist, kDistExtra[dc]);
        }
    }
    bw.put(litCodes[256], litLens[256]);
}

} // namespace

Bytes zlibDeflate(const u8* src, size_t size, int level)
{
    Bytes out;
    out.reserve(size / 2 + 64);
    out.push_back(0x78);
    out.push_back(level >= 6 ? 0xDA : 0x01);
    BitWriter bw(out);

    if (level <= 0) {
        size_t pos = 0;
        do {
            size_t n = std::min<size_t>(size - pos, 65535);
            bool last = pos + n >= size;
            bw.put(last ? 1 : 0, 1);
            bw.put(0, 2);
            bw.align();
            out.push_back((u8)n);
            out.push_back((u8)(n >> 8));
            out.push_back((u8)~n);
            out.push_back((u8)(~n >> 8));
            out.insert(out.end(), src + pos, src + pos + n);
            pos += n;
        } while (pos < size);
    } else {
        const int kWindow = 32768;
        const int kHashBits = 15;
        const int maxChain = level >= 8 ? 256 : level >= 5 ? 64 : 16;
        std::vector<int> head(1 << kHashBits, -1);
        std::vector<int> prev(kWindow, -1);
        auto hash3 = [&](size_t i) -> u32 {
            return ((u32)src[i] << 10 ^ (u32)src[i + 1] << 5 ^ (u32)src[i + 2]) & ((1u << kHashBits) - 1);
        };
        auto insert = [&](size_t i) {
            if (i + 2 >= size) return;
            u32 h = hash3(i);
            prev[i & (kWindow - 1)] = head[h];
            head[h] = (int)i;
        };
        auto findMatch = [&](size_t i, int& bestDist) -> int {
            int bestLen = 0;
            if (i + 2 >= size) return 0;
            int cand = head[hash3(i)];
            int chain = maxChain;
            size_t maxLen = std::min<size_t>(258, size - i);
            while (cand >= 0 && chain-- > 0) {
                size_t d = i - (size_t)cand;
                if (d == 0 || d > (size_t)kWindow - 262) break;
                const u8* a = src + cand;
                const u8* b = src + i;
                if (a[bestLen] == b[bestLen] && a[0] == b[0]) {
                    size_t l = 0;
                    while (l < maxLen && a[l] == b[l]) l++;
                    if ((int)l > bestLen) {
                        bestLen = (int)l;
                        bestDist = (int)d;
                        if (l == maxLen) break;
                    }
                }
                int next = prev[cand & (kWindow - 1)];
                if (next >= cand) break;
                cand = next;
            }
            return bestLen >= 3 ? bestLen : 0;
        };

        std::vector<Sym> syms;
        syms.reserve(32768);
        size_t blockStart = 0;
        size_t i = 0;
        while (i < size) {
            int dist = 0;
            int len = findMatch(i, dist);
            if (len && level >= 4 && i + 1 < size) {
                // lazy evaluation: prefer a longer match at the next byte
                insert(i);
                int dist2 = 0;
                int len2 = findMatch(i + 1, dist2);
                if (len2 > len + 1) {
                    syms.push_back({ src[i], 0, 0, 0 });
                    i++;
                    len = len2;
                    dist = dist2;
                    insert(i);
                }
                int e1, eb1, e2, eb2;
                int lc = lengthCode(len, e1, eb1);
                int dc = distCode(dist, e2, eb2);
                syms.push_back({ (u16)lc, (u16)e1, (u16)(dc + 1), (u16)e2 });
                size_t endPos = i + (size_t)len;
                for (size_t k = i + 1; k < endPos; k++) insert(k);
                i = endPos;
            } else if (len) {
                int e1, eb1, e2, eb2;
                int lc = lengthCode(len, e1, eb1);
                int dc = distCode(dist, e2, eb2);
                syms.push_back({ (u16)lc, (u16)e1, (u16)(dc + 1), (u16)e2 });
                for (size_t k = i; k < i + (size_t)len; k++) insert(k);
                i += (size_t)len;
            } else {
                insert(i);
                syms.push_back({ src[i], 0, 0, 0 });
                i++;
            }
            if (syms.size() >= 32000) {
                writeBlock(bw, syms, src + blockStart, i - blockStart, false);
                syms.clear();
                blockStart = i;
            }
        }
        writeBlock(bw, syms, src + blockStart, i - blockStart, true);
    }
    bw.align();
    u32 ad = adler32(src, size);
    out.push_back((u8)(ad >> 24));
    out.push_back((u8)(ad >> 16));
    out.push_back((u8)(ad >> 8));
    out.push_back((u8)ad);
    return out;
}

} // namespace fo
