// Test-only harness plugin decisions (tests/harness/HarnessPolicy.h).

#include "TestHarness.h"

#include "harness/HarnessPolicy.h"

#include <sstream>

using namespace Pulsar::TestHarness;

namespace {
    Settings Parse(const std::string& a_ini)
    {
        std::istringstream in(a_ini);
        return ParseSettings(in);
    }
}

TEST(HarnessPolicy, CsidlDocumentsAndLocalAppDataAreRedirected)
{
    CHECK(FolderFromCsidl(0x0005) == Folder::kDocuments);
    CHECK(FolderFromCsidl(0x001C) == Folder::kLocalAppData);
    // CSIDL_FLAG_CREATE (0x8000) is a flag, not a different folder (SFSE's log uses it).
    CHECK(FolderFromCsidl(0x0005 | 0x8000) == Folder::kDocuments);
    CHECK(FolderFromCsidl(0x001C | 0x8000) == Folder::kLocalAppData);
}

TEST(HarnessPolicy, OtherCsidlsPassThrough)
{
    CHECK(FolderFromCsidl(0x001A) == Folder::kNone);   // CSIDL_APPDATA (roaming)
    CHECK(FolderFromCsidl(0x0028) == Folder::kNone);   // CSIDL_PROFILE
    CHECK(FolderFromCsidl(0x0026) == Folder::kNone);   // CSIDL_PROGRAM_FILES
}

TEST(HarnessPolicy, KnownFolderIdsAreRecognised)
{
    CHECK(FolderFromKnownFolder(kFolderIdDocuments) == Folder::kDocuments);
    CHECK(FolderFromKnownFolder(kFolderIdLocalAppData) == Folder::kLocalAppData);
    // FOLDERID_RoamingAppData {3EB685DB-65F9-4CF6-A03A-E3EF65729F3D} is not redirected.
    const Guid roaming{ 0x3EB685DB, 0x65F9, 0x4CF6, { 0xA0, 0x3A, 0xE3, 0xEF, 0x65, 0x72, 0x9F, 0x3D } };
    CHECK(FolderFromKnownFolder(roaming) == Folder::kNone);
    // One byte off is a different folder.
    Guid nearly = kFolderIdDocuments;
    nearly.d4[7] ^= 1;
    CHECK(FolderFromKnownFolder(nearly) == Folder::kNone);
}

TEST(HarnessPolicy, MutexNamesBecomePerProcess)
{
    CHECK_EQ(UniqueMutexName("StarfieldMutex", 1234), std::string("StarfieldMutex-pulsartest-1234"));
    CHECK(UniqueMutexName("X", 1) != UniqueMutexName("X", 2));
    CHECK_EQ(UniqueMutexName("X", 7), UniqueMutexName("X", 7));   // consistent inside one process
    CHECK_EQ(UniqueMutexName("", 7), std::string());               // unnamed stays unnamed
}

TEST(HarnessPolicy, OnlyPulsarsCoopMutexIsRecognised)
{
    CHECK(IsPulsarCoopMutex(L"Local\\PULSAR_CoopSingleInstance"));
    CHECK(IsPulsarCoopMutex(L"local\\pulsar_coopsingleinstance"));
    CHECK(!IsPulsarCoopMutex(L"Local\\PULSAR_CoopSingleInstanceX"));
    CHECK(!IsPulsarCoopMutex(L"Global\\PULSAR_CoopSingleInstance"));
    CHECK(!IsPulsarCoopMutex(L"Local\\SteamOverlayMutex"));
    CHECK(!IsPulsarCoopMutex(L""));
}

TEST(HarnessPolicy, OnlyOtherProcessesWindowsAreHidden)
{
    CHECK(ShouldHideWindow(200, 100));
    CHECK(!ShouldHideWindow(100, 100));   // the game's own window
    CHECK(!ShouldHideWindow(0, 100));     // nothing found
}

TEST(HarnessPolicy, EveryCloudWriteSlotIsBlocked)
{
    // v016 slots that change the owner's cloud or Workshop state.
    const int writes[] = { 0, 2, 5, 6, 7, 8, 9, 10, 11, 12, 23, 30, 31, 32, 33, 34, 35, 36, 37,
                           38, 40, 42, 44, 45, 47, 50, 51 };
    for (int slot : writes)
        CHECK(CloudActionFor(slot, true) != CloudAction::kForward);
    CHECK(CloudActionFor(0, true) == CloudAction::kFailBool);      // FileWrite
    CHECK(CloudActionFor(2, true) == CloudAction::kFailCall);      // FileWriteAsync
    CHECK(CloudActionFor(9, true) == CloudAction::kFailHandle);    // FileWriteStreamOpen
}

