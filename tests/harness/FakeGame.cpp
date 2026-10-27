// pulsar_fakegame.exe - stand-in for Starfield.exe in the harness self-test
// (tests/test_harness_selftest.cpp, coop-anything multi-instance-testing.md
// section 10). It has both single-instance checks Starfield.exe imports
// (CreateMutexA, FindWindowA) and asks for the Documents and LocalAppData
// folders the way the game and SFSE do.
//
//   pulsar_fakegame.exe --name N --report FILE [--plugin DIR] [--hold MS]
//
// --plugin DIR loads DIR\PulsarTestHarness.dll and calls SFSEPlugin_Preload, as
// SFSE would. Exit codes: 0 ran to the end, 3 mutex says already running,
// 4 another copy's window was found, 5 plugin missing, 6 bad arguments.

#include <Windows.h>
#include <KnownFolders.h>
#include <ShlObj.h>

#include <cstdio>
#include <share.h>
#include <string>

namespace {
    std::FILE* g_report = nullptr;

    void Report(const char* a_key, const std::string& a_value)
    {
        if (!g_report) return;
        std::fprintf(g_report, "%s=%s\n", a_key, a_value.c_str());
        std::fflush(g_report);
    }

    std::string Narrow(const wchar_t* a_w)
    {
        const int n = WideCharToMultiByte(CP_ACP, 0, a_w, -1, nullptr, 0, nullptr, nullptr);
        std::string s(static_cast<std::size_t>(n), '\0');
        WideCharToMultiByte(CP_ACP, 0, a_w, -1, s.data(), n, nullptr, nullptr);
        s.resize(static_cast<std::size_t>(n) - 1);
        return s;
    }
}

int main(int argc, char** argv)
{
    std::string name, report, plugin;
    DWORD hold = 0;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string k = argv[i], v = argv[i + 1];
        if (k == "--name") name = v;
        else if (k == "--report") report = v;
        else if (k == "--plugin") plugin = v;
        else if (k == "--hold") hold = static_cast<DWORD>(std::stoul(v));
    }
    if (name.empty() || report.empty()) return 6;
    // Shared for reading: the self-test polls this file while the copy runs.
    g_report = _fsopen(report.c_str(), "w", _SH_DENYNO);

    if (!plugin.empty()) {
        HMODULE h = LoadLibraryA((plugin + "\\PulsarTestHarness.dll").c_str());
        auto preload = h ? reinterpret_cast<bool (*)(const void*)>(GetProcAddress(h, "SFSEPlugin_Preload")) : nullptr;
        if (!preload) { Report("result", "no-plugin"); return 5; }
        preload(nullptr);
    }

    PWSTR docs = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) {
        Report("documents", Narrow(docs));
        CoTaskMemFree(docs);
    }
    char local[MAX_PATH]{};
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE, nullptr, 0, local)))
        Report("localappdata", local);

    // Single-instance check 1: named mutex.
    const std::string mutexName = "PulsarFakeGame-" + name;
    HANDLE mutex = CreateMutexA(nullptr, TRUE, mutexName.c_str());
    if (GetLastError() == ERROR_ALREADY_EXISTS) { Report("result", "mutex-already-running"); return 3; }

    // Single-instance check 2: a window of an earlier copy.
    const std::string className = "PulsarFakeGameWnd-" + name;
    if (FindWindowA(className.c_str(), nullptr)) { Report("result", "window-already-running"); return 4; }

    WNDCLASSA wc{};
    wc.lpfnWndProc   = DefWindowProcA;
    wc.hInstance     = GetModuleHandleA(nullptr);
    wc.lpszClassName = className.c_str();
    RegisterClassA(&wc);
    HWND wnd = CreateWindowA(className.c_str(), "fake game", 0, 0, 0, 10, 10, nullptr, nullptr, wc.hInstance, nullptr);

    Report("result", "ready");
    Sleep(hold);
    if (wnd) DestroyWindow(wnd);
    if (mutex) CloseHandle(mutex);
    Report("exit", "0");
    if (g_report) std::fclose(g_report);
    return 0;
}
