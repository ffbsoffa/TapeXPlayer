#ifdef __APPLE__

#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#import <objc/runtime.h>
#include "FSTPToolsMenu.h"
#include "../FSTPPlayerModule/FSTPPlayerManager.h"
#include "FSTPWindowManager.h"
#include "FSTPPixelBufferManager.h"
#include "FSTPScreenshot.h"
#include "FSTPMemoryLocations.h"
#include "FSTPZoom.h"
#include "darwin/sdl/FSTPDarwinWS.h"
#include "../FSTPVideoModule/FSTPVideoFrame.h"
#include <vector>
#include <algorithm>

extern "C" {
#include <libavutil/frame.h>
}

// Forward declarations for SwiftUI Inspector
extern "C" {
    void ShowSwiftUIInspector();
    void HideSwiftUIInspector();
    void ToggleSwiftUIInspector();
}

// Inspector state
static bool g_inspector_active = false;

// Global variables for Memory Locations window
static NSWindow* g_memory_locations_window = nil;
static NSTableView* g_memory_locations_table = nil;
static bool g_memory_locations_active = false;
static bool g_memory_location_dialog_open = false;

// Constants for Memory Locations
static const CGFloat TIME_MARKERS_HEIGHT = 25.0;

// Custom NSView for Alt+Click handling
@interface MemoryLocationsContentView : NSView
@end

// Custom NSTableView for Alt+Click handling
@interface MemoryLocationsTableView : NSTableView
@end

// Delegate for Memory Locations Table View
@interface MemoryLocationsTableDelegate : NSObject <NSTableViewDataSource, NSTableViewDelegate>
@property (atomic) NSInteger cachedRowCount;
@property (atomic) BOOL needsRefresh;
- (void)refreshCache;
@end

// Helper for creating Memory Location
@interface MemoryLocationDialogHelper : NSObject
@property (strong) NSWindow* dialog;
@property (strong) NSTextField* numberField;
@property (strong) NSTextField* timecodeField;
@property (strong) NSTextField* nameField;
@property (strong) NSTextView* commentsField;
@property (strong) NSButton* zoomCheckbox;
@property int playerId;
@property double currentTime;
- (void)performAction:(id)sender;
@end

// Delegate for Memory Location dialog window
@interface MemoryLocationDialogDelegate : NSObject <NSWindowDelegate>
@end

// Delegate for Memory Locations window (main window with list)
@interface MemoryLocationsWindowDelegate : NSObject <NSWindowDelegate>
@end

// Delegate for dialog text fields (Enter key handling)
@interface MemoryLocationTextFieldDelegate : NSObject <NSTextFieldDelegate>
@property (strong) MemoryLocationDialogHelper* helper;
@end

// Global variable for table delegate (after interface declaration)
static MemoryLocationsTableDelegate* g_memory_locations_delegate = nil;

// === Memory Locations Content View Implementation ===
@implementation MemoryLocationsContentView
// Empty implementation - Alt+click handling now in custom table
@end

// === Custom NSTableView for Alt+Click ===
@implementation MemoryLocationsTableView

- (void)mouseDown:(NSEvent *)event {
    // Check Alt/Option
    if (event.modifierFlags & NSEventModifierFlagOption) {
        NSPoint point = [self convertPoint:[event locationInWindow] fromView:nil];
        NSInteger row = [self rowAtPoint:point];

        if (row >= 0) {
            // Alt+click on row - delete marker
            FSTP_MemoryLocationData data;
            if (FSTP_GetMemoryLocationData((int)row, &data)) {
                NSLog(@"🗑️ Alt+Click - Deleting Memory Location #%d: %s", data.id, data.name);

                if (FSTP_DeleteMemoryLocation(data.id)) {
                    dispatch_async(dispatch_get_main_queue(), ^{
                        if (g_memory_locations_delegate) {
                            g_memory_locations_delegate.needsRefresh = YES;
                        }
                        [self reloadData];
                    });
                    NSLog(@"✅ Memory Location #%d deleted", data.id);
                } else {
                    NSLog(@"❌ Failed to delete Memory Location #%d", data.id);
                }
            }
            return; // Don't pass event further
        }
    }

    // Normal click - standard handling
    [super mouseDown:event];
}

@end

// === Memory Locations Table Delegate Implementation ===
@implementation MemoryLocationsTableDelegate

