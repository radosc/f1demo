#include "core/dat.h"

namespace fo {

bool lzssDecompress(const u8* src, size_t srcSize, Bytes& out, size_t expectedSize)
{
    out.clear();
    out.reserve(expectedSize);
    size_t pos = 0;
    u8 dict[4096];
    while (pos + 2 <= srcSize && out.size() < expectedSize) {
        i16 desc = rdBE16s(src + pos);
        pos += 2;
        if (desc == 0) break;
        if (desc < 0) {
            // stored block
            size_t len = (size_t)(-desc);
            if (pos + len > srcSize) len = srcSize - pos;
            out.insert(out.end(), src + pos, src + pos + len);
            pos += len;
            continue;
        }
        size_t end = pos + (size_t)desc;
        if (end > srcSize) end = srcSize;
        memset(dict, ' ', sizeof(dict));
        int dictPos = 4096 - 18;
        while (pos < end) {
            u8 flags = src[pos++];
            for (int bit = 0; bit < 8 && pos < end; bit++, flags >>= 1) {
                if (flags & 1) {
                    u8 b = src[pos++];
                    out.push_back(b);
                    dict[dictPos] = b;
                    dictPos = (dictPos + 1) & 4095;
                } else {
                    if (pos + 2 > end) { pos = end; break; }
                    int lo = src[pos], hi = src[pos + 1];
                    pos += 2;
                    int off = lo | ((hi & 0xF0) << 4);
                    int len = (hi & 0x0F) + 3;
                    for (int i = 0; i < len; i++) {
                        u8 b = dict[(off + i) & 4095];
                        out.push_back(b);
                        dict[dictPos] = b;
                        dictPos = (dictPos + 1) & 4095;
                    }
                }
            }
        }
    }
    if (out.size() > expectedSize) out.resize(expectedSize);
    return out.size() == expectedSize;
}

DatArchive::~DatArchive()
{
    if (m_file) fclose(m_file);
}

bool DatArchive::open(const std::string& filePath)
{
    m_file = _wfopen(utf8ToWide(filePath).c_str(), L"rb");
    if (!m_file) {
        LOGE("Cannot open DAT %s", filePath.c_str());
        return false;
    }
    // The directory sits at the start of the file; read a generous chunk then the rest on demand.
    fseek(m_file, 0, SEEK_END);
    long fileSize = ftell(m_file);
    fseek(m_file, 0, SEEK_SET);
    size_t headerChunk = (size_t)std::min<long>(fileSize, 4 * 1024 * 1024);
    Bytes head(headerChunk);
    if (fread(head.data(), 1, headerChunk, m_file) != headerChunk) return false;
    BeReader r(head);
    u32 dirCount = r.u32v();
    r.skip(12);
    std::vector<std::string> dirs;
    for (u32 i = 0; i < dirCount && !r.bad; i++) {
        u8 len = r.u8v();
        dirs.push_back(r.str(len));
    }
    for (u32 d = 0; d < dirCount && !r.bad; d++) {
        u32 fileCount = r.u32v();
        r.skip(12);
        std::string dir = dirs[d] == "." ? std::string() : dirs[d];
        for (u32 f = 0; f < fileCount && !r.bad; f++) {
            u8 len = r.u8v();
            std::string name = r.str(len);
            DatEntry e;
            e.attributes = r.u32v();
            e.offset = r.u32v();
            e.size = r.u32v();
            e.packedSize = r.u32v();
            e.path = pathNormalize(dir.empty() ? name : dir + "/" + name);
            m_index[e.path] = m_entries.size();
            m_entries.push_back(e);
        }
    }
    if (r.bad) {
        LOGE("DAT directory truncated: %s", filePath.c_str());
        return false;
    }
    LOGI("Opened DAT %s: %d directories, %d files", filePath.c_str(), (int)dirCount, (int)m_entries.size());
    return true;
}

const DatEntry* DatArchive::find(const std::string& path) const
{
    auto it = m_index.find(pathNormalize(path));
    return it == m_index.end() ? nullptr : &m_entries[it->second];
}

bool DatArchive::read(const DatEntry& e, Bytes& out)
{
    if (!m_file) return false;
    bool compressed = (e.attributes & 0x40) != 0;
    size_t toRead = compressed ? e.packedSize : e.size;
    Bytes raw(toRead);
    if (_fseeki64(m_file, e.offset, SEEK_SET) != 0) return false;
    if (toRead && fread(raw.data(), 1, toRead, m_file) != toRead) return false;
    if (!compressed) {
        out.swap(raw);
        return true;
    }
    if (!lzssDecompress(raw.data(), raw.size(), out, e.size)) {
        LOGW("LZSS size mismatch for %s", e.path.c_str());
    }
    return true;
}

bool DatArchive::read(const std::string& path, Bytes& out)
{
    const DatEntry* e = find(path);
    return e && read(*e, out);
}

} // namespace fo
