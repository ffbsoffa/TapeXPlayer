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
// Files: TapeXPlayer.log (this session) + TapeXPlayer.prev.log (the one before).

#include <string>

namespace FSTPLog {

// Initialise the session log. Call ONCE, as early in main() as possible.
//  - resolves + creates the log directory,
//  - rotates the previous session's log aside,
//  - redirects stdout+stderr into the log file (so std::cout / printf / ffmpeg / SDL
//    output is all captured), UNLESS attached to an interactive terminal — a dev
//    running from a shell keeps their console.
// force_file: capture to the file even on a TTY (the old `--log` behaviour).
void Init(bool force_file);

// Append a self-describing header (build, OS, CPU) to the current log. Called by Init;
// call LogSystemInfo() separately once hardware/display info is available.
void WriteHeader();

// Log the app version + code name (whoever knows it can hand it over — the header is
// written before this is available on some platforms).
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

// Copy this + the previous session log into one combined, self-describing text file
// at dest_path (a location the user picked). Returns true on success.
bool SaveDiagnosticReport(const std::string& dest_path);

} // namespace FSTPLog
