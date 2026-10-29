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
//   * Steam Cloud: the ISteamRemoteStorage object Starfield.exe gets from
//     steam_api64 has every write-type method blocked and reports cloud as
//     disabled (BlockCloudWrites, ReportCloudDisabled), so a test copy can
//     never upload over or delete the owner's cloud saves.
//
// Runs in SFSE's preload phase (inside SFSE's _initterm_e hook, before the
// game's global initializers and WinMain). SFSE's own sfse.txt is opened
// before any plugin loads, so it still goes to the real Documents folder.
//
// Inert unless PulsarTestHarness.ini next to the DLL says Enabled = 1 and names
// both folders. Decisions live in HarnessPolicy.h (tested by pulsar_tests).

#include "HarnessPolicy.h"
#include "Diag/PerfStats.h"

#include <Windows.h>
#include <ShlObj.h>
#include <MinHook.h>

#include <dxgi1_2.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <share.h>
#include <string>
#include <thread>

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

    // ---- PULSAR's own single-instance mutex (process-wide, one name only) ----
    using CreateMutexW_t = HANDLE(WINAPI*)(LPSECURITY_ATTRIBUTES, BOOL, LPCWSTR);
    CreateMutexW_t g_origCreateMutexW = nullptr;

    HANDLE WINAPI Hook_CreateMutexW(LPSECURITY_ATTRIBUTES a_sa, BOOL a_own, LPCWSTR a_name)
    {
        if (!a_name || !g_settings.renamePulsarMutex || !IsPulsarCoopMutex(a_name))
            return g_origCreateMutexW(a_sa, a_own, a_name);
        const std::wstring unique = std::wstring(a_name) + L"-pulsartest-" + std::to_wstring(GetCurrentProcessId());
        HANDLE h = g_origCreateMutexW(a_sa, a_own, unique.c_str());
        const DWORD err = GetLastError();
        Log("CreateMutexW(PULSAR co-op guard) renamed for this test copy, lastError=%lu", err);
        SetLastError(err);
        return h;
    }

    // ---- Exit tracer ---------------------------------------------------------
    // A test copy that quits on purpose (ExitProcess / TerminateProcess on
    // itself) leaves no crash record. Log who asked, as module+offset frames
    // (run 20261028-211220: the partner exited after a Papyrus notification).
    using ExitProcess_t      = void(WINAPI*)(UINT);
    using TerminateProcess_t = BOOL(WINAPI*)(HANDLE, UINT);
    ExitProcess_t      g_origExitProcess = nullptr;
    TerminateProcess_t g_origTerminateProcess = nullptr;

    void LogCallers(const char* a_what, UINT a_code)
    {
        void* frames[32];
        const USHORT n = CaptureStackBackTrace(1, 32, frames, nullptr);
        Log("%s(code=%u) thread=%lu, %u frames:", a_what, a_code, GetCurrentThreadId(), n);
        for (USHORT i = 0; i < n; ++i) {
            HMODULE mod = nullptr;
            char name[MAX_PATH] = "?";
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   static_cast<LPCSTR>(frames[i]), &mod) && mod) {
                char path[MAX_PATH];
                if (GetModuleFileNameA(mod, path, MAX_PATH)) {
                    const char* slash = std::strrchr(path, '\\');
                    strncpy_s(name, slash ? slash + 1 : path, _TRUNCATE);
                }
            }
            Log("  #%02u %s+0x%llX", i, name,
                static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(frames[i]) -
                                                reinterpret_cast<std::uintptr_t>(mod)));
        }
    }

    // ntdll level: the CRT's exit reaches RtlExitUserProcess without kernel32.
    using RtlExitUserProcess_t = void(NTAPI*)(LONG);
    using NtTerminateProcess_t = LONG(NTAPI*)(HANDLE, LONG);
    RtlExitUserProcess_t g_origRtlExit = nullptr;
    NtTerminateProcess_t g_origNtTerminate = nullptr;

    void NTAPI Hook_RtlExitUserProcess(LONG a_code)
    {
        LogCallers("RtlExitUserProcess", static_cast<UINT>(a_code));
        g_origRtlExit(a_code);
    }

    LONG NTAPI Hook_NtTerminateProcess(HANDLE a_process, LONG a_code)
    {
        if (a_process == nullptr || a_process == GetCurrentProcess() ||
            GetProcessId(a_process) == GetCurrentProcessId())
            LogCallers("NtTerminateProcess(self)", static_cast<UINT>(a_code));
        return g_origNtTerminate(a_process, a_code);
    }

    // First-chance access violations and fail-fast codes, with where they came
    // from. The game and PULSAR catch many on purpose; the last ones before an
    // unexplained exit are the interesting ones. Capped.
    LONG CALLBACK FatalExceptionLogger(EXCEPTION_POINTERS* a_info)
    {
        static std::atomic<int> s_count{ 0 };
        const DWORD code = a_info->ExceptionRecord->ExceptionCode;
        if (code != EXCEPTION_ACCESS_VIOLATION && code != 0xC0000409 && code != EXCEPTION_STACK_OVERFLOW &&
            code != EXCEPTION_ILLEGAL_INSTRUCTION && code != 0xE06D7363 /* C++ throw */)
            return EXCEPTION_CONTINUE_SEARCH;
        void* at = a_info->ExceptionRecord->ExceptionAddress;
        // Each address at most 3 times (one handled AV in a loop filled a flat
        // cap of 200 before the fatal one, run 20261028-213422); 1000 lines total.
        {
            static std::mutex s_mu;
            static std::map<void*, int> s_seen;
            std::lock_guard lk(s_mu);
            if (++s_seen[at] > 3) return EXCEPTION_CONTINUE_SEARCH;
        }
        if (s_count.fetch_add(1) >= 1000) return EXCEPTION_CONTINUE_SEARCH;
        HMODULE mod = nullptr;
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               static_cast<LPCSTR>(at), &mod) && mod) {
            char path[MAX_PATH];
            if (GetModuleFileNameA(mod, path, MAX_PATH)) {
                const char* slash = std::strrchr(path, '\\');
                strncpy_s(name, slash ? slash + 1 : path, _TRUNCATE);
            }
        }
        Log("exception 0x%08lX at %s+0x%llX thread=%lu", code, name,
            static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(at) - reinterpret_cast<std::uintptr_t>(mod)),
            GetCurrentThreadId());
        if (code == EXCEPTION_ACCESS_VIOLATION) LogCallers("  stack of the exception", 0);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    void WINAPI Hook_ExitProcess(UINT a_code)
    {
        LogCallers("ExitProcess", a_code);
        g_origExitProcess(a_code);
    }

    BOOL WINAPI Hook_TerminateProcess(HANDLE a_process, UINT a_code)
    {
        if (a_process == GetCurrentProcess() || GetProcessId(a_process) == GetCurrentProcessId())
            LogCallers("TerminateProcess(self)", a_code);
        return g_origTerminateProcess(a_process, a_code);
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
        // PULSAR.dll loads after this preload, so its CreateMutexW import cannot be
        // patched yet; hook the API and act on its one mutex name only.
        if (g_settings.renamePulsarMutex) {
            ok &= MH_CreateHookApi(L"kernel32", "CreateMutexW", reinterpret_cast<LPVOID>(&Hook_CreateMutexW),
                                   reinterpret_cast<LPVOID*>(&g_origCreateMutexW)) == MH_OK;
        }
        const bool exitOk =
            MH_CreateHookApi(L"kernel32", "ExitProcess", reinterpret_cast<LPVOID>(&Hook_ExitProcess),
                             reinterpret_cast<LPVOID*>(&g_origExitProcess)) == MH_OK &&
            MH_CreateHookApi(L"kernel32", "TerminateProcess", reinterpret_cast<LPVOID>(&Hook_TerminateProcess),
                             reinterpret_cast<LPVOID*>(&g_origTerminateProcess)) == MH_OK &&
            MH_CreateHookApi(L"ntdll", "RtlExitUserProcess", reinterpret_cast<LPVOID>(&Hook_RtlExitUserProcess),
                             reinterpret_cast<LPVOID*>(&g_origRtlExit)) == MH_OK &&
            MH_CreateHookApi(L"ntdll", "NtTerminateProcess", reinterpret_cast<LPVOID>(&Hook_NtTerminateProcess),
                             reinterpret_cast<LPVOID*>(&g_origNtTerminate)) == MH_OK;
        AddVectoredExceptionHandler(1, &FatalExceptionLogger);
        ok &= MH_EnableHook(MH_ALL_HOOKS) == MH_OK;
        Log("folder hooks %s, exit tracer %s", ok ? "installed" : "FAILED", exitOk ? "on" : "off");
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

    // ---- Steam Cloud write block (ISteamRemoteStorage v016) -----------------
    // Starfield.exe gets the interface from steam_api64's
    // SteamInternal_FindOrCreateUserInterface. The harness gives that object a
    // private copy of its vtable in which every write-type slot fails (see
    // CloudActionFor) and every other slot is Steam's own function, so `this`
    // stays the real object. Only this test process sees the change.
    using FindOrCreateUserInterface_t = void*(__cdecl*)(int, const char*);
    FindOrCreateUserInterface_t g_origFindIface = nullptr;
    void*      g_cloudVtbl[kRemoteStorageSlots]{};
    std::mutex g_cloudMutex;

    // Blocked slots. Microsoft x64: the caller owns argument space, so a stub
    // may ignore trailing arguments. Named stubs log the file a write targeted.
    bool Cloud_FileWrite(void*, const char* a_file, const void*, std::int32_t a_size)
    {
        Log("BLOCKED Steam Cloud FileWrite(\"%s\", %d bytes)", a_file ? a_file : "?", a_size);
        return false;
    }
    std::uint64_t Cloud_FileWriteAsync(void*, const char* a_file, const void*, std::uint32_t a_size)
    {
        Log("BLOCKED Steam Cloud FileWriteAsync(\"%s\", %u bytes)", a_file ? a_file : "?", a_size);
        return 0;
    }
    bool Cloud_FileForget(void*, const char* a_file)
    {
        Log("BLOCKED Steam Cloud FileForget(\"%s\")", a_file ? a_file : "?");
        return false;
    }
    bool Cloud_FileDelete(void*, const char* a_file)
    {
        Log("BLOCKED Steam Cloud FileDelete(\"%s\")", a_file ? a_file : "?");
        return false;
    }
    std::uint64_t Cloud_FileWriteStreamOpen(void*, const char* a_file)
    {
        Log("BLOCKED Steam Cloud FileWriteStreamOpen(\"%s\")", a_file ? a_file : "?");
        return ~0ull;
    }
    bool          Cloud_FailBool(void*)       { Log("BLOCKED Steam Cloud write-type call"); return false; }
    std::uint64_t Cloud_FailCall(void*)       { Log("BLOCKED Steam Cloud write-type call"); return 0; }
    std::uint64_t Cloud_FailHandle(void*)     { Log("BLOCKED Steam Cloud write-type call"); return ~0ull; }
    void          Cloud_Ignore(void*)         { Log("IGNORED Steam Cloud SetCloudEnabledForApp"); }
    bool          Cloud_ReportDisabled(void*) { return false; }
    bool          Cloud_BatchOk(void*)        { return true; }

    void* StubFor(int a_slot, CloudAction a_action)
    {
        switch (a_slot) {
            case 0: return reinterpret_cast<void*>(&Cloud_FileWrite);
            case 2: return reinterpret_cast<void*>(&Cloud_FileWriteAsync);
            case 5: return reinterpret_cast<void*>(&Cloud_FileForget);
            case 6: return reinterpret_cast<void*>(&Cloud_FileDelete);
            case 9: return reinterpret_cast<void*>(&Cloud_FileWriteStreamOpen);
            default: break;
        }
        switch (a_action) {
            case CloudAction::kFailBool:       return reinterpret_cast<void*>(&Cloud_FailBool);
            case CloudAction::kFailCall:       return reinterpret_cast<void*>(&Cloud_FailCall);
            case CloudAction::kFailHandle:     return reinterpret_cast<void*>(&Cloud_FailHandle);
            case CloudAction::kIgnore:         return reinterpret_cast<void*>(&Cloud_Ignore);
            case CloudAction::kReportDisabled: return reinterpret_cast<void*>(&Cloud_ReportDisabled);
            case CloudAction::kBatchOk:        return reinterpret_cast<void*>(&Cloud_BatchOk);
            default:                           return nullptr;
        }
    }

    // Read-only calls through Steam's OWN vtable. If the slot numbers did not
    // match v016, these would return nonsense; the log shows the owner's real
    // cloud file count and names, which proves the mapping on this Steam.
    void ProbeRemoteStorage(void* a_iface, void** a_real)
    {
        using Count_t   = std::int32_t (*)(void*);
        using Name_t    = const char* (*)(void*, int, std::int32_t*);
        using Enabled_t = bool (*)(void*);
        const auto count = reinterpret_cast<Count_t>(a_real[kRsGetFileCount])(a_iface);
        Log("probe (Steam's own slots, read-only): GetFileCount=%d IsCloudEnabledForAccount=%d IsCloudEnabledForApp=%d",
            count,
            reinterpret_cast<Enabled_t>(a_real[kRsIsCloudEnabledForAccount])(a_iface) ? 1 : 0,
            reinterpret_cast<Enabled_t>(a_real[kRsIsCloudEnabledForApp])(a_iface) ? 1 : 0);
        for (int i = 0; i < count && i < 3; ++i) {
            std::int32_t size = 0;
            const char* name = reinterpret_cast<Name_t>(a_real[kRsGetFileNameAndSize])(a_iface, i, &size);
            Log("probe: file %d = \"%s\" (%d bytes)", i, name ? name : "?", size);
        }
    }

    void PatchRemoteStorage(void* a_iface)
    {
        std::lock_guard lk(g_cloudMutex);
        auto** vptrSlot = reinterpret_cast<void***>(a_iface);
        if (*vptrSlot == g_cloudVtbl) return;                       // already ours
        void** real = *vptrSlot;
        ProbeRemoteStorage(a_iface, real);
        int blocked = 0;
        for (int i = 0; i < kRemoteStorageSlots; ++i) {
            const auto action = CloudActionFor(i, g_settings.reportCloudDisabled);
            void* stub = action == CloudAction::kForward ? nullptr : StubFor(i, action);
            g_cloudVtbl[i] = stub ? stub : real[i];
            if (stub) ++blocked;
        }
        *vptrSlot = g_cloudVtbl;
        using Enabled_t = bool (*)(void*);
        const bool appEnabledNow =
            reinterpret_cast<Enabled_t>((*vptrSlot)[kRsIsCloudEnabledForApp])(a_iface);
        Log("Steam Cloud: remote storage %p patched, %d write-type slots blocked, game now sees IsCloudEnabledForApp=%d",
            a_iface, blocked, appEnabledNow ? 1 : 0);
    }

    void* __cdecl Hook_FindOrCreateUserInterface(int a_user, const char* a_version)
    {
        void* iface = g_origFindIface(a_user, a_version);
        const std::size_t prefixLen = std::strlen(kRemoteStoragePrefix);
        if (!iface || !a_version || std::strncmp(a_version, kRemoteStoragePrefix, prefixLen) != 0)
            return iface;
        if (std::strcmp(a_version, kRemoteStorageVersion) != 0) {
            // Unknown layout: the block cannot be trusted, and a test copy that
            // can write the owner's cloud must not run.
            Log("Steam Cloud: unknown interface %s - stopping this test copy to protect the owner's cloud", a_version);
            if (g_log) std::fclose(g_log);
            TerminateProcess(GetCurrentProcess(), 0xC10D);
        }
        PatchRemoteStorage(iface);
        return iface;
    }

    bool ExeImportsDll(const char* a_dll)
    {
        auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
        auto* nt   = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
        const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!dir.VirtualAddress) return false;
        for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp)
            if (_stricmp(reinterpret_cast<const char*>(base + imp->Name), a_dll) == 0) return true;
        return false;
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

