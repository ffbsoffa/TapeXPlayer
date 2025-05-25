#import <Cocoa/Cocoa.h>
#import <AppKit/AppKit.h>
#import <AppKit/NSApplication.h>
#import "menu_system_impl.h"
#import "menu_system.h"
#import "nfd.hpp"
#include "../remote/remote_control.h"
#include "../audio/mainau.h" // Include for audio device functions
#include <SDL2/SDL_syswm.h>

extern RemoteControl* g_remote_control;

// Forward declarations for audio device functions
extern std::vector<std::string> get_audio_output_devices();
extern int get_current_audio_device_index();
extern bool switch_audio_device(int deviceIndex);

#ifdef __APPLE__

@implementation MenuDelegate

- (void)applicationWillFinishLaunching:(NSNotification *)notification {
    // Register for URL events
    [[NSAppleEventManager sharedAppleEventManager]
        setEventHandler:self
            andSelector:@selector(handleGetURLEvent:withReplyEvent:)
          forEventClass:kInternetEventClass
             andEventID:kAEGetURL];
    NSLog(@"[MenuDelegate applicationWillFinishLaunching:] Registered kAEGetURL handler.");
}

- (void)applicationDidFinishLaunching:(NSNotification *)aNotification {
    NSApplication *app = [NSApplication sharedApplication];
    
    // Create main menu
    NSMenu *mainMenu = [[NSMenu alloc] init];
    [app setMainMenu:mainMenu];
    
    // Application menu
    NSMenuItem *appMenuItem = [[NSMenuItem alloc] init];
    [mainMenu addItem:appMenuItem];
    NSMenu *appMenu = [[NSMenu alloc] init];
    [appMenuItem setSubmenu:appMenu];
    
    [appMenu addItemWithTitle:@"About TapeXPlayer"
                      action:@selector(orderFrontStandardAboutPanel:)
               keyEquivalent:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:@"Quit"
                      action:@selector(terminate:)
               keyEquivalent:@"q"];
    
    // File menu
    NSMenuItem *fileMenuItem = [[NSMenuItem alloc] initWithTitle:@"File" action:nil keyEquivalent:@""];
    [mainMenu addItem:fileMenuItem];
    NSMenu *fileMenu = [[NSMenu alloc] initWithTitle:@"File"];
    [fileMenuItem setSubmenu:fileMenu];
    
    NSMenuItem *openItem = [[NSMenuItem alloc] initWithTitle:@"Open"
                                                     action:@selector(openFile:)
                                              keyEquivalent:@"o"];
    [fileMenu addItem:openItem];
    
    // File menu items will be added here if needed
    
    // Edit menu
    NSMenuItem *editMenuItem = [[NSMenuItem alloc] initWithTitle:@"Edit" action:nil keyEquivalent:@""];
    [mainMenu addItem:editMenuItem];
    NSMenu *editMenu = [[NSMenu alloc] initWithTitle:@"Edit"];
    [editMenuItem setSubmenu:editMenu];
    
    NSMenuItem *copyScreenshotItem = [[NSMenuItem alloc] initWithTitle:@"Copy Screenshot to Clipboard"
                                                               action:@selector(handleMenuAction:)
                                                        keyEquivalent:@"c"];
    [copyScreenshotItem setKeyEquivalentModifierMask:NSEventModifierFlagCommand]; // For Cmd+C
    [copyScreenshotItem setTag:MENU_EDIT_COPY_SCREENSHOT];
    [copyScreenshotItem setEnabled:NO]; // Initially disabled until file is loaded
    [editMenu addItem:copyScreenshotItem];

    NSMenuItem *copyLinkItem = [[NSMenuItem alloc] initWithTitle:@"Copy Link for Obsidian (fstp://)"
                                                          action:@selector(handleMenuAction:)
                                                   keyEquivalent:@"c"];
    [copyLinkItem setKeyEquivalentModifierMask:(NSEventModifierFlagCommand | NSEventModifierFlagShift)]; // For Shift+Cmd+C
    [copyLinkItem setTag:MENU_FILE_COPY_FSTP_URL_MARKDOWN];
    [copyLinkItem setEnabled:NO]; // Initially disabled
    [editMenu addItem:copyLinkItem];
    
    [editMenu addItem:[NSMenuItem separatorItem]];
    
    NSMenuItem *gotoTimecodeItem = [[NSMenuItem alloc] initWithTitle:@"Go to Timecode"
                                                            action:@selector(handleMenuAction:)
                                                     keyEquivalent:@"g"];
    [gotoTimecodeItem setKeyEquivalentModifierMask:NSEventModifierFlagCommand]; // For Cmd+G
    [gotoTimecodeItem setTag:MENU_EDIT_GOTO_TIMECODE];
    [editMenu addItem:gotoTimecodeItem];
    
    // Interface menu
    NSMenuItem *interfaceMenuItem = [[NSMenuItem alloc] init];
    [mainMenu addItem:interfaceMenuItem];
    NSMenu *interfaceMenu = [[NSMenu alloc] initWithTitle:@"Interface"];
    [interfaceMenuItem setSubmenu:interfaceMenu];
    
    // Initial population of interface menu
    [self updateInterfaceMenu:interfaceMenu];
    
    // Audio menu
    NSMenuItem *audioMenuItem = [[NSMenuItem alloc] init];
    [mainMenu addItem:audioMenuItem];
    NSMenu *audioMenu = [[NSMenu alloc] initWithTitle:@"Audio"];
    [audioMenuItem setSubmenu:audioMenu];
    
    // Initial population of audio menu
    [self updateAudioMenu:audioMenu];
    
    // Finish launching
    [app finishLaunching];
    [app activateIgnoringOtherApps:YES];
}

