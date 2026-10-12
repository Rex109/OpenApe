module;
#include <cctype>
#include <cstdio>
#include <utility>

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

export module openape.gamedata;

import openape.util;

// Readers for the compiled game data in <root>/raw: shader_bin (D3D9 bytecode), techsets, techniques
// and the binary material files.  See ape_work/decomp/NOTES.md for the formats.
export namespace oa::game
{
    // ---------------------------------------------------------------- shader bytecode
    struct ShaderConstant
    {
        std::string name;
        uint16_t regSet = 0;     // D3DXREGISTER_SET: 0 bool, 1 int4, 2 float4, 3 sampler
        uint16_t reg = 0;
        uint16_t count = 0;
    };

    struct ShaderCode
    {
        std::vector<uint8_t> bytes;                  // starts at the D3D version token
        std::vector<ShaderConstant> constants;       // from the CTAB comment block
        bool valid() const { return !bytes.empty(); }
        const ShaderConstant* Find(const std::string& n) const
        {
            for (auto& c : constants) if (c.name == n) return &c;
            return nullptr;
        }
    };

    // ---------------------------------------------------------------- techniques
    struct ShaderBinding
    {
        std::string target;   // shader constant / sampler name, e.g. "colorMapSampler"
        std::string source;   // "material.colorMap", "sampler.white", "float4(0,0,0,0)", ...
    };

    struct ShaderRef
    {
        std::string version;  // "2.0"
        std::string name;     // hashed to find the file in shader_bin
        std::vector<ShaderBinding> bindings;
        bool valid() const { return !name.empty(); }
    };

    struct VertexBinding
    {
        std::string target;   // "position", "normal", "color[0]", "texcoord[1]"
        std::string source;   // "position", "tangent", "texcoord[0]", ...
    };

    struct TechniquePass
    {
        std::string stateMap;
        ShaderRef vertex, pixel;
        std::vector<VertexBinding> vertexInputs;
    };

    struct Technique
    {
        std::string file;
        std::vector<TechniquePass> passes;
    };

    struct Techset
    {
        std::string name;
        std::map<std::string, std::string> techniqueFiles;   // "sunlight preview" -> "sunpre_r0c0n0s0"
    };

    // ---------------------------------------------------------------- compiled materials
    struct MaterialTexture { std::string semantic, image; uint32_t flags = 0; };
    struct MaterialConstant { std::string name; std::array<float, 4> value{}; };

    struct CompiledMaterial
    {
        std::string name, techset, surfaceType;
        uint8_t gameFlags = 0, sortKey = 0;
        std::vector<MaterialTexture> textures;
        std::vector<MaterialConstant> constants;

        const MaterialTexture* Texture(const std::string& semantic) const
        {
            for (auto& t : textures) if (t.semantic == semantic) return &t;
            return nullptr;
        }
        const MaterialConstant* Constant(const std::string& n) const
        {
            for (auto& c : constants) if (c.name == n) return &c;
            return nullptr;
        }
    };

    class GameData
    {
    public:
        void Open(const fs::path& root)
        {
            m_raw = root / "raw";
            m_shaders.clear(); m_techsets.clear(); m_techniques.clear(); m_materials.clear();
            m_nameTablesLoaded = false;
            m_vsNames.clear(); m_psNames.clear();
        }

        bool Available() const
        {
            std::error_code ec;
            return !m_raw.empty() && fs::is_directory(m_raw / "shader_bin", ec) && fs::is_directory(m_raw / "techsets", ec);
        }
        const fs::path& RawDir() const { return m_raw; }

        // The tool that built shader_bin names files by hashing the shader's name (see NOTES.md).
        static uint32_t HashName(const std::string& n)
        {
            uint32_t h = 0;
            for (unsigned char c : n) h = c ^ (33 * h);
            return h;
        }

