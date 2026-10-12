module;
#include <filesystem>
#include <utility>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

export module openape.mesh;

import openape.util;
import openape.math;

export namespace oa
{
    struct Vertex
    {
        float x, y, z;
        float nx, ny, nz;
        float u, v;
        uint32_t color = 0xffffffff;
        float tx = 1, ty = 0, tz = 0;   // tangent
        float bx = 0, by = 1, bz = 0;   // binormal
    };

    // One joint of a model's skeleton. Local = relative to the parent; world = model space at the bind pose.
    struct Bone
    {
        std::string name;
        int parent = -1;
        Vec3 localPos{};
        Quat localRot{};
        Vec3 worldPos{};
        Quat worldRot{};
    };

    // Up to four bone influences per vertex (weights sum to 1).
    struct VertexSkin
    {
        uint16_t bone[4] = { 0, 0, 0, 0 };
        float weight[4] = { 1, 0, 0, 0 };
    };

    // A range of indices drawn with one material.
    struct Submesh
    {
        uint32_t firstIndex = 0, indexCount = 0;
        std::string material;
    };

    class MeshData
    {
    public:
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
        std::vector<Submesh> submeshes;
        Vec3 boundsMin{ 0, 0, 0 }, boundsMax{ 0, 0, 0 };
        int boneCount = 0;
        std::vector<Bone> bones;
        std::vector<VertexSkin> skin;   // parallel to `vertices`; empty when the model is not skinned

        // Derives world transforms from the local ones (parents must precede children).
        void ComputeWorldFromLocal()
        {
            for (auto& b : bones)
            {
                if (b.parent < 0 || b.parent >= (int)bones.size()) { b.worldPos = b.localPos; b.worldRot = b.localRot; continue; }
                const Bone& p = bones[b.parent];
                b.worldPos = p.worldPos + p.worldRot.Rotate(b.localPos);
                b.worldRot = (p.worldRot * b.localRot).Normalized();
            }
        }
        // Derives local transforms from world ones.
        void ComputeLocalFromWorld()
        {
            for (auto& b : bones)
            {
                if (b.parent < 0 || b.parent >= (int)bones.size()) { b.localPos = b.worldPos; b.localRot = b.worldRot; continue; }
                const Bone& p = bones[b.parent];
                Quat ip = p.worldRot.Conjugate();
                b.localPos = ip.Rotate(b.worldPos - p.worldPos);
                b.localRot = (ip * b.worldRot).Normalized();
            }
        }

        bool Empty() const { return indices.empty(); }
        size_t TriangleCount() const { return indices.size() / 3; }
        Vec3 Center() const { return (boundsMin + boundsMax) * 0.5f; }
        float Radius() const { return (boundsMax - boundsMin).Length() * 0.5f; }

        // Makes every triangle's winding agree with its vertex normals, in the orientation the renderer
        // treats as front-facing (see Mat4::LookAtZUpRH: D3DCULL_CCW).
        void FixWinding()
        {
            // Decide once for the whole mesh. Fixing triangles individually would flip faces whose normals are
            // intentionally unrelated to their winding (hair cards, foliage), which then vanish when culled.
            size_t wrong = 0, right = 0;
            for (size_t i = 0; i + 2 < indices.size(); i += 3)
            {
                const Vertex& a = vertices[indices[i]]; const Vertex& b = vertices[indices[i + 1]]; const Vertex& c = vertices[indices[i + 2]];
                Vec3 e1{ b.x - a.x, b.y - a.y, b.z - a.z }, e2{ c.x - a.x, c.y - a.y, c.z - a.z };
                Vec3 n{ a.nx + b.nx + c.nx, a.ny + b.ny + c.ny, a.nz + b.nz + c.nz };
                float d = e1.Cross(e2).Dot(n);
                if (d > 0) ++wrong; else if (d < 0) ++right;
            }
            if (wrong <= right) return;
            for (size_t i = 0; i + 2 < indices.size(); i += 3) std::swap(indices[i + 1], indices[i + 2]);
        }

