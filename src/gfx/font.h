// Fallout bitmap fonts (converted FON/AAF atlases) with HD variants.
#pragma once
#include "gfx/renderer.h"

namespace fo {

struct Glyph {
    int x = 0, y = 0;   // atlas position (1x)
    int advance = 0;    // pixel width
    int h = 0;          // glyph bitmap height
    int yoff = 0;       // offset from line top
};

class Font {
public:
    bool load(const std::string& name); // "font1_aaf"
    int height() const { return m_height; }
    int lineHeight() const { return m_height + m_lineSpacing; }
    int charWidth(unsigned char c) const;
    int textWidth(const std::string& s) const;
    // Draws at canvas pixel position (x, y) = top-left, `scale` = canvas texels per game pixel.
    void draw(const std::string& s, float x, float y, float scale, u32 argb, float alpha = 1.0f) const;
    std::vector<std::string> wrap(const std::string& s, int maxWidth) const;
    bool valid() const { return m_valid; }

private:
    bool m_valid = false;
    bool m_aaf = false;
    int m_height = 8, m_spacing = 1, m_wordSpacing = 4, m_lineSpacing = 0;
    int m_sheetW = 0, m_sheetH = 0;
    Glyph m_glyphs[256];
    TexturePtr m_tex1x;
    TexturePtr m_texHD;
    int m_hdScale = 0;
};

// Font ids as used by the original game/scripts: 0-9 = FON fonts, 100+ = AAF fonts.
Font* font(int id);
void fontsSetUseHD(bool on);

// Text flags for drawText helpers
enum TextFlags : u32 { TEXT_SHADOW = 1, TEXT_CENTER = 2, TEXT_RIGHT = 4 };

} // namespace fo
