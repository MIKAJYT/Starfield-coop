// PulsarTestHarness.dll - TEST-ONLY SFSE plugin. Never part of a release
// (tools/package_release.ps1 copies Release\PULSAR.dll by name only).
//
// Lets a hard-linked test copy of Starfield run without touching the owner's
// settings and saves, and lets two copies run on one PC
// (coop-anything playbook/multi-instance-testing.md, sections 1 and 2):
//
//   * Folder redirect: SHGetKnownFolderPath / SHGetFolderPathW / SHGetFolderPathA
//     are hooked process-wide, so Documents and LocalAppData point into the test
//     slot for the game, SFSE's co-saves and every plugin's log.
//   * Single instance: only Starfield.exe's OWN imports of CreateMutexA and
//     FindWindowA are patched (Steam's DLLs keep theirs): named mutexes get a
//     per-process suffix, and windows of another process are reported missing.
//   * GetEnvironmentVariableA from Starfield.exe is logged (not changed), to
//     learn whether the game reads profile paths from the environment.
//
// Runs in SFSE's preload phase (inside SFSE's _initterm_e hook, before the
// game's global initializers and WinMain). SFSE's own sfse.txt is opened
// before any plugin loads, so it still goes to the real Documents folder.
//
// Inert unless PulsarTestHarness.ini next to the DLL says Enabled = 1 and names
// both folders. Decisions live in HarnessPolicy.h (tested by pulsar_tests).

#include "HarnessPolicy.h"

#include <Windows.h>
#include <ShlObj.h>
#include <MinHook.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <share.h>
#include <string>

using namespace Pulsar::TestHarness;
// ShlObj.h declares a COM interface named Folder; use an alias for ours.
using HFolder = Pulsar::TestHarness::Folder;

// ---- SFSE plugin interface (sfse/PluginAPI.h, SFSE 0.2.21) -----------------
struct SFSEPluginVersionData {
    std::uint32_t dataVersion;
    std::uint32_t pluginVersion;
    char          name[256];
    char          author[256];
    std::uint32_t addressIndependence;
    std::uint32_t structureIndependence;
    std::uint32_t compatibleVersions[16];
    std::uint32_t seVersionRequired;
    std::uint32_t reservedNonBreaking;
    std::uint32_t reservedBreaking;
};

extern "C" __declspec(dllexport) SFSEPluginVersionData SFSEPlugin_Version = {
    1,                       // kVersion
    1,                       // plugin version
    "PulsarTestHarness",
    "PULSAR Revived tests",
    1u << 0,                 // kAddressIndependence_Signatures: uses no game addresses
    1u << 0,                 // kStructureIndependence_NoStructs: reads no game structures
    { 0 },
    0, 0, 0
};

namespace {
    Settings     g_settings;
    std::wstring g_documentsW, g_localAppDataW;
    std::FILE*   g_log = nullptr;
    std::mutex   g_logMutex;

    void Log(const char* a_fmt, ...)
    {
        std::lock_guard lk(g_logMutex);
        if (!g_log) return;
        SYSTEMTIME t;
        GetLocalTime(&t);
        std::fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
        va_list args;
        va_start(args, a_fmt);
        std::vfprintf(g_log, a_fmt, args);
        va_end(args);
        std::fputc('\n', g_log);
        std::fflush(g_log);
    }

    // Log a message only the first time a key is seen (hooks run very often).
    bool FirstTime(const std::string& a_key)
    {
        static std::set<std::string> s_seen;
        static std::mutex            s_mu;
        std::lock_guard lk(s_mu);
        return s_seen.insert(a_key).second;
    }

    const char* FolderName(HFolder a_f)
    {
        return a_f == HFolder::kDocuments ? "Documents" : "LocalAppData";
    }

    const std::wstring& PathFor(HFolder a_f)
    {
        return a_f == HFolder::kDocuments ? g_documentsW : g_localAppDataW;
    }

    const std::string& PathForA(HFolder a_f)
    {
        return a_f == HFolder::kDocuments ? g_settings.documentsDir : g_settings.localAppDataDir;
    }

    std::wstring Widen(const std::string& a_s)
    {
        if (a_s.empty()) return {};
        const int n = MultiByteToWideChar(CP_ACP, 0, a_s.c_str(), -1, nullptr, 0);
        std::wstring w(static_cast<std::size_t>(n), L'\0');
        MultiByteToWideChar(CP_ACP, 0, a_s.c_str(), -1, w.data(), n);
        w.resize(static_cast<std::size_t>(n) - 1);
        return w;
    }

    // ---- folder hooks (process-wide, MinHook) ------------------------------
    using SHGetKnownFolderPath_t = HRESULT(WINAPI*)(REFKNOWNFOLDERID, DWORD, HANDLE, PWSTR*);
    using SHGetFolderPathW_t     = HRESULT(WINAPI*)(HWND, int, HANDLE, DWORD, LPWSTR);
    using SHGetFolderPathA_t     = HRESULT(WINAPI*)(HWND, int, HANDLE, DWORD, LPSTR);
    SHGetKnownFolderPath_t g_origKnownFolder = nullptr;
    SHGetFolderPathW_t     g_origFolderW = nullptr;
    SHGetFolderPathA_t     g_origFolderA = nullptr;

