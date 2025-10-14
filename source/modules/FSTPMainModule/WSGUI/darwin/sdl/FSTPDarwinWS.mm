#ifdef __APPLE__

#import <Cocoa/Cocoa.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import <SDL2/SDL.h>
#import <dispatch/dispatch.h>
#import <CoreVideo/CoreVideo.h>
#include <cmath>
#include <portaudio.h>
#include <atomic>
#include <thread>
#include <chrono>
#include <iostream>
#include <condition_variable>
#include <mutex>
#include "FSTPDarwinWS.h"
#include "FSTPOSDSystem.h"
#include "FSTPSettings.h"
#include "FSTPPlayerManager.h"
#include "FSTPWindowManager.h"
#include "FSTPPixelBufferManager.h"
#include "FSTPKeyboard.h"
#include "FSTPOSDInstance.h"
#include "FSTPToolsMenu.h"

// Debug control: set to true to enable verbose logging
[[maybe_unused]] static constexpr bool ENABLE_DARWIN_WS_DEBUG = false;

// Window system implementation for macOS (Darwin)
// SDL integration with native Cocoa capabilities

// Delegate for settings window close handling
@interface SettingsWindowDelegate : NSObject <NSWindowDelegate>
@end

// Flag to prevent multiple dialog invocations
static bool g_dialog_open = false;

void ShowNativeFileDialog(int target_player_id) {
    // Prevent multiple dialog invocations
    if (g_dialog_open) {
        NSLog(@"Dialog already open, skipping call");
        return;
    }

    g_dialog_open = true;
    NSOpenPanel* openPanel = [NSOpenPanel openPanel];
    [openPanel setCanChooseFiles:YES];
    [openPanel setCanChooseDirectories:NO];
    [openPanel setAllowsMultipleSelection:NO];
    if (@available(macOS 11.0, *)) {
        NSArray<UTType*>* allowedTypes = @[
            // Video formats
            [UTType typeWithFilenameExtension:@"mp4"],
            [UTType typeWithFilenameExtension:@"mov"],
            [UTType typeWithFilenameExtension:@"avi"],
            [UTType typeWithFilenameExtension:@"mkv"],
            // Audio formats
            [UTType typeWithFilenameExtension:@"mp3"],
            [UTType typeWithFilenameExtension:@"wav"],
            [UTType typeWithFilenameExtension:@"flac"],
            [UTType typeWithFilenameExtension:@"aac"],
            [UTType typeWithFilenameExtension:@"m4a"],
            [UTType typeWithFilenameExtension:@"ogg"]
        ];
        [openPanel setAllowedContentTypes:allowedTypes];
    } else {
        #pragma clang diagnostic push
        #pragma clang diagnostic ignored "-Wdeprecated-declarations"
        [openPanel setAllowedFileTypes:@[@"mp4", @"mov", @"avi", @"mkv", @"mp3", @"wav", @"flac", @"aac", @"m4a", @"ogg"]];
        #pragma clang diagnostic pop
    }

    if ([openPanel runModal] == NSModalResponseOK) {
        NSURL* url = [[openPanel URLs] objectAtIndex:0];
        NSString* path = [url path];

        // FIX: Determine player_id based on parameter
        int active_player_id;
        if (target_player_id >= 0) {
            // Open for specific instance (don't return focus to main window!)
            active_player_id = target_player_id;
            NSLog(@"Opening file for specific player instance: %d", target_player_id);
        } else {
            // Auto-detect: return focus to main window and get active player
            RestoreFocusToMainWindow();
            active_player_id = GetActivePlayerID();
            NSLog(@"Opening file for active player: %d", active_player_id);
        }

        const char* file_path = [path UTF8String];

        NSLog(@"Loading file '%@' into player %d", path, active_player_id);
        NSLog(@"File path C string: '%s'", file_path);
        NSLog(@"Active player ID: %d", active_player_id);

        // Check that path is not empty
        if (!file_path || strlen(file_path) == 0) {
            NSLog(@"Error: File path is empty!");
            return;
        }

        // FIX: Set initial loading state IMMEDIATELY (synchronously)
        // so user sees feedback before async loading starts
        // Detailed progress (0-100%) will be updated from FSTPVideoModule_wrapper.cpp

        // Reset OSD data (timecode, duration) to clear previous file
        UpdateOSDPosition(active_player_id, 0.0, 0.0);
        UpdateOSDPlayState(active_player_id, false, false, false);

        // CRITICAL: Set loading flag BEFORE mode!
        // This is needed so IsPlayerLoading() returns true and render skips old video texture
        SetPlayerLoadingState(active_player_id, true);
        UpdateOSDDisplayMode(active_player_id, OSD_MODE_LOADING);
        SetPlayerLoadingProgress(active_player_id, 0);
        SetPlayerLoadingStatus(active_player_id, "threading");

        // FIX: Clear old video texture to show black screen
        FSTPPixelBufferManager* pixel_mgr = GetPixelBufferManager();
        if (pixel_mgr) {
            pixel_mgr->ClearPlayerBuffers(active_player_id);
        }

        // Save file path and player ID for passing to async block
        NSString* filePathString = [path copy];
        int saved_player_id = active_player_id; // Save player ID
        int saved_target_player_id = target_player_id; // Save target to determine if focus is needed

        dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
            // Load file into player
            const char* file_path_cstr = [filePathString UTF8String];
            NSLog(@"Loading file with path: '%s'", file_path_cstr);

            // Use saved player ID
            int player_id = saved_player_id;

            int instance_id = -1;

            // Check if player instance is already active
            if (player_id >= 0 && IsPlayerInstanceActive(player_id)) {
                // Load file into existing instance
                NSLog(@"Loading file into existing player instance %d", player_id);
                instance_id = LoadFileIntoPlayerInstance(file_path_cstr, player_id);
            } else {
                // Create new player instance
                NSLog(@"Creating new player instance for player %d", player_id);
                instance_id = CreatePlayerInstance(file_path_cstr, player_id);
            }

            // Return to main thread for completion
            dispatch_async(dispatch_get_main_queue(), ^{
                if (instance_id >= 0) {
                    NSLog(@"File successfully loaded into player instance %d", instance_id);

                    // Find window index for OSD update
                    int window_index = -1;
                    for (int i = 0; i < MAX_WINDOWS; i++) {
                        FSTPWindow* window = GetWindowByIndex(i);
                        if (window && window->player_instance_id == player_id) {
                            window_index = i;
                            break;
                        }
                    }

                    if (window_index >= 0) {
                        // Get file info and update OSD for specific window
                        double duration = GetInstanceDuration(instance_id);
                        UpdateWindowOSD(window_index, 0.0, duration, false, 1.0, false);

                        NSLog(@"OSD updated for window %d (player %d), duration: %.2f sec", window_index, instance_id, duration);

                        // FIX: If file was loaded for specific instance (not auto-detect),
                        // switch focus to that window after loading completes
                        if (saved_target_player_id >= 0) {
                            FSTPWindow* window = GetWindowByIndex(window_index);
                            if (window && window->window) {
                                SDL_RaiseWindow(window->window);
                                NSLog(@"Focus switched to window %d after file loading", window_index);
                            }
                        }
                    }
                } else {
                    NSLog(@"File loading error: %d", instance_id);
                    // On error return OSD to normal mode
                    SetPlayerLoadingState(saved_player_id, false);
                    UpdateOSDDisplayMode(saved_player_id, OSD_MODE_NO_FILE);
                }
                // FIX: Successful loading completion is handled in FSTPVideoModule_wrapper.cpp (lines 251-252)
                // Don't do anything here to avoid overwriting state
            });
        });
    } else {
        // User cancelled file selection - also return focus
        RestoreFocusToMainWindow();
    }

    // Reset flag after dialog closes
    g_dialog_open = false;
}

