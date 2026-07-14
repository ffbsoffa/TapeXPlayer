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
#include <iostream>
#include <thread>
#include <chrono>

#if defined(_WIN32)
#  include <io.h>          // _isatty / _fileno / _dup2
#  include <process.h>     // _getpid
#  include <windows.h>
#  include <shellapi.h>    // ShellExecuteW
#  include "windows/BuildInfo.h"
#  define FSTP_ISATTY(fd)   _isatty(fd)
#  define FSTP_FILENO(f)    _fileno(f)
#  define FSTP_GETPID()     _getpid()
#  define FSTP_DUP2(a, b)   _dup2(a, b)
#else
#  include <unistd.h>      // isatty / fileno / getpid / dup2
#  define FSTP_ISATTY(fd)   isatty(fd)
#  define FSTP_FILENO(f)    fileno(f)
#  define FSTP_GETPID()     getpid()
#  define FSTP_DUP2(a, b)   dup2(a, b)
#  if defined(__linux__)
#    include <sys/utsname.h>   // uname (exact kernel version for the banner)
#    include "linux/BuildInfo.h"
#  elif defined(__APPLE__)
#    include "darwin/sdl/FSTPBundleVersion.h"
#  endif
#endif

namespace fs = std::filesystem;

namespace {

std::string g_log_dir;      // directory holding the logs
std::string g_log_path;     // current session log file ("" when logging to a console)
std::string g_app_version;  // "2026.01 \"Albatross\" Build 1853"
bool        g_to_console = false;      // true when output stays on the terminal
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

// Local time formatted with `fmt`. The localtime_s/localtime_r split lives here only, so a
// future change (UTC, a different clock) is one edit rather than one-per-call-site.
std::string FormatNow(const char* fmt) {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), fmt, &tm);
    return buf;
}

std::string NowStamp() { return FormatNow("%Y-%m-%d %H:%M:%S"); }

// OS name AND exact version — "macOS 15.7.7 (24G720)", "Windows 10.0.19045", "Linux 6.8.0".
// A bug report that says only "macOS" is a bug report you have to write back about.
std::string OSName() {
#if defined(_WIN32)
    // GetVersionEx lies for unmanifested apps; RtlGetVersion reports the real build.
    typedef LONG (WINAPI *RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);
    if (HMODULE hMod = GetModuleHandleW(L"ntdll.dll")) {
        auto rtl = (RtlGetVersionPtr)GetProcAddress(hMod, "RtlGetVersion");
        if (rtl) {
            RTL_OSVERSIONINFOW vi{};
            vi.dwOSVersionInfoSize = sizeof(vi);
            if (rtl(&vi) == 0) {
                return "Windows " + std::to_string(vi.dwMajorVersion) + "." +
                       std::to_string(vi.dwMinorVersion) + "." +
                       std::to_string(vi.dwBuildNumber);
            }
        }
    }
    return "Windows";
#elif defined(__APPLE__)
    // ProductVersion + ProductBuildVersion, e.g. "macOS 15.7.7 (24G720)".
    std::string v = FSTPGetOSVersion();
    return v.empty() ? "macOS" : "macOS " + v;
#elif defined(__linux__)
    struct utsname u{};
    if (uname(&u) == 0) {
        std::string s = std::string("Linux ") + u.release;
        // Prepend the distro's pretty name when /etc/os-release has one.
        std::ifstream rel("/etc/os-release");
        std::string line;
        while (std::getline(rel, line)) {
            if (line.rfind("PRETTY_NAME=", 0) == 0) {
                std::string name = line.substr(12);
                if (!name.empty() && name.front() == '"') name = name.substr(1);
                if (!name.empty() && name.back()  == '"') name.pop_back();
                if (!name.empty()) return name + " (kernel " + u.release + ")";
                break;
            }
        }
        return s;
    }
    return "Linux";
#else
    return "Unknown OS";
#endif
}

// Per-session log name: TapeXPlayer_YYYY-MM-DD_HH-MM-SS_<pid>.log. Each launch gets its
// own file — never appended to — so "the log from that run" is one unambiguous file. The
// pid keeps it unique even if two instances start in the same second (the date+time alone
// sorts chronologically as plain text; the pid suffix doesn't disturb that ordering).
std::string SessionLogName() {
    return FormatNow("TapeXPlayer_%Y-%m-%d_%H-%M-%S")
         + "_" + std::to_string((long)FSTP_GETPID()) + ".log";
}

