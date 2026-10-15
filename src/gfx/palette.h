// Fallout 256-color palette with animated color cycling ranges.
#pragma once
#include "core/base.h"

namespace fo {

class Palette {
public:
    // Loads COLOR.PAL (768 bytes of 6-bit RGB, optional 32K inverse table).
    bool load(const Bytes& palFile);
    // Static color as 0xAARRGGBB (index 0 is transparent).
    u32 argb(int index) const { return m_base[index & 255]; }
    const u32* base() const { return m_base; }
    // Current animated palette with the brightness applied (call `animate` once per frame).
    const u32* current() const { return m_out; }
    // Gamma-like brightness (1 = original); takes effect on the next `animate`.
    void setBrightness(float b);
    float brightness() const { return m_brightness; }
    // Advances color cycling. Returns true if any entry changed.
    bool animate(u32 nowMs);
    void resetCycling();
    void setCyclingEnabled(bool on) { m_cycleEnabled = on; }
    // Nearest palette index (excluding 0 and cycling range) for an RGB color.
    int nearest(int r, int g, int b) const;
    // True for indices that are animated by the cycler.
    static bool isCycling(int index) { return index >= 229 && index <= 254; }

private:
    u32 m_base[256] = {};
    u32 m_current[256] = {};
    u32 m_out[256] = {};
    float m_brightness = 1.0f;
    bool m_dirty = true;
    void buildOutput();
    bool m_cycleEnabled = true;
    u32 m_lastSlow = 0, m_lastMedium = 0, m_lastFast = 0, m_lastVeryFast = 0;
    int m_slimeStart = 0, m_shoreStart = 0, m_fireSlowStart = 0, m_fireFastStart = 0, m_monitorStart = 0;
    int m_bobberRed = 0, m_bobberDiff = -4;
};

extern Palette g_palette;

} // namespace fo
