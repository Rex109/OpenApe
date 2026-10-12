module;

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

export module openape.xanim;

import openape.util;
import openape.math;

// Animation data. Poses are LOCAL transforms (relative to the parent bone) for each named part.
export namespace oa
{
    struct AnimPose
    {
        Vec3 pos{};
        Quat rot{};
        bool valid = false;
        bool hasPos = true, hasRot = true;   // compiled animations leave untouched channels to the bind pose
    };

    struct NoteKey { int frame = 0; std::string name; };

    class Animation
    {
    public:
        std::string name;
        float framerate = 30.0f;
        int numFrames = 0;
        std::vector<std::string> parts;      // bone names, indexed by part number
        std::vector<NoteKey> notes;
        bool localPoses = false;             // poses are relative to the parent bone (compiled files) instead of model space (exports)

        const AnimPose& At(int frame, size_t part) const { return m_poses[(size_t)frame * parts.size() + part]; }
        float Duration() const { return framerate > 0 ? numFrames / framerate : 0.0f; }

        // Local pose of `part` at a fractional frame (frames are interpolated, the last one wraps if `loop`).
        AnimPose Sample(float frame, size_t part, bool loop) const
        {
            if (numFrames <= 0) return {};
            int f0 = (int)std::floor(frame);
            float t = frame - f0;
            int f1 = f0 + 1;
            auto wrap = [&](int f) { return loop ? ((f % numFrames) + numFrames) % numFrames : std::clamp(f, 0, numFrames - 1); };
            f0 = wrap(f0); f1 = wrap(f1);
            const AnimPose& a = At(f0, part);
            if (t <= 0 || f0 == f1) return a;
            const AnimPose& b = At(f1, part);
            if (!a.valid || !b.valid) return a.valid ? a : b;
            AnimPose r;
            r.valid = true;
            r.hasPos = a.hasPos; r.hasRot = a.hasRot;
            r.pos = a.pos + (b.pos - a.pos) * t;
            r.rot = Quat::Slerp(a.rot, b.rot, t);
            return r;
        }

        // Reads the text "ANIMATION / VERSION 3" interchange format written by the CoD exporters.
        static bool LoadExport(const fs::path& path, Animation& out, std::string& error)
        {
            std::string text;
            if (!ReadTextFile(path, text)) { error = "cannot read " + path.string(); return false; }
            out = Animation{};
            out.name = path.stem().string();

            Cursor c{ text };
            int curFrame = -1, curPart = -1;
            Vec3 axes[3];
            bool inNotes = false;
            std::string_view line;
            while (c.NextLine(line))
            {
                if (line.empty() || line.rfind("//", 0) == 0) continue;
                std::string_view kw = c.Word(line);
                if (kw == "NUMPARTS") { out.parts.resize((size_t)c.Int(line)); }
                else if (kw == "PART" && !inNotes && curFrame < 0)
                {
                    int i = c.Int(line);
                    size_t q1 = line.find('"');
                    if (i >= 0 && i < (int)out.parts.size() && q1 != std::string_view::npos)
                        out.parts[i] = std::string(line.substr(q1 + 1, line.find('"', q1 + 1) - q1 - 1));
                }
                else if (kw == "FRAMERATE") { out.framerate = std::max(c.Float(line), 1.0f); }
                else if (kw == "NUMFRAMES")
                {
                    out.numFrames = std::max(c.Int(line), 0);
                    out.m_poses.assign((size_t)out.numFrames * out.parts.size(), AnimPose{});
                }
                else if (kw == "NOTETRACKS") { inNotes = true; curFrame = -1; }
                else if (inNotes)
                {
                    if (kw == "FRAME")
                    {
                        int f = c.Int(line);
                        size_t q1 = line.find('"');
                        if (q1 != std::string_view::npos)
                            out.notes.push_back({ f, std::string(line.substr(q1 + 1, line.find('"', q1 + 1) - q1 - 1)) });
                    }
                }
                else if (kw == "FRAME") { curFrame = c.Int(line); curPart = -1; }
                else if (curFrame >= 0 && curFrame < out.numFrames)
                {
                    if (kw == "PART") curPart = c.Int(line);
                    else if (curPart >= 0 && curPart < (int)out.parts.size())
                    {
                        AnimPose& p = out.m_poses[(size_t)curFrame * out.parts.size() + curPart];
                        if (kw == "OFFSET") p.pos = c.Vec(line);
                        else if (kw == "X") axes[0] = c.Vec(line);
                        else if (kw == "Y") axes[1] = c.Vec(line);
                        else if (kw == "Z") { axes[2] = c.Vec(line); p.rot = Quat::FromAxes(axes[0], axes[1], axes[2]); p.valid = true; }
                    }
                }
            }
            if (out.numFrames <= 0 || out.parts.empty()) { error = "no animation data (is this an xanim_export file?)"; return false; }
            // parts that are missing in a frame keep the previous frame's pose
            for (int f = 1; f < out.numFrames; ++f)
                for (size_t p = 0; p < out.parts.size(); ++p)
                    if (!out.At(f, p).valid && out.At(f - 1, p).valid) out.m_poses[(size_t)f * out.parts.size() + p] = out.At(f - 1, p);
            return true;
        }