- (void)openFile:(id)sender {
    handleMenuCommand(MENU_FILE_OPEN);
}

- (void)handleMenuAction:(NSMenuItem *)sender {
    handleMenuCommand((int)[sender tag]);
}

- (void)updateInterfaceMenu:(NSMenu *)menu {
    // Clear existing items
    [menu removeAllItems];
    
    if (!g_remote_control) return;
    
    // Add input devices section
    NSMenuItem *inputHeader = [[NSMenuItem alloc] initWithTitle:@"Input Devices" action:nil keyEquivalent:@""];
    [inputHeader setEnabled:NO];
    [menu addItem:inputHeader];
    
    std::vector<std::string> inputDevices = g_remote_control->get_input_devices();
    std::string currentInput = g_remote_control->get_current_input_device();
    
    for (const auto& device : inputDevices) {
        NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:[NSString stringWithUTF8String:device.c_str()]
                                                    action:@selector(selectInterface:)
                                             keyEquivalent:@""];
        [item setState:device == currentInput ? NSControlStateValueOn : NSControlStateValueOff];
        [item setTag:1]; // 1 for input devices
        [menu addItem:item];
    }
    
    [menu addItem:[NSMenuItem separatorItem]];
    
    // Add output devices section
    NSMenuItem *outputHeader = [[NSMenuItem alloc] initWithTitle:@"Output Devices" action:nil keyEquivalent:@""];
    [outputHeader setEnabled:NO];
    [menu addItem:outputHeader];
    
    std::vector<std::string> outputDevices = g_remote_control->get_output_devices();
    std::string currentOutput = g_remote_control->get_current_output_device();
    
    for (const auto& device : outputDevices) {
        NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:[NSString stringWithUTF8String:device.c_str()]
                                                    action:@selector(selectInterface:)
                                             keyEquivalent:@""];
        [item setState:device == currentOutput ? NSControlStateValueOn : NSControlStateValueOff];
        [item setTag:2]; // 2 for output devices
        [menu addItem:item];
    }
}

- (void)updateAudioMenu:(NSMenu *)menu {
    // Clear existing items
    [menu removeAllItems];
    
    // Add refresh option
    NSMenuItem *refreshItem = [[NSMenuItem alloc] initWithTitle:@"Refresh Device List" 
                                                        action:@selector(refreshAudioDevices:) 
                                                 keyEquivalent:@"r"];
    [menu addItem:refreshItem];
    [menu addItem:[NSMenuItem separatorItem]];
    
    // Add audio output devices section
    NSMenuItem *outputHeader = [[NSMenuItem alloc] initWithTitle:@"Output Devices" action:nil keyEquivalent:@""];
    [outputHeader setEnabled:NO];
    [menu addItem:outputHeader];
    
    // Get the list of audio devices
    std::vector<std::string> audioDevices = get_audio_output_devices();
    int currentDeviceIndex = get_current_audio_device_index();
    
    // Add each device to the menu
    int deviceIndex = 0;
    for (const auto& device : audioDevices) {
        NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:[NSString stringWithUTF8String:device.c_str()]
                                                     action:@selector(selectAudioDevice:)
                                              keyEquivalent:@""];
        [item setState:deviceIndex == currentDeviceIndex ? NSControlStateValueOn : NSControlStateValueOff];
        [item setTag:deviceIndex]; // Store the device index in the tag
        [menu addItem:item];
        deviceIndex++;
    }
}

