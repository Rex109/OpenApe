module;
#include <cstdlib>
#include <utility>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

export module openape.shading;

import openape.util;
import openape.gdf;
import openape.gdt;
import openape.project;
import openape.gamedata;
import openape.iwd;
import openape.image;
import openape.renderer;

// Turns a material (a GDT entry and/or a compiled raw material) into a SurfaceStyle that the
// renderer can draw, optionally wired to the game's own techset / technique / shaders.
export namespace oa
{
    enum class ShadeMode { Fixed, Fakelight, Unlit, Sun };

    inline const char* ShadeModeName(ShadeMode m)
    {
        switch (m)
        {
        case ShadeMode::Fixed: return "Built-in (fixed function)";
        case ShadeMode::Fakelight: return "Game: fakelight";
        case ShadeMode::Unlit: return "Game: unlit";
        case ShadeMode::Sun: return "Game: sunlight preview";
        }
        return "";
    }

    class MaterialShading
    {
    public:
        MaterialShading(Renderer& r, game::GameData& g, Project& p, const IwdArchive& iwds) : m_renderer(r), m_game(g), m_project(p), m_iwds(iwds) {}

        struct Result
        {
            SurfaceStyle style;
            std::vector<std::string> notes;
            std::string techset;   // what was used, for the status overlay
        };

        void ClearCache() { m_special.clear(); m_converted.clear(); }

        // "mip standard (2x bilinear)*" | "<auto filter>*" | "bilinear" | "trilinear" | "anisotropic",
        // "mip expensive (4x bilinear)", "mip more expensive (2x trilinear)", "mip most expensive (4x trilinear)",
        // "nomip nearest" | "nearest", "nomip bilinear" | "linear"   (a file may use any of the aliases)
        static SurfaceStyle::Filter ParseFilter(std::string f)
        {
            using F = SurfaceStyle::Filter;
            f = ToLower(f);
            if (!f.empty() && f.back() == '*') f.pop_back();
            if (f.rfind("nomip nearest", 0) == 0 || f == "nearest") return F::NoMipNearest;
            if (f.rfind("nomip bilinear", 0) == 0 || f == "linear") return F::NoMipLinear;
            if (f.find("most expensive") != std::string::npos) return F::Trilinear4x;
            if (f.find("more expensive") != std::string::npos || f == "trilinear") return F::Trilinear2x;
            if (f.find("expensive") != std::string::npos) return F::Bilinear4x;
            if (f.rfind("mip standard", 0) == 0 || f == "bilinear" || f == "<auto filter>") return F::Standard;
            if (f == "anisotropic") return F::Trilinear4x;
            return F::Default;
        }

        // "tile both*" | "tile horizontal" | "tile vertical" | "no tile"
        static void ParseTile(const std::string& t, bool& u, bool& v)
        {
            u = t.find("no tile") == std::string::npos && t.find("vertical") == std::string::npos;
            v = t.find("no tile") == std::string::npos && t.find("horizontal") == std::string::npos;
        }

        // blendFunc "Custom" factor / op names
        static int ParseBlendFactor(const std::string& n)
        {
            static const char* names[] = { "One", "Zero", "SrcColor", "InvSrcColor", "SrcAlpha", "InvSrcAlpha", "DestAlpha", "InvDestAlpha", "DestColor", "InvDestColor" };
            static const int d3d[] = { 2, 1, 3, 4, 5, 6, 7, 8, 9, 10 };
            for (int i = 0; i < 10; ++i) if (n.rfind(names[i], 0) == 0 && (n.size() == strlen(names[i]) || n[strlen(names[i])] == '*')) return d3d[i];
            return 2;
        }
        static int ParseBlendOp(const std::string& n)   // D3DBLENDOP, 0 = Disable
        {
            if (n.rfind("Subtract", 0) == 0) return 2;
            if (n.rfind("RevSubtract", 0) == 0) return 3;
            if (n.rfind("Min", 0) == 0) return 4;
            if (n.rfind("Max", 0) == 0) return 5;
            if (n.rfind("Disable", 0) == 0) return 0;
            return 1;
        }

