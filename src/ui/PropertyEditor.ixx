module;

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>
#include <windows.h>
#include "imgui.h"
#include "imgui_internal.h"
#include "misc/cpp/imgui_stdlib.h"

export module openape.ui.properties;

import openape.util;
import openape.gdf;
import openape.gdt;
import openape.project;
import openape.session;
import openape.renderer;
import openape.image;
import openape.platform;
import openape.ui.theme;

// Renders the component tree of an asset's .gdf as ImGui widgets.
export namespace oa::ui
{
    class PropertyEditor
    {
    public:
        PropertyEditor(Renderer& r, Session& s, HWND hwnd) : m_renderer(r), m_session(s), m_hwnd(hwnd) {}

        void SetFilter(const std::string& f) { m_filter = f; }

        void Draw(gdt::Entry& entry)
        {
            m_entry = &entry;
            const gdf::Def* def = entry.Def();
            if (!def || !def->root)
            {
                ImGui::TextDisabled("No definition (%s) is available for this asset.", entry.GdfName().c_str());
                return;
            }
            m_def = def;
            float width = ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ScrollbarSize;
            DrawComponent(*def->root, width);
            ImGui::Dummy(ImVec2(0, Theme::S(30)));
        }

    private:
        // ------------------------------------------------------------------ helpers
        static float Spacing() { return ImGui::GetStyle().ItemSpacing.x; }

        const gdf::Variable* Var(const gdf::Component& c) const
        {
            std::string n = c.BoundVariable(*m_entry);
            return n.empty() ? nullptr : m_def->Find(n);
        }

        std::string LabelFor(const gdf::Component& c, const gdf::Variable& v) const
        {
            return c.hasLabel ? c.label : PrettifyName(v.name);
        }

        void Commit(const gdf::Variable& v, const std::string& value) { m_session.SetValue(*m_entry, v.name, value); }

        static std::string StripStar(const std::string& s) { return (!s.empty() && s.back() == '*') ? s.substr(0, s.size() - 1) : s; }

        bool Visible(const gdf::Component& c) const
        {
            if (c.visible && !c.visible->Eval(*m_entry).Truthy()) return false;
            return m_filter.empty() || Matches(c);
        }

        bool Matches(const gdf::Component& c) const
        {
            if (c.IsContainer())
            {
                for (auto& ch : c.children)
                {
                    if (ch->visible && !ch->visible->Eval(*m_entry).Truthy()) continue;
                    if (Matches(*ch)) return true;
                }
                return false;
            }
            const gdf::Variable* v = Var(c);
            if (!v) return false;
            if (IContains(LabelFor(c, *v), m_filter) || IContains(v->name, m_filter)) return true;
            return IContains(StripStar(m_entry->GetString(v->name)), m_filter);
        }

        float Weight(const gdf::Component& c) const
        {
            if (c.width > 0) return (float)c.width;
            if (c.IsContainer())
            {
                float sum = 0, mx = 0;
                for (auto& ch : c.children) { float w = Weight(*ch); sum += w; mx = std::max(mx, w); }
                if (c.children.empty()) return 1;
                return c.kind == gdf::CompKind::HContainer ? sum : mx;
            }
            return 200.0f;
        }

        // ------------------------------------------------------------------ containers
        void DrawComponent(const gdf::Component& c, float width)
        {
            if (!Visible(c)) return;
            ImGui::PushID(&c);
            if (c.IsContainer()) DrawContainer(c, width);
            else DrawLeaf(c, width);
            ImGui::PopID();
        }

