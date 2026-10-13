module;
#include <cstdlib>
#include <utility>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "imgui.h"

export module openape.ui.weapon;

import openape.util;
import openape.math;
import openape.mesh;
import openape.renderer;
import openape.shading;
import openape.xmodelfile;
import openape.xanim;
import openape.skinning;
import openape.gdf;
import openape.gdt;
import openape.project;
import openape.session;
import openape.ui.theme;

// Everything needed to preview a weapon asset (bulletweapon, projectileweapon, grenadeweapon, turretweapon, vehicleweapon):
// the view model (hands + gun + attachments), the world models, an animation picker, and the HUD / kill feed icons.
export namespace oa::ui
{
    inline bool IsWeaponDef(const std::string& gdfName)
    {
        std::string n = ToLower(gdfName);
        return n == "bulletweapon.gdf" || n == "projectileweapon.gdf" || n == "grenadeweapon.gdf" || n == "turretweapon.gdf" || n == "vehicleweapon.gdf";
    }

    class WeaponScene
    {
    public:
        enum class View { ViewModel, WorldModel };

        WeaponScene(Renderer& r, Session& s, MaterialShading& shading) : m_renderer(r), m_session(s), m_shading(shading) {}

        // ------------------------------------------------------------------ loading
        struct Part
        {
            std::string field;            // "gunModel", "gunModel2", "handModel", "worldModel3", ...
            std::string model;            // xmodel asset name
            // Rocket: the projectile loaded in the launcher (rocketModel), which hangs off the gun's tag_clip
            enum class Role { Hands, Gun, World, Rocket } role = Role::Gun;
            MeshData data, posed;
            std::unique_ptr<GpuMesh> gpu;
            std::vector<SurfaceStyle> styles;
            std::vector<Bone> posedBones;
            Skinner skinner;
            int bound = 0;                // bones driven by the current animation
            Mat4 attach;                  // placement on the hands' weapon tag when the animation does not drive the part
            bool visible = true;
            std::string error;
        };

        // Rebuilds the scene for a weapon entry. `mode` is the shading mode used for the model surfaces.
        void Load(const gdt::Entry& e, ShadeMode mode)
        {
            m_parts.clear(); m_anims.clear(); m_notes.clear(); m_hud = {};
            m_entry = &e;
            m_displayName = e.GetString("displayName");
            m_altWeapon = e.GetString("altWeapon");

            auto add = [&](const std::string& field, Part::Role role) {
                std::string name = e.GetString(field);
                if (name.empty()) return;
                auto p = std::make_unique<Part>();
                p->field = field; p->model = name; p->role = role;
                LoadModel(*p, mode);
                m_parts.push_back(std::move(p));
            };
            add("handModel", Part::Role::Hands);
            // Singleplayer weapons leave handModel empty: the level script picks the hands. Show the stock viewhands instead.
            if (e.GetString("handModel").empty())
            {
                auto p = std::make_unique<Part>();
                p->field = "handModel (default)"; p->model = "viewmodel_base_viewhands"; p->role = Part::Role::Hands;
                LoadModel(*p, mode);
                if (p->gpu) m_parts.insert(m_parts.begin(), std::move(p));
            }
            add("gunModel", Part::Role::Gun);
            for (int i = 2; i <= 16; ++i) add("gunModel" + std::to_string(i), Part::Role::Gun);
            add("worldModel", Part::Role::World);
            for (int i = 2; i <= 16; ++i) add("worldModel" + std::to_string(i), Part::Role::World);
            add("rocketModel", Part::Role::Rocket);

            // animations of the weapon: every non-empty *Anim field
            if (auto* def = e.Def())
                for (auto& v : def->vars)
                    if (v.type == gdf::VarType::String && EndsWith(v.name, "Anim") && !e.GetString(v.name).empty())
                        m_anims.push_back({ v.name, e.GetString(v.name) });

            // one gun variant and one world variant are active: the first that loaded
            for (auto role : { Part::Role::Gun, Part::Role::World })
            {
                bool done = false;
                for (auto& p : m_parts) if (p->role == role) { p->visible = !done && p->gpu != nullptr; done = done || p->visible; }
            }
            BuildHud(e, mode);
            if (m_view == View::WorldModel && !HasRole(Part::Role::World)) m_view = View::ViewModel;
            if (m_view == View::ViewModel && !HasRole(Part::Role::Hands) && !HasRole(Part::Role::Gun) && HasRole(Part::Role::World)) m_view = View::WorldModel;
            ComputeBounds();
        }