// Global variables for windows
static NSWindow* g_settingsWindow = nil;
static NSTabView* g_settingsTabView = nil;

// UI elements for audio settings
static NSPopUpButton* g_audioDevicePopup = nil;
static NSPopUpButton* g_bufferSizePopup = nil;
static NSSlider* g_volumeSlider = nil;
static NSTextField* g_volumeValueLabel = nil;

// UI elements for video and sync settings
static NSStepper* g_frameOffsetStepper = nil;
static NSTextField* g_frameOffsetTextField = nil;
static NSButton* g_autoFreezeCheckbox = nil;

// UI elements for MIDI settings
static NSButton* g_midiEnabledCheckbox = nil;
static NSPopUpButton* g_midiInputPopup = nil;
static NSPopUpButton* g_midiOutputPopup = nil;

// Settings window delegate implementation
@implementation SettingsWindowDelegate
- (void)windowWillClose:(NSNotification *)notification {
    NSLog(@"⚙️ Settings window closing...");

    // Return focus to main SDL window
    RestoreFocusToMainWindow();

    // Reset global variables
    g_settingsWindow = nil;
    g_settingsTabView = nil;
    g_audioDevicePopup = nil;
    g_bufferSizePopup = nil;
    g_volumeSlider = nil;
    g_volumeValueLabel = nil;
    g_frameOffsetStepper = nil;
    g_frameOffsetTextField = nil;
    g_autoFreezeCheckbox = nil;
    g_midiEnabledCheckbox = nil;
    g_midiInputPopup = nil;
    g_midiOutputPopup = nil;
}
@end

// Helper functions for creating settings UI elements
static NSTextField* CreateLabel(NSString* text, NSRect frame) {
    NSTextField* label = [[NSTextField alloc] initWithFrame:frame];
    [label setStringValue:text];
    [label setBezeled:NO];
    [label setDrawsBackground:NO];
    [label setEditable:NO];
    [label setSelectable:NO];
    return label;
}

__attribute__((unused)) static NSButton* CreateCheckbox(NSString* title, NSRect frame) {
    NSButton* checkbox = [[NSButton alloc] initWithFrame:frame];
    [checkbox setTitle:title];
    [checkbox setButtonType:NSButtonTypeSwitch];
    return checkbox;
}

static NSPopUpButton* CreatePopupButton(NSRect frame, NSArray<NSString*>* items) {
    NSPopUpButton* popup = [[NSPopUpButton alloc] initWithFrame:frame pullsDown:NO];
    for (NSString* item in items) {
        [popup addItemWithTitle:item];
    }
    return popup;
}

static NSSlider* CreateSlider(NSRect frame, double minValue, double maxValue) {
    NSSlider* slider = [[NSSlider alloc] initWithFrame:frame];
    [slider setMinValue:minValue];
    [slider setMaxValue:maxValue];
    return slider;
}