        Result Build(const std::string& materialName, const gdt::Entry* gdt, ShadeMode mode)
        {
            Result r;
            m_mapState.clear();
            const game::CompiledMaterial* cm = materialName.empty() ? nullptr : m_game.FindMaterial(materialName);

            // textures by semantic: GDT paths win, compiled materials fill the gaps
            std::map<std::string, std::shared_ptr<GpuTexture>> maps;
            std::map<std::string, std::array<float, 4>> consts;
            std::map<std::string, std::string> missing;   // semantic -> why it is absent; only reported if nothing else supplies it
            if (gdt) FromGdt(*gdt, r, maps, consts, missing);
            if (cm) FromCompiled(*cm, r, maps, consts, missing, gdt);
            for (auto& [sem, why] : missing) if (!maps.count(sem)) r.notes.push_back(why);

            if (gdt && ToLower(gdt->GetString("materialType")) == "water")
            {
                // the height map the water shader reads is generated from the waterMap* options and animated every frame
                auto& w = r.style.water;
                w.on = true;
                w.length = std::max(1.0f, (float)gdt->GetNumber("waterMapHorizontalWorldLength"));
                w.amplitude = (float)gdt->GetNumber("waterMapAmplitude");
                w.windX = (float)gdt->GetNumber("waterMapWindDirectionX"); w.windY = (float)gdt->GetNumber("waterMapWindDirectionY");
                w.windSpeed = std::max(1.0f, (float)gdt->GetNumber("waterMapWindSpeed"));
                w.width = std::clamp(gdt->GetInt("waterMapTextureWidth"), 16, 128);
                ImageData img;
                img.width = img.height = w.width; img.format = PixelFormat::BGRA8;
                img.pixels.assign((size_t)w.width * w.width * 4, 128);
                w.tex = m_renderer.CreateTexture(img);
                if (w.tex) maps["normalMap"] = w.tex;
                UpdateWater(r.style, 0.0);
            }
            if (!maps.count("colorMap")) maps["colorMap"] = Special("$white");
            if (!maps.count("normalMap")) maps["normalMap"] = Special("$identitynormalmap");
            if (!maps.count("specularMap")) maps["specularMap"] = Special("$gray");
            if (!maps.count("detailMap")) maps["detailMap"] = Special("$white");
            r.style.color = maps["colorMap"];
            if (!consts.count("colorTint")) consts["colorTint"] = { 1, 1, 1, 1 };
            r.style.tint = consts["colorTint"];

            // compiled materials mark alpha testing in the techset name (l_sm_a0c0, ...)
            if (!gdt && cm && (cm->techset.find("_a0") != std::string::npos || cm->techset.rfind("a0", 0) == 0)) r.style.alphaTest = true;

            if (mode == ShadeMode::Fixed || !m_game.Available()) return r;

            // The GDT is what the user is editing: its material type and maps decide the techset, so changing them changes the
            // preview. The compiled material (which may be out of date) is only used when the GDT does not name a known techset.
            std::string fromGdt = gdt ? TechsetForGdt(*gdt) : std::string();
            std::string techset = !fromGdt.empty() ? fromGdt : cm ? cm->techset : GuessTechset(gdt);
            r.techset = techset;
            bool sm_additive = false;
            // not every techset implements every technique; fall back to the closest one that exists
            std::vector<const char*> wanted;
            switch (mode)
            {
            case ShadeMode::Sun: wanted = { "sunlight preview", "fakelight normal", "unlit" }; break;
            case ShadeMode::Fakelight: wanted = { "fakelight normal", "fakelight view", "unlit" }; break;
            default: wanted = { "unlit", "fakelight normal" }; break;
            }
            const game::Technique* tech = nullptr;
            const char* techName = wanted.front();
            if (!m_game.FindTechset(techset))
            {
                // custom techset that is not in raw/techsets: guess the stock one from the material's maps
                r.notes.push_back("techset '" + techset + "' not found, using " + GuessTechset(gdt));
                techset = GuessTechset(gdt);
            }
            // water has its own sun-lit technique (there is no editor "sunlight preview" for it)
            if (techset == "water" || techset == "w_water") { wanted = { "lit sun", "unlit", "fakelight normal" }; techName = wanted.front(); }
            // unlit and effect techsets have no lighting to preview: their own unlit technique is the material
            if (techset.find("unlit") != std::string::npos || techset.rfind("effect", 0) == 0) { wanted = { "unlit", "fakelight normal" }; techName = wanted.front(); }
            // A material whose in-game lit and unlit techniques are one and the same shader (a custom effect that ignores the
            // lights) looks like that in the game; the editor's own "sunlight preview" technique would show a stock lit shader.
            if (mode == ShadeMode::Sun)
            {
                const game::Technique* unlit = m_game.FindTechnique(techset, "unlit");
                const game::Technique* litSun = m_game.FindTechnique(techset, "lit sun");
                if (unlit && litSun && unlit->file == litSun->file && techset.find("unlit") == std::string::npos)
                {
                    wanted = { "unlit", "fakelight normal" };
                    techName = wanted.front();
                    r.notes.push_back("this material does not react to lighting in the game (lit and unlit are the same shader), showing it unlit");
                }
            }
            for (const char* w : wanted)
                if ((tech = m_game.FindTechnique(techset, w)) != nullptr) { techName = w; break; }
            if (!tech)
            {
                r.notes.push_back("techset '" + techset + "' has no " + std::string(wanted.front()) + " technique - using built-in shading");
                return r;
            }
            if (techName != wanted.front()) r.notes.push_back(std::string("'") + wanted.front() + "' unavailable in this techset, showing '" + techName + "'");
            sm_additive = std::string(techName) == "sunlight preview";

            auto sm = std::make_shared<ShaderMaterial>();
            sm->additive = sm_additive;
            for (auto& pass : tech->passes)
            {
                const game::ShaderCode* vs = m_game.Shader(true, pass.vertex.version, pass.vertex.name);
                const game::ShaderCode* ps = m_game.Shader(false, pass.pixel.version, pass.pixel.name);
                if (!vs || !ps)
                {
                    r.notes.push_back("shader not found: " + (!vs ? pass.vertex.name : pass.pixel.name));
                    return r;
                }
                ShaderPass sp;
                std::string why;
                if (!m_renderer.CreateShaderPass(*vs, *ps, pass.vertexInputs, sp, &why))
                {
                    r.notes.push_back("could not create shaders vs_" + pass.vertex.version + " " + pass.vertex.name + " / ps_" + pass.pixel.version + " " + pass.pixel.name + ": " + why);
                    return r;
                }
                sp.label = "vs_" + pass.vertex.version + " " + pass.vertex.name + " + ps_" + pass.pixel.version + " " + pass.pixel.name;
                for (auto* ref : { &pass.vertex, &pass.pixel })
                {
                    const game::ShaderCode* code = ref == &pass.vertex ? vs : ps;
                    for (auto& b : ref->bindings) Bind(b, *code, maps, consts, sp);
                }
                // constants the technique does not bind (envMapParms for the specular term, ...) are filled from the material's
                // constant table by name, as the game does
                for (auto* code : { vs, ps })
                    for (auto& c : code->constants)
                    {
                        if (c.regSet != 2) continue;
                        bool have = false;
                        for (auto& l : sp.literals) if (l.name == c.name) have = true;
                        if (have) continue;
                        auto it = consts.find(c.name);
                        if (it != consts.end()) sp.literals.push_back({ c.name, it->second });
                    }
                // samplers the technique does not bind are fed by the engine at run time (reflection probes, ...): give them a
                // neutral texture instead of leaving whatever was bound last
                for (auto& c : ps->constants)
                {
                    if (c.regSet != 3) continue;
                    bool bound = false;
                    for (auto& t : sp.textures) if (t.sampler == (int)c.reg) bound = true;
                    if (!bound)
                        sp.textures.push_back({ (int)c.reg, Special(c.name.find("reflection") != std::string::npos ? "$skyprobe" : "$white"), false, false, false });
                }
                sm->passes.push_back(std::move(sp));
            }
            r.style.shader = std::move(sm);
            return r;
        }

