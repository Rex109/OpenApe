# OpenApe

An open reimplementation of the Call of Duty 4 **Asset Manager** (`asset_manager.exe`), rebuilt as a modern
Dear ImGui application with an Asset-Property-Editor-style Qt look and a Direct3D 9 preview.

* **Same data model as the original.** Reads `deffiles/*.gdf` (definitions + UI layout) and `*.gdt` (game data) and builds the
  property editor from them, exactly as the 2008 tool did. GDT files are written back byte-compatibly (line endings preserved).
* **Preview** (D3D9): materials on a sphere (right click for cube, cylinder, cone, torus, plane), models in 3D (from the `XMODEL_EXPORT` source, or from the compiled `raw/xmodel*` files when the source is missing),
  images (tga/png/jpg/dds/iwi) as a flat 2D picture. `File > Preview Compiled Asset...` browses every model and material in `raw/`. Materials/models can be drawn with the **game's own shaders**:
  techset -> technique -> `raw/shader_bin` bytecode (vs/ps 1.1-3.0) fed with the material's textures and constants.
* **Animations:** selecting an `xanim` asset plays its `xanim_export` file on a skeleton (matched by bone names) with a timeline: play/pause, frame stepping, scrubber, loop, speed, notetrack markers. A toolbar button shows any model's **bones**, with a second toggle for their **names**.
* **Assets / Filters** panels (name, type, GDT, value filters, filter sets, favourites), tabs, back/forward, undo/redo.
* C++20, object-oriented, built from modules (`.ixx`); dependencies are git submodules in `libs/`.

## Build

Requirements: Visual Studio 2026 (or 2022) with the C++ toolset and the Windows SDK. Premake 5 is bundled in `tools/`.

```
git clone --recurse-submodules <repo>
generate-buildfiles_vs26.bat
```

Open `build\openape.slnx` and build `x64` (or `Win32`), or from a developer prompt:

```
msbuild build\openape.vcxproj /p:Configuration=Release /p:Platform=x64
```

Output: `bin\x64\openape.exe`. Replace `assets\icon.ico` to change the application icon.

## Usage

Run `openape.exe`; it looks for a game tree (a folder containing `deffiles`) next to the exe, in the working directory, or in
the last used location. You can also pass the folder: `openape.exe D:\path\to\codroot`.

Expected tree:

```
<root>\deffiles\*.gdf         definitions (and the converter DLLs, unused by OpenApe)
<root>\source_data\*.gdt      game data (or the directories listed in bin\converter_gdt_dirs_0.txt)
<root>\raw\                   optional compiled data used for the "Game:" shading modes
<root>\bin\                   optional: pcConvert.bat/.exe etc. for File > Convert
```

Command line helpers: `--selftest [root]` (parses everything, checks GDT round-trips, writes `selftest.log`),
`--screenshot out.png [--select assetName] [--shade 0..3]` (renders a frame and exits).

## Layout

```
src/core      utilities
src/gdf       definition language: parser, expressions, components
src/gdt       .gdt reader/writer
src/project   game tree, asset lookup, external converter launcher
src/gamedata  raw/ readers: shader_bin, techsets, techniques, compiled materials
src/gfx       math, meshes, image loaders (tga/png/jpg/dds/iwi), D3D9 renderer, material shading
src/ui        theme + icons, preview, property editor, asset/filter panels
src/app       session (undo, tabs, filters), platform dialogs, application shell
libs/         imgui (docking branch), stb
```

## Credits

Reverse-engineering notes for the original tool and the compiled formats live outside the repository
(`ape_work/decomp/NOTES.md`). Dear ImGui and stb are used under their own licences.