        // Makes `chosen` the only visible part of its role (hands stay visible).
        void SelectVariant(Part* chosen)
        {
            for (auto& p : m_parts) if (p->role == chosen->role) p->visible = (p.get() == chosen);
            ComputeBounds();
        }

        // what the current view draws: the hands, the gun and its rocket, or the world models
        bool InView(const Part& p) const
        {
            if (m_view == View::WorldModel) return p.role == Part::Role::World;
            return p.role == Part::Role::Hands || p.role == Part::Role::Gun || p.role == Part::Role::Rocket;
        }
        // parts that are not driven by the animation's skeleton
        static bool IsStatic(Part::Role r) { return r == Part::Role::World || r == Part::Role::Rocket; }

        bool HasRole(Part::Role r) const { for (auto& p : m_parts) if (p->role == r && p->gpu) return true; return false; }

        const gdt::Entry* Entry() const { return m_entry; }
        const std::vector<std::unique_ptr<Part>>& Parts() const { return m_parts; }
        const std::string& DisplayName() const { return m_displayName; }
        const std::vector<std::string>& Overlay() const { return m_overlay; }

        struct AnimRef { std::string field, name; };
        const std::vector<AnimRef>& Anims() const { return m_anims; }
        const std::string& CurrentAnim() const { return m_currentAnim; }

        // ------------------------------------------------------------------ animation
        void SetAnimation(const Animation* a, const std::string& name)
        {
            m_anim = a; m_currentAnim = name;
            for (auto& p : m_parts)
            {
                p->bound = (a && !IsStatic(p->role)) ? p->skinner.Bind(p->data, *a) : 0;
                if (!a) { p->posed = p->data; p->posedBones = p->data.bones; if (p->gpu) m_renderer.UpdateMesh(*p->gpu, p->posed); }
            }
        }

        // Poses and skins every animated part for the given frame.
        void Pose(float frame, bool loop)
        {
            Part* hands = nullptr;
            for (auto& p : m_parts) if (p->role == Part::Role::Hands && p->gpu) { hands = p.get(); break; }
            // pose the hands first: the gun hangs from their weapon tag
            if (hands && m_anim && hands->bound > 0)
            {
                hands->skinner.Pose(hands->data, *m_anim, frame, loop, hands->posedBones);
                if (!hands->data.skin.empty()) { Skinner::Skin(hands->data, hands->posedBones, hands->posed); m_renderer.UpdateMesh(*hands->gpu, hands->posed); }
                hands->attach = Mat4{};
            }
            const Bone* tag = nullptr;
            if (hands)
            {
                const auto& hb = (m_anim && hands->bound > 0) ? hands->posedBones : hands->data.bones;
                for (auto& b : hb) if (IEquals(b.name, "tag_weapon")) { tag = &b; break; }
            }
            for (auto& p : m_parts)
            {
                if (p.get() == hands && m_anim && hands->bound > 0) continue;   // already done
                if (!p->gpu || IsStatic(p->role)) { p->attach = Mat4{}; continue; }
                if (m_anim && p->bound > 0)
                {
                    p->skinner.Pose(p->data, *m_anim, frame, loop, p->posedBones, p->role == Part::Role::Gun ? tag : nullptr, p->role == Part::Role::Gun);
                    if (!p->data.skin.empty()) { Skinner::Skin(p->data, p->posedBones, p->posed); m_renderer.UpdateMesh(*p->gpu, p->posed); }
                    p->attach = Mat4{};
                }
                else
                {
                    p->attach = Mat4{};
                    if (p->role == Part::Role::Gun && hands) p->attach = AttachMatrix(*hands, m_anim != nullptr);
                }
            }
            PlaceRockets();
        }

        // ------------------------------------------------------------------ rendering
        void Collect(std::vector<Renderer::DrawCall>& calls) const
        {
            for (auto& p : m_parts)
            {
                if (!p->gpu || !p->visible) continue;
                if (!InView(*p)) continue;
                for (size_t i = 0; i < p->gpu->submeshes.size(); ++i)
                {
                    Renderer::DrawCall dc{ p->gpu.get(), i, i < p->styles.size() ? p->styles[i] : SurfaceStyle{} };
                    dc.world = p->attach;
                    calls.push_back(std::move(dc));
                }
            }
        }

