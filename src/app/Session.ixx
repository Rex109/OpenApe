module;
#include <utility>

#include <algorithm>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include <filesystem>

export module openape.session;

import openape.util;
import openape.gdf;
import openape.gdt;
import openape.project;
import openape.gamedata;
import openape.skeletonindex;
import openape.iwd;

export namespace oa
{
    inline double NowSeconds()
    {
        using namespace std::chrono;
        return duration<double>(steady_clock::now().time_since_epoch()).count();
    }

    // ------------------------------------------------------------------ undo / redo
    class UndoStack
    {
    public:
        struct Action
        {
            std::string label;
            gdt::Entry* entry = nullptr;      // set for coalescable value edits
            std::string key, oldValue, newValue;
            std::function<void()> undo, redo;
            double time = 0;
        };

        bool CanUndo() const { return m_pos > 0; }
        bool CanRedo() const { return m_pos < m_actions.size(); }
        const std::string& UndoLabel() const { return m_actions[m_pos - 1].label; }
        const std::string& RedoLabel() const { return m_actions[m_pos].label; }
        void Clear() { m_actions.clear(); m_pos = 0; }

        void Push(Action a)
        {
            m_actions.resize(m_pos);
            m_actions.push_back(std::move(a));
            m_pos = m_actions.size();
        }

        // Typing into a field produces many edits; merge those made in quick succession.
        void PushValueEdit(gdt::Entry& e, const std::string& key, const std::string& oldV, const std::string& newV)
        {
            double now = NowSeconds();
            if (m_pos > 0 && m_pos == m_actions.size())
            {
                Action& top = m_actions.back();
                if (top.entry == &e && top.key == key && now - top.time < 0.8)
                {
                    top.newValue = newV; top.time = now;
                    gdt::Entry* ep = &e; std::string k = key, nv = newV;
                    top.redo = [ep, k, nv] { ep->Set(k, nv); };
                    return;
                }
            }
            Action a;
            a.label = "Edit " + key;
            a.entry = &e; a.key = key; a.oldValue = oldV; a.newValue = newV; a.time = now;
            gdt::Entry* ep = &e; std::string k = key, ov = oldV, nv = newV;
            a.undo = [ep, k, ov] { ep->Set(k, ov); };
            a.redo = [ep, k, nv] { ep->Set(k, nv); };
            Push(std::move(a));
        }

        void Undo() { if (CanUndo()) { m_actions[--m_pos].undo(); } }
        void Redo() { if (CanRedo()) { m_actions[m_pos++].redo(); } }

    private:
        std::vector<Action> m_actions;
        size_t m_pos = 0;
    };

    // ------------------------------------------------------------------ asset filters
    struct FilterState
    {
        bool nameOn = false;   std::string name;
        bool typesOn = false;  std::string types;
        bool gdtsOn = false;   std::string gdts;
        bool valuesOn = false; std::string values;
        bool favoritesOnly = false;

        bool Active() const { return (nameOn && !name.empty()) || (typesOn && !types.empty()) || (gdtsOn && !gdts.empty()) || (valuesOn && !values.empty()) || favoritesOnly; }

        static std::vector<std::string> Split(const std::string& s)
        {
            std::vector<std::string> r;
            std::stringstream ss(s);
            for (std::string t; std::getline(ss, t, ',');) { t = Trim(t); if (!t.empty()) r.push_back(t); }
            return r;
        }

        bool MatchesGdt(const gdt::GdtFile& g) const
        {
            if (!gdtsOn || gdts.empty()) return true;
            for (auto& t : Split(gdts)) if (IContains(g.Name(), t)) return true;
            return false;
        }