        bool Section(const gdf::Component& c, float width)
        {
            auto [it, inserted] = m_open.try_emplace(&c, c.groupExpanded);
            bool& open = it->second;
            bool forced = !m_filter.empty();   // searching expands everything that matches
            ImVec2 p = ImGui::GetCursorScreenPos();
            float h = Theme::S(26);
            if (ImGui::InvisibleButton("##sec", ImVec2(std::max(width, 10.f), h)) && !forced) open = !open;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            bool hot = ImGui::IsItemHovered();
            float box = Theme::S(11);
            ImVec2 bp(p.x + Theme::S(4), p.y + (h - box) * 0.5f + Theme::S(2));
            dl->AddRect(bp, ImVec2(bp.x + box, bp.y + box), hot ? Palette::Accent : IM_COL32(150, 150, 150, 255));
            dl->AddLine(ImVec2(bp.x + 2, bp.y + box * 0.5f), ImVec2(bp.x + box - 2, bp.y + box * 0.5f), Palette::Text);
            if (!(open || forced)) dl->AddLine(ImVec2(bp.x + box * 0.5f, bp.y + 2), ImVec2(bp.x + box * 0.5f, bp.y + box - 2), Palette::Text);
            ImGui::PushFont(Theme::bold, 0.0f);
            dl->AddText(ImVec2(p.x + Theme::S(24), p.y + Theme::S(4)), Palette::Text, c.groupTitle.c_str());
            ImGui::PopFont();
            return open || forced;
        }

        void DrawContainer(const gdf::Component& c, float width)
        {
            if (c.children.empty()) return;
            float innerW = width;
            bool indented = false;
            if (c.hasGroup)
            {
                if (!Section(c, width)) return;
                float ind = Theme::S(10);
                ImGui::Indent(ind); innerW -= ind; indented = true;
            }

            if (c.kind == gdf::CompKind::VContainer)
            {
                for (auto& ch : c.children) DrawComponent(*ch, innerW);
            }
            else
            {
                std::vector<const gdf::Component*> vis;
                for (auto& ch : c.children) if (Visible(*ch) && !(ch->IsContainer() && ch->children.empty())) vis.push_back(ch.get());
                float total = 0;
                for (auto* v : vis) total += Weight(*v);
                float avail = innerW - Spacing() * (float)(vis.size() > 0 ? vis.size() - 1 : 0);
                for (size_t i = 0; i < vis.size(); ++i)
                {
                    if (i) ImGui::SameLine();
                    float w = std::max(avail * Weight(*vis[i]) / std::max(total, 1.0f), Theme::S(60));
                    ImGui::BeginGroup();
                    DrawComponent(*vis[i], w);
                    ImGui::EndGroup();
                }
            }
            if (indented) ImGui::Unindent(Theme::S(10));
        }

        // ------------------------------------------------------------------ leaves
        struct Row { float labelW; float ctrlW; };

        // Draws the label column and positions the cursor for the control.
        Row BeginRow(const gdf::Component& c, const gdf::Variable& v, float width, bool wantLabel = true)
        {
            float labelW = c.labelWidth >= 0 ? Theme::S((float)c.labelWidth) : Theme::S(170);
            if (!wantLabel) labelW = 0;
            labelW = std::min(labelW, width * 0.5f);
            float x0 = ImGui::GetCursorPosX();
            if (labelW > 0)
            {
                ImGui::AlignTextToFramePadding();
                std::string label = LabelFor(c, v);
                ImVec2 p = ImGui::GetCursorScreenPos();
                ImGui::PushClipRect(p, ImVec2(p.x + labelW - Theme::S(6), p.y + ImGui::GetFrameHeight()), true);
                ImGui::TextUnformatted(label.c_str());
                ImGui::PopClipRect();
                LabelInteraction(c, v, p, labelW);
                ImGui::SameLine();
                ImGui::SetCursorPosX(x0 + labelW);
            }
            return { labelW, width - labelW };
        }