- (instancetype)init {
    self = [super init];
    if (self) {
        _cachedRowCount = 0;
        _needsRefresh = YES;
    }
    return self;
}

- (void)refreshCache {
    self.cachedRowCount = FSTP_GetMemoryLocationsCount();
    self.needsRefresh = NO;
}

- (NSInteger)numberOfRowsInTableView:(NSTableView *)tableView {
    if (self.needsRefresh) {
        [self refreshCache];
    }
    return self.cachedRowCount;
}

- (id)tableView:(NSTableView *)tableView objectValueForTableColumn:(NSTableColumn *)tableColumn row:(NSInteger)row {
    FSTP_MemoryLocationData data;
    if (!FSTP_GetMemoryLocationData((int)row, &data)) {
        return @"";
    }

    NSString* identifier = [tableColumn identifier];

    if ([identifier isEqualToString:@"ID"]) {
        return [NSString stringWithFormat:@"%d", data.id];
    } else if ([identifier isEqualToString:@"Name"]) {
        return [NSString stringWithUTF8String:data.name];
    } else if ([identifier isEqualToString:@"Timecode"]) {
        return [NSString stringWithUTF8String:data.timecode_display];
    } else if ([identifier isEqualToString:@"Comments"]) {
        return [NSString stringWithUTF8String:data.comments];
    } else if ([identifier isEqualToString:@"Active"]) {
        return @(data.is_active);
    }

    return @"";
}

- (void)tableView:(NSTableView *)tableView setObjectValue:(id)object forTableColumn:(NSTableColumn *)tableColumn row:(NSInteger)row {
    NSString* identifier = [tableColumn identifier];

    FSTP_MemoryLocationData data;
    if (!FSTP_GetMemoryLocationData((int)row, &data)) {
        return;
    }

    if ([identifier isEqualToString:@"Name"]) {
        // Update location name
        NSString* newName = (NSString*)object;
        FSTP_UpdateMemoryLocation(data.id, [newName UTF8String], data.comments);
        NSLog(@"✅ Updated Memory Location #%d name to: %@", data.id, newName);
    }
}

- (void)tableViewSelectionDidChange:(NSNotification *)notification {
    NSTableView* tableView = [notification object];
    NSInteger selectedRow = [tableView selectedRow];

    if (selectedRow >= 0) {
        FSTP_MemoryLocationData data;
        if (FSTP_GetMemoryLocationData((int)selectedRow, &data)) {
            NSLog(@"📍 Selected Memory Location #%d: %s at %s", data.id, data.name, data.timecode_display);

            // Update OSD on active player
            int active_player = GetActivePlayerID();
            if (active_player >= 0) {
                // Jump to selected location, this will update OSD
                FSTP_RecallMemoryLocation(data.id, active_player);
            }
        }
    }
}

@end

@implementation MemoryLocationDialogHelper
- (void)performAction:(id)sender {
    NSString* name = [self.nameField stringValue];
    NSString* numberStr = [self.numberField stringValue];
    NSString* timecodeStr = [self.timecodeField stringValue];

    if (name.length > 0) {
        // Get ID from Number field
        int marker_id = [numberStr intValue];
        if (marker_id <= 0) {
            marker_id = FSTP_GetMemoryLocationsCount() + 1; // If not specified - auto-ID
        }

        // Parse timecode if user changed it
        double timecode_seconds = self.currentTime;
        if (timecodeStr.length > 0) {
            std::string timecode_cpp = [timecodeStr UTF8String];
            timecode_seconds = FSTP::MemoryLocationsManager::TimecodeToSeconds(timecode_cpp);
        }

        FSTP_AddMemoryLocationWithTimecode(self.playerId, marker_id, [name UTF8String], "", timecode_seconds);
        if (g_memory_locations_table && g_memory_locations_delegate) {
            dispatch_async(dispatch_get_main_queue(), ^{
                g_memory_locations_delegate.needsRefresh = YES;
                [g_memory_locations_table reloadData];
            });
        }
        NSLog(@"✅ Memory Location #%d added: %@ at %@", marker_id, name, timecodeStr);
    }

    [self.dialog close];
    // Flag g_memory_location_dialog_open will be reset in windowWillClose with delay
}
@end

