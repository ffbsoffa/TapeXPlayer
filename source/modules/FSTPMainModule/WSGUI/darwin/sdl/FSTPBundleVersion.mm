// Reads the app version straight out of the .app bundle's Info.plist.
//
// macOS has no generated BuildInfo.h (its BuildInfo is Swift, unreachable from C++), so the
// session logger used to wait for InitToolsMenu to hand the version over — which meant a
// crash before the Tools menu was built produced a log stamped "Version : (not yet set)",
// exactly when the log mattered most. NSBundle is live from process start, long before SDL
// or any menu, so FSTPLog::Init can just ask for the version up front.

#import <Foundation/Foundation.h>
#include "FSTPBundleVersion.h"

std::string FSTPGetBundleVersion() {
    @autoreleasepool {
        NSString* v = [[NSBundle mainBundle]
            objectForInfoDictionaryKey:@"CFBundleShortVersionString"];
        return v ? std::string([v UTF8String]) : std::string();
    }
}

std::string FSTPGetBundleBuild() {
    @autoreleasepool {
        NSString* b = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleVersion"];
        return b ? std::string([b UTF8String]) : std::string();
    }
}

std::string FSTPGetOSVersion() {
    @autoreleasepool {
        NSOperatingSystemVersion v = [[NSProcessInfo processInfo] operatingSystemVersion];
        std::string s = std::to_string((long)v.majorVersion) + "." +
                        std::to_string((long)v.minorVersion) + "." +
                        std::to_string((long)v.patchVersion);
        // The build id (e.g. 24G720) is what actually pins down a point release, and Apple
        // only exposes it through this private-ish plist — worth the read for a bug report.
        NSDictionary* d = [NSDictionary dictionaryWithContentsOfFile:
            @"/System/Library/CoreServices/SystemVersion.plist"];
        NSString* build = d[@"ProductBuildVersion"];
        if (build) s += " (" + std::string([build UTF8String]) + ")";
        return s;
    }
}