        void LabelInteraction(const gdf::Component& c, const gdf::Variable& v, ImVec2 p, float labelW)
        {
            ImRect r(p, ImVec2(p.x + labelW, p.y + ImGui::GetFrameHeight()));
            bool hot = ImGui::IsMouseHoveringRect(r.Min, r.Max) && ImGui::IsWindowHovered();
            if (hot)
            {
                ImGui::BeginTooltip();
                if (!c.tooltip.empty()) { ImGui::PushTextWrapPos(Theme::S(380)); ImGui::TextUnformatted(c.tooltip.c_str()); ImGui::PopTextWrapPos(); }
                ImGui::TextDisabled("%s", v.name.c_str());
                ImGui::EndTooltip();
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##rowctx");
            }
            if (ImGui::BeginPopup("##rowctx"))
            {
                if (ImGui::MenuItem("Reset to default")) Commit(v, v.DefaultString());
                if (ImGui::MenuItem("Copy key name")) platform::SetClipboardText(m_hwnd, v.name);
                ImGui::EndPopup();
            }
        }

        void DrawLeaf(const gdf::Component& c, float width)
        {
            using K = gdf::CompKind;
            const gdf::Variable* v = Var(c);
            if (!v) return;
            switch (c.kind)
            {
            case K::ScrollBox: DrawCombo(c, *v, width); break;
            case K::SimpleListBox: DrawListBox(c, *v, width); break;
            case K::Edit: DrawEdit(c, *v, width, false); break;
            case K::MultiEdit: DrawEdit(c, *v, width, true); break;
            case K::SpinEdit: DrawNumber(c, *v, width, true); break;
            case K::FloatEdit: DrawNumber(c, *v, width, false); break;
            case K::FileEdit:
            case K::MultiFileEdit: DrawFile(c, *v, width); break;
            case K::CheckBox: DrawCheck(c, *v, width); break;
            case K::ColorPicker: DrawColor(c, *v, width); break;
            default: break;
            }
        }

        // ---- enum / keyenum -------------------------------------------------------------
        void DrawCombo(const gdf::Component& c, const gdf::Variable& v, float width)
        {
            Row r = BeginRow(c, v, width);
            ImGui::SetNextItemWidth(r.ctrlW);
            std::string cur = m_entry->GetString(v.name);
            if (v.type == gdf::VarType::KeyEnum)
            {
                const auto& names = m_session.project.EntryNames(v.keyGdfs);
                if (ImGui::BeginCombo("##c", cur.empty() ? "<none>" : cur.c_str()))
                {
                    if (ImGui::Selectable("<none>", cur.empty())) Commit(v, "");
                    for (auto& n : names)
                        if (ImGui::Selectable(n.c_str(), n == cur)) Commit(v, n);
                    ImGui::EndCombo();
                }
                return;
            }
            int idx = v.FindItem(cur);
            if (idx < 0) idx = v.defItem;
            std::string shown = idx >= 0 && idx < (int)v.items.size() ? StripStar(v.items[idx].Label()) : cur;
            if (ImGui::BeginCombo("##c", shown.c_str()))
            {
                std::vector<int> order(v.items.size());
                for (size_t i = 0; i < order.size(); ++i) order[i] = (int)i;
                if (!c.unsorted)
                    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return ToLower(v.items[a].Label()) < ToLower(v.items[b].Label()); });
                for (int i : order)
                {
                    const std::string& lab = v.items[i].Label();
                    if (lab.rfind("---", 0) == 0) { ImGui::Separator(); continue; }
                    if (ImGui::Selectable(StripStar(lab).c_str(), i == idx)) Commit(v, lab);
                    if (i == idx) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        }

        void DrawListBox(const gdf::Component& c, const gdf::Variable& v, float width)
        {
            Row r = BeginRow(c, v, width);
            float h = c.height > 0 ? Theme::S((float)c.height) : ImGui::GetTextLineHeightWithSpacing() * 6;
            std::string cur = m_entry->GetString(v.name);
            std::vector<std::string> items;
            if (v.type == gdf::VarType::KeyEnum) { items = m_session.project.EntryNames(v.keyGdfs); items.insert(items.begin(), ""); }
            else for (auto& it : v.items) items.push_back(it.Label());
            if (ImGui::BeginListBox("##lb", ImVec2(r.ctrlW, h)))
            {
                for (auto& s : items)
                {
                    bool sel = v.type == gdf::VarType::KeyEnum ? s == cur : (v.FindItem(cur) == v.FindItem(s));
                    if (ImGui::Selectable((s.empty() ? std::string("<none>") : StripStar(s)).c_str(), sel)) Commit(v, s);
                }
                ImGui::EndListBox();
            }
        }