    HRESULT WINAPI Hook_SHGetKnownFolderPath(REFKNOWNFOLDERID a_id, DWORD a_flags, HANDLE a_token, PWSTR* a_out)
    {
        const HFolder f = FolderFromKnownFolder(*reinterpret_cast<const Guid*>(&a_id));
        if (f == HFolder::kNone || !a_out) return g_origKnownFolder(a_id, a_flags, a_token, a_out);
        const auto& p = PathFor(f);
        auto* buf = static_cast<PWSTR>(CoTaskMemAlloc((p.size() + 1) * sizeof(wchar_t)));
        if (!buf) { *a_out = nullptr; return E_OUTOFMEMORY; }
        std::wmemcpy(buf, p.c_str(), p.size() + 1);
        *a_out = buf;
        if (FirstTime(std::string("kf-") + FolderName(f)))
            Log("redirect SHGetKnownFolderPath(%s) -> %s", FolderName(f), PathForA(f).c_str());
        return S_OK;
    }

    HRESULT WINAPI Hook_SHGetFolderPathW(HWND a_hwnd, int a_csidl, HANDLE a_token, DWORD a_flags, LPWSTR a_out)
    {
        const HFolder f = FolderFromCsidl(a_csidl);
        if (f == HFolder::kNone || !a_out) return g_origFolderW(a_hwnd, a_csidl, a_token, a_flags, a_out);
        wcsncpy_s(a_out, MAX_PATH, PathFor(f).c_str(), _TRUNCATE);
        if (FirstTime(std::string("fw-") + FolderName(f)))
            Log("redirect SHGetFolderPathW(%s) -> %s", FolderName(f), PathForA(f).c_str());
        return S_OK;
    }

    HRESULT WINAPI Hook_SHGetFolderPathA(HWND a_hwnd, int a_csidl, HANDLE a_token, DWORD a_flags, LPSTR a_out)
    {
        const HFolder f = FolderFromCsidl(a_csidl);
        if (f == HFolder::kNone || !a_out) return g_origFolderA(a_hwnd, a_csidl, a_token, a_flags, a_out);
        strncpy_s(a_out, MAX_PATH, PathForA(f).c_str(), _TRUNCATE);
        if (FirstTime(std::string("fa-") + FolderName(f)))
            Log("redirect SHGetFolderPathA(%s) -> %s", FolderName(f), PathForA(f).c_str());
        return S_OK;
    }

    bool HookFolderApis()
    {
        if (!GetModuleHandleW(L"shell32.dll")) LoadLibraryW(L"shell32.dll");
        if (MH_Initialize() != MH_OK) { Log("MinHook init failed"); return false; }
        bool ok = true;
        ok &= MH_CreateHookApi(L"shell32", "SHGetKnownFolderPath", reinterpret_cast<LPVOID>(&Hook_SHGetKnownFolderPath),
                               reinterpret_cast<LPVOID*>(&g_origKnownFolder)) == MH_OK;
        ok &= MH_CreateHookApi(L"shell32", "SHGetFolderPathW", reinterpret_cast<LPVOID>(&Hook_SHGetFolderPathW),
                               reinterpret_cast<LPVOID*>(&g_origFolderW)) == MH_OK;
        ok &= MH_CreateHookApi(L"shell32", "SHGetFolderPathA", reinterpret_cast<LPVOID>(&Hook_SHGetFolderPathA),
                               reinterpret_cast<LPVOID*>(&g_origFolderA)) == MH_OK;
        ok &= MH_EnableHook(MH_ALL_HOOKS) == MH_OK;
        Log("folder hooks %s", ok ? "installed" : "FAILED");
        return ok;
    }

