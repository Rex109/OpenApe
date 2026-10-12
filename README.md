[![Build](https://github.com/Rex109/OpenApe/actions/workflows/build.yml/badge.svg)](https://github.com/Rex109/OpenApe/actions/workflows/build.yml)
[![License](https://img.shields.io/badge/license-MIT-blue)](https://creativecommons.org/licenses/by-nc/4.0/)
[![Personal Discord](https://img.shields.io/discord/953653773962739793?color=%237289DA&label=Personal%20Discord&logo=discord&logoColor=%23FFFFFF)](https://discord.gg/QDYk75vBBk)
[![ManyAsset](https://img.shields.io/discord/585171589750849538?color=%23FF8711&label=ManyAsset&logo=discord&logoColor=%23FFFFFF)](https://discord.gg/v2TWkeR)

# OpenApe
<img width="1820" height="396" alt="logo" src="https://github.com/user-attachments/assets/f46cd4df-18ad-4c3f-b792-6057038322ca" />


*<p align="center"><sub>The classic toolkit, evolved.</sub></p>*
<br>
An open, modern reimplementation of `asset_manager.exe`, the Asset Manager from the Call of Duty 4 mod tools, rebuilt with Dear ImGui and Direct3D 9 in the style of the Asset Property Editor.
<br>

> [!WARNING]
> **This project was made entirely with AI.**
> Every line of this project, the reverse engineering of the original tool, the file format readers, the renderer, the UI and the build files, was written by an AI model, and the format knowledge comes from a machine-assisted analysis of decompiled binaries. It works on the data it has been tried on, but it has not been reviewed the way real software should be. **Do not treat it as an example of good programming**, and do not copy its patterns into code you care about.

## ✨ What it does



- 📝 **Same data model as the original**: reads the `deffiles\*.gdf` definitions (variables, enums and the UI layout) and the `*.gdt` game data, and builds the property editor from them exactly like the 2008 tool
- 💾 **Safe saving**: GDT files are written back byte-compatibly, and line endings are preserved
- 🖼️ **3D / 2D preview** on Direct3D 9: materials on a sphere (or a cube, cylinder, cone, torus and plane), models in 3D, images as a flat picture
- 🎮 **The game's own shaders**: techset, technique and `raw\shader_bin` bytecode (vs/ps 1.1 to 3.0) are fed with the material's textures and constants
- 🎬 **Animation preview** with a timeline
- 🦴 **Skeleton view** with 3D bone shapes and bone names
- 🌅 **In-game skyboxes and stock map lighting**
- 🗂️ **Assets and Filters panels**, tabs, back and forward, favourites and undo/redo
- 🧱 **C++20**, object-oriented, built from modules (`.ixx`), with dependencies as git submodules

## 🖼️ Preview

Right click on the preview for more options. Left drag orbits, middle drag pans and the wheel zooms.

- 🔮 **Materials** are drawn on a primitive. Textures are looked up in `raw\images`, in `tex_assets`, and in the stock `.iwd` archives when they are missing
- 🧍 **Models** come from the editable `XMODEL_EXPORT` source, or from the compiled `raw\xmodel`, `xmodelsurfs` and `xmodelparts` files when the source is missing
- 🏞️ **Images** (`tga`, `png`, `jpg`, `dds`, `iwi`) are shown flat, with a checkerboard behind transparent pixels
- 📂 **File > Preview Compiled Asset** browses every model and material under `raw` without needing a GDT entry

### 💡 Lighting

The lightbulb opens the lighting menu:

- 🌤️ **Presets** such as Morning, Noon, Day, Afternoon, Overcast, Sunset, Dusk, Night and Moonlit, each with its own skybox and lighting
- 🗺️ **Stock map lighting**, taken from the worldspawn settings of the single player and multiplayer maps
- 🌌 **Skyboxes** (`*_ft`), read straight from the stock `iw_*.iwd` archives
- 🎛️ **Shading**: the game's sunlight preview, fakelight and unlit modes, or a simple built-in shader
- 🔄 **Rotate the light**: hold `L` and drag on the preview, or use the sliders in the right click menu
- 🔦 **Headlight** that follows the camera

## 🎬 Animations

<img width="1267" height="813" alt="openape_z56M9nSPG6" src="https://github.com/user-attachments/assets/64a9e371-239f-4841-9cb1-55a6ddb9bc7c" />

Selecting an `xanim` asset plays its `xanim_export` file on a skeleton.

- ⏯️ Play and pause, frame stepping, a scrubber, loop, playback speed and notetrack markers
- 🔗 The file only stores bone names, so the skeleton is matched by name. You can pick a different model with **Skeleton...**
- 🧵 Skinning is done on the CPU, with linear blend skinning

## 🦴 Bones

Any model with bones gets two toolbar buttons:

- 🔶 **Show bones**, as shaded 3D bone shapes coloured by hierarchy depth (or plain lines, from the right click menu)
- 🏷️ **Show bone names**, which works with or without the bone shapes

## 📁 Assets, filters and tabs

- 🔎 Filter assets by name, type, GDT file or value, and save filter sets
- ⭐ Favourites, and sorting by file order, name or type
- 🗂️ Selecting an asset shows it in the current tab. `+` or `Ctrl+T` opens an empty tab, and `Ctrl+click` or middle click on an asset opens it in a new tab
- ↩️ Undo and redo for every edit, rename, copy and delete

## ⌨️ Shortcuts

| Key | Action |
| --- | --- |
| `Ctrl+S` / `Ctrl+Shift+S` | Save the current GDT / save all |
| `Ctrl+Z` / `Ctrl+Y` | Undo / redo |
| `Ctrl+N` | New asset |
| `Ctrl+T` / `Ctrl+W` | New tab / close tab |
| `Alt+Left` / `Alt+Right` | Back / forward |
| `F2` / `Del` | Rename / delete the selected asset |
| `L` + drag | Rotate the preview light |

## 🔨 Build

Requirements:

- Visual Studio 2026 (or 2022) with the C++ toolset and the Windows SDK, **x64** or **x86**
- Premake 5 (bundled as `tools\premake5.exe`)

```
git clone --recurse-submodules https://github.com/Rex109/OpenApe.git
generate-buildfiles_vs26.bat
```

Then open `build\openape.slnx` in Visual Studio and build the platform you want, or from a command line:

```
msbuild build\openape.vcxproj /p:Configuration=Release /p:Platform=x64
```

The output is `bin\x64\openape.exe`. Replace `assets\icon.ico` to change the application icon.

## 🚀 Usage

Put the exe in your Call of Duty 4 root, in the `bin` folder. It works both ways:

- 🔌 **Replace the original**: save it as `<your cod4 root>\bin\asset_manager.exe`, so that other tools (like the CoD4CompileTools) open it correctly
- 🧩 **Keep the original** and place it next to it as `<your cod4 root>\bin\openape.exe`

It finds the game tree by itself: it walks up from the exe until it reaches a folder that contains `deffiles`, the same way the original does. You can also pass the folder, or start it from anywhere and use **File > Open Project Folder**:

```
openape.exe D:\path\to\codroot
```

The tree it expects:

```
<root>\deffiles\*.gdf         definitions (the converter DLLs next to them are not used)
<root>\source_data\*.gdt      game data (or the folders listed in bin\converter_gdt_dirs_0.txt)
<root>\raw\                   optional: compiled shaders, techsets, materials, images and models
<root>\main\iw_*.iwd          optional: stock archives, for skyboxes and missing images
<root>\bin\                   optional: pcConvert.bat/.exe and friends for File > Convert
```

Only the stock `iw_00` to `iw_13` archives are read, mod archives are ignored on purpose.

## 🗺️ Layout

```
src/core       utilities
src/gdf        definition language: parser, expressions, components
src/gdt        .gdt reader and writer
src/project    game tree, asset lookup, external converter launcher
src/gamedata   raw readers (shaders, techsets, materials, models, animations), .iwd archives
src/gfx        math, meshes, image loaders, the D3D9 renderer, skinning, environment
src/ui         theme and icons, preview, property editor, asset and filter panels
src/app        session (undo, tabs, filters), platform dialogs, application shell
libs/          imgui (docking), stb
```

## 🙏 Credits

Dear ImGui and stb are used under their own licences.