@implementation MemoryLocationDialogDelegate
- (void)windowWillClose:(NSNotification *)notification {
    // Return focus to main SDL window
    RestoreFocusToMainWindow();

    // Reset flag with delay when closing by any method (Cancel, close button, OK)
    // Delay is needed so Enter/Return event doesn't get processed as global hotkey
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.2 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        g_memory_location_dialog_open = false;
        NSLog(@"📍 Memory Location dialog closed, ready for next hotkey");
    });
}
@end

@implementation MemoryLocationsWindowDelegate
- (void)windowWillClose:(NSNotification *)notification {
    NSLog(@"📍 Memory Locations window closing...");

    // Return focus to main SDL window
    RestoreFocusToMainWindow();

    // Clear table delegates before closing
    if (g_memory_locations_table != nil) {
        [g_memory_locations_table setDelegate:nil];
        [g_memory_locations_table setDataSource:nil];
    }

    // Reset global variables
    g_memory_locations_window = nil;
    g_memory_locations_table = nil;
    g_memory_locations_delegate = nil;
    g_memory_locations_active = false;
}
@end

@implementation MemoryLocationTextFieldDelegate
- (BOOL)control:(NSControl *)control textView:(NSTextView *)textView doCommandBySelector:(SEL)commandSelector {
    // Handle Enter inside text field
    if (commandSelector == @selector(insertNewline:)) {
        [self.helper performAction:nil];
        return YES; // Event handled
    }
    return NO; // Standard handling
}
@end

void InitToolsMenu() {
    NSLog(@"🔧 Initializing Tools Menu...");

    // Add "Tools" item to main menu
    NSMenu* mainMenu = [NSApp mainMenu];
    if (!mainMenu) {
        NSLog(@"⚠️ Main menu not found, cannot add Tools menu");
        return;
    }

    // Find existing Tools item or create new one
    NSMenuItem* toolsMenuItem = nil;
    for (NSMenuItem* item in [mainMenu itemArray]) {
        if ([[item title] isEqualToString:@"Tools"]) {
            toolsMenuItem = item;
            break;
        }
    }

    if (!toolsMenuItem) {
        toolsMenuItem = [[NSMenuItem alloc] initWithTitle:@"Tools" action:nil keyEquivalent:@""];
        NSMenu* toolsMenu = [[NSMenu alloc] initWithTitle:@"Tools"];
        [toolsMenuItem setSubmenu:toolsMenu];
        [mainMenu addItem:toolsMenuItem];
    }

    NSMenu* toolsMenu = [toolsMenuItem submenu];

    // Clear existing items
    [toolsMenu removeAllItems];

    // Add "Copy Screenshot" item
    NSMenuItem* screenshotItem = [[NSMenuItem alloc]
        initWithTitle:@"📸 Copy Screenshot"
        action:@selector(copyScreenshotAction:)
        keyEquivalent:@"c"];
    [screenshotItem setKeyEquivalentModifierMask:NSEventModifierFlagCommand];
    [screenshotItem setTarget:[NSApp delegate]];
    [toolsMenu addItem:screenshotItem];

    // Add "Memory Locations" item
    NSMenuItem* memoryLocationsItem = [[NSMenuItem alloc]
        initWithTitle:@"📍 Memory Locations..."
        action:@selector(showMemoryLocationsAction:)
        keyEquivalent:@"m"];
    [memoryLocationsItem setKeyEquivalentModifierMask:NSEventModifierFlagCommand | NSEventModifierFlagOption];
    [memoryLocationsItem setTarget:[NSApp delegate]];
    [toolsMenu addItem:memoryLocationsItem];

    // Add separator
    [toolsMenu addItem:[NSMenuItem separatorItem]];

    // Add "Show Inspector" item
    NSMenuItem* showInspectorItem = [[NSMenuItem alloc]
        initWithTitle:@"Show Inspector"
        action:@selector(showInspectorAction:)
        keyEquivalent:@"i"];
    [showInspectorItem setKeyEquivalentModifierMask:NSEventModifierFlagCommand];
    [showInspectorItem setTarget:[NSApp delegate]];
    [toolsMenu addItem:showInspectorItem];

    NSLog(@"✅ Tools Menu initialized successfully");
}

// Method for handling show inspector action
// Will be called through NSApp delegate
void ShowInspectorAction() {
    ShowInspector();
}

// Inspector implementation using SwiftUI
void ShowInspector() {
    NSLog(@"📋 Showing Inspector...");
    ShowSwiftUIInspector();
    g_inspector_active = true;
}