// Create settings window (simplified)
static void CreateSettingsWindow() {
    if (g_settingsWindow != nil) return;

    // Create settings window (smaller size)
    g_settingsWindow = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 500, 400)
                                                   styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                                     backing:NSBackingStoreBuffered
                                                       defer:NO];

    [g_settingsWindow setTitle:@"TapeXPlayer Settings"];
    [g_settingsWindow center];

    // Set delegate for window close handling
    SettingsWindowDelegate* settingsDelegate = [[SettingsWindowDelegate alloc] init];
    [g_settingsWindow setDelegate:settingsDelegate];

    NSView* contentView = [g_settingsWindow contentView];

    // Create TabView for settings categories
    g_settingsTabView = [[NSTabView alloc] initWithFrame:NSMakeRect(20, 60, 460, 300)];
    [contentView addSubview:g_settingsTabView];

    // === "Audio" Tab ===
    NSTabViewItem* audioTab = [[NSTabViewItem alloc] initWithIdentifier:@"audio"];
    [audioTab setLabel:@"Audio"];
    NSView* audioView = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 440, 260)];

    // Audio Device
    [audioView addSubview:CreateLabel(@"Audio Device:", NSMakeRect(20, 220, 150, 20))];
    g_audioDevicePopup = CreatePopupButton(NSMakeRect(180, 218, 240, 25), @[@"Default Audio Device"]);

    // Fill audio devices list
    PaDeviceIndex deviceCount = Pa_GetDeviceCount();
    for (PaDeviceIndex i = 0; i < deviceCount; i++) {
        const PaDeviceInfo* deviceInfo = Pa_GetDeviceInfo(i);
        if (deviceInfo && deviceInfo->maxOutputChannels > 0) {
            NSString* deviceName = [NSString stringWithUTF8String:deviceInfo->name];
            [g_audioDevicePopup addItemWithTitle:deviceName];
            [[g_audioDevicePopup lastItem] setTag:i];
        }
    }

    // Set current device
    int currentDevice = GetAudioDeviceIndex();
    [g_audioDevicePopup selectItemWithTag:currentDevice];
    [audioView addSubview:g_audioDevicePopup];

    // Master Volume
    [audioView addSubview:CreateLabel(@"Master Volume:", NSMakeRect(20, 180, 150, 20))];
    g_volumeSlider = CreateSlider(NSMakeRect(180, 178, 200, 25), 0.0, 1.0);
    g_volumeSlider.doubleValue = GetMasterVolume();
    [audioView addSubview:g_volumeSlider];

    // Label for displaying volume value
    g_volumeValueLabel = CreateLabel([NSString stringWithFormat:@"%.0f%%", GetMasterVolume() * 100], NSMakeRect(390, 180, 50, 20));
    [audioView addSubview:g_volumeValueLabel];

    // Update volume value when slider changes
    g_volumeSlider.target = [NSApp delegate];
    g_volumeSlider.action = @selector(volumeSliderChanged:);

    // Buffer Size
    [audioView addSubview:CreateLabel(@"Buffer Size:", NSMakeRect(20, 140, 150, 20))];
    g_bufferSizePopup = CreatePopupButton(NSMakeRect(180, 138, 150, 25), @[@"512", @"1024", @"2048", @"4096"]);

    // Set tags for buffer elements
    for (NSMenuItem* item in g_bufferSizePopup.itemArray) {
        int bufferSize = [item.title intValue];
        item.tag = bufferSize;
    }

    // Set current buffer size
    int currentBufferSize = GetAudioBufferSize();
    [g_bufferSizePopup selectItemWithTag:currentBufferSize];
    [audioView addSubview:g_bufferSizePopup];

    [audioTab setView:audioView];
    [g_settingsTabView addTabViewItem:audioTab];

    // === "Video & Sync" Tab ===
    NSTabViewItem* videoTab = [[NSTabViewItem alloc] initWithIdentifier:@"video"];
    [videoTab setLabel:@"Video & Sync"];
    NSView* videoView = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 440, 260)];

    // Frame Offset (to compensate for monitor delay)
    [videoView addSubview:CreateLabel(@"Frame Offset:", NSMakeRect(20, 220, 200, 20))];
    [videoView addSubview:CreateLabel(@"(-10 to +10 frames)", NSMakeRect(20, 200, 200, 16))];

    g_frameOffsetTextField = [[NSTextField alloc] initWithFrame:NSMakeRect(230, 218, 60, 24)];
    [g_frameOffsetTextField setIntValue:GetFrameOffset()];
    [g_frameOffsetTextField setEditable:NO];
    [g_frameOffsetTextField setAlignment:NSTextAlignmentCenter];
    [videoView addSubview:g_frameOffsetTextField];

    g_frameOffsetStepper = [[NSStepper alloc] initWithFrame:NSMakeRect(300, 218, 20, 24)];
    [g_frameOffsetStepper setMinValue:-10];
    [g_frameOffsetStepper setMaxValue:10];
    [g_frameOffsetStepper setIntValue:GetFrameOffset()];
    [g_frameOffsetStepper setTarget:[NSApp delegate]];
    [g_frameOffsetStepper setAction:@selector(frameOffsetStepperChanged:)];
    [videoView addSubview:g_frameOffsetStepper];

    // Auto-freeze inactive players (protection from forgotten players)
    g_autoFreezeCheckbox = CreateCheckbox(@"Auto-freeze inactive players", NSMakeRect(20, 160, 350, 24));
    [g_autoFreezeCheckbox setState:(GetAutoFreezeInactive() ? NSControlStateValueOn : NSControlStateValueOff)];
    [videoView addSubview:g_autoFreezeCheckbox];

    [videoView addSubview:CreateLabel(@"(prevents forgotten players from consuming resources)",
                                      NSMakeRect(20, 140, 400, 16))];

    [videoTab setView:videoView];
    [g_settingsTabView addTabViewItem:videoTab];

    // === "MIDI" Tab ===
    NSTabViewItem* midiTab = [[NSTabViewItem alloc] initWithIdentifier:@"midi"];
    [midiTab setLabel:@"MIDI"];
    NSView* midiView = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 440, 260)];

    // MIDI Enabled checkbox
    g_midiEnabledCheckbox = CreateCheckbox(@"Enable MIDI Controller", NSMakeRect(20, 220, 250, 24));
    [g_midiEnabledCheckbox setState:(GetMIDIEnabled() ? NSControlStateValueOn : NSControlStateValueOff)];
    [midiView addSubview:g_midiEnabledCheckbox];

    // MIDI Input Port
    [midiView addSubview:CreateLabel(@"Input Port:", NSMakeRect(20, 180, 150, 20))];
    g_midiInputPopup = CreatePopupButton(NSMakeRect(180, 178, 240, 25), @[@"(None)"]);

    // Fill MIDI input ports list
    int inputDeviceCount = GetMIDIInputDeviceCount();
    for (int i = 0; i < inputDeviceCount; i++) {
        const char* deviceName = GetMIDIInputDeviceName(i);
        if (deviceName) {
            NSString* name = [NSString stringWithUTF8String:deviceName];
            [g_midiInputPopup addItemWithTitle:name];
            [[g_midiInputPopup lastItem] setTag:i];  // Save index
        }
    }

    int currentInputPort = GetMIDIInputPort();
    if (currentInputPort >= 0) {
        [g_midiInputPopup selectItemWithTag:currentInputPort];
    } else {
        [g_midiInputPopup selectItemAtIndex:0];  // "(None)"
    }
    [midiView addSubview:g_midiInputPopup];

    // MIDI Output Port
    [midiView addSubview:CreateLabel(@"Output Port:", NSMakeRect(20, 140, 150, 20))];
    g_midiOutputPopup = CreatePopupButton(NSMakeRect(180, 138, 240, 25), @[@"(None)"]);

    // Fill MIDI output ports list
    int outputDeviceCount = GetMIDIOutputDeviceCount();
    for (int i = 0; i < outputDeviceCount; i++) {
        const char* deviceName = GetMIDIOutputDeviceName(i);
        if (deviceName) {
            NSString* name = [NSString stringWithUTF8String:deviceName];
            [g_midiOutputPopup addItemWithTitle:name];
            [[g_midiOutputPopup lastItem] setTag:i];  // Save index
        }
    }

    int currentOutputPort = GetMIDIOutputPort();
    if (currentOutputPort >= 0) {
        [g_midiOutputPopup selectItemWithTag:currentOutputPort];
    } else {
        [g_midiOutputPopup selectItemAtIndex:0];  // "(None)"
    }
    [midiView addSubview:g_midiOutputPopup];

    // Description
    [midiView addSubview:CreateLabel(@"Supports Mackie HUI / X-Touch One",
                                     NSMakeRect(20, 100, 400, 16))];
    [midiView addSubview:CreateLabel(@"Configure controller in HUI/Mackie mode",
                                     NSMakeRect(20, 80, 400, 16))];

    [midiTab setView:midiView];
    [g_settingsTabView addTabViewItem:midiTab];

    // === Control buttons (standard macOS alignment) ===
    NSButton* okButton = [[NSButton alloc] initWithFrame:NSMakeRect(400, 20, 80, 30)];
    [okButton setTitle:@"OK"];
    [okButton setButtonType:NSButtonTypeMomentaryPushIn];
    [okButton setTarget:[NSApp delegate]];
    [okButton setAction:@selector(settingsOKClicked:)];
    [contentView addSubview:okButton];

    NSButton* cancelButton = [[NSButton alloc] initWithFrame:NSMakeRect(310, 20, 80, 30)];
    [cancelButton setTitle:@"Cancel"];
    [cancelButton setButtonType:NSButtonTypeMomentaryPushIn];
    [cancelButton setTarget:[NSApp delegate]];
    [cancelButton setAction:@selector(settingsCancelClicked:)];
    [contentView addSubview:cancelButton];

    NSButton* resetButton = [[NSButton alloc] initWithFrame:NSMakeRect(20, 20, 120, 30)];
    [resetButton setTitle:@"Reset to Default"];
    [resetButton setButtonType:NSButtonTypeMomentaryPushIn];
    [resetButton setTarget:[NSApp delegate]];
    [resetButton setAction:@selector(settingsResetClicked:)];
    [contentView addSubview:resetButton];
}

