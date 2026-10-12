module;

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

export module openape.util;

export namespace oa
{
    namespace fs = std::filesystem;

    inline std::string ToLower(std::string_view s)
    {
        std::string r(s);
        std::transform(r.begin(), r.end(), r.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return r;
    }

    inline std::string Trim(std::string_view s)
    {
        size_t b = 0, e = s.size();
        while (b < e && std::isspace((unsigned char)s[b])) ++b;
        while (e > b && std::isspace((unsigned char)s[e - 1])) --e;
        return std::string(s.substr(b, e - b));
    }

    inline bool IEquals(std::string_view a, std::string_view b)
    {
        return a.size() == b.size() &&
               std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) { return std::tolower(x) == std::tolower(y); });
    }

    inline bool IContains(std::string_view hay, std::string_view needle)
    {
        if (needle.empty()) return true;
        auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
                              [](unsigned char x, unsigned char y) { return std::tolower(x) == std::tolower(y); });
        return it != hay.end();
    }

    inline bool EndsWith(std::string_view s, std::string_view suffix)
    {
        return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
    }

    // Case-insensitive wildcard match ('*' and '?'), used by the name filter.
    inline bool WildcardMatch(std::string_view text, std::string_view pattern)
    {
        size_t t = 0, p = 0, star = std::string_view::npos, mark = 0;
        while (t < text.size())
        {
            if (p < pattern.size() && (pattern[p] == '?' || std::tolower((unsigned char)pattern[p]) == std::tolower((unsigned char)text[t])))
            {
                ++t; ++p;
            }
            else if (p < pattern.size() && pattern[p] == '*')
            {
                star = p++; mark = t;
            }
            else if (star != std::string_view::npos)
            {
                p = star + 1; t = ++mark;
            }
            else return false;
        }
        while (p < pattern.size() && pattern[p] == '*') ++p;
        return p == pattern.size();
    }

    inline bool ReadTextFile(const fs::path& path, std::string& out)
    {
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        std::ostringstream ss;
        ss << f.rdbuf();
        out = ss.str();
        return true;
    }

    inline bool ReadBinaryFile(const fs::path& path, std::vector<unsigned char>& out)
    {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f) return false;
        out.resize((size_t)f.tellg());
        f.seekg(0);
        f.read((char*)out.data(), (std::streamsize)out.size());
        return true;
    }

    inline bool WriteTextFile(const fs::path& path, const std::string& data)
    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(data.data(), (std::streamsize)data.size());
        return (bool)f;
    }

    inline std::string FormatFloat(double v)
    {
        char buf[64];
        auto r = std::to_chars(buf, buf + sizeof(buf), v);
        return std::string(buf, r.ptr);
    }

    inline std::string NormalizeSlashes(std::string s)
    {
        std::replace(s.begin(), s.end(), '/', '\\');
        return s;
    }

    // "colorMap" -> "Color Map", "locale_Middle_East" -> "Locale Middle East"
    inline std::string PrettifyName(std::string_view n)
    {
        std::string r;
        for (size_t i = 0; i < n.size(); ++i)
        {
            char c = n[i];
            if (c == '_') { r += ' '; continue; }
            bool upper = std::isupper((unsigned char)c) != 0;
            bool prevLower = i > 0 && std::islower((unsigned char)n[i - 1]);
            bool prevDigit = i > 0 && std::isdigit((unsigned char)n[i - 1]) && !std::isdigit((unsigned char)c);
            if ((upper && prevLower) || prevDigit) r += ' ';
            if (r.empty() || r.back() == ' ') c = (char)std::toupper((unsigned char)c);
            r += c;
        }
        return r;
    }
}
