// Small platform layer. Kept apart from main.cpp because <windows.h> and
// raylib.h define clashing names and cannot share a translation unit.
#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

// Monotonic clock shared by the audio thread, the input thread and the game loop.
inline double NowSeconds()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// Native "open file" dialog. Returns false if the user cancelled.
bool PlatformOpenAudioDialog(void* ownerWindow, std::string& outUtf8Path);

// Reads a whole file given a UTF-8 path (raylib's own loader cannot open
// non-ASCII paths on Windows).
bool PlatformReadFile(const std::string& utf8Path, std::vector<uint8_t>& out);

// Lane key events stamped the moment the OS delivers them, independent of the
// frame rate. Lanes map to the physical D F J K keys and the arrow keys.
struct LaneKeyEvent {
    int    lane;   // 0..3
    bool   down;
    double time;   // NowSeconds() when the key changed
};

bool PlatformStartKeyInput(void* gameWindow);
void PlatformStopKeyInput();
int  PlatformPollKeys(LaneKeyEvent* out, int maxEvents);
