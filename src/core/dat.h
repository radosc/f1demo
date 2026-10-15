// Fallout 1 DAT archive reader (big-endian directory, LZSS-compressed entries).
#pragma once
#include "core/base.h"

namespace fo {

struct DatEntry {
    std::string path;   // normalized virtual path ("art/critters/hmjmpsaa.frm")
    u32 attributes = 0; // 0x40 = LZSS compressed, 0x20 = stored
    u32 offset = 0;
    u32 size = 0;       // unpacked size
    u32 packedSize = 0;
};

class DatArchive {
public:
    bool open(const std::string& filePath);
    bool isOpen() const { return m_file != nullptr; }
    ~DatArchive();

    const DatEntry* find(const std::string& path) const;
    bool read(const DatEntry& e, Bytes& out);
    bool read(const std::string& path, Bytes& out);
    const std::vector<DatEntry>& entries() const { return m_entries; }

private:
    FILE* m_file = nullptr;
    std::vector<DatEntry> m_entries;
    std::unordered_map<std::string, size_t> m_index;
};

// Fallout 1 LZSS (block based, 4K ring buffer). Returns false on malformed data.
bool lzssDecompress(const u8* src, size_t srcSize, Bytes& out, size_t expectedSize);

} // namespace fo
