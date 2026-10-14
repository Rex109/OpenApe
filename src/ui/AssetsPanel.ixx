module;

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <windows.h>
#include "imgui.h"
#include "imgui_internal.h"
#include "misc/cpp/imgui_stdlib.h"

export module openape.ui.assets;

import openape.util;
import openape.gdf;
import openape.gdt;
import openape.project;
import openape.session;
import openape.platform;
import openape.ui.theme;

export namespace oa::ui
{
    inline void AssetTypeIcon(const std::string& gdfName, Icon& icon, ImU32& c1, ImU32& c2)
    {
        std::string n = ToLower(gdfName);
        auto has = [&](const char* s) { return n.find(s) != std::string::npos; };
        c2 = IM_COL32(255, 255, 255, 200);
        if (has("material")) { icon = Icon::Sphere; c1 = IM_COL32(70, 110, 230, 255); c2 = IM_COL32(190, 215, 255, 255); }
        else if (has("xmodel") && !has("alias")) { icon = Icon::Cube; c1 = IM_COL32(120, 190, 200, 255); c2 = IM_COL32(190, 235, 240, 255); }
        else if (has("xanim")) { icon = Icon::Play; c1 = IM_COL32(120, 210, 110, 255); }
        else if (has("weapon")) { icon = Icon::Cylinder; c1 = IM_COL32(215, 90, 80, 255); c2 = IM_COL32(245, 160, 150, 255); }
        else if (has("physpreset")) { icon = Icon::Torus; c1 = IM_COL32(235, 160, 60, 255); }
        else if (has("vehicle")) { icon = Icon::Cone; c1 = IM_COL32(230, 210, 80, 255); c2 = IM_COL32(250, 240, 160, 255); }
        else if (has("level")) { icon = Icon::Plane; c1 = IM_COL32(150, 130, 220, 255); }
        else { icon = Icon::Document; c1 = IM_COL32(200, 200, 200, 255); }
    }

    // ------------------------------------------------------------------ filters
    class FiltersPanel
    {
    public:
        explicit FiltersPanel(Session& s) : m_s(s) {}

