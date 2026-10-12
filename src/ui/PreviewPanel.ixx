module;

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <filesystem>
#include "imgui.h"

export module openape.ui.preview;

import openape.util;
import openape.math;
import openape.mesh;
import openape.image;
import openape.renderer;
import openape.xmodelfile;
import openape.xanim;
import openape.skinning;
import openape.skeletonindex;
import openape.shading;
import openape.environment;
import openape.iwd;
import openape.gdf;
import openape.gdt;
import openape.project;
import openape.session;
import openape.ui.theme;
import openape.ui.weapon;

export namespace oa::ui
{
    enum class Shape { Sphere, Cube, Cylinder, Cone, Torus, Plane };

    // The 3D / 2D view. Materials are shown on a primitive, models in 3D, images as a flat picture.
    class PreviewPanel
    {
    public:
        PreviewPanel(Renderer& r, Session& s) : m_renderer(r), m_session(s), m_shading(r, s.game, s.project, s.iwds) { m_weapon = std::make_unique<WeaponScene>(r, s, m_shading); }

        void Invalidate() { m_signature.clear(); }
        void SetLight(int index) { m_envInit = true; ApplyLight(index); }
        void SetPreset(int i) { m_envInit = true; if (i >= 0 && i < (int)EnvironmentPresets().size()) ApplyPreset(i); }
        void SetSky(const std::string& name) { m_envInit = true; ApplySky(name); }
        void SetBones(bool names) { m_showBones = true; m_showBoneNames = names; }
        void SetZoom(float z) { m_zoom = z; }
        void SetLookUp(float u) { m_zoomUp = u; }
        void SetPitch(float p) { m_pitchOverride = p; }
        void SetFrame(int f) { m_pendingFrame = f; }
        void SetWeaponAnim(const std::string& n) { m_pendingWeaponAnim = n; }
        void SetFirstPerson(bool on) { m_fpv = on; }
        void SetShape(int sh) { m_shape = (Shape)std::clamp(sh, 0, 5); m_frameRequested = true; }
        void SetMode(int m) { m_mode = (ShadeMode)std::clamp(m, 0, 3); m_signature.clear(); }
        void OnDeviceLost() { m_target.Release(); m_opt.sky.reset(); m_skyCache.clear(); }
        void OnDeviceReset() { if (!m_skyName.empty()) { auto n = m_skyName; ApplySky(n); } }

        void Draw(ImVec2 size)
        {
            bool image = !m_session.previewImage.empty();
            EnsureEnvironment();
            if (!image) Refresh();
            DrawToolbar();
            if (!image && m_kind == Kind::Weapon) DrawWeaponControls();
            if (!image && m_isAnim && (m_kind == Kind::Model || m_kind == Kind::Weapon)) DrawTimeline();
            ImVec2 avail = ImGui::GetContentRegionAvail();
            if (avail.x < 8 || avail.y < 8) return;

            if (image) { DrawImageView(avail); return; }
            if (m_kind == Kind::None) { DrawMessage(avail, m_message.empty() ? "Select an asset to preview it" : m_message); return; }
            if (m_kind == Kind::Weapon)
            {
                float hud = m_showHud ? std::min(m_weapon->HudHeight(), avail.y * 0.45f) : 0.0f;
                DrawSceneView(ImVec2(avail.x, std::max(avail.y - hud - ImGui::GetStyle().ItemSpacing.y, 16.0f)));
                if (m_showHud) m_weapon->DrawHud(ImVec2(avail.x, hud));
                return;
            }
            DrawSceneView(avail);
        }

    private:
        enum class Kind { None, Material, Model, Weapon };