        const ShaderCode* Shader(bool vertex, const std::string& version, const std::string& name)
        {
            std::string key = std::string(vertex ? "vs" : "ps") + version + ":" + name;
            auto it = m_shaders.find(key);
            if (it != m_shaders.end()) return it->second.valid() ? &it->second : nullptr;
            ShaderCode code = LoadShader(vertex, version, name);
            auto& slot = m_shaders[key] = std::move(code);
            return slot.valid() ? &slot : nullptr;
        }

        const Techset* FindTechset(const std::string& name)
        {
            auto it = m_techsets.find(name);
            if (it != m_techsets.end()) return it->second ? it->second.get() : nullptr;
            auto ts = LoadTechset(name);
            auto* raw = ts.get();
            m_techsets[name] = std::move(ts);
            return raw;
        }

        const Technique* FindTechnique(const std::string& file)
        {
            auto it = m_techniques.find(file);
            if (it != m_techniques.end()) return it->second ? it->second.get() : nullptr;
            auto t = LoadTechnique(file);
            auto* raw = t.get();
            m_techniques[file] = std::move(t);
            return raw;
        }

        // Technique of a techset by its display name ("unlit", "sunlight preview", ...).
        const Technique* FindTechnique(const std::string& techset, const std::string& technique)
        {
            auto* ts = FindTechset(techset);
            if (!ts) return nullptr;
            auto it = ts->techniqueFiles.find(technique);
            return it == ts->techniqueFiles.end() ? nullptr : FindTechnique(it->second);
        }

        const CompiledMaterial* FindMaterial(const std::string& name)
        {
            auto it = m_materials.find(name);
            if (it != m_materials.end()) return it->second ? it->second.get() : nullptr;
            auto m = LoadMaterial(name);
            auto* raw = m.get();
            m_materials[name] = std::move(m);
            return raw;
        }

        bool TechsetExists(const std::string& name) const
        {
            std::error_code ec;
            return !m_raw.empty() && fs::exists(m_raw / "techsets" / (name + ".techset"), ec);
        }

    private:
        // ---- shaders -----------------------------------------------------------------
        void LoadNameTables()
        {
            m_nameTablesLoaded = true;
            std::vector<unsigned char> d;
            if (!ReadBinaryFile(m_raw / "shader_bin" / "shader_names", d) || d.size() < 8) return;
            auto u32 = [&](size_t o) { uint32_t v; std::memcpy(&v, &d[o], 4); return v; };
            size_t off = 0;
            for (auto* tbl : { &m_vsNames, &m_psNames })
            {
                if (off + 4 > d.size()) return;
                uint32_t n = u32(off); off += 4;
                if (off + (size_t)n * 8 > d.size()) return;
                for (uint32_t i = 0; i < n; ++i, off += 8) (*tbl)[u32(off)] = u32(off + 4);
            }
        }

        static void ParseCtab(ShaderCode& s)
        {
            const auto& b = s.bytes;
            for (size_t i = 0; i + 8 < b.size(); i += 4)
            {
                if (std::memcmp(&b[i], "CTAB", 4) != 0) continue;
                size_t base = i + 4;
                if (base + 28 > b.size()) return;
                auto u32 = [&](size_t o) { uint32_t v; std::memcpy(&v, &b[o], 4); return v; };
                uint32_t count = u32(base + 12), info = u32(base + 16);
                for (uint32_t k = 0; k < count; ++k)
                {
                    size_t e = base + info + (size_t)k * 20;
                    if (e + 20 > b.size()) return;
                    ShaderConstant c;
                    uint32_t nameOff = u32(e);
                    std::memcpy(&c.regSet, &b[e + 4], 2); std::memcpy(&c.reg, &b[e + 6], 2); std::memcpy(&c.count, &b[e + 8], 2);
                    size_t p = base + nameOff;
                    while (p < b.size() && b[p]) c.name += (char)b[p++];
                    s.constants.push_back(std::move(c));
                }
                return;
            }
        }

