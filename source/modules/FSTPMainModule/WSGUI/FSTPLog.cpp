#include "FSTPLog.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>
#include <algorithm>

#if defined(_WIN32)
#  include <io.h>          // _isatty / _fileno
#  include <windows.h>
#  include <shellapi.h>    // ShellExecuteW
#  define FSTP_ISATTY(fd) _isatty(fd)
#  define FSTP_FILENO(f)  _fileno(f)
#else
#  include <unistd.h>      // isatty / fileno
#  define FSTP_ISATTY(fd) isatty(fd)
#  define FSTP_FILENO(f)  fileno(f)
#endif

namespace fs = std::filesystem;

namespace {

std::string g_log_dir;      // directory holding the logs
std::string g_log_path;     // current session log file
std::string g_app_version;  // "2026.01 \"Albatross\" Build 1853"
std::mutex  g_mutex;

// Reliable per-user log directory. getenv keeps this dependency-free; the chosen
// roots are always writable and immune to the OneDrive-Desktop redirect that broke
// the old --log.
std::string ResolveLogDir() {
#if defined(_WIN32)
    if (const char* la = std::getenv("LOCALAPPDATA"))
        return std::string(la) + "\\TapeXPlayer\\logs";
    if (const char* up = std::getenv("USERPROFILE"))
        return std::string(up) + "\\AppData\\Local\\TapeXPlayer\\logs";
    return "TapeXPlayer-logs";
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME"))
        return std::string(home) + "/Library/Logs/TapeXPlayer";
    return "TapeXPlayer-logs";
#else
    if (const char* xdg = std::getenv("XDG_STATE_HOME"))
        return std::string(xdg) + "/TapeXPlayer/logs";
    if (const char* home = std::getenv("HOME"))
        return std::string(home) + "/.local/state/TapeXPlayer/logs";
    return "TapeXPlayer-logs";
#endif
}

std::string NowStamp() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

std::string OSName() {
#if defined(_WIN32)
    return "Windows";
#elif defined(__APPLE__)
    return "macOS";
#elif defined(__linux__)
    return "Linux";
#else
    return "Unknown OS";
#endif
}

// Log file name for today: TapeXPlayer_YYYY-MM-DD.log.
std::string DatedLogName() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "TapeXPlayer_%Y-%m-%d.log", &tm);
    return buf;
}

// Existing daily logs, sorted oldest→newest (the date in the name sorts as plain text).
std::vector<std::string> ListDailyLogs() {
    std::error_code ec;
    std::vector<std::string> logs;
    for (auto& e : fs::directory_iterator(g_log_dir, ec)) {
        const std::string name = e.path().filename().string();
        if (name.rfind("TapeXPlayer_", 0) == 0 &&
            name.size() > 4 && name.substr(name.size() - 4) == ".log")
            logs.push_back(e.path().string());
    }
    std::sort(logs.begin(), logs.end());
    return logs;
}

// Keep the newest `keep` daily logs; delete older ones so the folder stays bounded.
void PruneOldLogs(int keep) {
    std::error_code ec;
    std::vector<std::string> logs = ListDailyLogs();
    for (size_t i = 0; i + (size_t)keep < logs.size(); ++i)
        fs::remove(logs[i], ec);
}

} // namespace