// True only for names this module generates: TapeXPlayer_YYYY-MM-DD_HH-MM-SS_<pid>.log.
// Deliberately strict — pruning deletes whatever this matches, and a bare
// "TapeXPlayer_*.log" test would also sweep away files the user put here on purpose
// (a saved TapeXPlayer_crash.log, a log kept aside for a bug report) as well as legacy
// per-day TapeXPlayer_YYYY-MM-DD.log files from before the per-session scheme.
bool IsSessionLogName(const std::string& name) {
    // TapeXPlayer_ 2026-07-14 _ 12-34-56 _ <pid> .log
    static const char kPrefix[] = "TapeXPlayer_";
    const size_t kPrefixLen = sizeof(kPrefix) - 1;
    if (name.rfind(kPrefix, 0) != 0) return false;
    if (name.size() < kPrefixLen + 22) return false;          // shortest possible pid is 1 digit
    if (name.compare(name.size() - 4, 4, ".log") != 0) return false;

    const std::string stem = name.substr(kPrefixLen, name.size() - kPrefixLen - 4);
    // stem must be YYYY-MM-DD_HH-MM-SS_<pid>
    auto digits = [&](size_t at, size_t n) {
        if (at + n > stem.size()) return false;
        for (size_t i = at; i < at + n; ++i)
            if (stem[i] < '0' || stem[i] > '9') return false;
        return true;
    };
    if (!digits(0, 4) || stem[4] != '-' || !digits(5, 2) || stem[7] != '-' || !digits(8, 2))
        return false;                                          // date
    if (stem[10] != '_') return false;
    if (!digits(11, 2) || stem[13] != '-' || !digits(14, 2) || stem[16] != '-' || !digits(17, 2))
        return false;                                          // time
    if (stem[19] != '_') return false;
    return digits(20, stem.size() - 20) && stem.size() > 20;   // pid
}

// Session logs this module wrote, sorted oldest→newest (the timestamp sorts as plain text).
std::vector<std::string> ListSessionLogs() {
    std::error_code ec;
    std::vector<std::string> logs;
    for (auto& e : fs::directory_iterator(g_log_dir, ec)) {
        if (IsSessionLogName(e.path().filename().string()))
            logs.push_back(e.path().string());
    }
    std::sort(logs.begin(), logs.end());
    return logs;
}

// Terminal session: send output to BOTH the console and the log file, by piping through
// `tee`. The dev keeps their live console, and the session still produces a file that
// "Save Diagnostic Report" can export — which is the whole reason the log exists.
//
// popen gives us tee's stdin; freopen'ing stdout onto that pipe means C-runtime, SDL and
// ffmpeg output is mirrored too, not just std::cout. tee inherits the real terminal as its
// own stdout, so it writes there while appending to the file. Returns false if tee can't
// be started, in which case the caller falls back to a plain file.
bool MirrorToConsole(const std::string& path) {
#if defined(_WIN32)
    (void)path;
    return false;   // no tee on Windows; a GUI launch has no console to mirror to anyway
#else
    // Quote the path for the shell: ' -> '\'' inside a single-quoted string.
    std::string quoted = "'";
    for (char c : path) { if (c == '\'') quoted += "'\\''"; else quoted += c; }
    quoted += "'";

    FILE* pipe = popen(("tee -a " + quoted).c_str(), "w");
    if (!pipe) return false;

    // Point stdout at tee's stdin, then alias stderr onto the same fd (one offset, correct
    // interleaving — see OpenLogStream for why two independent handles are unsafe).
    if (FSTP_DUP2(FSTP_FILENO(pipe), FSTP_FILENO(stdout)) == -1) { pclose(pipe); return false; }
    if (FSTP_DUP2(FSTP_FILENO(stdout), FSTP_FILENO(stderr)) == -1) { pclose(pipe); return false; }
    // The pipe FILE* is intentionally leaked: it must outlive Init, and tee exits on its own
    // when the process ends and the write end closes.
    return true;
#endif
}

