module;

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <future>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

export module openape.skeletonindex;

import openape.util;

// An index of the bone names of every compiled model in <root>/raw, used to find a skeleton that an
// animation (which only stores bone NAMES, not the hierarchy) can be applied to.
export namespace oa
{
    class SkeletonIndex
    {
    public:
        struct Entry
        {
            std::string model;                           // file name in raw/xmodel
            std::vector<std::string> bones;              // lower-case
            std::unordered_set<std::string> boneSet;
        };

        struct Candidate
        {
            const Entry* entry = nullptr;
            int covered = 0;       // animated parts that exist in this skeleton
            int extra = 0;         // skeleton bones the animation does not touch
        };

        // Starts scanning in the background; Match() waits for the result.
        void Build(const fs::path& rawDir)
        {
            m_future = std::async(std::launch::async, [rawDir] { return Scan(rawDir); });
            m_ready = false;
        }

        const std::vector<Entry>& Entries()
        {
            if (!m_ready && m_future.valid()) { m_entries = m_future.get(); m_ready = true; }
            return m_entries;
        }

        const Entry* Find(const std::string& model)
        {
            for (auto& e : Entries()) if (IEquals(e.model, model)) return &e;
            return nullptr;
        }

        // Best skeletons for an animation with the given bone names, best first.
        std::vector<Candidate> Match(const std::vector<std::string>& parts, size_t maxResults = 40)
        {
            std::unordered_set<std::string> want;
            for (auto& p : parts) want.insert(ToLower(p));
            std::vector<Candidate> out;
            for (auto& e : Entries())
            {
                int covered = 0;
                for (auto& w : want) if (e.boneSet.count(w)) ++covered;
                if (covered == 0) continue;
                out.push_back({ &e, covered, (int)e.bones.size() - covered });
            }
            std::sort(out.begin(), out.end(), [](const Candidate& a, const Candidate& b) {
                if (a.covered != b.covered) return a.covered > b.covered;
                if (a.extra != b.extra) return a.extra < b.extra;
                return a.entry->model < b.entry->model;
            });
            if (out.size() > maxResults) out.resize(maxResults);
            return out;
        }

    private:
        static std::vector<Entry> Scan(const fs::path& raw)
        {
            std::vector<Entry> result;
            std::error_code ec;
            for (auto& de : fs::directory_iterator(raw / "xmodel", ec))
            {
                if (!de.is_regular_file()) continue;
                std::string lod0 = ReadLod0Name(de.path());
                if (lod0.empty()) continue;
                Entry e;
                e.model = de.path().filename().string();
                if (!ReadBoneNames(raw / "xmodelparts" / lod0, e.bones) || e.bones.empty()) continue;
                for (auto& b : e.bones) e.boneSet.insert(b);
                result.push_back(std::move(e));
            }
            return result;
        }

        static std::string ReadLod0Name(const fs::path& file)
        {
            std::vector<unsigned char> d;
            FILE* f = _wfopen(file.c_str(), L"rb");
            if (!f) return {};
            d.resize(512);
            d.resize(fread(d.data(), 1, d.size(), f));
            fclose(f);
            // u16 version, u8 flags, float[6], cstring, then (float distance, cstring name) per LOD
            size_t p = 27;
            while (p < d.size() && d[p]) ++p;
            p += 1 + 4;
            std::string name;
            while (p < d.size() && d[p]) name += (char)d[p++];
            return name;
        }

        static bool ReadBoneNames(const fs::path& file, std::vector<std::string>& names)
        {
            std::vector<unsigned char> d;
            if (!ReadBinaryFile(file, d) || d.size() < 6) return false;
            uint16_t nonRoot, roots;
            std::memcpy(&nonRoot, &d[2], 2); std::memcpy(&roots, &d[4], 2);
            size_t p = 6 + (size_t)nonRoot * 19;
            for (size_t i = 0; i < (size_t)nonRoot + roots; ++i)
            {
                std::string s;
                while (p < d.size() && d[p]) s += (char)d[p++];
                if (p >= d.size()) return false;
                ++p;
                names.push_back(ToLower(s));
            }
            return true;
        }

        std::future<std::vector<Entry>> m_future;
        std::vector<Entry> m_entries;
        bool m_ready = false;
    };
}
