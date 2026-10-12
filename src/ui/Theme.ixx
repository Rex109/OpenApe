module;

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <string>
#include "imgui.h"

export module openape.ui.theme;

// Dark "Qt Fusion"-flavoured look modelled on the Asset Property Editor.
export namespace oa::ui
{
    constexpr float kHalfPi = 1.57079632679f;

    struct Palette
    {
        static constexpr ImU32 Window     = IM_COL32(0x31, 0x31, 0x31, 255);
        static constexpr ImU32 Panel      = IM_COL32(0x2b, 0x2b, 0x2b, 255);
        static constexpr ImU32 Header     = IM_COL32(0x3a, 0x3a, 0x3a, 255);
        static constexpr ImU32 Field      = IM_COL32(0x3d, 0x3d, 0x3d, 255);
        static constexpr ImU32 FieldHot   = IM_COL32(0x47, 0x47, 0x47, 255);
        static constexpr ImU32 Border     = IM_COL32(0x22, 0x22, 0x22, 255);
        static constexpr ImU32 Text       = IM_COL32(0xe2, 0xe2, 0xe2, 255);
        static constexpr ImU32 TextDim    = IM_COL32(0x9a, 0x9a, 0x9a, 255);
        static inline ImU32 Accent        = IM_COL32(0xf2, 0x9b, 0x24, 255);   // the only colour the theme selector changes
        static inline ImU32 AccentDim     = IM_COL32(0xb9, 0x75, 0x1a, 255);
        static inline ImU32 AccentSoft    = IM_COL32(0xf2, 0x9b, 0x24, 70);    // translucent fill for toggled-on buttons
        static constexpr ImU32 ListBg     = IM_COL32(0x6c, 0x6c, 0x6c, 255);
        static constexpr ImU32 ListAlt    = IM_COL32(0x66, 0x66, 0x66, 255);
        static constexpr ImU32 ListText   = IM_COL32(0xf4, 0xf4, 0xf4, 255);
        static constexpr ImU32 Link       = IM_COL32(0x58, 0x9d, 0xf2, 255);
    };