TEST(HarnessPolicy, CloudReadsAreForwarded)
{
    // FileRead, FileReadAsync(+Complete), FileExists, FilePersisted, GetFileSize,
    // GetFileTimestamp, GetSyncPlatforms, GetFileCount, GetFileNameAndSize, GetQuota.
    const int reads[] = { 1, 3, 4, 13, 14, 15, 16, 17, 18, 19, 20 };
    for (int slot : reads)
        CHECK(CloudActionFor(slot, true) == CloudAction::kForward);
}

TEST(HarnessPolicy, CloudEnabledReportingIsASwitch)
{
    CHECK(CloudActionFor(kRsIsCloudEnabledForAccount, true) == CloudAction::kReportDisabled);
    CHECK(CloudActionFor(kRsIsCloudEnabledForApp, true) == CloudAction::kReportDisabled);
    CHECK(CloudActionFor(kRsIsCloudEnabledForAccount, false) == CloudAction::kForward);
    CHECK(CloudActionFor(kRsIsCloudEnabledForApp, false) == CloudAction::kForward);
    // Writes stay blocked either way.
    CHECK(CloudActionFor(kRsFileWrite, false) == CloudAction::kFailBool);
}

TEST(HarnessPolicy, CloudSettingsDefaultOnAndCanBeSwitchedOff)
{
    const auto d = Parse("Enabled = 1\nDocumentsDir = C:\\a\nLocalAppDataDir = C:\\b\n");
    CHECK(d.blockCloudWrites);
    CHECK(d.reportCloudDisabled);
    const auto s = Parse("Enabled = 1\nDocumentsDir = C:\\a\nLocalAppDataDir = C:\\b\n"
                         "BlockCloudWrites = 0\nReportCloudDisabled = off\n");
    CHECK(!s.blockCloudWrites);
    CHECK(!s.reportCloudDisabled);
}

TEST(HarnessPolicy, WorkingSetCapIsOffUnlessSetSensibly)
{
    const std::string base = "Enabled = 1\nDocumentsDir = C:\\a\nLocalAppDataDir = C:\\b\n";
    CHECK_EQ(Parse(base).maxWorkingSetMB, 0u);
    CHECK_EQ(Parse(base + "MaxWorkingSetMB = 6144\n").maxWorkingSetMB, 6144u);
    CHECK_EQ(Parse(base + "MaxWorkingSetMB = 12\n").maxWorkingSetMB, 0u);      // too small: off
    CHECK_EQ(Parse(base + "MaxWorkingSetMB = lots\n").maxWorkingSetMB, 0u);    // junk: off
}

TEST(HarnessPolicy, MissingOrDisabledSettingsAreInert)
{
    CHECK(!Parse("").enabled);
    CHECK(!Parse("Enabled = 0\nDocumentsDir = C:\\a\nLocalAppDataDir = C:\\b\n").enabled);
    // Enabled but a folder missing: refuse, never redirect only half.
    CHECK(!Parse("Enabled = 1\nDocumentsDir = C:\\a\n").enabled);
    CHECK(!Parse("Enabled = 1\nLocalAppDataDir = C:\\b\n").enabled);
}

TEST(HarnessPolicy, SettingsAreRead)
{
    const auto s = Parse(
        "; test slot sf-a\n"
        "Enabled = 1\n"
        "DocumentsDir = C:\\slots\\sf-a\\profile\\Documents\n"
        "localappdatadir=C:\\slots\\sf-a\\profile\\LocalAppData  \r\n"
        "LogFile = C:\\slots\\sf-a\\logs\\harness.log ; inline comment\n"
        "RenameMutexes = off\n"
        "garbage line\n");
    CHECK(s.enabled);
    CHECK_EQ(s.documentsDir, std::string("C:\\slots\\sf-a\\profile\\Documents"));
    CHECK_EQ(s.localAppDataDir, std::string("C:\\slots\\sf-a\\profile\\LocalAppData"));
    CHECK_EQ(s.logFile, std::string("C:\\slots\\sf-a\\logs\\harness.log"));
    CHECK(!s.renameMutexes);
    CHECK(s.hideOtherWindows);
}

TEST(HarnessPolicy, CommentMarksInsideAPathStay)
{
    // ';' and '#' are legal in Windows paths; only one at the line start or after a
    // space begins a comment, so the redirect never lands in a cut-off folder.
    const auto s = Parse(
        "Enabled = 1\n"
        "DocumentsDir = C:\\dev\\C#\\sf-a;x\\Documents\n"
        "LocalAppDataDir = C:\\dev\\C#\\sf-a\\LocalAppData # trailing comment\n"
        "# DocumentsDir = C:\\not-this-one\n");
    CHECK(s.enabled);
    CHECK_EQ(s.documentsDir, std::string("C:\\dev\\C#\\sf-a;x\\Documents"));
    CHECK_EQ(s.localAppDataDir, std::string("C:\\dev\\C#\\sf-a\\LocalAppData"));
}