void ShowNativeSettingsDialog() {
    @autoreleasepool {
        // Initialize settings system
        InitSettings();

        // Create settings window if it doesn't exist yet
        CreateSettingsWindow();

        // Show window
        [g_settingsWindow makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];

        NSLog(@"Settings dialog opened");
    }
}

// Old ShowAboutWindow() function removed - using SwiftUI version

void CreateNativeMenu() {
    @autoreleasepool {
        // Create main menu
        NSMenu* mainMenu = [[NSMenu alloc] initWithTitle:@"TapeXPlayer"];

        // Application menu
        NSMenuItem* appMenuItem = [[NSMenuItem alloc] init];
        NSMenu* appMenu = [[NSMenu alloc] initWithTitle:@"TapeXPlayer"];

        // About menu item
        NSMenuItem* aboutItem = [[NSMenuItem alloc] initWithTitle:@"About TapeXPlayer"
                                                           action:@selector(showAboutWindow:)
                                                    keyEquivalent:@""];
        [appMenu addItem:aboutItem];

        [appMenu addItem:[NSMenuItem separatorItem]];

        // Preferences menu item
        NSMenuItem* preferencesItem = [[NSMenuItem alloc] initWithTitle:@"Preferences..."
                                                                 action:@selector(showPreferences:)
                                                          keyEquivalent:@","];
        [appMenu addItem:preferencesItem];

        [appMenu addItem:[NSMenuItem separatorItem]];

        // Quit menu item
        NSMenuItem* quitItem = [[NSMenuItem alloc] initWithTitle:@"Quit TapeXPlayer"
                                                          action:@selector(terminate:)
                                                   keyEquivalent:@"q"];
        [quitItem setTarget:NSApp];
        [appMenu addItem:quitItem];

        [appMenuItem setSubmenu:appMenu];
        [mainMenu addItem:appMenuItem];

        // File menu
        NSMenuItem* fileMenuItem = [[NSMenuItem alloc] init];
        NSMenu* fileMenu = [[NSMenu alloc] initWithTitle:@"File"];

        NSMenuItem* newWindowItem = [[NSMenuItem alloc] initWithTitle:@"New Window"
                                                               action:@selector(newWindow:)
                                                        keyEquivalent:@"n"];
        [fileMenu addItem:newWindowItem];

        NSMenuItem* openItem = [[NSMenuItem alloc] initWithTitle:@"Open..."
                                                          action:@selector(openFile:)
                                                   keyEquivalent:@"o"];
        [fileMenu addItem:openItem];

        NSMenuItem* openInNewItem = [[NSMenuItem alloc] initWithTitle:@"Open in New Instance..."
                                                               action:@selector(openInNewInstance:)
                                                        keyEquivalent:@"o"];
        [openInNewItem setKeyEquivalentModifierMask:NSEventModifierFlagCommand | NSEventModifierFlagShift];
        [fileMenu addItem:openInNewItem];

        [fileMenuItem setSubmenu:fileMenu];
        [mainMenu addItem:fileMenuItem];

        // "Window" menu
        NSMenuItem* windowMenuItem = [[NSMenuItem alloc] init];
        NSMenu* windowMenu = [[NSMenu alloc] initWithTitle:@"Window"];

        // New Window (duplicate from File menu for convenience)
        NSMenuItem* newWindowInWindowMenu = [[NSMenuItem alloc] initWithTitle:@"New Window"
                                                                       action:@selector(newWindow:)
                                                                keyEquivalent:@"n"];
        [newWindowInWindowMenu setKeyEquivalentModifierMask:NSEventModifierFlagCommand | NSEventModifierFlagShift];
        [windowMenu addItem:newWindowInWindowMenu];

        [windowMenu addItem:[NSMenuItem separatorItem]];

        // Minimize
        NSMenuItem* minimizeItem = [[NSMenuItem alloc] initWithTitle:@"Minimize"
                                                              action:@selector(performMiniaturize:)
                                                       keyEquivalent:@"m"];
        [windowMenu addItem:minimizeItem];

        // Zoom
        NSMenuItem* zoomItem = [[NSMenuItem alloc] initWithTitle:@"Zoom"
                                                          action:@selector(performZoom:)
                                                   keyEquivalent:@""];
        [windowMenu addItem:zoomItem];

        [windowMenu addItem:[NSMenuItem separatorItem]];

        // Bring All to Front
        NSMenuItem* bringAllToFrontItem = [[NSMenuItem alloc] initWithTitle:@"Bring All to Front"
                                                                     action:@selector(arrangeInFront:)
                                                              keyEquivalent:@""];
        [bringAllToFrontItem setTarget:NSApp];
        [windowMenu addItem:bringAllToFrontItem];

        [windowMenu addItem:[NSMenuItem separatorItem]];

        // Window list will be automatically added by system
        [windowMenuItem setSubmenu:windowMenu];
        [mainMenu addItem:windowMenuItem];

        // Set Window menu for automatic window list management
        [NSApp setWindowsMenu:windowMenu];

        // Set main menu
        [NSApp setMainMenu:mainMenu];

        // Initialize tools menu
        InitToolsMenu();
    }
}

void HandleNativeAppEvents() {
    // Throttling: no more than 60 times/sec (every ~16ms)
    static uint64_t last_call_time = 0;
    uint64_t now = SDL_GetTicks64();

    if (now - last_call_time < 16) { // 16ms = ~60 FPS
        return; // Skip if called too early
    }
    last_call_time = now;

    @autoreleasepool {
        // Safe NSApp check before event processing
        if (!NSApp || ![NSApp isKindOfClass:[NSApplication class]]) {
            return; // NSApp not initialized or corrupted
        }

        @try {
            // Process Cocoa events without blocking SDL
            NSEvent* event;
            while ((event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                               untilDate:[NSDate distantPast]
                                                  inMode:NSDefaultRunLoopMode
                                                 dequeue:YES])) {
                if (event) {
                    [NSApp sendEvent:event];
                }
            }
        }
        @catch (NSException* exception) {
            // If something went wrong with AppKit, just ignore
            NSLog(@"HandleNativeAppEvents exception: %@", exception);
        }
    }
}