        // ------------------------------------------------------------------ toolbar
        void DrawToolbar()
        {
            float ic = Theme::S(16);
            if (IconButton("light", Icon::Bulb, ic, "Lighting and environment", !m_opt.lighting)) ImGui::OpenPopup("##lightmenu");
            if (ImGui::BeginPopup("##lightmenu"))
            {
                if (ImGui::MenuItem("Wireframe", nullptr, m_opt.wireframe)) m_opt.wireframe = !m_opt.wireframe;
                if (ImGui::MenuItem("No Lighting", nullptr, !m_opt.lighting)) { m_opt.lighting = !m_opt.lighting; m_signature.clear(); }
                ImGui::Separator();
                if (ImGui::BeginMenu("Shading"))
                {
                    ImGui::TextDisabled("The Game modes run the real shaders from raw/shader_bin");
                    for (ShadeMode m : { ShadeMode::Fakelight, ShadeMode::Unlit, ShadeMode::Sun, ShadeMode::Fixed })
                        if (ImGui::MenuItem(ShadeModeName(m), nullptr, m == m_mode)) { m_mode = m; m_signature.clear(); }
                    ImGui::EndMenu();
                }
                if (ImGui::BeginMenu("Presets"))
                {
                    const auto& presets = EnvironmentPresets();
                    for (int i = 0; i < (int)presets.size(); ++i)
                        if (ImGui::MenuItem(presets[i].name, nullptr, m_presetIndex == i)) ApplyPreset(i);
                    ImGui::EndMenu();
                }
                if (ImGui::BeginMenu("Lighting"))
                {
                    if (ImGui::MenuItem("Headlight (follows camera)", nullptr, m_lightIndex == -1)) ApplyLight(-1);
                    const auto& all = StockLightings();
                    for (int pass = 0; pass < 2; ++pass)
                    {
                        ImGui::SeparatorText(pass == 0 ? "Single player" : "Multiplayer");
                        for (int i = 0; i < (int)all.size(); ++i)
                            if (all[i].multiplayer == (pass == 1) && ImGui::MenuItem(all[i].name, nullptr, m_lightIndex == i && m_presetIndex < 0))
                            {
                                ApplyLight(i);
                                auto sky = SkyForMap(all[i].name, SkyNames());   // pick the sky that belongs to that map
                                if (!sky.empty()) ApplySky(sky);
                            }
                    }
                    ImGui::EndMenu();
                }
                if (ImGui::BeginMenu("Skybox"))
                {
                    for (auto& n : SkyNames()) if (ImGui::MenuItem(n.c_str(), nullptr, n == m_skyName)) ApplySky(n);
                    if (SkyNames().empty()) ImGui::TextDisabled("No stock .iwd archives found in <root>/main");
                    ImGui::EndMenu();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Draw Skybox", nullptr, m_opt.skybox)) m_opt.skybox = !m_opt.skybox;
                ImGui::EndPopup();
            }
            ImGui::SameLine(0, 2);
            if (IconButton("reset", Icon::Camera, ic, "Reset camera")) m_frameRequested = true;
            ImGui::SameLine(0, 2);
            if (IconButton("wire", Icon::Wire, ic, "Wireframe", m_opt.wireframe)) m_opt.wireframe = !m_opt.wireframe;
            ImGui::SameLine(0, 2);
            if (IconButton("grid", Icon::Grid, ic, "Floor grid", m_opt.grid)) m_opt.grid = !m_opt.grid;
            ImGui::SameLine(0, 2);
            if (IconButton("info", Icon::Info, ic, "Show or hide the info text in the corner of the view", m_session.showPreviewInfo)) m_session.showPreviewInfo = !m_session.showPreviewInfo;
            bool modelShown = (m_kind == Kind::Model || m_kind == Kind::Weapon) && m_session.previewImage.empty();
            if (modelShown)
            {
                ImGui::SameLine(0, 2);
                bool has = m_kind == Kind::Weapon ? !m_weaponBones.empty() : !m_modelData.bones.empty();
                ImGui::BeginDisabled(!has);
                if (IconButton("bones", Icon::Bone, ic, has ? "Show the skeleton" : "This model has no bones", m_showBones && has)) m_showBones = !m_showBones;
                ImGui::SameLine(0, 2);
                if (IconButton("bonenames", Icon::Tag, ic, "Show bone names (works with or without the bone shapes)", m_showBoneNames && has)) m_showBoneNames = !m_showBoneNames;
                ImGui::EndDisabled();
                if (m_kind == Kind::Weapon)
                {
                    ImGui::SameLine(0, 2);
                    bool can = m_weapon->CurrentView() == WeaponScene::View::ViewModel;
                    ImGui::BeginDisabled(!can);
                    if (IconButton("fpv", Icon::Eye, ic, can ? "Look through the camera bone (tag_camera) as in game" : "Only available for the view model", m_fpv && can))
                    { m_fpv = !m_fpv; if (!m_fpv) m_frameRequested = true; }
                    ImGui::EndDisabled();
                }
            }
            if (!(m_isAnim && (m_kind == Kind::Model || m_kind == Kind::Weapon)))
            {
                ImGui::SameLine(0, Theme::S(14));
                if (IconButton("prev", Icon::Prev, ic, "Rotate back")) m_cam.yaw -= 0.26f;
                ImGui::SameLine(0, 2);
                if (IconButton("play", m_spin ? Icon::Pause : Icon::Play, ic, "Turntable", m_spin)) m_spin = !m_spin;
                ImGui::SameLine(0, 2);
                if (IconButton("next", Icon::Next, ic, "Rotate forward")) m_cam.yaw += 0.26f;
                ImGui::SameLine(0, Theme::S(8));
                ImGui::SetNextItemWidth(Theme::S(70));
                ImGui::SliderFloat("##speed", &m_spinSpeed, 0.1f, 3.0f, "");
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Turntable speed");
            }
            if (!m_session.previewImage.empty())
            {
                ImGui::SameLine(0, Theme::S(14));
                if (ImGui::SmallButton("Back to asset")) m_session.previewImage.clear();
            }
        }

        void DrawMessage(ImVec2 avail, const std::string& text)
        {
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilledMultiColor(p, ImVec2(p.x + avail.x, p.y + avail.y), IM_COL32(110, 110, 118, 255), IM_COL32(110, 110, 118, 255),
                                        IM_COL32(48, 48, 52, 255), IM_COL32(48, 48, 52, 255));
            ImVec2 ts = ImGui::CalcTextSize(text.c_str(), nullptr, false, avail.x - 20);
            ImGui::SetCursorScreenPos(ImVec2(p.x + (avail.x - ts.x) * 0.5f, p.y + (avail.y - ts.y) * 0.5f));
            ImGui::PushTextWrapPos(p.x + avail.x - 10);
            ImGui::TextColored(ImVec4(0.85f, 0.85f, 0.85f, 1), "%s", text.c_str());
            ImGui::PopTextWrapPos();
            ImGui::SetCursorScreenPos(p);
            ImGui::Dummy(avail);
        }

        // ------------------------------------------------------------------ 3D view
        void DrawSceneView(ImVec2 avail)
        {
            int w = (int)avail.x, h = (int)avail.y;
            if (!m_renderer.EnsureTarget(m_target, w, h)) { DrawMessage(avail, "Could not create render target"); return; }

            if (m_frameRequested)
            {
                m_frameRequested = false;
                m_cam = OrbitCamera{};
                if (m_kind == Kind::Model && m_model && !m_modelData.Empty()) m_cam.Frame(m_modelData.Center(), m_modelData.Radius() * 0.8f);
                else if (m_kind == Kind::Weapon) { m_cam.Frame(m_weapon->Center(), m_weapon->Radius() * 0.8f); m_cam.yaw = -1.2f; m_cam.pitch = 0.18f; }
                else m_cam.Frame({ 0, 0, 0 }, ShapeRadius());
            }
            if (m_spin && !m_isAnim) m_cam.yaw += ImGui::GetIO().DeltaTime * m_spinSpeed * 0.8f;
            if (m_pitchOverride > -9) { m_cam.pitch = m_pitchOverride; m_pitchOverride = -10; }
            if (m_zoom > 0) { m_cam.distance *= m_zoom; m_cam.target.z += m_zoomUp; m_zoom = 0; }
            if (m_isAnim) AdvanceAnimation();
            if (m_kind == Kind::Weapon && !m_isAnim) m_weapon->Pose(0, false);
            m_cam.fov = m_orbitFovDeg * kPi / 180.0f;
            if (m_kind == Kind::Weapon)
            {
                m_weapon->CollectBones(m_weaponBones);
                Vec3 eye, dir, up;
                if (m_fpv && m_weapon->CameraPose(eye, dir, up))
                {
                    m_cam.firstPerson = true; m_cam.fpEye = eye; m_cam.fpDir = dir; m_cam.fpUp = up;
                    // the in-game field of view is horizontal
                    m_cam.fov = 2.0f * std::atan(std::tan(m_fpvFovDeg * kPi / 360.0f) * (float)h / (float)std::max(w, 1));
                }
                else if (m_cam.firstPerson) { m_cam.firstPerson = false; m_frameRequested = true; }
            }
            else m_cam.firstPerson = false;

            std::vector<Renderer::DrawCall> calls;
            Mat4 world;
            if (m_kind == Kind::Material)
            {
                if (auto* mesh = ShapeMesh()) calls.push_back({ mesh, 0, m_style });
            }
            else if (m_kind == Kind::Weapon) m_weapon->Collect(calls);
            else if (m_model)
            {
                for (size_t i = 0; i < m_model->submeshes.size(); ++i)
                    calls.push_back({ m_model.get(), i, i < m_subStyles.size() ? m_subStyles[i] : SurfaceStyle{} });
            }
            if (m_kind == Kind::Model && m_model && !m_modelData.Empty()) { m_cam.subjectCenter = m_modelData.Center(); m_cam.subjectRadius = m_modelData.Radius(); }
            else if (m_kind == Kind::Weapon) { m_cam.subjectCenter = m_weapon->Center(); m_cam.subjectRadius = m_weapon->Radius() * 1.5f; }
            else { m_cam.subjectCenter = { 0, 0, 0 }; m_cam.subjectRadius = ShapeRadius(); }
            OverlayGeometry bones3d;
            if ((m_kind == Kind::Model || m_kind == Kind::Weapon) && m_showBones && m_boneShapes) BuildBoneShapes(bones3d);
            m_renderer.RenderScene(m_target, m_cam, m_opt, calls, world, &bones3d);

            ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::Image((ImTextureID)(intptr_t)m_target.colorTex.Get(), avail);
            HandleViewInput();
            DrawContextMenu();
            if ((m_kind == Kind::Model || m_kind == Kind::Weapon) && (m_showBones || m_showBoneNames)) DrawBones(p, avail);
            if (m_rotatingLight)
            {
                char lb[96];
                if (m_lightIndex == -1) std::snprintf(lb, sizeof(lb), "Headlight angle: %.0f deg", m_opt.lightAngle * 180.0f / kPi);
                else std::snprintf(lb, sizeof(lb), "Light rotation: %.0f deg   height: %+.0f deg", m_opt.lightYawDeg, m_opt.lightElevDeg);
                ImDrawList* ldl = ImGui::GetWindowDrawList();
                ImVec2 lp(p.x + Theme::S(8), p.y + avail.y - ImGui::GetTextLineHeight() * 2.4f);
                ldl->AddText(ImVec2(lp.x + 1, lp.y + 1), IM_COL32(0, 0, 0, 220), lb);
                ldl->AddText(lp, IM_COL32(255, 220, 120, 255), lb);
            }
            if (m_isAnim)
            {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "Frame: %d", (int)m_frame);
                ImDrawList* fdl = ImGui::GetWindowDrawList();
                ImVec2 tp(p.x + Theme::S(8), p.y + avail.y - ImGui::GetTextLineHeight() - Theme::S(6));
                fdl->AddText(ImVec2(tp.x + 1, tp.y + 1), IM_COL32(0, 0, 0, 220), buf);
                fdl->AddText(tp, IM_COL32(240, 240, 240, 255), buf);
            }

            // overlay
            ImDrawList* dl = ImGui::GetWindowDrawList();
            float y = p.y + Theme::S(4);
            for (auto& line : m_overlay)
            {
                if (!m_session.showPreviewInfo) break;
                dl->AddText(ImVec2(p.x + Theme::S(6) + 1, y + 1), IM_COL32(0, 0, 0, 200), line.c_str());
                dl->AddText(ImVec2(p.x + Theme::S(6), y), IM_COL32(235, 235, 235, 255), line.c_str());
                y += ImGui::GetTextLineHeight();
            }
        }

