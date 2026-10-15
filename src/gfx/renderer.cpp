#include "gfx/renderer.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>

namespace fo {

Renderer g_renderer;

template <class T> static void safeRelease(T*& p)
{
    if (p) {
        p->Release();
        p = nullptr;
    }
}

Texture::~Texture()
{
    safeRelease(srv);
    safeRelease(tex);
}

RenderTarget::~RenderTarget()
{
    safeRelease(srv);
    safeRelease(rtv);
    safeRelease(tex);
}

static const char* kShaderSource = R"HLSL(
cbuffer Globals : register(b0)
{
    float2 invTarget;   // 2/width, 2/height
    float2 pad0;
    float4 eggRect;     // x, y, w, h in target pixels
    float  eggOn;
    float3 pad1;
};

Texture2D<uint>   texIndexed : register(t0);
Texture2D<float4> texColor   : register(t0);
Texture2D<float4> palette    : register(t1);
Texture2D<float4> eggTex     : register(t2);
SamplerState      sampPoint  : register(s0);
SamplerState      sampLinear : register(s1);

struct VSIn {
    float2 pos   : POSITION;
    float2 uv    : TEXCOORD0;
    float4 color : COLOR0;
    float  light : TEXCOORD1;
    float4 extra : TEXCOORD2;
    float4 flat  : COLOR1;
};

struct PSIn {
    float4 pos   : SV_Position;
    float2 uv    : TEXCOORD0;
    float4 color : COLOR0;
    float  light : TEXCOORD1;
    float4 extra : TEXCOORD2;
    float4 flat  : COLOR1;
    float2 spos  : TEXCOORD3;
};

PSIn VSMain(VSIn i)
{
    PSIn o;
    o.pos = float4(i.pos.x * invTarget.x - 1.0, 1.0 - i.pos.y * invTarget.y, 0.0, 1.0);
    o.uv = i.uv;
    o.color = i.color;
    o.light = i.light;
    o.extra = i.extra;
    o.flat = i.flat;
    o.spos = i.pos;
    return o;
}

// Egg see-through mask (Fallout's wall transparency around the player). Returns wall opacity.
float eggOpacity(float2 spos, float enabled)
{
    if (eggOn < 0.5 || enabled < 0.5) return 1.0;
    float2 t = (spos - eggRect.xy) / eggRect.zw;
    if (t.x < 0.0 || t.y < 0.0 || t.x > 1.0 || t.y > 1.0) return 1.0;
    float m = eggTex.SampleLevel(sampLinear, t, 0).r * 255.0;
    if (m < 0.5) return 1.0;
    return saturate(m / 128.0);
}

float4 finish(PSIn i, float4 c)
{
    // c is straight alpha
    if (i.flat.a > 0.0) c.rgb = i.flat.rgb;
    else c.rgb *= i.light;
    c *= i.color;
    c.a *= i.extra.z;
    c.a *= eggOpacity(i.spos, i.extra.x);
    return float4(c.rgb * c.a, c.a); // premultiplied
}

float4 PSIndexed(PSIn i) : SV_Target
{
    int k = (int)i.extra.y;
    if (k <= 1) {
        uint idx = texIndexed.Load(int3(int2(floor(i.uv)), 0));
        if (idx == 0) discard;
        float4 c = palette.Load(int3(idx, 0, 0));
        c.a = 1.0;
        return finish(i, c);
    }
    int2 origin = int2(floor(i.uv - 0.5 * (k - 1)));
    float4 acc = 0;
    [loop] for (int y = 0; y < k; y++) {
        [loop] for (int x = 0; x < k; x++) {
            uint idx = texIndexed.Load(int3(origin + int2(x, y), 0));
            if (idx != 0) {
                float4 c = palette.Load(int3(idx, 0, 0));
                acc += float4(c.rgb, 1.0);
            }
        }
    }
    if (acc.a <= 0.0) discard;
    float4 c = float4(acc.rgb / acc.a, acc.a / (k * k));
    return finish(i, c);
}

float4 PSColor(PSIn i) : SV_Target
{
    int k = (int)i.extra.y;
    if (k <= 1) {
        uint w, h, levels;
        texColor.GetDimensions(0, w, h, levels);
        float4 c = texColor.Sample(sampLinear, i.uv / float2(w, h));
        if (c.a <= 0.003) discard;
        return finish(i, c);
    }
    // HD texture drawn smaller than its native scale: average k x k texels (premultiplied)
    int2 origin = int2(floor(i.uv - 0.5 * (k - 1)));
    float4 acc = 0;
    [loop] for (int y = 0; y < k; y++) {
        [loop] for (int x = 0; x < k; x++) {
            float4 t = texColor.Load(int3(origin + int2(x, y), 0));
            acc += float4(t.rgb * t.a, t.a);
        }
    }
    if (acc.a <= 0.003 * k * k) discard;
    float4 c = float4(acc.rgb / acc.a, acc.a / (k * k));
    return finish(i, c);
}

float4 PSAlpha(PSIn i) : SV_Target
{
    uint w, h, levels;
    texColor.GetDimensions(0, w, h, levels);
    float a = texColor.Sample(sampLinear, i.uv / float2(w, h)).r;
    if (a <= 0.003) discard;
    float4 c = i.color;
    c.a *= a * i.extra.z;
    return float4(c.rgb * c.a, c.a);
}

float4 PSSolid(PSIn i) : SV_Target
{
    float4 c = i.color;
    return float4(c.rgb * c.a, c.a);
}

float4 PSBlit(PSIn i) : SV_Target
{
    uint w, h, levels;
    texColor.GetDimensions(0, w, h, levels);
    float4 c = texColor.Sample(sampLinear, i.uv / float2(w, h));
    return c * i.color.a;
}
)HLSL";