        // Bones of the shown parts in scene space (skeleton overlay).
        void CollectBones(std::vector<Bone>& out) const
        {
            out.clear();
            for (auto& p : m_parts)
            {
                if (!p->gpu || !p->visible) continue;
                if (!InView(*p)) continue;
                const auto& base = (m_anim && p->bound > 0) ? p->posedBones : p->data.bones;
                int off = (int)out.size();
                for (auto& b : base)
                {
                    Bone c = b;
                    const Vec3 v = b.worldPos;
                    const auto& m = p->attach.m;
                    c.worldPos = { v.x * m[0][0] + v.y * m[1][0] + v.z * m[2][0] + m[3][0],
                                   v.x * m[0][1] + v.y * m[1][1] + v.z * m[2][1] + m[3][1],
                                   v.x * m[0][2] + v.y * m[1][2] + v.z * m[2][2] + m[3][2] };
                    c.parent = b.parent >= 0 ? b.parent + off : -1;
                    out.push_back(std::move(c));
                }
            }
        }

        // The game camera: the hands' tag_camera bone (X forward, Z up) of the current pose.
        bool CameraPose(Vec3& eye, Vec3& dir, Vec3& up) const
        {
            if (m_view != View::ViewModel) return false;
            for (auto& p : m_parts)
            {
                if (p->role != Part::Role::Hands || !p->gpu) continue;
                const auto& bones = (m_anim && p->bound > 0) ? p->posedBones : p->data.bones;
                for (auto& b : bones)
                    if (IEquals(b.name, "tag_camera"))
                    {
                        eye = b.worldPos; dir = b.worldRot.Rotate({ 1, 0, 0 }); up = b.worldRot.Rotate({ 0, 0, 1 });
                        return true;
                    }
            }
            return false;
        }

        // Bounding sphere of what is drawn right now (animations move parts far from the bind pose).
        void PosedSphere(Vec3& center, float& radius) const
        {
            Vec3 mn{ 1e30f, 1e30f, 1e30f }, mx{ -1e30f, -1e30f, -1e30f };
            bool any = false;
            for (auto& p : m_parts)
            {
                if (!p->gpu || !p->visible) continue;
                if (!InView(*p)) continue;
                const auto& m = p->attach.m;
                for (auto& v : p->posed.vertices)
                {
                    Vec3 w{ v.x * m[0][0] + v.y * m[1][0] + v.z * m[2][0] + m[3][0],
                            v.x * m[0][1] + v.y * m[1][1] + v.z * m[2][1] + m[3][1],
                            v.x * m[0][2] + v.y * m[1][2] + v.z * m[2][2] + m[3][2] };
                    mn = { std::min(mn.x, w.x), std::min(mn.y, w.y), std::min(mn.z, w.z) };
                    mx = { std::max(mx.x, w.x), std::max(mx.y, w.y), std::max(mx.z, w.z) };
                    any = true;
                }
            }
            if (!any) { center = m_center; radius = m_radius; return; }
            center = (mn + mx) * 0.5f;
            radius = std::max((mx - mn).Length() * 0.5f, 0.01f);
        }

        Vec3 Center() const { return m_center; }
        float Radius() const { return m_radius; }

