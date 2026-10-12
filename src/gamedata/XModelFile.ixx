module;

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

export module openape.xmodelfile;

import openape.util;
import openape.math;
import openape.mesh;

// Reader for the compiled model files in <root>/raw:
//   xmodel/<name>        header: LOD table, per-LOD surface material names
//   xmodelsurfs/<lod>    geometry, one record per surface
//   xmodelparts/<lod>    bone hierarchy with the bind pose
// (all version 25; see ape_work/decomp/NOTES.md for the byte layouts)
export namespace oa
{
    class CompiledXModel
    {
    public:
        struct LodInfo { float distance = 0; std::string name; std::vector<std::string> materials; };

        // The LODs of `<raw>/xmodel/<name>` (distance and surface file); empty if the model cannot be read.
        static std::vector<LodInfo> Lods(const fs::path& rawDir, const std::string& name)
        {
            std::vector<unsigned char> hdr;
            std::vector<LodInfo> lods;
            std::string err;
            if (ReadBinaryFile(rawDir / "xmodel" / name, hdr)) ParseHeader(hdr, lods, err);
            return lods;
        }

        // Loads LOD `lod` of `<raw>/xmodel/<name>` into `out` (world space at the bind pose, Z up).
        static bool Load(const fs::path& rawDir, const std::string& name, int lod, MeshData& out, std::string& error)
        {
            std::vector<unsigned char> hdr;
            if (!ReadBinaryFile(rawDir / "xmodel" / name, hdr)) { error = "no compiled model '" + name + "' in raw/xmodel"; return false; }
            std::vector<LodInfo> lods;
            if (!ParseHeader(hdr, lods, error)) return false;
            if (lods.empty()) { error = "model has no LODs"; return false; }
            lod = std::clamp(lod, 0, (int)lods.size() - 1);
            const LodInfo& li = lods[lod];

            std::vector<unsigned char> surfs, parts;
            if (!ReadBinaryFile(rawDir / "xmodelsurfs" / li.name, surfs)) { error = "missing raw/xmodelsurfs/" + li.name; return false; }
            out = MeshData{};
            if (ReadBinaryFile(rawDir / "xmodelparts" / li.name, parts)) ParseParts(parts, out.bones);
            if (out.bones.empty()) out.bones.push_back(Bone{});
            out.ComputeWorldFromLocal();
            out.boneCount = (int)out.bones.size();
            return ParseSurfaces(surfs, out, li.materials, error);
        }

    private:
        struct Reader
        {
            const std::vector<unsigned char>& d;
            size_t p = 0;
            bool ok = true;
            bool Has(size_t n) { if (p + n > d.size()) ok = false; return ok; }
            template <class T> T Get() { T v{}; if (Has(sizeof(T))) { std::memcpy(&v, &d[p], sizeof(T)); p += sizeof(T); } return v; }
            std::string CStr()
            {
                std::string s;
                while (p < d.size() && d[p]) s += (char)d[p++];
                if (p >= d.size()) ok = false; else ++p;
                return s;
            }
            void Skip(size_t n) { if (Has(n)) p += n; }
        };

        static bool ParseHeader(const std::vector<unsigned char>& d, std::vector<LodInfo>& lods, std::string& error)
        {
            Reader r{ d };
            uint16_t ver = r.Get<uint16_t>();
            if (ver != 25) { error = "unsupported xmodel version " + std::to_string(ver); return false; }
            r.Skip(1 + 24);                  // flags, bounds
            r.CStr();                        // (empty in all shipped files)
            LodInfo l[4];
            for (auto& x : l) { x.distance = r.Get<float>(); x.name = r.CStr(); }
            r.Skip(4);                       // collision LOD index
            uint32_t collSurfs = r.Get<uint32_t>();
            for (uint32_t i = 0; i < collSurfs && r.ok; ++i)
            {
                uint32_t tris = r.Get<uint32_t>();
                r.Skip((size_t)tris * 48 + 12 + 12 + 4 + 4 + 4);
            }
            for (auto& x : l)
            {
                if (x.name.empty()) break;
                uint16_t n = r.Get<uint16_t>();
                for (uint16_t i = 0; i < n && r.ok; ++i) x.materials.push_back(r.CStr());
                lods.push_back(x);
            }
            if (!r.ok) { error = "truncated xmodel header"; return false; }
            return true;
        }