// Point stdout AND stderr at `path`, returning false if it can't be opened.
//
// stderr is aliased onto stdout's file descriptor rather than opened separately: two
// handles on one file keep independent offsets and would overwrite each other's bytes
// (append mode hides it, dup2 removes the hazard outright). Sharing one fd also means
// stdout/stderr interleave in true chronological order — an ffmpeg warning lands between
// the two log lines it actually occurred between, which is the whole point of a bug-report
// log. freopen is still what redirects stdout, so C-runtime/SDL/ffmpeg output is captured
// too, not just std::cout.
bool OpenLogStream(const std::string& path) {
#if defined(_WIN32)
    FILE* f = nullptr;
    if (freopen_s(&f, path.c_str(), "a", stdout) != 0 || !f)
        return false;   // Windows used to ignore this and silently discard every log line
#else
    if (!std::freopen(path.c_str(), "a", stdout))
        return false;
#endif
    if (FSTP_DUP2(FSTP_FILENO(stdout), FSTP_FILENO(stderr)) == -1) {
        // dup2 failed (vanishingly rare). Fall back to a second handle in APPEND mode:
        // O_APPEND makes each write atomically seek to EOF, so the streams can't clobber
        // one another even with separate offsets.
#if defined(_WIN32)
        FILE* e = nullptr;
        freopen_s(&e, path.c_str(), "a", stderr);
#else
        std::freopen(path.c_str(), "a", stderr);
#endif
    }
    return true;
}

// Keep the newest `keep` session logs; delete older ones so the folder stays bounded.
void PruneOldLogs(int keep) {
    std::error_code ec;
    std::vector<std::string> logs = ListSessionLogs();
    for (size_t i = 0; i + (size_t)keep < logs.size(); ++i)
        fs::remove(logs[i], ec);
}

// The running app's version, resolved at startup on every platform. Windows/Linux compile
// it in via BuildInfo.h; macOS has no C-visible BuildInfo (its one is Swift), so it comes
// from the .app bundle's Info.plist — which NSBundle serves from process start, no GUI
// needed. Knowing the version this early is what lets the banner be written synchronously
// with the real version in it, instead of being deferred and racing whoever sets it later.
std::string ResolveAppVersion() {
#if defined(_WIN32) || defined(__linux__)
    std::string v = GetTapeXPlayerVersion();
    const std::string codename = GetTapeXPlayerCodeName();
    const std::string build    = GetTapeXPlayerBuildNumber();
    if (!codename.empty()) v += " \"" + codename + "\"";
    if (!build.empty())    v += " Build " + build;
    return v;
#elif defined(__APPLE__)
    // CFBundleShortVersionString already reads "Albatross (Build 1855)" here, so only
    // append CFBundleVersion when it isn't already spelled out in there.
    std::string v = FSTPGetBundleVersion();
    const std::string build = FSTPGetBundleBuild();
    if (!build.empty() && v.find(build) == std::string::npos)
        v += " Build " + build;
    return v;
#else
    return std::string();
#endif
}

} // namespace