        // Fills the water height map for time `t` (seconds): a sum of travelling waves whose wave numbers tile the map and
        // whose strength follows a wind-driven spectrum (long waves along the wind dominate), like the game's generator.
        void UpdateWater(const SurfaceStyle& s, double t)
        {
            const auto& w = s.water;
            if (!w.on || !w.tex) return;
            struct Wave { float kx, ky, amp, omega, phase; };
            static std::vector<Wave> base;
            std::vector<Wave> waves;
            const float g = 386.0f, tau = 6.2831853f;
            float wl = std::sqrt(w.windX * w.windX + w.windY * w.windY);
            float wx = wl > 1e-4f ? w.windX / wl : 1.0f, wy = wl > 1e-4f ? w.windY / wl : 0.0f;
            float largest = w.windSpeed * w.windSpeed / g;
            uint32_t seed = 12345;
            auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / 16777216.0f; };
            float sum = 0;
            for (int i = 0; i < 32; ++i)
            {
                int nx = (int)std::lround(rnd() * 12 - 6), ny = (int)std::lround(rnd() * 12 - 6);
                if (nx == 0 && ny == 0) { nx = 1; }
                float kx = tau * nx / w.length, ky = tau * ny / w.length, k = std::sqrt(kx * kx + ky * ky);
                float align = (kx * wx + ky * wy) / k;
                float spectrum = std::exp(-1.0f / (k * k * largest * largest)) / (k * k * k * k) * align * align;
                float amp = std::pow(std::max(spectrum, 1e-30f), 0.2f) * (0.6f + 0.8f * rnd());   // flattened so the small ripples show too
                waves.push_back({ kx, ky, amp, std::sqrt(g * k), rnd() * tau });
                sum += amp;
            }
            if (sum <= 0) return;
            float norm = std::clamp(w.amplitude * 14.0f, 0.2f, 0.8f) / sum * 2.5f;
            std::vector<uint8_t> px((size_t)w.width * w.width * 4);
            for (int y = 0; y < w.width; ++y)
                for (int x = 0; x < w.width; ++x)
                {
                    float wx2 = w.length * x / w.width, wy2 = w.length * y / w.width, h = 0;
                    for (auto& wv : waves) h += wv.amp * std::cos(wv.kx * wx2 + wv.ky * wy2 - wv.omega * (float)t * 0.5f + wv.phase);
                    uint8_t c = (uint8_t)std::clamp((int)((0.5f + h * norm) * 255.0f), 0, 255);
                    size_t o = ((size_t)y * w.width + x) * 4;
                    px[o] = px[o + 1] = px[o + 2] = c; px[o + 3] = 255;
                }
            m_renderer.UpdateTexture(*w.tex, px);
        }

