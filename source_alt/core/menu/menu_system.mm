#import <Cocoa/Cocoa.h>
#import <AppKit/AppKit.h>
#import <AppKit/NSApplication.h>
#import "menu_system_impl.h"
#import "menu_system.h"
#import "nfd.hpp"
#include "../remote/remote_control.h"
#include "../audio/mainau.h" // Include for audio device functions

extern RemoteControl* g_remote_control;

// Forward declarations for audio device functions
extern std::vector<std::string> get_audio_output_devices();
extern int get_current_audio_device_index();
extern bool switch_audio_device(int deviceIndex);

#ifdef __APPLE__

@implementation MenuDelegate

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
    
    NSMenuItem *copyLinkItem = [[NSMenuItem alloc] initWithTitle:@"Copy Link for Obsidian (fstp://)"
                                                          action:@selector(handleMenuAction:)
                                                   keyEquivalent:@"l"];
    [copyLinkItem setKeyEquivalentModifierMask:NSEventModifierFlagCommand]; // For Cmd+L
    [copyLinkItem setTag:MENU_FILE_COPY_FSTP_URL_MARKDOWN];
    [copyLinkItem setEnabled:NO]; // Initially disabled
    [fileMenu addItem:copyLinkItem];
    
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

    // Register for GetURL Apple Events
    [[NSAppleEventManager sharedAppleEventManager]
        setEventHandler:self
            andSelector:@selector(handleGetURLEvent:withReplyEvent:)
          forEventClass:kInternetEventClass
             andEventID:kAEGetURL];
    NSLog(@"[MenuDelegate applicationDidFinishLaunching:] Registered kAEGetURL handler.");
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
    NSMenuItem *fileMenuItem = [mainMenu itemWithTitle:@"File"];
    if (fileMenuItem) {
        NSMenu *fileMenu = [fileMenuItem submenu];
        NSMenuItem *copyLinkItem = [fileMenu itemWithTag:MENU_FILE_COPY_FSTP_URL_MARKDOWN];
        if (copyLinkItem) {
            NSLog(@"[menu_system.mm] Found copyLinkItem. Current enabled state: %d. Setting to: %d", [copyLinkItem isEnabled], isFileLoaded);
            [copyLinkItem setEnabled:isFileLoaded];
            NSLog(@"[menu_system.mm] After setEnabled, new state: %d", [copyLinkItem isEnabled]);
        } else {
            NSLog(@"[menu_system.mm] copyLinkItem with tag %d NOT FOUND.", MENU_FILE_COPY_FSTP_URL_MARKDOWN);
        }
    } else {
        NSLog(@"[menu_system.mm] File menu item NOT FOUND.");
    }
}

- (BOOL)validateMenuItem:(NSMenuItem *)menuItem {
    if ([menuItem action] == @selector(handleMenuAction:)) {
        if ([menuItem tag] == MENU_FILE_OPEN) {
            return YES;
        }
        if ([menuItem tag] == MENU_FILE_COPY_FSTP_URL_MARKDOWN) {
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

#else // __APPLE__

void initializeMenuSystem() {}
void cleanupMenuSystem() {}

#endif // __APPLE__ 