    inline ImVec4 V4(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

    // Accent themes: named after studios. Everything else in the look stays the same.
    struct AccentTheme { const char* name; unsigned char r, g, b; };
    inline const std::array<AccentTheme, 7>& AccentThemes()
    {
        static const std::array<AccentTheme, 7> t = { {
            { "Treyarch",           0xf2, 0x9b, 0x24 },   // orange (default)
            { "Infinity Ward",      0x6c, 0xc2, 0x4a },   // green
            { "High Moon",          0xf0, 0xd0, 0x2a },   // yellow
            { "Sledgehammer",       0x3d, 0x8f, 0xe8 },   // blue
            { "Raven",              0xe0, 0x4a, 0x4a },   // red
            { "Beenox",             0xa8, 0x6c, 0xe6 },   // purple
            { "Vicarious Visions",  0x2c, 0xc4, 0xb8 },   // teal
        } };
        return t;
    }

    class Theme
    {
    public:
        static inline ImFont* regular = nullptr;
        static inline ImFont* bold = nullptr;
        static inline float scale = 1.0f;

        static float S(float v) { return std::round(v * scale); }

        static void Apply(float dpiScale)
        {
            scale = dpiScale;
            ImGuiIO& io = ImGui::GetIO();
            io.Fonts->Clear();
            ImFontConfig cfg;
            cfg.OversampleH = 2; cfg.OversampleV = 1;
            const float px = std::round(15.0f * dpiScale);
            regular = AddFont("segoeui.ttf", px, cfg);
            bold = AddFont("segoeuib.ttf", px, cfg);
            if (!regular) regular = io.Fonts->AddFontDefault();
            if (!bold) bold = regular;
            io.FontDefault = regular;

            ImGuiStyle& s = ImGui::GetStyle();
            s = ImGuiStyle();
            s.WindowRounding = 0; s.ChildRounding = 0; s.FrameRounding = S(2); s.PopupRounding = S(2);
            s.ScrollbarRounding = S(3); s.GrabRounding = S(2); s.TabRounding = S(2);
            s.WindowBorderSize = 0; s.ChildBorderSize = 1; s.FrameBorderSize = 1; s.PopupBorderSize = 1;
            s.WindowPadding = { S(6), S(6) };
            s.FramePadding = { S(5), S(3) };
            s.ItemSpacing = { S(6), S(5) };
            s.ItemInnerSpacing = { S(5), S(4) };
            s.ScrollbarSize = S(10);
            s.GrabMinSize = S(10);
            s.IndentSpacing = S(16);
            s.WindowMenuButtonPosition = ImGuiDir_None;

            ApplyColors();
        }

        static inline int accentIndex = 0;

        // Switches the accent colour (index into AccentThemes()) and re-applies the colours of the style.
        static void SetAccent(int index)
        {
            const auto& t = AccentThemes();
            accentIndex = std::clamp(index, 0, (int)t.size() - 1);
            const auto& a = t[accentIndex];
            Palette::Accent = IM_COL32(a.r, a.g, a.b, 255);
            Palette::AccentDim = IM_COL32(a.r * 3 / 4, a.g * 3 / 4, a.b * 3 / 4, 255);
            Palette::AccentSoft = IM_COL32(a.r, a.g, a.b, 70);
            if (ImGui::GetCurrentContext()) ApplyColors();
        }

        static void ApplyColors()
        {
            ImVec4* c = ImGui::GetStyle().Colors;
            c[ImGuiCol_Text]                 = V4(Palette::Text);
            c[ImGuiCol_TextDisabled]         = V4(Palette::TextDim);
            c[ImGuiCol_WindowBg]             = V4(Palette::Window);
            c[ImGuiCol_ChildBg]              = V4(Palette::Window);
            c[ImGuiCol_PopupBg]              = V4(IM_COL32(0x35, 0x35, 0x35, 250));
            c[ImGuiCol_Border]               = V4(Palette::Border);
            c[ImGuiCol_FrameBg]              = V4(Palette::Field);
            c[ImGuiCol_FrameBgHovered]       = V4(Palette::FieldHot);
            c[ImGuiCol_FrameBgActive]        = V4(IM_COL32(0x2f, 0x2f, 0x2f, 255));
            c[ImGuiCol_TitleBg]              = V4(Palette::Header);
            c[ImGuiCol_TitleBgActive]        = V4(Palette::Header);
            c[ImGuiCol_MenuBarBg]            = V4(IM_COL32(0x2a, 0x2a, 0x2a, 255));
            c[ImGuiCol_ScrollbarBg]          = V4(IM_COL32(0x2a, 0x2a, 0x2a, 255));
            c[ImGuiCol_ScrollbarGrab]        = V4(Palette::AccentDim);
            c[ImGuiCol_ScrollbarGrabHovered] = V4(Palette::Accent);
            c[ImGuiCol_ScrollbarGrabActive]  = V4(Palette::Accent);
            c[ImGuiCol_CheckMark]            = V4(Palette::Accent);
            c[ImGuiCol_SliderGrab]           = V4(Palette::Accent);
            c[ImGuiCol_SliderGrabActive]     = V4(Palette::Accent);
            c[ImGuiCol_Button]               = V4(IM_COL32(0x44, 0x44, 0x44, 255));
            c[ImGuiCol_ButtonHovered]        = V4(IM_COL32(0x55, 0x55, 0x55, 255));
            c[ImGuiCol_ButtonActive]         = V4(Palette::AccentDim);
            c[ImGuiCol_Header]               = V4(IM_COL32(0x4a, 0x4a, 0x4a, 255));
            c[ImGuiCol_HeaderHovered]        = V4(IM_COL32(0x5a, 0x5a, 0x5a, 255));
            c[ImGuiCol_HeaderActive]         = V4(Palette::AccentDim);
            c[ImGuiCol_Separator]            = V4(Palette::Border);
            c[ImGuiCol_Tab]                  = V4(IM_COL32(0x35, 0x35, 0x35, 255));
            c[ImGuiCol_TabHovered]           = V4(IM_COL32(0x4a, 0x4a, 0x4a, 255));
            c[ImGuiCol_TabSelected]          = V4(IM_COL32(0x45, 0x45, 0x45, 255));
            c[ImGuiCol_TabSelectedOverline]  = V4(Palette::Accent);
            c[ImGuiCol_TextSelectedBg]       = V4((Palette::Accent & 0x00FFFFFFu) | (110u << IM_COL32_A_SHIFT));
            c[ImGuiCol_NavCursor]            = V4(Palette::Accent);
            c[ImGuiCol_ModalWindowDimBg]     = V4(IM_COL32(0, 0, 0, 120));
        }

    private:
        static ImFont* AddFont(const char* file, float px, const ImFontConfig& cfg)
        {
            std::error_code ec;
            std::filesystem::path p = std::filesystem::path("C:\\Windows\\Fonts") / file;
            if (!std::filesystem::exists(p, ec)) return nullptr;
            return ImGui::GetIO().Fonts->AddFontFromFileTTF(p.string().c_str(), px, &cfg);
        }
    };

    enum class Icon
    {
        New, Open, Save, Undo, Redo, Back, Forward, Refresh, Star, StarFilled, Lock, Search, Bulb, Camera, Wire, Grid,
        Play, Pause, Prev, Next, Funnel, Copy, Sync, Table, Close, Folder, Plus, Minus,
        Sphere, Cube, Cylinder, Cone, Torus, Plane, Pencil, Trash, Document, Bone, Tag, Loop, Info, Eye
    };

    // Small vector icons drawn with the draw list so no icon font / image assets are needed.
    inline void DrawIcon(ImDrawList* dl, Icon icon, ImVec2 p, float sz, ImU32 col, ImU32 col2 = 0)
    {
        const float u = sz / 16.0f;
        auto P = [&](float x, float y) { return ImVec2(p.x + x * u, p.y + y * u); };
        const float th = std::max(1.0f, 1.5f * u);
        if (!col2) col2 = col;
        switch (icon)
        {
        case Icon::New:
            dl->AddRectFilled(P(3, 1.5f), P(13, 14.5f), IM_COL32(240, 240, 240, 255), 1);
            dl->AddRect(P(3, 1.5f), P(13, 14.5f), IM_COL32(90, 90, 90, 255), 1);
            dl->AddCircleFilled(P(11, 4), 3 * u, IM_COL32(255, 190, 40, 255));
            break;
        case Icon::Open:
            dl->AddRectFilled(P(1.5f, 4), P(14.5f, 13.5f), IM_COL32(230, 190, 90, 255), 1);
            dl->AddRectFilled(P(1.5f, 2.5f), P(6.5f, 5), IM_COL32(210, 165, 60, 255), 1);
            dl->AddRectFilled(P(3, 6.5f), P(15, 13.5f), IM_COL32(250, 220, 130, 255), 1);
            break;
        case Icon::Save:
            dl->AddRectFilled(P(2, 2), P(14, 14), IM_COL32(110, 120, 210, 255), 1.5f);
            dl->AddRectFilled(P(4.5f, 2), P(11.5f, 6.5f), IM_COL32(235, 235, 245, 255));
            dl->AddRectFilled(P(4, 9), P(12, 14), IM_COL32(60, 65, 120, 255));
            break;
        case Icon::Undo:
        case Icon::Redo:
        {
            // a U-turn arrow: the arc runs from the lower right over the top to the left end, which carries the head
            const bool undo = icon == Icon::Undo;
            auto X = [&](float x) { return undo ? x : 16.0f - x; };
            const float cx = 8.0f, cy = 9.0f, r = 4.6f;
            const float a0 = 0.45f, a1 = -3.14159265f - 0.25f;
            for (int i = 0; i <= 24; ++i)
            {
                float a = a0 + (a1 - a0) * (float)i / 24.0f;
                dl->PathLineTo(P(X(cx + r * std::cos(a)), cy + r * std::sin(a)));
            }
            dl->PathStroke(col, th * 1.7f);
            // head at the end of the arc, pointing along its direction of travel
            float tx = std::sin(a1), ty = -std::cos(a1);
            float ex = cx + r * std::cos(a1), ey = cy + r * std::sin(a1);
            float nx = -ty, ny = tx, hl = 3.6f, hw = 3.2f;
            dl->AddTriangleFilled(P(X(ex + tx * hl), ey + ty * hl), P(X(ex + nx * hw), ey + ny * hw), P(X(ex - nx * hw), ey - ny * hw), col);
            break;
        }
        case Icon::Back:
        case Icon::Forward:
        {
            float d = icon == Icon::Back ? 1.f : -1.f;
            auto X = [&](float x) { return d > 0 ? x : 16 - x; };
            dl->AddCircleFilled(P(8, 8), 7 * u, IM_COL32(70, 140, 235, 255));
            dl->AddCircle(P(8, 8), 7 * u, IM_COL32(200, 225, 255, 255), 0, std::max(1.f, u));
            // the mirrored icon must keep a consistent winding and ordered rectangle corners, or the fill renders garbled
            if (d > 0) dl->AddTriangleFilled(P(3.5f, 8), P(8, 4), P(8, 12), IM_COL32(255, 255, 255, 255));
            else dl->AddTriangleFilled(P(12.5f, 8), P(8, 12), P(8, 4), IM_COL32(255, 255, 255, 255));
            dl->AddRectFilled(P(std::min(X(8), X(12.5f)), 6.6f), P(std::max(X(8), X(12.5f)), 9.4f), IM_COL32(255, 255, 255, 255));
            break;
        }
        case Icon::Refresh:
            dl->PathArcTo(P(8, 8), 5 * u, 0.5f, 5.4f, 20);
            dl->PathStroke(IM_COL32(70, 140, 235, 255), th * 1.6f);
            dl->AddTriangleFilled(P(11.5f, 1.5f), P(14.5f, 6), P(8.5f, 6), IM_COL32(70, 140, 235, 255));
            break;
        case Icon::Star:
        case Icon::StarFilled:
        {
            ImVec2 pts[10];
            for (int i = 0; i < 10; ++i)
            {
                float a = -kHalfPi + i * 3.14159265f / 5, r = (i % 2 == 0 ? 7.f : 3.0f) * u;
                pts[i] = ImVec2(p.x + 8 * u + std::cos(a) * r, p.y + 8.6f * u + std::sin(a) * r);
            }
            if (icon == Icon::StarFilled) dl->AddConvexPolyFilled(pts, 10, IM_COL32(255, 200, 40, 255));
            else dl->AddPolyline(pts, 10, col, std::max(1.f, u), ImDrawFlags_Closed);
            break;
        }
        case Icon::Lock:
            dl->AddRectFilled(P(3.5f, 7), P(12.5f, 14), IM_COL32(235, 185, 60, 255), 1);
            dl->PathArcTo(P(8, 7), 3 * u, 3.14159265f, 6.2832f, 12);
            dl->PathStroke(IM_COL32(200, 200, 200, 255), th);
            break;
        case Icon::Search:
            dl->AddCircle(P(6.5f, 6.5f), 4.2f * u, col, 0, th * 1.3f);
            dl->AddLine(P(9.7f, 9.7f), P(14, 14), col, th * 1.6f);
            break;
        case Icon::Bulb:
            dl->AddCircleFilled(P(8, 6.5f), 4.6f * u, IM_COL32(255, 225, 90, 255));
            dl->AddRectFilled(P(6, 10.5f), P(10, 14), IM_COL32(190, 190, 190, 255), 1);
            break;
        case Icon::Camera:
            dl->AddRectFilled(P(1.5f, 4.5f), P(14.5f, 13), IM_COL32(205, 205, 205, 255), 2);
            dl->AddRectFilled(P(5, 2.5f), P(10, 5), IM_COL32(205, 205, 205, 255));
            dl->AddCircleFilled(P(8, 8.8f), 3 * u, IM_COL32(60, 60, 60, 255));
            dl->AddCircleFilled(P(8, 8.8f), 1.6f * u, IM_COL32(120, 160, 220, 255));
            break;
        case Icon::Wire:
            dl->AddRect(P(2.5f, 4.5f), P(11.5f, 13.5f), col, 0.0f, th);
            dl->AddRect(P(5, 2.5f), P(13.5f, 11), col, 0.0f, th);
            dl->AddLine(P(2.5f, 4.5f), P(5, 2.5f), col, th); dl->AddLine(P(11.5f, 13.5f), P(13.5f, 11), col, th);
            break;
        case Icon::Grid:
            for (int i = 0; i < 4; ++i)
            {
                dl->AddLine(P(2 + i * 4, 2), P(2 + i * 4, 14), col, std::max(1.f, u));
                dl->AddLine(P(2, 2 + i * 4), P(14, 2 + i * 4), col, std::max(1.f, u));
            }
            break;
        case Icon::Play:  dl->AddTriangleFilled(P(4, 2.5f), P(13, 8), P(4, 13.5f), col); break;
        case Icon::Pause:
            dl->AddRectFilled(P(3.5f, 3), P(7, 13), col); dl->AddRectFilled(P(9, 3), P(12.5f, 13), col); break;
        case Icon::Prev:
            dl->AddRectFilled(P(2.5f, 3), P(4.5f, 13), col); dl->AddTriangleFilled(P(13, 3), P(13, 13), P(5.5f, 8), col); break;
        case Icon::Next:
            dl->AddRectFilled(P(11.5f, 3), P(13.5f, 13), col); dl->AddTriangleFilled(P(3, 3), P(3, 13), P(10.5f, 8), col); break;
        case Icon::Funnel:
            dl->AddTriangleFilled(P(1.5f, 2.5f), P(14.5f, 2.5f), P(8, 9.5f), IM_COL32(70, 140, 235, 255));
            dl->AddRectFilled(P(6.5f, 8), P(9.5f, 14), IM_COL32(70, 140, 235, 255));
            break;
        case Icon::Copy:
            dl->AddRectFilled(P(2, 2), P(10, 11), IM_COL32(230, 230, 230, 255), 1);
            dl->AddRectFilled(P(6, 5), P(14, 14), IM_COL32(180, 205, 245, 255), 1);
            dl->AddRect(P(6, 5), P(14, 14), IM_COL32(70, 100, 170, 255), 1);
            break;
        case Icon::Sync:
            dl->AddTriangleFilled(P(4, 1.5f), P(7.5f, 7), P(0.5f, 7), IM_COL32(90, 220, 80, 255));
            dl->AddRectFilled(P(3, 7), P(5, 14), IM_COL32(90, 220, 80, 255));
            dl->AddTriangleFilled(P(12, 14.5f), P(8.5f, 9), P(15.5f, 9), IM_COL32(90, 220, 80, 255));
            dl->AddRectFilled(P(11, 2), P(13, 9), IM_COL32(90, 220, 80, 255));
            break;
        case Icon::Table:
            dl->AddRectFilled(P(1.5f, 2), P(14.5f, 14), IM_COL32(40, 150, 80, 255), 1.5f);
            for (int i = 1; i < 3; ++i)
            {
                dl->AddLine(P(1.5f + i * 4.33f, 2), P(1.5f + i * 4.33f, 14), IM_COL32(255, 255, 255, 220), 1);
                dl->AddLine(P(1.5f, 2 + i * 4), P(14.5f, 2 + i * 4), IM_COL32(255, 255, 255, 220), 1);
            }
            break;
        case Icon::Close:
            dl->AddLine(P(3.5f, 3.5f), P(12.5f, 12.5f), col, th * 1.4f); dl->AddLine(P(12.5f, 3.5f), P(3.5f, 12.5f), col, th * 1.4f); break;
        case Icon::Folder:
            dl->AddRectFilled(P(1.5f, 3.5f), P(14.5f, 13), IM_COL32(235, 195, 95, 255), 1);
            break;
        case Icon::Plus:
            dl->AddLine(P(8, 3), P(8, 13), col, th * 1.4f); dl->AddLine(P(3, 8), P(13, 8), col, th * 1.4f); break;
        case Icon::Minus:
            dl->AddLine(P(3, 8), P(13, 8), col, th * 1.4f); break;
        case Icon::Sphere:
            dl->AddCircleFilled(P(8, 8), 6.5f * u, col);
            dl->AddCircleFilled(P(6, 6), 2.6f * u, col2);
            break;
        case Icon::Cube:
        {
            ImVec2 top[4] = { P(8, 1.5f), P(14, 4.5f), P(8, 7.5f), P(2, 4.5f) };
            ImVec2 left[4] = { P(2, 4.5f), P(8, 7.5f), P(8, 14.5f), P(2, 11.5f) };
            ImVec2 right[4] = { P(8, 7.5f), P(14, 4.5f), P(14, 11.5f), P(8, 14.5f) };
            dl->AddConvexPolyFilled(top, 4, col2); dl->AddConvexPolyFilled(left, 4, col); dl->AddConvexPolyFilled(right, 4, IM_COL32(((col >> 0) & 255) * 7 / 10, ((col >> 8) & 255) * 7 / 10, ((col >> 16) & 255) * 7 / 10, 255));
            break;
        }
        case Icon::Cylinder:
            dl->AddRectFilled(P(3, 4), P(13, 12), col);
            dl->AddEllipseFilled(P(8, 12), ImVec2(5 * u, 2.4f * u), col);
            dl->AddEllipseFilled(P(8, 4), ImVec2(5 * u, 2.4f * u), col2);
            break;
        case Icon::Cone:
            dl->AddTriangleFilled(P(8, 1.5f), P(14, 12), P(2, 12), col);
            dl->AddEllipseFilled(P(8, 12), ImVec2(6 * u, 2.4f * u), col2);
            break;
        case Icon::Torus:
            dl->AddCircle(P(8, 8), 5 * u, col, 0, 3.6f * u);
            break;
        case Icon::Plane:
        {
            ImVec2 q[4] = { P(4, 4), P(15, 4), P(12, 12), P(1, 12) };
            dl->AddConvexPolyFilled(q, 4, col);
            break;
        }
        case Icon::Pencil:
            dl->AddTriangleFilled(P(2, 14), P(3, 10), P(6, 13), col);
            dl->AddQuadFilled(P(3.5f, 9.5f), P(11, 2), P(14, 5), P(6.5f, 12.5f), IM_COL32(240, 190, 70, 255));
            break;
        case Icon::Trash:
            dl->AddRectFilled(P(3.5f, 5), P(12.5f, 14), col, 1);
            dl->AddRectFilled(P(2.5f, 3), P(13.5f, 4.6f), col);
            break;
        case Icon::Bone:
            dl->AddLine(P(4.5f, 11.5f), P(11.5f, 4.5f), col, th * 1.3f);
            dl->AddCircleFilled(P(3.6f, 11.2f), 2.1f * u, col); dl->AddCircleFilled(P(5.0f, 12.8f), 2.1f * u, col);
            dl->AddCircleFilled(P(12.4f, 4.8f), 2.1f * u, col); dl->AddCircleFilled(P(11.0f, 3.2f), 2.1f * u, col);
            break;
        case Icon::Tag:
            dl->AddRectFilled(P(1.5f, 4), P(14.5f, 12), col, 2 * u);
            dl->AddRectFilled(P(3.5f, 6), P(9.5f, 7.2f), IM_COL32(40, 40, 40, 255)); dl->AddRectFilled(P(3.5f, 8.8f), P(12, 10), IM_COL32(40, 40, 40, 255));
            break;
        case Icon::Loop:
            dl->PathArcTo(P(8, 8), 4.6f * u, 0.4f, 5.6f, 18); dl->PathStroke(col, th * 1.4f);
            dl->AddTriangleFilled(P(9.5f, 1.8f), P(13.2f, 5.6f), P(8.0f, 5.8f), col);
            break;
        case Icon::Eye:
            dl->PathArcTo(P(8, 12.2f), 7.2f * u, -2.35f, -0.79f, 14); dl->PathStroke(col, th * 1.4f);
            dl->PathArcTo(P(8, 3.8f), 7.2f * u, 0.79f, 2.35f, 14); dl->PathStroke(col, th * 1.4f);
            dl->AddCircleFilled(P(8, 8), 2.4f * u, col);
            break;
        case Icon::Info:
            dl->AddCircleFilled(P(8, 8), 6.6f * u, col);
            dl->AddCircleFilled(P(8, 4.8f), 1.1f * u, IM_COL32(40, 40, 40, 255));
            dl->AddRectFilled(P(7.1f, 7), P(8.9f, 12), IM_COL32(40, 40, 40, 255));
            break;
        case Icon::Document:
            dl->AddRectFilled(P(3, 1.5f), P(13, 14.5f), IM_COL32(235, 235, 235, 255), 1);
            for (int i = 0; i < 4; ++i) dl->AddLine(P(5, 5 + i * 2.2f), P(11, 5 + i * 2.2f), IM_COL32(120, 120, 120, 255), 1);
            break;
        }
    }

    // Icon button with hover/pressed background. Returns true when clicked.
    inline bool IconButton(const char* id, Icon icon, float size, const char* tooltip = nullptr, bool active = false, ImU32 col = Palette::Text)
    {
        ImGui::PushID(id);
        ImVec2 p = ImGui::GetCursorScreenPos();
        float box = size + Theme::S(8);
        bool clicked = ImGui::InvisibleButton("##ib", ImVec2(box, box));
        bool hot = ImGui::IsItemHovered(), down = ImGui::IsItemActive();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (hot || down || active)
            dl->AddRectFilled(p, ImVec2(p.x + box, p.y + box), down ? IM_COL32(0, 0, 0, 90) : active ? Palette::AccentSoft : IM_COL32(255, 255, 255, 28), Theme::S(2));
        const int firstVtx = dl->VtxBuffer.Size;
        DrawIcon(dl, icon, ImVec2(p.x + Theme::S(4), p.y + Theme::S(4)), size, col);
        if (ImGui::GetStyle().Alpha < 1.0f)   // disabled: the fixed icon colours must fade too
        {
            const float k = ImGui::GetStyle().Alpha;
            for (int i = firstVtx; i < dl->VtxBuffer.Size; ++i)
            {
                ImU32 c = dl->VtxBuffer[i].col;
                ImU32 a = (ImU32)(((c >> IM_COL32_A_SHIFT) & 0xFF) * k);
                dl->VtxBuffer[i].col = (c & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
            }
        }
        if (tooltip && hot) ImGui::SetTooltip("%s", tooltip);
        ImGui::PopID();
        return clicked;
    }
}