        // Reads a compiled raw/xanim file (version 17). All parts are stored as keyframes of local (parent relative)
        // rotations and translations; they are expanded here to one pose per frame. Delta animations lose their root motion,
        // additive ones are not supported.
        static bool LoadCompiled(const fs::path& path, Animation& out, std::string& error)
        {
            std::vector<unsigned char> d;
            if (!ReadBinaryFile(path, d)) { error = "cannot read " + path.string(); return false; }
            out = Animation{};
            out.name = path.filename().string();
            out.localPoses = true;
            Rd r{ d };
            if (r.U16() != 17) { error = "unsupported xanim version"; return false; }
            int frames = r.U16(), nparts = r.U16();
            int flags = r.U8(), type = r.U8();
            out.framerate = (float)std::max<int>(r.U16(), 1);
            if (!r.ok) { error = "truncated xanim"; return false; }
            if (type == 6) { error = "additive animations are not supported"; return false; }
            bool loop = (flags & 1) != 0;
            int L = frames + (loop ? 1 : 0);          // key index range (a looping animation repeats frame 0 at the end)
            auto indices = [&](int n) {
                std::vector<int> v;
                if (n < L) { for (int i = 0; i < n; ++i) v.push_back(L <= 256 ? (int)r.U8() : (int)r.U16()); }
                else for (int i = 0; i < n; ++i) v.push_back(i);
                return v;
            };
            struct Keys { std::vector<int> idx; std::vector<Vec3> v; };
            auto readTrans = [&](Keys& k) {
                int n = r.U16();
                if (n == 1) { k.idx = { 0 }; k.v = { { r.F32(), r.F32(), r.F32() } }; }
                else if (n > 1)
                {
                    k.idx = indices(n);
                    bool q8 = r.U8() != 0;
                    Vec3 mn{ r.F32(), r.F32(), r.F32() }, sz{ r.F32(), r.F32(), r.F32() };
                    for (int i = 0; i < n; ++i)
                    {
                        float a, b, c;
                        if (q8) { a = r.U8() / 255.0f; b = r.U8() / 255.0f; c = r.U8() / 255.0f; }
                        else { a = r.U16() / 65535.0f; b = r.U16() / 65535.0f; c = r.U16() / 65535.0f; }
                        k.v.push_back({ mn.x + sz.x * a, mn.y + sz.y * b, mn.z + sz.z * c });
                    }
                }
            };
            if (type == 2)    // delta: skip the root's rotation (one short per key) and translation
            {
                int n = r.U16();
                if (n) { if (n > 1) indices(n); for (int i = 0; i < n; ++i) r.U16(); }
                Keys skip; readTrans(skip);
            }
            int nb = nparts ? ((nparts - 1) >> 3) + 1 : 0;
            r.Skip(nb);                                             // sign flip flags (rotation is sign-normalised anyway)
            std::vector<unsigned char> simple(d.begin() + std::min(r.p, d.size()), d.begin() + std::min(r.p + nb, d.size()));
            r.Skip(nb);
            for (int i = 0; i < nparts; ++i) out.parts.push_back(r.CStr());
            if (!r.ok) { error = "truncated xanim"; return false; }

            out.numFrames = frames;
            out.m_poses.assign((size_t)frames * nparts, AnimPose{});
            for (int pi = 0; pi < nparts; ++pi)
            {
                bool two = (simple[pi >> 3] >> (pi & 7)) & 1;
                int nq = r.U16();
                std::vector<int> qidx; std::vector<Quat> q;
                if (nq)
                {
                    qidx = nq > 1 ? indices(nq) : std::vector<int>{ 0 };
                    for (int i = 0; i < nq; ++i)
                    {
                        float x = 0, y = 0, z;
                        if (two) z = r.S16() / 32767.0f;
                        else { x = r.S16() / 32767.0f; y = r.S16() / 32767.0f; z = r.S16() / 32767.0f; }
                        float w2 = 1 - x * x - y * y - z * z;
                        q.push_back(Quat{ x, y, z, w2 > 0 ? std::sqrt(w2) : 0 }.Normalized());
                    }
                }
                Keys tk; readTrans(tk);
                if (!r.ok) { error = "truncated xanim data"; return false; }
                for (int f = 0; f < frames; ++f)
                {
                    AnimPose& p = out.m_poses[(size_t)f * nparts + pi];
                    p.valid = true;
                    p.hasRot = !q.empty(); p.hasPos = !tk.v.empty();
                    p.rot = q.empty() ? Quat{} : SampleKeys(qidx, q, f);
                    p.pos = tk.v.empty() ? Vec3{} : SampleKeysV(tk.idx, tk.v, f);
                }
            }
            int notes = r.U8();
            for (int i = 0; i < notes && r.ok; ++i) { NoteKey k; k.name = r.CStr(); k.frame = r.U16(); out.notes.push_back(std::move(k)); }
            return true;
        }

