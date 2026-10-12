module;
#include <utility>

#include <algorithm>
#include <array>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <windows.h>

export module openape.project;

import openape.util;
import openape.gdf;
import openape.gdt;

// A project is a "game tree": a root directory containing `deffiles\` (the .gdf definitions),
// `bin\` (converter tools) and the .gdt source data.
export namespace oa
{
    class Project
    {
    public:
        // Same search as asset_manager.exe: walk up from `start` until a directory holds "deffiles".
        static fs::path FindRoot(fs::path start)
        {
            std::error_code ec;
            start = fs::absolute(start, ec);
            for (fs::path p = start; !p.empty(); p = p.parent_path())
            {
                if (fs::is_directory(p / "deffiles", ec)) return p;
                if (p == p.parent_path()) break;
            }
            return {};
        }

        bool Open(const fs::path& root)
        {
            m_errors.clear(); m_defs.clear(); m_defIndex.clear(); m_gdts.clear(); m_gdtDirs.clear();
            m_root = root;
            std::error_code ec;
            if (!fs::is_directory(root / "deffiles", ec))
            {
                m_errors.push_back("No 'deffiles' directory in " + root.string());
                m_root.clear();
                return false;
            }
            LoadDefs();
            CollectGdtDirs();
            LoadGdts();
            ++m_revision;
            return true;
        }

        const fs::path& Root() const { return m_root; }
        bool IsOpen() const { return !m_root.empty(); }
        const std::vector<std::string>& Errors() const { return m_errors; }
        const std::vector<fs::path>& GdtDirs() const { return m_gdtDirs; }
        // Bumped whenever entries are added, removed, renamed or reloaded.
        unsigned Revision() const { return m_revision; }
        void Touch() { ++m_revision; }

        const std::vector<std::unique_ptr<gdf::Def>>& Defs() const { return m_defs; }
        std::vector<std::unique_ptr<gdt::GdtFile>>& Gdts() { return m_gdts; }
        const std::vector<std::unique_ptr<gdt::GdtFile>>& Gdts() const { return m_gdts; }

        const gdf::Def* FindDef(const std::string& gdfName) const
        {
            auto it = m_defIndex.find(ToLower(gdfName));
            return it == m_defIndex.end() ? nullptr : m_defs[it->second].get();
        }

        gdt::Entry* FindEntry(const std::string& gdfName, const std::string& name) const
        {
            for (auto& g : m_gdts)
                for (auto& e : g->Entries())
                    if (e->Name() == name && IEquals(e->GdfName(), gdfName)) return e.get();
            return nullptr;
        }

        size_t EntryCount() const
        {
            size_t n = 0;
            for (auto& g : m_gdts) n += g->Entries().size();
            return n;
        }

        bool Owns(const gdt::Entry* e) const
        {
            for (auto& g : m_gdts)
                for (auto& x : g->Entries()) if (x.get() == e) return true;
            return false;
        }

        // Names of all entries whose type is one of `gdfs` (used by keyenum variables).
        const std::vector<std::string>& EntryNames(const std::vector<std::string>& gdfs)
        {
            std::string key;
            for (auto& g : gdfs) key += ToLower(g) + ";";
            auto& c = m_keyCache[key];
            if (c.first != m_revision)
            {
                std::set<std::string> names;
                for (auto& g : m_gdts)
                    for (auto& e : g->Entries())
                        for (auto& t : gdfs) if (IEquals(e->GdfName(), t)) { names.insert(e->Name()); break; }
                c.second.assign(names.begin(), names.end());
                c.first = m_revision;
            }
            return c.second;
        }

        // Looks for a source file referenced by an asset in the usual tree locations.
        fs::path ResolveFile(const std::string& rel, const std::string& relDir = {}) const
        {
            if (rel.empty()) return {};
            std::error_code ec;
            fs::path r = NormalizeSlashes(rel);
            if (r.is_absolute()) return fs::is_regular_file(r, ec) ? r : fs::path{};
            std::vector<fs::path> bases;
            if (!relDir.empty()) bases.push_back(m_root / NormalizeSlashes(relDir));
            for (auto d : { "", "texture_assets", "raw/images", "model_export", "raw", "source_data", "map_source" })
                bases.push_back(m_root / d);
            for (auto& b : bases)
            {
                fs::path p = b / r;
                if (fs::is_regular_file(p, ec)) return p;
            }
            return {};
        }

        bool AnyDirty() const
        {
            for (auto& g : m_gdts) if (g->Dirty()) return true;
            return false;
        }