// ─── Frame counter (docs/PERFORMANCE.md) ───
// Counts the game's real frames at IDXGISwapChain::Present / Present1 and logs the
// frame rate every 10 s. PULSAR's SFSE task runs several times per frame, so it can't
// count frames; this test-only hook keeps a render hook out of the shipped plugin.
// The game goes through NVIDIA Streamline's interposer, which wraps DXGI objects in
// proxies (a dummy swap chain made here crashed in sl.common.dll, run 20261029-175220).
// So nothing is created: the system dxgi.dll's factory functions are hooked at preload,
// the factory's swap-chain creation when a factory appears, and Present on the real
// swap chain it returns. Only Presents on the newest swap chain are counted.
namespace {
    using CreateFactoryFn  = HRESULT(WINAPI*)(REFIID, void**);
    using CreateFactory2Fn = HRESULT(WINAPI*)(UINT, REFIID, void**);
    using CreateForHwndFn  = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                                         const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
    using PresentFn  = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
    using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
    CreateFactoryFn            g_origFactory1 = nullptr;
    CreateFactory2Fn           g_origFactory2 = nullptr;
    CreateForHwndFn            g_origForHwnd  = nullptr;
    PresentFn                  g_origPresent  = nullptr;
    Present1Fn                 g_origPresent1 = nullptr;
    std::once_flag             g_hookForHwndOnce, g_hookPresentOnce;
    std::atomic<void*>         g_gameChain{ nullptr };
    std::atomic<std::uint32_t> g_chains{ 0 };
    std::mutex                 g_framesMutex;
    Pulsar::Diag::FrameStats   g_frames;
    std::atomic<std::uint64_t> g_presentCalls{ 0 }, g_present1Calls{ 0 }, g_otherCalls{ 0 };