namespace FSTPLog {

void WriteHeaderLocked();   // defined below; caller must hold g_mutex

void Init(bool force_file) {
    std::lock_guard<std::mutex> lk(g_mutex);

    std::error_code ec;
    g_log_dir = ResolveLogDir();
    fs::create_directories(g_log_dir, ec);   // best-effort; the fallbacks below cover failure

    g_app_version = ResolveAppVersion();

    // ALWAYS write a file, terminal or not. An earlier cut skipped the file whenever stdout
    // was a TTY, on the theory that a dev running from a shell only wants their console —
    // but that silently broke the feature's whole point: launch from a terminal, hit "Save
    // Diagnostic Report", and the report was empty because no file was ever written. A
    // terminal session additionally MIRRORS to the console (see MirrorToConsole), so the dev
    // keeps their live output and still gets a report they can send.
    //
    // force_file (--log) opts out of the mirroring: capture straight to the file exactly as
    // a GUI launch would, even from a shell.
    const bool interactive = FSTP_ISATTY(FSTP_FILENO(stdout)) != 0 && !force_file;

    // One log file per SESSION (TapeXPlayer_YYYY-MM-DD_HH-MM-SS_<pid>.log): each run is a
    // single self-contained file, so "send me the log from that run" is unambiguous.
    g_log_path = (fs::path(g_log_dir) / SessionLogName()).string();

    // "a", not "w": the pid already guarantees a fresh file, and only append mode is safe
    // here. stderr is pointed at stdout's fd below, but if that ever fails we fall back to
    // two handles on one path — and two "w" handles keep INDEPENDENT offsets and overwrite
    // each other's bytes, whereas O_APPEND makes every write atomically seek to EOF.
    // A terminal session mirrors to the console AND the file; a GUI session just writes the
    // file. If mirroring can't start, fall through to the plain file — the file is the part
    // that must not be lost.
    bool opened = interactive ? MirrorToConsole(g_log_path) : false;
    if (!opened) opened = OpenLogStream(g_log_path);

    if (!opened) {
        // Log dir unwritable (locked-down profile, AV, disk full, redirected folder — the
        // very cases this module exists for)? Fall back to temp rather than lose the log.
        g_log_path = (fs::temp_directory_path(ec) / SessionLogName()).string();
        if (!OpenLogStream(g_log_path)) {
            g_to_console = true;   // nowhere to write: leave the streams as they are
            g_log_path.clear();
            WriteHeaderLocked();
            return;
        }
    }
    PruneOldLogs(20);   // only now, on the path that actually created a file
    // Flush every line promptly (and survive a force-quit). Two layers matter: std::cout on
    // libc++/macOS buffers at the C++ level independent of the C stdout FILE*, so setvbuf alone
    // left recent lines stuck — `unitbuf` makes cout/cerr flush each write; line-buffering the
    // FILE* then turns those into one disk write per newline instead of per-operation.
    std::setvbuf(stdout, nullptr, _IOLBF, BUFSIZ);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    // Safety net: setvbuf/unitbuf don't reliably keep the on-disk file current on every C++
    // runtime (libc++/macOS block-buffers the redirected file regardless of the mode we ask
    // for), which makes a live log look "stale" — lines only land in ~4 KB bursts. A tiny
    // detached thread fflushes a few times a second, so the file is never more than ~250 ms
    // behind, even without a clean exit. fflush() is thread-safe.
    std::thread([]{
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            std::fflush(stdout);
            std::fflush(stderr);
        }
    }).detach();

    WriteHeaderLocked();   // version is already resolved, so the banner is correct first time
}

// Write the banner. Caller must hold g_mutex.
void WriteHeaderLocked() {
    std::fprintf(stdout,
        "================ TapeXPlayer session log ================\n"
        "Started : %s\n"
        "OS      : %s\n"
        "Version : %s\n"
        "Log     : %s\n"
        "=========================================================\n",
        NowStamp().c_str(),
        OSName().c_str(),
        g_app_version.empty() ? "(unknown)" : g_app_version.c_str(),
        g_to_console ? "(console)" : g_log_path.c_str());
    std::fflush(stdout);
}

void WriteHeader() {
    std::lock_guard<std::mutex> lk(g_mutex);
    WriteHeaderLocked();
}

void SetAppVersion(const std::string& version, const std::string& build,
                   const std::string& codename) {
    std::lock_guard<std::mutex> lk(g_mutex);
    // Init already resolved the version for the banner; this only exists for a caller that
    // knows better (or more) than BuildInfo/Info.plist did. Log it so the override is on
    // the record rather than silently changing what later readers assume.
    std::string v = version;
    if (!codename.empty()) v += " \"" + codename + "\"";
    if (!build.empty())    v += " Build " + build;
    if (v == g_app_version) return;   // nothing new to say
    g_app_version = v;
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

    // Export ONLY this session's log — everything the program has written from launch up to
    // the moment of this request. Flush the live log first so nothing recent is missing.
    std::fflush(stdout);
    std::fflush(stderr);

    if (g_to_console) {
        out << "(this session logs to the console — run without a terminal, or pass --log,\n"
               " to capture output to a file that can be exported here)\n";
        return out.good();
    }
    if (g_log_path.empty() || !fs::exists(g_log_path, ec)) {
        out << "(no session log file — the log directory could not be opened)\n";
        return out.good();
    }

    // Just one line of our own, then the session log verbatim. The log already opens with a
    // banner carrying the build, OS and its own path, so repeating any of that here would
    // only make the reader wonder which of the two headers to believe.
    out << "TapeXPlayer diagnostic report — saved " << NowStamp() << "\n\n";
    std::ifstream in(g_log_path, std::ios::binary);
    if (in) out << in.rdbuf();
    return out.good();
}

} // namespace FSTPLog