        ShaderCode LoadShader(bool vertex, const std::string& version, const std::string& name)
        {
            if (!m_nameTablesLoaded) LoadNameTables();
            auto& tbl = vertex ? m_vsNames : m_psNames;
            auto it = tbl.find(HashName(name));
            if (it == tbl.end()) return {};
            std::string ver = version;
            for (auto& c : ver) if (c == '.') c = '_';
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%s_%s_%08x", vertex ? "vs" : "ps", ver.c_str(), it->second);
            std::vector<unsigned char> d;
            if (!ReadBinaryFile(m_raw / "shader_bin" / buf, d) || d.size() < 8) return {};
            // files start with a 4 byte header (size/flags) in front of the D3D version token
            ShaderCode s;
            size_t skip = 0;
            for (size_t o : { (size_t)4, (size_t)0 })
                if (d.size() > o + 4 && d[o + 2] == (vertex ? 0xfe : 0xff) && d[o + 3] == 0xff) { skip = o; break; }
            s.bytes.assign(d.begin() + skip, d.end());
            ParseCtab(s);
            return s;
        }

        // ---- text scanning ------------------------------------------------------------
        struct Scanner
        {
            std::string t;
            size_t p = 0;
            void Skip()
            {
                for (;;)
                {
                    while (p < t.size() && std::isspace((unsigned char)t[p])) ++p;
                    if (t.compare(p, 2, "//") == 0) { while (p < t.size() && t[p] != '\n') ++p; continue; }
                    if (t.compare(p, 2, "/*") == 0) { p = t.find("*/", p + 2); p = p == std::string::npos ? t.size() : p + 2; continue; }
                    return;
                }
            }
            bool End() { Skip(); return p >= t.size(); }
            char Peek() { Skip(); return p < t.size() ? t[p] : 0; }
            bool Accept(char c) { if (Peek() == c) { ++p; return true; } return false; }
            std::string Quoted()
            {
                Skip();
                std::string r;
                if (p < t.size() && t[p] == '"') { ++p; while (p < t.size() && t[p] != '"') r += t[p++]; ++p; }
                return r;
            }
            static bool IdChar(char c) { return std::isalnum((unsigned char)c) || c == '_' || c == '.' || c == '$' || c == '-'; }
            std::string Word()
            {
                Skip();
                std::string r;
                while (p < t.size() && IdChar(t[p])) r += t[p++];
                return r;
            }
            // raw text up to (not including) the terminator, honouring parentheses
            std::string UntilSemicolon()
            {
                Skip();
                std::string r; int depth = 0;
                while (p < t.size() && !(t[p] == ';' && depth == 0))
                {
                    if (t[p] == '(') ++depth; else if (t[p] == ')') --depth;
                    if (t.compare(p, 2, "//") == 0) { while (p < t.size() && t[p] != '\n') ++p; continue; }
                    r += t[p++];
                }
                if (p < t.size()) ++p;
                return Trim(r);
            }
        };

        std::unique_ptr<Techset> LoadTechset(const std::string& name)
        {
            std::string text;
            if (!ReadTextFile(m_raw / "techsets" / (name + ".techset"), text)) return nullptr;
            auto ts = std::make_unique<Techset>();
            ts->name = name;
            Scanner s{ text };
            std::vector<std::string> pending;
            while (!s.End())
            {
                if (s.Peek() == '"')
                {
                    pending.push_back(s.Quoted());
                    s.Accept(':');
                }
                else
                {
                    std::string file = s.Word();
                    s.Accept(';');
                    if (file.empty()) { ++s.p; continue; }
                    for (auto& n : pending) ts->techniqueFiles[n] = file;
                    pending.clear();
                }
            }
            return ts;
        }

