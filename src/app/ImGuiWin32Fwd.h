#pragma once
// Forward declaration of the Win32 backend's message handler (commented out in the backend header).
#include <windows.h>
#include "imgui.h"
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