@interface AppDelegate : NSObject <NSApplicationDelegate>
- (IBAction)showAboutWindow:(id)sender;
- (IBAction)newWindow:(id)sender;
- (IBAction)openFile:(id)sender;
- (IBAction)openInNewInstance:(id)sender;
- (IBAction)settingsOKClicked:(id)sender;
- (IBAction)settingsCancelClicked:(id)sender;
- (IBAction)settingsResetClicked:(id)sender;
- (IBAction)volumeSliderChanged:(id)sender;
- (IBAction)frameOffsetStepperChanged:(id)sender;
- (IBAction)performMiniaturize:(id)sender;
- (IBAction)performZoom:(id)sender;
- (IBAction)copyScreenshotAction:(id)sender;
- (IBAction)showMemoryLocationsAction:(id)sender;
- (IBAction)showInspectorAction:(id)sender;
@end

@implementation AppDelegate
- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    CreateNativeMenu();
}

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
    // Correct termination through special event
    SDL_Event quitEvent;
    quitEvent.type = SDL_USEREVENT;
    quitEvent.user.code = 1; // Code for "real" application termination
    SDL_PushEvent(&quitEvent);
    return NSTerminateCancel; // Cancel system termination, let SDL handle it
}

- (IBAction)showPreferences:(id)sender {
    // Use SwiftUI settings window (macOS 13.0+ Ventura)
    if (@available(macOS 13.0, *)) {
        ShowSwiftUISettingsDialog();
    } else {
        // Fallback for older macOS versions
        ShowNativeSettingsDialog();
    }
}

- (IBAction)showAboutWindow:(id)sender {
    // Use SwiftUI About window (macOS 13.0+)
    if (@available(macOS 13.0, *)) {
        ShowSwiftUIAboutWindow();
    } else {
        // Fallback for older macOS versions - show simple window
        NSAlert* alert = [[NSAlert alloc] init];
        [alert setMessageText:@"TapeXPlayer 2026"];
        [alert setInformativeText:@"Professional Video Playback System\n\nDeveloped by Maksim Maloletkin (FFB_soffa)\nhttps://apps.ffbsoffa.org/tapexplayer\n\nLicensed under GPL (GNU General Public License)\nCopyright © 2025 Maksim Maloletkin\n\nThis software uses FFmpeg, SDL2, PortAudio, RtMidi, OpenSSL and Apple frameworks."];
        [alert addButtonWithTitle:@"OK"];
        [alert runModal];
    }
}

- (IBAction)newWindow:(id)sender {
    // Create new window with automatic player binding
    int window_index = CreateNewWindow("TapeXPlayer 2026 - New Player", 1280, 720);
    if (window_index >= 0) {
        NSLog(@"New window created with index %d", window_index);
    } else {
        NSLog(@"Failed to create new window: error %d", window_index);
    }
}

- (IBAction)openFile:(id)sender {
    ShowNativeFileDialog();
}

- (IBAction)openInNewInstance:(id)sender {
    // Create new window
    int window_index = CreateNewWindow("TapeXPlayer 2026 - New Player", 1280, 720);
    if (window_index >= 0) {
        int player_id = window_index;  // 1:1 binding window → player
        NSLog(@"New window %d created, opening file dialog for player %d", window_index, player_id);
        // FIX: Pass player_id so file loads into new instance
        ShowNativeFileDialog(player_id);
    } else {
        NSLog(@"Failed to create new window: error %d", window_index);
    }
}

- (IBAction)settingsOKClicked:(id)sender {
    NSLog(@"Settings OK clicked - saving settings");

    bool bufferSizeChanged = false;

    // Save settings from UI elements
    if (g_audioDevicePopup) {
        NSMenuItem* selectedDevice = [g_audioDevicePopup selectedItem];
        int deviceIndex = (int)[selectedDevice tag];
        FSTPSettings* settings = GetSettings();
        settings->audio_device_index = deviceIndex;
        NSLog(@"Audio device set to: %d", deviceIndex);
    }
    
    if (g_bufferSizePopup) {
        NSMenuItem* selectedBuffer = [g_bufferSizePopup selectedItem];
        int bufferSize = (int)[selectedBuffer tag];
        FSTPSettings* settings = GetSettings();

        // Check if buffer size changed
        if (settings->audio_buffer_size != bufferSize) {
            bufferSizeChanged = true;
        }

        settings->audio_buffer_size = bufferSize;
        NSLog(@"Buffer size set to: %d", bufferSize);
    }

    if (g_volumeSlider) {
        float volume = (float)[g_volumeSlider doubleValue];
        FSTPSettings* settings = GetSettings();
        settings->audio_master_volume = volume;
        NSLog(@"Master volume set to: %.2f", volume);
    }

    // Save video and sync settings
    if (g_frameOffsetStepper) {
        int frameOffset = (int)[g_frameOffsetStepper intValue];
        FSTPSettings* settings = GetSettings();
        settings->frame_offset = frameOffset;
        NSLog(@"Frame offset set to: %d", frameOffset);
    }

    if (g_autoFreezeCheckbox) {
        int autoFreeze = ([g_autoFreezeCheckbox state] == NSControlStateValueOn) ? 1 : 0;
        FSTPSettings* settings = GetSettings();
        settings->auto_freeze_inactive = autoFreeze;
        NSLog(@"Auto-freeze inactive set to: %d", autoFreeze);
    }

    // Save MIDI settings
    if (g_midiEnabledCheckbox) {
        int midiEnabled = ([g_midiEnabledCheckbox state] == NSControlStateValueOn) ? 1 : 0;
        FSTPSettings* settings = GetSettings();
        settings->midi_enabled = midiEnabled;
        NSLog(@"MIDI enabled set to: %d", midiEnabled);
    }

    if (g_midiInputPopup) {
        NSMenuItem* selectedItem = [g_midiInputPopup selectedItem];
        int inputPort = -1;  // -1 means "(None)"

        if ([selectedItem tag] >= 0) {
            inputPort = (int)[selectedItem tag];  // Use tag instead of index
        }

        FSTPSettings* settings = GetSettings();
        settings->midi_input_port = inputPort;
        NSLog(@"MIDI input port set to: %d (%@)", inputPort, [selectedItem title]);
    }

    if (g_midiOutputPopup) {
        NSMenuItem* selectedItem = [g_midiOutputPopup selectedItem];
        int outputPort = -1;  // -1 means "(None)"

        if ([selectedItem tag] >= 0) {
            outputPort = (int)[selectedItem tag];  // Use tag instead of index
        }

        FSTPSettings* settings = GetSettings();
        settings->midi_output_port = outputPort;
        NSLog(@"MIDI output port set to: %d (%@)", outputPort, [selectedItem title]);
    }

    // Save settings to file
    SaveSettings();

    // Apply settings to audio system (only volume and device)
    ApplyAudioSettings();

    // Apply MIDI settings
    ApplyMIDISettings();

    // Show restart dialog if buffer size changed
    if (bufferSizeChanged) {
        NSAlert* restartAlert = [[NSAlert alloc] init];
        restartAlert.messageText = @"Buffer settings changed";
        restartAlert.informativeText = @"Changes to buffer size settings will be applied after restarting the application.";
        [restartAlert addButtonWithTitle:@"OK"];
        [restartAlert runModal];
    }
    
    [g_settingsWindow close];
}