        // `gdtOk` should come from MatchesGdt for the owning file (cached by the caller).
        bool Matches(const gdt::Entry& e, bool favorite) const
        {
            if (favoritesOnly && !favorite) return false;
            if (nameOn && !name.empty())
            {
                bool wild = name.find_first_of("*?") != std::string::npos;
                if (wild ? !WildcardMatch(e.Name(), name) : !IContains(e.Name(), name)) return false;
            }
            if (typesOn && !types.empty())
            {
                std::string type = e.GdfName();
                if (EndsWith(ToLower(type), ".gdf")) type.resize(type.size() - 4);
                bool ok = false;
                // whole type names (the picker stores exact ones, so "vehicle" must not also match "vehicleweapon"); * and ? work as wildcards
                auto same = [](const std::string& have, const std::string& want) {
                    return want.find_first_of("*?") != std::string::npos ? WildcardMatch(have, want) : ToLower(have) == ToLower(want);
                };
                for (auto& t : Split(types)) if (same(type, t) || (e.Def() && same(e.Def()->displayName, t))) { ok = true; break; }
                if (!ok) return false;
            }
            if (valuesOn && !values.empty())
            {
                bool ok = false;
                for (auto& kv : e.Values()) if (IContains(kv.second, values)) { ok = true; break; }
                if (!ok) return false;
            }
            return true;
        }

        std::string Serialize() const
        {
            const char sep = '\x1f';
            std::string s;
            s += std::to_string(nameOn) + sep + name + sep + std::to_string(typesOn) + sep + types + sep + std::to_string(gdtsOn) + sep + gdts + sep +
                 std::to_string(valuesOn) + sep + values + sep + std::to_string(favoritesOnly);
            return s;
        }

        static FilterState Deserialize(const std::string& s)
        {
            std::vector<std::string> f;
            std::stringstream ss(s);
            for (std::string t; std::getline(ss, t, '\x1f');) f.push_back(t);
            FilterState r;
            if (f.size() >= 9)
            {
                r.nameOn = f[0] == "1"; r.name = f[1]; r.typesOn = f[2] == "1"; r.types = f[3];
                r.gdtsOn = f[4] == "1"; r.gdts = f[5]; r.valuesOn = f[6] == "1"; r.values = f[7]; r.favoritesOnly = f[8] == "1";
            }
            return r;
        }
    };

    // ------------------------------------------------------------------ session
    class Session
    {
    public:
        Project project;
        IwdArchive iwds;           // the stock .iwd archives in <root>/main (images the raw folder lacks, sky boxes)
        SkeletonIndex skeletons;   // bone-name index of the compiled models (built in the background)
        game::GameData game;     // compiled data under <root>/raw (shaders, techsets, materials)
        UndoStack undo;
        FilterState filter;
        std::map<std::string, FilterState> filterSets;
        std::set<std::string> favorites;

        struct Tab { gdt::Entry* entry = nullptr; int uid = 0; };   // entry == nullptr is an empty tab
        std::vector<Tab> tabs;
        int activeTab = -1;
        int assetSort = 0;               // 0 file order, 1 name A-Z, 2 name Z-A, 3 type
        std::string ignoredUpdate;       // release the user chose to ignore: no dialog for it, the title and Help still show it
        bool checkUpdates = true;        // ask GitHub for a newer release at startup
        int accentTheme = 0;             // index into ui::AccentThemes(), 0 = Treyarch
        bool showPreviewInfo = true;     // the debug/info text in the corner of the preview
        std::vector<gdt::Entry*> backStack, forwardStack;
        gdt::Entry* lastOpened = nullptr;

        // Preview override: show this image file instead of the selected asset.
        fs::path previewImage;
        // Preview a compiled asset from <root>/raw that has no GDT entry.
        std::string previewRawModel, previewRawMaterial, previewRawAnim;

        // layout (persisted)
        float leftWidth = 0.30f, previewHeight = 0.34f, filtersHeight = 0.20f;

        std::string statusText;
        double statusUntil = 0;

        int m_tabCounter = 0;

        // ---- settings file ------------------------------------------------------------
        fs::path settingsPath;
        fs::path lastRoot;