void HideInspector() {
    NSLog(@"📋 Hiding Inspector...");
    HideSwiftUIInspector();
    g_inspector_active = false;
}

void ToggleInspector() {
    NSLog(@"📋 Toggling Inspector...");
    ToggleSwiftUIInspector();
    g_inspector_active = !g_inspector_active;
}

bool IsInspectorActive() {
    return g_inspector_active;
}

// Implementation of copy screenshot to clipboard function
void CopyScreenshotToClipboard() {
    NSLog(@"📸 Copying screenshot to clipboard...");

    // Get active player
    int active_player = GetActivePlayerID();
    if (active_player < 0) {
        NSLog(@"⚠️ No active player for screenshot");

        // Show error notification
        NSUserNotification* notification = [[NSUserNotification alloc] init];
        notification.title = @"Screenshot Failed";
        notification.informativeText = @"No active player";
        notification.soundName = NSUserNotificationDefaultSoundName;
        [[NSUserNotificationCenter defaultUserNotificationCenter] deliverNotification:notification];
        return;
    }

    // Get current pixel buffer
    FSTPPixelBufferManager* manager = GetPixelBufferManager();
    if (!manager) {
        NSLog(@"⚠️ Pixel buffer manager not available");

        NSUserNotification* notification = [[NSUserNotification alloc] init];
        notification.title = @"Screenshot Failed";
        notification.informativeText = @"Pixel buffer manager not available";
        notification.soundName = NSUserNotificationDefaultSoundName;
        [[NSUserNotificationCenter defaultUserNotificationCenter] deliverNotification:notification];
        return;
    }

    const FSTPPixelBufferManager::PixelBuffer* pixel_buffer =
        manager->GetPixelBuffer(active_player);

    if (!pixel_buffer || !pixel_buffer->is_valid || !pixel_buffer->av_frame) {
        NSLog(@"⚠️ No valid frame for screenshot");

        // Show error notification
        NSUserNotification* notification = [[NSUserNotification alloc] init];
        notification.title = @"Screenshot Failed";
        notification.informativeText = @"No valid frame available";
        notification.soundName = NSUserNotificationDefaultSoundName;
        [[NSUserNotificationCenter defaultUserNotificationCenter] deliverNotification:notification];
        return;
    }

    int width = pixel_buffer->width;
    int height = pixel_buffer->height;
    double current_time = GetInstancePosition(active_player);

    // Get actual video FPS
    double video_fps = GetInstanceVideoFPS(active_player);
    if (video_fps <= 0) video_fps = 25.0; // fallback

    // Format timecode (HH:MM:SS:FF)
    int hours = (int)(current_time / 3600);
    int minutes = (int)((current_time - hours * 3600) / 60);
    int seconds = (int)current_time % 60;
    int frames = (int)((current_time - (int)current_time) * video_fps); // Use actual FPS

    char timecode_buf[32];
    snprintf(timecode_buf, sizeof(timecode_buf), "%02d:%02d:%02d:%02d", hours, minutes, seconds, frames);
    std::string timecode(timecode_buf);

    // Determine pixel format
    enum AVPixelFormat pix_fmt = (enum AVPixelFormat)pixel_buffer->av_frame->format;
    bool is_nv12 = (pix_fmt == AV_PIX_FMT_NV12);

    // CRITICAL CHECK: Validate av_frame data before copying
    if (!pixel_buffer->av_frame->data[0] || !pixel_buffer->av_frame->data[1]) {
        NSLog(@"❌ Screenshot failed: invalid av_frame data pointers (Y=%p UV=%p)",
              pixel_buffer->av_frame->data[0],
              pixel_buffer->av_frame->data[1]);

        NSUserNotification* notification = [[NSUserNotification alloc] init];
        notification.title = @"Screenshot Failed";
        notification.informativeText = @"Invalid frame data";
        notification.soundName = NSUserNotificationDefaultSoundName;
        [[NSUserNotificationCenter defaultUserNotificationCenter] deliverNotification:notification];
        return;
    }

    // For YUV420P check V plane
    if (!is_nv12 && !pixel_buffer->av_frame->data[2]) {
        NSLog(@"❌ Screenshot failed: YUV420P format but V plane is null");

        NSUserNotification* notification = [[NSUserNotification alloc] init];
        notification.title = @"Screenshot Failed";
        notification.informativeText = @"Invalid YUV420P data";
        notification.soundName = NSUserNotificationDefaultSoundName;
        [[NSUserNotificationCenter defaultUserNotificationCenter] deliverNotification:notification];
        return;
    }

    NSLog(@"📸 Screenshot format: %s", is_nv12 ? "NV12 (semi-planar)" : "YUV420P (planar)");

    // Get window index for active player
    int window_idx = -1;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        FSTPWindow* window = GetWindowByIndex(i);
        if (window && window->is_active && window->player_instance_id == active_player) {
            window_idx = i;
            break;
        }
    }

    // Get zoom state
    FSTPZoomState* zoom = nullptr;
    int window_width = width;
    int window_height = height;

    if (window_idx >= 0) {
        zoom = GetZoomState(window_idx);

        FSTPWindow* window = GetWindowByIndex(window_idx);
        if (window && window->window) {
            SDL_GetWindowSize(window->window, &window_width, &window_height);
        }
    }

    // ZERO-COPY: Create temporary contiguous buffer for screenshot
    // (screenshots are taken rarely, copying is acceptable)
    int y_size = width * height;
    int uv_size = (width / 2) * (height / 2);
    std::vector<uint8_t> temp_yuv_buffer(y_size + uv_size * 2);

    uint8_t* dst_y = temp_yuv_buffer.data();
    uint8_t* dst_u = dst_y + y_size;
    uint8_t* dst_v = dst_u + uv_size;

    // Copy Y plane
    for (int row = 0; row < height; row++) {
        memcpy(dst_y + row * width,
               pixel_buffer->av_frame->data[0] + row * pixel_buffer->av_frame->linesize[0],
               width);
    }

    // Copy U and V planes (NV12 vs YUV420P handling)
    int uv_height = height / 2;
    int uv_width = width / 2;

    if (is_nv12) {
        // NV12: UV interleaved (UVUVUVUV...) in data[1]
        // Split into separate U and V planes
        for (int row = 0; row < uv_height; row++) {
            const uint8_t* src_uv = pixel_buffer->av_frame->data[1] +
                                    row * pixel_buffer->av_frame->linesize[1];
            uint8_t* row_dst_u = dst_u + row * uv_width;
            uint8_t* row_dst_v = dst_v + row * uv_width;

            for (int col = 0; col < uv_width; col++) {
                row_dst_u[col] = src_uv[col * 2];     // U (even bytes)
                row_dst_v[col] = src_uv[col * 2 + 1]; // V (odd bytes)
            }
        }
    } else {
        // YUV420P: U and V separate planes
        for (int row = 0; row < uv_height; row++) {
            memcpy(dst_u + row * uv_width,
                   pixel_buffer->av_frame->data[1] + row * pixel_buffer->av_frame->linesize[1],
                   uv_width);
            memcpy(dst_v + row * uv_width,
                   pixel_buffer->av_frame->data[2] + row * pixel_buffer->av_frame->linesize[2],
                   uv_width);
        }
    }

    // Use FSTPScreenshot for correct YUV→RGB conversion and copying to clipboard
    // For screenshots ALWAYS show thumbnail if zoom is active (for clarity)
    bool show_thumb = zoom && zoom->enabled && zoom->factor > 1.0f;

    bool success = TakeScreenshotFromPixelBuffer(
        temp_yuv_buffer.data(),  // YUV420P data
        width,
        height,
        timecode,
        window_width,
        window_height,
        zoom ? zoom->enabled : false,
        zoom ? zoom->factor : 1.0f,
        zoom ? zoom->center_x : 0.5f,
        zoom ? zoom->center_y : 0.5f,
        show_thumb  // Always show thumbnail on screenshot when zoom is active
    );

    if (success) {
        NSLog(@"✅ Screenshot copied to clipboard (%dx%d)", width, height);

        // Show success notification
        NSUserNotification* notification = [[NSUserNotification alloc] init];
        notification.title = @"Screenshot Copied";
        notification.informativeText = [NSString stringWithFormat:@"Frame %s copied to clipboard (%dx%d)",
                                       timecode_buf, width, height];
        notification.soundName = NSUserNotificationDefaultSoundName;
        [[NSUserNotificationCenter defaultUserNotificationCenter] deliverNotification:notification];
    } else {
        NSLog(@"❌ Failed to copy screenshot to clipboard");

        // Show error notification
        NSUserNotification* notification = [[NSUserNotification alloc] init];
        notification.title = @"Screenshot Failed";
        notification.informativeText = @"Failed to convert and copy frame";
        notification.soundName = NSUserNotificationDefaultSoundName;
        [[NSUserNotificationCenter defaultUserNotificationCenter] deliverNotification:notification];
    }
}