- (IBAction)settingsCancelClicked:(id)sender {
    NSLog(@"Settings Cancel clicked");
    [g_settingsWindow close];
}

- (IBAction)settingsResetClicked:(id)sender {
    NSAlert* alert = [[NSAlert alloc] init];
    [alert setMessageText:@"Reset Settings"];
    [alert setInformativeText:@"Are you sure you want to reset all settings to default values?"];
    [alert addButtonWithTitle:@"Reset"];
    [alert addButtonWithTitle:@"Cancel"];

    if ([alert runModal] == NSAlertFirstButtonReturn) {
        NSLog(@"Resetting settings to default");
        ResetSettingsToDefault();

        // Update UI elements with new values - Audio
        if (g_audioDevicePopup) {
            [g_audioDevicePopup selectItemWithTag:0]; // Default device
        }

        if (g_bufferSizePopup) {
            [g_bufferSizePopup selectItemWithTag:1024]; // 1024 samples by default
        }

        if (g_volumeSlider) {
            g_volumeSlider.doubleValue = 1.0; // 100% volume by default
            if (g_volumeValueLabel) {
                g_volumeValueLabel.stringValue = @"100%";
            }
        }

        // Update UI elements - Video & Sync
        if (g_frameOffsetStepper) {
            [g_frameOffsetStepper setIntValue:0]; // 0 by default
        }
        if (g_frameOffsetTextField) {
            [g_frameOffsetTextField setIntValue:0];
        }
        if (g_autoFreezeCheckbox) {
            [g_autoFreezeCheckbox setState:NSControlStateValueOn]; // Enabled by default
        }

        // Update UI elements - MIDI
        if (g_midiEnabledCheckbox) {
            [g_midiEnabledCheckbox setState:NSControlStateValueOff]; // Disabled by default
        }
        if (g_midiInputPopup) {
            [g_midiInputPopup selectItemAtIndex:0]; // "(None)" by default
        }
        if (g_midiOutputPopup) {
            [g_midiOutputPopup selectItemAtIndex:0]; // "(None)" by default
        }
    }
}


- (IBAction)volumeSliderChanged:(id)sender {
    if (g_volumeSlider && g_volumeValueLabel) {
        float volume = (float)[g_volumeSlider doubleValue];
        g_volumeValueLabel.stringValue = [NSString stringWithFormat:@"%.0f%%", volume * 100];
    }
}

- (IBAction)frameOffsetStepperChanged:(id)sender {
    if (g_frameOffsetStepper && g_frameOffsetTextField) {
        int offset = (int)[g_frameOffsetStepper intValue];
        g_frameOffsetTextField.intValue = offset;
    }
}

- (IBAction)performMiniaturize:(id)sender {
    NSWindow* keyWindow = [NSApp keyWindow];
    if (keyWindow) {
        [keyWindow miniaturize:sender];
    }
}

- (IBAction)performZoom:(id)sender {
    NSWindow* keyWindow = [NSApp keyWindow];
    if (keyWindow) {
        [keyWindow zoom:sender];
    }
}

- (IBAction)copyScreenshotAction:(id)sender {
    CopyScreenshotToClipboard();
}

- (IBAction)showMemoryLocationsAction:(id)sender {
    ShowMemoryLocations();
}

- (IBAction)showInspectorAction:(id)sender {
    ToggleInspector();
}

@end

// Global app delegate
static AppDelegate* g_appDelegate = nil;

// Autonomous renderer
static SDL_Renderer* g_renderer = nullptr;
[[maybe_unused]] static dispatch_source_t g_renderTimer = nullptr;  // Reserved for future use
static bool g_renderingActive = false;
// macOS live resize flag: skip rendering during it to avoid artifacts
static std::atomic<bool> g_isLiveResizing{false};