        // ---- text -----------------------------------------------------------------------
        void DrawEdit(const gdf::Component& c, const gdf::Variable& v, float width, bool multi)
        {
            Row r = BeginRow(c, v, width);
            std::string s = m_entry->GetString(v.name);
            if (multi)
            {
                float h = c.height > 0 ? Theme::S((float)c.height) : Theme::S(80);
                if (ImGui::InputTextMultiline("##t", &s, ImVec2(r.ctrlW, h))) Commit(v, s);
            }
            else
            {
                ImGui::SetNextItemWidth(r.ctrlW);
                if (ImGui::InputText("##t", &s)) Commit(v, s);
            }
        }

        // ---- numbers --------------------------------------------------------------------
        bool Spin(const char* id, double& val, double mn, double mx, double step, bool isInt, float width)
        {
            const float arrowW = Theme::S(15);
            bool changed = false;
            ImGui::SetNextItemWidth(std::max(width - arrowW, Theme::S(30)));
            double before = val;
            if (ImGui::InputDouble(id, &val, 0, 0, isInt ? "%.0f" : "%.6g")) changed = true;
            ImVec2 pmin = ImGui::GetItemRectMin(), pmax = ImGui::GetItemRectMax();
            ImGui::SameLine(0, 1);
            ImGui::Dummy(ImVec2(arrowW, pmax.y - pmin.y));   // reserves the arrows' space in the layout
            ImVec2 amin = ImGui::GetItemRectMin();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            float half = (pmax.y - pmin.y) * 0.5f;
            for (int dir = 0; dir < 2; ++dir)
            {
                ImRect r(ImVec2(amin.x, pmin.y + dir * half), ImVec2(amin.x + arrowW, pmin.y + (dir + 1) * half));
                ImGuiID bid = ImGui::GetID(dir ? "##dn" : "##up");
                bool hovered, held;
                ImGui::PushItemFlag(ImGuiItemFlags_ButtonRepeat, true);
                bool pressed = ImGui::ButtonBehavior(r, bid, &hovered, &held);
                ImGui::PopItemFlag();
                dl->AddRectFilled(r.Min, r.Max, held ? IM_COL32(0, 0, 0, 120) : hovered ? IM_COL32(95, 95, 95, 255) : IM_COL32(68, 68, 68, 255));
                dl->AddRect(r.Min, r.Max, Palette::Border);
                ImVec2 c(r.GetCenter());
                float s = Theme::S(3);
                if (dir == 0) dl->AddTriangleFilled(ImVec2(c.x - s, c.y + s * 0.6f), ImVec2(c.x + s, c.y + s * 0.6f), ImVec2(c.x, c.y - s * 0.7f), Palette::Text);
                else dl->AddTriangleFilled(ImVec2(c.x - s, c.y - s * 0.6f), ImVec2(c.x + s, c.y - s * 0.6f), ImVec2(c.x, c.y + s * 0.7f), Palette::Text);
                if (pressed) { val += dir == 0 ? step : -step; changed = true; }
            }
            if (changed)
            {
                if (isInt) val = std::round(val);
                if (mx > mn) val = std::clamp(val, mn, mx);
                if (val == before) changed = false;
            }
            return changed;
        }

        void DrawNumber(const gdf::Component& c, const gdf::Variable& v, float width, bool isIntEdit)
        {
            Row r = BeginRow(c, v, width);
            bool isInt = v.type == gdf::VarType::Int;
            double val = m_entry->GetNumber(v.name);
            double slow = c.NumArg(1, *m_entry, isInt ? 1 : 0.1);
            if (Spin("##n", val, v.minV, v.maxV, slow, isInt, std::min(r.ctrlW, Theme::S(150))))
                Commit(v, isInt ? std::to_string((long long)std::llround(val)) : FormatFloat(val));
        }