        // Per-vertex tangent frame from the UV layout (needed by the game's normal-mapped shaders).
        void ComputeTangents()
        {
            std::vector<Vec3> tan(vertices.size()), bin(vertices.size());
            for (size_t i = 0; i + 2 < indices.size(); i += 3)
            {
                const Vertex& a = vertices[indices[i]]; const Vertex& b = vertices[indices[i + 1]]; const Vertex& c = vertices[indices[i + 2]];
                Vec3 e1{ b.x - a.x, b.y - a.y, b.z - a.z }, e2{ c.x - a.x, c.y - a.y, c.z - a.z };
                float du1 = b.u - a.u, dv1 = b.v - a.v, du2 = c.u - a.u, dv2 = c.v - a.v;
                float det = du1 * dv2 - du2 * dv1;
                if (std::fabs(det) < 1e-12f) continue;
                float r = 1.0f / det;
                Vec3 t = (e1 * dv2 - e2 * dv1) * r, bt = (e2 * du1 - e1 * du2) * r;
                for (int k = 0; k < 3; ++k) { tan[indices[i + k]] = tan[indices[i + k]] + t; bin[indices[i + k]] = bin[indices[i + k]] + bt; }
            }
            for (size_t i = 0; i < vertices.size(); ++i)
            {
                Vertex& v = vertices[i];
                Vec3 n = Vec3{ v.nx, v.ny, v.nz }.Normalized();
                Vec3 t = tan[i] - n * n.Dot(tan[i]);
                if (t.Length() < 1e-8f) t = std::fabs(n.z) < 0.9f ? Vec3{ 0, 0, 1 }.Cross(n) : Vec3{ 1, 0, 0 };
                t = t.Normalized();
                Vec3 b = n.Cross(t);
                if (b.Dot(bin[i]) < 0) b = b * -1.0f;
                v.tx = t.x; v.ty = t.y; v.tz = t.z; v.bx = b.x; v.by = b.y; v.bz = b.z;
            }
        }

        void ComputeBounds()
        {
            if (vertices.empty()) return;
            boundsMin = boundsMax = { vertices[0].x, vertices[0].y, vertices[0].z };
            for (auto& v : vertices)
            {
                boundsMin = { std::min(boundsMin.x, v.x), std::min(boundsMin.y, v.y), std::min(boundsMin.z, v.z) };
                boundsMax = { std::max(boundsMax.x, v.x), std::max(boundsMax.y, v.y), std::max(boundsMax.z, v.z) };
            }
        }

        // ---- primitive generators (Z-up, unit size) ----------------------------------
        static MeshData Sphere(int slices = 64, int stacks = 32)
        {
            MeshData m;
            for (int i = 0; i <= stacks; ++i)
            {
                float v = (float)i / stacks, phi = v * kPi;
                for (int j = 0; j <= slices; ++j)
                {
                    float u = (float)j / slices, th = u * 2 * kPi;
                    Vec3 n{ std::sin(phi) * std::cos(th), std::sin(phi) * std::sin(th), std::cos(phi) };
                    m.vertices.push_back({ n.x, n.y, n.z, n.x, n.y, n.z, u * 2, v });
                }
            }
            for (int i = 0; i < stacks; ++i)
                for (int j = 0; j < slices; ++j)
                {
                    uint32_t a = i * (slices + 1) + j, b = a + slices + 1;
                    m.AddQuad(a, a + 1, b + 1, b);
                }
            return m.Finish();
        }

        static MeshData Cube()
        {
            MeshData m;
            struct Face { Vec3 n, u, v; };
            const Face faces[6] = { { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } }, { { -1, 0, 0 }, { 0, -1, 0 }, { 0, 0, 1 } },
                                    { { 0, 1, 0 }, { -1, 0, 0 }, { 0, 0, 1 } }, { { 0, -1, 0 }, { 1, 0, 0 }, { 0, 0, 1 } },
                                    { { 0, 0, 1 }, { 1, 0, 0 }, { 0, 1, 0 } }, { { 0, 0, -1 }, { 1, 0, 0 }, { 0, -1, 0 } } };
            for (auto& f : faces)
            {
                uint32_t base = (uint32_t)m.vertices.size();
                const float s = 0.8f;
                for (int k = 0; k < 4; ++k)
                {
                    float a = (k == 0 || k == 3) ? -1.f : 1.f, b = (k < 2) ? -1.f : 1.f;
                    Vec3 p = (f.n + f.u * a + f.v * b) * s;
                    m.vertices.push_back({ p.x, p.y, p.z, f.n.x, f.n.y, f.n.z, (a + 1) * 0.5f, 1 - (b + 1) * 0.5f });
                }
                m.AddQuad(base, base + 1, base + 2, base + 3);
            }
            return m.Finish();
        }