    private:
        struct Rd
        {
            const std::vector<unsigned char>& d;
            size_t p = 0;
            bool ok = true;
            bool Has(size_t n) { if (p + n > d.size()) ok = false; return ok; }
            int U8() { return Has(1) ? d[p++] : 0; }
            int U16() { if (!Has(2)) return 0; int v = d[p] | (d[p + 1] << 8); p += 2; return v; }
            int S16() { return (int16_t)U16(); }
            float F32() { if (!Has(4)) return 0; float v; std::memcpy(&v, &d[p], 4); p += 4; return v; }
            void Skip(size_t n) { if (Has(n)) p += n; }
            std::string CStr() { std::string s; while (p < d.size() && d[p]) s += (char)d[p++]; if (p >= d.size()) ok = false; else ++p; return s; }
        };

        static void Segment(const std::vector<int>& idx, int f, size_t& a, size_t& b, float& t)
        {
            size_t n = idx.size();
            a = b = 0; t = 0;
            if (n <= 1 || f <= idx[0]) return;
            if (f >= idx[n - 1]) { a = b = n - 1; return; }
            size_t i = 1;
            while (i < n && idx[i] <= f) ++i;
            a = i - 1; b = i;
            t = (float)(f - idx[a]) / (float)std::max(idx[b] - idx[a], 1);
        }
        static Quat SampleKeys(const std::vector<int>& idx, const std::vector<Quat>& q, int f)
        {
            size_t a, b; float t; Segment(idx, f, a, b, t);
            return a == b ? q[a] : Quat::Slerp(q[a], q[b], t);
        }
        static Vec3 SampleKeysV(const std::vector<int>& idx, const std::vector<Vec3>& v, int f)
        {
            size_t a, b; float t; Segment(idx, f, a, b, t);
            return a == b ? v[a] : v[a] + (v[b] - v[a]) * t;
        }

        // Allocation-free line / number scanner (animation files are tens of megabytes).
        struct Cursor
        {
            const std::string& t;
            size_t p = 0;
            bool NextLine(std::string_view& line)
            {
                if (p >= t.size()) return false;
                size_t e = t.find('\n', p);
                if (e == std::string::npos) e = t.size();
                size_t b = p, en = e;
                p = e + 1;
                while (b < en && (t[b] == ' ' || t[b] == '\t')) ++b;
                while (en > b && (t[en - 1] == '\r' || t[en - 1] == ' ' || t[en - 1] == '\t')) --en;
                line = std::string_view(t).substr(b, en - b);
                m_rest = 0;
                return true;
            }
            std::string_view Word(std::string_view line)
            {
                size_t e = line.find_first_of(" \t");
                if (e == std::string_view::npos) e = line.size();
                m_rest = e;
                return line.substr(0, e);
            }
            float Float(std::string_view line)
            {
                while (m_rest < line.size() && (line[m_rest] == ' ' || line[m_rest] == '\t' || line[m_rest] == ',')) ++m_rest;
                char* end = nullptr;
                float v = std::strtof(line.data() + m_rest, &end);   // views end at a line break or the terminator
                m_rest = (size_t)(end - line.data());
                return v;
            }
            int Int(std::string_view line) { return (int)Float(line); }
            Vec3 Vec(std::string_view line) { float x = Float(line), y = Float(line), z = Float(line); return { x, y, z }; }
            size_t m_rest = 0;
        };

        std::vector<AnimPose> m_poses;
    };
}