        // ------------------------------------------------------------------ controls
        // Row with the view mode, the animation picker and the model list. Returns the name of an animation the user
        // picked (to be loaded by the caller), or an empty string.
        std::string DrawControls(bool& modelsChanged)
        {
            std::string picked;
            modelsChanged = false;
            ImGui::AlignTextToFramePadding();
            ImGui::SetNextItemWidth(Theme::S(112));
            const char* views[] = { "View model", "World model" };
            if (ImGui::BeginCombo("##wview", views[(int)m_view]))
            {
                for (int i = 0; i < 2; ++i)
                    if (ImGui::Selectable(views[i], (int)m_view == i)) { m_view = (View)i; ComputeBounds(); modelsChanged = true; m_viewChanged = true; }
                ImGui::EndCombo();
            }
            ImGui::SameLine(0, Theme::S(6));
            ImGui::SetNextItemWidth(Theme::S(170));
            const char* cur = m_currentAnim.empty() ? "<no animation>" : m_currentAnim.c_str();
            ImGui::BeginDisabled(m_view != View::ViewModel);
            if (ImGui::BeginCombo("##wanim", cur))
            {
                if (ImGui::Selectable("<no animation>", m_currentAnim.empty())) picked = "-";
                for (auto& a : m_anims)
                {
                    std::string label = a.field + ":  " + a.name;
                    if (ImGui::Selectable(label.c_str(), m_currentAnim == a.name)) picked = a.name;
                }
                ImGui::EndCombo();
            }
            ImGui::EndDisabled();
            ImGui::SameLine(0, Theme::S(6));
            {
                // gunModel / gunModel2.. and worldModel / worldModel2.. are variants (camos): exactly one of each is shown.
                // Only the selector of the current view is offered.
                const Part::Role role = m_view == View::WorldModel ? Part::Role::World : Part::Role::Gun;
                const Part* shown = nullptr;
                for (auto& p : m_parts) if (p->role == role && p->gpu && p->visible) { shown = p.get(); break; }
                std::string cur = shown ? shown->field + ":  " + shown->model : "<none>";
                ImGui::SameLine(0, Theme::S(6));
                ImGui::SetNextItemWidth(Theme::S(m_view == View::WorldModel ? 250 : 230));
                ImGui::BeginDisabled(!HasRole(role));
                if (ImGui::BeginCombo("##wvariant", cur.c_str()))
                {
                    for (auto& p : m_parts)
                    {
                        if (p->role != role || !p->gpu) continue;
                        std::string label = p->field + ":  " + p->model;
                        if (ImGui::Selectable(label.c_str(), p.get() == shown)) { SelectVariant(p.get()); modelsChanged = true; }
                    }
                    ImGui::EndCombo();
                }
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip(m_view == View::WorldModel ? "World model variant (worldModel...)" : "Gun model variant (gunModel...)");
            }
            if (!m_altWeapon.empty())
            {
                ImGui::SameLine(0, Theme::S(6));
                if (auto* alt = FindWeapon(m_altWeapon); alt && ImGui::Button(("Alt: " + m_altWeapon).c_str())) m_openRequest = alt;
                else if (!alt) ImGui::TextDisabled("alt: %s (not found)", m_altWeapon.c_str());
            }
            return picked;
        }

        gdt::Entry* TakeOpenRequest() { auto* r = m_openRequest; m_openRequest = nullptr; return r; }

        View CurrentView() const { return m_view; }
        void SetView(int v) { if (v >= 0 && v <= 1) { m_view = (View)v; ComputeBounds(); } }
        // true once after the user switched between view model / world model / projectile (the camera frames the new subject)
        bool TakeViewChanged() { bool v = m_viewChanged; m_viewChanged = false; return v; }

        // ------------------------------------------------------------------ HUD / kill feed
        float HudHeight() const { return Theme::S(150); }

        void DrawHud(ImVec2 size)
        {
            ImVec2 p0 = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y), IM_COL32(44, 46, 50, 255));
            ImGui::PushClipRect(p0, ImVec2(p0.x + size.x, p0.y + size.y), true);

            // the four icons of the weapon, on a dark card each
            const float pad = Theme::S(8), labelH = ImGui::GetTextLineHeight();
            float x = p0.x + pad;
            const float cellH = size.y - pad * 2 - labelH - Theme::S(4);
            for (auto& ic : m_hud.icons)
            {
                float h = std::min(cellH, Theme::S(44)), w = h * ic.ratio;
                if (w > Theme::S(96)) { w = Theme::S(96); h = w / ic.ratio; }
                float cw = std::max(w, ImGui::CalcTextSize(ic.label.c_str()).x);
                ImVec2 a(x, p0.y + pad), b(x + cw + pad, p0.y + pad + cellH);
                dl->AddRectFilled(a, b, IM_COL32(24, 25, 28, 255), 3);
                dl->AddRect(a, b, IM_COL32(70, 72, 78, 255), 3);
                ImVec2 ia(a.x + (b.x - a.x - w) * 0.5f, a.y + (cellH - h) * 0.5f);
                DrawIcon(dl, ic, ia, ImVec2(ia.x + w, ia.y + h), false);
                dl->AddText(ImVec2(a.x + (b.x - a.x - ImGui::CalcTextSize(ic.label.c_str()).x) * 0.5f, b.y + 2), Palette::TextDim, ic.label.c_str());
                if (ImGui::IsMouseHoveringRect(a, b) && !ic.material.empty())
                    ImGui::SetTooltip("%s\nmaterial: %s  ratio %s%s", ic.label.c_str(), ic.material.c_str(), ic.ratioText.c_str(), ic.tex ? "" : "\n(image not found)");
                x = b.x + pad;
            }