        // The techset the game's material tool picks for a GDT material, derived from the compiled materials of the stock
        // tree: "l_sm_" + r/b/a/t (replace, blend, add, alpha test) + "c0" + d0 n0 s0 for the detail / normal / specular maps,
        // "unlit", "2d", "effect", "sky", and a custom material's customString. Empty when no such techset exists.
        std::string TechsetForGdt(const gdt::Entry& e) const
        {
            std::string type = ToLower(e.GetString("materialType"));
            auto first = [&](const std::vector<std::string>& names) {
                for (auto& n : names) if (m_game.TechsetExists(n)) return n;
                return std::string();
            };
            if (type == "custom")
            {
                std::string cs = e.GetString("customString");
                return !cs.empty() && m_game.TechsetExists(cs) ? cs : std::string();
            }
            if (type == "2d") return first({ "2d" });
            if (type == "effect") return first({ "effect" });
            if (type == "sky") return first({ "sky" });
            if (type == "water") return first({ "water", "w_water" });
            if (type.find("unlit") != std::string::npos)
            {
                // textured unlit materials use the w_ / m_ variants; "unlit" alone is the untextured vertex-colour one
                std::string blend = e.GetString("blendFunc");
                std::string suffix = blend.rfind("Blend", 0) == 0 ? "_blend" : blend.rfind("Add", 0) == 0 ? "_add"
                                   : e.GetString("alphaTest").rfind("GE128", 0) == 0 ? "_alphatest" : "";
                std::string pre = type.rfind("model", 0) == 0 ? "m_" : "w_";
                bool textured = !e.GetString("colorMap").empty();
                if (textured) return first({ pre + "unlit" + suffix, "m_unlit" + suffix, pre + "unlit", "m_unlit", "unlit" });
                return first({ "unlit" });
            }
            if (type == "world phong" || type == "model phong" || type == "model ambient")
            {
                std::string blend = e.GetString("blendFunc");
                char mode = 'r';
                if (blend.rfind("Blend", 0) == 0) mode = 'b';
                else if (blend.rfind("Add", 0) == 0) mode = 'a';
                else if (e.GetString("alphaTest").rfind("GE128", 0) == 0) mode = 't';
                bool det = !e.GetString("detailMap").empty(), nrm = !e.GetString("normalMap").empty();
                bool spc = !e.GetString("specColorMap").empty() || !e.GetString("cosinePowerMap").empty();
                std::string base = std::string(1, mode) + "0c0";
                std::vector<std::string> names;
                for (const char* pre : { "l_sm_scroll_", "l_sm_" })
                {
                    if (pre[5] == 's' && e.GetInt("texScroll") == 0) continue;   // the scrolling variants only for texScroll
                    std::string d = det ? "d0" : "", n = nrm ? "n0" : "", s = spc ? "s0" : "";
                    names.push_back(pre + base + d + n + s);
                    names.push_back(pre + base + d + n);
                    names.push_back(pre + base + n + s);
                    names.push_back(pre + base + n);
                    names.push_back(pre + base + s);
                    names.push_back(pre + base);
                }
                return first(names);
            }
            return {};
        }

        // First of the plausible techset names that exists in raw/techsets.
        std::string GuessTechset(const gdt::Entry* e)
        {
            std::string type = e ? e->GetString("materialType") : "world phong";
            bool model = type.find("model") != std::string::npos;
            std::string pre = model ? "m_" : "w_";
            bool nrm = e && !e->GetString("normalMap").empty();
            bool spc = e && !e->GetString("specColorMap").empty();
            std::vector<std::string> names;
            if (type.find("unlit") != std::string::npos || type == "effect")
                names = { pre + "unlit", "m_unlit", "wc_unlit" };
            else
                names = { pre + "l_sm_r0c0" + (nrm ? "n0" : "") + (spc ? "s0" : ""), pre + "l_sm_r0c0n0s0", pre + "l_sm_r0c0", "mc_l_sm_r0c0", "wc_l_sm_r0c0" };
            for (auto& n : names) if (m_game.TechsetExists(n)) return n;
            return names.front();
        }

