// Direct3D 11 renderer: batched 2D quads into integer-scaled canvases, palette-indexed and RGBA textures.
#pragma once
#include "core/base.h"
#include "core/png.h"

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
struct ID3D11ShaderResourceView;
struct ID3D11RenderTargetView;
struct IDXGISwapChain1;
struct ID3D11VertexShader;
struct ID3D11PixelShader;
struct ID3D11InputLayout;
struct ID3D11Buffer;
struct ID3D11BlendState;
struct ID3D11SamplerState;
struct ID3D11RasterizerState;

namespace fo {

enum class TexKind : u8 {
    Indexed, // R8_UINT palette indices (0 = transparent)
    RGBA,    // R8G8B8A8 straight alpha
    Alpha,   // R8 coverage (fonts) - tinted by vertex color
};

struct Texture {
    TexKind kind = TexKind::RGBA;
    int width = 0, height = 0;
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    size_t bytes = 0;
    ~Texture();
};
using TexturePtr = std::shared_ptr<Texture>;

struct RenderTarget {
    int width = 0, height = 0;
    ID3D11Texture2D* tex = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    int mipLevels = 1;
    ~RenderTarget();
};

// Per-quad shading parameters.
struct DrawParams {
    u32 color = 0xFFFFFFFFu;   // multiplied (ARGB)
    float light = 1.0f;        // 0..1 brightness (indexed & RGBA)
    float lightCorners[4] = { -1, -1, -1, -1 }; // optional per-corner light (TL, TR, BR, BL); -1 = use `light`
    int texScaleDiv = 1;       // indexed only: box filter k x k texels per output pixel
    bool egg = false;          // apply the "egg" see-through mask
    u32 flatColor = 0;         // if non zero (ARGB): replace sprite colour (silhouettes / outlines)
    float alphaMul = 1.0f;     // additional alpha (translucency)
};

class Renderer {
public:
    bool init(void* hwnd, int width, int height, bool vsync);
    void shutdown();
    void resize(int width, int height);
    int width() const { return m_width; }
    int height() const { return m_height; }
    void setVsync(bool v) { m_vsync = v; }

    TexturePtr createTexture(const Image& img, bool alphaFromPalette = false);
    TexturePtr createTextureRGBA(int w, int h, const u32* argb);
    // Raw 8-bit values (masks) as an Alpha texture (value/255 in the red channel).
    TexturePtr createTextureRaw8(int w, int h, const u8* values);
    std::unique_ptr<RenderTarget> createRenderTarget(int w, int h, bool mips);

    void updatePalette(const u32* argb256);

    // ---- frame
    void beginFrame(u32 clearColor);
    // Starts drawing into a canvas render target (cleared). Coordinates are in RT pixels.
    void beginCanvas(RenderTarget* rt, u32 clearColor);
    void endCanvas();
    // Egg mask: rectangle in current canvas pixels + texture (Alpha kind or Indexed).
    void setEgg(const TexturePtr& tex, float x, float y, float w, float h);
    void clearEgg();

    // Draws a textured quad. src in texels, dst in canvas pixels.
    void draw(const TexturePtr& tex, float sx, float sy, float sw, float sh, float dx, float dy, float dw, float dh, const DrawParams& p = DrawParams());
    void fillRect(float x, float y, float w, float h, u32 argb);
    void line(float x0, float y0, float x1, float y1, u32 argb, float thickness = 1.0f);
    void flush();

    // Composites a canvas onto the back buffer, stretched to dst rect (window pixels).
    void blitToScreen(RenderTarget* rt, float dx, float dy, float dw, float dh, bool smooth);
    void endFrame();

    // Screenshot of the back buffer (RGBA).
    bool captureBackBuffer(Image& out);

    ID3D11Device* device() { return m_device; }
    int frameDrawCalls() const { return m_drawCalls; }

private:
    struct Vertex {
        float x, y;
        float u, v;
        u32 color;
        float light;
        float extra[4]; // x: mode flags, y: texScaleDiv, z: alphaMul, w: unused
        u32 flat;
    };
    bool createShaders();
    void setTargetSize(int w, int h);
    void bindForKind(TexKind kind, ID3D11ShaderResourceView* srv);

    void* m_hwnd = nullptr;
    int m_width = 0, m_height = 0;
    bool m_vsync = true;

    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_ctx = nullptr;
    IDXGISwapChain1* m_swap = nullptr;
    ID3D11RenderTargetView* m_backRtv = nullptr;
    ID3D11VertexShader* m_vs = nullptr;
    ID3D11PixelShader* m_psIndexed = nullptr;
    ID3D11PixelShader* m_psRGBA = nullptr;
    ID3D11PixelShader* m_psAlpha = nullptr;
    ID3D11PixelShader* m_psSolid = nullptr;
    ID3D11PixelShader* m_psBlit = nullptr;
    ID3D11InputLayout* m_layout = nullptr;
    ID3D11Buffer* m_vb = nullptr;
    ID3D11Buffer* m_ib = nullptr;
    ID3D11Buffer* m_cb = nullptr;
    ID3D11BlendState* m_blendAlpha = nullptr;
    ID3D11SamplerState* m_samplerPoint = nullptr;
    ID3D11SamplerState* m_samplerLinear = nullptr;
    ID3D11RasterizerState* m_raster = nullptr;
    ID3D11Texture2D* m_paletteTex = nullptr;
    ID3D11ShaderResourceView* m_paletteSrv = nullptr;
    TexturePtr m_white;

    // batching
    std::vector<Vertex> m_verts;
    TexKind m_batchKind = TexKind::RGBA;
    ID3D11ShaderResourceView* m_batchSrv = nullptr;
    bool m_batchSolid = false;
    int m_targetW = 0, m_targetH = 0;
    RenderTarget* m_currentRt = nullptr;

    // egg
    TexturePtr m_eggTex;
    float m_egg[4] = {};
    bool m_eggOn = false;

    int m_drawCalls = 0;
    static const int kMaxQuads = 8192;
};

extern Renderer g_renderer;

} // namespace fo