static ID3DBlob* compile(const char* entry, const char* target)
{
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = D3DCompile(kShaderSource, strlen(kShaderSource), "fo1hd", nullptr, nullptr, entry, target,
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr)) {
        LOGE("Shader %s failed: %s", entry, errors ? (const char*)errors->GetBufferPointer() : "?");
        safeRelease(errors);
        return nullptr;
    }
    safeRelease(errors);
    return code;
}

struct GlobalsCB {
    float invTarget[2];
    float pad0[2];
    float eggRect[4];
    float eggOn;
    float pad1[3];
};

bool Renderer::init(void* hwnd, int width, int height, bool vsync)
{
    m_hwnd = hwnd;
    m_width = std::max(1, width);
    m_height = std::max(1, height);
    m_vsync = vsync;

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 3, D3D11_SDK_VERSION, &m_device, &got, &m_ctx);
    if (FAILED(hr)) {
        LOGW("Hardware D3D11 device failed (0x%08X), trying WARP", (unsigned)hr);
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags & ~D3D11_CREATE_DEVICE_DEBUG, levels, 3, D3D11_SDK_VERSION, &m_device, &got, &m_ctx);
        if (FAILED(hr)) {
            LOGE("D3D11CreateDevice failed: 0x%08X", (unsigned)hr);
            return false;
        }
    }
    LOGI("D3D11 device created, feature level %x", (unsigned)got);

    IDXGIDevice* dxgiDevice = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory2* factory = nullptr;
    m_device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);
    dxgiDevice->GetAdapter(&adapter);
    adapter->GetParent(__uuidof(IDXGIFactory2), (void**)&factory);
    DXGI_ADAPTER_DESC ad;
    if (SUCCEEDED(adapter->GetDesc(&ad))) LOGI("GPU: %s (%u MB)", wideToUtf8(ad.Description).c_str(), (unsigned)(ad.DedicatedVideoMemory >> 20));

    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = m_width;
    sd.Height = m_height;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    hr = factory->CreateSwapChainForHwnd(m_device, (HWND)hwnd, &sd, nullptr, nullptr, &m_swap);
    if (FAILED(hr)) {
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        sd.BufferCount = 1;
        hr = factory->CreateSwapChainForHwnd(m_device, (HWND)hwnd, &sd, nullptr, nullptr, &m_swap);
    }
    if (FAILED(hr)) {
        LOGE("CreateSwapChainForHwnd failed: 0x%08X", (unsigned)hr);
        return false;
    }
    factory->MakeWindowAssociation((HWND)hwnd, DXGI_MWA_NO_ALT_ENTER);
    safeRelease(factory);
    safeRelease(adapter);
    safeRelease(dxgiDevice);

    ID3D11Texture2D* back = nullptr;
    m_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back);
    m_device->CreateRenderTargetView(back, nullptr, &m_backRtv);
    safeRelease(back);

    if (!createShaders()) return false;

    // dynamic vertex buffer + static quad index buffer
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(Vertex) * 4 * kMaxQuads;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    m_device->CreateBuffer(&bd, nullptr, &m_vb);
    std::vector<u32> idx((size_t)kMaxQuads * 6);
    for (u32 q = 0; q < (u32)kMaxQuads; q++) {
        u32* p = &idx[q * 6];
        p[0] = q * 4;
        p[1] = q * 4 + 1;
        p[2] = q * 4 + 2;
        p[3] = q * 4;
        p[4] = q * 4 + 2;
        p[5] = q * 4 + 3;
    }
    D3D11_BUFFER_DESC ib = {};
    ib.ByteWidth = (UINT)(idx.size() * 4);
    ib.Usage = D3D11_USAGE_IMMUTABLE;
    ib.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA ibData = { idx.data() };
    m_device->CreateBuffer(&ib, &ibData, &m_ib);
    D3D11_BUFFER_DESC cb = {};
    cb.ByteWidth = sizeof(GlobalsCB);
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    m_device->CreateBuffer(&cb, nullptr, &m_cb);

    // premultiplied alpha blending
    D3D11_BLEND_DESC bl = {};
    bl.RenderTarget[0].BlendEnable = TRUE;
    bl.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    m_device->CreateBlendState(&bl, &m_blendAlpha);

    D3D11_SAMPLER_DESC smp = {};
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.MaxLOD = D3D11_FLOAT32_MAX;
    m_device->CreateSamplerState(&smp, &m_samplerPoint);
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    m_device->CreateSamplerState(&smp, &m_samplerLinear);

    D3D11_RASTERIZER_DESC rs = {};
    rs.FillMode = D3D11_FILL_SOLID;
    rs.CullMode = D3D11_CULL_NONE;
    rs.ScissorEnable = FALSE;
    rs.DepthClipEnable = TRUE;
    m_device->CreateRasterizerState(&rs, &m_raster);

    // palette texture (256x1)
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = 256;
    td.Height = 1;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    m_device->CreateTexture2D(&td, nullptr, &m_paletteTex);
    m_device->CreateShaderResourceView(m_paletteTex, nullptr, &m_paletteSrv);

    u32 white = 0xFFFFFFFFu;
    m_white = createTextureRGBA(1, 1, &white);
    m_verts.reserve((size_t)kMaxQuads * 4);
    return true;
}

