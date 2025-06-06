#ifndef MENU_SYSTEM_H
#define MENU_SYSTEM_H

#ifdef __cplusplus
extern "C" {
#endif

// Menu command identifiers
enum MenuCommand {
    MENU_FILE_OPEN = 1,
    MENU_INTERFACE_SELECT = 2,
    MENU_AUDIO_DEVICE_SELECT = 3,
    MENU_FILE_COPY_FSTP_URL_MARKDOWN = 102,
    MENU_EDIT_COPY_SCREENSHOT = 103,
    MENU_EDIT_GOTO_TIMECODE = 104,
    MENU_VIEW_TOGGLE_BETACAM_EFFECT = 105
};

void initializeMenuSystem();
void cleanupMenuSystem();
void updateCopyLinkMenuState(bool isFileLoaded);
void updateCopyScreenshotMenuState(bool isFileLoaded);
void updateBetacamEffectMenuState(bool isEnabled);
void showMenuBarTemporarily();
void toggleNativeFullscreen(void* sdlWindow);

// Function to be called from Objective-C when a URL is opened
void processIncomingFstpUrl(const char* url_c_str);

#ifdef __cplusplus
}
#endif

#endif // MENU_SYSTEM_H 