        void HandleViewInput()
        {
            ImGuiIO& io = ImGui::GetIO();
            bool hover = ImGui::IsItemHovered();
            // hold L and drag over the view to turn the light (horizontal = around, vertical = height)
            m_rotatingLight = hover && ImGui::IsKeyDown(ImGuiKey_L) && io.MouseDown[0];
            if (m_rotatingLight)
            {
                if (m_lightIndex == -1) m_opt.lightAngle += io.MouseDelta.x * 0.01f;
                else
                {
                    m_opt.lightYawDeg = std::fmod(m_opt.lightYawDeg + io.MouseDelta.x * 0.5f + 360.0f, 360.0f);
                    m_opt.lightElevDeg = std::clamp(m_opt.lightElevDeg - io.MouseDelta.y * 0.3f, -80.0f, 80.0f);
                }
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
                m_orbiting = false;
            }
            else if (hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsKeyDown(ImGuiKey_L)) m_orbiting = true;
            if (hover && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) m_panning = true;
            if (!io.MouseDown[0]) m_orbiting = false;
            if (!io.MouseDown[2]) m_panning = false;
            if (m_fpv && m_kind == Kind::Weapon) { m_orbiting = false; m_panning = false; }
            if (m_orbiting)
            {
                m_cam.yaw -= io.MouseDelta.x * 0.01f;
                m_cam.pitch = std::clamp(m_cam.pitch + io.MouseDelta.y * 0.01f, -1.5f, 1.5f);
            }
            if (m_panning)
            {
                Vec3 eye = m_cam.Eye();
                Vec3 fwd = (m_cam.target - eye).Normalized();
                Vec3 right = fwd.Cross({ 0, 0, 1 }).Normalized();
                Vec3 up = right.Cross(fwd);
                float k = m_cam.distance * 0.0015f;
                m_cam.target = m_cam.target - right * (io.MouseDelta.x * k) + up * (io.MouseDelta.y * k);
            }
            if (hover && io.MouseWheel != 0)
                m_cam.distance = std::clamp(m_cam.distance * std::pow(0.88f, io.MouseWheel), 0.01f, 100000.f);
        }

        void DrawContextMenu()
        {
            if (!ImGui::BeginPopupContextItem("##previewctx")) return;
            if (m_kind == Kind::Material)
            {
                if (ImGui::BeginMenu("Shape"))
                {
                    struct S { const char* n; Shape s; };
                    for (auto& it : { S{ "Sphere", Shape::Sphere }, S{ "Cube", Shape::Cube }, S{ "Cylinder", Shape::Cylinder },
                                      S{ "Cone", Shape::Cone }, S{ "Torus", Shape::Torus }, S{ "Plane", Shape::Plane } })
                        if (ImGui::MenuItem(it.n, nullptr, m_shape == it.s)) { m_shape = it.s; m_frameRequested = true; }
                    ImGui::EndMenu();
                }
                ImGui::Separator();
            }
            ImGui::MenuItem("Show info text", nullptr, &m_session.showPreviewInfo);
            if ((m_kind == Kind::Model && !m_modelData.bones.empty()) || (m_kind == Kind::Weapon && !m_weaponBones.empty()))
            {
                ImGui::MenuItem("Show bones", nullptr, &m_showBones);
                ImGui::MenuItem("Show bone names", nullptr, &m_showBoneNames);
                if (ImGui::BeginMenu("Bone style"))
                {
                    if (ImGui::MenuItem("3D shapes", nullptr, m_boneShapes)) m_boneShapes = true;
                    if (ImGui::MenuItem("Lines", nullptr, !m_boneShapes)) m_boneShapes = false;
                    ImGui::Separator();
                    ImGui::MenuItem("Show through the model", nullptr, &m_boneXray);
                    ImGui::EndMenu();
                }
                ImGui::Separator();
            }
            if (m_cam.firstPerson)
            {
                ImGui::SetNextItemWidth(Theme::S(170));
                ImGui::SliderFloat("Field of view", &m_fpvFovDeg, 40.0f, 120.0f, "%.0f deg");
            }
            else
            {
                ImGui::SetNextItemWidth(Theme::S(170));
                ImGui::SliderFloat("Field of view", &m_orbitFovDeg, 15.0f, 100.0f, "%.0f deg");
            }
            ImGui::Separator();
            ImGui::MenuItem("Wireframe", nullptr, &m_opt.wireframe);
            ImGui::MenuItem("Floor grid", nullptr, &m_opt.grid);
            ImGui::MenuItem("Turntable", nullptr, &m_spin);
            if (m_lightIndex == -1) ImGui::SliderFloat("Headlight angle", &m_opt.lightAngle, 0.0f, 6.28f);
            else
            {
                ImGui::SliderFloat("Light rotation", &m_opt.lightYawDeg, 0.0f, 360.0f, "%.0f deg");
                ImGui::SliderFloat("Light height", &m_opt.lightElevDeg, -80.0f, 80.0f, "%+.0f deg");
                if (ImGui::MenuItem("Reset light rotation")) { m_opt.lightYawDeg = 0; m_opt.lightElevDeg = 0; }
            }
            ImGui::TextDisabled("Hold L and drag in the view to turn the light");
            ImGui::Separator();
            if (ImGui::MenuItem("Reset camera")) m_frameRequested = true;
            ImGui::EndPopup();
        }