bool Renderer::createShaders()
{
    ID3DBlob* vs = compile("VSMain", "vs_4_0");
    if (!vs) return false;
    m_device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &m_vs);
    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(Vertex, x), D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(Vertex, u), D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, offsetof(Vertex, color), D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32_FLOAT, 0, offsetof(Vertex, light), D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(Vertex, extra), D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 1, DXGI_FORMAT_R8G8B8A8_UNORM, 0, offsetof(Vertex, flat), D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    m_device->CreateInputLayout(layout, 6, vs->GetBufferPointer(), vs->GetBufferSize(), &m_layout);
    vs->Release();
    struct { const char* entry; ID3D11PixelShader** out; } ps[] = {
        { "PSIndexed", &m_psIndexed }, { "PSColor", &m_psRGBA }, { "PSAlpha", &m_psAlpha }, { "PSSolid", &m_psSolid }, { "PSBlit", &m_psBlit },
    };
    for (auto& p : ps) {
        ID3DBlob* b = compile(p.entry, "ps_4_0");
        if (!b) return false;
        m_device->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, p.out);
        b->Release();
    }
    return true;
}

void Renderer::shutdown()
{
    m_white.reset();
    m_eggTex.reset();
    safeRelease(m_paletteSrv);
    safeRelease(m_paletteTex);
    safeRelease(m_raster);
    safeRelease(m_samplerLinear);
    safeRelease(m_samplerPoint);
    safeRelease(m_blendAlpha);
    safeRelease(m_cb);
    safeRelease(m_ib);
    safeRelease(m_vb);
    safeRelease(m_layout);
    safeRelease(m_psBlit);
    safeRelease(m_psSolid);
    safeRelease(m_psAlpha);
    safeRelease(m_psRGBA);
    safeRelease(m_psIndexed);
    safeRelease(m_vs);
    safeRelease(m_backRtv);
    if (m_swap) m_swap->Release(), m_swap = nullptr;
    safeRelease(m_ctx);
    safeRelease(m_device);
}

