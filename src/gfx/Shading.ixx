module;

#include <array>
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

        void ClearCache() { m_special.clear(); }

        Result Build(const std::string& materialName, const gdt::Entry* gdt, ShadeMode mode)
        {
            Result r;
            const game::CompiledMaterial* cm = materialName.empty() ? nullptr : m_game.FindMaterial(materialName);

            // textures by semantic: GDT paths win, compiled materials fill the gaps
            std::map<std::string, std::shared_ptr<GpuTexture>> maps;
            std::map<std::string, std::array<float, 4>> consts;
            std::map<std::string, std::string> missing;   // semantic -> why it is absent; only reported if nothing else supplies it
            if (gdt) FromGdt(*gdt, r, maps, consts, missing);
            if (cm) FromCompiled(*cm, r, maps, consts, missing);
            for (auto& [sem, why] : missing) if (!maps.count(sem)) r.notes.push_back(why);

            if (!maps.count("colorMap")) maps["colorMap"] = Special("$white");
            if (!maps.count("normalMap")) maps["normalMap"] = Special("$identitynormalmap");
            if (!maps.count("specularMap")) maps["specularMap"] = Special("$gray");
            if (!maps.count("detailMap")) maps["detailMap"] = Special("$white");
            r.style.color = maps["colorMap"];
            if (!consts.count("colorTint")) consts["colorTint"] = { 1, 1, 1, 1 };
            r.style.tint = consts["colorTint"];

            // compiled materials mark alpha testing in the techset name (l_sm_a0c0, ...)
            if (cm && (cm->techset.find("_a0") != std::string::npos || cm->techset.rfind("a0", 0) == 0)) r.style.alphaTest = true;

            if (mode == ShadeMode::Fixed || !m_game.Available()) return r;

            std::string techset = cm ? cm->techset : GuessTechset(gdt);
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
                sm->passes.push_back(std::move(sp));
            }
            r.style.shader = std::move(sm);
            return r;
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
            std::string tile = e.GetString("tileColor");
            r.style.tileU = tile.find("no tile") == std::string::npos && tile.find("vertical") == std::string::npos;
            r.style.tileV = tile.find("no tile") == std::string::npos && tile.find("horizontal") == std::string::npos;
            std::string blend = e.GetString("blendFunc");
            r.style.blend = blend.rfind("Blend", 0) == 0 || blend.rfind("Multiply", 0) == 0;
            r.style.additive = blend.rfind("Add", 0) == 0 || blend.rfind("Screen", 0) == 0;
            r.style.doubleSided = e.GetString("cullFace").rfind("None", 0) == 0;
            r.style.alphaTest = e.GetString("alphaTest").rfind("GE128", 0) == 0;
            consts["colorTint"] = e.GetColor("colorTint");
            consts["envMapParms"] = { (float)e.GetNumber("envMapMin"), (float)e.GetNumber("envMapMax"), (float)e.GetNumber("envMapExponent"), 0 };
            consts["detailScale"] = { (float)e.GetNumber("detailScaleX"), (float)e.GetNumber("detailScaleY"), 0, 0 };

            struct Map { const char* key; const char* semantic; };
            for (auto m : { Map{ "colorMap", "colorMap" }, Map{ "normalMap", "normalMap" }, Map{ "specColorMap", "specularMap" }, Map{ "detailMap", "detailMap" } })
            {
                std::string path = e.GetString(m.key);
                if (path.empty()) continue;
                auto file = m_project.ResolveFile(path);
                if (file.empty()) { missing[m.semantic] = std::string(m.key) + " not found: " + path; continue; }
                std::string err;
                auto tex = m_renderer.LoadTexture(file, err);
                if (tex) maps[m.semantic] = tex; else missing[m.semantic] = file.filename().string() + ": " + err;
            }
        }

        void FromCompiled(const game::CompiledMaterial& cm, Result& r, std::map<std::string, std::shared_ptr<GpuTexture>>& maps, std::map<std::string, std::array<float, 4>>& consts, std::map<std::string, std::string>& missing)
        {
            for (auto& t : cm.textures)
            {
                if (maps.count(t.semantic) || t.image.empty()) continue;
                auto tex = ImageByName(t.image);
                if (tex) maps[t.semantic] = tex;
                else if (!missing.count(t.semantic)) missing[t.semantic] = "image not found: " + t.image;
            }
            for (auto& c : cm.constants) if (!consts.count(c.name)) consts[c.name] = c.value;
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
                bool clamp = false;
                sp.textures.push_back({ (int)c->reg, tex, clamp, clamp, src == "material.colorMap" });
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
    };
}
