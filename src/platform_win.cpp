#ifdef _WIN32

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>

#include "platform.h"

#include <atomic>
#include <cstdio>
#include <thread>

namespace {

std::vector<wchar_t> Widen(const std::string& s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::vector<wchar_t> w((size_t)(n > 0 ? n : 1), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

std::string Narrow(const wchar_t* w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return std::string();
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    s.resize((size_t)n - 1);
    return s;
}

} // namespace

bool PlatformOpenAudioDialog(void* ownerWindow, std::string& outUtf8Path)
{
    wchar_t file[2048] = L"";
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = (HWND)ownerWindow;
    ofn.lpstrFilter = L"Audio (MP3, OGG, WAV, QOA)\0*.mp3;*.ogg;*.wav;*.qoa\0All files\0*.*\0";
    ofn.lpstrFile   = file;
    ofn.nMaxFile    = (DWORD)(sizeof(file) / sizeof(file[0]));
    ofn.lpstrTitle  = L"Choose a song";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&ofn)) return false;
    outUtf8Path = Narrow(file);
    return !outUtf8Path.empty();
}

bool PlatformReadFile(const std::string& utf8Path, std::vector<uint8_t>& out)
{
    FILE* f = _wfopen(Widen(utf8Path).data(), L"rb");
    if (!f) return false;
    _fseeki64(f, 0, SEEK_END);
    const long long size = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    bool ok = size > 0 && size < (1LL << 30);
    if (ok) {
        out.resize((size_t)size);
        ok = fread(out.data(), 1, (size_t)size, f) == (size_t)size;
    }
    fclose(f);
    return ok;
}

bool PlatformWindowMinimized(void* window) { return window && IsIconic((HWND)window) != 0; }

bool PlatformFocusWindow(void* window)
{
    HWND hwnd = (HWND)window;
    if (GetForegroundWindow() == hwnd) return true;
    // Windows only lets the foreground thread hand over the foreground, so borrow its input queue for a moment.
    const DWORD front = GetWindowThreadProcessId(GetForegroundWindow(), nullptr), self = GetCurrentThreadId();
    if (front && front != self) AttachThreadInput(front, self, TRUE);
    ShowWindow(hwnd, SW_RESTORE);
    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    if (front && front != self) AttachThreadInput(front, self, FALSE);
    return GetForegroundWindow() == hwnd;
}

static bool SendScan(void* window, WORD scan, bool extended, bool down)
{
    if (GetForegroundWindow() != (HWND)window) return false;   // never type into someone else's window
    INPUT in = {};
    in.type = INPUT_KEYBOARD;
    in.ki.wScan = scan;
    in.ki.dwFlags = KEYEVENTF_SCANCODE | (extended ? KEYEVENTF_EXTENDEDKEY : 0) | (down ? 0 : KEYEVENTF_KEYUP);
    return SendInput(1, &in, sizeof(in)) == 1;
}

bool PlatformSendLaneKey(void* window, int lane, bool down)
{
    static const WORD kScan[4] = { 0x20, 0x21, 0x24, 0x25 };   // D F J K
    return lane >= 0 && lane < 4 && SendScan(window, kScan[lane], false, down);
}

bool PlatformSendKey(void* window, int virtualKey, bool down)
{
    const bool extended = (virtualKey >= 0x21 && virtualKey <= 0x28) || virtualKey == 0x2D || virtualKey == 0x2E;   // arrows and their neighbours
    return SendScan(window, (WORD)MapVirtualKeyW((UINT)virtualKey, MAPVK_VK_TO_VSC), extended, down);
}

void PlatformSleep(double seconds)
{
    if (seconds <= 0.0) return;
    // A high-resolution waitable timer sleeps precisely, so no busy-waiting is needed to hit a frame time.
    // One timer per thread: two threads sharing a timer re-arm it under each other, and one of them never wakes.
    thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
    if (timer) {
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)(seconds * 1e7);   // relative, in 100 ns units
        if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
            WaitForSingleObject(timer, (DWORD)(seconds * 1000.0) + 100);   // bounded, so a lost timer can never hang a thread
            return;
        }
    }
    Sleep((DWORD)(seconds * 1000.0));
}

// ---------------------------------------------------------------------------
// Raw keyboard input on its own thread
// ---------------------------------------------------------------------------
// The game loop only sees keys once per frame, which at 60 Hz blurs every hit
// by up to 16 ms. A message-only window registered for Raw Input receives key
// changes as they happen, so each one gets an accurate timestamp.

