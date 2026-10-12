module;

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <d3d9.h>
#include "ImGuiWin32Fwd.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "misc/cpp/imgui_stdlib.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx9.h"


export module openape.app;

import openape.util;
import openape.gdf;
import openape.gdt;
import openape.project;
import openape.session;
import openape.version;
import openape.update;
import openape.platform;
import openape.renderer;
import openape.ui.theme;
import openape.ui.preview;
import openape.ui.properties;
import openape.ui.assets;

export namespace oa
{
    class Application
    {
    public:
        int Run(int argc, char** argv)
        {
            ImGui_ImplWin32_EnableDpiAwareness();
            if (!CreateMainWindow()) return 1;

            fs::path exeDir = ExeDirectory();
            m_session.LoadSettings(exeDir / "openape.cfg");

            if (!m_renderer.Init(m_hwnd, 1280, 800))
            {
                MessageBoxA(nullptr, "Could not create a Direct3D 9 device.", "OpenApe", MB_ICONERROR);
                return 1;
            }

            IMGUI_CHECKVERSION();
            ImGui::CreateContext();
            ImGuiIO& io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
            float dpi = ImGui_ImplWin32_GetDpiScaleForHwnd(m_hwnd);
            ui::Theme::Apply(dpi);
            ui::Theme::SetAccent(m_session.accentTheme);
            ImGui_ImplWin32_Init(m_hwnd);
            ImGui_ImplDX9_Init(m_renderer.Device());

            m_preview = std::make_unique<ui::PreviewPanel>(m_renderer, m_session);
            m_props = std::make_unique<ui::PropertyEditor>(m_renderer, m_session, m_hwnd);
            m_assets = std::make_unique<ui::AssetsPanel>(m_session, m_hwnd);
            m_filters = std::make_unique<ui::FiltersPanel>(m_session);
            WireCallbacks();

            for (int i = 1; i < argc; ++i)
                if (std::string(argv[i]) == "--fpv") m_preview->SetFirstPerson(true); else if (std::string(argv[i]) == "--bones") m_preview->SetBones(false); else if (std::string(argv[i]) == "--names") m_preview->SetBones(true);
            for (int i = 1; i + 1 < argc; ++i)
            {
                std::string a = argv[i];
                if (a == "--screenshot") m_shotPath = argv[i + 1];
                else if (a == "--select") m_shotSelect = argv[i + 1];
                else if (a == "--shape") m_preview->SetShape(std::atoi(argv[i + 1]));
                else if (a == "--shade") m_preview->SetMode(std::atoi(argv[i + 1]));
                else if (a == "--frame") m_preview->SetFrame(std::atoi(argv[i + 1]));
                else if (a == "--light") m_preview->SetLight(std::atoi(argv[i + 1]));
                else if (a == "--lookup") m_preview->SetLookUp((float)std::atof(argv[i + 1]));
                else if (a == "--pitch") m_preview->SetPitch((float)std::atof(argv[i + 1]));
                else if (a == "--preset") m_preview->SetPreset(std::atoi(argv[i + 1]));
                else if (a == "--sky") m_preview->SetSky(argv[i + 1]);
                else if (a == "--theme") { m_session.accentTheme = std::atoi(argv[i + 1]); ui::Theme::SetAccent(m_session.accentTheme); }
                else if (a == "--zoom") m_preview->SetZoom((float)std::atof(argv[i + 1]));
                else if (a == "--edit") m_testEdit = argv[i + 1];
                else if (a == "--log") m_testLog = argv[i + 1];
                else if (a == "--click") std::sscanf(argv[i + 1], "%d,%d", &m_testClickX, &m_testClickY);
                else if (a == "--raw-model") m_shotRawModel = argv[i + 1];
                else if (a == "--image") m_shotImage = argv[i + 1];
                else if (a == "--raw-material") m_shotRawMat = argv[i + 1];
                else if (a == "--raw-anim") m_shotRawAnim = argv[i + 1];
                else if (a == "--then") m_testThen = argv[i + 1];
                else if (a == "--weapon-anim") m_preview->SetWeaponAnim(argv[i + 1]);
            }
            OpenInitialProject(argc, argv, exeDir);
            m_session.previewRawModel = m_shotRawModel; m_session.previewRawMaterial = m_shotRawMat; m_session.previewRawAnim = m_shotRawAnim; m_session.previewImage = m_shotImage;
            if (!m_shotSelect.empty())
                for (auto& g : m_session.project.Gdts())
                    if (auto* e = g->Find(m_shotSelect)) { m_session.Open(e); break; }

            if (m_session.checkUpdates && m_shotPath.empty()) m_update.Start();   // not during scripted screenshot runs
            ShowWindow(m_hwnd, SW_SHOWMAXIMIZED);
            UpdateWindow(m_hwnd);

            MSG msg{};
            while (!m_quit)
            {
                while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
                {
                    TranslateMessage(&msg);
                    DispatchMessage(&msg);
                    if (msg.message == WM_QUIT) m_quit = true;
                }
                if (m_quit) break;
                Frame();
            }

            m_session.SaveSettings();
            m_preview.reset();
            ImGui_ImplDX9_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
            m_renderer.Shutdown();
            DestroyWindow(m_hwnd);
            UnregisterClassA("OpenApeWindow", GetModuleHandle(nullptr));
            return 0;
        }

    private:
        // ------------------------------------------------------------------ window
        static fs::path ExeDirectory()
        {
            char buf[MAX_PATH];
            GetModuleFileNameA(nullptr, buf, MAX_PATH);
            return fs::path(buf).parent_path();
        }