- (void)refreshAudioDevices:(id)sender {
    // Update the audio menu
    [self updateAudioMenu:[sender menu]];
}

- (void)selectAudioDevice:(NSMenuItem *)sender {
    int deviceIndex = [sender tag];
    
    // Switch to the selected audio device
    if (switch_audio_device(deviceIndex)) {
        // Update the menu to show the new selection
        [self updateAudioMenu:[sender menu]];
    }
}

- (void)selectInterface:(NSMenuItem *)sender {
    if (!g_remote_control) return;
    
    NSString *deviceName = [sender title];
    bool isInput = [sender tag] == 1;
    
    if (g_remote_control->select_device(std::string([deviceName UTF8String]), isInput)) {
        // Update menu to show new selection
        [self updateInterfaceMenu:[sender menu]];
    }
}

- (void)updateCopyLinkMenuItemState:(BOOL)isFileLoaded {
    NSLog(@"[menu_system.mm] updateCopyLinkMenuItemState called with isFileLoaded: %d", isFileLoaded);
    NSMenu *mainMenu = [NSApp mainMenu];
    NSMenuItem *editMenuItem = [mainMenu itemWithTitle:@"Edit"];
    if (editMenuItem) {
        NSMenu *editMenu = [editMenuItem submenu];
        NSMenuItem *copyLinkItem = [editMenu itemWithTag:MENU_FILE_COPY_FSTP_URL_MARKDOWN];
        if (copyLinkItem) {
            NSLog(@"[menu_system.mm] Found copyLinkItem. Current enabled state: %d. Setting to: %d", [copyLinkItem isEnabled], isFileLoaded);
            [copyLinkItem setEnabled:isFileLoaded];
            NSLog(@"[menu_system.mm] After setEnabled, new state: %d", [copyLinkItem isEnabled]);
        } else {
            NSLog(@"[menu_system.mm] copyLinkItem with tag %d NOT FOUND.", MENU_FILE_COPY_FSTP_URL_MARKDOWN);
        }
    } else {
        NSLog(@"[menu_system.mm] Edit menu item NOT FOUND.");
    }
}

- (void)updateCopyScreenshotMenuItemState:(BOOL)isFileLoaded {
    NSLog(@"[menu_system.mm] updateCopyScreenshotMenuItemState called with isFileLoaded: %d", isFileLoaded);
    NSMenu *mainMenu = [NSApp mainMenu];
    NSMenuItem *editMenuItem = [mainMenu itemWithTitle:@"Edit"];
    if (editMenuItem) {
        NSMenu *editMenu = [editMenuItem submenu];
        NSMenuItem *copyScreenshotItem = [editMenu itemWithTag:MENU_EDIT_COPY_SCREENSHOT];
        if (copyScreenshotItem) {
            NSLog(@"[menu_system.mm] Found copyScreenshotItem. Current enabled state: %d. Setting to: %d", [copyScreenshotItem isEnabled], isFileLoaded);
            [copyScreenshotItem setEnabled:isFileLoaded];
            NSLog(@"[menu_system.mm] After setEnabled, new state: %d", [copyScreenshotItem isEnabled]);
        } else {
            NSLog(@"[menu_system.mm] copyScreenshotItem with tag %d NOT FOUND.", MENU_EDIT_COPY_SCREENSHOT);
        }
    } else {
        NSLog(@"[menu_system.mm] Edit menu item NOT FOUND.");
    }
}

- (BOOL)validateMenuItem:(NSMenuItem *)menuItem {
    if ([menuItem action] == @selector(handleMenuAction:)) {
        if ([menuItem tag] == MENU_FILE_OPEN) {
            return YES;
        }
        if ([menuItem tag] == MENU_FILE_COPY_FSTP_URL_MARKDOWN) {
            return YES; // Always allow the action if the menu item exists
        }
        if ([menuItem tag] == MENU_EDIT_COPY_SCREENSHOT) {
            return [menuItem isEnabled];
        }
    }
    return YES;
}