        // returns true when the filter changed this frame
        bool Draw()
        {
            bool changed = false;
            FilterState& f = m_s.filter;
            const float labelW = Theme::S(62);
            const float avail = ImGui::GetContentRegionAvail().x;
            const float btnW = Theme::S(26);

            auto row = [&](const char* label, bool& on, std::string& text, const char* id, bool dropdown, const std::function<void()>& popup) {
                ImGui::PushID(id);
                if (ImGui::Checkbox("##on", &on)) changed = true;
                ImGui::SameLine(0, Theme::S(4));
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(label);
                ImGui::SameLine(Theme::S(26) + labelW);
                float w = avail - (Theme::S(26) + labelW) - (dropdown ? btnW + Theme::S(4) : 0);
                ImGui::SetNextItemWidth(w);
                if (ImGui::InputText("##t", &text)) { on = !text.empty(); changed = true; }
                if (dropdown)
                {
                    ImGui::SameLine(0, Theme::S(4));
                    if (ImGui::ArrowButton("##dd", ImGuiDir_Down)) ImGui::OpenPopup("##pop");
                    if (ImGui::BeginPopup("##pop")) { popup(); ImGui::EndPopup(); }
                }
                ImGui::PopID();
            };

            row("Name:", f.nameOn, f.name, "name", false, {});
            row("Types:", f.typesOn, f.types, "types", true, [&] {
                std::set<std::string> cur;
                for (auto& t : FilterState::Split(f.types)) cur.insert(ToLower(t));
                for (auto& d : m_s.project.Defs())
                {
                    std::string t = d->fileName; if (EndsWith(ToLower(t), ".gdf")) t.resize(t.size() - 4);
                    bool on = cur.count(ToLower(t)) != 0;
                    if (ImGui::Checkbox(t.c_str(), &on))
                    {
                        if (on) cur.insert(ToLower(t)); else cur.erase(ToLower(t));
                        std::string out;
                        for (auto& c : cur) out += (out.empty() ? "" : ", ") + c;
                        f.types = out; f.typesOn = !out.empty(); changed = true;
                    }
                }
            });
            row("GDTs:", f.gdtsOn, f.gdts, "gdts", true, [&] {
                std::set<std::string> cur;
                for (auto& t : FilterState::Split(f.gdts)) cur.insert(ToLower(t));
                for (auto& g : m_s.project.Gdts())
                {
                    std::string t = g->Name();
                    bool on = cur.count(ToLower(t)) != 0;
                    if (ImGui::Checkbox(t.c_str(), &on))
                    {
                        if (on) cur.insert(ToLower(t)); else cur.erase(ToLower(t));
                        std::string out;
                        for (auto& c : cur) out += (out.empty() ? "" : ", ") + c;
                        f.gdts = out; f.gdtsOn = !out.empty(); changed = true;
                    }
                }
            });
            row("Values:", f.valuesOn, f.values, "values", true, [&] {
                ImGui::TextDisabled("Matches any property value");
                for (auto& h : m_history)
                    if (ImGui::Selectable(h.c_str())) { f.values = h; f.valuesOn = true; changed = true; }
            });
            if (changed && f.valuesOn && !f.values.empty() && (m_history.empty() || m_history.front() != f.values) && !ImGui::IsAnyItemActive())
            {
                m_history.insert(m_history.begin(), f.values);
                if (m_history.size() > 12) m_history.pop_back();
            }

            // filter sets
            ImGui::AlignTextToFramePadding();
            ImGui::Dummy(ImVec2(0, 0));
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + Theme::S(26));
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Filter sets:");
            ImGui::SameLine(Theme::S(26) + labelW + Theme::S(14));
            ImGui::SetNextItemWidth(avail - (Theme::S(26) + labelW + Theme::S(14)) - btnW - Theme::S(10));
            if (ImGui::BeginCombo("##sets", m_activeSet.empty() ? "<none>" : m_activeSet.c_str()))
            {
                if (ImGui::Selectable("<none>", m_activeSet.empty())) { m_activeSet.clear(); f = FilterState{}; changed = true; }
                for (auto& [n, st] : m_s.filterSets)
                    if (ImGui::Selectable(n.c_str(), n == m_activeSet)) { m_activeSet = n; f = st; changed = true; }
                ImGui::EndCombo();
            }
            ImGui::SameLine(0, Theme::S(4));
            if (IconButton("funnel", Icon::Funnel, Theme::S(16), "Save / delete filter sets")) ImGui::OpenPopup("##setmenu");
            if (ImGui::BeginPopup("##setmenu"))
            {
                ImGui::SetNextItemWidth(Theme::S(160));
                ImGui::InputTextWithHint("##nm", "Set name", &m_newSet);
                if (ImGui::MenuItem("Save current filter as set", nullptr, false, !m_newSet.empty())) { m_s.filterSets[m_newSet] = f; m_activeSet = m_newSet; m_newSet.clear(); }
                if (ImGui::MenuItem("Delete selected set", nullptr, false, !m_activeSet.empty())) { m_s.filterSets.erase(m_activeSet); m_activeSet.clear(); }
                ImGui::Separator();
                if (ImGui::MenuItem("Clear all filters")) { f = FilterState{}; m_activeSet.clear(); changed = true; }
                ImGui::EndPopup();
            }
            return changed;
        }

