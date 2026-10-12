module;

#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module openape.gdt;

import openape.util;
import openape.gdf;

// .gdt "game data" files: text containers holding many asset entries, each tagged with the .gdf
// definition that describes it:
//     {
//         "name" ( "xmodel.gdf" )
//         {
//             "key" "value"
//         }
//     }
export namespace oa::gdt
{
    class GdtFile;

    class Entry : public gdf::IValueSource
    {
    public:
        Entry(std::string name, std::string gdfName, const gdf::Def* def, GdtFile* owner)
            : m_name(std::move(name)), m_gdfName(std::move(gdfName)), m_def(def), m_owner(owner) {}

        const std::string& Name() const { return m_name; }
        const std::string& GdfName() const { return m_gdfName; }
        const gdf::Def* Def() const { return m_def; }
        GdtFile* Owner() const { return m_owner; }
        void Rename(std::string n);
        const std::vector<std::pair<std::string, std::string>>& Values() const { return m_values; }

        // Raw stored string; falls back to the variable's default when the key is absent.
        std::string GetString(std::string_view key) const
        {
            if (auto* s = FindRaw(key)) return *s;
            if (m_def) if (auto* v = m_def->Find(key)) return v->DefaultString();
            return {};
        }
        bool Has(std::string_view key) const { return FindRaw(key) != nullptr; }

        double GetNumber(std::string_view key) const { return std::strtod(GetString(key).c_str(), nullptr); }
        int GetInt(std::string_view key) const { return (int)std::llround(GetNumber(key)); }

        std::array<float, 4> GetColor(std::string_view key) const
        {
            std::array<float, 4> c{ 1, 1, 1, 1 };
            std::string s = GetString(key);
            const char* p = s.c_str();
            for (auto& f : c) { char* e; f = std::strtof(p, &e); if (e == p) break; p = e; }
            return c;
        }

        // Returns true when the stored value changed.
        bool Set(std::string_view key, const std::string& value);
        void SetRaw(const std::string& key, const std::string& value)   // loader: no dirty tracking
        {
            m_values.emplace_back(key, value);
        }

        void FillDefaults()
        {
            if (!m_def) return;
            for (auto& v : m_def->vars)
                if (!FindRaw(v.name)) m_values.emplace_back(v.name, v.DefaultString());
        }

        std::unique_ptr<Entry> Clone(std::string newName, GdtFile* owner) const
        {
            auto e = std::make_unique<Entry>(std::move(newName), m_gdfName, m_def, owner);
            e->m_values = m_values;
            return e;
        }

        // IValueSource
        const gdf::Variable* FindVariable(std::string_view name) const override { return m_def ? m_def->Find(name) : nullptr; }
        gdf::Value GetValue(const gdf::Variable& v) const override
        {
            return gdf::Def::ValueFromString(v, GetString(v.name));
        }

    private:
        const std::string* FindRaw(std::string_view key) const
        {
            for (auto& kv : m_values) if (kv.first == key) return &kv.second;
            return nullptr;
        }

        std::string m_name, m_gdfName;
        const gdf::Def* m_def;
        GdtFile* m_owner;
        std::vector<std::pair<std::string, std::string>> m_values;
    };

    using DefResolver = std::function<const gdf::Def*(const std::string& gdfName)>;

    class GdtFile
    {
    public:
        explicit GdtFile(fs::path p) : m_path(std::move(p)) {}

        const fs::path& Path() const { return m_path; }
        std::string Name() const { return m_path.stem().string(); }
        bool Dirty() const { return m_dirty; }
        void MarkDirty() { m_dirty = true; }
        const std::vector<std::string>& Errors() const { return m_errors; }
        std::vector<std::unique_ptr<Entry>>& Entries() { return m_entries; }
        const std::vector<std::unique_ptr<Entry>>& Entries() const { return m_entries; }

        bool IsReadOnly() const
        {
            std::error_code ec;
            auto p = fs::status(m_path, ec).permissions();
            return !ec && (p & fs::perms::owner_write) == fs::perms::none;
        }

        Entry* Find(std::string_view name) const
        {
            for (auto& e : m_entries) if (e->Name() == name) return e.get();
            return nullptr;
        }

        Entry* Add(std::unique_ptr<Entry> e, size_t at = (size_t)-1)
        {
            Entry* raw = e.get();
            if (at >= m_entries.size()) m_entries.push_back(std::move(e));
            else m_entries.insert(m_entries.begin() + at, std::move(e));
            m_dirty = true;
            return raw;
        }

        std::unique_ptr<Entry> Remove(const Entry* e)
        {
            for (size_t i = 0; i < m_entries.size(); ++i)
                if (m_entries[i].get() == e)
                {
                    auto r = std::move(m_entries[i]);
                    m_entries.erase(m_entries.begin() + i);
                    m_dirty = true;
                    return r;
                }
            return nullptr;
        }

