// Test-only stand-in for steam_api64.dll (built as steam_api64.dll next to
// pulsar_fakegame.exe). It exports SteamInternal_FindOrCreateUserInterface and
// hands out a fake ISteamRemoteStorage v016 object whose vtable has the real
// slot order, so the harness self-test can check the cloud write block without
// Steam. Every write that reaches this "cloud" is counted.

#include <atomic>
#include <cstdint>
#include <cstring>

namespace {
    std::atomic<int> g_writes{ 0 };

    // Default for slots the self-test does not use.
    std::uint64_t Unused(void*) { return 0; }

    bool          FileWrite(void*, const char*, const void*, std::int32_t) { ++g_writes; return true; }
    std::uint64_t FileWriteAsync(void*, const char*, const void*, std::uint32_t) { ++g_writes; return 1; }
    bool          FileDelete(void*, const char*) { ++g_writes; return true; }
    std::int32_t  GetFileCount(void*) { return 2; }
    const char*   GetFileNameAndSize(void*, int a_i, std::int32_t* a_size)
    {
        if (a_size) *a_size = 100 + a_i;
        return a_i == 0 ? "fake_save_a.sfs" : "fake_save_b.sfs";
    }
    bool IsEnabled(void*) { return true; }

    struct FakeRemoteStorage {
        void** vtbl;
    };

    void*             g_vtbl[59];
    FakeRemoteStorage g_storage{ g_vtbl };

    void InitVtbl()
    {
        static bool s_done = false;
        if (s_done) return;
        s_done = true;
        for (auto& p : g_vtbl) p = reinterpret_cast<void*>(&Unused);
        g_vtbl[0]  = reinterpret_cast<void*>(&FileWrite);
        g_vtbl[2]  = reinterpret_cast<void*>(&FileWriteAsync);
        g_vtbl[6]  = reinterpret_cast<void*>(&FileDelete);
        g_vtbl[18] = reinterpret_cast<void*>(&GetFileCount);
        g_vtbl[19] = reinterpret_cast<void*>(&GetFileNameAndSize);
        g_vtbl[21] = reinterpret_cast<void*>(&IsEnabled);
        g_vtbl[22] = reinterpret_cast<void*>(&IsEnabled);
    }
}

extern "C" __declspec(dllexport) void* __cdecl SteamInternal_FindOrCreateUserInterface(int, const char* a_version)
{
    InitVtbl();
    if (a_version && std::strcmp(a_version, "STEAMREMOTESTORAGE_INTERFACE_VERSION016") == 0) return &g_storage;
    if (a_version && std::strncmp(a_version, "STEAMREMOTESTORAGE_INTERFACE_VERSION", 36) == 0) return &g_storage;
    return nullptr;
}

// How many writes reached the fake cloud (the self-test's oracle).
extern "C" __declspec(dllexport) int __cdecl FakeSteam_CloudWriteCount()
{
    return g_writes.load();
}
