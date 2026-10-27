// Harness self-test: real processes against a stand-in game (pulsar_fakegame.exe)
// that has Starfield.exe's single-instance checks. coop-anything
// multi-instance-testing.md section 10: a check that cannot fail tests nothing,
// so every pass case has a twin with one part of the harness switched off.

#include "TestHarness.h"

#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace {
    // A temporary slot: harness DLL + settings file, private folders, reports.
    struct Slot {
        fs::path dir;

        explicit Slot(const std::string& a_tag, const std::string& a_settings)
        {
            dir = fs::temp_directory_path() / ("pulsar_harness_" + a_tag + "_" + std::to_string(GetCurrentProcessId()));
            fs::remove_all(dir);
            fs::create_directories(dir);
            fs::copy_file(PULSAR_HARNESS_DLL, dir / "PulsarTestHarness.dll");
            std::ofstream(dir / "PulsarTestHarness.ini")
                << "DocumentsDir = " << (dir / "Documents").string() << "\n"
                << "LocalAppDataDir = " << (dir / "LocalAppData").string() << "\n"
                << "LogFile = " << (dir / "harness.log").string() << "\n"
                << a_settings;
        }
        ~Slot() { std::error_code ec; fs::remove_all(dir, ec); }
    };

    struct Run {
        PROCESS_INFORMATION pi{};
        fs::path            report;
    };

    Run Start(const Slot& a_slot, const std::string& a_name, const std::string& a_report, bool a_plugin, DWORD a_holdMs,
              const std::string& a_extraArgs = {})
    {
        Run r;
        r.report = a_slot.dir / a_report;
        std::string cmd = std::string("\"") + PULSAR_FAKEGAME_EXE + "\" --name " + a_name +
                          " --report \"" + r.report.string() + "\" --hold " + std::to_string(a_holdMs);
        if (a_plugin) cmd += " --plugin \"" + a_slot.dir.string() + "\"";
        if (!a_extraArgs.empty()) cmd += " " + a_extraArgs;
        STARTUPINFOA si{ sizeof(si) };
        CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &r.pi);
        return r;
    }

    std::map<std::string, std::string> ReadReport(const fs::path& a_file)
    {
        std::map<std::string, std::string> kv;
        std::ifstream in(a_file);
        std::string line;
        while (std::getline(in, line)) {
            const auto eq = line.find('=');
            if (eq != std::string::npos) kv[line.substr(0, eq)] = line.substr(eq + 1);
        }
        return kv;
    }

    // Poll (never check once): wait until the copy reports ready, or time out.
    bool WaitReady(const Run& a_run, DWORD a_timeoutMs)
    {
        const auto start = GetTickCount64();
        while (GetTickCount64() - start < a_timeoutMs) {
            if (ReadReport(a_run.report)["result"] == "ready") return true;
            if (WaitForSingleObject(a_run.pi.hProcess, 0) == WAIT_OBJECT_0) return false;
            Sleep(20);
        }
        return false;
    }

    DWORD WaitExit(const Run& a_run, DWORD a_timeoutMs)
    {
        DWORD code = 0xFFFFFFFF;
        if (WaitForSingleObject(a_run.pi.hProcess, a_timeoutMs) == WAIT_OBJECT_0)
            GetExitCodeProcess(a_run.pi.hProcess, &code);
        return code;
    }

    // Only the processes this test started are ever stopped (pid-scoped).
    void Stop(Run& a_run)
    {
        if (!a_run.pi.hProcess) return;
        if (WaitForSingleObject(a_run.pi.hProcess, 0) != WAIT_OBJECT_0) TerminateProcess(a_run.pi.hProcess, 1);
        WaitForSingleObject(a_run.pi.hProcess, 5000);
        CloseHandle(a_run.pi.hThread);
        CloseHandle(a_run.pi.hProcess);
        a_run.pi = {};
    }

    // First copy holds; the second copy's exit code is the verdict.
    DWORD SecondCopyExit(const Slot& a_slot, const std::string& a_name, bool a_plugin,
                         std::map<std::string, std::string>* a_secondReport = nullptr)
    {
        Run first = Start(a_slot, a_name, "first.txt", a_plugin, 8000);
        if (!WaitReady(first, 10000)) { Stop(first); return 0xBAD; }
        Run second = Start(a_slot, a_name, "second.txt", a_plugin, 200);
        const DWORD code = WaitExit(second, 10000);
        if (a_secondReport) *a_secondReport = ReadReport(second.report);
        // Precondition: the first copy must still be running, or the second copy
        // simply ran alone and the verdict means nothing (a hollow pass).
        const bool firstStillRunning = WaitForSingleObject(first.pi.hProcess, 0) == WAIT_TIMEOUT;
        Stop(second);
        Stop(first);
        return firstStillRunning ? code : 0xBAD;
    }

    std::string Unique(const char* a_tag) { return std::string(a_tag) + std::to_string(GetTickCount64()); }

    // One copy that tries three cloud writes through the fake steam_api64.
    struct CloudResult {
        DWORD                              exitCode = 0xFFFFFFFF;
        std::map<std::string, std::string> report;
        std::string                        harnessLog;
    };

    CloudResult RunCloud(const Slot& a_slot, bool a_plugin,
                         const char* a_version = "STEAMREMOTESTORAGE_INTERFACE_VERSION016")
    {
        CloudResult r;
        Run run = Start(a_slot, Unique("cloud"), "cloud.txt", a_plugin, 0, std::string("--cloud ") + a_version);
        r.exitCode = WaitExit(run, 10000);
        Stop(run);
        r.report = ReadReport(run.report);
        std::ifstream log(a_slot.dir / "harness.log");
        std::stringstream ss;
        ss << log.rdbuf();
        r.harnessLog = ss.str();
        return r;
    }
}