    private:
        void FromGdt(const gdt::Entry& e, Result& r, std::map<std::string, std::shared_ptr<GpuTexture>>& maps, std::map<std::string, std::array<float, 4>>& consts, std::map<std::string, std::string>& missing)
        {
            r.style.specular = e.GetString("materialType").find("phong") != std::string::npos ? 0.45f : 0.08f;
            ParseTile(e.GetString("tileColor"), r.style.tileU, r.style.tileV);
            r.style.filter = ParseFilter(e.GetString("filterColor"));
            // per-map tiling and filtering (only the shader path samples those maps)
            {
                MapState ms;
                ParseTile(e.GetString("tileColor"), ms.tileU, ms.tileV); ms.filter = r.style.filter; m_mapState["colorMap"] = ms;
                ParseTile(e.GetString("tileNormal"), ms.tileU, ms.tileV); ms.filter = ParseFilter(e.GetString("filterNormal")); m_mapState["normalMap"] = ms;
                ParseTile(e.GetString("tileSpecular"), ms.tileU, ms.tileV); ms.filter = ParseFilter(e.GetString("filterSpecular")); m_mapState["specularMap"] = ms;
                ms.tileU = ms.tileV = true; ms.filter = ParseFilter(e.GetString("filterDetail")); m_mapState["detailMap"] = ms;
            }
            {
                double ss = e.GetNumber("specColorStrength"), cs = e.GetNumber("cosinePowerStrength");
                r.style.specStrength = ss > 0 ? (float)std::clamp(ss / 100.0, 0.1, 1.0) : 1.0f;
                r.style.powerStrength = cs > 0 ? (float)std::clamp(cs / 100.0, 0.1, 1.0) : 1.0f;
                r.style.scroll = e.GetInt("texScroll") != 0;
                r.style.atlasRows = std::clamp(e.GetInt("textureAtlasRowCount"), 1, 64);
                r.style.atlasCols = std::clamp(e.GetInt("textureAtlasColumnCount"), 1, 64);
                if (e.GetInt("nopicmipColor") || e.GetInt("nopicmipDetail") || e.GetInt("nopicmipNormal") || e.GetInt("nopicmipSpecular"))
                    r.notes.push_back("nopicmip: the preview always shows the full resolution (picmip is not simulated)");
            }
            std::string blend = e.GetString("blendFunc");
            r.style.blend = blend.rfind("Blend", 0) == 0 || blend.rfind("Multiply", 0) == 0;
            r.style.additive = blend.rfind("Add", 0) == 0 || blend.rfind("Screen", 0) == 0;
            r.style.doubleSided = e.GetString("cullFace").rfind("None", 0) == 0;
            r.style.cullFront = e.GetString("cullFace").rfind("Front", 0) == 0;
            r.style.alphaTest = e.GetString("alphaTest").rfind("GE128", 0) == 0;
            if (blend.rfind("Custom", 0) == 0)
            {
                auto& st = r.style;
                st.customBlend = true; st.blend = true;
                st.opRgb = ParseBlendOp(e.GetString("customBlendOpRgb")); st.opAlpha = ParseBlendOp(e.GetString("customBlendOpAlpha"));
                st.srcRgb = ParseBlendFactor(e.GetString("srcCustomBlendFunc")); st.dstRgb = ParseBlendFactor(e.GetString("destCustomBlendFunc"));
                st.srcAlpha = ParseBlendFactor(e.GetString("srcCustomBlendFuncAlpha")); st.dstAlpha = ParseBlendFactor(e.GetString("destCustomBlendFuncAlpha"));
                st.additive = false;
            }
            r.style.multiply = blend.rfind("Multiply", 0) == 0;
            r.style.screen = blend.rfind("Screen", 0) == 0;
            {
                std::string dt = e.GetString("depthTest");
                if (dt.rfind("Less", 0) == 0 && dt.rfind("LessEqual", 0) != 0) r.style.depthFunc = 2; // D3DCMP_LESS
                else if (dt.rfind("Equal", 0) == 0) r.style.depthFunc = 3; // D3DCMP_EQUAL
                else if (dt.rfind("Always", 0) == 0 || dt.rfind("Disable", 0) == 0) r.style.noDepthTest = true;
                std::string dw = e.GetString("depthWrite");
                r.style.depthWrite = dw.rfind("On", 0) == 0 ? 1 : dw.rfind("Off", 0) == 0 ? 0 : -1;
                std::string po = e.GetString("polygonOffset");
                r.style.polyOffset = po.rfind("Static", 0) == 0 ? 1 : po.rfind("Weapon", 0) == 0 ? 2 : 0;
                unsigned mask = 0xF;
                if (e.GetString("colorWriteRed").rfind("Disable", 0) == 0) mask &= ~1u;
                if (e.GetString("colorWriteGreen").rfind("Disable", 0) == 0) mask &= ~2u;
                if (e.GetString("colorWriteBlue").rfind("Disable", 0) == 0) mask &= ~4u;
                if (e.GetString("colorWriteAlpha").rfind("Disable", 0) == 0) mask &= ~8u;
                r.style.colorMask = mask;
            }
            consts["colorTint"] = e.GetColor("colorTint");
            // the 4th value is the specular intensity the lit shaders multiply the sun's specular with: 0.625 in every stock material
            consts["envMapParms"] = { (float)e.GetNumber("envMapMin"), (float)e.GetNumber("envMapMax"), (float)e.GetNumber("envMapExponent"), 0.625f * r.style.specStrength };
            consts["detailScale"] = { (float)e.GetNumber("detailScaleX"), (float)e.GetNumber("detailScaleY"), 0, 0 };
            consts["distortionScale"] = { (float)e.GetNumber("distortionScaleX"), (float)e.GetNumber("distortionScaleY"), 0, 0 };
            consts["waterColor"] = { (float)e.GetNumber("waterColorR"), (float)e.GetNumber("waterColorG"), (float)e.GetNumber("waterColorB"), 1 };

            struct Map { const char* key; const char* semantic; };
            for (auto m : { Map{ "colorMap", "colorMap" }, Map{ "normalMap", "normalMap" }, Map{ "detailMap", "detailMap" } })
            {
                std::string path = e.GetString(m.key);
                if (path.empty()) continue;
                if (path[0] == '$') { maps[m.semantic] = Special(path); continue; }
                auto file = m_project.ResolveFile(path);
                if (file.empty()) { missing[m.semantic] = std::string(m.key) + " not found: " + path; continue; }
                std::string err;
                // the game stores normal maps in its own packed layout; a plain RGB normal map from the tools is converted
                auto tex = std::string(m.key) == "normalMap" ? PackedNormalMap(file, err) : m_renderer.LoadTexture(file, err);
                if (tex) maps[m.semantic] = tex; else missing[m.semantic] = file.filename().string() + ": " + err;
            }
            {
                // the specular map the shaders read is the specular colour with the cosine power (gloss) in its alpha
                std::string sp = e.GetString("specColorMap"), cp = e.GetString("cosinePowerMap");
                if (!sp.empty() || !cp.empty())
                {
                    std::string err;
                    auto tex = SpecularWithGloss(sp, cp, err);
                    if (tex) maps["specularMap"] = tex; else missing["specularMap"] = err;
                }
            }
        }