        bool Load(const DefResolver& resolve)
        {
            std::string text;
            m_entries.clear(); m_errors.clear();
            if (!ReadTextFile(m_path, text)) { m_errors.push_back("cannot read file"); return false; }
            std::error_code ec;
            m_stamp = fs::last_write_time(m_path, ec);
            m_crlf = text.find("\r\n") != std::string::npos;
            Reader r{ text, 0, 1 };
            try
            {
                r.Expect('{');
                while (!r.Accept('}'))
                {
                    std::string name = r.String();
                    r.Expect('('); std::string gdfName = r.String(); r.Expect(')');
                    auto e = std::make_unique<Entry>(name, gdfName, resolve(gdfName), this);
                    r.Expect('{');
                    while (!r.Accept('}'))
                    {
                        std::string k = r.String(); std::string v = r.String();
                        e->SetRaw(k, v);
                    }
                    m_entries.push_back(std::move(e));
                }
            }
            catch (const std::string& msg) { m_errors.push_back(msg); }
            m_dirty = false;
            return m_errors.empty();
        }

        std::string Serialize() const
        {
            std::string out = "\n{\n";
            for (auto& e : m_entries)
            {
                out += "\t\"" + Escape(e->Name()) + "\" ( \"" + Escape(e->GdfName()) + "\" )\n\t{\n";
                for (auto& kv : e->Values())
                    out += "\t\t\"" + Escape(kv.first) + "\" \"" + Escape(kv.second) + "\"\n";
                out += "\t}\n";
            }
            out += "}\n";
            if (m_crlf)
            {
                std::string crlf;
                for (char c : out) { if (c == '\n') crlf += '\r'; crlf += c; }
                return crlf;
            }
            return out;
        }

        // Empty string on success, otherwise the error message.
        std::string Save()
        {
            if (IsReadOnly()) return "The GDT file is read only: " + m_path.string();
            if (!WriteTextFile(m_path, Serialize())) return "Could not write " + m_path.string();
            std::error_code ec;
            m_stamp = fs::last_write_time(m_path, ec);
            m_dirty = false;
            return {};
        }

        // True when the file on disk is newer than what we loaded.
        bool ChangedOnDisk() const
        {
            std::error_code ec;
            auto t = fs::last_write_time(m_path, ec);
            return !ec && t != m_stamp;
        }

        static std::string Escape(std::string_view s)
        {
            std::string r;
            for (char c : s)
            {
                switch (c)
                {
                case '\\': r += "\\\\"; break;
                case '"':  r += "\\\""; break;
                case '\n': r += "\\n"; break;
                case '\r': r += "\\r"; break;
                case '\t': r += "\\t"; break;
                default: r += c;
                }
            }
            return r;
        }

    private:
        struct Reader
        {
            const std::string& s;
            size_t pos;
            int line;

            void Skip()
            {
                for (;;)
                {
                    while (pos < s.size() && std::isspace((unsigned char)s[pos])) { if (s[pos] == '\n') ++line; ++pos; }
                    if (s.compare(pos, 2, "//") == 0) { while (pos < s.size() && s[pos] != '\n') ++pos; continue; }
                    return;
                }
            }
            std::string Err(const std::string& m) const { return "line " + std::to_string(line) + ": " + m; }
            bool Accept(char c) { Skip(); if (pos < s.size() && s[pos] == c) { ++pos; return true; } return false; }
            void Expect(char c) { if (!Accept(c)) throw Err(std::string("expected '") + c + "'"); }
            std::string String()
            {
                Skip();
                if (pos >= s.size() || s[pos] != '"') throw Err("expected string");
                ++pos;
                std::string r;
                while (pos < s.size() && s[pos] != '"')
                {
                    char c = s[pos++];
                    if (c == '\\' && pos < s.size())
                    {
                        char n = s[pos++];
                        switch (n) { case 'n': r += '\n'; break; case 't': r += '\t'; break; case 'r': r += '\r'; break;
                                     case '\\': r += '\\'; break; case '"': r += '"'; break; default: r += '\\'; r += n; }
                    }
                    else { if (c == '\n') ++line; r += c; }
                }
                if (pos >= s.size()) throw Err("unterminated string");
                ++pos;
                return r;
            }
        };

        fs::path m_path;
        std::vector<std::unique_ptr<Entry>> m_entries;
        std::vector<std::string> m_errors;
        bool m_dirty = false;
        bool m_crlf = false;
        fs::file_time_type m_stamp{};
    };

    inline void Entry::Rename(std::string n) { m_name = std::move(n); if (m_owner) m_owner->MarkDirty(); }

    inline bool Entry::Set(std::string_view key, const std::string& value)
    {
        for (auto& kv : m_values)
            if (kv.first == key)
            {
                if (kv.second == value) return false;
                kv.second = value;
                if (m_owner) m_owner->MarkDirty();
                return true;
            }
        m_values.emplace_back(std::string(key), value);
        if (m_owner) m_owner->MarkDirty();
        return true;
    }
}