        bool CreateMainWindow()
        {
            WNDCLASSEXA wc{};
            wc.cbSize = sizeof(wc);
            wc.style = CS_CLASSDC;
            wc.lpfnWndProc = &Application::WndProc;
            wc.hInstance = GetModuleHandle(nullptr);
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            wc.lpszClassName = "OpenApeWindow";
            wc.hIcon = (HICON)LoadImageA(wc.hInstance, "IDI_ICON1", IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED);
            wc.hIconSm = (HICON)LoadImageA(wc.hInstance, "IDI_ICON1", IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED);
            RegisterClassExA(&wc);
            m_hwnd = CreateWindowExA(0, wc.lpszClassName, "Asset Property Editor (OpenApe)", WS_OVERLAPPEDWINDOW, 100, 100, 1500, 900,
                                     nullptr, nullptr, wc.hInstance, this);
            if (m_hwnd)
            {
                BOOL dark = TRUE;
                DwmSetWindowAttribute(m_hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
            }
            return m_hwnd != nullptr;
        }

        static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
        {
            if (msg == WM_NCCREATE)
                SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCT*)lp)->lpCreateParams);
            auto* app = (Application*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
            if (ImGui::GetCurrentContext() && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return true;
            switch (msg)
            {
            case WM_ENTERSIZEMOVE:
                if (app) app->m_inSizeMove = true;
                return 0;
            case WM_EXITSIZEMOVE:
                if (app) app->m_inSizeMove = false;
                return 0;
            case WM_SIZE:
                if (app && wp != SIZE_MINIMIZED)
                {
                    app->m_width = LOWORD(lp); app->m_height = HIWORD(lp); app->m_renderer.RequestReset();
                    // Windows runs its own message loop while the window is being dragged larger or smaller, so the main loop
                    // does not get to draw: render from here to follow the resize live
                    if (app->m_inSizeMove) app->Frame();
                }
                return 0;
            case WM_SYSCOMMAND:
                if ((wp & 0xfff0) == SC_KEYMENU) return 0;
                break;
            case WM_CLOSE:
                if (app && app->m_session.project.AnyDirty()) { app->m_modal = Modal::Quit; app->m_modalOpen = true; return 0; }
                if (app) app->m_quit = true;
                return 0;
            case WM_DESTROY:
                PostQuitMessage(0);
                return 0;
            }
            return DefWindowProcA(hwnd, msg, wp, lp);
        }

        // ------------------------------------------------------------------ project
        void OpenInitialProject(int argc, char** argv, const fs::path& exeDir)
        {
            std::vector<fs::path> candidates;
            if (argc > 1) candidates.push_back(argv[1]);
            if (!m_session.lastRoot.empty()) candidates.push_back(m_session.lastRoot);
            candidates.push_back(Project::FindRoot(exeDir));
            candidates.push_back(Project::FindRoot(fs::current_path()));
            for (auto& c : candidates)
            {
                if (c.empty()) continue;
                fs::path root = Project::FindRoot(c);
                if (!root.empty() && m_session.OpenRoot(root)) { AfterProjectLoad(); return; }
            }
            m_session.Notify("No game tree found - use File > Open Project Folder (a folder that contains 'deffiles').", 60);
        }

        void AfterProjectLoad()
        {
            m_assets->ForceRebuild();
            m_session.SaveSettings();
            // definition files we do not understand (e.g. the sample input.gdf) are not worth a banner
            std::vector<std::string> problems;
            for (auto& e : m_session.project.Errors()) if (e.find(".gdf:") == std::string::npos) problems.push_back(e);
            if (!problems.empty())
                m_session.Notify(std::to_string(problems.size()) + " problem(s) while loading; first: " + problems.front(), 15);
        }

        void OpenProjectDialog()
        {
            fs::path dir = platform::PickFolder(m_hwnd, "Select the game tree (the folder containing 'deffiles')");
            if (dir.empty()) return;
            fs::path root = Project::FindRoot(dir);
            if (root.empty()) { Message("No 'deffiles' folder found in or above:\n" + dir.string()); return; }
            if (m_session.OpenRoot(root)) AfterProjectLoad();
        }

        void ReloadAll()
        {
            int skipped = 0;
            auto nav = m_session.SnapshotNav();
            for (auto& g : m_session.project.Gdts())
            {
                if (g->Dirty()) { ++skipped; continue; }
                m_session.project.ReloadGdt(*g);
            }
            // entries were recreated: find the selected assets and the history again by name
            m_session.undo.Clear();
            m_session.RestoreNav(nav);
            m_appliedTab = -2;
            m_assets->ForceRebuild();
            m_session.Notify(skipped ? "Reloaded; " + std::to_string(skipped) + " file(s) with unsaved changes were skipped" : "Reloaded all GDT files");
        }

        void SaveEntryFile()
        {
            auto* e = m_session.Selected();
            if (!e) return;
            auto err = e->Owner()->Save();
            m_session.Notify(err.empty() ? "Saved " + e->Owner()->Name() + ".gdt" : err);
            if (!err.empty()) Message(err);
        }

        void SaveAll()
        {
            auto errs = m_session.project.SaveAll();
            if (errs.empty()) m_session.Notify("Saved all changes");
            else { std::string m; for (auto& e : errs) m += e + "\n"; Message(m); }
        }

        void RunConvert(const char* tool, bool single)
        {
            std::string args;
            if (single)
            {
                auto* e = m_session.Selected();
                if (!e) { Message("No asset selected"); return; }
                auto err = e->Owner()->Dirty() ? e->Owner()->Save() : std::string();
                if (!err.empty()) { Message(err); return; }
                args = "\"" + e->Owner()->Path().string() + "\" \"" + e->Name() + "\"";
            }
            else
            {
                auto errs = m_session.project.SaveAll();
                if (!errs.empty()) { Message(errs.front()); return; }
            }
            auto err = m_session.project.RunTool(tool, args);
            if (!err.empty()) Message("Run External Command Error\n\n" + err);
            else m_session.Notify(std::string("Started ") + tool);
        }

        // ------------------------------------------------------------------ callbacks / dialogs
        enum class Modal { None, NewEntry, Duplicate, Rename, Delete, Quit, Message, About, Compiled };

        void Message(const std::string& text) { m_modal = Modal::Message; m_modalText = text; m_modalOpen = true; }

        void WireCallbacks()
        {
            auto& cb = m_assets->cb;
            cb.newEntry = [this](gdt::GdtFile* g) { BeginNewEntry(g); };
            cb.duplicate = [this](gdt::Entry* e) { m_target = e; m_nameBuf = m_session.UniqueName(e->Name(), e->GdfName()); m_modal = Modal::Duplicate; m_modalOpen = true; };
            cb.rename = [this](gdt::Entry* e) { m_target = e; m_nameBuf = e->Name(); m_modal = Modal::Rename; m_modalOpen = true; };
            cb.remove = [this](gdt::Entry* e) { m_target = e; m_modal = Modal::Delete; m_modalOpen = true; };
            cb.reloadAll = [this] { ReloadAll(); };
            cb.exportCsv = [this] { ExportCsv(); };
        }

        void BeginNewEntry(gdt::GdtFile* g)
        {
            if (m_session.project.Gdts().empty() || m_session.project.Defs().empty()) { Message("Open a project first."); return; }
            m_newGdt = g ? g : m_session.project.Gdts().front().get();
            if (auto* sel = m_session.Selected(); sel && sel->Def()) m_newType = sel->GdfName();
            else for (auto& d : m_session.project.Defs()) if (d->root) { m_newType = d->fileName; break; }
            m_nameBuf = "new_asset";
            m_modal = Modal::NewEntry; m_modalOpen = true;
        }

        void ExportCsv()
        {
            fs::path out = platform::PickSaveFile(m_hwnd, "CSV (*.csv)\0*.csv\0", "csv", "Export assets");
            if (out.empty()) return;
            std::string csv = "name,type,gdt\n";
            for (auto& g : m_session.project.Gdts())
                for (auto& e : g->Entries())
                    if (!m_session.filter.Active() || m_session.filter.Matches(*e, m_session.IsFavorite(*e)))
                        csv += e->Name() + "," + e->GdfName() + "," + g->Name() + "\n";
            if (!WriteTextFile(out, csv)) Message("Could not write " + out.string());
            else m_session.Notify("Exported " + out.filename().string());
        }

        void DrawModals()
        {
            if (m_modalOpen) { ImGui::OpenPopup("##modal"); m_modalOpen = false; }
            ImVec2 centre = ImGui::GetMainViewport()->GetCenter();
            ImGui::SetNextWindowPos(centre, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
            const char* titles[] = { "", "New Asset", "Duplicate Asset", "Rename Asset", "Delete Asset", "Unsaved changes", "Asset Manager", "About OpenApe", "Compiled Assets (raw)" };
            std::string title = std::string(titles[(int)m_modal]) + "##modal";
            if (m_modal == Modal::None) return;
            ImGui::SetNextWindowSizeConstraints(ImVec2(ui::Theme::S(380), 0), ImVec2(ui::Theme::S(640), ui::Theme::S(600)));
            if (!ImGui::BeginPopupModal("##modal", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) return;

            ImGui::PushFont(ui::Theme::bold, 0.0f);
            ImGui::TextUnformatted(titles[(int)m_modal]);
            ImGui::PopFont();
            ImGui::Separator();
            ImGui::Spacing();
            const float w = ui::Theme::S(90);

            switch (m_modal)
            {
            case Modal::NewEntry:
            {
                ImGui::SetNextItemWidth(ui::Theme::S(320));
                if (ImGui::BeginCombo("Type", m_newType.c_str()))
                {
                    for (auto& d : m_session.project.Defs())
                        if (d->root && ImGui::Selectable((d->displayName + "  (" + d->fileName + ")").c_str(), d->fileName == m_newType)) m_newType = d->fileName;
                    ImGui::EndCombo();
                }
                ImGui::SetNextItemWidth(ui::Theme::S(320));
                if (ImGui::BeginCombo("GDT file", m_newGdt ? m_newGdt->Name().c_str() : ""))
                {
                    for (auto& g : m_session.project.Gdts()) if (ImGui::Selectable(g->Name().c_str(), g.get() == m_newGdt)) m_newGdt = g.get();
                    ImGui::EndCombo();
                }
                ImGui::SetNextItemWidth(ui::Theme::S(320));
                if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
                bool enter = ImGui::InputText("Name", &m_nameBuf, ImGuiInputTextFlags_EnterReturnsTrue);
                std::string err = ValidateName(m_nameBuf, m_newType, nullptr);
                if (!err.empty()) ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.4f, 1), "%s", err.c_str());
                ImGui::Spacing();
                if ((ImGui::Button("Create", ImVec2(w, 0)) || enter) && err.empty() && m_newGdt)
                {
                    auto* e = m_session.AddEntry(*m_newGdt, m_nameBuf, m_newType);
                    m_session.Open(e);
                    m_assets->ForceRebuild();
                    ImGui::CloseCurrentPopup(); m_modal = Modal::None;
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(w, 0))) { ImGui::CloseCurrentPopup(); m_modal = Modal::None; }
                break;
            }
            case Modal::Duplicate:
            case Modal::Rename:
            {
                ImGui::SetNextItemWidth(ui::Theme::S(320));
                if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
                bool enter = ImGui::InputText("Name", &m_nameBuf, ImGuiInputTextFlags_EnterReturnsTrue);
                std::string err = m_target ? ValidateName(m_nameBuf, m_target->GdfName(), m_modal == Modal::Rename ? m_target : nullptr) : "No asset";
                if (!err.empty()) ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.4f, 1), "%s", err.c_str());
                ImGui::Spacing();
                if ((ImGui::Button("OK", ImVec2(w, 0)) || enter) && err.empty())
                {
                    if (m_modal == Modal::Rename) m_session.RenameEntry(*m_target, m_nameBuf);
                    else m_session.Open(m_session.DuplicateEntry(*m_target, m_nameBuf));
                    m_assets->ForceRebuild();
                    ImGui::CloseCurrentPopup(); m_modal = Modal::None;
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(w, 0))) { ImGui::CloseCurrentPopup(); m_modal = Modal::None; }
                break;
            }
            case Modal::Delete:
                ImGui::Text("Are you sure you want to delete the entry '%s'?", m_target ? m_target->Name().c_str() : "");
                ImGui::TextDisabled("This can be undone with Ctrl+Z until you quit.");
                ImGui::Spacing();
                if (ImGui::Button("Delete", ImVec2(w, 0)) && m_target)
                {
                    m_session.DeleteEntry(*m_target);
                    m_target = nullptr; m_assets->ForceRebuild();
                    ImGui::CloseCurrentPopup(); m_modal = Modal::None;
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(w, 0))) { ImGui::CloseCurrentPopup(); m_modal = Modal::None; }
                break;
            case Modal::Quit:
                ImGui::TextUnformatted("Some GDT files have unsaved changes.");
                ImGui::Spacing();
                if (ImGui::Button("Save and quit", ImVec2(w * 1.5f, 0)))
                {
                    auto errs = m_session.project.SaveAll();
                    if (errs.empty()) m_quit = true;
                    else { m_modal = Modal::Message; m_modalText = errs.front(); }
                }
                ImGui::SameLine();
                if (ImGui::Button("Discard", ImVec2(w, 0))) m_quit = true;
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(w, 0))) { ImGui::CloseCurrentPopup(); m_modal = Modal::None; }
                break;
            case Modal::Message:
                ImGui::PushTextWrapPos(ui::Theme::S(520));
                ImGui::TextUnformatted(m_modalText.c_str());
                ImGui::PopTextWrapPos();
                ImGui::Spacing();
                if (ImGui::Button("OK", ImVec2(w, 0))) { ImGui::CloseCurrentPopup(); m_modal = Modal::None; }
                break;
            case Modal::Compiled:
                DrawCompiledPicker(w);
                break;
            case Modal::About:
                ImGui::TextUnformatted("OpenApe - an open reimplementation of the Call of Duty 4 Asset Manager.");
                ImGui::TextDisabled("Dear ImGui + Direct3D 9. Reads deffiles/*.gdf and *.gdt like the original tool.");
                ImGui::Spacing();
                ImGui::Text("Version %s", version::Describe().c_str());
                {
                    auto upd = m_update.Get();
                    if (upd.state == UpdateChecker::State::Available) ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.45f, 1), "Version %s is available", upd.latest.c_str());
                    else if (upd.state == UpdateChecker::State::UpToDate) ImGui::TextDisabled("Up to date (latest release: %s)", upd.latest.c_str());
                }
                ImGui::Spacing();
                if (ImGui::Button("OK", ImVec2(w, 0))) { ImGui::CloseCurrentPopup(); m_modal = Modal::None; }
                break;
            default: break;
            }
            ImGui::EndPopup();
        }

        // Lists compiled models/materials under raw/ so any of them can be previewed without a GDT entry.
        void DrawCompiledPicker(float w)
        {
            if (m_rawLists.empty())
            {
                for (auto sub : { "xmodel", "materials", "xanim" })
                {
                    std::vector<std::string> names;
                    std::error_code ec;
                    for (auto& de : fs::directory_iterator(m_session.project.Root() / "raw" / sub, ec))
                        if (de.is_regular_file()) names.push_back(de.path().filename().string());
                    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) { return ToLower(a) < ToLower(b); });
                    m_rawLists.push_back(std::move(names));
                }
            }
            if (ImGui::BeginTabBar("##rawtabs"))
            {
                const char* tabs[] = { "Models", "Materials", "Animations" };
                for (int t = 0; t < 3; ++t)
                    if (ImGui::BeginTabItem(tabs[t])) { m_rawTab = t; ImGui::EndTabItem(); }
                ImGui::EndTabBar();
            }
            ImGui::SetNextItemWidth(ui::Theme::S(420));
            if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
            ImGui::InputTextWithHint("##rawfilter", "Filter", &m_rawFilter);
            auto& list = m_rawLists[m_rawTab];
            std::vector<const std::string*> shown;
            for (auto& n : list) if (m_rawFilter.empty() || IContains(n, m_rawFilter)) shown.push_back(&n);
            if (ImGui::BeginListBox("##rawlist", ImVec2(ui::Theme::S(420), ui::Theme::S(320))))
            {
                ImGuiListClipper clip;
                clip.Begin((int)shown.size());
                while (clip.Step())
                    for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i)
                    {
                        const std::string& n = *shown[i];
                        if (ImGui::Selectable(n.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick))
                        {
                            m_session.previewRawModel.clear(); m_session.previewRawMaterial.clear(); m_session.previewRawAnim.clear(); m_session.previewImage.clear();
                            (m_rawTab == 0 ? m_session.previewRawModel : m_rawTab == 1 ? m_session.previewRawMaterial : m_session.previewRawAnim) = n;
                            if (ImGui::IsMouseDoubleClicked(0)) { ImGui::CloseCurrentPopup(); m_modal = Modal::None; }
                        }
                    }
                ImGui::EndListBox();
            }
            ImGui::TextDisabled("%zu of %zu shown - click to preview, double-click to close", shown.size(), list.size());
            if (ImGui::Button("Close", ImVec2(w, 0))) { ImGui::CloseCurrentPopup(); m_modal = Modal::None; }
        }

        std::string ValidateName(const std::string& name, const std::string& gdfName, const gdt::Entry* self) const
        {
            if (name.empty()) return "Empty string not allowed";
            if (name.size() >= 64) return "Name is too long";
            if (name.find_first_of("\"") != std::string::npos) return "Name cannot contain quotes";
            auto* existing = m_session.project.FindEntry(gdfName, name);
            if (existing && existing != self) return "'" + name + "' already exists";
            return {};
        }

        // ------------------------------------------------------------------ frame
        void Frame()
        {
            if (m_inFrame) return;   // a modal dialog or the resize loop pumped messages while a frame was being built
            struct Guard { bool& f; Guard(bool& x) : f(x) { f = true; } ~Guard() { f = false; } } guard(m_inFrame);
            auto lost = [this] { ImGui_ImplDX9_InvalidateDeviceObjects(); m_preview->OnDeviceLost(); };
            auto reset = [this] { ImGui_ImplDX9_CreateDeviceObjects(); m_preview->OnDeviceReset(); };
            if (!m_renderer.EnsureDevice(m_width, m_height, lost, reset)) return;

            ImGui_ImplDX9_NewFrame();
            ImGui_ImplWin32_NewFrame();
            TestHooks();
            ImGui::NewFrame();

            DrawMain();
            DrawModals();
            UpdateTitle();

            ImGui::EndFrame();
            auto* dev = m_renderer.Device();
            dev->SetRenderState(D3DRS_ZENABLE, FALSE);
            dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
            dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
            dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_RGBA(0x31, 0x31, 0x31, 255), 1.0f, 0);
            if (dev->BeginScene() >= 0)
            {
                ImGui::Render();
                ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
                dev->EndScene();
            }
            if (!m_shotPath.empty() && ++m_frameCount == 12)
            {
                m_renderer.SaveBackBuffer(m_shotPath);
                m_quit = true;
            }
            HRESULT hr = dev->Present(nullptr, nullptr, nullptr, nullptr);
            if (hr == D3DERR_DEVICELOST) m_renderer.RequestReset();
        }

        // Scripted input for automated checks: --edit key=value, --click x,y (device pixels), --log file
        void TestHooks()
        {
            if (m_shotPath.empty()) return;
            ImGuiIO& io = ImGui::GetIO();
            int f = m_frameCount;
            if (f == 3 && !m_testEdit.empty())
                if (auto* e = m_session.Selected())
                {
                    size_t eq = m_testEdit.find('=');
                    m_session.SetValue(*e, m_testEdit.substr(0, eq), m_testEdit.substr(eq + 1));
                }
            if (f == 3 && !m_testThen.empty())
                for (auto& g : m_session.project.Gdts())
                    if (auto* e = g->Find(m_testThen)) { m_session.Open(e); break; }
            if (m_testClickX >= 0)
            {
                if (f == 5) io.AddMousePosEvent((float)m_testClickX, (float)m_testClickY);
                if (f == 7) io.AddMouseButtonEvent(0, true);
                if (f == 9) io.AddMouseButtonEvent(0, false);
            }
            if (f == 11 && !m_testLog.empty())
            {
                std::string out = "can-undo=" + std::to_string((int)m_session.undo.CanUndo()) + " can-redo=" + std::to_string((int)m_session.undo.CanRedo()) +
                                  " tabs=" + std::to_string(m_session.tabs.size()) + " back=" + std::to_string(m_session.backStack.size()) + " fwd=" + std::to_string(m_session.forwardStack.size());
                if (auto* e = m_session.Selected()) out += " selected=" + e->Name() + " usage=" + e->GetString("usage");
                WriteTextFile(m_testLog, out + "\n");
            }
        }

        void UpdateTitle()
        {
            std::string t = std::string("OpenApe ") + version::kVersion + "  |  Asset Property Editor  |  " + version::BuildLabel();
            auto upd = m_update.Get();
            if (upd.state == UpdateChecker::State::Available) t += "  |  update available: " + upd.latest;
            if (m_session.project.IsOpen()) t += "  |  " + m_session.project.Root().string();
            if (m_session.project.AnyDirty()) t += " *";
            if (upd.state != m_lastUpdateState)
            {
                m_lastUpdateState = upd.state;
                if (upd.state == UpdateChecker::State::Available && !m_updateAnnounced)
                {
                    m_updateAnnounced = true;
                    m_session.Notify("Version " + upd.latest + " is available (you have " + version::kVersion + ") - see Help > Check for updates", 10);
                }
                else if (m_updateManual && (upd.state == UpdateChecker::State::UpToDate || upd.state == UpdateChecker::State::NoRelease || upd.state == UpdateChecker::State::Failed))
                {
                    m_updateManual = false;
                    m_session.Notify(upd.state == UpdateChecker::State::UpToDate ? "OpenApe " + std::string(version::kVersion) + " is up to date"
                                   : upd.state == UpdateChecker::State::NoRelease ? "No release has been published yet"
                                   : "Update check failed: " + upd.error);
                }
            }
            if (t != m_title) { m_title = t; SetWindowTextA(m_hwnd, t.c_str()); }
        }

        void HandleShortcuts()
        {
            ImGuiIO& io = ImGui::GetIO();
            if (m_modal != Modal::None) return;
            bool ctrl = io.KeyCtrl, shift = io.KeyShift, alt = io.KeyAlt;
            bool typing = io.WantTextInput;
            if (ctrl && !shift && ImGui::IsKeyPressed(ImGuiKey_S, false)) SaveEntryFile();
            if (ctrl && shift && ImGui::IsKeyPressed(ImGuiKey_S, false)) SaveAll();
            if (!typing && ctrl && ImGui::IsKeyPressed(ImGuiKey_Z)) m_session.undo.Undo();
            if (!typing && ctrl && ImGui::IsKeyPressed(ImGuiKey_Y)) m_session.undo.Redo();
            if (ctrl && ImGui::IsKeyPressed(ImGuiKey_N, false)) BeginNewEntry(nullptr);
            if (ctrl && ImGui::IsKeyPressed(ImGuiKey_W, false)) m_session.CloseTab(m_session.activeTab);
            if (ctrl && ImGui::IsKeyPressed(ImGuiKey_T, false)) m_session.NewTab();
            if (alt && ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false)) m_session.GoBack();
            if (alt && ImGui::IsKeyPressed(ImGuiKey_RightArrow, false)) m_session.GoForward();
            if (!typing && ImGui::IsKeyPressed(ImGuiKey_F2, false) && m_session.Selected()) m_assets->cb.rename(m_session.Selected());
            if (!typing && ImGui::IsKeyPressed(ImGuiKey_Delete, false) && m_session.Selected() && ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows))
                m_assets->cb.remove(m_session.Selected());
        }

        void DrawMenuBar()
        {
            if (!ImGui::BeginMenuBar()) return;
            auto* sel = m_session.Selected();
            if (ImGui::BeginMenu("File"))
            {
                if (ImGui::MenuItem("Open Project Folder...")) OpenProjectDialog();
                if (ImGui::MenuItem("New Asset...", "Ctrl+N")) BeginNewEntry(nullptr);
                ImGui::Separator();
                if (ImGui::MenuItem("Save", "Ctrl+S", false, sel && sel->Owner()->Dirty())) SaveEntryFile();
                if (ImGui::MenuItem("Save All", "Ctrl+Shift+S", false, m_session.project.AnyDirty())) SaveAll();
                if (ImGui::MenuItem("Reload From Disk")) ReloadAll();
                ImGui::Separator();
                if (ImGui::MenuItem("Preview Image File..."))
                {
                    fs::path p = platform::PickFile(m_hwnd, m_session.project.Root(), "Images\0*.tga;*.dds;*.png;*.jpg;*.bmp\0All files\0*.*\0", "Preview image");
                    if (!p.empty()) m_session.previewImage = p;
                }
                if (ImGui::MenuItem("Preview Compiled Asset...", nullptr, false, m_session.project.IsOpen())) { m_rawLists.clear(); m_modal = Modal::Compiled; m_modalOpen = true; }
                if (ImGui::BeginMenu("Convert"))
                {
                    if (ImGui::MenuItem("PC - all")) RunConvert("pcConvert", false);
                    if (ImGui::MenuItem("PC - selected asset", nullptr, false, sel != nullptr)) RunConvert("pcConvertSingle", true);
                    ImGui::Separator();
                    if (ImGui::MenuItem("Xenon - all")) RunConvert("xenonConvert", false);
                    if (ImGui::MenuItem("Xenon - selected asset", nullptr, false, sel != nullptr)) RunConvert("xenonConvertSingle", true);
                    ImGui::EndMenu();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Exit", "Alt+F4")) PostMessage(m_hwnd, WM_CLOSE, 0, 0);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Edit"))
            {
                auto& u = m_session.undo;
                if (ImGui::MenuItem(u.CanUndo() ? ("Undo " + u.UndoLabel()).c_str() : "Undo", "Ctrl+Z", false, u.CanUndo())) u.Undo();
                if (ImGui::MenuItem(u.CanRedo() ? ("Redo " + u.RedoLabel()).c_str() : "Redo", "Ctrl+Y", false, u.CanRedo())) u.Redo();
                ImGui::Separator();
                if (ImGui::MenuItem("Duplicate Asset...", nullptr, false, sel != nullptr)) m_assets->cb.duplicate(sel);
                if (ImGui::MenuItem("Rename Asset...", "F2", false, sel != nullptr)) m_assets->cb.rename(sel);
                if (ImGui::MenuItem("Delete Asset...", "Del", false, sel != nullptr)) m_assets->cb.remove(sel);
                ImGui::Separator();
                if (ImGui::MenuItem("Toggle Favourite", nullptr, false, sel != nullptr)) m_session.ToggleFavorite(*sel);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("View"))
            {
                if (ImGui::MenuItem("Back", "Alt+Left", false, m_session.CanGoBack())) m_session.GoBack();
                if (ImGui::MenuItem("Forward", "Alt+Right", false, m_session.CanGoForward())) m_session.GoForward();
                if (ImGui::MenuItem("New Tab", "Ctrl+T")) m_session.NewTab();
                if (ImGui::MenuItem("Close Tab", "Ctrl+W", false, !m_session.tabs.empty())) m_session.CloseTab(m_session.activeTab);
                ImGui::Separator();
                if (ImGui::MenuItem("Reset Layout")) { m_session.leftWidth = 0.30f; m_session.previewHeight = 0.34f; m_session.filtersHeight = 0.20f; }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Theme"))
            {
                const auto& themes = ui::AccentThemes();
                for (int i = 0; i < (int)themes.size(); ++i)
                {
                    // a small swatch of the accent in front of the name: the label is padded with spaces to make room
                    const float sw = ImGui::GetTextLineHeight() * 0.75f, gap = ui::Theme::S(10);
                    int pad = (int)std::ceil((sw + gap) / std::max(ImGui::CalcTextSize(" ").x, 1.0f));
                    ImVec2 p = ImGui::GetCursorScreenPos();
                    bool picked = ImGui::MenuItem((std::string(pad, ' ') + themes[i].name).c_str(), nullptr, ui::Theme::accentIndex == i);
                    float x0 = p.x + ImGui::GetStyle().WindowPadding.x * 0.5f + ui::Theme::S(2);
                    float y0 = p.y + (ImGui::GetTextLineHeight() - sw) * 0.5f + ImGui::GetStyle().FramePadding.y * 0.0f;
                    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + sw, y0 + sw), IM_COL32(themes[i].r, themes[i].g, themes[i].b, 255), 2.0f);
                    if (picked) { m_session.accentTheme = i; ui::Theme::SetAccent(i); }
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Help"))
            {
                auto upd = m_update.Get();
                if (upd.state == UpdateChecker::State::Available && UpdateChecker::SafeUrl(upd.url) &&
                    ImGui::MenuItem(("Download version " + upd.latest + "...").c_str()))
                    platform::OpenInShell(upd.url);
                if (ImGui::MenuItem(upd.state == UpdateChecker::State::Checking ? "Checking for updates..." : "Check for updates", nullptr, false, upd.state != UpdateChecker::State::Checking))
                { m_updateManual = true; m_updateAnnounced = false; m_lastUpdateState = UpdateChecker::State::Idle; m_update.Start(); }
                ImGui::MenuItem("Check for updates at startup", nullptr, &m_session.checkUpdates);
                ImGui::Separator();
                if (ImGui::MenuItem("About")) { m_modal = Modal::About; m_modalOpen = true; }
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }

        void DrawToolbar()
        {
            using namespace ui;
            float ic = Theme::S(18);
            if (IconButton("tb_new", Icon::New, ic, "New asset (Ctrl+N)")) BeginNewEntry(nullptr);
            ImGui::SameLine(0, 2);
            if (IconButton("tb_open", Icon::Open, ic, "Open project folder")) OpenProjectDialog();
            ImGui::SameLine(0, 2);
            if (IconButton("tb_save", Icon::Save, ic, "Save all (Ctrl+Shift+S)", m_session.project.AnyDirty())) SaveAll();
            ImGui::SameLine(0, Theme::S(14));
            ImGui::BeginDisabled(!m_session.undo.CanUndo());
            if (IconButton("tb_undo", Icon::Undo, ic, "Undo (Ctrl+Z)", false, IM_COL32(110, 160, 245, 255))) m_session.undo.Undo();
            ImGui::EndDisabled();
            ImGui::SameLine(0, 2);
            ImGui::BeginDisabled(!m_session.undo.CanRedo());
            if (IconButton("tb_redo", Icon::Redo, ic, "Redo (Ctrl+Y)", false, IM_COL32(110, 160, 245, 255))) m_session.undo.Redo();
            ImGui::EndDisabled();
        }

        // A bordered panel with an Asset-Property-Editor style title strip.
        bool BeginPanel(const char* id, const char* title, ImVec2 size)
        {
            using namespace ui;
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            ImGui::PushStyleColor(ImGuiCol_ChildBg, V4(Palette::Panel));
            bool open = ImGui::BeginChild(id, size, ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            ImGui::PopStyleColor();
            ImGui::PopStyleVar();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 p = ImGui::GetWindowPos();
            float w = ImGui::GetWindowWidth(), h = Theme::S(18);
            dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(0x2a, 0x2a, 0x2a, 255));
            dl->AddText(ImVec2(p.x + Theme::S(5), p.y + (h - ImGui::GetTextLineHeight()) * 0.5f), Palette::TextDim, title);
            dl->AddRectFilled(ImVec2(p.x + w - Theme::S(18), p.y + Theme::S(3)), ImVec2(p.x + w - Theme::S(6), p.y + Theme::S(9)), Palette::Accent, 1);
            ImGui::SetCursorPos(ImVec2(Theme::S(4), h + Theme::S(3)));
            ImGui::BeginGroup();
            return open;
        }

        void EndPanel() { ImGui::EndGroup(); ImGui::EndChild(); }

        // returns delta (pixels) while dragging
        float Splitter(const char* id, bool vertical, float length)
        {
            using namespace ui;
            const float t = Theme::S(5);
            ImVec2 sz = vertical ? ImVec2(t, length) : ImVec2(length, t);
            ImGui::InvisibleButton(id, sz);
            if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(vertical ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
            if (ImGui::IsItemActive()) return vertical ? ImGui::GetIO().MouseDelta.x : ImGui::GetIO().MouseDelta.y;
            return 0;
        }

        void DrawMain()
        {
            using namespace ui;
            ImGuiViewport* vp = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(vp->WorkPos);
            ImGui::SetNextWindowSize(vp->WorkSize);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Theme::S(4), 0));
            ImGui::Begin("##main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                            ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            ImGui::PopStyleVar();
            HandleShortcuts();
            DrawMenuBar();

            // toolbar
            ImGui::Dummy(ImVec2(0, Theme::S(2)));
            DrawToolbar();
            ImGui::Dummy(ImVec2(0, Theme::S(2)));

            const float statusH = Theme::S(22);
            ImVec2 avail = ImGui::GetContentRegionAvail();
            float contentH = avail.y - statusH;
            const float sp = Theme::S(5);

            float leftW = std::clamp(m_session.leftWidth * avail.x, Theme::S(220), avail.x * 0.7f);
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));

            // ---- left column
            ImGui::BeginGroup();
            float prevH = std::clamp(m_session.previewHeight * contentH, Theme::S(120), contentH * 0.7f);
            float filtH = std::clamp(m_session.filtersHeight * contentH, Theme::S(110), contentH * 0.5f);
            float assetsH = contentH - prevH - filtH - sp * 2;
            if (assetsH < Theme::S(80)) { assetsH = Theme::S(80); }

            if (BeginPanel("##preview", "Preview", ImVec2(leftW, prevH)))
            {
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Theme::S(6), Theme::S(5)));
                m_preview->Draw(ImVec2(leftW, prevH));
                ImGui::PopStyleVar();
            }
            EndPanel();
            float d = Splitter("##s1", false, leftW);
            if (d != 0) m_session.previewHeight = std::clamp((prevH + d) / contentH, 0.1f, 0.7f);

            if (BeginPanel("##assets", "Assets", ImVec2(leftW, assetsH)))
            {
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Theme::S(6), Theme::S(5)));
                m_assets->Draw();
                ImGui::PopStyleVar();
            }
            EndPanel();
            d = Splitter("##s2", false, leftW);
            if (d != 0) m_session.filtersHeight = std::clamp((filtH - d) / contentH, 0.1f, 0.5f);

            if (BeginPanel("##filters", "Filters", ImVec2(leftW, filtH)))
            {
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Theme::S(6), Theme::S(5)));
                m_filters->Draw();
                ImGui::PopStyleVar();
            }
            EndPanel();
            ImGui::EndGroup();

            ImGui::SameLine(0, 0);
            d = Splitter("##sv", true, contentH);
            if (d != 0) m_session.leftWidth = std::clamp((leftW + d) / avail.x, 0.15f, 0.7f);
            ImGui::SameLine(0, 0);

            // ---- right: tabs + property editor
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Theme::S(6), Theme::S(5)));
            DrawRightPane(ImVec2(avail.x - leftW - sp, contentH));
            ImGui::PopStyleVar();
            ImGui::PopStyleVar();

            DrawStatusBar(statusH);
            ImGui::End();
        }

        void DrawRightPane(ImVec2 size)
        {
            using namespace ui;
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Theme::S(6), Theme::S(4)));
            ImGui::PushStyleColor(ImGuiCol_ChildBg, V4(Palette::Window));
            ImGui::BeginChild("##right", size, ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
            ImGui::PopStyleColor();
            ImGui::PopStyleVar();

            // tabs
            if (ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_FittingPolicyScroll | ImGuiTabBarFlags_TabListPopupButton))
            {
                if (ImGui::TabItemButton("+", ImGuiTabItemFlags_Leading | ImGuiTabItemFlags_NoTooltip)) m_session.NewTab();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("New empty tab (Ctrl+T)");
                int closeIdx = -1;
                int want = m_session.activeTab;
                for (int i = 0; i < (int)m_session.tabs.size(); ++i)
                {
                    const auto& tab = m_session.tabs[i];
                    std::string label = (tab.entry ? tab.entry->Name() + (tab.entry->Owner()->Dirty() ? " *" : "") : std::string("New Tab")) +
                                        "###tab" + std::to_string(tab.uid);
                    bool open = true;
                    ImGuiTabItemFlags flags = (want == i && m_appliedTab != want) ? ImGuiTabItemFlags_SetSelected : 0;
                    if (ImGui::BeginTabItem(label.c_str(), &open, flags))
                    {
                        if (m_appliedTab == want) m_session.SetActiveTab(i);   // the user clicked this tab
                        ImGui::EndTabItem();
                    }
                    if (!open) closeIdx = i;
                }
                m_appliedTab = m_session.activeTab;
                ImGui::EndTabBar();
                if (closeIdx >= 0) { m_session.CloseTab(closeIdx); m_appliedTab = -2; }
            }

            gdt::Entry* sel = m_session.Selected();
            DrawNavBar(sel);

            ImGui::PushStyleColor(ImGuiCol_ChildBg, V4(Palette::Window));
            ImGui::BeginChild("##props", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_None);
            ImGui::PopStyleColor();
            if (sel && m_session.project.IsOpen())
            {
                m_props->SetFilter(m_propFilter);
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Theme::S(6), Theme::S(6)));
                m_props->Draw(*sel);
                ImGui::PopStyleVar();
            }
            else
            {
                ImGui::SetCursorPos(ImVec2(Theme::S(16), Theme::S(16)));
                ImGui::TextDisabled(m_session.project.IsOpen() ? "Select an asset from the list to edit its properties." : "No project is open. Use File > Open Project Folder.");
                if (!m_session.project.IsOpen())
                {
                    ImGui::SetCursorPosX(Theme::S(16));
                    if (ImGui::Button("Open Project Folder...")) OpenProjectDialog();
                }
            }
            ImGui::EndChild();
            ImGui::EndChild();
        }

        void DrawNavBar(gdt::Entry* sel)
        {
            using namespace ui;
            float ic = Theme::S(16);
            ImGui::BeginDisabled(!m_session.CanGoBack());
            if (IconButton("nav_back", Icon::Back, ic, "Back (Alt+Left)")) m_session.GoBack();
            ImGui::EndDisabled();
            ImGui::SameLine(0, 2);
            ImGui::BeginDisabled(!m_session.CanGoForward());
            if (IconButton("nav_fwd", Icon::Forward, ic, "Forward (Alt+Right)")) m_session.GoForward();
            ImGui::EndDisabled();
            ImGui::SameLine(0, 2);
            if (IconButton("nav_reload", Icon::Refresh, ic, "Reload this GDT from disk") && sel)
            {
                if (sel->Owner()->Dirty()) Message("This GDT has unsaved changes; save or use Edit > Undo first.");
                else ReloadAll();
            }
            ImGui::SameLine(0, Theme::S(8));

            const float filterW = Theme::S(200);
            float barW = ImGui::GetContentRegionAvail().x - filterW - Theme::S(60);
            ImVec2 p = ImGui::GetCursorScreenPos();
            float h = ImGui::GetFrameHeight();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(p, ImVec2(p.x + barW, p.y + h), Palette::Field, Theme::S(2));
            dl->AddRect(p, ImVec2(p.x + barW, p.y + h), Palette::Border, Theme::S(2));
            if (sel)
            {
                std::string type = sel->Def() ? sel->Def()->displayName : sel->GdfName();
                std::string path = sel->Name() + "." + type + "   (" + sel->Owner()->Name() + ".gdt)";
                Icon icon; ImU32 c1, c2; AssetTypeIcon(sel->GdfName(), icon, c1, c2);
                DrawIcon(dl, icon, ImVec2(p.x + Theme::S(5), p.y + (h - Theme::S(14)) * 0.5f), Theme::S(14), c1, c2);
                dl->AddText(ImVec2(p.x + Theme::S(26), p.y + (h - ImGui::GetTextLineHeight()) * 0.5f), Palette::Text, path.c_str());
            }
            ImGui::Dummy(ImVec2(barW, h));
            ImGui::SameLine(0, Theme::S(4));
            bool fav = sel && m_session.IsFavorite(*sel);
            if (IconButton("nav_fav", fav ? Icon::StarFilled : Icon::Star, ic, "Toggle favourite", fav) && sel) m_session.ToggleFavorite(*sel);
            ImGui::SameLine(0, 2);
            bool ro = sel && sel->Owner()->IsReadOnly();
            if (ro) { IconButton("nav_lock", Icon::Lock, ic, "The GDT file is read only"); ImGui::SameLine(0, 2); }
            ImGui::SetNextItemWidth(filterW);
            ImGui::SetCursorPosX(ImGui::GetWindowWidth() - filterW - Theme::S(10));
            ImGui::InputTextWithHint("##pf", "Filter by Value or Key", &m_propFilter);
            ImGui::Separator();
        }

        void DrawStatusBar(float h)
        {
            using namespace ui;
            ImGui::SetCursorPosY(ImGui::GetWindowHeight() - h);
            ImVec2 p = ImGui::GetCursorScreenPos();
            float w = ImGui::GetWindowWidth();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(ImVec2(ImGui::GetWindowPos().x, p.y), ImVec2(ImGui::GetWindowPos().x + w, p.y + h), IM_COL32(0x2a, 0x2a, 0x2a, 255));
            std::string left;
            if (NowSeconds() < m_session.statusUntil) left = m_session.statusText;
            else if (m_session.project.IsOpen()) left = m_session.project.Root().string();
            dl->AddText(ImVec2(p.x + Theme::S(4), p.y + (h - ImGui::GetTextLineHeight()) * 0.5f), Palette::TextDim, left.c_str());
            size_t pos = m_assets->UpdateSelectedPosition();
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%zu / %zu", pos, m_session.project.EntryCount());
            ImVec2 ts = ImGui::CalcTextSize(buf);
            dl->AddText(ImVec2(ImGui::GetWindowPos().x + w - ts.x - Theme::S(12), p.y + (h - ts.y) * 0.5f), Palette::Text, buf);
        }

        // ------------------------------------------------------------------ state
        HWND m_hwnd = nullptr;
        int m_width = 1500, m_height = 900;
        bool m_quit = false;
        std::string m_title;
        bool m_inSizeMove = false, m_inFrame = false;
        UpdateChecker m_update;
        UpdateChecker::State m_lastUpdateState = UpdateChecker::State::Idle;
        bool m_updateAnnounced = false, m_updateManual = false;

        Renderer m_renderer;
        Session m_session;
        std::unique_ptr<ui::PreviewPanel> m_preview;
        std::unique_ptr<ui::PropertyEditor> m_props;
        std::unique_ptr<ui::AssetsPanel> m_assets;
        std::unique_ptr<ui::FiltersPanel> m_filters;

        std::string m_shotPath, m_shotSelect, m_shotShape, m_shotRawModel, m_shotRawMat, m_shotRawAnim, m_shotImage;
        int m_frameCount = 0, m_testClickX = -1, m_testClickY = -1;
        std::string m_testEdit, m_testLog, m_testThen;
        std::string m_propFilter, m_rawFilter;
        std::vector<std::vector<std::string>> m_rawLists;
        int m_rawTab = 0;
        int m_appliedTab = -2;

        Modal m_modal = Modal::None;
        bool m_modalOpen = false;
        std::string m_modalText, m_nameBuf, m_newType;
        gdt::GdtFile* m_newGdt = nullptr;
        gdt::Entry* m_target = nullptr;
    };
}
