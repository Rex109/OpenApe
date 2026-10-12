workspace "openape"
    location "build"
    configurations { "Debug", "Release" }
    platforms { "x64", "Win32" }
    startproject "openape"

    if _ACTION and not _ACTION:startswith("vs") then
        error("openape requires MSVC (Direct3D 9 + Win32).")
    end

project "openape"
    kind "WindowedApp"
    language "C++"
    cppdialect "C++20"
    targetdir "bin/%{cfg.platform}"
    objdir "build/obj/%{cfg.platform}/%{cfg.buildcfg}"
    targetname "openape"
    debugdir "."

    files {
        "src/**.cpp", "src/**.h", "src/**.ixx",
        "libs/imgui/imgui.cpp", "libs/imgui/imgui_draw.cpp", "libs/imgui/imgui_tables.cpp",
        "libs/imgui/imgui_widgets.cpp", "libs/imgui/imgui_demo.cpp",
        "libs/imgui/misc/cpp/imgui_stdlib.cpp", "libs/imgui/backends/imgui_impl_dx9.cpp", "libs/imgui/backends/imgui_impl_win32.cpp",
        "assets/openape.rc",
    }

    includedirs { "src", "libs/imgui", "libs/imgui/backends", "libs/imgui/misc/cpp", "libs/stb" }

    vpaths {
        ["src/*"]    = { "src/**" },
        ["imgui/*"]  = { "libs/imgui/**" },
        ["assets/*"] = { "assets/**" },
    }

    defines { "WIN32", "_WINDOWS", "_CRT_SECURE_NO_WARNINGS", "NOMINMAX", "WIN32_LEAN_AND_MEAN",
              "IMGUI_DISABLE_OBSOLETE_FUNCTIONS" }

    links { "d3d9", "dwmapi", "user32", "gdi32", "shell32", "comdlg32", "ole32", "uuid" }

    buildoptions { "/utf-8", "/permissive-" }
    staticruntime "On"
    runtime "Release"
    warnings "Default"
    disablewarnings { "4244", "4267", "4305", "4996" }
    exceptionhandling "On"
    editandcontinue "Off"
    multiprocessorcompile "On"

    filter "platforms:x64"
        architecture "x86_64"
    filter "platforms:Win32"
        architecture "x86"

    filter "configurations:Debug"
        defines { "_DEBUG" }
        symbols "On"
        optimize "Off"

    filter "configurations:Release"
        defines { "NDEBUG" }
        symbols "On"
        optimize "Speed"
        linkoptions { "/OPT:REF", "/OPT:ICF" }