        void FromCompiled(const game::CompiledMaterial& cm, Result& r, std::map<std::string, std::shared_ptr<GpuTexture>>& maps, std::map<std::string, std::array<float, 4>>& consts, std::map<std::string, std::string>& missing, const gdt::Entry* gdt)
        {
            for (auto& t : cm.textures)
            {
                if (maps.count(t.semantic) || t.image.empty()) continue;
                // the GDT is what the user edits: a map whose field is empty there stays removed even if the compiled material
                // still has it. A map the GDT names but whose source file is missing still falls back to the compiled one.
                if (gdt)
                {
                    bool emptyInGdt = (t.semantic == "colorMap" && gdt->GetString("colorMap").empty()) ||
                                      (t.semantic == "normalMap" && gdt->GetString("normalMap").empty()) ||
                                      (t.semantic == "detailMap" && gdt->GetString("detailMap").empty()) ||
                                      (t.semantic == "specularMap" && gdt->GetString("specColorMap").empty() && gdt->GetString("cosinePowerMap").empty());
                    if (emptyInGdt) continue;
                }
                auto tex = ImageByName(t.image);
                if (tex) maps[t.semantic] = tex;
                else if (!missing.count(t.semantic)) missing[t.semantic] = "image not found: " + t.image;
            }
            for (auto& c : cm.constants) if (!consts.count(c.name)) consts[c.name] = c.value;
        }

        // decodes an image file to BGRA8 pixels
        bool LoadBgra(const fs::path& file, ImageData& img, std::string& err) const
        {
            if (!ImageLoader::Load(file, img, err)) return false;
            if (!ImageLoader::ToBgra8(img)) { err = file.filename().string() + ": cannot decode " + img.description; return false; }
            return true;
        }

        static std::string Stamp(const fs::path& file)
        {
            std::error_code ec;
            return file.string() + "|" + std::to_string(fs::last_write_time(file, ec).time_since_epoch().count());
        }