void InitializeNativeApp() {
    @autoreleasepool {
        if (g_appDelegate == nil) {
            g_appDelegate = [[AppDelegate alloc] init];
            [NSApp setDelegate:g_appDelegate];
            [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        }

        // Track live-resize of all windows and temporarily pause rendering
        [[NSNotificationCenter defaultCenter] addObserverForName:NSWindowWillStartLiveResizeNotification
                                                          object:nil
                                                           queue:[NSOperationQueue mainQueue]
                                                      usingBlock:^(NSNotification* _Nonnull note) {
            g_isLiveResizing.store(true);
        }];

        [[NSNotificationCenter defaultCenter] addObserverForName:NSWindowDidEndLiveResizeNotification
                                                          object:nil
                                                           queue:[NSOperationQueue mainQueue]
                                                      usingBlock:^(NSNotification* _Nonnull note) {
            g_isLiveResizing.store(false);
        }];
    }
}

// Autonomous rendering function - works independently of events
void AutoRenderFrame() {
    if (g_renderingActive) {
        // PROFILING: Total frame time
        static uint64_t total_frame_us = 0;
        static uint64_t total_render_us = 0;
        static uint64_t total_osd_update_us = 0;
        static int profile_samples = 0;
        auto frame_start = std::chrono::high_resolution_clock::now();

        // During window live-resize skip frame to avoid diagonal lines/artifacts
        if (g_isLiveResizing.load()) {
            return;
        }
        // Simple crash protection in rendering
        static int error_counter = 0;
        const int MAX_ERRORS = 10;

        // PROFILING: Rendering time
        auto render_start = std::chrono::high_resolution_clock::now();

        // Render all active windows through WindowManager
        // Basic check before calling RenderAllWindows
        if (error_counter < MAX_ERRORS) {
            @try {
                RenderAllWindows();
            }
            @catch (NSException* exception) {
                error_counter++;
                NSLog(@"🚨 [RENDER ERROR] Exception in RenderAllWindows: %@ (count: %d)", exception, error_counter);
                if (error_counter >= MAX_ERRORS) {
                    g_renderingActive = false;
                    NSLog(@"🛑 [RENDER] Too many rendering errors, disabling");
                    return;
                }
            }
        } else {
            // If too many errors, temporarily disable rendering
            g_renderingActive = false;
            NSLog(@"🛑 [RENDER] Too many rendering errors, disabling");
            return;
        }

        auto render_end = std::chrono::high_resolution_clock::now();
        uint64_t render_us = std::chrono::duration_cast<std::chrono::microseconds>(render_end - render_start).count();
        total_render_us += render_us;

        // PROFILING: OSD update time
        auto osd_start = std::chrono::high_resolution_clock::now();

        // Update OSD data for all active windows with real player data
        static int osd_update_counter = 0;
        osd_update_counter++;

        for (int i = 0; i < MAX_WINDOWS; i++) {
            FSTPWindow* window = GetWindowByIndex(i);
            if (window && window->is_active) {
                int player_id = window->player_instance_id;

                if (player_id >= 0 && IsPlayerInstanceActive(player_id)) {
                    // Get real player data (only if instance is active)
                    bool is_playing = IsInstancePlaying(player_id);
                    double current_position = GetInstancePosition(player_id);
                    double duration = GetInstanceDuration(player_id);
                    double speed = GetInstanceSpeed(player_id);                  // Target speed
                    double actual_speed = GetInstanceActualSpeed(player_id);     // Actual speed
                    bool is_reverse = IsInstanceReverse(player_id);

                    // Get audio signal levels
                    float audio_left = GetInstanceAudioLevelLeft(player_id);
                    float audio_right = GetInstanceAudioLevelRight(player_id);
                    float peak_left = GetInstanceAudioPeakLeft(player_id);
                    float peak_right = GetInstanceAudioPeakRight(player_id);

                    // Update OSD for all windows every frame - remove optimization that caused delays
                    UpdateWindowOSD(i, current_position, duration, is_playing, speed, is_reverse);

                    // Update actual speed for correct mode indication
                    UpdateOSDActualSpeed(player_id, actual_speed);

                    // Update VU meters in OSD
                    UpdateOSDAudioLevels(player_id, audio_left, audio_right, peak_left, peak_right);
                }
                // Modes switch explicitly at key points:
                // - When loading file -> LOADING
                // - After loading -> NORMAL
                // - When unloading -> NO_FILE
            }
        }

        auto osd_end = std::chrono::high_resolution_clock::now();
        uint64_t osd_us = std::chrono::duration_cast<std::chrono::microseconds>(osd_end - osd_start).count();
        total_osd_update_us += osd_us;

        // PROFILING: Total frame time
        auto frame_end = std::chrono::high_resolution_clock::now();
        uint64_t frame_us = std::chrono::duration_cast<std::chrono::microseconds>(frame_end - frame_start).count();
        total_frame_us += frame_us;

        profile_samples++;

        // Profiling disabled for performance
        // if (profile_samples >= 60) {
        //     uint64_t avg_frame = total_frame_us / profile_samples;
        //     uint64_t avg_render = total_render_us / profile_samples;
        //     uint64_t avg_osd = total_osd_update_us / profile_samples;

        //     std::cout << "📊 [RENDER PROFILE] Frame: " << avg_frame << "μs"
        //               << " (Render: " << avg_render << "μs"
        //               << ", OSD: " << avg_osd << "μs)" << std::endl;

        //     // Reset counters
        //     total_frame_us = 0;
        //     total_render_us = 0;
        //     total_osd_update_us = 0;
        //     profile_samples = 0;
        // }
    }
}

// Variables for VSync via CVDisplayLink
static CVDisplayLinkRef g_displayLink = nullptr;
static std::atomic<bool> g_renderThreadRunning{false};
static std::thread g_renderThread;
static std::mutex g_vsyncMutex;
static std::condition_variable g_vsyncCV;
static std::atomic<bool> g_vsyncSignal{false};
static std::atomic<bool> g_forceRender{false};  // Force render on zoom/UI changes

// CVDisplayLink callback - called synchronously with display vertical retrace
static CVReturn DisplayLinkCallback(CVDisplayLinkRef displayLink,
                                   const CVTimeStamp* now,
                                   const CVTimeStamp* outputTime,
                                   CVOptionFlags flagsIn,
                                   CVOptionFlags* flagsOut,
                                   void* displayLinkContext) {
    (void)displayLink;
    (void)now;
    (void)outputTime;
    (void)flagsIn;
    (void)flagsOut;
    (void)displayLinkContext;

    // Signal render thread that it's time to render
    g_vsyncSignal.store(true);
    g_vsyncCV.notify_one();
    return kCVReturnSuccess;
}

// Start autonomous rendering for all windows
void StartAutonomousRendering(SDL_Renderer* renderer) {
    // renderer no longer used, rendering happens through WindowManager
    (void)renderer;  // Suppress warning about unused parameter
    g_renderer = nullptr;
    g_renderingActive = true;
    g_renderThreadRunning = true;

    // Create CVDisplayLink for native VSync
    // Suppress deprecation warnings - CVDisplayLink is still most efficient for VSync
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wdeprecated-declarations"
    CVDisplayLinkCreateWithActiveCGDisplays(&g_displayLink);
    CVDisplayLinkSetOutputCallback(g_displayLink, &DisplayLinkCallback, nullptr);

    // Get main display for synchronization
    CGDirectDisplayID displayID = CGMainDisplayID();
    CVDisplayLinkSetCurrentCGDisplay(g_displayLink, displayID);

    // Start CVDisplayLink
    CVDisplayLinkStart(g_displayLink);
    #pragma clang diagnostic pop

    NSLog(@"🎬 [VSYNC] CVDisplayLink started - native VSync enabled");

    // Render thread waits for signals from CVDisplayLink
    g_renderThread = std::thread([]() {
        // std::cout << "🎬 [RENDER THREAD] CVDisplayLink-based render thread started" << std::endl;

        // FPS counter for diagnostics
        int fps_counter = 0;
        auto fps_start = std::chrono::steady_clock::now();

        while (g_renderThreadRunning.load()) {
            // Wait for signal from CVDisplayLink (VSync)
            {
                std::unique_lock<std::mutex> lock(g_vsyncMutex);
                g_vsyncCV.wait(lock, []{ return g_vsyncSignal.load() || !g_renderThreadRunning.load(); });

                if (!g_renderThreadRunning.load()) break;
                g_vsyncSignal.store(false);
            }

            // Always render at full VSync (60 FPS) for stability
            // Adaptive FPS was disabled due to desync at 1x speed
            fps_counter++;

            // Render frame synchronized with VSync
            AutoRenderFrame();

            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - fps_start).count() >= 1) {
                // FPS counter for diagnostics (disabled in production)
                // std::cout << "🎬 [RENDER THREAD] FPS: " << fps_counter << " (CVDisplayLink)" << std::endl;
                fps_counter = 0;
                fps_start = now;
            }
        }

        // std::cout << "🎬 [RENDER THREAD] CVDisplayLink-based render thread stopped" << std::endl;
    });
}