        // ------------------------------------------------------------------ 2D image view
        void DrawImageView(ImVec2 avail)
        {
            std::string err;
            auto tex = m_renderer.LoadTexture(m_session.previewImage, err);
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(p, ImVec2(p.x + avail.x, p.y + avail.y), IM_COL32(40, 40, 40, 255));
            ImGui::InvisibleButton("##imgview", avail, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
            if (!tex)
            {
                dl->AddText(ImVec2(p.x + 10, p.y + 10), IM_COL32(230, 120, 120, 255), err.empty() ? "Cannot load image" : err.c_str());
                return;
            }
            ImGuiIO& io = ImGui::GetIO();
            if (ImGui::IsItemHovered() && io.MouseWheel != 0) m_imgZoom = std::clamp(m_imgZoom * std::pow(1.15f, io.MouseWheel), 0.05f, 40.f);
            if (ImGui::IsItemActive() && (io.MouseDown[0] || io.MouseDown[2])) { m_imgPan.x += io.MouseDelta.x; m_imgPan.y += io.MouseDelta.y; }
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) { m_imgZoom = 1.0f; m_imgPan = { 0, 0 }; }

            float fit = std::min((avail.x - 20) / tex->width, (avail.y - 20) / tex->height);
            float scale = fit * m_imgZoom;
            ImVec2 sz(tex->width * scale, tex->height * scale);
            ImVec2 a(p.x + (avail.x - sz.x) * 0.5f + m_imgPan.x, p.y + (avail.y - sz.y) * 0.5f + m_imgPan.y);
            ImVec2 b(a.x + sz.x, a.y + sz.y);

            dl->PushClipRect(p, ImVec2(p.x + avail.x, p.y + avail.y), true);
            // checkerboard behind transparent pixels
            const float cell = Theme::S(10);
            ImVec2 ca(std::max(a.x, p.x), std::max(a.y, p.y)), cb(std::min(b.x, p.x + avail.x), std::min(b.y, p.y + avail.y));
            if (tex->hasAlpha)
                for (float y = ca.y; y < cb.y; y += cell)
                    for (float x = ca.x; x < cb.x; x += cell)
                    {
                        bool odd = ((int)((x - a.x) / cell) + (int)((y - a.y) / cell)) & 1;
                        dl->AddRectFilled(ImVec2(x, y), ImVec2(std::min(x + cell, cb.x), std::min(y + cell, cb.y)), odd ? IM_COL32(90, 90, 90, 255) : IM_COL32(60, 60, 60, 255));
                    }
            dl->AddImage((ImTextureID)(intptr_t)tex->tex.Get(), a, b);
            dl->AddRect(a, b, IM_COL32(0, 0, 0, 200));
            dl->PopClipRect();

            std::string info = m_session.previewImage.filename().string() + "   " + tex->description + "   " + std::to_string((int)std::round(scale * 100)) + "%";
            dl->AddText(ImVec2(p.x + 7, p.y + 5), IM_COL32(0, 0, 0, 220), info.c_str());
            dl->AddText(ImVec2(p.x + 6, p.y + 4), IM_COL32(235, 235, 235, 255), info.c_str());
        }

        // ------------------------------------------------------------------ scene building
        float ShapeRadius() const
        {
            static const float r[6] = { 1.0f, 1.4f, 1.15f, 1.15f, 1.05f, 1.45f };
            return r[(int)m_shape];
        }

        const GpuMesh* ShapeMesh()
        {
            auto& slot = m_shapes[(int)m_shape];
            if (!slot)
            {
                MeshData md;
                switch (m_shape)
                {
                case Shape::Sphere: md = MeshData::Sphere(); break;
                case Shape::Cube: md = MeshData::Cube(); break;
                case Shape::Cylinder: md = MeshData::Cylinder(false); break;
                case Shape::Cone: md = MeshData::Cylinder(true); break;
                case Shape::Torus: md = MeshData::Torus(); break;
                case Shape::Plane: md = MeshData::Plane(); break;
                }
                slot = m_renderer.CreateMesh(md);
            }
            return slot.get();
        }

        std::string Signature(const gdt::Entry& e) const
        {
            std::string s = std::to_string((uintptr_t)&e) + "|" + std::to_string((int)EffectiveMode());
            // A hash over every stored key and value: whatever the user edits (alpha test, filtering, tiling, a map...)
            // changes it, so the preview never shows a stale state.
            uint64_t h = 1469598103934665603ull;
            auto mix = [&](const std::string& t) { for (unsigned char c : t) { h ^= c; h *= 1099511628211ull; } h ^= 0xff; h *= 1099511628211ull; };
            for (auto& kv : e.Values()) { mix(kv.first); mix(kv.second); }
            s += "|" + std::to_string(h);
            s += "|" + std::to_string(m_session.project.Revision());   // bumped by reloads, renames and deletes
            if (false) {}
            else if (IEquals(e.GdfName(), "xanim.gdf")) s += "|" + m_skelOverride;
            return s;
        }

        void Refresh()
        {
            const gdt::Entry* e = m_session.Selected();
            bool rawModel = !m_session.previewRawModel.empty(), rawMat = !m_session.previewRawMaterial.empty();
            bool rawAnim = !m_session.previewRawAnim.empty();
            if (!e && !rawModel && !rawMat && !rawAnim)
            {
                m_kind = Kind::None; m_message.clear(); m_signature.clear();
                return;
            }
            std::string sig = rawAnim ? "rawa|" + m_session.previewRawAnim + "|" + m_skelOverride + "|" + std::to_string((int)EffectiveMode())
                            : rawModel ? "rawm|" + m_session.previewRawModel + "|" + std::to_string((int)EffectiveMode())
                            : rawMat ? "rawt|" + m_session.previewRawMaterial + "|" + std::to_string((int)EffectiveMode())
                            : Signature(*e);
            if (sig == m_signature) return;
            m_signature = sig;
            m_overlay.clear(); m_message.clear(); m_model.reset(); m_subStyles.clear(); m_modelData = MeshData{};
            m_isAnim = false; m_posedMesh = MeshData{}; m_posedBones.clear();
            if (e != m_lastEntry) { m_skelOverride.clear(); m_lastEntry = e; }

            if (rawAnim) { BuildRawAnim(m_session.previewRawAnim); m_frameRequested = true; return; }
            if (rawModel) { BuildRawModel(m_session.previewRawModel); m_frameRequested = true; return; }
            if (rawMat)
            {
                m_kind = Kind::Material;
                std::vector<std::string> notes;
                m_style = StyleFor(m_session.previewRawMaterial, nullptr, notes);
                m_overlay = notes;
                if (!m_session.game.FindMaterial(m_session.previewRawMaterial)) m_overlay.push_back("no compiled material '" + m_session.previewRawMaterial + "'");
                m_frameRequested = true;
                return;
            }
            if (IEquals(e->GdfName(), "material.gdf")) { m_kind = Kind::Material; BuildMaterial(*e); m_frameRequested = true; }
            else if (IEquals(e->GdfName(), "xmodel.gdf")) { BuildModel(*e); m_frameRequested = true; }
            else if (IEquals(e->GdfName(), "xanim.gdf")) { BuildAnim(*e); m_frameRequested = true; }
            else if (IsWeaponDef(e->GdfName())) { BuildWeapon(*e); m_frameRequested = true; }
            else
            {
                m_kind = Kind::None;
                m_message = "No preview for " + (e->Def() ? e->Def()->displayName : e->GdfName()) + " assets";
            }
        }