// Create Memory Locations window
static void CreateMemoryLocationsWindow() {
    if (g_memory_locations_window != nil) return;

    NSLog(@"📍 Creating Memory Locations window...");

    // Create window
    NSRect windowRect = NSMakeRect(0, 0, 600, 400);
    g_memory_locations_window = [[NSWindow alloc]
        initWithContentRect:windowRect
        styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable)
        backing:NSBackingStoreBuffered
        defer:NO];

    [g_memory_locations_window setTitle:@"📍 Memory Locations"];
    [g_memory_locations_window setMinSize:NSMakeSize(500, 300)];
    [g_memory_locations_window setCollectionBehavior:NSWindowCollectionBehaviorFullScreenNone];  // Disable fullscreen mode
    [g_memory_locations_window center];

    // Set delegate for window close handling
    MemoryLocationsWindowDelegate* windowDelegate = [[MemoryLocationsWindowDelegate alloc] init];
    [g_memory_locations_window setDelegate:windowDelegate];

    // Create custom contentView for Alt+Click handling
    MemoryLocationsContentView* contentView = [[MemoryLocationsContentView alloc] initWithFrame:NSMakeRect(0, 0, 600, 400)];
    [contentView setWantsLayer:YES];
    [contentView setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [g_memory_locations_window setContentView:contentView];

    // Create ScrollView for table (with small top padding)
    NSScrollView* scrollView = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 5, 600, 395)];
    [scrollView setHasVerticalScroller:YES];
    [scrollView setHasHorizontalScroller:NO];
    [scrollView setAutohidesScrollers:NO];
    [scrollView setBorderType:NSBezelBorder];
    [scrollView setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [contentView addSubview:scrollView];

    // Create table (custom class for Alt+Click handling)
    g_memory_locations_table = [[MemoryLocationsTableView alloc] initWithFrame:NSMakeRect(0, 0, 600, 400)];
    [g_memory_locations_table setUsesAlternatingRowBackgroundColors:YES];
    [g_memory_locations_table setAllowsColumnReordering:NO];
    [g_memory_locations_table setAllowsMultipleSelection:NO];
    [g_memory_locations_table setDoubleAction:@selector(memoryLocationDoubleClick:)];
    [g_memory_locations_table setTarget:[NSApp delegate]];
    [g_memory_locations_table setRowHeight:20.0];
    [g_memory_locations_table setHeaderView:[[NSTableHeaderView alloc] init]];

    // Column #
    NSTableColumn* idColumn = [[NSTableColumn alloc] initWithIdentifier:@"ID"];
    [[idColumn headerCell] setStringValue:@"#"];
    [idColumn setWidth:40];
    [idColumn setMinWidth:40];
    [idColumn setMaxWidth:60];
    [g_memory_locations_table addTableColumn:idColumn];

    // Timecode column
    NSTableColumn* timecodeColumn = [[NSTableColumn alloc] initWithIdentifier:@"Timecode"];
    [[timecodeColumn headerCell] setStringValue:@"Timecode"];
    [timecodeColumn setWidth:120];
    [timecodeColumn setMinWidth:100];
    [timecodeColumn setMaxWidth:150];
    [g_memory_locations_table addTableColumn:timecodeColumn];

    // Name column (expands when window is resized)
    NSTableColumn* nameColumn = [[NSTableColumn alloc] initWithIdentifier:@"Name"];
    [[nameColumn headerCell] setStringValue:@"Name"];
    [nameColumn setWidth:420];
    [nameColumn setMinWidth:200];
    [nameColumn setEditable:YES]; // Allows editing names
    [nameColumn setResizingMask:NSTableColumnAutoresizingMask];
    [g_memory_locations_table addTableColumn:nameColumn];

    // Set column auto-resize mode
    [g_memory_locations_table setColumnAutoresizingStyle:NSTableViewLastColumnOnlyAutoresizingStyle];

    // Set delegate
    g_memory_locations_delegate = [[MemoryLocationsTableDelegate alloc] init];
    [g_memory_locations_table setDataSource:g_memory_locations_delegate];
    [g_memory_locations_table setDelegate:g_memory_locations_delegate];

    [scrollView setDocumentView:g_memory_locations_table];

    g_memory_locations_active = true;
}