        // RGB tangent-space normal map -> the game's layout: x in alpha, y in green, both as slopes (x/z, y/z) scaled so that
        // 130/255 and 129/255 are zero, the way the lit shaders decode them (n = x*T + y*B + N, normalised)
        std::shared_ptr<GpuTexture> PackedNormalMap(const fs::path& file, std::string& err)
        {
            std::string key = "normal|" + Stamp(file);
            if (auto it = m_converted.find(key); it != m_converted.end()) return it->second;
            ImageData img;
            if (!LoadBgra(file, img, err)) { m_converted[key] = nullptr; return nullptr; }
            for (size_t i = 0; i + 3 < img.pixels.size(); i += 4)
            {
                float nx = img.pixels[i + 2] / 127.5f - 1.0f, ny = img.pixels[i + 1] / 127.5f - 1.0f, nz = std::max(img.pixels[i] / 127.5f - 1.0f, 0.25f);
                float a = 130.0f / 255.0f + (nx / nz) / 4.08f, g = 129.0f / 255.0f + (ny / nz) / 4.06452f;
                img.pixels[i + 3] = (uint8_t)std::clamp((int)std::lround(a * 255.0f), 0, 255);
                img.pixels[i + 1] = (uint8_t)std::clamp((int)std::lround(g * 255.0f), 0, 255);
                img.pixels[i] = 255; img.pixels[i + 2] = 128;
            }
            img.hasAlpha = true;
            auto t = m_renderer.CreateTexture(img);
            if (!t) err = file.filename().string() + ": could not create the normal map";
            m_converted[key] = t;
            return t;
        }

        // specular colour (RGB) + cosine power map (grey) in the alpha channel
        std::shared_ptr<GpuTexture> SpecularWithGloss(const std::string& specPath, const std::string& cosPath, std::string& err)
        {
            auto solid = [&](const std::string& n, uint8_t& v) { if (n == "$black") v = 0; else if (n == "$gray" || n == "$grey") v = 128; else v = 255; };
            ImageData spec, cosine;
            bool haveSpec = false, haveCos = false;
            fs::path specFile, cosFile;
            uint8_t specValue = 255;
            if (!specPath.empty())
            {
                if (specPath[0] == '$') solid(specPath, specValue);
                else
                {
                    specFile = m_project.ResolveFile(specPath);
                    if (specFile.empty()) { err = "specColorMap not found: " + specPath; return nullptr; }
                    if (!LoadBgra(specFile, spec, err)) return nullptr;
                    haveSpec = true;
                }
            }
            if (!cosPath.empty() && cosPath[0] != '$')
            {
                cosFile = m_project.ResolveFile(cosPath);
                if (cosFile.empty()) { err = "cosinePowerMap not found: " + cosPath; return nullptr; }
                if (!LoadBgra(cosFile, cosine, err)) return nullptr;
                haveCos = true;
            }
            std::string key = "spec|" + specPath + "|" + cosPath + "|" + (haveSpec ? Stamp(specFile) : "") + "|" + (haveCos ? Stamp(cosFile) : "");
            if (auto it = m_converted.find(key); it != m_converted.end()) return it->second;
            ImageData out;
            out.format = PixelFormat::BGRA8;
            out.width = haveSpec ? spec.width : haveCos ? cosine.width : 4;
            out.height = haveSpec ? spec.height : haveCos ? cosine.height : 4;
            out.pixels.resize((size_t)out.width * out.height * 4);
            for (int y = 0; y < out.height; ++y)
                for (int x = 0; x < out.width; ++x)
                {
                    uint8_t* o = &out.pixels[((size_t)y * out.width + x) * 4];
                    if (haveSpec) { const uint8_t* s = &spec.pixels[((size_t)y * spec.width + x) * 4]; o[0] = s[0]; o[1] = s[1]; o[2] = s[2]; }
                    else o[0] = o[1] = o[2] = specValue;
                    if (haveCos)
                    {
                        int cx = x * cosine.width / out.width, cy = y * cosine.height / out.height;
                        o[3] = cosine.pixels[((size_t)cy * cosine.width + cx) * 4 + 2];   // the grey value (red)
                    }
                    else o[3] = 128;   // a medium gloss when no cosine power map is given
                }
            out.hasAlpha = true;
            auto t = m_renderer.CreateTexture(out);
            if (!t) err = "could not create the specular map";
            m_converted[key] = t;
            return t;
        }

        std::shared_ptr<GpuTexture> ImageByName(const std::string& name)
        {
            if (!name.empty() && name[0] == '$') return Special(name);
            if (name == "white" || name == "black" || name == "gray" || name == "grey") return Special("$" + name);
            for (auto sub : { "raw/images", "raw/images_pc", "texture_assets" })
                for (auto ext : { ".iwi", ".dds", ".tga", ".png" })
                {
                    fs::path p = m_project.Root() / sub / (name + ext);
                    std::error_code ec;
                    if (fs::is_regular_file(p, ec))
                    {
                        std::string err;
                        if (auto t = m_renderer.LoadTexture(p, err)) return t;
                    }
                }
            // not in raw/images: the game loads it from the .iwd archives
            std::vector<unsigned char> bytes;
            std::string err;
            if (m_iwds.ReadImage(name, bytes)) return m_renderer.LoadTextureFromMemory("iwd:" + ToLower(name), bytes, ".iwi", err);
            return nullptr;
        }