        static MeshData Cylinder(bool cone = false, int slices = 64)
        {
            MeshData m;
            const float r0 = 0.7f, r1 = cone ? 0.0f : 0.7f, h = 0.9f;
            for (int i = 0; i <= slices; ++i)
            {
                float u = (float)i / slices, a = u * 2 * kPi, c = std::cos(a), s = std::sin(a);
                float slope = (r0 - r1) / (2 * h);
                Vec3 n = Vec3{ c, s, slope }.Normalized();
                m.vertices.push_back({ c * r0, s * r0, -h, n.x, n.y, n.z, u * 2, 1 });
                m.vertices.push_back({ c * r1, s * r1, h, n.x, n.y, n.z, u * 2, 0 });
            }
            for (int i = 0; i < slices; ++i)
            {
                uint32_t a = i * 2;
                m.AddQuad(a, a + 2, a + 3, a + 1);
            }
            for (int cap = 0; cap < (cone ? 1 : 2); ++cap)
            {
                float z = cap ? h : -h, nz = cap ? 1.f : -1.f, r = cap ? r1 : r0;
                uint32_t centre = (uint32_t)m.vertices.size();
                m.vertices.push_back({ 0, 0, z, 0, 0, nz, 0.5f, 0.5f });
                for (int i = 0; i <= slices; ++i)
                {
                    float a = (float)i / slices * 2 * kPi;
                    m.vertices.push_back({ std::cos(a) * r, std::sin(a) * r, z, 0, 0, nz, 0.5f + std::cos(a) * 0.5f, 0.5f + std::sin(a) * 0.5f });
                }
                for (int i = 0; i < slices; ++i)
                {
                    if (cap) m.AddTri(centre, centre + 1 + i, centre + 2 + i);
                    else     m.AddTri(centre, centre + 2 + i, centre + 1 + i);
                }
            }
            return m.Finish();
        }

        static MeshData Torus(int rings = 64, int sides = 32)
        {
            MeshData m;
            const float R = 0.65f, r = 0.3f;
            for (int i = 0; i <= rings; ++i)
            {
                float u = (float)i / rings, a = u * 2 * kPi;
                for (int j = 0; j <= sides; ++j)
                {
                    float v = (float)j / sides, b = v * 2 * kPi;
                    Vec3 n{ std::cos(a) * std::cos(b), std::sin(a) * std::cos(b), std::sin(b) };
                    Vec3 p{ (R + r * std::cos(b)) * std::cos(a), (R + r * std::cos(b)) * std::sin(a), r * std::sin(b) };
                    m.vertices.push_back({ p.x, p.y, p.z, n.x, n.y, n.z, u * 4, v });
                }
            }
            for (int i = 0; i < rings; ++i)
                for (int j = 0; j < sides; ++j)
                {
                    uint32_t a = i * (sides + 1) + j, b = a + sides + 1;
                    m.AddQuad(a, b, b + 1, a + 1);
                }
            return m.Finish();
        }

        // A large square (half size `half`) whose texture coordinates run 0..tiles: the texture repeats `tiles` x `tiles` times,
        // which shows seams and clamp / repeat behaviour.
        static MeshData TiledPlane(float half = 6.0f, float tiles = 6.0f)
        {
            MeshData m;
            m.vertices = { { -half, -half, 0, 0, 0, 1, 0, tiles }, { half, -half, 0, 0, 0, 1, tiles, tiles }, { half, half, 0, 0, 0, 1, tiles, 0 }, { -half, half, 0, 0, 0, 1, 0, 0 } };
            m.AddQuad(0, 1, 2, 3);
            return m.Finish();
        }

        static MeshData Plane()
        {
            MeshData m;
            m.vertices = { { -1, -1, 0, 0, 0, 1, 0, 1 }, { 1, -1, 0, 0, 0, 1, 1, 1 }, { 1, 1, 0, 0, 0, 1, 1, 0 }, { -1, 1, 0, 0, 0, 1, 0, 0 } };
            m.AddQuad(0, 1, 2, 3);
            return m.Finish();
        }

    private:
        void AddTri(uint32_t a, uint32_t b, uint32_t c) { indices.push_back(a); indices.push_back(b); indices.push_back(c); }
        void AddQuad(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { AddTri(a, b, c); AddTri(a, c, d); }
        MeshData& Finish()
        {
            submeshes = { Submesh{ 0, (uint32_t)indices.size(), {} } };
            FixWinding();
            ComputeTangents();
            ComputeBounds();
            return *this;
        }

        friend class XModelExportLoader;
    };