namespace {
    // Two copies that both create PULSAR's issue #1 guard mutex; returns what the
    // SECOND copy reported ("enabled" = co-op stays on).
    std::string SecondCopyPulsarCoop(const Slot& a_slot, bool a_plugin)
    {
        const auto name = Unique("pm");
        Run first = Start(a_slot, name, "pm1.txt", a_plugin, 8000, "--pulsarmutex 1");
        if (!WaitReady(first, 10000)) { Stop(first); return "first-not-ready"; }
        // Use a different --name so the stand-in's own mutex/window checks do not stop it.
        Run second = Start(a_slot, name + "b", "pm2.txt", a_plugin, 200, "--pulsarmutex 1");
        WaitExit(second, 10000);
        const bool firstAlive = WaitForSingleObject(first.pi.hProcess, 0) == WAIT_TIMEOUT;
        auto rep = ReadReport(second.report);
        Stop(second);
        Stop(first);
        return firstAlive ? rep["pulsarcoop"] : "first-exited";
    }
}

TEST(HarnessSelfTest, PulsarCoopGuardIsRenamedPerTestCopy)
{
    Slot slot("pm", "Enabled = 1\n");
    CHECK_EQ(SecondCopyPulsarCoop(slot, true), std::string("enabled"));
}

TEST(HarnessSelfTest, TwinWithoutHarnessPulsarGuardDisablesSecondCopy)
{
    Slot slot("pmoff", "Enabled = 1\n");
    CHECK_EQ(SecondCopyPulsarCoop(slot, false), std::string("disabled"));
}

TEST(HarnessSelfTest, TwinRenamePulsarMutexOffKeepsTheGuard)
{
    Slot slot("pmkeep", "Enabled = 1\nRenamePulsarMutex = 0\n");
    CHECK_EQ(SecondCopyPulsarCoop(slot, true), std::string("disabled"));
}

TEST(HarnessSelfTest, CloudWritesAreBlockedAndCloudLooksDisabled)
{
    Slot slot("cloud", "Enabled = 1\n");
    const auto r = RunCloud(slot, true);
    CHECK_EQ(r.exitCode, DWORD(0));
    CHECK_EQ(r.report.at("cloudwritesreached"), std::string("0"));
    CHECK_EQ(r.report.at("cloudwrite"), std::string("0"));
    CHECK_EQ(r.report.at("cloudwriteasync"), std::string("0"));
    CHECK_EQ(r.report.at("clouddelete"), std::string("0"));
    CHECK_EQ(r.report.at("cloudappenabled"), std::string("0"));
    CHECK(r.harnessLog.find("BLOCKED Steam Cloud FileWrite(\"Save1_test.sfs\"") != std::string::npos);
    // The read-only probe went through the fake's own slots 18/19.
    CHECK(r.harnessLog.find("GetFileCount=2") != std::string::npos);
    CHECK(r.harnessLog.find("fake_save_a.sfs") != std::string::npos);
}