    // ---- Starfield.exe import patches --------------------------------------
    // Returns the IAT slot of a_dll!a_func in the main exe, or nullptr.
    void** FindExeImport(const char* a_dll, const char* a_func)
    {
        auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
        auto* dos  = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        auto* nt   = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!dir.VirtualAddress) return nullptr;
        for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
            if (_stricmp(reinterpret_cast<const char*>(base + imp->Name), a_dll) != 0) continue;
            if (!imp->OriginalFirstThunk) return nullptr;   // names unavailable
            auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->OriginalFirstThunk);
            auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
            for (; names->u1.AddressOfData; ++names, ++slots) {
                if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
                auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
                if (std::strcmp(reinterpret_cast<const char*>(byName->Name), a_func) == 0)
                    return reinterpret_cast<void**>(&slots->u1.Function);
            }
        }
        return nullptr;
    }

    template <class T>
    bool PatchExeImport(const char* a_dll, const char* a_func, T a_hook, T& a_orig)
    {
        void** slot = FindExeImport(a_dll, a_func);
        if (!slot) { Log("import %s!%s not found in Starfield.exe", a_dll, a_func); return false; }
        DWORD old = 0;
        if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) { Log("VirtualProtect failed for %s", a_func); return false; }
        a_orig = reinterpret_cast<T>(*slot);
        *slot  = reinterpret_cast<void*>(a_hook);
        VirtualProtect(slot, sizeof(void*), old, &old);
        Log("patched Starfield.exe import %s!%s", a_dll, a_func);
        return true;
    }

    using CreateMutexA_t           = HANDLE(WINAPI*)(LPSECURITY_ATTRIBUTES, BOOL, LPCSTR);
    using FindWindowA_t            = HWND(WINAPI*)(LPCSTR, LPCSTR);
    using GetEnvironmentVariableA_t = DWORD(WINAPI*)(LPCSTR, LPSTR, DWORD);
    CreateMutexA_t            g_origCreateMutexA = nullptr;
    FindWindowA_t             g_origFindWindowA = nullptr;
    GetEnvironmentVariableA_t g_origGetEnvA = nullptr;

    HANDLE WINAPI Hook_CreateMutexA(LPSECURITY_ATTRIBUTES a_sa, BOOL a_own, LPCSTR a_name)
    {
        if (!a_name || !g_settings.renameMutexes) return g_origCreateMutexA(a_sa, a_own, a_name);
        const auto unique = UniqueMutexName(a_name, GetCurrentProcessId());
        HANDLE h = g_origCreateMutexA(a_sa, a_own, unique.c_str());
        const DWORD err = GetLastError();
        Log("CreateMutexA(\"%s\") -> \"%s\" handle=%p lastError=%lu", a_name, unique.c_str(), h, err);
        SetLastError(err);   // the game checks ERROR_ALREADY_EXISTS after the call
        return h;
    }

    HWND WINAPI Hook_FindWindowA(LPCSTR a_class, LPCSTR a_title)
    {
        HWND h = g_origFindWindowA(a_class, a_title);
        DWORD pid = 0;
        if (h) GetWindowThreadProcessId(h, &pid);
        const bool hide = g_settings.hideOtherWindows && ShouldHideWindow(pid, GetCurrentProcessId());
        Log("FindWindowA(class=\"%s\", title=\"%s\") -> %p pid=%lu%s",
            a_class && !IS_INTRESOURCE(a_class) ? a_class : "(atom/null)", a_title ? a_title : "(null)",
            h, pid, hide ? " HIDDEN (other process)" : "");
        if (hide) { SetLastError(ERROR_SUCCESS); return nullptr; }
        return h;
    }

    DWORD WINAPI Hook_GetEnvironmentVariableA(LPCSTR a_name, LPSTR a_buf, DWORD a_size)
    {
        if (a_name && FirstTime(std::string("env-") + a_name))
            Log("GetEnvironmentVariableA(\"%s\") (logged only)", a_name);
        return g_origGetEnvA(a_name, a_buf, a_size);
    }

    std::string OwnDirectory()
    {
        HMODULE self = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&OwnDirectory), &self);
        char path[MAX_PATH]{};
        GetModuleFileNameA(self, path, MAX_PATH);
        std::string s(path);
        const auto slash = s.find_last_of("\\/");
        return slash == std::string::npos ? std::string() : s.substr(0, slash + 1);
    }
}

extern "C" __declspec(dllexport) bool SFSEPlugin_Preload(const void* /*sfse*/)
{
    std::ifstream ini(OwnDirectory() + "PulsarTestHarness.ini");
    if (!ini) return true;                     // no settings file: inert
    g_settings = ParseSettings(ini);
    if (!g_settings.enabled) return true;      // disabled or a folder missing: inert

    std::error_code ec;
    std::filesystem::create_directories(g_settings.documentsDir, ec);
    std::filesystem::create_directories(g_settings.localAppDataDir, ec);
    if (!g_settings.logFile.empty()) {
        std::filesystem::create_directories(std::filesystem::path(g_settings.logFile).parent_path(), ec);
        // Shared for reading: the test runner watches this log while the game runs
        // (an exclusive open hid it until exit on the first real launch).
        g_log = _fsopen(g_settings.logFile.c_str(), "a", _SH_DENYWR);
    }
    g_documentsW    = Widen(g_settings.documentsDir);
    g_localAppDataW = Widen(g_settings.localAppDataDir);

    Log("=== PulsarTestHarness preload, pid %lu", GetCurrentProcessId());
    Log("Documents -> %s", g_settings.documentsDir.c_str());
    Log("LocalAppData -> %s", g_settings.localAppDataDir.c_str());

    // Without the redirect the game would write the owner's real settings and
    // saves folders. In a test slot that is worse than not starting at all.
    if (!HookFolderApis()) {
        Log("folder redirect failed - stopping this test copy to protect the owner's folders");
        if (g_log) std::fclose(g_log);
        TerminateProcess(GetCurrentProcess(), 0xDEAD);
    }
    PatchExeImport("KERNEL32.dll", "CreateMutexA", &Hook_CreateMutexA, g_origCreateMutexA);
    PatchExeImport("USER32.dll", "FindWindowA", &Hook_FindWindowA, g_origFindWindowA);
    PatchExeImport("KERNEL32.dll", "GetEnvironmentVariableA", &Hook_GetEnvironmentVariableA, g_origGetEnvA);
    return true;
}
