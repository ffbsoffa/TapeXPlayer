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
    MENU_FILE_COPY_FSTP_URL_MARKDOWN = 102
};

void initializeMenuSystem();
void cleanupMenuSystem();
void handleMenuCommand(int command);
void updateCopyLinkMenuState(bool isFileLoaded);

// Function to be called from Objective-C when a URL is opened
void processIncomingFstpUrl(const char* url_c_str);

#ifdef __cplusplus
}
#endif

#endif // MENU_SYSTEM_H 