// Implementation of show Memory Locations function
void ShowMemoryLocations() {
    NSLog(@"📍 Showing Memory Locations window...");

    // Initialize Memory Locations system
    FSTP_InitMemoryLocations();

    // Create window if it doesn't exist yet
    CreateMemoryLocationsWindow();

    // Show window
    [g_memory_locations_window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];

    // Update table asynchronously
    if (g_memory_locations_delegate) {
        dispatch_async(dispatch_get_main_queue(), ^{
            g_memory_locations_delegate.needsRefresh = YES;
            [g_memory_locations_table reloadData];
        });
    }
}

// Category for adding methods to App Delegate
@interface NSObject (ToolsMenuAdditions)
- (void)showDecoderVisualizationAction:(id)sender;
- (void)copyScreenshotAction:(id)sender;
- (void)showMemoryLocationsAction:(id)sender;
- (void)memoryLocationAdd:(id)sender;
- (void)memoryLocationDelete:(id)sender;
- (void)memoryLocationRecall:(id)sender;
- (void)memoryLocationDoubleClick:(id)sender;
- (void)memoryLocationImport:(id)sender;
- (void)memoryLocationExport:(id)sender;
@end

// Forward declaration
static void ShowMemoryLocationDialog(int active_player);

@implementation NSObject (ToolsMenuAdditions)
- (void)showInspectorAction:(id)sender {
    ToggleInspector();
}

