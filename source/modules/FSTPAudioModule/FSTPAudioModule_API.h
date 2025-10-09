#pragma once

class FSTPAudioModuleWrapper;

class FSTPAudioModule_API {
public:
    FSTPAudioModule_API();
    ~FSTPAudioModule_API();

    // Time and seek functions (moved from FSTPAudioModule_wrapper)
    static double GetPosition(FSTPAudioModuleWrapper* wrapper);
    static void SetPosition(FSTPAudioModuleWrapper* wrapper, double position_seconds);
    static double GetDuration(FSTPAudioModuleWrapper* wrapper);
    
    // Time to frame conversion functions
    static int GetCurrentFrame(FSTPAudioModuleWrapper* wrapper, double fps);
    static int GetTotalFrames(FSTPAudioModuleWrapper* wrapper, double fps);
    static double FrameToTime(int frame_number, double fps);
    static int TimeToFrame(double time_seconds, double fps);
};