        static void ParseParts(const std::vector<unsigned char>& d, std::vector<Bone>& bones)
        {
            Reader r{ d };
            if (r.Get<uint16_t>() != 25) return;
            uint16_t nonRoot = r.Get<uint16_t>(), roots = r.Get<uint16_t>();
            bones.assign(roots, Bone{});
            for (uint16_t i = 0; i < nonRoot && r.ok; ++i)
            {
                Bone b;
                b.parent = r.Get<uint8_t>();
                b.localPos = { r.Get<float>(), r.Get<float>(), r.Get<float>() };
                float v[3];
                for (auto& c : v) c = r.Get<int16_t>() / 32767.0f;
                b.localRot = Quat{ v[0], v[1], v[2], std::sqrt(std::max(0.0f, 1 - v[0] * v[0] - v[1] * v[1] - v[2] * v[2])) };
                bones.push_back(b);
            }
            for (auto& b : bones) b.name = r.CStr();
            if (!r.ok) bones.clear();
        }

        static bool ParseSurfaces(const std::vector<unsigned char>& d, MeshData& out, const std::vector<std::string>& materials, std::string& error)
        {
            Reader r{ d };
            if (r.Get<uint16_t>() != 25) { error = "unsupported xmodelsurfs version"; return false; }
            uint16_t numSurfs = r.Get<uint16_t>();
            for (uint16_t s = 0; s < numSurfs && r.ok; ++s)
            {
                r.Skip(1);                                   // tile mode
                r.Skip(2);                                   // checksum
                uint16_t nv = r.Get<uint16_t>(), nt = r.Get<uint16_t>();
                std::vector<std::pair<uint16_t, uint16_t>> lists;
                for (;;)
                {
                    uint16_t c = r.Get<uint16_t>();
                    if (!r.ok || c == 0) break;
                    lists.emplace_back(c, r.Get<uint16_t>());
                }
                bool rigid = lists.size() == 1 && lists[0].first == nv;   // otherwise the surface deforms
                if (!rigid) r.Skip(2);                                    // total minor-bone count

                uint32_t base = (uint32_t)out.vertices.size();
                for (uint16_t v = 0; v < nv && r.ok; ++v)
                {
                    Vec3 n{ r.Get<float>(), r.Get<float>(), r.Get<float>() };
                    uint8_t col[4]; for (auto& c : col) c = r.Get<uint8_t>();
                    float u = r.Get<float>(), vv = r.Get<float>();
                    Vec3 t{ r.Get<float>(), r.Get<float>(), r.Get<float>() };
                    Vec3 b{ r.Get<float>(), r.Get<float>(), r.Get<float>() };
                    Vec3 pos;
                    VertexSkin sk;
                    if (rigid)
                    {
                        sk.bone[0] = lists[0].second;
                        pos = { r.Get<float>(), r.Get<float>(), r.Get<float>() };
                    }
                    else
                    {
                        uint8_t minor = r.Get<uint8_t>();
                        sk.bone[0] = r.Get<uint16_t>();
                        pos = { r.Get<float>(), r.Get<float>(), r.Get<float>() };
                        float rest = 1.0f;
                        for (uint8_t k = 0; k < minor && r.ok; ++k)        // (bone, weight/65536) pairs; the main bone gets the remainder
                        {
                            uint16_t bone = r.Get<uint16_t>();
                            float w = r.Get<uint16_t>() / 65536.0f;
                            if (k < 3) { sk.bone[k + 1] = bone; sk.weight[k + 1] = w; rest -= w; }
                        }
                        sk.weight[0] = std::max(rest, 0.0f);
                    }
                    if (!r.ok) break;
                    // Positions and vectors are stored in model space at the bind pose (the engine skins with
                    // boneWorld * inverseBind), so they can be used as they are.
                    out.skin.push_back(sk);
                    Vertex vx;
                    vx.x = pos.x; vx.y = pos.y; vx.z = pos.z;
                    vx.nx = n.x; vx.ny = n.y; vx.nz = n.z;
                    vx.tx = t.x; vx.ty = t.y; vx.tz = t.z;
                    vx.bx = b.x; vx.by = b.y; vx.bz = b.z;
                    vx.u = u; vx.v = vv;
                    vx.color = (uint32_t)col[3] << 24 | (uint32_t)col[0] << 16 | (uint32_t)col[1] << 8 | col[2];
                    out.vertices.push_back(vx);
                }
                Submesh sm;
                sm.firstIndex = (uint32_t)out.indices.size();
                sm.material = s < materials.size() ? materials[s] : std::string();
                for (uint16_t t = 0; t < nt && r.ok; ++t)
                    for (int k = 0; k < 3; ++k) out.indices.push_back(base + r.Get<uint16_t>());
                sm.indexCount = (uint32_t)out.indices.size() - sm.firstIndex;
                out.submeshes.push_back(std::move(sm));
            }
            if (!r.ok) { error = "truncated xmodelsurfs data"; return false; }
            out.FixWinding();
            out.ComputeBounds();
            return !out.Empty();
        }
    };
}