        // ------------------------------------------------------------------ animation
        void BuildAnim(const gdt::Entry& e)
        {
            std::string err;
            std::string rel = e.GetString("filename");
            fs::path path = rel.empty() ? fs::path{} : m_session.project.ResolveFile(rel, "xanim_export/");
            if (path.empty()) { m_kind = Kind::None; m_message = "Animation source not found:\n" + (rel.empty() ? std::string("(no filename set)") : rel) + "\n(expected under xanim_export/)"; return; }
            if (!Animation::LoadExport(path, m_anim, err)) { m_kind = Kind::None; m_message = path.filename().string() + ": " + err; return; }
            m_anim.name = e.Name();
            ShowAnimation(e.GetString("model"));
        }

        // A compiled animation of the game files (raw/xanim), shown on the best matching skeleton.
        void BuildRawAnim(const std::string& name)
        {
            std::string err;
            if (!Animation::LoadCompiled(m_session.project.Root() / "raw" / "xanim" / name, m_anim, err))
            { m_kind = Kind::None; m_message = "Cannot show compiled animation '" + name + "'\n" + err; return; }
            m_anim.name = name;
            ShowAnimation({});
        }

        void ShowAnimation(const std::string& declaredModel)
        {
            std::string err;
            // choose a skeleton: the one the asset names, else the best match by bone names
            auto& index = m_session.skeletons;
            m_candidates = index.Match(m_anim.parts);
            std::string modelName = m_skelOverride;
            if (modelName.empty())
            {
                std::string declared = NormalizeSlashes(declaredModel);
                if (!declared.empty())
                    for (auto& g : m_session.project.Gdts())
                        for (auto& x : g->Entries())
                            if (IEquals(x->GdfName(), "xmodel.gdf") && IEquals(NormalizeSlashes(x->GetString("filename")), declared)) { modelName = x->Name(); break; }
                const SkeletonIndex::Entry* ent = modelName.empty() ? nullptr : index.Find(modelName);
                int covered = 0;
                if (ent) for (auto& p : m_anim.parts) if (ent->boneSet.count(ToLower(p))) ++covered;
                if (!ent || covered * 2 < (int)m_anim.parts.size())
                {
                    modelName = m_candidates.empty() ? std::string() : m_candidates.front().entry->model;
                    if (!declared.empty()) m_overlay.push_back("model '" + declared + "' is not available compiled; using the best matching skeleton");
                }
            }
            if (modelName.empty()) { m_kind = Kind::None; m_message = "No compiled model has the bones of this animation.\nIt needs a skeleton (raw/xmodel + xmodelparts) to be shown."; return; }
            m_skeletonName = modelName;

            if (!CompiledXModel::Load(m_session.project.Root() / "raw", modelName, 0, m_modelData, err))
            { m_kind = Kind::None; m_message = "Cannot load skeleton model '" + modelName + "'\n" + err; return; }
            auto overlayHead = m_overlay; m_overlay.clear();
            FinishModel("skeleton " + modelName);
            if (m_kind != Kind::Model) return;
            m_overlay.insert(m_overlay.begin(), overlayHead.begin(), overlayHead.end());
            int bound = m_skinner.Bind(m_modelData, m_anim);
            m_overlay.push_back(std::to_string(bound) + " of " + std::to_string(m_anim.parts.size()) + " animated bones found in the skeleton; " +
                                std::to_string(m_anim.numFrames) + " frames @ " + std::to_string((int)m_anim.framerate) + " fps");
            m_isAnim = true;
            m_frame = 0; m_playing = true;
            if (m_pendingFrame >= 0) { m_frame = (float)std::min(m_pendingFrame, m_anim.numFrames - 1); m_playing = false; }
            m_posedMesh = m_modelData;
            m_skinner.Pose(m_modelData, m_anim, 0, m_loop, m_posedBones);
        }

        void AdvanceAnimation()
        {
            if (!m_isAnim || m_anim.numFrames <= 0) return;
            const bool weapon = m_kind == Kind::Weapon;
            float n = (float)m_anim.numFrames;
            if (m_playing)
            {
                m_frame += ImGui::GetIO().DeltaTime * m_anim.framerate * m_animSpeed;
                if (m_frame >= n - 1)
                {
                    if (m_loop) m_frame = std::fmod(m_frame, std::max(n - 1, 1.0f));
                    else { m_frame = n - 1; m_playing = false; }
                }
            }
            m_frame = std::clamp(m_frame, 0.0f, n - 1);
            if (weapon) { m_weapon->Pose(m_frame, m_loop); return; }
            m_skinner.Pose(m_modelData, m_anim, m_frame, m_loop, m_posedBones);
            if (m_model && !m_modelData.skin.empty())
            {
                Skinner::Skin(m_modelData, m_posedBones, m_posedMesh);
                m_renderer.UpdateMesh(*m_model, m_posedMesh);
            }
        }