        // Engine built-in images ("$white", "$identitynormalmap", ...) are generated.
        std::shared_ptr<GpuTexture> Special(const std::string& name)
        {
            auto it = m_special.find(name);
            if (it != m_special.end()) return it->second;
            uint8_t b = 255, g = 255, r = 255, a = 255;
            if (name == "$black") { b = g = r = 0; }
            else if (name == "$gray" || name == "$grey") { b = g = r = 128; }
            else if (name == "$skyprobe") { b = 225; g = 185; r = 150; }   // a pale sky blue standing in for a reflection probe
            else if (name == "$waterheight")
            {
                // tileable ripples: the water shader reads the red channel as the height of the surface
                ImageData w;
                w.width = w.height = 128; w.format = PixelFormat::BGRA8;
                const float tau = 6.2831853f;
                for (int y = 0; y < 128; ++y)
                    for (int x = 0; x < 128; ++x)
                    {
                        float u = x / 128.0f, v = y / 128.0f;
                        float h = 0.5f + 0.16f * std::sin(tau * (3 * u + 2 * v)) + 0.12f * std::sin(tau * (5 * u - 4 * v + 0.3f))
                                + 0.08f * std::sin(tau * (9 * u + 7 * v + 0.6f)) + 0.05f * std::sin(tau * (14 * u - 11 * v));
                        uint8_t c = (uint8_t)std::clamp((int)(h * 255.0f), 0, 255);
                        w.pixels.push_back(c); w.pixels.push_back(c); w.pixels.push_back(c); w.pixels.push_back(255);
                    }
                auto t = m_renderer.CreateTexture(w);
                m_special[name] = t;
                return t;
            }
            else if (name == "$identitynormalmap") { b = 255; g = 129; r = 128; a = 130; }   // the game stores normal x in alpha, scaled so that 130 == 0
            ImageData img;
            img.width = img.height = 4; img.format = PixelFormat::BGRA8;
            for (int i = 0; i < 16; ++i) { img.pixels.push_back(b); img.pixels.push_back(g); img.pixels.push_back(r); img.pixels.push_back(a); }
            auto t = m_renderer.CreateTexture(img);
            m_special[name] = t;
            return t;
        }

        void Bind(const game::ShaderBinding& b, const game::ShaderCode& code, std::map<std::string, std::shared_ptr<GpuTexture>>& maps,
                  std::map<std::string, std::array<float, 4>>& consts, ShaderPass& sp)
        {
            const std::string& src = b.source;
            const game::ShaderConstant* c = code.Find(b.target);
            if (c && c->regSet == 3)   // sampler
            {
                std::shared_ptr<GpuTexture> tex;
                if (src.rfind("material.", 0) == 0)
                {
                    std::string sem = src.substr(9);
                    auto it = maps.find(sem);
                    if (it != maps.end()) tex = it->second;
                    else if (sem.rfind("normalMap", 0) == 0) tex = Special("$identitynormalmap");
                    else tex = Special("$white");
                }
                else tex = Special("$white");   // sampler.white / feedback / outdoor ... have no preview equivalent
                bool clampU = false, clampV = false;
                SurfaceStyle::Filter filter = SurfaceStyle::Filter::Default;
                if (src.rfind("material.", 0) == 0)
                {
                    auto ms = m_mapState.find(src.substr(9));
                    if (ms != m_mapState.end()) { clampU = !ms->second.tileU; clampV = !ms->second.tileV; filter = ms->second.filter; }
                }
                sp.textures.push_back({ (int)c->reg, tex, clampU, clampV, src == "material.colorMap", filter });
                return;
            }
            if (src.rfind("material.", 0) == 0)
            {
                auto it = consts.find(src.substr(9));
                if (it != consts.end()) sp.literals.push_back({ b.target, it->second });
                return;
            }
            if (src.rfind("float4", 0) == 0 || src.rfind("float", 0) == 0)
            {
                size_t open = src.find('(');
                if (open == std::string::npos) return;
                std::array<float, 4> v{};
                const char* p = src.c_str() + open + 1;
                for (int i = 0; i < 4; ++i)
                {
                    char* end; v[i] = std::strtof(p, &end);
                    if (end == p) break;
                    p = end; while (*p == ',' || *p == ' ') ++p;
                }
                sp.literals.push_back({ b.target, v });
            }
        }

        Renderer& m_renderer;
        game::GameData& m_game;
        Project& m_project;
        const IwdArchive& m_iwds;
        std::map<std::string, std::shared_ptr<GpuTexture>> m_special;
        std::map<std::string, std::shared_ptr<GpuTexture>> m_converted;   // normal / specular maps built from GDT images
        struct MapState { bool tileU = true, tileV = true; SurfaceStyle::Filter filter = SurfaceStyle::Filter::Default; };
        std::map<std::string, MapState> m_mapState;   // by semantic, filled from the GDT of the material being built
    };
}