        std::unique_ptr<Technique> LoadTechnique(const std::string& file)
        {
            std::string text;
            if (!ReadTextFile(m_raw / "techniques" / (file + ".tech"), text)) return nullptr;
            auto tech = std::make_unique<Technique>();
            tech->file = file;
            Scanner s{ text };
            while (!s.End())
            {
                if (!s.Accept('{')) { ++s.p; continue; }
                TechniquePass pass;
                while (!s.End() && !s.Accept('}'))
                {
                    std::string kw = s.Word();
                    if (kw == "stateMap") { pass.stateMap = s.Quoted(); s.Accept(';'); }
                    else if (kw == "vertexShader" || kw == "pixelShader")
                    {
                        ShaderRef r;
                        r.version = s.Word();
                        r.name = s.Quoted();
                        if (s.Accept('{'))
                        {
                            while (!s.End() && !s.Accept('}'))
                            {
                                std::string lhs = s.Word();
                                if (!s.Accept('=')) { s.UntilSemicolon(); continue; }
                                r.bindings.push_back({ lhs, s.UntilSemicolon() });
                            }
                        }
                        (kw == "vertexShader" ? pass.vertex : pass.pixel) = std::move(r);
                    }
                    else if (kw.rfind("vertex.", 0) == 0)
                    {
                        std::string target = kw.substr(7);
                        // "texcoord[1]" may be split by the word scanner at '[': re-join
                        while (s.Peek() == '[') { s.Accept('['); target += "[" + s.Word() + "]"; s.Accept(']'); }
                        s.Accept('=');
                        std::string src = s.UntilSemicolon();
                        if (src.rfind("code.", 0) == 0) src = src.substr(5);
                        pass.vertexInputs.push_back({ target, src });
                    }
                    else if (kw.empty()) ++s.p;
                    else s.UntilSemicolon();
                }
                tech->passes.push_back(std::move(pass));
            }
            return tech;
        }

        // ---- compiled materials --------------------------------------------------------
        std::unique_ptr<CompiledMaterial> LoadMaterial(const std::string& name)
        {
            std::vector<unsigned char> d;
            if (!ReadBinaryFile(m_raw / "materials" / name, d) || d.size() < 0x44) return nullptr;
            auto u32 = [&](size_t o) { uint32_t v = 0; if (o + 4 <= d.size()) std::memcpy(&v, &d[o], 4); return v; };
            auto u16 = [&](size_t o) { uint16_t v = 0; if (o + 2 <= d.size()) std::memcpy(&v, &d[o], 2); return v; };
            auto str = [&](size_t o) {
                std::string r;
                for (; o < d.size() && d[o]; ++o) r += (char)d[o];
                return r;
            };
            auto m = std::make_unique<CompiledMaterial>();
            m->name = str(u32(0));
            m->techset = str(u32(0x34));
            m->gameFlags = d[8]; m->sortKey = d[9];
            uint16_t texCount = u16(0x30), constCount = u16(0x32);
            size_t texOff = u32(0x38), constOff = u32(0x3c);
            if (texCount > 32 || constCount > 64) return m;
            for (uint16_t i = 0; i < texCount; ++i)
            {
                size_t o = texOff + (size_t)i * 12;
                if (o + 12 > d.size()) break;
                MaterialTexture t;
                t.semantic = str(u32(o));
                t.flags = u32(o + 4);
                uint32_t imgOff = u32(o + 8);
                if (imgOff > 0 && imgOff < d.size()) t.image = str(imgOff);
                m->textures.push_back(std::move(t));
            }
            for (uint16_t i = 0; i < constCount; ++i)
            {
                size_t o = constOff + (size_t)i * 20;
                if (o + 20 > d.size()) break;
                MaterialConstant c;
                c.name = str(u32(o));
                std::memcpy(c.value.data(), &d[o + 4], 16);
                m->constants.push_back(std::move(c));
            }
            return m;
        }

        fs::path m_raw;
        bool m_nameTablesLoaded = false;
        std::unordered_map<uint32_t, uint32_t> m_vsNames, m_psNames;
        std::unordered_map<std::string, ShaderCode> m_shaders;
        std::unordered_map<std::string, std::unique_ptr<Techset>> m_techsets;
        std::unordered_map<std::string, std::unique_ptr<Technique>> m_techniques;
        std::unordered_map<std::string, std::unique_ptr<CompiledMaterial>> m_materials;
    };
}
