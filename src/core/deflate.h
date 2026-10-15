// zlib (RFC 1950/1951) compression and decompression.
#pragma once
#include "core/base.h"

namespace fo {

// Decompresses a zlib stream (with header + adler32). Returns false on error.
bool zlibInflate(const u8* src, size_t size, Bytes& out, size_t sizeHint = 0);
// Raw deflate stream (no header).
bool rawInflate(const u8* src, size_t size, Bytes& out, size_t sizeHint = 0);
// Compresses into a zlib stream. level 0..9 (0 = stored blocks).
Bytes zlibDeflate(const u8* src, size_t size, int level = 6);

u32 crc32Update(u32 crc, const u8* data, size_t size);
u32 adler32(const u8* data, size_t size);

} // namespace fo
