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

    // PulsarTestHarness.ini next to the DLL. The harness does nothing unless the
    // file exists AND says Enabled = 1, so a stray copy of the DLL is inert.
    struct Settings {
        bool        enabled = false;
        std::string documentsDir;      // replaces %USERPROFILE%\Documents
        std::string localAppDataDir;   // replaces %LOCALAPPDATA%
        std::string logFile;
        bool        renameMutexes = true;
        bool        hideOtherWindows = true;
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
        }
        // Redirecting needs both folders; refusing to run is safer than sending
        // one of them to the owner's real folder.
        if (s.documentsDir.empty() || s.localAppDataDir.empty()) s.enabled = false;
        return s;
    }
}
