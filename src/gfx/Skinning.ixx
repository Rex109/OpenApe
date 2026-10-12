module;

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

export module openape.skinning;

import openape.util;
import openape.math;
import openape.mesh;
import openape.xanim;

export namespace oa
{
    // Poses a skeleton with an animation and deforms the model's vertices with it (CPU linear-blend skinning).
    class Skinner
    {
    public:
        // Maps animation parts to skeleton bones by (case-insensitive) name. Returns how many bones are animated.
        int Bind(const MeshData& model, const Animation& anim)
        {
            std::unordered_map<std::string, size_t> byName;
            for (size_t i = 0; i < anim.parts.size(); ++i) byName[ToLower(anim.parts[i])] = i;
            m_partOfBone.assign(model.bones.size(), -1);
            int n = 0;
            for (size_t b = 0; b < model.bones.size(); ++b)
            {
                auto it = byName.find(ToLower(model.bones[b].name));
                if (it != byName.end()) { m_partOfBone[b] = (int)it->second; ++n; }
            }
            return n;
        }

        // World-space bone transforms at `frame`. The animation files store ABSOLUTE (model space) poses for every
        // part; bones the animation does not mention follow their parent with their bind-pose offset.
        void Pose(const MeshData& model, const Animation& anim, float frame, bool loop, std::vector<Bone>& posed) const
        {
            posed = model.bones;
            for (size_t b = 0; b < posed.size(); ++b)
            {
                Bone& bn = posed[b];
                AnimPose p;
                if (b < m_partOfBone.size() && m_partOfBone[b] >= 0) p = anim.Sample(frame, (size_t)m_partOfBone[b], loop);
                if (p.valid) { bn.worldPos = p.pos; bn.worldRot = p.rot; }
                else if (bn.parent >= 0 && bn.parent < (int)b)
                {
                    const Bone& par = posed[bn.parent];
                    bn.worldPos = par.worldPos + par.worldRot.Rotate(bn.localPos);
                    bn.worldRot = (par.worldRot * bn.localRot).Normalized();
                }
            }
        }

        // Deforms `bind` (the model at its bind pose) into `out`, which must start as a copy of `bind`.
        static void Skin(const MeshData& bind, const std::vector<Bone>& posed, MeshData& out)
        {
            if (bind.skin.size() != bind.vertices.size() || posed.size() != bind.bones.size()) return;
            struct Xf { Quat r; Vec3 t; };
            std::vector<Xf> xf(posed.size());
            for (size_t b = 0; b < posed.size(); ++b)
            {
                xf[b].r = (posed[b].worldRot * bind.bones[b].worldRot.Conjugate()).Normalized();
                xf[b].t = posed[b].worldPos - xf[b].r.Rotate(bind.bones[b].worldPos);
            }
            for (size_t i = 0; i < bind.vertices.size(); ++i)
            {
                const Vertex& v = bind.vertices[i];
                const VertexSkin& s = bind.skin[i];
                Vec3 p{}, n{}, t{}, bi{};
                float total = 0;
                for (int k = 0; k < 4; ++k)
                {
                    float w = s.weight[k];
                    if (w <= 0 || s.bone[k] >= xf.size()) continue;
                    const Xf& x = xf[s.bone[k]];
                    p = p + (x.r.Rotate({ v.x, v.y, v.z }) + x.t) * w;
                    n = n + x.r.Rotate({ v.nx, v.ny, v.nz }) * w;
                    t = t + x.r.Rotate({ v.tx, v.ty, v.tz }) * w;
                    bi = bi + x.r.Rotate({ v.bx, v.by, v.bz }) * w;
                    total += w;
                }
                Vertex& o = out.vertices[i];
                if (total <= 0) { o = v; continue; }
                p = p * (1.0f / total);
                n = n.Normalized(); t = t.Normalized(); bi = bi.Normalized();
                o.x = p.x; o.y = p.y; o.z = p.z;
                o.nx = n.x; o.ny = n.y; o.nz = n.z;
                o.tx = t.x; o.ty = t.y; o.tz = t.z;
                o.bx = bi.x; o.by = bi.y; o.bz = bi.z;
            }
        }

    private:
        std::vector<int> m_partOfBone;
    };
}