namespace FSTPLog {

void Init(bool force_file) {
    std::lock_guard<std::mutex> lk(g_mutex);

    std::error_code ec;
    g_log_dir = ResolveLogDir();
    fs::create_directories(g_log_dir, ec);   // best-effort; ignore ec, we fall back below

    // One log file per calendar day (TapeXPlayer_YYYY-MM-DD.log). Same-day sessions append,
    // each with its own header banner, so "send me the log for <date>" is unambiguous.
    g_log_path = (fs::path(g_log_dir) / DatedLogName()).string();
    PruneOldLogs(14);   // keep the last two weeks of daily logs

    // Keep the console for a developer running from a terminal; redirect to the file for
    // the normal (GUI, no-TTY) launch so every real user's session is captured.
    const bool interactive = FSTP_ISATTY(FSTP_FILENO(stdout)) != 0;
    if (interactive && !force_file) {
        WriteHeader();  // still drop a header into the file even when logging to console
        return;
    }

#if defined(_WIN32)
    FILE* f = nullptr;
    freopen_s(&f, g_log_path.c_str(), "a", stdout);
    freopen_s(&f, g_log_path.c_str(), "a", stderr);
#else
    if (!std::freopen(g_log_path.c_str(), "a", stdout)) {
        // Directory unwritable? fall back to a temp file so we never lose the log.
        g_log_path = (fs::temp_directory_path(ec) / DatedLogName()).string();
        std::freopen(g_log_path.c_str(), "a", stdout);
    }
    std::freopen(g_log_path.c_str(), "a", stderr);
#endif
    // Unbuffered so the log is complete even if the app is force-quit mid-run.
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    WriteHeader();
}

void WriteHeader() {
    std::fprintf(stdout,
        "================ TapeXPlayer session log ================\n"
        "Started : %s\n"
        "OS      : %s\n"
        "Version : %s\n"
        "Log     : %s\n"
        "=========================================================\n",
        NowStamp().c_str(),
        OSName().c_str(),
        g_app_version.empty() ? "(not yet set)" : g_app_version.c_str(),
        g_log_path.empty() ? "(console)" : g_log_path.c_str());
    std::fflush(stdout);
}

void SetAppVersion(const std::string& version, const std::string& build,
                   const std::string& codename) {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_app_version = version;
    if (!codename.empty()) g_app_version += " \"" + codename + "\"";
    if (!build.empty())    g_app_version += " Build " + build;
    std::fprintf(stdout, "[FSTPLog] Version: %s\n", g_app_version.c_str());
    std::fflush(stdout);
}

void LogSystemInfo(const std::string& info) {
    std::fprintf(stdout, "---- system info ----\n%s\n---------------------\n", info.c_str());
    std::fflush(stdout);
}

std::string GetLogDir()        { std::lock_guard<std::mutex> lk(g_mutex); return g_log_dir; }
std::string GetCurrentLogPath(){ std::lock_guard<std::mutex> lk(g_mutex); return g_log_path; }

void RevealLogFolder() {
    std::string dir = GetLogDir();
    if (dir.empty()) return;
#if defined(_WIN32)
    std::wstring wdir(dir.begin(), dir.end());
    ShellExecuteW(nullptr, L"open", wdir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    std::string cmd = "open \"" + dir + "\"";
    (void)std::system(cmd.c_str());
#else
    std::string cmd = "xdg-open \"" + dir + "\" >/dev/null 2>&1 &";
    (void)std::system(cmd.c_str());
#endif
}

bool SaveDiagnosticReport(const std::string& dest_path) {
    std::lock_guard<std::mutex> lk(g_mutex);
    std::error_code ec;

    std::ofstream out(dest_path, std::ios::binary | std::ios::trunc);
    if (!out) return false;

    out << "================ TapeXPlayer diagnostic report ================\n";
    out << "Saved   : " << NowStamp() << "\n";
    out << "OS      : " << OSName() << "\n";
    out << "Version : " << (g_app_version.empty() ? "(unknown)" : g_app_version) << "\n";
    out << "===============================================================\n\n";

    // The most recent OTHER daily log (usually yesterday) — handy if the bug was earlier.
    std::string prev;
    for (const std::string& p : ListDailyLogs())
        if (p != g_log_path) prev = p;   // sorted oldest→newest, so this keeps the newest other

    auto append_file = [&](const std::string& path, const char* label) {
        if (path.empty() || !fs::exists(path, ec)) return;
        out << "\n########## " << label << " (" << path << ") ##########\n";
        std::ifstream in(path, std::ios::binary);
        std::fflush(stdout);   // flush live log so the current file is up to date
        if (in) out << in.rdbuf();
        out << "\n";
    };

    append_file(prev, "PREVIOUS DAY");
    append_file(g_log_path, "CURRENT DAY");
    return out.good();
}

} // namespace FSTPLog
