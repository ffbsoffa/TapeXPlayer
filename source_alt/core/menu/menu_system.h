#ifndef MENU_SYSTEM_H
#define MENU_SYSTEM_H

#ifdef __cplusplus
extern "C" {
#endif

// Menu command identifiers
enum MenuCommand {
    MENU_FILE_OPEN = 1,
    MENU_INTERFACE_SELECT = 2,
    MENU_AUDIO_DEVICE_SELECT = 3
};

void initializeMenuSystem();
void cleanupMenuSystem();
void handleMenuCommand(int command);

#ifdef __cplusplus
}
#endif

#endif // MENU_SYSTEM_H 