// Stop autonomous rendering
void StopAutonomousRendering() {
    g_renderingActive = false;
    g_renderThreadRunning = false;

    // Stop CVDisplayLink
    if (g_displayLink) {
        #pragma clang diagnostic push
        #pragma clang diagnostic ignored "-Wdeprecated-declarations"
        CVDisplayLinkStop(g_displayLink);
        CVDisplayLinkRelease(g_displayLink);
        #pragma clang diagnostic pop
        g_displayLink = nullptr;
        NSLog(@"🎬 [VSYNC] CVDisplayLink stopped");
    }

    // Notify render thread about termination
    g_vsyncCV.notify_all();

    // Wait for render thread to finish
    if (g_renderThread.joinable()) {
        g_renderThread.join();
    }

    g_renderer = nullptr;
}

// Main UI loop for macOS - complete implementation
int RunMainUILoop() {
    // OPTIMIZED CONFIGURATION: Surface-based textures + VSync + full 60 FPS
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");     // Bilinear filtering
    SDL_SetHint(SDL_HINT_RENDER_VSYNC, "1");             // ENABLE VSync for smoothness
    SDL_SetHint("SDL_VIDEODRIVER", "cocoa");             // Use native Cocoa

    // PROTECTION: FramePacing for stable synchronization
    SDL_SetHint("SDL_MAC_DISABLE_FRAMESYNC", "0");       // Enable macOS FramePacing by default
    SDL_SetHint("SDL_METAL_PREFER_LOW_POWER_DEVICE", "0"); // Disable Metal power saving

    // VSYNC: Enable to eliminate tearing
    // CVDisplayLink synchronized with VBlank, render constantly at 60 FPS for stability
    SDL_SetHint(SDL_HINT_RENDER_VSYNC, "1");
    NSLog(@"🚀 [VSYNC] Enabled VSync to eliminate tearing");

    NSLog(@"🎬 [OPTIMIZED CONFIG] Surface textures + VSync + Full 60 FPS rendering");

    // Full SDL initialization
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        NSLog(@"SDL initialization error: %s", SDL_GetError());
        return -1;
    }

    // Initialize native app and menu
    InitializeNativeApp();

    // Initialize window manager
    if (InitWindowManager() != 0) {
        NSLog(@"Window manager initialization error");
        SDL_Quit();
        return -1;
    }

    // Create main window (automatically bound to player #0)
    int main_window_index = CreateNewWindow("TapeXPlayer 2026 - Player 0", 1280, 720);
    if (main_window_index < 0) {
        NSLog(@"Main window creation error");
        ShutdownWindowManager();
        SDL_Quit();
        return -1;
    }

    // Get main window for OSD system
    FSTPWindow* main_window = GetMainWindow();
    if (main_window == nullptr) {
        NSLog(@"Error getting main window");
        ShutdownWindowManager();
        SDL_Quit();
        return -1;
    }

    // Initialize settings system
    if (InitSettings() != 0) {
        NSLog(@"Settings system initialization error");
    }

    // Initialize OSD system with main window renderer
    if (InitOSDSystem(main_window->renderer) != 0) {
        NSLog(@"OSD system initialization error");
        ShutdownWindowManager();
        SDL_Quit();
        return -1;
    }

    // Start autonomous rendering of all windows
    StartAutonomousRendering(main_window->renderer);

    // Main macOS UI loop - only event processing
    bool running = true;
    SDL_Event event;

    // Event loop profiling (disabled in production)
    // static int event_loop_wakeups = 0;
    // static auto event_loop_last_report = std::chrono::steady_clock::now();

    while (running) {
        // event_loop_wakeups++;

        // auto now = std::chrono::steady_clock::now();
        // if (std::chrono::duration_cast<std::chrono::seconds>(now - event_loop_last_report).count() >= 1) {
        //     std::cout << "🔄 [EVENT LOOP] Wakeups per second: " << event_loop_wakeups << std::endl;
        //     event_loop_wakeups = 0;
        //     event_loop_last_report = now;
        // }

        // Process native macOS events (may block - but rendering continues!)
        HandleNativeAppEvents();

        // CPU OPTIMIZATION: Use SDL_WaitEventTimeout instead of PollEvent + usleep
        // Blocks until event or timeout → CPU savings!
        // Adaptive timeout: zoom panning requires fast response (1ms), normally 16ms (60 Hz)
        bool zoom_panning = IsZoomPanningActive();
        int timeout_ms = zoom_panning ? 1 : 16;

        // Wait for event OR timeout (blocks thread → saves CPU!)
        if (SDL_WaitEventTimeout(&event, timeout_ms)) {
            // Event available - process it
            do {
            // Process window events through window manager
            HandleWindowEvents(&event);

            // Process keyboard events for player control
            if (!HandleKeyboardEvents(event)) {
                running = false;
                break;
            }

            switch (event.type) {
                case SDL_QUIT:
                    // Ignore SDL_QUIT from window closures
                    // Application terminates only through "Quit" menu
                    NSLog(@"SDL_QUIT ignored - use menu to quit application");
                    break;

                case SDL_USEREVENT:
                    // Process user events
                    if (event.user.code == 1) {
                        // Real application termination through menu
                        NSLog(@"Application termination requested via menu");
                        running = false;
                    }
                    break;

                case SDL_KEYDOWN:
                    switch (event.key.keysym.sym) {
                        case SDLK_ESCAPE:
                        case SDLK_q:
                            if (event.key.keysym.mod & KMOD_GUI) {
                                running = false;
                            }
                            break;

                        // Remove Cmd+O duplication - handled through menu
                        // case SDLK_o: handled through native macOS menu

                        case SDLK_COMMA:
                            if (event.key.keysym.mod & KMOD_GUI) {
                                ShowNativeSettingsDialog(); // Unified settings for all players
                            }
                            break;

                        case SDLK_n:
                            if (event.key.keysym.mod & KMOD_GUI) {
                                // Cmd+N handled in FSTPKeyboard.cpp
                                // Don't duplicate handling here
                            }
                            break;
                    }
                    break;
            }
            // Process all accumulated events after WaitEventTimeout
            } while (SDL_PollEvent(&event));
        }
        // If SDL_WaitEventTimeout returned false (timeout without events) - just continue loop
        // WaitEventTimeout blocking saves CPU instead of active polling + usleep!
    }

    // Stop autonomous rendering
    StopAutonomousRendering();

    // Shutdown OSD system
    ShutdownOSDSystem();

    // Shutdown settings system
    ShutdownSettings();

    // Shutdown window manager (will automatically close all windows)
    ShutdownWindowManager();

    // Shutdown SDL
    SDL_Quit();

    return 0;
}

// Request force render (for UI changes like zoom)
extern "C" void RequestForceRender() {
    g_forceRender.store(true);
}

// Initial file to load from command line
static std::string g_initial_file_to_load;

// Set initial file to load from command line
void SetInitialFileToLoad(const char* filepath) {
    g_initial_file_to_load = filepath;
    std::cout << "📂 Initial file to load set: " << filepath << std::endl;
}

#endif // __APPLE__