            // an in-game kill feed made of the weapon's kill icon, like the obituary text of the game
            float fx = std::max(x + pad, p0.x + size.x * 0.46f), fw = p0.x + size.x - pad - fx;
            if (fw > Theme::S(150))
            {
                ImVec2 a(fx, p0.y + pad), b(fx + fw, p0.y + size.y - pad);
                dl->AddRectFilled(a, b, IM_COL32(66, 92, 104, 255), 3);
                dl->AddRectFilledMultiColor(a, b, IM_COL32(120, 130, 120, 70), IM_COL32(40, 50, 60, 0), IM_COL32(20, 24, 30, 160), IM_COL32(20, 24, 30, 160));
                const Icon* kill = FindIcon("Kill icon");
                float lh = labelH * 1.35f;
                float y = a.y + Theme::S(5);
                auto text = [&](ImVec2 p, const char* t, ImU32 c) {
                    dl->AddText(ImVec2(p.x + 1, p.y + 1), IM_COL32(0, 0, 0, 220), t);
                    dl->AddText(p, c, t);
                };
                // call-in message
                text(ImVec2(a.x + Theme::S(6), y), "UAV Recon called in by Zed for 30 seconds", IM_COL32(210, 210, 200, 255));
                y += lh;
                struct Row { const char* killer; const char* victim; ImU32 kc, vc; bool headshot; };
                const Row rows[] = { { "Zed", "Nightcrawler", IM_COL32(150, 235, 140, 255), IM_COL32(255, 255, 255, 255), false },
                                     { "Viper", "Ace", IM_COL32(255, 255, 255, 255), IM_COL32(255, 150, 120, 255), false },
                                     { "Wolfpack", "Blaze", IM_COL32(255, 255, 255, 255), IM_COL32(255, 255, 255, 255), true },
                                     { "Ghost", "Dragonfly", IM_COL32(255, 255, 255, 255), IM_COL32(255, 150, 120, 255), false } };
                for (auto& r : rows)
                {
                    if (y + lh > b.y) break;
                    float cx = a.x + Theme::S(6);
                    text(ImVec2(cx, y), r.killer, r.kc);
                    cx += ImGui::CalcTextSize(r.killer).x + Theme::S(8);
                    if (kill && kill->tex)
                    {
                        float ih = lh * 1.05f, iw = ih * kill->ratio;
                        iw = std::min(iw, fw * 0.4f);
                        DrawIcon(dl, *kill, ImVec2(cx, y + (labelH - ih) * 0.5f), ImVec2(cx + iw, y + (labelH - ih) * 0.5f + ih), m_hud.flipKill);
                        cx += iw + Theme::S(8);
                    }
                    else { text(ImVec2(cx, y), "[no kill icon]", IM_COL32(200, 160, 120, 255)); cx += ImGui::CalcTextSize("[no kill icon]").x + Theme::S(8); }
                    text(ImVec2(cx, y), r.victim, r.vc);
                    y += lh;
                }
            }
            ImGui::PopClipRect();
            ImGui::Dummy(size);
        }

        struct Icon
        {
            std::string label, material, ratioText;
            float ratio = 4.0f;
            std::shared_ptr<GpuTexture> tex;
        };

    private:
        // ------------------------------------------------------------------ helpers
        gdt::Entry* FindWeapon(const std::string& name) const
        {
            for (auto d : { "bulletweapon.gdf", "projectileweapon.gdf", "grenadeweapon.gdf", "turretweapon.gdf", "vehicleweapon.gdf" })
                if (auto* e = m_session.project.FindEntry(d, name)) return e;
            return nullptr;
        }

        void LoadModel(Part& p, ShadeMode mode)
        {
            std::string err, source;
            bool ok = false;
            if (auto* me = m_session.project.FindEntry("xmodel.gdf", p.model))
            {
                std::string rel = me->GetString("filename");
                auto path = rel.empty() ? fs::path{} : m_session.project.ResolveFile(rel, "model_export/");
                if (!path.empty()) ok = XModelExportLoader::Load(path, p.data, err);
            }
            if (!ok)
            {
                std::string cerr;
                ok = CompiledXModel::Load(m_session.project.Root() / "raw", p.model, 0, p.data, cerr);
                if (!ok) err = err.empty() ? cerr : err + "; " + cerr;
            }
            if (!ok) { p.error = "model '" + p.model + "': " + err; m_overlay.push_back(p.error); return; }
            p.gpu = m_renderer.CreateMesh(p.data);
            if (!p.gpu) { p.error = "could not upload " + p.model; return; }
            p.posed = p.data; p.posedBones = p.data.bones;
            for (auto& sm : p.data.submeshes)
            {
                gdt::Entry* mat = m_session.project.FindEntry("material.gdf", sm.material);
                p.styles.push_back(m_shading.Build(sm.material, mat, mode).style);
            }
        }

        // placement of a bone as a row-vector matrix (v' = v * M): rotation, then the translation
        static Mat4 BoneMatrix(const Bone& b)
        {
            const Quat& q = b.worldRot;
            Vec3 X = q.Rotate({ 1, 0, 0 }), Y = q.Rotate({ 0, 1, 0 }), Z = q.Rotate({ 0, 0, 1 });
            Mat4 m;
            m.m[0][0] = X.x; m.m[0][1] = X.y; m.m[0][2] = X.z;
            m.m[1][0] = Y.x; m.m[1][1] = Y.y; m.m[1][2] = Y.z;
            m.m[2][0] = Z.x; m.m[2][1] = Z.y; m.m[2][2] = Z.z;
            m.m[3][0] = b.worldPos.x; m.m[3][1] = b.worldPos.y; m.m[3][2] = b.worldPos.z;
            return m;
        }

        // The loaded rocket sits on the gun's tag_clip, wherever the animation moves that tag.
        void PlaceRockets()
        {
            const Part* gun = nullptr;
            for (auto& p : m_parts) if (p->role == Part::Role::Gun && p->gpu && p->visible) { gun = p.get(); break; }
            for (auto& p : m_parts)
            {
                if (p->role != Part::Role::Rocket) continue;
                p->attach = Mat4{};
                if (!gun) continue;
                const auto& bones = (m_anim && gun->bound > 0) ? gun->posedBones : gun->data.bones;
                for (auto& b : bones)
                    if (IEquals(b.name, "tag_clip")) { p->attach = BoneMatrix(b) * gun->attach; break; }
            }
        }

        Mat4 AttachMatrix(const Part& hands, bool animated) const
        {
            const std::vector<Bone>& bones = animated ? hands.posedBones : hands.data.bones;
            for (auto name : { "tag_weapon", "tag_weapon_right", "tag_weapon_left", "j_gun" })
                for (auto& b : bones)
                    if (IEquals(b.name, name))
                    {
                        // rotation as a row-vector matrix (v' = v * M) followed by the translation
                        const Quat& q = b.worldRot;
                        Vec3 X = q.Rotate({ 1, 0, 0 }), Y = q.Rotate({ 0, 1, 0 }), Z = q.Rotate({ 0, 0, 1 });
                        Mat4 m;
                        m.m[0][0] = X.x; m.m[0][1] = X.y; m.m[0][2] = X.z;
                        m.m[1][0] = Y.x; m.m[1][1] = Y.y; m.m[1][2] = Y.z;
                        m.m[2][0] = Z.x; m.m[2][1] = Z.y; m.m[2][2] = Z.z;
                        m.m[3][0] = b.worldPos.x; m.m[3][1] = b.worldPos.y; m.m[3][2] = b.worldPos.z;
                        return m;
                    }
            return Mat4{};
        }

        void ComputeBounds()
        {
            bool any = false;
            Vec3 mn{}, mx{};
            for (auto& p : m_parts)
            {
                if (!p->gpu || !p->visible) continue;
                if (!InView(*p)) continue;
                if (p->role == Part::Role::Rocket) continue;   // inside the gun's extent, and its model origin is the gun's tag
                Vec3 a = p->data.boundsMin, b = p->data.boundsMax;
                // guns hang off the hands' weapon tag: include that offset
                if (p->role == Part::Role::Gun)
                    for (auto& h : m_parts)
                        if (h->role == Part::Role::Hands && h->gpu)
                            for (auto& bn : h->data.bones) if (IEquals(bn.name, "tag_weapon")) { a = a + bn.worldPos; b = b + bn.worldPos; }
                if (!any) { mn = a; mx = b; any = true; }
                mn = { std::min(mn.x, a.x), std::min(mn.y, a.y), std::min(mn.z, a.z) };
                mx = { std::max(mx.x, b.x), std::max(mx.y, b.y), std::max(mx.z, b.z) };
            }
            if (!any) { m_center = {}; m_radius = 1; return; }
            m_center = (mn + mx) * 0.5f;
            m_radius = std::max((mx - mn).Length() * 0.5f, 0.01f);
        }

        // ------------------------------------------------------------------ HUD icons
        struct HudInfo
        {
            std::vector<Icon> icons;
            bool flipKill = false;
        };

        static float ParseRatio(const std::string& t, std::string& text)
        {
            text = t;
            size_t c = t.find(':');
            if (c == std::string::npos) return 4.0f;
            float a = std::strtof(t.c_str(), nullptr), b = std::strtof(t.c_str() + c + 1, nullptr);
            return b > 0 ? a / b : 4.0f;
        }

        void BuildHud(const gdt::Entry& e, ShadeMode)
        {
            struct Def { const char* label; const char* matKey; const char* ratioKey; };
            for (auto d : { Def{ "HUD icon", "hudIcon", "hudIconRatio" }, Def{ "Kill icon", "killIcon", "killIconRatio" },
                            Def{ "D-pad icon", "dpadIcon", "dpadIconRatio" }, Def{ "Ammo counter icon", "ammoCounterIcon", "ammoCounterIconRatio" } })
            {
                Icon ic;
                ic.label = d.label;
                ic.material = e.GetString(d.matKey);
                ic.ratio = ParseRatio(e.GetString(d.ratioKey), ic.ratioText);
                if (!ic.material.empty())
                {
                    gdt::Entry* mat = m_session.project.FindEntry("material.gdf", ic.material);
                    ic.tex = m_shading.Build(ic.material, mat, ShadeMode::Fixed).style.color;
                    // the generic white placeholder is not the icon
                    if (ic.tex && ic.tex->width <= 4 && ic.tex->height <= 4) ic.tex.reset();
                }
                m_hud.icons.push_back(std::move(ic));
            }
            m_hud.flipKill = e.GetInt("flipKillIcon") != 0;
        }

        const Icon* FindIcon(const char* label) const
        {
            for (auto& i : m_hud.icons) if (i.label == label) return &i;
            return nullptr;
        }

        void DrawIcon(ImDrawList* dl, const Icon& ic, ImVec2 a, ImVec2 b, bool flip) const
        {
            if (!ic.tex || !ic.tex->tex)
            {
                dl->AddRect(a, b, IM_COL32(120, 120, 120, 255));
                const char* t = ic.material.empty() ? "none" : "missing";
                dl->AddText(ImVec2(a.x + 3, a.y + (b.y - a.y - ImGui::GetTextLineHeight()) * 0.5f), IM_COL32(150, 150, 150, 255), t);
                return;
            }
            dl->AddImage((ImTextureID)(intptr_t)ic.tex->tex.Get(), a, b, ImVec2(flip ? 1.0f : 0.0f, 0), ImVec2(flip ? 0.0f : 1.0f, 1));
        }

        Renderer& m_renderer;
        Session& m_session;
        MaterialShading& m_shading;
        const gdt::Entry* m_entry = nullptr;
        std::vector<std::unique_ptr<Part>> m_parts;
        std::vector<AnimRef> m_anims;
        std::vector<std::string> m_notes, m_overlay;
        std::string m_displayName, m_altWeapon, m_currentAnim;
        const Animation* m_anim = nullptr;
        HudInfo m_hud;
        View m_view = View::ViewModel;
        bool m_viewChanged = false;
        Vec3 m_center{};
        float m_radius = 1;
        gdt::Entry* m_openRequest = nullptr;
    };
}