        // Returns messages for files that could not be saved.
        std::vector<std::string> SaveAll()
        {
            std::vector<std::string> errs;
            for (auto& g : m_gdts)
                if (g->Dirty()) { auto e = g->Save(); if (!e.empty()) errs.push_back(e); }
            return errs;
        }

        // Re-reads one GDT from disk (drops unsaved changes).
        void ReloadGdt(gdt::GdtFile& g)
        {
            g.Load([this](const std::string& n) { return FindDef(n); });
            ++m_revision;
        }

        gdt::GdtFile* CreateGdt(const std::string& name)
        {
            fs::path dir = m_gdtDirs.empty() ? m_root / "source_data" : m_gdtDirs.front();
            std::error_code ec;
            fs::create_directories(dir, ec);
            fs::path p = dir / (name + ".gdt");
            if (fs::exists(p, ec)) return nullptr;
            auto g = std::make_unique<gdt::GdtFile>(p);
            g->MarkDirty();
            m_gdts.push_back(std::move(g));
            ++m_revision;
            return m_gdts.back().get();
        }

        // Runs bin\<name>.bat (or .exe) like asset_manager.exe does for its convert commands.
        // Returns an error message, empty on success.
        std::string RunTool(const std::string& name, const std::string& args) const
        {
            fs::path bin = m_root / "bin";
            for (auto ext : { ".bat", ".exe" })
            {
                fs::path exe = bin / (name + ext);
                std::error_code ec;
                if (!fs::exists(exe, ec)) continue;
                std::string cmd = "\"" + exe.string() + "\" " + args;
                STARTUPINFOA si{}; si.cb = sizeof(si);
                PROCESS_INFORMATION pi{};
                std::string dir = bin.string();
                if (CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr, dir.c_str(), &si, &pi))
                {
                    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
                    return {};
                }
                return "CreateProcess failed for " + exe.string();
            }
            return "Neither " + (bin / (name + ".bat")).string() + " nor " + (bin / (name + ".exe")).string() + " exists";
        }

    private:
        void LoadDefs()
        {
            std::error_code ec;
            std::vector<fs::path> files;
            for (auto& de : fs::directory_iterator(m_root / "deffiles", ec))
                if (de.is_regular_file() && IEquals(de.path().extension().string(), ".gdf")) files.push_back(de.path());
            std::sort(files.begin(), files.end());
            for (auto& f : files)
            {
                auto d = gdf::Def::Load(f);
                if (!d) { m_errors.push_back("cannot read " + f.string()); continue; }
                for (auto& e : d->errors) m_errors.push_back(d->fileName + ": " + e);
                m_defIndex[ToLower(d->fileName)] = m_defs.size();
                m_defs.push_back(std::move(d));
            }
        }

        void CollectGdtDirs()
        {
            std::string text;
            if (ReadTextFile(m_root / "bin" / "converter_gdt_dirs_0.txt", text))
            {
                std::istringstream ss(text);
                for (std::string line; std::getline(ss, line);)
                {
                    line = Trim(line);
                    if (line.empty()) continue;
                    fs::path p = NormalizeSlashes(line);
                    if (p.is_relative()) p = m_root / p;
                    m_gdtDirs.push_back(p);
                }
            }
            if (m_gdtDirs.empty()) m_gdtDirs.push_back(m_root / "source_data");
        }

        void LoadGdts()
        {
            std::error_code ec;
            for (auto& dir : m_gdtDirs)
            {
                std::vector<fs::path> files;
                for (auto& de : fs::directory_iterator(dir, ec))
                    if (de.is_regular_file() && IEquals(de.path().extension().string(), ".gdt")) files.push_back(de.path());
                std::sort(files.begin(), files.end());
                for (auto& f : files)
                {
                    auto g = std::make_unique<gdt::GdtFile>(f);
                    g->Load([this](const std::string& n) { return FindDef(n); });
                    for (auto& e : g->Errors()) m_errors.push_back(f.filename().string() + ": " + e);
                    m_gdts.push_back(std::move(g));
                }
            }
        }

        fs::path m_root;
        std::vector<std::unique_ptr<gdf::Def>> m_defs;
        std::unordered_map<std::string, size_t> m_defIndex;
        std::vector<std::unique_ptr<gdt::GdtFile>> m_gdts;
        std::vector<fs::path> m_gdtDirs;
        std::vector<std::string> m_errors;
        std::map<std::string, std::pair<unsigned, std::vector<std::string>>> m_keyCache;
        unsigned m_revision = 1;
    };
}