void Renderer::resize(int width, int height)
{
    width = std::max(1, width);
    height = std::max(1, height);
    if (width == m_width && height == m_height) return;
    m_width = width;
    m_height = height;
    if (!m_swap) return;
    m_ctx->OMSetRenderTargets(0, nullptr, nullptr);
    safeRelease(m_backRtv);
    HRESULT hr = m_swap->ResizeBuffers(0, (UINT)width, (UINT)height, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) LOGE("ResizeBuffers failed: 0x%08X", (unsigned)hr);
    ID3D11Texture2D* back = nullptr;
    m_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back);
    m_device->CreateRenderTargetView(back, nullptr, &m_backRtv);
    safeRelease(back);
}

TexturePtr Renderer::createTexture(const Image& img, bool alphaFromPalette)
{
    if (img.width <= 0 || img.height <= 0 || !m_device) return nullptr;
    auto t = std::make_shared<Texture>();
    t->width = img.width;
    t->height = img.height;
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = (UINT)img.width;
    td.Height = (UINT)img.height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd = {};
    std::vector<u8> converted;
    if (img.indexed && alphaFromPalette) {
        // fonts / masks: palette alpha becomes coverage
        t->kind = TexKind::Alpha;
        td.Format = DXGI_FORMAT_R8_UNORM;
        converted.resize((size_t)img.width * img.height);
        for (size_t i = 0; i < converted.size(); i++) {
            u8 idx = img.pixels[i];
            converted[i] = idx < img.palette.size() ? (u8)(img.palette[idx] >> 24) : 0;
        }
        sd.pSysMem = converted.data();
        sd.SysMemPitch = (UINT)img.width;
        t->bytes = converted.size();
    } else if (img.indexed) {
        t->kind = TexKind::Indexed;
        td.Format = DXGI_FORMAT_R8_UINT;
        sd.pSysMem = img.pixels.data();
        sd.SysMemPitch = (UINT)img.width;
        t->bytes = img.pixels.size();
    } else {
        t->kind = TexKind::RGBA;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.pSysMem = img.pixels.data();
        sd.SysMemPitch = (UINT)img.width * 4;
        t->bytes = img.pixels.size();
    }
    HRESULT hr = m_device->CreateTexture2D(&td, &sd, &t->tex);
    if (FAILED(hr)) {
        LOGE("CreateTexture2D %dx%d failed: 0x%08X", img.width, img.height, (unsigned)hr);
        return nullptr;
    }
    m_device->CreateShaderResourceView(t->tex, nullptr, &t->srv);
    return t;
}

TexturePtr Renderer::createTextureRGBA(int w, int h, const u32* argb)
{
    Image img;
    img.allocRGBA(w, h);
    for (int i = 0; i < w * h; i++) {
        u32 c = argb[i];
        u8* p = &img.pixels[(size_t)i * 4];
        p[0] = (u8)(c >> 16);
        p[1] = (u8)(c >> 8);
        p[2] = (u8)c;
        p[3] = (u8)(c >> 24);
    }
    return createTexture(img);
}

TexturePtr Renderer::createTextureRaw8(int w, int h, const u8* values)
{
    if (w <= 0 || h <= 0 || !m_device) return nullptr;
    auto t = std::make_shared<Texture>();
    t->kind = TexKind::Alpha;
    t->width = w;
    t->height = h;
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = (UINT)w;
    td.Height = (UINT)h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.Format = DXGI_FORMAT_R8_UNORM;
    D3D11_SUBRESOURCE_DATA sd = { values, (UINT)w, 0 };
    if (FAILED(m_device->CreateTexture2D(&td, &sd, &t->tex))) return nullptr;
    m_device->CreateShaderResourceView(t->tex, nullptr, &t->srv);
    t->bytes = (size_t)w * h;
    return t;
}

std::unique_ptr<RenderTarget> Renderer::createRenderTarget(int w, int h, bool mips)
{
    auto rt = std::make_unique<RenderTarget>();
    rt->width = std::max(1, w);
    rt->height = std::max(1, h);
    int levels = 1;
    if (mips) {
        int m = std::max(rt->width, rt->height);
        while (m > 1) {
            m >>= 1;
            levels++;
        }
        levels = std::min(levels, 4);
    }
    rt->mipLevels = levels;
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = (UINT)rt->width;
    td.Height = (UINT)rt->height;
    td.MipLevels = (UINT)levels;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = levels > 1 ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;
    HRESULT hr = m_device->CreateTexture2D(&td, nullptr, &rt->tex);
    if (FAILED(hr)) {
        LOGE("Render target %dx%d failed: 0x%08X", w, h, (unsigned)hr);
        return nullptr;
    }
    D3D11_RENDER_TARGET_VIEW_DESC rd = {};
    rd.Format = td.Format;
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    m_device->CreateRenderTargetView(rt->tex, &rd, &rt->rtv);
    m_device->CreateShaderResourceView(rt->tex, nullptr, &rt->srv);
    return rt;
}

