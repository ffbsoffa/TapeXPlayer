#pragma once

// macOS: the app version/build read from the .app bundle's Info.plist. Available from
// process start (no SDL, no menu bar), so FSTPLog::Init can stamp the real version into
// the session banner instead of deferring it. Returns "" if the key is missing.

#include <string>

std::string FSTPGetBundleVersion();
std::string FSTPGetBundleBuild();

// Exact macOS version incl. build id, e.g. "15.7.7 (24G720)". "" if unavailable.
std::string FSTPGetOSVersion();
