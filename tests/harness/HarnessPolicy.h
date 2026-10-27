#pragma once

// Decisions of the test-only harness plugin (tests/harness/Harness.cpp), kept
// free of Windows API calls so pulsar_tests can check them without the game.
// See docs/TESTING.md "Test harness plugin".

#include <cctype>
#include <cstdint>
#include <istream>
#include <string>

namespace Pulsar::TestHarness {

    // Folders the harness redirects into the test slot.
    enum class Folder : std::uint8_t { kNone, kDocuments, kLocalAppData };

    // CSIDL values (shlobj.h). The low byte is the folder; the high bits are
    // flags such as CSIDL_FLAG_CREATE (0x8000).
    inline constexpr int kCsidlPersonal     = 0x0005;   // CSIDL_PERSONAL == CSIDL_MYDOCUMENTS
    inline constexpr int kCsidlLocalAppData = 0x001C;   // CSIDL_LOCAL_APPDATA

    inline Folder FolderFromCsidl(int a_csidl)
    {
        switch (a_csidl & 0xFF) {
            case kCsidlPersonal:     return Folder::kDocuments;
            case kCsidlLocalAppData: return Folder::kLocalAppData;
            default:                 return Folder::kNone;
        }
    }

    // A GUID laid out like Windows' GUID, so this header needs no windows.h.
    struct Guid {
        std::uint32_t d1;
        std::uint16_t d2;
        std::uint16_t d3;
        std::uint8_t  d4[8];
    };

    inline bool SameGuid(const Guid& a, const Guid& b)
    {
        if (a.d1 != b.d1 || a.d2 != b.d2 || a.d3 != b.d3) return false;
        for (int i = 0; i < 8; ++i)
            if (a.d4[i] != b.d4[i]) return false;
        return true;
    }

    // FOLDERID_Documents {FDD39AD0-238F-46AF-ADB4-6C85480369C7}
    inline constexpr Guid kFolderIdDocuments{ 0xFDD39AD0, 0x238F, 0x46AF,
        { 0xAD, 0xB4, 0x6C, 0x85, 0x48, 0x03, 0x69, 0xC7 } };
    // FOLDERID_LocalAppData {F1B32785-6FBA-4FCF-9D55-7B8E7F157091}
    inline constexpr Guid kFolderIdLocalAppData{ 0xF1B32785, 0x6FBA, 0x4FCF,
        { 0x9D, 0x55, 0x7B, 0x8E, 0x7F, 0x15, 0x70, 0x91 } };

    inline Folder FolderFromKnownFolder(const Guid& a_id)
    {
        if (SameGuid(a_id, kFolderIdDocuments))    return Folder::kDocuments;
        if (SameGuid(a_id, kFolderIdLocalAppData)) return Folder::kLocalAppData;
        return Folder::kNone;
    }

    // Named mutexes created by Starfield.exe get a per-process suffix, so a
    // single-instance mutex no longer collides with the other test copy. Within
    // one process the rename is consistent, so the game's own use still works.
    // An unnamed mutex (empty name) stays unnamed.
    inline std::string UniqueMutexName(const std::string& a_name, std::uint32_t a_pid)
    {
        if (a_name.empty()) return a_name;
        return a_name + "-pulsartest-" + std::to_string(a_pid);
    }

    // FindWindowA from Starfield.exe: a window that belongs to ANOTHER process
    // (the other test copy) is reported as not found; the game's own windows
    // are returned unchanged. 0 = no window found by the real call.
    inline bool ShouldHideWindow(std::uint32_t a_windowPid, std::uint32_t a_selfPid)
    {
        return a_windowPid != 0 && a_windowPid != a_selfPid;
    }

    // ---- Steam Cloud (ISteamRemoteStorage) ---------------------------------
    // Starfield.exe asks steam_api64 for this interface by name through
    // SteamInternal_FindOrCreateUserInterface. Method order from Valve's
    // isteamremotestorage.h (STEAMREMOTESTORAGE_INTERFACE_VERSION016), as
    // mirrored in Steamworks.NET CodeGen/steam/isteamremotestorage.h.
    inline constexpr const char* kRemoteStoragePrefix  = "STEAMREMOTESTORAGE_INTERFACE_VERSION";
    inline constexpr const char* kRemoteStorageVersion = "STEAMREMOTESTORAGE_INTERFACE_VERSION016";
    inline constexpr int         kRemoteStorageSlots   = 59;

    // v016 slot numbers the harness uses by name.
    inline constexpr int kRsFileWrite                = 0;
    inline constexpr int kRsGetFileCount             = 18;
    inline constexpr int kRsGetFileNameAndSize       = 19;
    inline constexpr int kRsIsCloudEnabledForAccount = 21;
    inline constexpr int kRsIsCloudEnabledForApp     = 22;

    enum class CloudAction : std::uint8_t {
        kForward,          // call Steam (reads, listings, quota, UGC downloads)
        kFailBool,         // return false
        kFailCall,         // return k_uAPICallInvalid (0)
        kFailHandle,       // return an invalid 64-bit handle (~0)
        kIgnore,           // void method: do nothing
        kReportDisabled,   // IsCloudEnabledFor*: return false
        kBatchOk,          // Begin/EndFileWriteBatch: return true, nothing to batch
    };