        void LoadSettings(const fs::path& path)
        {
            settingsPath = path;
            std::string text;
            if (!ReadTextFile(path, text)) return;
            std::istringstream ss(text);
            for (std::string line; std::getline(ss, line);)
            {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                size_t eq = line.find('=');
                if (eq == std::string::npos) continue;
                std::string k = line.substr(0, eq), v = line.substr(eq + 1);
                if (k == "root") lastRoot = v;
                else if (k == "fav") favorites.insert(v);
                else if (k == "filterset") { size_t t = v.find('\t'); if (t != std::string::npos) filterSets[v.substr(0, t)] = FilterState::Deserialize(v.substr(t + 1)); }
                else if (k == "layout.left") leftWidth = std::stof(v);
                else if (k == "layout.preview") previewHeight = std::stof(v);
                else if (k == "layout.filters") filtersHeight = std::stof(v);
                else if (k == "assets.sort") assetSort = std::clamp(std::stoi(v), 0, 3);
                else if (k == "preview.info") showPreviewInfo = v != "0";
                else if (k == "theme") accentTheme = std::max(0, std::stoi(v));
                else if (k == "updates.check") checkUpdates = v != "0";
                else if (k == "updates.ignored") ignoredUpdate = v;
            }
        }

        void SaveSettings() const
        {
            if (settingsPath.empty()) return;
            std::string out;
            out += "root=" + project.Root().string() + "\n";
            for (auto& f : favorites) out += "fav=" + f + "\n";
            for (auto& [n, f] : filterSets) out += "filterset=" + n + "\t" + f.Serialize() + "\n";
            out += "layout.left=" + std::to_string(leftWidth) + "\n";
            out += "layout.preview=" + std::to_string(previewHeight) + "\n";
            out += "layout.filters=" + std::to_string(filtersHeight) + "\n";
            out += "assets.sort=" + std::to_string(assetSort) + "\n";
            out += std::string("preview.info=") + (showPreviewInfo ? "1" : "0") + "\n";
            out += "theme=" + std::to_string(accentTheme) + "\n";
            out += std::string("updates.check=") + (checkUpdates ? "1" : "0") + "\n";
            out += "updates.ignored=" + ignoredUpdate + "\n";
            WriteTextFile(settingsPath, out);
        }

        void Notify(std::string text, double seconds = 5.0)
        {
            statusText = std::move(text);
            statusUntil = NowSeconds() + seconds;
        }

        // ---- project -----------------------------------------------------------------
        bool OpenRoot(const fs::path& root)
        {
            tabs.clear(); activeTab = -1; backStack.clear(); forwardStack.clear(); lastOpened = nullptr; undo.Clear(); previewImage.clear();
            bool ok = project.Open(root);
            game.Open(root);
            if (ok) skeletons.Build(root / "raw");
            iwds.Open(root / "main");
            if (!ok) Notify(project.Errors().empty() ? "Could not open project" : project.Errors().front());
            return ok;
        }

        // ---- favourites --------------------------------------------------------------
        static std::string FavKey(const gdt::Entry& e) { return e.GdfName() + "/" + e.Name(); }
        bool IsFavorite(const gdt::Entry& e) const { return favorites.count(FavKey(e)) != 0; }
        void ToggleFavorite(const gdt::Entry& e)
        {
            auto k = FavKey(e);
            if (!favorites.erase(k)) favorites.insert(k);
        }

        // ---- navigation / tabs -------------------------------------------------------
        gdt::Entry* Selected() const
        {
            return activeTab >= 0 && activeTab < (int)tabs.size() ? tabs[activeTab].entry : nullptr;
        }

        void Open(gdt::Entry* e, bool recordHistory = true)
        {
            if (!e) return;
            if (recordHistory) RecordVisit(e); else lastOpened = e;
            // selecting an asset shows it in the current tab; only an explicit request opens another one
            if (activeTab < 0 || activeTab >= (int)tabs.size()) NewTab();
            tabs[activeTab].entry = e;
            previewImage.clear(); previewRawModel.clear(); previewRawMaterial.clear(); previewRawAnim.clear();
        }

