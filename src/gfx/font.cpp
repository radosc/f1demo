#include "gfx/font.h"
#include "gfx/sprite.h"
#include "core/fs.h"
#include "core/json.h"

namespace fo {

static bool g_fontsHD = true;
static std::map<int, std::unique_ptr<Font>> g_fonts;

void fontsSetUseHD(bool on) { g_fontsHD = on; }

bool Font::load(const std::string& name)
{
    std::string text;
    if (!g_vfs.readText("fonts/" + name + ".json", text)) return false;
    Json j = Json::parse(text);
    m_aaf = j["type"].str() == "aaf";
    m_height = j["height"].toInt(8);
    m_spacing = j["spacing"].toInt(1);
    m_wordSpacing = j["wordSpacing"].toInt(4);
    m_lineSpacing = j["lineSpacing"].toInt(0);
    m_sheetW = j["sheetSize"][(size_t)0].toInt();
    m_sheetH = j["sheetSize"][(size_t)1].toInt();
    const Json& g = j["glyphs"];
    for (int i = 0; i < 256 && i < (int)g.size(); i++) {
        m_glyphs[i].x = g[(size_t)i][(size_t)0].toInt();
        m_glyphs[i].y = g[(size_t)i][(size_t)1].toInt();
        m_glyphs[i].advance = g[(size_t)i][(size_t)2].toInt();
        m_glyphs[i].h = g[(size_t)i][(size_t)3].toInt();
        m_glyphs[i].yoff = g[(size_t)i][(size_t)4].toInt();
    }
    Image img;
    if (!loadPng("fonts/" + name + ".png", img)) return false;
    m_tex1x = g_renderer.createTexture(img, true);
    Image hd;
    if (loadPng("fonts/hd/" + name + ".png", hd) && m_sheetW > 0 && hd.width % m_sheetW == 0) {
        m_hdScale = hd.width / m_sheetW;
        m_texHD = g_renderer.createTexture(hd, true);
    }
    m_valid = m_tex1x != nullptr;
    return m_valid;
}

int Font::charWidth(unsigned char c) const
{
    if (c == ' ') return m_wordSpacing;
    return m_glyphs[c].advance;
}

int Font::textWidth(const std::string& s) const
{
    int w = 0;
    for (unsigned char c : s) w += charWidth(c) + m_spacing;
    if (!s.empty()) w -= m_spacing;
    return w;
}

void Font::draw(const std::string& s, float x, float y, float scale, u32 argb, float alpha) const
{
    if (!m_valid) return;
    const TexturePtr& tex = (g_fontsHD && m_texHD && scale > 1.0f) ? m_texHD : m_tex1x;
    float ts = (tex == m_texHD) ? (float)m_hdScale : 1.0f;
    DrawParams p;
    p.color = argb;
    p.alphaMul = alpha;
    float cx = x;
    for (unsigned char c : s) {
        const Glyph& g = m_glyphs[c];
        int w = c == ' ' ? 0 : g.advance;
        if (w > 0 && g.h > 0) {
            g_renderer.draw(tex, g.x * ts, g.y * ts, w * ts, g.h * ts, cx, y + g.yoff * scale, w * scale, g.h * scale, p);
        }
        cx += (charWidth(c) + m_spacing) * scale;
    }
}

std::vector<std::string> Font::wrap(const std::string& s, int maxWidth) const
{
    std::vector<std::string> lines;
    std::string cur;
    std::string word;
    auto flushWord = [&]() {
        if (word.empty()) return;
        std::string candidate = cur.empty() ? word : cur + " " + word;
        if (!cur.empty() && textWidth(candidate) > maxWidth) {
            lines.push_back(cur);
            cur = word;
        } else {
            cur = candidate;
        }
        // hard-break words longer than a line
        while (textWidth(cur) > maxWidth && cur.size() > 1) {
            size_t n = cur.size();
            while (n > 1 && textWidth(cur.substr(0, n)) > maxWidth) n--;
            lines.push_back(cur.substr(0, n));
            cur = cur.substr(n);
        }
        word.clear();
    };
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (c == '\n') {
            flushWord();
            lines.push_back(cur);
            cur.clear();
        } else if (c == ' ' || c == '\t') {
            flushWord();
        } else {
            word += c;
        }
    }
    flushWord();
    if (!cur.empty() || lines.empty()) lines.push_back(cur);
    return lines;
}

Font* font(int id)
{
    static std::map<int, Font*> lookup;
    auto it = lookup.find(id);
    if (it != lookup.end()) return it->second;
    auto f = std::make_unique<Font>();
    std::string name = id >= 100 ? strFormat("font%d_aaf", id - 100) : strFormat("font%d_fon", id);
    Font* result = nullptr;
    if (f->load(name)) {
        result = f.get();
        g_fonts[id] = std::move(f);
    } else {
        LOGW("Font %d (%s) missing", id, name.c_str());
        if (id != 101) result = font(101); // fall back to the main interface font
    }
    lookup[id] = result;
    return result;
}

} // namespace fo
