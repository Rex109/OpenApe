module;
#include <cstdio>
#include <cstdlib>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <d3d9.h>
#include <wrl/client.h>
#include <stb_image_write.h>

export module openape.renderer;

import openape.util;
import openape.math;
import openape.mesh;
import openape.image;
import openape.gamedata;
import openape.environment;

export namespace oa
{
    using Microsoft::WRL::ComPtr;

    class GpuTexture
    {
    public:
        ComPtr<IDirect3DTexture9> tex;
        int width = 0, height = 0;
        bool hasAlpha = false;
        std::string description;
    };

    class GpuCube
    {
    public:
        ComPtr<IDirect3DCubeTexture9> tex;
        std::string name;
    };

    class GpuMesh
    {
    public:
        ComPtr<IDirect3DVertexBuffer9> vb;
        ComPtr<IDirect3DIndexBuffer9> ib;
        ComPtr<IDirect3DVertexBuffer9> vbPacked;   // stream 1: normal/tangent/binormal as UBYTE4, the way the game's shaders decode them
        std::vector<Submesh> submeshes;
        uint32_t vertexCount = 0, indexCount = 0;
    };

    struct ShaderMaterial;

    // How one submesh is shaded (fixed-function, or the game shaders when `shader` is set).
    struct SurfaceStyle
    {
        std::shared_ptr<GpuTexture> color;
        std::array<float, 4> tint{ 1, 1, 1, 1 };
        float specular = 0.35f;
        float shininess = 24.0f;
        bool tileU = true, tileV = true;
        bool blend = false, additive = false;
        bool doubleSided = false;
        bool cullFront = false;     // cullFace "Front": the faces turned towards the viewer are the ones dropped
        unsigned CullMode() const { return doubleSided ? 1u : cullFront ? 2u : 3u; }   // D3DCULL_NONE / CW / CCW
        bool alphaTest = false;   // cut-out material: texels with alpha < 128 are discarded
        // texture filtering of the colour map (the material's filterColor)
        enum class Filter { Default, NoMipNearest, NoMipLinear, Standard, Bilinear4x, Trilinear2x, Trilinear4x } filter = Filter::Default;
        // raster state of the material (blendFunc extras, depthTest, depthWrite, polygonOffset, colorWrite*)
        bool multiply = false, screen = false;
        int depthFunc = 4;          // D3DCMP_LESSEQUAL
        int depthWrite = -1;        // -1 auto, 0 off, 1 on
        int polyOffset = 0;         // 0 none, 1 static decal, 2 weapon impact
        unsigned colorMask = 0xF;
        bool noDepthTest = false;
        // blendFunc "Custom": D3DBLENDOP / D3DBLEND values; an op of 0 means "Disable"
        bool customBlend = false;
        int opRgb = 1, opAlpha = 1, srcRgb = 2, dstRgb = 1, srcAlpha = 2, dstAlpha = 1;
        // specColorStrength / cosinePowerStrength (0..1): scale the highlight of the built-in lighting
        float specStrength = 1.0f, powerStrength = 1.0f;
        // texScroll and texture atlases: animated by the preview on the shape meshes
        bool scroll = false;
        int atlasRows = 1, atlasCols = 1;
        std::shared_ptr<ShaderMaterial> shader;   // when set, drawn with the game's own shaders
    };

    // One pass of a game technique: real D3D9 shader bytecode plus everything needed to feed it.
    struct ShaderPass
    {
        ComPtr<IDirect3DVertexShader9> vs;
        ComPtr<IDirect3DPixelShader9> ps;
        ComPtr<IDirect3DVertexDeclaration9> decl;
        std::vector<game::ShaderConstant> vsConsts, psConsts;
        struct Literal { std::string name; std::array<float, 4> value; };
        std::vector<Literal> literals;                       // material constants and technique literals
        struct Tex { int sampler; std::shared_ptr<GpuTexture> tex; bool clampU, clampV; bool isColor = false; SurfaceStyle::Filter filter = SurfaceStyle::Filter::Default; };
        std::vector<Tex> textures;
        std::string label;                                   // "vs_2_0 sunpre_s_tc0n0.hlsl + ps_2_0 ..."
    };

    struct ShaderMaterial
    {
        std::vector<ShaderPass> passes;
        bool additive = false;          // composite over a fixed-function ambient base (sunlight preview)
        std::vector<std::string> notes;
    };

    // Coloured triangles/lines drawn on top of the scene (the skeleton display).
    struct OverlayVertex { float x, y, z; DWORD color; };
    struct OverlayGeometry
    {
        std::vector<OverlayVertex> tris, lines;
        bool xray = true;      // draw through the model
        bool empty() const { return tris.empty() && lines.empty(); }
    };

    struct OrbitCamera
    {
        float yaw = 0.6f, pitch = 0.35f, distance = 3.0f;
        Vec3 target{ 0, 0, 0 };
        float fov = 0.7f;

        float radius = 1.0f;

        // first-person mode: look from `fpEye` along `fpDir` (an in-game view) instead of orbiting the target
        bool firstPerson = false;
        Vec3 fpEye{ 0, 0, 0 }, fpDir{ 1, 0, 0 }, fpUp{ 0, 0, 1 };

        void Frame(const Vec3& centre, float r)
        {
            float radius = r;
            this->radius = r;
            target = centre;
            distance = std::max(radius, 0.001f) / std::sin(fov * 0.5f) * 1.15f;
        }
        Mat4 View() const { return firstPerson ? Mat4::LookAtZUpRH(fpEye, fpEye + fpDir, fpUp) : Mat4::LookAtZUpRH(Eye(), target, { 0, 0, 1 }); }
        // The thing being looked at: its bounding sphere decides the clip planes.
        Vec3 subjectCenter{ 0, 0, 0 };
        float subjectRadius = 0;