    // Loads the text "XMODEL_EXPORT" interchange format (versions 5/6) written by the CoD exporters.
    class XModelExportLoader
    {
    public:
        // Returns false and fills `error` on failure.
        static bool Load(const fs::path& path, MeshData& out, std::string& error)
        {
            std::string text;
            if (!ReadTextFile(path, text)) { error = "cannot read " + path.string(); return false; }
            return Parse(text, out, error);
        }

        static inline thread_local Vec3 m_axisY{}, m_axisZ{};

        static bool Parse(const std::string& text, MeshData& out, std::string& error)
        {
            struct V { Vec3 pos; VertexSkin skin; int nInf = 0; };
            std::vector<V> verts;
            std::vector<std::string> materials;
            out = MeshData{};

            std::istringstream in(text);
            std::string line;
            bool inVerts = false, inFaces = false;
            int curVert = -1, faceCorner = 0, curFaceMat = 0;
            struct Corner { int v; Vec3 n; float u = 0, vv = 0; };
            Corner corner[3];
            std::vector<std::vector<uint32_t>> perMaterial;
            std::unordered_map<std::string, uint32_t> weld;
            int expectedVerts = 0, curBone = -1;
            bool haveSkin = false;

            auto nums = [](const std::string& s, size_t from) {
                std::vector<float> r;
                std::string t = s.substr(from);
                for (auto& c : t) if (c == ',') c = ' ';
                std::istringstream ss(t);
                for (float f; ss >> f;) r.push_back(f);
                return r;
            };

            auto emitFace = [&]() {
                if (curFaceMat >= (int)perMaterial.size()) perMaterial.resize(curFaceMat + 1);
                for (int k = 0; k < 3; ++k)
                {
                    auto& c = corner[k];
                    if (c.v < 0 || c.v >= (int)verts.size()) return;
                }
                for (int k = 0; k < 3; ++k)
                {
                    auto& c = corner[k];
                    // Corners that agree on vertex, normal and UV are one vertex (as in the compiled model). Without this
                    // every triangle has private vertices and the tangent frames, which are averaged per vertex, come out
                    // faceted: normal-mapped surfaces then show the triangles.
                    uint32_t keyBits[6];
                    std::memcpy(&keyBits[0], &c.n.x, 4); std::memcpy(&keyBits[1], &c.n.y, 4); std::memcpy(&keyBits[2], &c.n.z, 4);
                    std::memcpy(&keyBits[3], &c.u, 4); std::memcpy(&keyBits[4], &c.vv, 4); keyBits[5] = (uint32_t)c.v;
                    std::string key((const char*)keyBits, sizeof(keyBits));
                    if (auto hit = weld.find(key); hit != weld.end()) { perMaterial[curFaceMat].push_back(hit->second); continue; }
                    uint32_t idx = (uint32_t)out.vertices.size();
                    weld.emplace(std::move(key), idx);
                    Vec3 p = verts[c.v].pos;
                    Vertex vx{}; vx.x = p.x; vx.y = p.y; vx.z = p.z; vx.nx = c.n.x; vx.ny = c.n.y; vx.nz = c.n.z; vx.u = c.u; vx.v = c.vv;
                    out.vertices.push_back(vx);
                    out.skin.push_back(verts[c.v].skin);
                    perMaterial[curFaceMat].push_back(idx);
                }
            };

            while (std::getline(in, line))
            {
                line = Trim(line);
                if (line.empty() || line.rfind("//", 0) == 0) continue;
                std::istringstream ls(line);
                std::string kw; ls >> kw;
                if (kw == "NUMBONES") { int n = 0; ls >> n; out.boneCount = n; out.bones.resize(n); }
                else if (kw == "BONE" && !inVerts && !inFaces)
                {
                    // header: BONE i parent "name"   /   pose block: BONE i, then OFFSET / SCALE / X / Y / Z
                    int idx = -1, parent = -1; ls >> idx;
                    size_t q1 = line.find('"');
                    if (q1 != std::string::npos && idx >= 0 && idx < (int)out.bones.size())
                    {
                        ls >> parent;
                        out.bones[idx].parent = parent;
                        out.bones[idx].name = line.substr(q1 + 1, line.find('"', q1 + 1) - q1 - 1);
                    }
                    else curBone = idx;
                }
                else if (!inVerts && !inFaces && curBone >= 0 && curBone < (int)out.bones.size() && (kw == "OFFSET" || kw == "X" || kw == "Y" || kw == "Z"))
                {
                    auto n = nums(line, kw.size());
                    if (n.size() >= 3)
                    {
                        Bone& b = out.bones[curBone];
                        Vec3 v{ n[0], n[1], n[2] };
                        if (kw == "OFFSET") b.worldPos = v;
                        else if (kw == "X") b.worldRot.x = v.x, b.worldRot.y = v.y, b.worldRot.z = v.z;   // stash the axes, converted below
                        else if (kw == "Y") m_axisY = v;
                        else { m_axisZ = v; Vec3 X{ b.worldRot.x, b.worldRot.y, b.worldRot.z }; b.worldRot = Quat::FromAxes(X, m_axisY, m_axisZ); }
                    }
                }
                else if (kw == "NUMVERTS") { ls >> expectedVerts; verts.resize(expectedVerts); inVerts = true; inFaces = false; }
                else if (kw == "NUMFACES") { inVerts = false; inFaces = true; faceCorner = 0; }
                else if (kw == "NUMOBJECTS") { inFaces = false; }
                else if (kw == "NUMMATERIALS") { inFaces = false; }
                else if (kw == "MATERIAL")
                {
                    int idx = 0; ls >> idx;
                    size_t q1 = line.find('"');
                    if (q1 != std::string::npos)
                    {
                        size_t q2 = line.find('"', q1 + 1);
                        if (idx >= (int)materials.size()) materials.resize(idx + 1);
                        materials[idx] = line.substr(q1 + 1, q2 - q1 - 1);
                    }
                }
                else if (inVerts)
                {
                    if (kw == "VERT") { ls >> curVert; }
                    else if (kw == "BONES" && curVert >= 0 && curVert < (int)verts.size()) { verts[curVert].nInf = 0; verts[curVert].skin = VertexSkin{}; verts[curVert].skin.weight[0] = 0; }
                    else if (kw == "BONE" && curVert >= 0 && curVert < (int)verts.size())
                    {
                        int b = 0; float w = 0; ls >> b >> w;
                        auto& v = verts[curVert];
                        if (v.nInf < 4) { v.skin.bone[v.nInf] = (uint16_t)b; v.skin.weight[v.nInf] = w; ++v.nInf; haveSkin = true; }
                    }
                    else if (kw == "OFFSET" && curVert >= 0 && curVert < (int)verts.size())
                    {
                        auto n = nums(line, 6);
                        if (n.size() >= 3) verts[curVert].pos = { n[0], n[1], n[2] };
                    }
                }
                else if (inFaces)
                {
                    if (kw == "TRI")
                    {
                        int obj = 0, mat = 0; ls >> obj >> mat;
                        curFaceMat = mat; faceCorner = 0;
                    }
                    else if (kw == "VERT")
                    {
                        int idx = 0; ls >> idx;
                        if (faceCorner < 3) { corner[faceCorner] = Corner{}; corner[faceCorner].v = idx; }
                    }
                    else if (kw == "NORMAL" && faceCorner < 3)
                    {
                        auto n = nums(line, 6);
                        if (n.size() >= 3) corner[faceCorner].n = { n[0], n[1], n[2] };
                    }
                    else if (kw == "UV" && faceCorner < 3)
                    {
                        auto n = nums(line, 2);
                        if (n.size() >= 3) { corner[faceCorner].u = n[1]; corner[faceCorner].vv = n[2]; }   // already top-down: the exporters write 1 - v
                        ++faceCorner;   // UV is the last record of a corner
                        if (faceCorner == 3) { emitFace(); faceCorner = 0; }
                    }
                }
            }

            if (!haveSkin) out.skin.clear();
            if (!out.bones.empty()) out.ComputeLocalFromWorld();
            if (verts.empty() || perMaterial.empty()) { error = "no geometry found (is this an XMODEL_EXPORT file?)"; return false; }
            for (size_t m = 0; m < perMaterial.size(); ++m)
            {
                if (perMaterial[m].empty()) continue;
                Submesh s;
                s.firstIndex = (uint32_t)out.indices.size();
                s.indexCount = (uint32_t)perMaterial[m].size();
                s.material = m < materials.size() ? materials[m] : std::string();
                out.indices.insert(out.indices.end(), perMaterial[m].begin(), perMaterial[m].end());
                out.submeshes.push_back(std::move(s));
            }
            out.FixWinding();
            out.ComputeTangents();
            out.ComputeBounds();
            return true;
        }
    };
}