        void OpenInNewTab(gdt::Entry* e)
        {
            if (!e) return;
            RecordVisit(e);
            NewTab();
            tabs[activeTab].entry = e;
        }

        // An empty tab, ready to receive the next asset that is selected.
        void NewTab()
        {
            tabs.push_back({ nullptr, ++m_tabCounter });
            activeTab = (int)tabs.size() - 1;
            previewImage.clear(); previewRawModel.clear(); previewRawMaterial.clear(); previewRawAnim.clear();
        }

        void SetActiveTab(int i)
        {
            if (i == activeTab || i < 0 || i >= (int)tabs.size()) return;
            activeTab = i;
            if (tabs[i].entry) RecordVisit(tabs[i].entry);
            previewImage.clear(); previewRawModel.clear(); previewRawMaterial.clear(); previewRawAnim.clear();
        }

        void CloseTab(int i)
        {
            if (i < 0 || i >= (int)tabs.size()) return;
            tabs.erase(tabs.begin() + i);
            if (activeTab >= (int)tabs.size()) activeTab = (int)tabs.size() - 1;
            else if (i < activeTab) --activeTab;
            previewImage.clear();
        }

        void ForgetEntry(const gdt::Entry* e)
        {
            for (auto& t : tabs) if (t.entry == e) t.entry = nullptr;
            if (lastOpened == e) lastOpened = nullptr;
            auto drop = [e](std::vector<gdt::Entry*>& v) { v.erase(std::remove(v.begin(), v.end(), e), v.end()); };
            drop(backStack); drop(forwardStack);
        }

        // Tabs and history by name, so they survive a reload that recreates every entry.
        struct NavSnapshot
        {
            struct Ref { std::string gdf, name; bool valid = false; };
            std::vector<Ref> tabs; std::vector<int> uids;
            int active = -1;
            std::vector<Ref> back, forward;
            Ref last;
        };
        static NavSnapshot::Ref RefOf(const gdt::Entry* e)
        {
            NavSnapshot::Ref r;
            if (e) { r.gdf = e->GdfName(); r.name = e->Name(); r.valid = true; }
            return r;
        }
        NavSnapshot SnapshotNav() const
        {
            NavSnapshot s;
            for (auto& t : tabs) { s.tabs.push_back(RefOf(t.entry)); s.uids.push_back(t.uid); }
            s.active = activeTab;
            for (auto* e : backStack) s.back.push_back(RefOf(e));
            for (auto* e : forwardStack) s.forward.push_back(RefOf(e));
            s.last = RefOf(lastOpened);
            return s;
        }
        void RestoreNav(const NavSnapshot& s)
        {
            auto find = [&](const NavSnapshot::Ref& r) -> gdt::Entry* { return r.valid ? project.FindEntry(r.gdf, r.name) : nullptr; };
            tabs.clear();
            for (size_t i = 0; i < s.tabs.size(); ++i) tabs.push_back({ find(s.tabs[i]), s.uids[i] });
            activeTab = tabs.empty() ? -1 : std::clamp(s.active, 0, (int)tabs.size() - 1);
            backStack.clear(); forwardStack.clear();
            for (auto& r : s.back) if (auto* e = find(r)) backStack.push_back(e);
            for (auto& r : s.forward) if (auto* e = find(r)) forwardStack.push_back(e);
            lastOpened = find(s.last);
        }

        // The history follows what the user looked at, whichever tab it was shown in.
        void RecordVisit(gdt::Entry* e)
        {
            if (!e || e == lastOpened) return;
            if (lastOpened) backStack.push_back(lastOpened);
            forwardStack.clear();
            lastOpened = e;
        }