- (void)copyScreenshotAction:(id)sender {
    CopyScreenshotToClipboard();
}

- (void)showMemoryLocationsAction:(id)sender {
    ShowMemoryLocations();
}

- (void)memoryLocationAdd:(id)sender {
    NSLog(@"📍 Adding new Memory Location...");

    int active_player = GetActivePlayerID();
    if (active_player < 0) {
        NSLog(@"⚠️ No active player for adding memory location");
        return;
    }

    ShowMemoryLocationDialog(active_player);
}

- (void)memoryLocationDelete:(id)sender {
    NSInteger selectedRow = [g_memory_locations_table selectedRow];
    if (selectedRow < 0) {
        NSLog(@"⚠️ No memory location selected");
        return;
    }

    FSTP_MemoryLocationData data;
    if (FSTP_GetMemoryLocationData((int)selectedRow, &data)) {
        NSAlert* alert = [[NSAlert alloc] init];
        [alert setMessageText:@"Delete Memory Location"];
        [alert setInformativeText:[NSString stringWithFormat:@"Delete '%s'?", data.name]];
        [alert addButtonWithTitle:@"Delete"];
        [alert addButtonWithTitle:@"Cancel"];

        if ([alert runModal] == NSAlertFirstButtonReturn) {
            FSTP_DeleteMemoryLocation(data.id);
            if (g_memory_locations_delegate) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    g_memory_locations_delegate.needsRefresh = YES;
                    [g_memory_locations_table reloadData];
                });
            }
            NSLog(@"✅ Memory Location deleted: %d", data.id);
        }
    }
}

- (void)memoryLocationRecall:(id)sender {
    NSInteger selectedRow = [g_memory_locations_table selectedRow];
    if (selectedRow < 0) {
        NSLog(@"⚠️ No memory location selected");
        return;
    }

    int active_player = GetActivePlayerID();
    if (active_player < 0) {
        NSLog(@"⚠️ No active player for recalling memory location");
        return;
    }

    FSTP_MemoryLocationData data;
    if (FSTP_GetMemoryLocationData((int)selectedRow, &data)) {
        FSTP_RecallMemoryLocation(data.id, active_player);
        NSLog(@"✅ Recalled Memory Location: %s at %s", data.name, data.timecode_display);
    }
}

- (void)memoryLocationDoubleClick:(id)sender {
    NSInteger clickedRow = [g_memory_locations_table clickedRow];
    if (clickedRow < 0) return;

    int active_player = GetActivePlayerID();
    if (active_player < 0) {
        NSLog(@"⚠️ No active player for recalling memory location");
        return;
    }

    FSTP_MemoryLocationData data;
    if (FSTP_GetMemoryLocationData((int)clickedRow, &data)) {
        FSTP_RecallMemoryLocation(data.id, active_player);
        NSLog(@"✅ Recalled Memory Location (double-click): %s", data.name);
    }
}

