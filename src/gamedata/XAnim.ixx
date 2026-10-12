module;

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
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

    private:
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
