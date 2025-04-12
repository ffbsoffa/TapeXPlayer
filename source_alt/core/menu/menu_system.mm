#import "menu_system_impl.h"
#import "menu_system.h"
#import <Cocoa/Cocoa.h>
#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
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
    NSMenuItem *fileMenuItem = [[NSMenuItem alloc] init];
    [mainMenu addItem:fileMenuItem];
    NSMenu *fileMenu = [[NSMenu alloc] initWithTitle:@"File"];
    [fileMenuItem setSubmenu:fileMenu];
    
    NSMenuItem *openItem = [[NSMenuItem alloc] initWithTitle:@"Open"
                                                     action:@selector(openFile:)
                                              keyEquivalent:@"o"];
    [fileMenu addItem:openItem];
    
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
    [self updateAudioMenu:[(NSMenuItem *)sender menu]];
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

#else // __APPLE__

void initializeMenuSystem() {}
void cleanupMenuSystem() {}

#endif // __APPLE__ 