- (void)memoryLocationImport:(id)sender {
    NSLog(@"📍 Importing Memory Locations...");

    NSOpenPanel* panel = [NSOpenPanel openPanel];
    [panel setCanChooseFiles:YES];
    [panel setCanChooseDirectories:NO];
    [panel setAllowsMultipleSelection:NO];
    [panel setMessage:@"Select Memory Locations file to import"];

    if ([panel runModal] == NSModalResponseOK) {
        NSURL* url = [[panel URLs] objectAtIndex:0];
        NSString* filepath = [url path];

        if (FSTP_LoadMemoryLocations([filepath UTF8String])) {
            if (g_memory_locations_delegate) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    g_memory_locations_delegate.needsRefresh = YES;
                    [g_memory_locations_table reloadData];
                });
            }
            NSLog(@"✅ Memory Locations imported from: %@", filepath);
        } else {
            NSAlert* alert = [[NSAlert alloc] init];
            [alert setMessageText:@"Import Failed"];
            [alert setInformativeText:@"Failed to import Memory Locations file"];
            [alert addButtonWithTitle:@"OK"];
            [alert runModal];
        }
    }

    // Return focus to main window after closing dialog
    RestoreFocusToMainWindow();
}

- (void)memoryLocationExport:(id)sender {
    NSLog(@"📍 Exporting Memory Locations...");

    NSSavePanel* panel = [NSSavePanel savePanel];
    [panel setNameFieldStringValue:@"memory_locations.txt"];
    [panel setMessage:@"Export Memory Locations"];

    if ([panel runModal] == NSModalResponseOK) {
        NSURL* url = [panel URL];
        NSString* filepath = [url path];

        if (FSTP_SaveMemoryLocations([filepath UTF8String])) {
            NSLog(@"✅ Memory Locations exported to: %@", filepath);
        } else {
            NSAlert* alert = [[NSAlert alloc] init];
            [alert setMessageText:@"Export Failed"];
            [alert setInformativeText:@"Failed to export Memory Locations file"];
            [alert addButtonWithTitle:@"OK"];
            [alert runModal];
        }
    }

    // Return focus to main window after closing dialog
    RestoreFocusToMainWindow();
}
@end

// Callback from Swift when dialog closes
extern "C" void OnMemoryLocationDialogClosedCallback() {
    // Small delay so Enter event doesn't get processed again
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.2 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        g_memory_location_dialog_open = false;
    });
}

// Modern SwiftUI Memory Location dialog
static void ShowMemoryLocationDialog(int active_player) {
    // Check that file is loaded
    double duration = GetInstanceDuration(active_player);
    if (duration <= 0.0) {
        NSLog(@"⚠️ No video file loaded - cannot create memory location");
        return;
    }

    // Get current player time
    double current_time = GetInstancePosition(active_player);

    // Use SwiftUI dialog (macOS 13.0+)
    if (@available(macOS 13.0, *)) {
        ShowSwiftUIMemoryLocationDialog(active_player, current_time);
        g_memory_location_dialog_open = true;
    } else {
        NSLog(@"⚠️ Memory Location dialog requires macOS 13.0 Ventura or later");
    }
}

// C API for calling from keyboard hotkeys
void CreateMemoryLocationAtCurrentTime() {
    // If dialog is already open - don't open a second one
    if (g_memory_location_dialog_open) {
        return;
    }

    int active_player = GetActivePlayerID();
    if (active_player < 0) {
        NSLog(@"⚠️ No active player for adding memory location");
        return;
    }

    ShowMemoryLocationDialog(active_player);
}

// Check if text field is active
bool IsTextFieldActive() {
    NSWindow* keyWindow = [NSApp keyWindow];
    if (keyWindow) {
        NSResponder* firstResponder = [keyWindow firstResponder];
        if ([firstResponder isKindOfClass:[NSTextView class]] ||
            [firstResponder isKindOfClass:[NSTextField class]]) {
            return true;
        }
    }
    return false;
}

bool IsMemoryLocationsWindowActive() {
    return g_memory_locations_active;
}

bool IsMemoryLocationDialogActive() {
    return g_memory_location_dialog_open;
}

#endif // __APPLE__