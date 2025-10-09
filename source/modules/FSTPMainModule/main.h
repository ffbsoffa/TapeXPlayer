#ifndef MAIN_H
#define MAIN_H

// UI "program" - complete window system
int ui_main();

// Platform-specific main UI loop (implementation in corresponding WS file)
int RunMainUILoop();

#endif // MAIN_H