void Renderer::updatePalette(const u32* argb)
{
    u32 rgba[256];
    for (int i = 0; i < 256; i++) {
        u32 c = argb[i];
        rgba[i] = ((c >> 16) & 0xFF) | (c & 0xFF00) | ((c & 0xFF) << 16) | 0xFF000000u;
    }
    m_ctx->UpdateSubresource(m_paletteTex, 0, nullptr, rgba, 256 * 4, 0);
}

void Renderer::setTargetSize(int w, int h)
{
    m_targetW = w;
    m_targetH = h;
    D3D11_VIEWPORT vp = { 0, 0, (float)w, (float)h, 0, 1 };
    m_ctx->RSSetViewports(1, &vp);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(m_ctx->Map(m_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        GlobalsCB* g = (GlobalsCB*)m.pData;
        memset(g, 0, sizeof(*g));
        g->invTarget[0] = 2.0f / (float)w;
        g->invTarget[1] = 2.0f / (float)h;
        g->eggRect[0] = m_egg[0];
        g->eggRect[1] = m_egg[1];
        g->eggRect[2] = std::max(1.0f, m_egg[2]);
        g->eggRect[3] = std::max(1.0f, m_egg[3]);
        g->eggOn = m_eggOn && m_eggTex ? 1.0f : 0.0f;
        m_ctx->Unmap(m_cb, 0);
    }
}

void Renderer::beginFrame(u32 clearColor)
{
    m_drawCalls = 0;
    m_ctx->IASetInputLayout(m_layout);
    m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    UINT stride = sizeof(Vertex), offset = 0;
    m_ctx->IASetVertexBuffers(0, 1, &m_vb, &stride, &offset);
    m_ctx->IASetIndexBuffer(m_ib, DXGI_FORMAT_R32_UINT, 0);
    m_ctx->VSSetShader(m_vs, nullptr, 0);
    m_ctx->VSSetConstantBuffers(0, 1, &m_cb);
    m_ctx->PSSetConstantBuffers(0, 1, &m_cb);
    ID3D11SamplerState* samplers[2] = { m_samplerPoint, m_samplerLinear };
    m_ctx->PSSetSamplers(0, 2, samplers);
    float bf[4] = {};
    m_ctx->OMSetBlendState(m_blendAlpha, bf, 0xFFFFFFFF);
    m_ctx->RSSetState(m_raster);
    m_ctx->OMSetRenderTargets(1, &m_backRtv, nullptr);
    float c[4] = { ((clearColor >> 16) & 255) / 255.0f, ((clearColor >> 8) & 255) / 255.0f, (clearColor & 255) / 255.0f, 1.0f };
    m_ctx->ClearRenderTargetView(m_backRtv, c);
    m_currentRt = nullptr;
    setTargetSize(m_width, m_height);
}

void Renderer::beginCanvas(RenderTarget* rt, u32 clearColor)
{
    flush();
    // unbind any SRV that may alias this target
    ID3D11ShaderResourceView* none[3] = {};
    m_ctx->PSSetShaderResources(0, 3, none);
    m_currentRt = rt;
    m_ctx->OMSetRenderTargets(1, &rt->rtv, nullptr);
    float c[4] = { ((clearColor >> 16) & 255) / 255.0f, ((clearColor >> 8) & 255) / 255.0f, (clearColor & 255) / 255.0f,
        ((clearColor >> 24) & 255) / 255.0f };
    m_ctx->ClearRenderTargetView(rt->rtv, c);
    setTargetSize(rt->width, rt->height);
}

void Renderer::endCanvas()
{
    flush();
    if (m_currentRt && m_currentRt->mipLevels > 1) {
        m_ctx->OMSetRenderTargets(1, &m_backRtv, nullptr);
        m_ctx->GenerateMips(m_currentRt->srv);
    }
    m_currentRt = nullptr;
    m_ctx->OMSetRenderTargets(1, &m_backRtv, nullptr);
    setTargetSize(m_width, m_height);
}

void Renderer::setEgg(const TexturePtr& tex, float x, float y, float w, float h)
{
    flush();
    m_eggTex = tex;
    m_egg[0] = x;
    m_egg[1] = y;
    m_egg[2] = w;
    m_egg[3] = h;
    m_eggOn = tex != nullptr;
    setTargetSize(m_targetW, m_targetH);
}

void Renderer::clearEgg()
{
    flush();
    m_eggOn = false;
    setTargetSize(m_targetW, m_targetH);
}

static inline u32 argbToRgba(u32 c) { return ((c >> 16) & 0xFF) | (c & 0xFF00) | ((c & 0xFF) << 16) | (c & 0xFF000000u); }

void Renderer::bindForKind(TexKind kind, ID3D11ShaderResourceView* srv)
{
    ID3D11ShaderResourceView* views[3] = { srv, m_paletteSrv, m_eggTex ? m_eggTex->srv : nullptr };
    m_ctx->PSSetShaderResources(0, 3, views);
    switch (kind) {
    case TexKind::Indexed: m_ctx->PSSetShader(m_psIndexed, nullptr, 0); break;
    case TexKind::RGBA: m_ctx->PSSetShader(m_psRGBA, nullptr, 0); break;
    case TexKind::Alpha: m_ctx->PSSetShader(m_psAlpha, nullptr, 0); break;
    }
}

void Renderer::flush()
{
    if (m_verts.empty()) return;
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(m_ctx->Map(m_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        m_verts.clear();
        return;
    }
    memcpy(m.pData, m_verts.data(), m_verts.size() * sizeof(Vertex));
    m_ctx->Unmap(m_vb, 0);
    if (m_batchSolid) {
        ID3D11ShaderResourceView* views[3] = { nullptr, m_paletteSrv, m_eggTex ? m_eggTex->srv : nullptr };
        m_ctx->PSSetShaderResources(0, 3, views);
        m_ctx->PSSetShader(m_psSolid, nullptr, 0);
    } else {
        bindForKind(m_batchKind, m_batchSrv);
    }
    m_ctx->DrawIndexed((UINT)(m_verts.size() / 4 * 6), 0, 0);
    m_drawCalls++;
    m_verts.clear();
}

void Renderer::draw(const TexturePtr& tex, float sx, float sy, float sw, float sh, float dx, float dy, float dw, float dh, const DrawParams& p)
{
    if (!tex || !tex->srv) return;
    if (m_batchSolid || m_batchSrv != tex->srv || m_batchKind != tex->kind || m_verts.size() >= (size_t)kMaxQuads * 4) {
        flush();
        m_batchSolid = false;
        m_batchSrv = tex->srv;
        m_batchKind = tex->kind;
    }
    u32 col = argbToRgba(p.color);
    u32 flat = argbToRgba(p.flatColor);
    float ex[4] = { p.egg ? 1.0f : 0.0f, (float)std::max(1, p.texScaleDiv), p.alphaMul, 0.0f };
    float lt[4];
    for (int i = 0; i < 4; i++) lt[i] = p.lightCorners[i] >= 0.0f ? p.lightCorners[i] : p.light;
    Vertex v[4] = {
        { dx, dy, sx, sy, col, lt[0], { ex[0], ex[1], ex[2], ex[3] }, flat },
        { dx + dw, dy, sx + sw, sy, col, lt[1], { ex[0], ex[1], ex[2], ex[3] }, flat },
        { dx + dw, dy + dh, sx + sw, sy + sh, col, lt[2], { ex[0], ex[1], ex[2], ex[3] }, flat },
        { dx, dy + dh, sx, sy + sh, col, lt[3], { ex[0], ex[1], ex[2], ex[3] }, flat },
    };
    m_verts.insert(m_verts.end(), v, v + 4);
}

void Renderer::fillRect(float x, float y, float w, float h, u32 argb)
{
    if (!m_batchSolid || m_verts.size() >= (size_t)kMaxQuads * 4) {
        flush();
        m_batchSolid = true;
        m_batchSrv = nullptr;
    }
    u32 col = argbToRgba(argb);
    Vertex v[4] = {
        { x, y, 0, 0, col, 1, { 0, 1, 1, 0 }, 0 },
        { x + w, y, 0, 0, col, 1, { 0, 1, 1, 0 }, 0 },
        { x + w, y + h, 0, 0, col, 1, { 0, 1, 1, 0 }, 0 },
        { x, y + h, 0, 0, col, 1, { 0, 1, 1, 0 }, 0 },
    };
    m_verts.insert(m_verts.end(), v, v + 4);
}

void Renderer::line(float x0, float y0, float x1, float y1, u32 argb, float t)
{
    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.001f) return;
    float nx = -dy / len * t * 0.5f, ny = dx / len * t * 0.5f;
    if (!m_batchSolid || m_verts.size() >= (size_t)kMaxQuads * 4) {
        flush();
        m_batchSolid = true;
        m_batchSrv = nullptr;
    }
    u32 col = argbToRgba(argb);
    Vertex v[4] = {
        { x0 + nx, y0 + ny, 0, 0, col, 1, { 0, 1, 1, 0 }, 0 },
        { x1 + nx, y1 + ny, 0, 0, col, 1, { 0, 1, 1, 0 }, 0 },
        { x1 - nx, y1 - ny, 0, 0, col, 1, { 0, 1, 1, 0 }, 0 },
        { x0 - nx, y0 - ny, 0, 0, col, 1, { 0, 1, 1, 0 }, 0 },
    };
    m_verts.insert(m_verts.end(), v, v + 4);
}

void Renderer::blitToScreen(RenderTarget* rt, float dx, float dy, float dw, float dh, bool smooth)
{
    flush();
    ID3D11ShaderResourceView* views[3] = { rt->srv, m_paletteSrv, nullptr };
    m_ctx->PSSetShaderResources(0, 3, views);
    m_ctx->PSSetShader(m_psBlit, nullptr, 0);
    ID3D11SamplerState* samplers[2] = { m_samplerPoint, smooth ? m_samplerLinear : m_samplerPoint };
    m_ctx->PSSetSamplers(0, 2, samplers);
    u32 col = 0xFFFFFFFFu;
    float w = (float)rt->width, h = (float)rt->height;
    Vertex v[4] = {
        { dx, dy, 0, 0, col, 1, { 0, 1, 1, 0 }, 0 },
        { dx + dw, dy, w, 0, col, 1, { 0, 1, 1, 0 }, 0 },
        { dx + dw, dy + dh, w, h, col, 1, { 0, 1, 1, 0 }, 0 },
        { dx, dy + dh, 0, h, col, 1, { 0, 1, 1, 0 }, 0 },
    };
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(m_ctx->Map(m_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, v, sizeof(v));
        m_ctx->Unmap(m_vb, 0);
        m_ctx->DrawIndexed(6, 0, 0);
        m_drawCalls++;
    }
    ID3D11SamplerState* restore[2] = { m_samplerPoint, m_samplerLinear };
    m_ctx->PSSetSamplers(0, 2, restore);
    ID3D11ShaderResourceView* none[3] = {};
    m_ctx->PSSetShaderResources(0, 3, none);
}

void Renderer::endFrame()
{
    flush();
    m_swap->Present(m_vsync ? 1 : 0, 0);
}

bool Renderer::captureBackBuffer(Image& out)
{
    flush();
    ID3D11Texture2D* back = nullptr;
    if (FAILED(m_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back))) return false;
    D3D11_TEXTURE2D_DESC td;
    back->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    ID3D11Texture2D* staging = nullptr;
    bool ok = false;
    if (SUCCEEDED(m_device->CreateTexture2D(&td, nullptr, &staging))) {
        m_ctx->CopyResource(staging, back);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(m_ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
            out.allocRGBA((int)td.Width, (int)td.Height);
            for (UINT y = 0; y < td.Height; y++) {
                const u8* src = (const u8*)m.pData + (size_t)y * m.RowPitch;
                u8* dst = out.row((int)y);
                for (UINT x = 0; x < td.Width; x++) {
                    dst[x * 4 + 0] = src[x * 4 + 2];
                    dst[x * 4 + 1] = src[x * 4 + 1];
                    dst[x * 4 + 2] = src[x * 4 + 0];
                    dst[x * 4 + 3] = 255;
                }
            }
            m_ctx->Unmap(staging, 0);
            ok = true;
        }
        staging->Release();
    }
    back->Release();
    return ok;
}

} // namespace fo