    private:
        Session& m_s;
        std::string m_activeSet, m_newSet;
        std::vector<std::string> m_history;
    };

    // ------------------------------------------------------------------ asset tree
    class AssetsPanel
    {
    public:
        struct Callbacks
        {
            std::function<void(gdt::GdtFile*)> newEntry;
            std::function<void(gdt::Entry*)> duplicate, rename, remove;
            std::function<void()> reloadAll, exportCsv;
        };

        AssetsPanel(Session& s, HWND hwnd) : m_s(s), m_hwnd(hwnd) {}
        Callbacks cb;

        size_t VisibleCount() const { return m_visibleEntries; }
        size_t SelectedPosition() const { return m_selectedPos; }

        void Draw()
        {
            DrawToolbar();
            Rebuild();
            ImGui::BeginChild("##tree", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 wp = ImGui::GetWindowPos();
            ImVec2 ws = ImGui::GetWindowSize();
            dl->AddRectFilled(wp, ImVec2(wp.x + ws.x, wp.y + ws.y), Palette::ListBg);

            const float rowH = Theme::S(18);
            gdt::Entry* sel = m_s.Selected();
            HandleKeyboard(sel);

            if (sel != m_lastSelected)
            {
                m_lastSelected = sel;
                m_scrollPending = sel != nullptr;
                if (sel && m_expanded.insert(sel->Owner()).second) m_dirtyRows = true;
            }
            if (m_scrollPending)
            {
                for (size_t i = 0; i < m_rows.size(); ++i)
                    if (m_rows[i].e == sel)
                    {
                        float y = i * rowH;
                        if (y < ImGui::GetScrollY() || y + rowH > ImGui::GetScrollY() + ws.y) ImGui::SetScrollY(std::max(0.f, y - ws.y * 0.4f));
                        break;
                    }
                m_scrollPending = false;
            }

            ImGuiListClipper clip;
            clip.Begin((int)m_rows.size(), rowH);
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(1, 1, 1, 0.10f));
            ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0, 0, 0, 0));
            while (clip.Step())
                for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) DrawRow(i, rowH, ws.x);
            ImGui::PopStyleColor(3);
            ImGui::PopStyleVar();
            if (m_rows.empty())
            {
                ImGui::SetCursorPos(ImVec2(Theme::S(10), Theme::S(10)));
                ImGui::PushStyleColor(ImGuiCol_Text, Palette::ListText);
                ImGui::TextUnformatted(m_s.project.IsOpen() ? "No assets match the filters." : "No project open.");
                ImGui::PopStyleColor();
            }
            ImGui::EndChild();
        }

        void ForceRebuild() { m_rev = 0; }

    private:
        struct Row { gdt::GdtFile* g; gdt::Entry* e; int count; };

        void DrawToolbar()
        {
            float ic = Theme::S(16);
            bool fav = m_s.filter.favoritesOnly;
            if (IconButton("fav", fav ? Icon::StarFilled : Icon::Star, ic, "Show favourites only", fav)) { m_s.filter.favoritesOnly = !fav; m_rev = 0; }
            ImGui::SameLine(0, 2);
            if (IconButton("new", Icon::New, ic, "New asset")) { if (cb.newEntry) cb.newEntry(SelectedGdt()); }
            ImGui::SameLine(0, 2);
            if (IconButton("dup", Icon::Copy, ic, "Duplicate selected asset")) { if (auto* e = m_s.Selected(); e && cb.duplicate) cb.duplicate(e); }
            ImGui::SameLine(0, 2);
            if (IconButton("sort", Icon::Sync, ic, "Sort the asset list", m_s.assetSort != 0)) ImGui::OpenPopup("##sortmenu");
            if (ImGui::BeginPopup("##sortmenu"))
            {
                ImGui::TextDisabled("Sort assets");
                const char* names[] = { "File order", "Name (A - Z)", "Name (Z - A)", "Type, then name" };
                for (int i = 0; i < 4; ++i)
                    if (ImGui::MenuItem(names[i], nullptr, m_s.assetSort == i)) { m_s.assetSort = i; m_rev = 0; }
                ImGui::EndPopup();
            }
            ImGui::SameLine(0, 2);
            if (IconButton("csv", Icon::Table, ic, "Export visible assets to CSV")) { if (cb.exportCsv) cb.exportCsv(); }
        }

        gdt::GdtFile* SelectedGdt() const
        {
            if (auto* e = m_s.Selected()) return e->Owner();
            return m_s.project.Gdts().empty() ? nullptr : m_s.project.Gdts().front().get();
        }

        void Rebuild()
        {
            std::string sig = m_s.filter.Serialize();
            if (m_rev == m_s.project.Revision() && sig == m_filterSig && m_favCount == m_s.favorites.size() && !m_dirtyRows && m_sortApplied == m_s.assetSort) return;
            m_rev = m_s.project.Revision(); m_filterSig = sig; m_favCount = m_s.favorites.size(); m_dirtyRows = false; m_sortApplied = m_s.assetSort;
            m_rows.clear();
            m_visibleEntries = 0;
            const bool filtering = m_s.filter.Active();
            std::vector<gdt::GdtFile*> files;
            for (auto& g : m_s.project.Gdts()) files.push_back(g.get());
            if (m_s.assetSort != 0)
                std::stable_sort(files.begin(), files.end(), [&](gdt::GdtFile* a, gdt::GdtFile* b) {
                    return m_s.assetSort == 2 ? ToLower(a->Name()) > ToLower(b->Name()) : ToLower(a->Name()) < ToLower(b->Name());
                });
            for (gdt::GdtFile* gp : files)
            {
                auto* g = gp;
                if (!m_s.filter.MatchesGdt(*g)) continue;
                std::vector<gdt::Entry*> matches;
                for (auto& e : g->Entries())
                    if (!filtering || m_s.filter.Matches(*e, m_s.IsFavorite(*e))) matches.push_back(e.get());
                if (m_s.assetSort != 0)
                    std::stable_sort(matches.begin(), matches.end(), [&](gdt::Entry* a, gdt::Entry* b) {
                        std::string na = ToLower(a->Name()), nb = ToLower(b->Name());
                        switch (m_s.assetSort)
                        {
                        case 2: return na > nb;
                        case 3: { std::string ta = ToLower(a->GdfName()), tb = ToLower(b->GdfName()); return ta != tb ? ta < tb : na < nb; }
                        default: return na < nb;
                        }
                    });
                if (filtering && matches.empty()) continue;
                m_rows.push_back({ g, nullptr, (int)matches.size() });
                m_visibleEntries += matches.size();
                bool open = filtering || m_expanded.count(g);
                if (open) for (auto* e : matches) m_rows.push_back({ g, e, 0 });
            }
        }

        void HandleKeyboard(gdt::Entry* sel)
        {
            if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) || ImGui::GetIO().WantTextInput) return;
            int dir = ImGui::IsKeyPressed(ImGuiKey_DownArrow) ? 1 : ImGui::IsKeyPressed(ImGuiKey_UpArrow) ? -1 : 0;
            if (!dir) return;
            int cur = -1;
            for (size_t i = 0; i < m_rows.size(); ++i) if (m_rows[i].e == sel) cur = (int)i;
            for (int i = cur + dir; i >= 0 && i < (int)m_rows.size(); i += dir)
                if (m_rows[i].e) { m_s.Open(m_rows[i].e); break; }
        }

        void DrawRow(int i, float rowH, float width)
        {
            Row& r = m_rows[i];
            ImGui::PushID(i);
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            bool header = r.e == nullptr;
            gdt::Entry* sel = m_s.Selected();
            bool selected = !header && r.e == sel;

            if (selected) dl->AddRectFilled(p, ImVec2(p.x + 10000, p.y + rowH), Palette::Accent);
            else if (i & 1) dl->AddRectFilled(p, ImVec2(p.x + 10000, p.y + rowH), Palette::ListAlt);

            bool clicked = ImGui::Selectable("##row", selected, ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_SpanAllColumns, ImVec2(0, rowH));
            bool hovered = ImGui::IsItemHovered();
            float x = p.x + Theme::S(4);
            ImU32 textCol = selected ? IM_COL32(255, 255, 255, 255) : Palette::ListText;
            float ty = p.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f;

            if (header)
            {
                bool open = m_expanded.count(r.g) != 0 || m_s.filter.Active();
                float b = Theme::S(9);
                ImVec2 bp(x, p.y + (rowH - b) * 0.5f);
                dl->AddRect(bp, ImVec2(bp.x + b, bp.y + b), IM_COL32(40, 40, 40, 255));
                dl->AddLine(ImVec2(bp.x + 2, bp.y + b * 0.5f), ImVec2(bp.x + b - 2, bp.y + b * 0.5f), IM_COL32(20, 20, 20, 255));
                if (!open) dl->AddLine(ImVec2(bp.x + b * 0.5f, bp.y + 2), ImVec2(bp.x + b * 0.5f, bp.y + b - 2), IM_COL32(20, 20, 20, 255));
                DrawIcon(dl, Icon::Grid, ImVec2(x + Theme::S(14), p.y + (rowH - Theme::S(14)) * 0.5f), Theme::S(14), IM_COL32(230, 230, 255, 255));
                std::string label = r.g->Name() + (r.g->Dirty() ? " *" : "");
                if (m_s.filter.Active()) label += "  (" + std::to_string(r.count) + ")";
                dl->AddText(ImVec2(x + Theme::S(32), ty), textCol, label.c_str());
                if (clicked && !m_s.filter.Active())
                {
                    if (m_expanded.count(r.g)) m_expanded.erase(r.g); else m_expanded.insert(r.g);
                    m_dirtyRows = true;
                }
                if (ImGui::BeginPopupContextItem("##gctx"))
                {
                    if (ImGui::MenuItem("New asset here...")) { if (cb.newEntry) cb.newEntry(r.g); }
                    if (ImGui::MenuItem("Save", nullptr, false, r.g->Dirty())) { auto e = r.g->Save(); m_s.Notify(e.empty() ? "Saved " + r.g->Name() : e); }
                    if (ImGui::MenuItem("Reveal in Explorer")) platform::RevealInExplorer(r.g->Path());
                    ImGui::EndPopup();
                }
            }
            else
            {
                Icon icon; ImU32 c1, c2;
                AssetTypeIcon(r.e->GdfName(), icon, c1, c2);
                float ix = x + Theme::S(26);
                DrawIcon(dl, icon, ImVec2(ix, p.y + (rowH - Theme::S(14)) * 0.5f), Theme::S(14), c1, c2);
                dl->AddText(ImVec2(ix + Theme::S(20), ty), textCol, r.e->Name().c_str());
                if (m_s.IsFavorite(*r.e))
                    DrawIcon(dl, Icon::StarFilled, ImVec2(x + Theme::S(7), p.y + (rowH - Theme::S(12)) * 0.5f), Theme::S(12), 0);   // in the empty slot left of the type icon
                if (clicked)
                {
                    if (ImGui::GetIO().KeyCtrl) m_s.OpenInNewTab(r.e); else m_s.Open(r.e);
                    m_selectedPos = 0;
                }
                if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) m_s.OpenInNewTab(r.e);
                if (hovered)
                {
                    ImGui::BeginTooltip();
                    ImGui::TextUnformatted(r.e->Name().c_str());
                    ImGui::TextDisabled("%s  -  %s.gdt", r.e->Def() ? r.e->Def()->displayName.c_str() : r.e->GdfName().c_str(), r.g->Name().c_str());
                    ImGui::EndTooltip();
                }
                if (ImGui::BeginPopupContextItem("##ectx"))
                {
                    if (!selected) m_s.Open(r.e);
                    if (ImGui::MenuItem("Open in new tab", "Ctrl+click")) m_s.OpenInNewTab(r.e);
                    ImGui::Separator();
                    if (ImGui::MenuItem("Duplicate...")) { if (cb.duplicate) cb.duplicate(r.e); }
                    if (ImGui::MenuItem("Rename...")) { if (cb.rename) cb.rename(r.e); }
                    if (ImGui::MenuItem("Delete...")) { if (cb.remove) cb.remove(r.e); }
                    ImGui::Separator();
                    if (ImGui::MenuItem(m_s.IsFavorite(*r.e) ? "Remove from favourites" : "Add to favourites")) m_s.ToggleFavorite(*r.e);
                    if (ImGui::MenuItem("Copy name")) platform::SetClipboardText(m_hwnd, r.e->Name());
                    if (ImGui::MenuItem("Reveal GDT in Explorer")) platform::RevealInExplorer(r.g->Path());
                    ImGui::EndPopup();
                }
            }
            ImGui::PopID();
        }

        void ComputePosition()
        {
            m_selectedPos = 0;
            size_t n = 0;
            for (auto& r : m_rows) if (r.e) { ++n; if (r.e == m_s.Selected()) { m_selectedPos = n; break; } }
        }

    public:
        // 1-based index of the selected asset among visible assets (0 if none) for the status bar.
        size_t UpdateSelectedPosition() { ComputePosition(); return m_selectedPos; }

    private:
        Session& m_s;
        HWND m_hwnd;
        std::vector<Row> m_rows;
        std::set<gdt::GdtFile*> m_expanded;
        unsigned m_rev = 0;
        std::string m_filterSig;
        size_t m_favCount = 0, m_visibleEntries = 0, m_selectedPos = 0;
        bool m_dirtyRows = false, m_scrollPending = false;
        int m_sortApplied = 0;
        gdt::Entry* m_lastSelected = nullptr;
    };
}