        bool CanGoBack() const { return !backStack.empty(); }
        bool CanGoForward() const { return !forwardStack.empty(); }
        void GoBack()
        {
            if (backStack.empty()) return;
            if (lastOpened) forwardStack.push_back(lastOpened);
            auto* e = backStack.back(); backStack.pop_back();
            Open(e, false);
        }
        void GoForward()
        {
            if (forwardStack.empty()) return;
            if (lastOpened) backStack.push_back(lastOpened);
            auto* e = forwardStack.back(); forwardStack.pop_back();
            Open(e, false);
        }

        // ---- edits (all undoable) ----------------------------------------------------
        void SetValue(gdt::Entry& e, const std::string& key, const std::string& value)
        {
            std::string old = e.GetString(key);
            if (e.Set(key, value)) undo.PushValueEdit(e, key, old, value);
        }

        bool NameExists(const std::string& gdfName, const std::string& name) const
        {
            return project.FindEntry(gdfName, name) != nullptr;
        }

        std::string UniqueName(const std::string& base, const std::string& gdfName) const
        {
            if (!NameExists(gdfName, base)) return base;
            for (int i = 2;; ++i)
            {
                std::string n = base + "_" + std::to_string(i);
                if (!NameExists(gdfName, n)) return n;
            }
        }

        gdt::Entry* AddEntry(gdt::GdtFile& file, const std::string& name, const std::string& gdfName)
        {
            auto e = std::make_unique<gdt::Entry>(name, gdfName, project.FindDef(gdfName), &file);
            e->FillDefaults();
            return InsertEntry(file, std::move(e), "New entry " + name);
        }

        gdt::Entry* DuplicateEntry(const gdt::Entry& src, const std::string& newName)
        {
            auto* file = src.Owner();
            return InsertEntry(*file, src.Clone(newName, file), "Copy entry " + newName);
        }

        void DeleteEntry(gdt::Entry& e)
        {
            gdt::GdtFile* file = e.Owner();
            size_t index = 0;
            for (size_t i = 0; i < file->Entries().size(); ++i) if (file->Entries()[i].get() == &e) index = i;
            ForgetEntry(&e);
            auto holder = std::make_shared<std::unique_ptr<gdt::Entry>>(file->Remove(&e));
            gdt::Entry* raw = holder->get();
            project.Touch();
            UndoStack::Action a;
            a.label = "Delete entry " + raw->Name();
            a.undo = [this, file, holder, index] { file->Add(std::move(*holder), index); project.Touch(); };
            a.redo = [this, file, holder, raw] { ForgetEntry(raw); *holder = file->Remove(raw); project.Touch(); };
            undo.Push(std::move(a));
        }

        void RenameEntry(gdt::Entry& e, const std::string& newName)
        {
            std::string old = e.Name();
            if (old == newName) return;
            gdt::Entry* ep = &e;
            e.Rename(newName);
            project.Touch();
            UndoStack::Action a;
            a.label = "Rename " + old;
            a.undo = [this, ep, old] { ep->Rename(old); project.Touch(); };
            a.redo = [this, ep, newName] { ep->Rename(newName); project.Touch(); };
            undo.Push(std::move(a));
        }

    private:
        gdt::Entry* InsertEntry(gdt::GdtFile& file, std::unique_ptr<gdt::Entry> e, std::string label)
        {
            gdt::Entry* raw = file.Add(std::move(e));
            project.Touch();
            auto holder = std::make_shared<std::unique_ptr<gdt::Entry>>();
            gdt::GdtFile* fp = &file;
            UndoStack::Action a;
            a.label = std::move(label);
            a.undo = [this, fp, holder, raw] { ForgetEntry(raw); *holder = fp->Remove(raw); project.Touch(); };
            a.redo = [this, fp, holder] { fp->Add(std::move(*holder)); project.Touch(); };
            undo.Push(std::move(a));
            return raw;
        }
    };
}
