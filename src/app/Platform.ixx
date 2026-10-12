module;

#include <filesystem>
#include <string>
#include <cstring>
#include <vector>
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>

export module openape.platform;

import openape.util;

// Thin wrappers over the native Windows dialogs / shell.
export namespace oa::platform
{
    // `filter` uses the Win32 double-NUL format, e.g. "Images\0*.tga;*.dds\0All files\0*.*\0".
    inline fs::path PickFile(HWND owner, const fs::path& initialDir, const char* filter, const char* title = "Select file")
    {
        char file[MAX_PATH] = {};
        std::string dir = initialDir.string();
        OPENFILENAMEA ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = owner;
        ofn.lpstrFilter = filter;
        ofn.lpstrFile = file;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrInitialDir = dir.empty() ? nullptr : dir.c_str();
        ofn.lpstrTitle = title;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
        if (!GetOpenFileNameA(&ofn)) return {};
        return file;
    }

    inline fs::path PickFolder(HWND owner, const char* title)
    {
        char name[MAX_PATH] = {};
        BROWSEINFOA bi{};
        bi.hwndOwner = owner;
        bi.pszDisplayName = name;
        bi.lpszTitle = title;
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        PIDLIST_ABSOLUTE pidl = SHBrowseForFolderA(&bi);
        if (!pidl) return {};
        char path[MAX_PATH] = {};
        bool ok = SHGetPathFromIDListA(pidl, path) != FALSE;
        CoTaskMemFree(pidl);
        return ok ? fs::path(path) : fs::path{};
    }

    inline fs::path PickSaveFile(HWND owner, const char* filter, const char* defExt, const char* title)
    {
        char file[MAX_PATH] = {};
        OPENFILENAMEA ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = owner;
        ofn.lpstrFilter = filter;
        ofn.lpstrFile = file;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrDefExt = defExt;
        ofn.lpstrTitle = title;
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
        if (!GetSaveFileNameA(&ofn)) return {};
        return file;
    }

    inline void OpenInShell(const fs::path& p)
    {
        ShellExecuteA(nullptr, "open", p.string().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    inline void RevealInExplorer(const fs::path& p)
    {
        std::string args = "/select,\"" + p.string() + "\"";
        ShellExecuteA(nullptr, "open", "explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    }

    inline void SetClipboardText(HWND owner, const std::string& text)
    {
        if (!OpenClipboard(owner)) return;
        EmptyClipboard();
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1);
        if (h)
        {
            std::memcpy(GlobalLock(h), text.c_str(), text.size() + 1);
            GlobalUnlock(h);
            SetClipboardData(CF_TEXT, h);
        }
        CloseClipboard();
    }

    inline bool IsReadOnlyFile(const fs::path& p)
    {
        DWORD a = GetFileAttributesA(p.string().c_str());
        return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY);
    }
}
