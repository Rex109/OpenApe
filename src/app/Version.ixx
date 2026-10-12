module;

#include <array>
#include <string>
#include "buildinfo.h"

export module openape.version;

// The version of this build (x.x.x) and the git state it was built from.
export namespace oa::version
{
    inline constexpr const char* kVersion = "1.0.0";
    inline constexpr const char* kCommit = OPENAPE_COMMIT;
    inline constexpr int kCommitCount = OPENAPE_COMMIT_COUNT;
    inline constexpr bool kDirty = OPENAPE_DIRTY != 0;

    // "x.y.z" -> {x, y, z}; a leading 'v' and anything after the third number (-rc1, +build) is ignored.
    inline std::array<int, 3> Parse(const std::string& text)
    {
        std::array<int, 3> v{ 0, 0, 0 };
        size_t i = 0;
        while (i < text.size() && (text[i] == 'v' || text[i] == 'V' || text[i] == ' ')) ++i;
        for (int part = 0; part < 3 && i < text.size(); ++part)
        {
            int n = 0;
            bool any = false;
            while (i < text.size() && text[i] >= '0' && text[i] <= '9') { n = n * 10 + (text[i] - '0'); ++i; any = true; }
            if (!any) break;
            v[part] = n;
            if (i < text.size() && text[i] == '.') ++i; else break;
        }
        return v;
    }

    inline bool IsNewer(const std::string& candidate, const std::string& current) { return Parse(candidate) > Parse(current); }

    // "83c50f6  (modified)" for the window title
    inline std::string BuildLabel()
    {
        std::string s = kCommit;
        if (kDirty) s += "  (modified)";
        return s;
    }

    // "1.0.0 (build 12, 83c50f6, dirty)"
    inline std::string Describe()
    {
        std::string s = kVersion;
        s += " (build " + std::to_string(kCommitCount) + ", " + kCommit;
        if (kDirty) s += ", dirty";
        return s + ")";
    }
}