    std::uint64_t NowUs()
    {
        static const std::int64_t s_freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart; }();
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        return static_cast<std::uint64_t>(c.QuadPart / s_freq * 1'000'000 + c.QuadPart % s_freq * 1'000'000 / s_freq);
    }

    void CountFrame(void* a_chain, UINT a_flags, std::atomic<std::uint64_t>& a_calls)
    {
        if (a_flags & DXGI_PRESENT_TEST) return;
        if (a_chain != g_gameChain.load(std::memory_order_relaxed)) { g_otherCalls.fetch_add(1, std::memory_order_relaxed); return; }
        a_calls.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard lk(g_framesMutex);
        g_frames.Record(NowUs(), 0);
    }

    HRESULT STDMETHODCALLTYPE Hook_Present(IDXGISwapChain* a_sc, UINT a_sync, UINT a_flags)
    {
        CountFrame(a_sc, a_flags, g_presentCalls);
        return g_origPresent(a_sc, a_sync, a_flags);
    }

    HRESULT STDMETHODCALLTYPE Hook_Present1(IDXGISwapChain1* a_sc, UINT a_sync, UINT a_flags, const DXGI_PRESENT_PARAMETERS* a_p)
    {
        CountFrame(a_sc, a_flags, g_present1Calls);
        return g_origPresent1(a_sc, a_sync, a_flags, a_p);
    }

    HRESULT STDMETHODCALLTYPE Hook_CreateForHwnd(IDXGIFactory2* a_f, IUnknown* a_dev, HWND a_hwnd, const DXGI_SWAP_CHAIN_DESC1* a_desc,
                                                 const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* a_fs, IDXGIOutput* a_out, IDXGISwapChain1** a_chain)
    {
        const HRESULT hr = g_origForHwnd(a_f, a_dev, a_hwnd, a_desc, a_fs, a_out, a_chain);
        if (SUCCEEDED(hr) && a_chain && *a_chain) {
            g_gameChain.store(*a_chain);
            const auto n = g_chains.fetch_add(1) + 1;
            Log("frame counter: swap chain %u created (%ux%u)", n, a_desc ? a_desc->Width : 0, a_desc ? a_desc->Height : 0);
            std::call_once(g_hookPresentOnce, [&] {
                void** vt = *reinterpret_cast<void***>(*a_chain);
                const bool ok =
                    MH_CreateHook(vt[8], reinterpret_cast<LPVOID>(&Hook_Present), reinterpret_cast<LPVOID*>(&g_origPresent)) == MH_OK &&
                    MH_CreateHook(vt[22], reinterpret_cast<LPVOID>(&Hook_Present1), reinterpret_cast<LPVOID*>(&g_origPresent1)) == MH_OK &&
                    MH_EnableHook(vt[8]) == MH_OK && MH_EnableHook(vt[22]) == MH_OK;
                Log("frame counter: Present hook %s", ok ? "installed" : "FAILED");
            });
        }
        return hr;
    }

    void HookFactory(void* a_factory)
    {
        if (!a_factory) return;
        std::call_once(g_hookForHwndOnce, [&] {
            IDXGIFactory2* f2 = nullptr;
            if (FAILED(static_cast<IUnknown*>(a_factory)->QueryInterface(IID_PPV_ARGS(&f2))) || !f2) {
                Log("frame counter: factory has no IDXGIFactory2 - no Frames lines");
                return;
            }
            void** vt = *reinterpret_cast<void***>(f2);
            const bool ok = MH_CreateHook(vt[15], reinterpret_cast<LPVOID>(&Hook_CreateForHwnd), reinterpret_cast<LPVOID*>(&g_origForHwnd)) == MH_OK &&
                            MH_EnableHook(vt[15]) == MH_OK;   // IDXGIFactory2::CreateSwapChainForHwnd
            f2->Release();
            Log("frame counter: factory hook %s", ok ? "installed" : "FAILED");
        });
    }

    HRESULT WINAPI Hook_CreateDXGIFactory1(REFIID a_iid, void** a_out)
    {
        const HRESULT hr = g_origFactory1(a_iid, a_out);
        if (SUCCEEDED(hr) && a_out) HookFactory(*a_out);
        return hr;
    }

    HRESULT WINAPI Hook_CreateDXGIFactory2(UINT a_flags, REFIID a_iid, void** a_out)
    {
        const HRESULT hr = g_origFactory2(a_flags, a_iid, a_out);
        if (SUCCEEDED(hr) && a_out) HookFactory(*a_out);
        return hr;
    }

    void StartFrameCounter()
    {
        if (!LoadLibraryW(L"dxgi.dll")) { Log("frame counter: dxgi.dll not loaded - no Frames lines"); return; }
        const bool ok =
            MH_CreateHookApi(L"dxgi", "CreateDXGIFactory1", reinterpret_cast<LPVOID>(&Hook_CreateDXGIFactory1),
                             reinterpret_cast<LPVOID*>(&g_origFactory1)) == MH_OK &&
            MH_CreateHookApi(L"dxgi", "CreateDXGIFactory2", reinterpret_cast<LPVOID>(&Hook_CreateDXGIFactory2),
                             reinterpret_cast<LPVOID*>(&g_origFactory2)) == MH_OK &&
            MH_EnableHook(MH_ALL_HOOKS) == MH_OK;
        Log("frame counter: dxgi factory hooks %s", ok ? "installed" : "FAILED");
        if (!ok) return;
        std::thread([] {
            for (;;) {
                Sleep(10000);
                Pulsar::Diag::FrameStats::Window w;
                {
                    std::lock_guard lk(g_framesMutex);
                    w = g_frames.Take();
                }
                if (!w.frames) continue;
                Log("Frames 10s: %.1f fps (frame p50 %.1f ms, p95 %.1f ms, max %.1f ms, %zu frames; Present %llu, Present1 %llu, other chains %llu)",
                    w.fps, w.frameMsP50, w.frameMsP95, w.frameMsMax, w.frames,
                    g_presentCalls.exchange(0), g_present1Calls.exchange(0), g_otherCalls.exchange(0));
            }
        }).detach();
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

    // PULSAR.dll reads %LOCALAPPDATA% from the environment (_dupenv_s in
    // LobbyHandoff::LobbyDir), not through the shell APIs: the first real launch
    // created an empty %LOCALAPPDATA%\PULSAR in the owner's profile. Point the
    // variable at the slot in both the Win32 block and the shared UCRT copy
    // (PULSAR and this DLL both use the DLL CRT, /MD).
    SetEnvironmentVariableW(L"LOCALAPPDATA", g_localAppDataW.c_str());
    _wputenv_s(L"LOCALAPPDATA", g_localAppDataW.c_str());
    Log("environment LOCALAPPDATA -> %s", g_settings.localAppDataDir.c_str());

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

    if (g_settings.maxWorkingSetMB != 0) {
        const SIZE_T maxBytes = static_cast<SIZE_T>(g_settings.maxWorkingSetMB) * 1024 * 1024;
        const BOOL ok = SetProcessWorkingSetSizeEx(GetCurrentProcess(), 256ull * 1024 * 1024, maxBytes,
                                                   QUOTA_LIMITS_HARDWS_MAX_ENABLE | QUOTA_LIMITS_HARDWS_MIN_DISABLE);
        Log("working-set cap %u MB: %s (lastError=%lu)", g_settings.maxWorkingSetMB, ok ? "set" : "FAILED",
            ok ? 0ul : GetLastError());
    }

    StartFrameCounter();

    if (g_settings.blockCloudWrites) {
        if (!ExeImportsDll("steam_api64.dll")) {
            Log("Steam Cloud: the exe does not import steam_api64.dll - nothing to block");
        } else if (!PatchExeImport("steam_api64.dll", "SteamInternal_FindOrCreateUserInterface",
                                   &Hook_FindOrCreateUserInterface, g_origFindIface)) {
            Log("Steam Cloud: could not install the write block - stopping this test copy to protect the owner's cloud");
            if (g_log) std::fclose(g_log);
            TerminateProcess(GetCurrentProcess(), 0xC10D);
        }
    }
    return true;
}
