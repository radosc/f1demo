#include "gfx/palette.h"

namespace fo {

Palette g_palette;

// Cycling colors are stored as 8-bit and converted to the palette's 6-bit precision (like the original).
static const u8 kSlime[12] = { 0, 108, 0, 11, 115, 7, 27, 123, 15, 43, 131, 27 };
static const u8 kShoreline[18] = { 83, 63, 43, 75, 59, 43, 67, 55, 39, 63, 51, 39, 55, 47, 35, 51, 43, 35 };
static const u8 kFireSlow[15] = { 255, 0, 0, 215, 0, 0, 147, 43, 11, 255, 119, 0, 255, 59, 0 };
static const u8 kFireFast[15] = { 71, 0, 0, 123, 0, 0, 179, 0, 0, 123, 0, 0, 71, 0, 0 };
static const u8 kMonitors[15] = { 107, 107, 111, 99, 103, 127, 87, 107, 143, 0, 147, 163, 107, 187, 255 };

static u32 from6(int r, int g, int b)
{
    auto up = [](int v) { v &= 63; return (u32)((v << 2) | (v >> 4)); };
    return 0xFF000000u | (up(r) << 16) | (up(g) << 8) | up(b);
}

static u32 from8via6(const u8* rgb) { return from6(rgb[0] >> 2, rgb[1] >> 2, rgb[2] >> 2); }

bool Palette::load(const Bytes& pal)
{
    if (pal.size() < 768) return false;
    for (int i = 0; i < 256; i++) m_base[i] = from6(pal[i * 3], pal[i * 3 + 1], pal[i * 3 + 2]);
    m_base[0] &= 0x00FFFFFFu; // index 0 is the transparent color key
    memcpy(m_current, m_base, sizeof(m_base));
    resetCycling();
    buildOutput();
    return true;
}

void Palette::setBrightness(float b)
{
    b = clampv(b, 0.25f, 2.0f);
    if (b == m_brightness) return;
    m_brightness = b;
    m_dirty = true;
}

void Palette::buildOutput()
{
    m_dirty = false;
    if (m_brightness == 1.0f) {
        memcpy(m_out, m_current, sizeof(m_out));
        return;
    }
    float g = 1.0f / m_brightness;
    for (int i = 0; i < 256; i++) {
        u32 c = m_current[i];
        u32 out = c & 0xFF000000u;
        for (int k = 0; k < 3; k++) {
            float v = ((c >> (k * 8)) & 0xFF) / 255.0f;
            int o = (int)(powf(v, g) * 255.0f + 0.5f);
            out |= (u32)clampv(o, 0, 255) << (k * 8);
        }
        m_out[i] = out;
    }
}

void Palette::resetCycling()
{
    m_lastSlow = m_lastMedium = m_lastFast = m_lastVeryFast = 0;
    m_slimeStart = m_shoreStart = m_fireSlowStart = m_fireFastStart = m_monitorStart = 0;
    m_bobberRed = 0;
    m_bobberDiff = -4;
}

static void rotateInto(u32* dst, const u8* table, int count, int start)
{
    int o = 0;
    for (int i = start; i < count * 3; i += 3) dst[o++] = from8via6(table + i);
    for (int i = 0; i < start; i += 3) dst[o++] = from8via6(table + i);
}

bool Palette::animate(u32 now)
{
    bool changed = m_dirty;
    if (!m_cycleEnabled) {
        if (changed) buildOutput();
        return changed;
    }
    if (now - m_lastSlow >= 200) {
        m_lastSlow = now;
        changed = true;
        rotateInto(m_current + 229, kSlime, 4, m_slimeStart);
        m_slimeStart -= 3;
        if (m_slimeStart < 0) m_slimeStart = 9;
        rotateInto(m_current + 248, kShoreline, 6, m_shoreStart);
        m_shoreStart -= 3;
        if (m_shoreStart < 0) m_shoreStart = 15;
        rotateInto(m_current + 238, kFireSlow, 5, m_fireSlowStart);
        m_fireSlowStart -= 3;
        if (m_fireSlowStart < 0) m_fireSlowStart = 12;
    }
    if (now - m_lastMedium >= 142) {
        m_lastMedium = now;
        changed = true;
        rotateInto(m_current + 243, kFireFast, 5, m_fireFastStart);
        m_fireFastStart -= 3;
        if (m_fireFastStart < 0) m_fireFastStart = 12;
    }
    if (now - m_lastFast >= 100) {
        m_lastFast = now;
        changed = true;
        rotateInto(m_current + 233, kMonitors, 5, m_monitorStart);
        m_monitorStart -= 3;
        if (m_monitorStart < 0) m_monitorStart = 12;
    }
    if (now - m_lastVeryFast >= 33) {
        m_lastVeryFast = now;
        changed = true;
        if (m_bobberRed == 0 || m_bobberRed == 60) m_bobberDiff = -m_bobberDiff;
        m_bobberRed += m_bobberDiff;
        m_current[254] = from6(m_bobberRed, 0, 0);
    }
    if (changed) buildOutput();
    return changed;
}

int Palette::nearest(int r, int g, int b) const
{
    int best = 1, bestD = INT32_MAX;
    for (int i = 1; i < 229; i++) {
        u32 c = m_base[i];
        int dr = (int)((c >> 16) & 255) - r, dg = (int)((c >> 8) & 255) - g, db = (int)(c & 255) - b;
        int d = dr * dr * 3 + dg * dg * 4 + db * db * 2;
        if (d < bestD) {
            bestD = d;
            best = i;
        }
    }
    return best;
}

} // namespace fo