        void DrawCheck(const gdf::Component& c, const gdf::Variable& v, float width)
        {
            bool on = m_entry->GetInt(v.name) != 0;
            std::string label = LabelFor(c, v);
            bool changed = false;
            if (c.rightText)
            {
                changed = ImGui::Checkbox((label + "##cb").c_str(), &on);
                if (!c.tooltip.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", c.tooltip.c_str());
            }
            else
            {
                Row r = BeginRow(c, v, width);
                changed = ImGui::Checkbox("##cb", &on);
            }
            if (changed) Commit(v, on ? "1" : "0");
        }

        // ---- colour ---------------------------------------------------------------------
        void DrawColor(const gdf::Component& c, const gdf::Variable& v, float width)
        {
            Row r = BeginRow(c, v, width);
            auto col = m_entry->GetColor(v.name);
            bool changed = false;
            ImVec4 cv(col[0], col[1], col[2], col[3]);
            if (ImGui::ColorButton("##sw", cv, ImGuiColorEditFlags_NoAlpha, ImVec2(Theme::S(60), ImGui::GetFrameHeight()))) ImGui::OpenPopup("##picker");
            if (ImGui::BeginPopup("##picker"))
            {
                float tmp[4] = { col[0], col[1], col[2], col[3] };
                if (ImGui::ColorPicker4("##cp", tmp, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_NoSidePreview)) { for (int i = 0; i < 4; ++i) col[i] = tmp[i]; changed = true; }
                ImGui::EndPopup();
            }
            const char* names[4] = { "R", "G", "B", "A" };
            for (int i = 0; i < 4; ++i)
            {
                ImGui::SameLine();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(names[i]);
                ImGui::SameLine(0, Theme::S(4));
                ImGui::SetNextItemWidth(Theme::S(58));
                int iv = (int)std::lround(col[i] * 255.0f);
                ImGui::PushID(i);
                if (ImGui::InputInt("##ch", &iv, 0, 0)) { col[i] = std::clamp(iv, 0, 255) / 255.0f; changed = true; }
                ImGui::PopID();
            }
            if (changed)
            {
                char buf[128];
                std::snprintf(buf, sizeof(buf), "%f %f %f %f", col[0], col[1], col[2], col[3]);
                Commit(v, buf);
            }
        }

        // ---- files ----------------------------------------------------------------------
        static bool LooksLikeImage(const std::string& s) { return ImageLoader::IsImageFile(fs::path(s)); }

        void DrawFile(const gdf::Component& c, const gdf::Variable& v, float width)
        {
            std::string s = m_entry->GetString(v.name);
            fs::path resolved = s.empty() ? fs::path{} : m_session.project.ResolveFile(s, c.relDir);
            bool image = LooksLikeImage(s) || (s.empty() && !c.relDir.size() && (v.name.find("Map") != std::string::npos));
            const float fh = ImGui::GetFrameHeight();
            const float btn = fh;

            if (image)
            {
                // thumbnail + dimensions like the Asset Property Editor
                float thumb = Theme::S(64);
                Row r = BeginRow(c, v, width);
                float x0 = ImGui::GetCursorPosX();
                ImGui::BeginGroup();
                ImVec2 tp = ImGui::GetCursorScreenPos();
                std::shared_ptr<GpuTexture> tex;
                if (!resolved.empty()) { std::string err; tex = m_renderer.LoadTexture(resolved, err); }
                ImGui::InvisibleButton("##thumb", ImVec2(thumb, thumb));
                ImDrawList* dl = ImGui::GetWindowDrawList();
                if (tex) dl->AddImage((ImTextureID)(intptr_t)tex->tex.Get(), tp, ImVec2(tp.x + thumb, tp.y + thumb));
                else
                {
                    dl->AddRectFilled(tp, ImVec2(tp.x + thumb, tp.y + thumb), IM_COL32(58, 58, 58, 255));
                    dl->AddText(ImVec2(tp.x + Theme::S(8), tp.y + thumb * 0.5f - Theme::S(8)), Palette::TextDim, s.empty() ? "none" : "missing");
                }
                dl->AddRect(tp, ImVec2(tp.x + thumb, tp.y + thumb), IM_COL32(20, 20, 20, 255));
                if (tex && ImGui::IsItemClicked()) m_session.previewImage = resolved;
                if (tex && ImGui::IsItemHovered()) ImGui::SetTooltip("Click to view in the preview");
                ImGui::EndGroup();
                ImGui::SameLine();
                ImGui::BeginGroup();
                float ctrlW = r.ctrlW - thumb - Spacing();
                if (IconButton("zoom", Icon::Search, Theme::S(14), "Show in preview") && tex) m_session.previewImage = resolved;
                ImGui::SameLine(0, 2);
                FileField(c, v, s, ctrlW - btn - Theme::S(8), btn, resolved);
                ImGui::Dummy(ImVec2(0, Theme::S(2)));
                if (tex) ImGui::TextDisabled("%s", tex->description.c_str());
                else if (!s.empty()) ImGui::TextColored(ImVec4(0.9f, 0.45f, 0.35f, 1), "file not found");
                ImGui::EndGroup();
                (void)x0;
                ImGui::Dummy(ImVec2(0, Theme::S(2)));
            }
            else
            {
                Row r = BeginRow(c, v, width);
                FileField(c, v, s, r.ctrlW - Theme::S(8), btn, resolved);
            }
        }

        void FileField(const gdf::Component& c, const gdf::Variable& v, std::string& s, float totalW, float btn, const fs::path& resolved)
        {
            ImGui::SetNextItemWidth(std::max(totalW - btn * 2 - Spacing() * 2, Theme::S(40)));
            if (ImGui::InputText("##f", &s)) Commit(v, s);
            ImGui::SameLine();
            if (IconButton("browse", Icon::Folder, Theme::S(16), "Browse for file")) Browse(c, v);
            ImGui::SameLine(0, 2);
            if (IconButton("open", Icon::Document, Theme::S(16), "Open file in Windows") && !resolved.empty()) platform::OpenInShell(resolved);
        }

        void Browse(const gdf::Component& c, const gdf::Variable& v)
        {
            const fs::path& root = m_session.project.Root();
            fs::path startDir = root / NormalizeSlashes(c.relDir);
            std::error_code ec;
            if (!fs::exists(startDir, ec)) startDir = root;
            bool img = c.relDir.empty();
            fs::path pick = platform::PickFile(m_hwnd, startDir,
                img ? "Images (*.tga;*.dds;*.png;*.jpg;*.bmp)\0*.tga;*.dds;*.png;*.jpg;*.bmp\0All files (*.*)\0*.*\0"
                    : "All files (*.*)\0*.*\0");
            if (pick.empty()) return;

            std::vector<fs::path> bases;
            if (!c.relDir.empty()) bases.push_back(root / NormalizeSlashes(c.relDir));
            else for (auto d : { "texture_assets", "raw/images", "" }) bases.push_back(root / d);
            for (auto& b : bases)
            {
                fs::path rel = fs::relative(pick, b, ec);
                if (!ec && !rel.empty() && *rel.begin() != "..")
                {
                    Commit(v, NormalizeSlashes(rel.string()));
                    return;
                }
            }
            m_session.Notify("Filename must be in a subdirectory of '" + bases.front().string() + "'");
        }

        Renderer& m_renderer;
        Session& m_session;
        HWND m_hwnd;
        std::string m_filter;
        gdt::Entry* m_entry = nullptr;
        const gdf::Def* m_def = nullptr;
        std::unordered_map<const gdf::Component*, bool> m_open;
    };
}