    // What the harness does with each v016 slot when blocking cloud writes.
    inline CloudAction CloudActionFor(int a_slot, bool a_reportDisabled)
    {
        switch (a_slot) {
            case 0:  return CloudAction::kFailBool;     // FileWrite
            case 2:  return CloudAction::kFailCall;     // FileWriteAsync
            case 5:  return CloudAction::kFailBool;     // FileForget
            case 6:  return CloudAction::kFailBool;     // FileDelete
            case 7:  return CloudAction::kFailCall;     // FileShare
            case 8:  return CloudAction::kFailBool;     // SetSyncPlatforms
            case 9:  return CloudAction::kFailHandle;   // FileWriteStreamOpen
            case 10: return CloudAction::kFailBool;     // FileWriteStreamWriteChunk
            case 11: return CloudAction::kFailBool;     // FileWriteStreamClose
            case 12: return CloudAction::kFailBool;     // FileWriteStreamCancel
            case 21: return a_reportDisabled ? CloudAction::kReportDisabled : CloudAction::kForward;  // IsCloudEnabledForAccount
            case 22: return a_reportDisabled ? CloudAction::kReportDisabled : CloudAction::kForward;  // IsCloudEnabledForApp
            case 23: return CloudAction::kIgnore;       // SetCloudEnabledForApp
            case 30: return CloudAction::kFailCall;     // PublishWorkshopFile
            case 31: return CloudAction::kFailHandle;   // CreatePublishedFileUpdateRequest
            case 32: case 33: case 34: case 35: case 36: case 37:
                     return CloudAction::kFailBool;     // UpdatePublishedFile*
            case 38: return CloudAction::kFailCall;     // CommitPublishedFileUpdate
            case 40: return CloudAction::kFailCall;     // DeletePublishedFile
            case 42: return CloudAction::kFailCall;     // SubscribePublishedFile
            case 44: return CloudAction::kFailCall;     // UnsubscribePublishedFile
            case 45: return CloudAction::kFailBool;     // UpdatePublishedFileSetChangeDescription
            case 47: return CloudAction::kFailCall;     // UpdateUserPublishedItemVote
            case 50: return CloudAction::kFailCall;     // PublishVideo
            case 51: return CloudAction::kFailCall;     // SetUserPublishedFileAction
            case 57: case 58:
                     return CloudAction::kBatchOk;      // Begin/EndFileWriteBatch
            default: return CloudAction::kForward;
        }
    }

    // PulsarTestHarness.ini next to the DLL. The harness does nothing unless the
    // file exists AND says Enabled = 1, so a stray copy of the DLL is inert.
    struct Settings {
        bool        enabled = false;
        std::string documentsDir;      // replaces %USERPROFILE%\Documents
        std::string localAppDataDir;   // replaces %LOCALAPPDATA%
        std::string logFile;
        bool        renameMutexes = true;
        bool        hideOtherWindows = true;
        bool        blockCloudWrites = true;      // never let a test copy write the owner's Steam Cloud
        bool        reportCloudDisabled = true;   // tell the game cloud is off for this account and app
    };

    inline std::string Trim(std::string a_s)
    {
        const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
        while (!a_s.empty() && !notSpace(static_cast<unsigned char>(a_s.back()))) a_s.pop_back();
        std::size_t i = 0;
        while (i < a_s.size() && !notSpace(static_cast<unsigned char>(a_s[i]))) ++i;
        return a_s.substr(i);
    }

    inline std::string Lower(std::string a_s)
    {
        for (auto& c : a_s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return a_s;
    }

    inline bool ParseBool(const std::string& a_value, bool a_default)
    {
        const auto v = Lower(a_value);
        if (v == "1" || v == "true" || v == "yes" || v == "on") return true;
        if (v == "0" || v == "false" || v == "no" || v == "off") return false;
        return a_default;
    }

    inline Settings ParseSettings(std::istream& a_in)
    {
        Settings s;
        std::string line;
        while (std::getline(a_in, line)) {
            const auto semi = line.find_first_of(";#");
            if (semi != std::string::npos) line.resize(semi);
            const auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            const auto key   = Lower(Trim(line.substr(0, eq)));
            const auto value = Trim(line.substr(eq + 1));
            if (key == "enabled")               s.enabled = ParseBool(value, false);
            else if (key == "documentsdir")     s.documentsDir = value;
            else if (key == "localappdatadir")  s.localAppDataDir = value;
            else if (key == "logfile")          s.logFile = value;
            else if (key == "renamemutexes")    s.renameMutexes = ParseBool(value, true);
            else if (key == "hideotherwindows") s.hideOtherWindows = ParseBool(value, true);
            else if (key == "blockcloudwrites") s.blockCloudWrites = ParseBool(value, true);
            else if (key == "reportclouddisabled") s.reportCloudDisabled = ParseBool(value, true);
        }
        // Redirecting needs both folders; refusing to run is safer than sending
        // one of them to the owner's real folder.
        if (s.documentsDir.empty() || s.localAppDataDir.empty()) s.enabled = false;
        return s;
    }
}