        // Near/far planes wrap tightly around the subject's bounding sphere (measured from the eye, so panning and
        // orbiting stay correct). The tighter they are, the finer the depth buffer resolves surfaces that almost
        // coincide (shirt on skin, coat over shirt).
        void ClipPlanes(float& zn, float& zf) const
        {
            if (subjectRadius <= 0) { zn = std::max(distance * 0.01f, 0.01f); zf = distance * 20.0f + 100.0f; return; }
            // the near plane follows the depth along the view axis (not the distance to the eye): with the subject far off
            // to one side the two differ a lot, and a distance-based near plane would cut the subject away
            Vec3 eye = Eye();
            Vec3 fwd = firstPerson ? fpDir : (target - eye);
            fwd = fwd.Length() > 1e-6f ? fwd.Normalized() : Vec3{ 1, 0, 0 };
            float d = (eye - subjectCenter).Length();
            float depth = (subjectCenter - eye).Dot(fwd);
            zn = std::max(depth - subjectRadius * 1.05f, subjectRadius * 0.01f);
            zf = std::max(d + subjectRadius * 1.05f, zn * 2.0f);
        }
        Mat4 Proj(float aspect) const
        {
            float zn, zf;
            ClipPlanes(zn, zf);
            return Mat4::PerspectiveFovLH(fov, aspect, zn, zf);
        }
        // Wide planes for things that extend far beyond the subject (the floor grid).
        Mat4 ProjWide(float aspect) const
        {
            return Mat4::PerspectiveFovLH(fov, aspect, std::max(distance * 0.01f, 0.01f), distance * 20.0f + 100.0f);
        }
        Vec3 Eye() const
        {
            if (firstPerson) return fpEye;
            return target + Vec3{ std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), std::sin(pitch) } * distance;
        }
    };



    struct SceneOptions
    {
        bool wireframe = false;
        bool lighting = true;
        bool grid = true;
        bool turntableLight = false;
        bool skybox = true;
        LightSetup light;                      // sun + ambient (stock map lighting, or the camera headlight)
        std::shared_ptr<GpuCube> sky;          // in-game sky box; the flat gradient is used when empty
        float lightAngle = 0.8f;               // headlight only
        float lightYawDeg = 0, lightElevDeg = 0;   // user rotation of the sun on top of the preset
        std::array<float, 4> background{ 0.30f, 0.30f, 0.33f, 1 };
    };

    class RenderTarget
    {
    public:
        ComPtr<IDirect3DTexture9> colorTex;
        ComPtr<IDirect3DSurface9> colorSurf, depth;
        int width = 0, height = 0;
        void Release() { colorSurf.Reset(); colorTex.Reset(); depth.Reset(); width = height = 0; }
    };

    class Renderer
    {
    public:
        ~Renderer() { Shutdown(); }

        bool Init(HWND hwnd, int w, int h)
        {
            m_d3d.Attach(Direct3DCreate9(D3D_SDK_VERSION));
            if (!m_d3d) return false;
            ZeroMemory(&m_pp, sizeof(m_pp));
            m_pp.Windowed = TRUE;
            m_pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
            m_pp.BackBufferFormat = D3DFMT_UNKNOWN;
            m_pp.BackBufferWidth = (UINT)w; m_pp.BackBufferHeight = (UINT)h;
            m_pp.EnableAutoDepthStencil = TRUE;
            m_pp.AutoDepthStencilFormat = D3DFMT_D24S8;
            m_pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
            m_pp.hDeviceWindow = hwnd;
            if (FAILED(m_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &m_pp, &m_dev)))
                if (FAILED(m_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &m_pp, &m_dev)))
                    return false;
            m_dev->GetDeviceCaps(&m_caps);
            return true;
        }

        void Shutdown()
        {
            m_textures.clear();
            m_ambientPs.Reset();
            m_dev.Reset(); m_d3d.Reset();
        }

        IDirect3DDevice9* Device() const { return m_dev.Get(); }
        const D3DCAPS9& Caps() const { return m_caps; }

        // Returns true while the device is usable; handles lost/reset.
        bool EnsureDevice(int w, int h, const std::function<void()>& onLost, const std::function<void()>& onReset)
        {
            HRESULT hr = m_dev->TestCooperativeLevel();
            bool needReset = m_resizeRequested;
            if (hr == D3DERR_DEVICELOST) { Sleep(10); return false; }
            if (hr == D3DERR_DEVICENOTRESET) needReset = true;
            if (needReset)
            {
                if (w <= 0 || h <= 0) return false;
                onLost();
                m_pp.BackBufferWidth = (UINT)w; m_pp.BackBufferHeight = (UINT)h;
                if (FAILED(m_dev->Reset(&m_pp))) return false;
                m_resizeRequested = false;
                onReset();
            }
            return true;
        }
        void RequestReset() { m_resizeRequested = true; }
        int BackBufferWidth() const { return (int)m_pp.BackBufferWidth; }
        int BackBufferHeight() const { return (int)m_pp.BackBufferHeight; }

        // Writes the current back buffer to a PNG (call between EndScene and Present).
        bool SaveBackBuffer(const fs::path& path)
        {
            ComPtr<IDirect3DSurface9> bb, sys;
            if (FAILED(m_dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return false;
            D3DSURFACE_DESC d; bb->GetDesc(&d);
            if (FAILED(m_dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr))) return false;
            if (FAILED(m_dev->GetRenderTargetData(bb.Get(), sys.Get()))) return false;
            D3DLOCKED_RECT lr;
            if (FAILED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) return false;
            std::vector<uint8_t> rgb((size_t)d.Width * d.Height * 3);
            for (UINT y = 0; y < d.Height; ++y)
            {
                const uint8_t* src = (const uint8_t*)lr.pBits + (size_t)y * lr.Pitch;
                for (UINT x = 0; x < d.Width; ++x)
                {
                    rgb[((size_t)y * d.Width + x) * 3 + 0] = src[x * 4 + 2];
                    rgb[((size_t)y * d.Width + x) * 3 + 1] = src[x * 4 + 1];
                    rgb[((size_t)y * d.Width + x) * 3 + 2] = src[x * 4 + 0];
                }
            }
            sys->UnlockRect();
            return stbi_write_png(path.string().c_str(), (int)d.Width, (int)d.Height, 3, rgb.data(), (int)d.Width * 3) != 0;
        }

        // ------------------------------------------------------------------ resources
        std::shared_ptr<GpuTexture> CreateTexture(const ImageData& img)
        {
            auto t = std::make_shared<GpuTexture>();
            D3DFORMAT fmt = D3DFMT_A8R8G8B8;
            switch (img.format)
            {
            case PixelFormat::DXT1: fmt = D3DFMT_DXT1; break;
            case PixelFormat::DXT3: fmt = D3DFMT_DXT3; break;
            case PixelFormat::DXT5: fmt = D3DFMT_DXT5; break;
            default: break;
            }
            HRESULT hr = m_dev->CreateTexture(img.width, img.height, 0, D3DUSAGE_AUTOGENMIPMAP, fmt, D3DPOOL_MANAGED, &t->tex, nullptr);
            if (FAILED(hr)) hr = m_dev->CreateTexture(img.width, img.height, 1, 0, fmt, D3DPOOL_MANAGED, &t->tex, nullptr);
            if (FAILED(hr)) return nullptr;
            D3DLOCKED_RECT lr;
            if (FAILED(t->tex->LockRect(0, &lr, nullptr, 0))) return nullptr;
            if (img.format == PixelFormat::BGRA8)
            {
                for (int y = 0; y < img.height; ++y)
                    std::memcpy((uint8_t*)lr.pBits + (size_t)y * lr.Pitch, &img.pixels[(size_t)y * img.width * 4], (size_t)img.width * 4);
            }
            else
            {
                size_t blockBytes = img.format == PixelFormat::DXT1 ? 8 : 16;
                size_t rows = (img.height + 3) / 4, rowBytes = ((img.width + 3) / 4) * blockBytes;
                for (size_t y = 0; y < rows; ++y)
                    std::memcpy((uint8_t*)lr.pBits + y * lr.Pitch, &img.pixels[y * rowBytes], rowBytes);
            }
            t->tex->UnlockRect(0);
            if (t->tex->GetLevelCount() > 1) t->tex->GenerateMipSubLevels();
            t->width = img.width; t->height = img.height; t->hasAlpha = img.hasAlpha; t->description = img.description;
            return t;
        }

        // Loads (and caches) an image file as a texture; null + error on failure.
        std::shared_ptr<GpuTexture> LoadTexture(const fs::path& path, std::string& error)
        {
            std::error_code ec;
            std::string key = path.string() + "|" + std::to_string(fs::last_write_time(path, ec).time_since_epoch().count());
            auto it = m_textures.find(key);
            if (it != m_textures.end())
            {
                if (!it->second) error = m_textureErrors[key];
                return it->second;
            }
            ImageData img;
            if (!ImageLoader::Load(path, img, error)) { m_textures[key] = nullptr; m_textureErrors[key] = error; return nullptr; }
            auto t = CreateTexture(img);
            if (!t) { error = "could not create texture (" + img.description + ")"; m_textureErrors[key] = error; }
            m_textures[key] = t;
            return t;
        }

        // Loads an image that does not live on disk (e.g. inside an .iwd); `key` identifies it in the cache.
        std::shared_ptr<GpuTexture> LoadTextureFromMemory(const std::string& key, const std::vector<unsigned char>& bytes, const std::string& ext, std::string& error)
        {
            auto it = m_textures.find(key);
            if (it != m_textures.end())
            {
                if (!it->second) error = m_textureErrors[key];
                return it->second;
            }
            ImageData img;
            if (!ImageLoader::LoadFromMemory(bytes, ext, img, error)) { m_textures[key] = nullptr; m_textureErrors[key] = error; return nullptr; }
            auto t = CreateTexture(img);
            if (!t) { error = "could not create texture (" + img.description + ")"; m_textureErrors[key] = error; }
            m_textures[key] = t;
            return t;
        }

        // Builds a cube texture from an IWI cube map (six faces, DXT compressed).
        std::shared_ptr<GpuCube> CreateCube(const std::string& name, const std::vector<unsigned char>& iwi, std::string& error)
        {
            ImageData faces[6];
            if (!ImageLoader::LoadIwiCube(iwi, faces, error)) return nullptr;
            D3DFORMAT fmt = faces[0].format == PixelFormat::DXT1 ? D3DFMT_DXT1 : faces[0].format == PixelFormat::DXT3 ? D3DFMT_DXT3 :
                            faces[0].format == PixelFormat::DXT5 ? D3DFMT_DXT5 : D3DFMT_A8R8G8B8;
            auto c = std::make_shared<GpuCube>();
            c->name = name;
            if (FAILED(m_dev->CreateCubeTexture(faces[0].width, 1, 0, fmt, D3DPOOL_MANAGED, &c->tex, nullptr))) { error = "could not create cube texture"; return nullptr; }
            const bool raw = faces[0].format == PixelFormat::BGRA8;
            size_t block = faces[0].format == PixelFormat::DXT1 ? 8 : 16;
            size_t rows = raw ? faces[0].height : (faces[0].height + 3) / 4, rowBytes = raw ? (size_t)faces[0].width * 4 : ((faces[0].width + 3) / 4) * block;
            for (int i = 0; i < 6; ++i)
            {
                D3DLOCKED_RECT lr;
                if (FAILED(c->tex->LockRect((D3DCUBEMAP_FACES)i, 0, &lr, nullptr, 0))) { error = "could not lock cube face"; return nullptr; }
                for (size_t y = 0; y < rows; ++y) std::memcpy((uint8_t*)lr.pBits + y * lr.Pitch, &faces[i].pixels[y * rowBytes], rowBytes);
                c->tex->UnlockRect((D3DCUBEMAP_FACES)i, 0);
            }
            return c;
        }

        std::unique_ptr<GpuMesh> CreateMesh(const MeshData& m)
        {
            auto g = std::make_unique<GpuMesh>();
            if (m.Empty()) return g;
            g->vertexCount = (uint32_t)m.vertices.size();
            g->indexCount = (uint32_t)m.indices.size();
            g->submeshes = m.submeshes;
            if (FAILED(m_dev->CreateVertexBuffer(g->vertexCount * sizeof(Vertex), D3DUSAGE_WRITEONLY, kFvf, D3DPOOL_MANAGED, &g->vb, nullptr))) return nullptr;
            void* p = nullptr;
            g->vb->Lock(0, 0, &p, 0); std::memcpy(p, m.vertices.data(), g->vertexCount * sizeof(Vertex)); g->vb->Unlock();
            if (FAILED(m_dev->CreateVertexBuffer((UINT)(m.vertices.size() * 12), D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &g->vbPacked, nullptr))) return nullptr;
            UploadPacked(*g, m);
            if (FAILED(m_dev->CreateIndexBuffer(g->indexCount * 4, D3DUSAGE_WRITEONLY, D3DFMT_INDEX32, D3DPOOL_MANAGED, &g->ib, nullptr))) return nullptr;
            g->ib->Lock(0, 0, &p, 0); std::memcpy(p, m.indices.data(), g->indexCount * 4); g->ib->Unlock();
            return g;
        }

        // Re-uploads the vertices of an animated mesh (same topology as when it was created).
        void UpdateMesh(GpuMesh& g, const MeshData& m)
        {
            if (!g.vb || m.vertices.size() != g.vertexCount) return;
            void* p = nullptr;
            if (SUCCEEDED(g.vb->Lock(0, 0, &p, 0))) { std::memcpy(p, m.vertices.data(), g.vertexCount * sizeof(Vertex)); g.vb->Unlock(); }
            UploadPacked(g, m);
        }

    private:
        void UploadPacked(GpuMesh& g, const MeshData& m)
        {
            if (!g.vbPacked) return;
            {
                // game vertex shaders decode n = byte / 127 - 1 and scale by (w + 192) / 255, so w = 63 means unit length
                auto pack = [](float x, float y, float z) {
                    auto b = [](float v) { return (uint32_t)std::clamp((int)std::lround((v + 1.0f) * 127.0f), 0, 255); };
                    return b(x) | (b(y) << 8) | (b(z) << 16) | (63u << 24);
                };
                void* pp = nullptr;
                if (FAILED(g.vbPacked->Lock(0, 0, &pp, 0))) return;
                auto* out = (uint32_t*)pp;
                for (auto& v : m.vertices)
                {
                    *out++ = pack(v.nx, v.ny, v.nz);
                    *out++ = pack(v.tx, v.ty, v.tz);
                    *out++ = pack(v.bx, v.by, v.bz);
                }
                g.vbPacked->Unlock();
            }
        }

    public:

        bool EnsureTarget(RenderTarget& rt, int w, int h)
        {
            w = std::max(w, 8); h = std::max(h, 8);
            if (rt.colorTex && rt.width == w && rt.height == h) return true;
            rt.Release();
            if (FAILED(m_dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &rt.colorTex, nullptr))) return false;
            rt.colorTex->GetSurfaceLevel(0, &rt.colorSurf);
            if (FAILED(m_dev->CreateDepthStencilSurface(w, h, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE, &rt.depth, nullptr))) return false;
            rt.width = w; rt.height = h;
            return true;
        }

        // Builds D3D shader objects and the vertex declaration for one technique pass.
        bool CreateShaderPass(const game::ShaderCode& vs, const game::ShaderCode& ps, const std::vector<game::VertexBinding>& inputs, ShaderPass& out, std::string* why = nullptr)
        {
            auto fail = [&](const char* what, HRESULT hr) {
                if (why) { char b[96]; std::snprintf(b, sizeof(b), "%s failed (0x%08x)", what, (unsigned)hr); *why = b; }
                return false;
            };
            HRESULT hr;
            if (FAILED(hr = m_dev->CreateVertexShader((const DWORD*)vs.bytes.data(), &out.vs))) return fail("vertex shader", hr);
            if (FAILED(hr = m_dev->CreatePixelShader((const DWORD*)ps.bytes.data(), &out.ps))) return fail("pixel shader", hr);
            out.vsConsts = vs.constants; out.psConsts = ps.constants;

            std::vector<D3DVERTEXELEMENT9> el;
            for (auto& in : inputs)
            {
                WORD offset = 0; BYTE type = D3DDECLTYPE_FLOAT3; WORD stream = 0;
                const std::string& src = in.source;
                if (src == "position") { offset = 0; }
                else if (src == "normal") { stream = 1; offset = 0; type = D3DDECLTYPE_UBYTE4; }
                else if (src.rfind("texcoord", 0) == 0) { offset = 24; type = D3DDECLTYPE_FLOAT2; }
                else if (src == "color") { offset = 32; type = D3DDECLTYPE_D3DCOLOR; }
                else if (src == "tangent" || src == "normalTransform[0]") { stream = 1; offset = 4; type = D3DDECLTYPE_UBYTE4; }
                else if (src == "binormal" || src == "normalTransform[1]") { stream = 1; offset = 8; type = D3DDECLTYPE_UBYTE4; }
                else continue;
                BYTE usage; BYTE idx = 0;
                const std::string& t = in.target;
                auto index = [&](size_t from) { size_t a = t.find('[', from); return a == std::string::npos ? 0 : std::atoi(t.c_str() + a + 1); };
                if (t == "position") usage = D3DDECLUSAGE_POSITION;
                else if (t == "normal") usage = D3DDECLUSAGE_NORMAL;
                else if (t.rfind("color", 0) == 0) { usage = D3DDECLUSAGE_COLOR; idx = (BYTE)index(0); }
                else if (t.rfind("texcoord", 0) == 0) { usage = D3DDECLUSAGE_TEXCOORD; idx = (BYTE)index(0); }
                else continue;
                el.push_back({ stream, offset, type, D3DDECLMETHOD_DEFAULT, usage, idx });
            }
            std::stable_sort(el.begin(), el.end(), [](const D3DVERTEXELEMENT9& x, const D3DVERTEXELEMENT9& y) { return x.Stream < y.Stream; });   // D3D wants elements grouped by stream
            el.push_back(D3DDECL_END());
            if (FAILED(hr = m_dev->CreateVertexDeclaration(el.data(), &out.decl))) return fail("vertex declaration", hr);
            return true;
        }

        // ------------------------------------------------------------------ drawing
        struct DrawCall
        {
            const GpuMesh* mesh;
            size_t submesh;
            SurfaceStyle style;
            Mat4 world;           // placement of this draw call relative to the scene's world matrix
        };

        void RenderScene(RenderTarget& rt, const OrbitCamera& cam, const SceneOptions& opt, const std::vector<DrawCall>& calls, const Mat4& world, const OverlayGeometry* overlay = nullptr)
        {
            ComPtr<IDirect3DSurface9> oldRt, oldDepth;
            m_dev->GetRenderTarget(0, &oldRt);
            m_dev->GetDepthStencilSurface(&oldDepth);
            m_dev->SetRenderTarget(0, rt.colorSurf.Get());
            m_dev->SetDepthStencilSurface(rt.depth.Get());

            D3DVIEWPORT9 vp{ 0, 0, (DWORD)rt.width, (DWORD)rt.height, 0.0f, 1.0f };
            m_dev->SetViewport(&vp);
            auto c = opt.background;
            m_dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL,
                         D3DCOLOR_COLORVALUE(c[0], c[1], c[2], 1), 1.0f, 0);

            if (SUCCEEDED(m_dev->BeginScene()))
            {
                DrawBackdrop(rt.width, rt.height, opt);
                if (opt.skybox && opt.sky) DrawSky(cam, *opt.sky, (float)rt.width / (float)rt.height);

                float aspect = (float)rt.width / (float)rt.height;
                Mat4 view = cam.View();
                Mat4 proj = cam.Proj(aspect);
                m_dev->SetTransform(D3DTS_VIEW, (const D3DMATRIX*)&view);
                if (opt.grid)
                {
                    Mat4 wide = cam.ProjWide(aspect);
                    m_dev->SetTransform(D3DTS_PROJECTION, (const D3DMATRIX*)&wide);
                    DrawGrid(cam);
                }
                m_dev->SetTransform(D3DTS_PROJECTION, (const D3DMATRIX*)&proj);

                m_dev->SetTransform(D3DTS_WORLD, (const D3DMATRIX*)&world);
                m_frame.world = world; m_frame.view = view; m_frame.proj = proj;
                m_frame.eye = cam.Eye();
                m_frame.time = (float)(GetTickCount64() % 1000000) * 0.001f;
                m_frame.sunDir = SetupLighting(cam, opt) * -1.0f;   // direction towards the light
                m_dev->SetRenderState(D3DRS_ZENABLE, TRUE);
                m_dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
                m_dev->SetRenderState(D3DRS_FILLMODE, opt.wireframe ? D3DFILL_WIREFRAME : D3DFILL_SOLID);
                m_dev->SetRenderState(D3DRS_NORMALIZENORMALS, TRUE);
                m_dev->SetFVF(kFvf);

                // opaque first, blended last
                for (int pass = 0; pass < 2; ++pass)
                    for (auto& dc : calls)
                    {
                        if (!dc.mesh || !dc.mesh->vb || (dc.style.blend || dc.style.additive) != (pass == 1)) continue;
                        Mat4 w = dc.world * world;
                        m_dev->SetTransform(D3DTS_WORLD, (const D3DMATRIX*)&w);
                        m_frame.world = w;
                        DrawSubmesh(dc, opt);
                    }
                m_dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
                m_dev->SetTexture(0, nullptr);
                if (overlay && !overlay->empty()) DrawOverlay(*overlay);
                m_dev->EndScene();
            }
            m_dev->SetRenderTarget(0, oldRt.Get());
            m_dev->SetDepthStencilSurface(oldDepth.Get());
        }

    private:
        static constexpr DWORD kFvf = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1;

        void DrawBackdrop(int w, int h, const SceneOptions& opt)
        {
            struct V { float x, y, z, rhw; DWORD c; };
            auto col = [&](float k) {
                return D3DCOLOR_COLORVALUE(std::clamp(opt.background[0] * k, 0.f, 1.f), std::clamp(opt.background[1] * k, 0.f, 1.f),
                                           std::clamp(opt.background[2] * k, 0.f, 1.f), 1.f);
            };
            DWORD top = col(1.35f), bottom = col(0.55f);
            V q[4] = { { -0.5f, -0.5f, 1, 1, top }, { w - 0.5f, -0.5f, 1, 1, top },
                       { -0.5f, h - 0.5f, 1, 1, bottom }, { w - 0.5f, h - 0.5f, 1, 1, bottom } };
            m_dev->SetRenderState(D3DRS_ZENABLE, FALSE);
            m_dev->SetRenderState(D3DRS_LIGHTING, FALSE);
            m_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
            m_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
            m_dev->SetTexture(0, nullptr);
            m_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            m_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
            m_dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
            m_dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V));
        }

        // The sky box: a cube around the camera sampled with the view direction. Game axes are used as the cube axes.
        void DrawSky(const OrbitCamera& cam, const GpuCube& sky, float aspect)
        {
            struct V { float x, y, z, u, v, w; };
            static const float s = 1.0f;
            static const V corners[8] = { { -s, -s, -s }, { s, -s, -s }, { s, s, -s }, { -s, s, -s }, { -s, -s, s }, { s, -s, s }, { s, s, s }, { -s, s, s } };
            static const int quads[6][4] = { { 1, 2, 6, 5 }, { 3, 0, 4, 7 }, { 2, 3, 7, 6 }, { 0, 1, 5, 4 }, { 4, 5, 6, 7 }, { 3, 2, 1, 0 } };
            std::vector<V> v;
            for (auto& q : quads)
                for (int k : { 0, 1, 2, 0, 2, 3 })
                {
                    V c = corners[q[k]];
                    v.push_back({ c.x, c.y, c.z, c.x, c.y, c.z });   // texture coordinate = direction
                }
            Mat4 view = cam.View();
            view.m[3][0] = view.m[3][1] = view.m[3][2] = 0;           // rotation only: the sky never gets closer
            Mat4 proj = Mat4::PerspectiveFovLH(cam.fov, aspect, 0.1f, 10.0f);
            Mat4 id;
            m_dev->SetTransform(D3DTS_WORLD, (const D3DMATRIX*)&id);
            m_dev->SetTransform(D3DTS_VIEW, (const D3DMATRIX*)&view);
            m_dev->SetTransform(D3DTS_PROJECTION, (const D3DMATRIX*)&proj);
            m_dev->SetRenderState(D3DRS_ZENABLE, FALSE);
            m_dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
            m_dev->SetRenderState(D3DRS_LIGHTING, FALSE);
            m_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
            m_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
            m_dev->SetVertexShader(nullptr); m_dev->SetPixelShader(nullptr);
            m_dev->SetTexture(0, sky.tex.Get());
            m_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            m_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            m_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
            m_dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
            m_dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
            m_dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            m_dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
            m_dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            m_dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            m_dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
            m_dev->SetSamplerState(0, D3DSAMP_ADDRESSW, D3DTADDRESS_CLAMP);
            m_dev->SetFVF(D3DFVF_XYZ | D3DFVF_TEX1 | D3DFVF_TEXCOORDSIZE3(0));
            m_dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, (UINT)v.size() / 3, v.data(), sizeof(V));
            m_dev->SetTexture(0, nullptr);
            m_dev->SetRenderState(D3DRS_ZENABLE, TRUE);
        }

        void DrawOverlay(const OverlayGeometry& ov)
        {
            Mat4 id;
            m_dev->SetTransform(D3DTS_WORLD, (const D3DMATRIX*)&id);
            m_dev->SetVertexShader(nullptr); m_dev->SetPixelShader(nullptr);
            m_dev->SetTexture(0, nullptr);
            m_dev->SetRenderState(D3DRS_LIGHTING, FALSE);
            m_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
            m_dev->SetRenderState(D3DRS_ZENABLE, ov.xray ? FALSE : TRUE);
            m_dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
            m_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
            m_dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
            m_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
            m_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            m_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
            m_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
            m_dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
            m_dev->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE);
            if (!ov.tris.empty()) m_dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, (UINT)ov.tris.size() / 3, ov.tris.data(), sizeof(OverlayVertex));
            if (!ov.lines.empty()) m_dev->DrawPrimitiveUP(D3DPT_LINELIST, (UINT)ov.lines.size() / 2, ov.lines.data(), sizeof(OverlayVertex));
            m_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
            m_dev->SetRenderState(D3DRS_ZENABLE, TRUE);
        }

        void DrawGrid(const OrbitCamera& cam)
        {
            struct V { float x, y, z; DWORD c; };
            std::vector<V> lines;
            float step = std::pow(10.0f, std::floor(std::log10(std::max(cam.distance, 0.01f))));
            int n = 10;
            float zlevel = 0.0f;
            for (int i = -n; i <= n; ++i)
            {
                DWORD c = i == 0 ? D3DCOLOR_ARGB(160, 200, 200, 200) : D3DCOLOR_ARGB(70, 160, 160, 160);
                lines.push_back({ i * step, -n * step, zlevel, c }); lines.push_back({ i * step, n * step, zlevel, c });
                lines.push_back({ -n * step, i * step, zlevel, c }); lines.push_back({ n * step, i * step, zlevel, c });
            }
            Mat4 id;
            m_dev->SetTransform(D3DTS_WORLD, (const D3DMATRIX*)&id);
            m_dev->SetRenderState(D3DRS_LIGHTING, FALSE);
            m_dev->SetRenderState(D3DRS_ZENABLE, TRUE);
            m_dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
            m_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
            m_dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
            m_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
            m_dev->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE);
            m_dev->DrawPrimitiveUP(D3DPT_LINELIST, (UINT)lines.size() / 2, lines.data(), sizeof(V));
            m_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        }

        // The preset's sun direction turned by the user's yaw and height offsets.
        static Vec3 EffectiveSun(const SceneOptions& opt)
        {
            Vec3 s = opt.light.toSun;
            float az = std::atan2(s.y, s.x) + opt.lightYawDeg * kPi / 180.0f;
            float el = std::asin(std::clamp(s.z, -1.0f, 1.0f)) + opt.lightElevDeg * kPi / 180.0f;
            el = std::clamp(el, 0.05f, 1.5f);
            return { std::cos(el) * std::cos(az), std::cos(el) * std::sin(az), std::sin(el) };
        }

        Vec3 SetupLighting(const OrbitCamera& cam, const SceneOptions& opt)
        {
            m_dev->SetRenderState(D3DRS_LIGHTING, opt.lighting ? TRUE : FALSE);
            const LightSetup& ls = opt.light;
            m_frame.ambient = ls.ambient;
            m_frame.sunColor = ls.sunColor;
            m_dev->SetRenderState(D3DRS_AMBIENT, D3DCOLOR_COLORVALUE(std::min(ls.ambient.x, 1.f), std::min(ls.ambient.y, 1.f), std::min(ls.ambient.z, 1.f), 1));
            m_dev->SetRenderState(D3DRS_SPECULARENABLE, TRUE);
            D3DLIGHT9 l{};
            l.Type = D3DLIGHT_DIRECTIONAL;
            l.Diffuse = { std::min(ls.sunColor.x, 1.f), std::min(ls.sunColor.y, 1.f), std::min(ls.sunColor.z, 1.f), 1 };
            l.Specular = { 1, 1, 1, 1 };
            Vec3 dir;
            if (ls.headlight)
            {
                // light is attached to the camera, rotated by lightAngle around the view axis
                Vec3 toCam = (cam.Eye() - cam.target).Normalized();
                Vec3 right = Vec3{ 0, 0, 1 }.Cross(toCam).Normalized();
                Vec3 up = toCam.Cross(right);
                float a = opt.lightAngle;
                dir = (toCam * 0.6f + right * (std::cos(a) * -0.6f) + up * (std::sin(a) * -0.6f + 0.35f)).Normalized() * -1.0f;
            }
            else
            {
                dir = EffectiveSun(opt) * -1.0f;   // from the sun to the scene
            }
            l.Direction = { dir.x, dir.y, dir.z };
            m_dev->SetLight(0, &l);
            m_dev->LightEnable(0, TRUE);
            return dir;
        }

        void DrawSubmesh(const DrawCall& dc, const SceneOptions& opt)
        {
            const SurfaceStyle& s = dc.style;
            if (s.shader && !s.shader->passes.empty())
            {
                if (s.shader->additive)
                {
                    // Sunlight is added on top of an ambient base. Fixed-function and shader transforms do not give
                    // bit-identical depth, so depth comes from the shader pass itself (depth-only first), the ambient
                    // pass is nudged towards the camera and never writes depth, and the additive pass matches exactly.
                    const bool translucent = s.blend || s.additive;   // blended surfaces never write depth
                    // Cut-out / blended surfaces (hair, fabric edges): the sunlight shader outputs alpha 1, so it cannot cut
                    // them out itself. The ambient pass (which does see the texture alpha) marks the visible texels in the
                    // stencil buffer and the sunlight pass only lights those; a last pass clears the marks again.
                    const bool cutout = (s.alphaTest || translucent) && s.color && s.color->hasAlpha;
                    if (cutout)
                    {
                        DrawAmbientShaded(dc, AmbientMode::Cutout);
                        m_stencilEqual = true;
                        DrawShaded(dc, ShadePhase::Additive);
                        m_stencilEqual = false;
                        DrawAmbientShaded(dc, AmbientMode::ClearStencil);
                    }
                    else
                    {
                        if (!translucent) DrawShaded(dc, ShadePhase::DepthOnly);
                        DrawAmbientShaded(dc, AmbientMode::Opaque);
                        DrawShaded(dc, ShadePhase::Additive);
                    }
                }
                else DrawShaded(dc, ShadePhase::Normal);
                return;
            }
            DrawFixed(dc, opt, false);
        }

        void DrawFixed(const DrawCall& dc, const SceneOptions& opt, bool ambientOnly)
        {
            const SurfaceStyle& s = dc.style;
            m_dev->SetVertexShader(nullptr); m_dev->SetPixelShader(nullptr);
            m_dev->SetFVF(kFvf);
            if (ambientOnly)
            {
                m_dev->SetRenderState(D3DRS_LIGHTING, TRUE);
                m_dev->LightEnable(0, FALSE);
                m_dev->SetRenderState(D3DRS_AMBIENT, D3DCOLOR_COLORVALUE(m_frame.ambient.x, m_frame.ambient.y, m_frame.ambient.z, 1));
            }
            D3DMATERIAL9 mtl{};
            const float k = (s.color && s.color->tex) ? 1.0f : 0.72f;   // untextured surfaces: soft grey so shading stays visible
            mtl.Diffuse = { s.tint[0] * k, s.tint[1] * k, s.tint[2] * k, s.tint[3] };
            mtl.Ambient = mtl.Diffuse;
            const float sp = s.specular * s.specStrength;
            mtl.Specular = { sp, sp, sp, 1 };
            mtl.Power = std::max(s.shininess * (0.1f + 0.9f * s.powerStrength), 1.0f);
            m_dev->SetMaterial(&mtl);

            m_dev->SetStreamSource(0, dc.mesh->vb.Get(), 0, sizeof(Vertex));
            m_dev->SetIndices(dc.mesh->ib.Get());

            m_dev->SetRenderState(D3DRS_CULLMODE, s.CullMode());
            m_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, (s.blend || s.additive) ? TRUE : FALSE);
            m_dev->SetRenderState(D3DRS_SRCBLEND, s.additive ? D3DBLEND_SRCALPHA : D3DBLEND_SRCALPHA);
            m_dev->SetRenderState(D3DRS_DESTBLEND, s.additive ? D3DBLEND_ONE : D3DBLEND_INVSRCALPHA);
            m_dev->SetRenderState(D3DRS_ZWRITEENABLE, (s.blend || s.additive) ? FALSE : TRUE);
            ApplyMaterialRaster(s);
            m_dev->SetRenderState(D3DRS_COLORVERTEX, FALSE);
            m_dev->SetRenderState(D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_MATERIAL);

            if (s.color && s.color->tex)
            {
                m_dev->SetTexture(0, s.color->tex.Get());
                m_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
                m_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
                m_dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
                m_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, s.color->hasAlpha ? D3DTOP_MODULATE : D3DTOP_SELECTARG2);
                m_dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
                m_dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
                m_dev->SetSamplerState(0, D3DSAMP_ADDRESSU, s.tileU ? D3DTADDRESS_WRAP : D3DTADDRESS_CLAMP);
                m_dev->SetSamplerState(0, D3DSAMP_ADDRESSV, s.tileV ? D3DTADDRESS_WRAP : D3DTADDRESS_CLAMP);
                SetFilter(0, s.filter);
            }
            else
            {
                m_dev->SetTexture(0, nullptr);
                m_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
                m_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
                m_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
                m_dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
            }

            const Submesh& sm = dc.mesh->submeshes[dc.submesh];
            SetAlphaTest(s.alphaTest);
            if (ambientOnly)
            {
                float bias = -0.0002f, slope = -2.0f;
                m_dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
                m_dev->SetRenderState(D3DRS_DEPTHBIAS, *(DWORD*)&bias);
                m_dev->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, *(DWORD*)&slope);
            }
            m_dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, dc.mesh->vertexCount, sm.firstIndex, sm.indexCount / 3);
            ResetMaterialRaster();
            SetAlphaTest(false);
            if (ambientOnly)
            {
                m_dev->LightEnable(0, TRUE);
                m_dev->SetRenderState(D3DRS_DEPTHBIAS, 0);
                m_dev->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, 0);
            }
        }

        struct FrameContext
        {
            Mat4 world, view, proj;
            Vec3 eye, sunDir, sunColor{ 0.95f, 0.92f, 0.86f }, ambient{ 0.22f, 0.22f, 0.24f };
            float time = 0;
        };

        static void Transposed(const Mat4& m, float* out)   // registers hold matrix columns
        {
            for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) out[r * 4 + c] = m.m[c][r];
        }

        // Fills `out` (4 floats per register) for engine-provided constants; returns false if unknown.
        bool FrameConstant(const std::string& n, float* out) const
        {
            auto vec = [&](float a, float b, float c, float d) { out[0] = a; out[1] = b; out[2] = c; out[3] = d; return true; };
            // The game renders camera-relative: the eye sits at the origin, so world positions double as view vectors.
            Mat4 world = m_frame.world * Mat4::Translation(m_frame.eye * -1.0f);
            Mat4 viewRot = m_frame.view;
            viewRot.m[3][0] = viewRot.m[3][1] = viewRot.m[3][2] = 0;
            Mat4 wv = world * viewRot, vp = viewRot * m_frame.proj, wvp = wv * m_frame.proj;
            if (n == "worldMatrix") { Transposed(world, out); return true; }
            if (n == "viewMatrix") { Transposed(viewRot, out); return true; }
            if (n == "projectionMatrix") { Transposed(m_frame.proj, out); return true; }
            if (n == "viewProjectionMatrix") { Transposed(vp, out); return true; }
            if (n == "worldViewMatrix") { Transposed(wv, out); return true; }
            if (n == "worldViewProjectionMatrix") { Transposed(wvp, out); return true; }
            if (n == "inverseWorldMatrix" || n == "inverseWorldViewMatrix") { Transposed(Mat4::Identity(), out); return true; }
            if (n == "eyePosition") return vec(0, 0, 0, 1);
            if (n == "sunPosition") return vec(m_frame.sunDir.x, m_frame.sunDir.y, m_frame.sunDir.z, 0);
            if (n == "sunDiffuse") return vec(m_frame.sunColor.x, m_frame.sunColor.y, m_frame.sunColor.z, 1);
            if (n == "sunSpecular") return vec(1, 1, 1, 1);
            if (n == "gameTime") return vec(m_frame.time, m_frame.time, m_frame.time, m_frame.time);
            if (n == "fogConsts") return vec(0, 0, 0, 0);
            if (n == "materialColor") return vec(0, 0, 0, 0);   // override colour; alpha 0 = no override
            if (n == "colorTint") return vec(1, 1, 1, 1);
            return false;
        }

        void UploadConstants(const std::vector<game::ShaderConstant>& consts, const ShaderPass& pass, bool pixel)
        {
            for (auto& c : consts)
            {
                if (c.regSet != 2) continue;   // float constants only
                float v[16 * 4] = {};
                int regs = std::min<int>(c.count, 16);
                bool have = false;
                for (auto& lit : pass.literals)
                    if (lit.name == c.name) { std::memcpy(v, lit.value.data(), 16); have = true; break; }
                if (!have) have = FrameConstant(c.name, v);
                if (!have) continue;
                if (pixel) m_dev->SetPixelShaderConstantF(c.reg, v, regs);
                else m_dev->SetVertexShaderConstantF(c.reg, v, regs);
            }
        }

        void SetFilter(int sampler, SurfaceStyle::Filter f)
        {
            using F = SurfaceStyle::Filter;
            if (f == F::NoMipNearest || f == F::NoMipLinear)
            {
                const DWORD tf = f == F::NoMipNearest ? D3DTEXF_POINT : D3DTEXF_LINEAR;
                m_dev->SetSamplerState(sampler, D3DSAMP_MINFILTER, tf);
                m_dev->SetSamplerState(sampler, D3DSAMP_MAGFILTER, tf);
                m_dev->SetSamplerState(sampler, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
                return;
            }
            // the game's mip filters: bilinear picks the nearest mip level, trilinear blends two; "2x"/"4x" is the anisotropy
            DWORD aniso = 8, mip = D3DTEXF_LINEAR;
            switch (f)
            {
            case F::Standard:    aniso = 2; mip = D3DTEXF_POINT; break;
            case F::Bilinear4x:  aniso = 4; mip = D3DTEXF_POINT; break;
            case F::Trilinear2x: aniso = 2; mip = D3DTEXF_LINEAR; break;
            case F::Trilinear4x: aniso = 4; mip = D3DTEXF_LINEAR; break;
            default: break;
            }
            m_dev->SetSamplerState(sampler, D3DSAMP_MINFILTER, D3DTEXF_ANISOTROPIC);
            m_dev->SetSamplerState(sampler, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
            m_dev->SetSamplerState(sampler, D3DSAMP_MIPFILTER, mip);
            m_dev->SetSamplerState(sampler, D3DSAMP_MAXANISOTROPY, std::min<DWORD>(aniso, m_caps.MaxAnisotropy));
        }

        // The material's own raster state, applied on top of the blend setup of the draw path (only for visible passes).
        void ApplyMaterialRaster(const SurfaceStyle& s)
        {
            if (s.customBlend)
            {
                bool on = s.opRgb != 0 || s.opAlpha != 0;
                m_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, on ? TRUE : FALSE);
                m_dev->SetRenderState(D3DRS_BLENDOP, s.opRgb ? s.opRgb : D3DBLENDOP_ADD);
                m_dev->SetRenderState(D3DRS_SRCBLEND, s.srcRgb);
                m_dev->SetRenderState(D3DRS_DESTBLEND, s.dstRgb);
                m_dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
                m_dev->SetRenderState(D3DRS_BLENDOPALPHA, s.opAlpha ? s.opAlpha : D3DBLENDOP_ADD);
                m_dev->SetRenderState(D3DRS_SRCBLENDALPHA, s.srcAlpha);
                m_dev->SetRenderState(D3DRS_DESTBLENDALPHA, s.dstAlpha);
                m_dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
            }
            else if (s.multiply) { m_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE); m_dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_DESTCOLOR); m_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ZERO); }
            else if (s.screen) { m_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE); m_dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE); m_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCCOLOR); }
            m_dev->SetRenderState(D3DRS_ZFUNC, s.noDepthTest ? D3DCMP_ALWAYS : s.depthFunc);
            if (s.depthWrite >= 0) m_dev->SetRenderState(D3DRS_ZWRITEENABLE, s.depthWrite ? TRUE : FALSE);
            if (s.polyOffset)
            {
                float bias = s.polyOffset == 1 ? -0.00002f : -0.00004f, slope = s.polyOffset == 1 ? -1.0f : -2.0f;
                m_dev->SetRenderState(D3DRS_DEPTHBIAS, *(DWORD*)&bias);
                m_dev->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, *(DWORD*)&slope);
            }
            if (s.colorMask != 0xF) m_dev->SetRenderState(D3DRS_COLORWRITEENABLE, s.colorMask);
        }
        void ResetMaterialRaster()
        {
            m_dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
            m_dev->SetRenderState(D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD);
            m_dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
            m_dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
            m_dev->SetRenderState(D3DRS_DEPTHBIAS, 0);
            m_dev->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, 0);
            m_dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
        }

        void SetAlphaTest(bool on)
        {
            m_dev->SetRenderState(D3DRS_ALPHATESTENABLE, on ? TRUE : FALSE);
            if (on)
            {
                m_dev->SetRenderState(D3DRS_ALPHAREF, 128);
                m_dev->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
            }
        }

        // ps_2_0:  texld r0, t0, s0 ; mul r0, r0, c0 ; mov oC0, r0   (colour map times a constant)
        IDirect3DPixelShader9* AmbientPixelShader()
        {
            if (!m_ambientPs)
            {
                static const DWORD code[] = {
                    0xFFFF0200,
                    0x0200001F, 0x80000000, 0xB0030000,            // dcl t0.xy
                    0x0200001F, 0x90000000, 0xA00F0800,            // dcl_2d s0
                    0x03000042, 0x800F0000, 0xB0E40000, 0xA0E40800,// texld r0, t0, s0
                    0x03000005, 0x800F0000, 0x80E40000, 0xA0E40000,// mul r0, r0, c0
                    0x02000001, 0x800F0800, 0x80E40000,            // mov oC0, r0
                    0x0000FFFF };
                m_dev->CreatePixelShader(code, &m_ambientPs);
            }
            return m_ambientPs.Get();
        }

        // Ambient light for a game-shader surface. It runs through the SAME vertex shader as the sunlight passes, so its
        // depth is bit-identical to theirs: a hidden surface can never pick up ambient light where its sunlight is rejected.
        enum class AmbientMode { Opaque, Cutout, ClearStencil };

        void DrawAmbientShaded(const DrawCall& dc, AmbientMode mode)
        {
            const SurfaceStyle& s = dc.style;
            const Submesh& sm = dc.mesh->submeshes[dc.submesh];
            auto* ps = AmbientPixelShader();
            if (!ps || s.shader->passes.empty()) return;
            const ShaderPass& pass = s.shader->passes.front();
            m_dev->SetStreamSource(0, dc.mesh->vb.Get(), 0, sizeof(Vertex));
            m_dev->SetStreamSource(1, dc.mesh->vbPacked.Get(), 0, 12);
            m_dev->SetIndices(dc.mesh->ib.Get());
            m_dev->SetRenderState(D3DRS_CULLMODE, s.CullMode());
            m_dev->SetRenderState(D3DRS_LIGHTING, FALSE);
            m_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
            m_dev->SetRenderState(D3DRS_ZENABLE, mode == AmbientMode::ClearStencil ? FALSE : TRUE);
            m_dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
            m_dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
            m_dev->SetRenderState(D3DRS_COLORWRITEENABLE, mode == AmbientMode::ClearStencil ? 0 : 0xF);
            SetAlphaTest(mode == AmbientMode::Opaque ? s.alphaTest : mode == AmbientMode::Cutout);
            if (mode == AmbientMode::Cutout)
            {
                m_dev->SetRenderState(D3DRS_ALPHAREF, s.alphaTest ? 128 : 8);
                if (s.blend || s.additive)
                {
                    m_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
                    m_dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
                    m_dev->SetRenderState(D3DRS_DESTBLEND, s.additive ? D3DBLEND_ONE : D3DBLEND_INVSRCALPHA);
                }
                else m_dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);   // hard cut-outs occlude what is behind them
            }
            if (mode != AmbientMode::Opaque)
            {
                m_dev->SetRenderState(D3DRS_STENCILENABLE, TRUE);
                m_dev->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_ALWAYS);
                m_dev->SetRenderState(D3DRS_STENCILPASS, mode == AmbientMode::Cutout ? D3DSTENCILOP_REPLACE : D3DSTENCILOP_ZERO);
                m_dev->SetRenderState(D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP);
                m_dev->SetRenderState(D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP);
                m_dev->SetRenderState(D3DRS_STENCILREF, 1);
                m_dev->SetRenderState(D3DRS_STENCILMASK, 0xFF);
                m_dev->SetRenderState(D3DRS_STENCILWRITEMASK, 0xFF);
            }
            m_dev->SetVertexDeclaration(pass.decl.Get());
            m_dev->SetVertexShader(pass.vs.Get());
            m_dev->SetPixelShader(ps);
            UploadConstants(pass.vsConsts, pass, false);
            float c0[4] = { m_frame.ambient.x * s.tint[0], m_frame.ambient.y * s.tint[1], m_frame.ambient.z * s.tint[2], 1.0f };
            m_dev->SetPixelShaderConstantF(0, c0, 1);
            std::shared_ptr<GpuTexture> color = s.color;
            for (auto& t : pass.textures) if (t.isColor && t.tex) color = t.tex;
            m_dev->SetTexture(0, color ? color->tex.Get() : nullptr);
            m_dev->SetSamplerState(0, D3DSAMP_ADDRESSU, s.tileU ? D3DTADDRESS_WRAP : D3DTADDRESS_CLAMP);
            m_dev->SetSamplerState(0, D3DSAMP_ADDRESSV, s.tileV ? D3DTADDRESS_WRAP : D3DTADDRESS_CLAMP);
            SetFilter(0, s.filter);
            m_dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, dc.mesh->vertexCount, sm.firstIndex, sm.indexCount / 3);
            m_dev->SetTexture(0, nullptr);
            SetAlphaTest(false);
            m_dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
            m_dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
            m_dev->SetRenderState(D3DRS_ZENABLE, TRUE);
            m_dev->SetStreamSource(1, nullptr, 0, 0);
            m_dev->SetVertexShader(nullptr);
            m_dev->SetPixelShader(nullptr);
        }

        enum class ShadePhase { Normal, DepthOnly, Additive };

        void DrawShaded(const DrawCall& dc, ShadePhase phase)
        {
            const SurfaceStyle& s = dc.style;
            const Submesh& sm = dc.mesh->submeshes[dc.submesh];
            m_dev->SetStreamSource(0, dc.mesh->vb.Get(), 0, sizeof(Vertex));
            m_dev->SetStreamSource(1, dc.mesh->vbPacked.Get(), 0, 12);
            m_dev->SetIndices(dc.mesh->ib.Get());
            m_dev->SetRenderState(D3DRS_CULLMODE, s.CullMode());
            m_dev->SetRenderState(D3DRS_LIGHTING, FALSE);
            m_dev->SetRenderState(D3DRS_COLORWRITEENABLE, phase == ShadePhase::DepthOnly ? 0 : 0xF);
            // cut-out materials (hair, fabric edges, foliage): the game alpha-tests them, so transparent texels must neither
            // draw nor write depth, otherwise they hide whatever is behind them
            SetAlphaTest(s.alphaTest);
            m_dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
            if (phase == ShadePhase::DepthOnly)
            {
                m_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
                m_dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
            }
            else if (phase == ShadePhase::Additive)
            {
                if (m_stencilEqual)
                {
                    m_dev->SetRenderState(D3DRS_STENCILENABLE, TRUE);
                    m_dev->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_EQUAL);
                    m_dev->SetRenderState(D3DRS_STENCILREF, 1);
                    m_dev->SetRenderState(D3DRS_STENCILMASK, 0xFF);
                    m_dev->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_KEEP);
                    m_dev->SetRenderState(D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP);
                    m_dev->SetRenderState(D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP);
                    SetAlphaTest(false);
                }
                m_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
                m_dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
                m_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
                m_dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
            }
            else
            {
                bool bl = s.blend || s.additive;
                m_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, bl ? TRUE : FALSE);
                m_dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
                m_dev->SetRenderState(D3DRS_DESTBLEND, s.additive ? D3DBLEND_ONE : D3DBLEND_INVSRCALPHA);
                m_dev->SetRenderState(D3DRS_ZWRITEENABLE, bl ? FALSE : TRUE);
                ApplyMaterialRaster(s);
            }
            for (auto& pass : s.shader->passes)
            {
                m_dev->SetVertexDeclaration(pass.decl.Get());
                m_dev->SetVertexShader(pass.vs.Get());
                m_dev->SetPixelShader(pass.ps.Get());
                UploadConstants(pass.vsConsts, pass, false);
                UploadConstants(pass.psConsts, pass, true);
                for (auto& t : pass.textures)
                {
                    m_dev->SetTexture(t.sampler, t.tex ? t.tex->tex.Get() : nullptr);
                    m_dev->SetSamplerState(t.sampler, D3DSAMP_ADDRESSU, t.clampU ? D3DTADDRESS_CLAMP : D3DTADDRESS_WRAP);
                    m_dev->SetSamplerState(t.sampler, D3DSAMP_ADDRESSV, t.clampV ? D3DTADDRESS_CLAMP : D3DTADDRESS_WRAP);
                    SetFilter(t.sampler, t.isColor ? s.filter : t.filter);
                }
                m_dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, dc.mesh->vertexCount, sm.firstIndex, sm.indexCount / 3);
                for (auto& t : pass.textures) m_dev->SetTexture(t.sampler, nullptr);
            }
            m_dev->SetStreamSource(1, nullptr, 0, 0);
            ResetMaterialRaster();
            m_dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
            SetAlphaTest(false);
            m_dev->SetVertexShader(nullptr);
            m_dev->SetPixelShader(nullptr);
        }

        ComPtr<IDirect3D9> m_d3d;
        ComPtr<IDirect3DDevice9> m_dev;
        D3DPRESENT_PARAMETERS m_pp{};
        D3DCAPS9 m_caps{};
        bool m_resizeRequested = false;
        ComPtr<IDirect3DPixelShader9> m_ambientPs;
        bool m_stencilEqual = false;
        FrameContext m_frame;
        std::map<std::string, std::shared_ptr<GpuTexture>> m_textures;
        std::map<std::string, std::string> m_textureErrors;
    };
}
