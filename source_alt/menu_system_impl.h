#ifndef MENU_SYSTEM_IMPL_H
#define MENU_SYSTEM_IMPL_H

#ifdef __APPLE__
#import <Cocoa/Cocoa.h>
#import <ApplicationServices/ApplicationServices.h>

@interface MenuDelegate : NSObject <NSApplicationDelegate>
- (void)applicationDidFinishLaunching:(NSNotification *)notification;
- (void)openFile:(id)sender;
- (void)selectInterface:(id)sender;
- (void)updateInterfaceMenu:(NSMenu *)menu;
@end

#endif // __APPLE__

#endif // MENU_SYSTEM_IMPL_H 