        void DrawTimeline()
        {
            float ic = Theme::S(16);
            int last = std::max(m_anim.numFrames - 1, 0);
            int frame = (int)m_frame;
            if (IconButton("tl_first", Icon::Prev, ic, "Previous frame")) { m_playing = false; m_frame = (float)std::max(frame - 1, 0); }
            ImGui::SameLine(0, 2);
            if (IconButton("tl_play", m_playing ? Icon::Pause : Icon::Play, ic, m_playing ? "Pause" : "Play", m_playing))
            {
                if (!m_playing && m_frame >= (float)last - 0.01f) m_frame = 0;
                m_playing = !m_playing;
            }
            ImGui::SameLine(0, 2);
            if (IconButton("tl_next", Icon::Next, ic, "Next frame")) { m_playing = false; m_frame = (float)std::min(frame + 1, last); }
            ImGui::SameLine(0, 2);
            if (IconButton("tl_loop", Icon::Loop, ic, "Loop", m_loop)) m_loop = !m_loop;
            ImGui::SameLine(0, Theme::S(8));

            const float tail = Theme::S(190);
            ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x - tail, Theme::S(60)));
            ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, Theme::S(8));
            if (ImGui::SliderInt("##frame", &frame, 0, last, "%d")) { m_frame = (float)frame; m_playing = false; }
            ImGui::PopStyleVar();
            ImVec2 smin = ImGui::GetItemRectMin(), smax = ImGui::GetItemRectMax();
            for (auto& n : m_anim.notes)   // notetrack keys appear as ticks under the scrubber
            {
                float x = smin.x + (smax.x - smin.x) * (last ? (float)n.frame / last : 0.0f);
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddTriangleFilled(ImVec2(x - 3, smax.y + 5), ImVec2(x + 3, smax.y + 5), ImVec2(x, smax.y), Palette::Accent);
                if (ImGui::IsMouseHoveringRect(ImVec2(x - 4, smax.y - 2), ImVec2(x + 4, smax.y + 7))) ImGui::SetTooltip("Frame %d: %s", n.frame, n.name.c_str());
            }
            ImGui::SameLine(0, Theme::S(6));
            ImGui::SetNextItemWidth(Theme::S(54));
            const float speeds[] = { 0.25f, 0.5f, 1.0f, 2.0f };
            char sp[16]; std::snprintf(sp, sizeof(sp), "%gx", m_animSpeed);
            if (ImGui::BeginCombo("##animspeed", sp))
            {
                for (float v : speeds) { char b[16]; std::snprintf(b, sizeof(b), "%gx", v); if (ImGui::Selectable(b, v == m_animSpeed)) m_animSpeed = v; }
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Playback speed");
            if (!m_anim.notes.empty())
            {
                ImGui::SameLine(0, Theme::S(6));
                if (ImGui::Button("Notes")) ImGui::OpenPopup("##notes");
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Notetracks of this animation");
                if (ImGui::BeginPopup("##notes"))
                {
                    ImGui::TextDisabled("%zu notetracks - click to jump", m_anim.notes.size());
                    for (auto& n : m_anim.notes)
                    {
                        char b[200];
                        std::snprintf(b, sizeof(b), "%4d   %s", n.frame, n.name.c_str());
                        if (ImGui::Selectable(b, (int)m_frame == n.frame)) { m_frame = (float)std::clamp(n.frame, 0, last); m_playing = false; }
                    }
                    ImGui::EndPopup();
                }
            }
            if (m_kind == Kind::Weapon) goto skip_skeleton;
            ImGui::SameLine(0, Theme::S(6));
            if (ImGui::Button("Skeleton...")) ImGui::OpenPopup("##skelpick");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Model whose skeleton is used: %s", m_skeletonName.c_str());
            if (ImGui::BeginPopup("##skelpick"))
            {
                ImGui::TextDisabled("Models whose bones match this animation (%zu parts)", m_anim.parts.size());
                for (auto& c : m_candidates)
                {
                    char b[256];
                    std::snprintf(b, sizeof(b), "%s   (%d/%zu matched, %d other bones)", c.entry->model.c_str(), c.covered, m_anim.parts.size(), c.extra);
                    if (ImGui::Selectable(b, c.entry->model == m_skeletonName)) { m_skelOverride = c.entry->model; m_signature.clear(); }
                }
                ImGui::EndPopup();
            }
            skip_skeleton:;
        }

        // ------------------------------------------------------------------ weapons
        // Finds the animation for a weapon: an xanim asset of that name (its xanim_export source) first.
        bool ResolveAnimation(const std::string& name, Animation& out, std::string& err)
        {
            if (auto* xe = m_session.project.FindEntry("xanim.gdf", name))
            {
                std::string rel = xe->GetString("filename");
                auto path = rel.empty() ? fs::path{} : m_session.project.ResolveFile(rel, "xanim_export/");
                if (!path.empty() && Animation::LoadExport(path, out, err)) { out.name = name; return true; }
                if (err.empty()) err = "export file not found";
            }
            // not a GDT asset: the compiled animation of the game files
            fs::path compiled = m_session.project.Root() / "raw" / "xanim" / name;
            std::error_code ec;
            if (fs::is_regular_file(compiled, ec)) { std::string cerr; if (Animation::LoadCompiled(compiled, out, cerr)) { out.name = name; err.clear(); return true; } err = cerr; return false; }
            if (err.empty()) err = "no xanim asset or compiled animation named '" + name + "'";
            return false;
        }

        void SelectWeaponAnim(const std::string& name)
        {
            if (name == "-" || name.empty())
            {
                m_isAnim = false; m_weapon->SetAnimation(nullptr, {}); m_weaponAnimError.clear();
                return;
            }
            std::string err;
            if (!ResolveAnimation(name, m_anim, err))
            {
                m_isAnim = false; m_weapon->SetAnimation(nullptr, {});
                m_weaponAnimError = "Animation '" + name + "' cannot be shown: " + err;
                return;
            }
            m_weaponAnimError.clear();
            m_weapon->SetAnimation(&m_anim, name);
            m_isAnim = true; m_frame = 0; m_playing = true;
            m_weapon->Pose(0, m_loop);
        }

        void BuildWeapon(const gdt::Entry& e)
        {
            m_weapon->Load(e, EffectiveMode());
            m_kind = Kind::Weapon;
            m_weaponAnimError.clear();
            for (auto& line : m_weapon->Overlay()) m_overlay.push_back(line);
            // start on the idle animation when it can be shown
            m_isAnim = false;
            for (auto& a : m_weapon->Anims())
                if (a.field == "idleAnim") { SelectWeaponAnim(a.name); break; }
            if (!m_pendingWeaponAnim.empty())
                for (auto& a : m_weapon->Anims())
                    if (a.field == m_pendingWeaponAnim || a.name == m_pendingWeaponAnim) { SelectWeaponAnim(a.name); break; }
            if (m_pendingFrame >= 0 && m_isAnim) { m_frame = (float)std::min(m_pendingFrame, m_anim.numFrames - 1); m_playing = false; }
        }

        void DrawWeaponControls()
        {
            bool modelsChanged = false;
            std::string picked = m_weapon->DrawControls(modelsChanged);
            ImGui::SameLine(0, Theme::S(6));
            if (ImGui::Checkbox("HUD", &m_showHud)) {}
            if (modelsChanged) m_frameRequested = true;
            if (!picked.empty()) SelectWeaponAnim(picked);
            if (auto* open = m_weapon->TakeOpenRequest()) m_session.Open(open);
            if (!m_weaponAnimError.empty()) ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.4f, 1), "%s", m_weaponAnimError.c_str());
        }

        const std::vector<Bone>& ActiveBones() const
        {
            if (m_kind == Kind::Weapon) return m_weaponBones;
            return m_isAnim ? m_posedBones : m_modelData.bones;
        }

        // Octahedral bone shapes (wide near the parent joint, tapering to the child), coloured by depth in the hierarchy.
        void BuildBoneShapes(OverlayGeometry& ov)
        {
            const std::vector<Bone>& bones = ActiveBones();
            ov.xray = m_boneXray;
            const Vec3 light = Vec3{ 0.35f, 0.45f, 0.82f }.Normalized();
            std::vector<int> depth(bones.size(), 0);
            int maxDepth = 1;
            for (size_t i = 0; i < bones.size(); ++i)
            {
                int p = bones[i].parent;
                depth[i] = (p >= 0 && p < (int)i) ? depth[p] + 1 : 0;
                maxDepth = std::max(maxDepth, depth[i]);
            }
            auto hsv = [](float h, float s, float v) {
                float r = 0, g = 0, b = 0;
                int i = (int)(h * 6) % 6; float f = h * 6 - std::floor(h * 6), p = v * (1 - s), q = v * (1 - f * s), t = v * (1 - (1 - f) * s);
                switch (i) { case 0: r = v; g = t; b = p; break; case 1: r = q; g = v; b = p; break; case 2: r = p; g = v; b = t; break;
                             case 3: r = p; g = q; b = v; break; case 4: r = t; g = p; b = v; break; default: r = v; g = p; b = q; break; }
                return Vec3{ r, g, b };
            };
            auto pack = [](Vec3 c, float shade, int a) {
                auto ch = [&](float v) { return (DWORD)std::clamp((int)std::lround(v * shade * 255.0f), 0, 255); };
                return (DWORD)(a << 24) | (ch(c.x) << 16) | (ch(c.y) << 8) | ch(c.z);
            };
            for (size_t i = 0; i < bones.size(); ++i)
            {
                int pi = bones[i].parent;
                if (pi < 0 || pi >= (int)bones.size()) continue;
                Vec3 head = bones[pi].worldPos, tip = bones[i].worldPos;
                Vec3 axis = tip - head;
                float len = axis.Length();
                if (len < 1e-4f) continue;
                Vec3 d = axis * (1.0f / len);
                Vec3 ref = std::fabs(d.z) < 0.9f ? Vec3{ 0, 0, 1 } : Vec3{ 1, 0, 0 };
                Vec3 u = d.Cross(ref).Normalized(), v = d.Cross(u).Normalized();
                float subjectR = m_kind == Kind::Weapon ? m_weapon->Radius() : m_modelData.Radius();
                float r = std::min(len * 0.14f, subjectR * 0.03f + 0.001f);
                Vec3 ringC = head + d * (len * 0.18f);
                Vec3 ring[4] = { ringC + u * r, ringC + v * r, ringC - u * r, ringC - v * r };
                Vec3 base = hsv(std::fmod(0.58f + 0.42f * (float)depth[pi] / (float)maxDepth, 1.0f), 0.55f, 0.95f);
                auto tri = [&](const Vec3& a, const Vec3& b, const Vec3& c) {
                    Vec3 n = (b - a).Cross(c - a).Normalized();
                    float shade = 0.45f + 0.55f * std::fabs(n.Dot(light));
                    DWORD col = pack(base, shade, m_boneXray ? 215 : 255);
                    ov.tris.push_back({ a.x, a.y, a.z, col }); ov.tris.push_back({ b.x, b.y, b.z, col }); ov.tris.push_back({ c.x, c.y, c.z, col });
                };
                for (int k = 0; k < 4; ++k)
                {
                    tri(head, ring[k], ring[(k + 1) % 4]);   // the short wide end
                    tri(tip, ring[(k + 1) % 4], ring[k]);    // the long taper
                }
                DWORD edge = 0xC8101018;
                auto line = [&](const Vec3& a, const Vec3& b) { ov.lines.push_back({ a.x, a.y, a.z, edge }); ov.lines.push_back({ b.x, b.y, b.z, edge }); };
                for (int k = 0; k < 4; ++k) { line(head, ring[k]); line(tip, ring[k]); line(ring[k], ring[(k + 1) % 4]); }
            }
        }

        // Projects the skeleton onto the 3D view: lines to the parents, joint dots, optional names.
        void DrawBones(ImVec2 origin, ImVec2 size)
        {
            const std::vector<Bone>& bones = ActiveBones();
            if (bones.empty()) return;
            Mat4 vp = m_cam.View() * m_cam.Proj(size.x / std::max(size.y, 1.0f));
            auto project = [&](const Vec3& p, ImVec2& out) {
                float x = p.x * vp.m[0][0] + p.y * vp.m[1][0] + p.z * vp.m[2][0] + vp.m[3][0];
                float y = p.x * vp.m[0][1] + p.y * vp.m[1][1] + p.z * vp.m[2][1] + vp.m[3][1];
                float w = p.x * vp.m[0][3] + p.y * vp.m[1][3] + p.z * vp.m[2][3] + vp.m[3][3];
                if (w <= 1e-4f) return false;
                out = ImVec2(origin.x + (x / w * 0.5f + 0.5f) * size.x, origin.y + (1.0f - (y / w * 0.5f + 0.5f)) * size.y);
                return true;
            };
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);
            const ImU32 line = IM_COL32(255, 196, 64, 255), dot = IM_COL32(255, 240, 160, 255);
            for (size_t i = 0; i < bones.size() && m_showBones && !m_boneShapes; ++i)
            {
                ImVec2 a;
                if (!project(bones[i].worldPos, a)) continue;
                int par = bones[i].parent;
                ImVec2 b;
                if (par >= 0 && par < (int)bones.size() && project(bones[par].worldPos, b))
                {
                    dl->AddLine(b, a, IM_COL32(0, 0, 0, 200), Theme::S(3.5f));
                    dl->AddLine(b, a, line, Theme::S(1.8f));
                }
                dl->AddCircleFilled(a, Theme::S(3.2f), IM_COL32(0, 0, 0, 220));
                dl->AddCircleFilled(a, Theme::S(2.2f), dot);
            }
            if (m_showBoneNames)
                for (auto& b : bones)
                {
                    ImVec2 a;
                    if (!project(b.worldPos, a)) continue;
                    ImVec2 t(a.x + Theme::S(6), a.y - ImGui::GetTextLineHeight() * 0.5f);
                    dl->AddText(ImVec2(t.x + 1, t.y + 1), IM_COL32(0, 0, 0, 230), b.name.c_str());
                    dl->AddText(t, IM_COL32(255, 238, 190, 255), b.name.c_str());
                }
            dl->PopClipRect();
        }

        // ------------------------------------------------------------------ environment (lighting + sky box)
        const std::vector<std::string>& SkyNames()
        {
            if (m_skyNames.empty() && m_session.iwds.Available())
            {
                m_skyNames = m_session.iwds.ImagesWithSuffix("_ft");
                std::sort(m_skyNames.begin(), m_skyNames.end());
            }
            return m_skyNames;
        }

        void ApplyPreset(int index)
        {
            const auto& p = EnvironmentPresets()[index];
            m_opt.lightYawDeg = 0; m_opt.lightElevDeg = 0;
            m_lightIndex = -2;   // custom (not one of the stock maps)
            m_opt.light = MakeLightSetup(p.lighting);
            m_opt.lighting = true;
            ApplySky(p.sky);
            m_signature.clear();
            m_presetIndex = index;
        }

        void ApplyLight(int index)
        {
            m_presetIndex = -1;
            const auto& all = StockLightings();
            if (index < 0 || index >= (int)all.size())
            {
                m_lightIndex = -1;
                m_opt.light = LightSetup{};
                m_opt.light.headlight = true;
                m_opt.light.sunColor = { 0.85f, 0.83f, 0.80f };
                m_opt.light.ambient = { 0.20f, 0.20f, 0.23f };
            }
            else { m_lightIndex = index; m_opt.light = MakeLightSetup(all[index]); }
            m_opt.lighting = true;
            m_signature.clear();
        }

        void ApplySky(const std::string& name)
        {
            if (m_skyName != name && m_presetIndex >= 0 && name != EnvironmentPresets()[m_presetIndex].sky) m_presetIndex = -1;
            m_skyName = name;
            m_opt.sky.reset();
            if (name.empty()) return;
            auto it = m_skyCache.find(name);
            if (it == m_skyCache.end())
            {
                std::vector<unsigned char> bytes;
                std::string err;
                std::shared_ptr<GpuCube> cube;
                if (m_session.iwds.ReadImage(name, bytes)) cube = m_renderer.CreateCube(name, bytes, err);
                it = m_skyCache.emplace(name, std::move(cube)).first;
            }
            m_opt.sky = it->second;
        }

        void EnsureEnvironment()
        {
            if (m_envInit || !m_session.iwds.Available()) return;
            m_envInit = true;
            const auto& presets = EnvironmentPresets();
            int def = 0;
            for (int i = 0; i < (int)presets.size(); ++i) if (std::string(presets[i].name) == "Day") def = i;
            ApplyPreset(def);
        }

        // "No Lighting" shows the raw material: the game's unlit technique, or no lights in the built-in path.
        ShadeMode EffectiveMode() const { return (!m_opt.lighting && m_mode != ShadeMode::Fixed) ? ShadeMode::Unlit : m_mode; }

        SurfaceStyle StyleFor(const std::string& name, const gdt::Entry* mat, std::vector<std::string>& notes)
        {
            auto r = m_shading.Build(name, mat, EffectiveMode());
            for (auto& n : r.notes) notes.push_back(n);
            if (!r.techset.empty() && r.style.shader) notes.insert(notes.begin(), "techset " + r.techset);
            return r.style;
        }

        void BuildMaterial(const gdt::Entry& e)
        {
            std::vector<std::string> notes;
            m_style = StyleFor(e.Name(), &e, notes);
            m_overlay = notes;
        }

        void BuildModel(const gdt::Entry& e)
        {
            // Prefer the editable XMODEL_EXPORT source; fall back to the compiled model under raw/.
            std::string rel = e.GetString("filename");
            std::string err, source;
            bool loaded = false;
            if (!rel.empty())
            {
                auto path = m_session.project.ResolveFile(rel, "model_export/");
                if (!path.empty())
                {
                    loaded = XModelExportLoader::Load(path, m_modelData, err);
                    if (loaded) source = path.filename().string();
                }
                else err = "source not found: " + rel;
            }
            if (!loaded)
            {
                std::string cerr;
                fs::path raw = m_session.project.Root() / "raw";
                loaded = CompiledXModel::Load(raw, e.Name(), 0, m_modelData, cerr);
                if (loaded) source = "compiled raw/xmodel/" + e.Name();
                else err += (err.empty() ? "" : "\n") + cerr;
            }
            if (!loaded) { m_kind = Kind::None; m_message = "No model to show for '" + e.Name() + "'\n" + err; return; }

            FinishModel(source);
        }

        void BuildRawModel(const std::string& name)
        {
            std::string err;
            if (!CompiledXModel::Load(m_session.project.Root() / "raw", name, 0, m_modelData, err))
            {
                m_kind = Kind::None; m_message = "Cannot show compiled model '" + name + "'\n" + err; return;
            }
            FinishModel("compiled raw/xmodel/" + name);
        }

        void FinishModel(const std::string& source)
        {
            m_model = m_renderer.CreateMesh(m_modelData);
            if (!m_model) { m_kind = Kind::None; m_message = "Could not upload model"; return; }
            m_kind = Kind::Model;
            std::vector<std::string> notes;
            for (auto& sm : m_modelData.submeshes)
            {
                gdt::Entry* mat = m_session.project.FindEntry("material.gdf", sm.material);
                m_subStyles.push_back(StyleFor(sm.material, mat, notes));
            }
            m_overlay.push_back(source + ": " + std::to_string(m_modelData.vertices.size()) + " verts, " + std::to_string(m_modelData.TriangleCount()) +
                                " tris, " + std::to_string(m_modelData.submeshes.size()) + " surfaces, " + std::to_string(m_modelData.boneCount) + " bones");
            std::sort(notes.begin(), notes.end());
            notes.erase(std::unique(notes.begin(), notes.end()), notes.end());
            for (size_t i = 0; i < notes.size() && i < 5; ++i) m_overlay.push_back(notes[i]);
        }

        Renderer& m_renderer;
        Session& m_session;
        RenderTarget m_target;
        OrbitCamera m_cam;
        SceneOptions m_opt;
        MaterialShading m_shading;
        ShadeMode m_mode = ShadeMode::Sun;
        Kind m_kind = Kind::None;
        Shape m_shape = Shape::Sphere;
        std::unique_ptr<GpuMesh> m_shapes[6];
        std::unique_ptr<GpuMesh> m_model;
        MeshData m_modelData;
        SurfaceStyle m_style;
        std::vector<SurfaceStyle> m_subStyles;
        std::vector<std::string> m_overlay;
        std::string m_message, m_signature;
        bool m_frameRequested = true, m_spin = false, m_orbiting = false, m_panning = false;
        float m_spinSpeed = 1.0f;
        float m_imgZoom = 1.0f;
        ImVec2 m_imgPan{ 0, 0 };

        // skeleton overlay
        float m_zoom = 0, m_zoomUp = 0, m_pitchOverride = -10;
        std::unique_ptr<WeaponScene> m_weapon;
        bool m_showHud = true;
        std::string m_weaponAnimError;
        bool m_envInit = false, m_rotatingLight = false;
        int m_lightIndex = -1, m_presetIndex = -1;
        std::string m_skyName;
        std::vector<std::string> m_skyNames;
        std::map<std::string, std::shared_ptr<GpuCube>> m_skyCache;
        int m_pendingFrame = -1;
        std::string m_pendingWeaponAnim;
        bool m_fpv = false;
        float m_fpvFovDeg = 90.0f, m_orbitFovDeg = 40.0f;
        std::vector<Bone> m_weaponBones;
        bool m_showBones = false, m_showBoneNames = false, m_boneShapes = true, m_boneXray = true;

        // animation playback
        bool m_isAnim = false, m_playing = true, m_loop = true;
        float m_frame = 0, m_animSpeed = 1.0f;
        Animation m_anim;
        Skinner m_skinner;
        MeshData m_posedMesh;
        std::vector<Bone> m_posedBones;
        std::vector<SkeletonIndex::Candidate> m_candidates;
        std::string m_skeletonName, m_skelOverride;
        const gdt::Entry* m_lastEntry = nullptr;
    };
}
