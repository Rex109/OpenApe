#include <windows.h>
#include <cstdio>
#include <filesystem>
#include <string>

import openape.util;
import openape.gdf;
import openape.gdt;
import openape.project;
import openape.mesh;
import openape.session;
import openape.app;

// `openape --selftest <root>` parses every .gdf/.gdt of a game tree and verifies GDT round-trips.
static int SelfTest(const std::filesystem::path& arg)
{
    freopen("selftest.log", "w", stdout);
    oa::Project p;
    auto root = oa::Project::FindRoot(arg.empty() ? std::filesystem::current_path() : arg);
    if (!p.Open(root)) { std::puts("open failed"); return 1; }
    std::printf("defs %zu gdts %zu entries %zu problems %zu\n", p.Defs().size(), p.Gdts().size(), p.EntryCount(), p.Errors().size());
    for (auto& e : p.Errors()) std::printf("  %s\n", e.c_str());
    int bad = 0;
    for (auto& g : p.Gdts())
    {
        std::string orig; oa::ReadTextFile(g->Path(), orig);
        if (orig != g->Serialize()) { ++bad; std::printf("round-trip differs: %s\n", g->Path().string().c_str()); }
    }
    std::printf("round-trip differences: %d\n", bad);
    return 0;
}

// `openape --selftest-model <file>` loads an XMODEL_EXPORT and prints what the viewer would see.
static int SelfTestModel(const std::filesystem::path& file)
{
    freopen("selftest.log", "w", stdout);
    oa::MeshData m; std::string err;
    if (!oa::XModelExportLoader::Load(file, m, err)) { std::printf("load failed: %s\n", err.c_str()); return 1; }
    std::printf("verts %zu tris %zu submeshes %zu bones %d\n", m.vertices.size(), m.TriangleCount(), m.submeshes.size(), m.boneCount);
    for (auto& s : m.submeshes) std::printf("  material '%s' indices %u\n", s.material.c_str(), s.indexCount);
    std::printf("bounds (%g %g %g) - (%g %g %g)\n", m.boundsMin.x, m.boundsMin.y, m.boundsMin.z, m.boundsMax.x, m.boundsMax.y, m.boundsMax.z);
    return 0;
}

// `openape --selftest-undo [root]` edits a value through the Session and checks undo/redo restore it.
static int SelfTestUndo(const std::filesystem::path& arg)
{
    freopen("selftest.log", "w", stdout);
    oa::Session s;
    if (!s.OpenRoot(oa::Project::FindRoot(arg.empty() ? std::filesystem::current_path() : arg))) { std::puts("open failed"); return 1; }
    oa::gdt::Entry* e = nullptr;
    for (auto& g : s.project.Gdts()) for (auto& x : g->Entries()) if (!e && x->Def() && x->Has("usage")) e = x.get();
    if (!e) { std::puts("no entry"); return 1; }
    std::string before = e->GetString("usage");
    s.SetValue(*e, "usage", "door");
    std::printf("after edit: %s can-undo=%d\n", e->GetString("usage").c_str(), (int)s.undo.CanUndo());
    s.undo.Undo();
    std::printf("after undo: %s (expected %s) can-redo=%d\n", e->GetString("usage").c_str(), before.c_str(), (int)s.undo.CanRedo());
    s.undo.Redo();
    std::printf("after redo: %s\n", e->GetString("usage").c_str());
    return 0;
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    if (__argc > 1 && std::string(__argv[1]) == "--selftest-undo") return SelfTestUndo(__argc > 2 ? std::filesystem::path(__argv[2]) : std::filesystem::path{});
    if (__argc > 2 && std::string(__argv[1]) == "--selftest-model") return SelfTestModel(__argv[2]);
    if (__argc > 1 && std::string(__argv[1]) == "--selftest")
        return SelfTest(__argc > 2 ? std::filesystem::path(__argv[2]) : std::filesystem::path{});
    oa::Application app;
    return app.Run(__argc, __argv);
}