TEST(HarnessSelfTest, TwinWithoutHarnessCloudWritesGetThrough)
{
    Slot slot("cloudoff", "Enabled = 1\n");
    const auto r = RunCloud(slot, false);
    CHECK_EQ(r.exitCode, DWORD(0));
    CHECK_EQ(r.report.at("cloudwritesreached"), std::string("3"));
    CHECK_EQ(r.report.at("cloudappenabled"), std::string("1"));
}

TEST(HarnessSelfTest, TwinBlockCloudWritesOffLetsWritesThrough)
{
    Slot slot("cloudallowed", "Enabled = 1\nBlockCloudWrites = 0\n");
    const auto r = RunCloud(slot, true);
    CHECK_EQ(r.report.at("cloudwritesreached"), std::string("3"));
}

TEST(HarnessSelfTest, ReportCloudDisabledOffStillBlocksWrites)
{
    Slot slot("cloudreport", "Enabled = 1\nReportCloudDisabled = 0\n");
    const auto r = RunCloud(slot, true);
    CHECK_EQ(r.report.at("cloudwritesreached"), std::string("0"));
    CHECK_EQ(r.report.at("cloudappenabled"), std::string("1"));
}

TEST(HarnessSelfTest, UnknownRemoteStorageVersionStopsTheCopy)
{
    Slot slot("cloudver", "Enabled = 1\n");
    const auto r = RunCloud(slot, true, "STEAMREMOTESTORAGE_INTERFACE_VERSION015");
    CHECK_EQ(r.exitCode, DWORD(0xC10D));
    CHECK(r.report.find("cloudwritesreached") == r.report.end());   // never got to write
    CHECK(r.harnessLog.find("unknown interface") != std::string::npos);
}

TEST(HarnessSelfTest, WithHarnessTwoCopiesRunAndFoldersAreRedirected)
{
    Slot slot("on", "Enabled = 1\n");
    std::map<std::string, std::string> report;
    CHECK_EQ(SecondCopyExit(slot, Unique("on"), true, &report), DWORD(0));
    CHECK_EQ(report["documents"], (slot.dir / "Documents").string());
    CHECK_EQ(report["localappdata"], (slot.dir / "LocalAppData").string());
    CHECK_EQ(report["envlocalappdata"], (slot.dir / "LocalAppData").string());   // CRT environment too
    CHECK(fs::exists(slot.dir / "Documents"));
    CHECK(fs::exists(slot.dir / "harness.log"));
}

TEST(HarnessSelfTest, TwinWithoutHarnessTheSecondCopyIsBlocked)
{
    Slot slot("off", "Enabled = 1\n");
    std::map<std::string, std::string> report;
    CHECK_EQ(SecondCopyExit(slot, Unique("off"), false, &report), DWORD(3));   // mutex check
    CHECK(report["documents"] != (slot.dir / "Documents").string());            // real folder
    CHECK(report["envlocalappdata"] != (slot.dir / "LocalAppData").string());   // real environment
}

TEST(HarnessSelfTest, TwinDisabledSettingsMakeTheHarnessInert)
{
    Slot slot("disabled", "Enabled = 0\n");
    std::map<std::string, std::string> report;
    CHECK_EQ(SecondCopyExit(slot, Unique("disabled"), true, &report), DWORD(3));
    CHECK(report["documents"] != (slot.dir / "Documents").string());
    CHECK(!fs::exists(slot.dir / "harness.log"));
}

TEST(HarnessSelfTest, TwinWithoutMutexRenameTheMutexStillBlocks)
{
    Slot slot("nomutex", "Enabled = 1\nRenameMutexes = 0\n");
    CHECK_EQ(SecondCopyExit(slot, Unique("nomutex"), true), DWORD(3));
}

TEST(HarnessSelfTest, TwinWithoutWindowHidingTheWindowStillBlocks)
{
    Slot slot("nowindow", "Enabled = 1\nHideOtherWindows = 0\n");
    CHECK_EQ(SecondCopyExit(slot, Unique("nowindow"), true), DWORD(4));
}
