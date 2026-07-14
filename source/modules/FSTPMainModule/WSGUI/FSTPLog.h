#pragma once

// FSTPLog — always-on session logging.
//
// The old approach (--log → freopen to %USERPROFILE%\Desktop) was opt-in and broke
// on OneDrive-redirected desktops. This writes a rolling session log to a reliable
// per-user location on EVERY launch, so a bug report already has a log — no flag, no
// hunting for the file. The UI exposes "Open Log Folder" / "Save Diagnostic Report".
//
// Log location:
//   Windows : %LOCALAPPDATA%\TapeXPlayer\logs\
//   macOS   : ~/Library/Logs/TapeXPlayer/
//   Linux   : $XDG_STATE_HOME/TapeXPlayer/logs/  (fallback ~/.local/state/…)
// One file PER SESSION: TapeXPlayer_YYYY-MM-DD_HH-MM-SS_<pid>.log. Each launch is its own
// self-contained file (never appended across runs); the newest 20 are kept.

#include <string>

namespace FSTPLog {

// Initialise the session log. Call ONCE, as early in main() as possible.
//  - resolves + creates the log directory,
//  - opens this session's own log file and prunes all but the newest 20,
//  - resolves the app version (BuildInfo.h on Windows/Linux, Info.plist on macOS) and
//    writes the header banner,
//  - redirects stdout+stderr into the log file, so std::cout / printf / ffmpeg / SDL
//    output is all captured.
// A file is ALWAYS written, terminal or not — "Save Diagnostic Report" has to have
// something to export no matter how the app was launched. When stdout is a terminal the
// output is additionally mirrored to the console, so a dev running from a shell keeps
// their live output as well.
// force_file (--log): skip the console mirroring and capture straight to the file, i.e.
// behave exactly as a GUI launch does.
void Init(bool force_file);

// Re-write the header banner. Init already writes one; this is only for a caller that
// wants a fresh banner mid-session. Call LogSystemInfo() separately once hardware/display
// info is available.
void WriteHeader();

// Override the version Init resolved on its own. Rarely needed — only for a caller that
// knows better than BuildInfo.h / Info.plist. Logged, not silently applied.
void SetAppVersion(const std::string& version, const std::string& build,
                   const std::string& codename);

// One-time dump of detected hardware (GPU, decoders, displays) — call after the
// hardware-detection stage so the log is self-describing for bug reports.
void LogSystemInfo(const std::string& info);

// Absolute path to the log directory / current log file (empty if init failed).
std::string GetLogDir();
std::string GetCurrentLogPath();

// Open the log folder in the OS file manager (Explorer / Finder / xdg-open).
void RevealLogFolder();

// Export THIS session's log — everything the program has written from launch up to now —
// into one self-describing text file at dest_path (a location the user picked). Returns
// true on success.
bool SaveDiagnosticReport(const std::string& dest_path);

// Default file name to offer in the save dialog, stamped with the current date and time:
// "TapeXPlayer-diagnostic_2026-07-14_18-30-05.txt". Reports pile up in one folder and get
// mailed around, so the name has to say when it was taken without opening it.
std::string SuggestedReportName();

} // namespace FSTPLog