// Handle URL opening when the app is already running (NEWER SIGNATURE)
- (BOOL)application:(NSApplication *)application openURL:(NSURL *)url options:(NSDictionary<NSString*, id> *)options {
    NSLog(@"[MenuDelegate application:openURL:options:] METHOD ENTRY - URL: %@", [url absoluteString]);
    NSString *urlString = [url absoluteString];
    NSLog(@"[MenuDelegate application:openURL:options:] received URL: %@", urlString);

    if ([[url scheme] isEqualToString:@"fstp"]) {
        NSLog(@"[MenuDelegate application:openURL:options:] Is an fstp URL. Calling processIncomingFstpUrl.");
        processIncomingFstpUrl([urlString UTF8String]);
        return YES;
    } else {
        NSLog(@"[MenuDelegate application:openURL:options:] Is NOT an fstp URL.");
    }
    return NO;
}

// Новый метод для обработки kAEGetURL Apple Event
- (void)handleGetURLEvent:(NSAppleEventDescriptor *)event
           withReplyEvent:(NSAppleEventDescriptor *)replyEvent {
    NSString *urlString = [[event paramDescriptorForKeyword:keyDirectObject] stringValue];
    NSLog(@"[MenuDelegate handleGetURLEvent:] Received URL via Apple Event: %@", urlString);

    if (urlString) {
        if ([[[NSURL URLWithString:urlString] scheme] isEqualToString:@"fstp"]) {
            NSLog(@"[MenuDelegate handleGetURLEvent:] Is an fstp URL. Calling processIncomingFstpUrl.");
            processIncomingFstpUrl([urlString UTF8String]);
        } else {
            NSLog(@"[MenuDelegate handleGetURLEvent:] Is NOT an fstp URL.");
        }
    }
}

@end

static MenuDelegate *menuDelegate = nil;

void initializeMenuSystem() {
    if (menuDelegate == nil) {
        // Create the application
        [NSApplication sharedApplication];
        
        // Set up the delegate
        menuDelegate = [[MenuDelegate alloc] init];
        [NSApp setDelegate:menuDelegate];
        
        // Set up as a proper GUI application
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        
        // Initialize and activate
        [NSApp finishLaunching];
        [NSApp activateIgnoringOtherApps:YES];
    }
}

void cleanupMenuSystem() {
    if (menuDelegate != nil) {
        [NSApp setDelegate:nil];
        menuDelegate = nil;
    }
}

void updateCopyLinkMenuState(bool isFileLoaded) {
    if (menuDelegate) {
        [menuDelegate updateCopyLinkMenuItemState:isFileLoaded];
    }
}

void updateCopyScreenshotMenuState(bool isFileLoaded) {
    if (menuDelegate) {
        [menuDelegate updateCopyScreenshotMenuItemState:isFileLoaded];
    }
}

void showMenuBarTemporarily() {
    // In fullscreen mode, we can temporarily show the menu bar
    // by setting the presentation options
    NSApplication *app = [NSApplication sharedApplication];
    NSApplicationPresentationOptions opts = [app presentationOptions];
    
    // Check if we're in fullscreen mode
    if (opts & NSApplicationPresentationFullScreen) {
        // Temporarily show menu bar
        [app setPresentationOptions:(opts & ~NSApplicationPresentationAutoHideMenuBar) | NSApplicationPresentationAutoHideMenuBar];
        
        // You could also try this alternative approach:
        // This will make the menu bar appear when the mouse moves to the top
        [NSMenu setMenuBarVisible:YES];
    }
}

void toggleNativeFullscreen(void* sdlWindow) {
    if (!sdlWindow) return;
    
    // Get the native NSWindow from SDL window
    SDL_SysWMinfo wmInfo;
    SDL_VERSION(&wmInfo.version);
    
    if (SDL_GetWindowWMInfo((SDL_Window*)sdlWindow, &wmInfo)) {
        NSWindow *window = wmInfo.info.cocoa.window;
        
        // Enable native fullscreen support if not already enabled
        if (!([window collectionBehavior] & NSWindowCollectionBehaviorFullScreenPrimary)) {
            [window setCollectionBehavior:NSWindowCollectionBehaviorFullScreenPrimary];
        }
        
        // Toggle fullscreen using the native macOS method
        [window toggleFullScreen:nil];
    }
}

#else // __APPLE__

void initializeMenuSystem() {}
void cleanupMenuSystem() {}
void updateCopyLinkMenuState(bool isFileLoaded) {}
void updateCopyScreenshotMenuState(bool isFileLoaded) {}
void showMenuBarTemporarily() {}
void toggleNativeFullscreen(void* sdlWindow) {}

#endif // __APPLE__ 