namespace {

const unsigned kQueueSize = 64;   // power of two
LaneKeyEvent          gQueue[kQueueSize];
std::atomic<unsigned> gWrite{ 0 }, gRead{ 0 };
std::atomic<bool>     gRunning{ false };
std::thread           gThread;
DWORD                 gThreadId = 0;
HWND                  gGameWindow = nullptr;
bool                  gHeld[4] = {};

int LaneFromScanCode(USHORT makeCode, bool extended)
{
    if (!extended) {
        switch (makeCode) {
            case 0x20: return 0;   // D
            case 0x21: return 1;   // F
            case 0x24: return 2;   // J
            case 0x25: return 3;   // K
        }
    } else {
        switch (makeCode) {
            case 0x4B: return 0;   // Left
            case 0x50: return 1;   // Down
            case 0x48: return 2;   // Up
            case 0x4D: return 3;   // Right
        }
    }
    return -1;
}

LRESULT CALLBACK RawProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_INPUT) {
        const double now = NowSeconds();
        RAWINPUT raw;
        UINT size = sizeof(raw);
        if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
            raw.header.dwType == RIM_TYPEKEYBOARD) {
            const RAWKEYBOARD& kb = raw.data.keyboard;
            const int lane = LaneFromScanCode(kb.MakeCode, (kb.Flags & RI_KEY_E0) != 0);
            if (lane >= 0) {
                const bool down = (kb.Flags & RI_KEY_BREAK) == 0;
                const bool focused = GetForegroundWindow() == gGameWindow;
                // Auto-repeat sends "down" again while held; only edges matter.
                if (down != gHeld[lane] && (focused || !down)) {
                    gHeld[lane] = down;
                    const unsigned w = gWrite.load(std::memory_order_relaxed);
                    if (w - gRead.load(std::memory_order_acquire) < kQueueSize) {
                        gQueue[w & (kQueueSize - 1)] = { lane, down, now };
                        gWrite.store(w + 1, std::memory_order_release);
                    }
                }
            }
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void RawThread(std::atomic<int>* ready)
{
    gThreadId = GetCurrentThreadId();
    WNDCLASSW wc = {};
    wc.lpfnWndProc   = RawProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"OniriaRawInput";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);

    RAWINPUTDEVICE rid = {};
    rid.usUsagePage = 0x01;   // generic desktop
    rid.usUsage     = 0x06;   // keyboard
    rid.dwFlags     = RIDEV_INPUTSINK;
    rid.hwndTarget  = hwnd;
    const bool ok = hwnd && RegisterRawInputDevices(&rid, 1, sizeof(rid));
    ready->store(ok ? 1 : -1);
    if (!ok) return;

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) DispatchMessageW(&msg);
    DestroyWindow(hwnd);
}

} // namespace

bool PlatformStartKeyInput(void* gameWindow)
{
    if (gRunning.load()) return true;
    gGameWindow = (HWND)gameWindow;
    std::atomic<int> ready{ 0 };
    gThread = std::thread(RawThread, &ready);
    while (ready.load() == 0) Sleep(1);
    if (ready.load() < 0) { gThread.join(); return false; }
    gRunning.store(true);
    return true;
}

void PlatformStopKeyInput()
{
    if (!gRunning.exchange(false)) return;
    PostThreadMessageW(gThreadId, WM_QUIT, 0, 0);
    gThread.join();
}

int PlatformPollKeys(LaneKeyEvent* out, int maxEvents)
{
    int n = 0;
    unsigned r = gRead.load(std::memory_order_relaxed);
    const unsigned w = gWrite.load(std::memory_order_acquire);
    while (r != w && n < maxEvents) out[n++] = gQueue[r++ & (kQueueSize - 1)];
    gRead.store(r, std::memory_order_release);
    return n;
}

#else   // Other platforms: no native dialog or raw input yet; the game falls back to per-frame polling.

#include "platform.h"

#include <cstdio>
#include <thread>

bool PlatformOpenAudioDialog(void*, std::string&) { return false; }

bool PlatformReadFile(const std::string& utf8Path, std::vector<uint8_t>& out)
{
    FILE* f = fopen(utf8Path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    bool ok = size > 0;
    if (ok) {
        out.resize((size_t)size);
        ok = fread(out.data(), 1, (size_t)size, f) == (size_t)size;
    }
    fclose(f);
    return ok;
}

bool PlatformWindowMinimized(void*) { return false; }
bool PlatformFocusWindow(void*) { return false; }
bool PlatformSendLaneKey(void*, int, bool) { return false; }
bool PlatformSendKey(void*, int, bool) { return false; }

void PlatformSleep(double seconds)
{
    if (seconds > 0.0) std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
}

bool PlatformStartKeyInput(void*) { return false; }
void PlatformStopKeyInput() {}
int  PlatformPollKeys(LaneKeyEvent*, int) { return 0; }

#endif
