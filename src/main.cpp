// ONIRIA - a cozy low-poly rhythm game. A cat strolls along a path that your
// song builds; every beat puts something on the path to play with.
#define _CRT_SECURE_NO_WARNINGS

#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"

#include "analysis.h"
#include "platform.h"
#include "sfx.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static const float kPi = 3.14159265f;

// Hit windows in seconds, either side of the note.
static const double kPerfect = 0.045, kGreat = 0.090, kGood = 0.140;
static const int    kPoints[3] = { 300, 200, 100 };
static const float  kAccWeight[3] = { 1.0f, 0.7f, 0.4f };

static const double kLeadIn      = 2.0;     // quiet stroll before the song starts
static const double kBaseLatency = 0.030;   // typical shared-mode output latency; tuned further by the offset setting
static const float  kScrollBase  = 5.0f;    // world units the cat walks per second at speed 1.0
static const float  kReach       = 0.95f;   // how far ahead of the cat's centre its nose is
static const float  kCatSize     = 1.15f;

// The four things the cat can do. Each one answers one kind of object on the path.
enum Action { ACT_HOP, ACT_BAT, ACT_SWAT, ACT_RING, ACT_COUNT, ACT_STUMBLE = ACT_COUNT };
static const char* kActionKey[ACT_COUNT]  = { "D", "F", "J", "K" };
static const char* kActionName[ACT_COUNT] = { "hop", "bat", "swat", "ring" };
static const int   kKeyA[ACT_COUNT] = { KEY_D, KEY_F, KEY_J, KEY_K };
static const int   kKeyB[ACT_COUNT] = { KEY_LEFT, KEY_DOWN, KEY_UP, KEY_RIGHT };
static const float kActionTime[ACT_COUNT + 1] = { 0.40f, 0.28f, 0.34f, 0.38f, 0.35f };

// Easier difficulties use fewer actions. Chart lanes map onto them so that a
// chord always pairs something on the ground with something in the air.
static const int kLaneAction[DIFF_COUNT][4] = {
    { ACT_HOP, ACT_BAT, ACT_HOP, ACT_BAT },
    { ACT_HOP, ACT_BAT, ACT_SWAT, ACT_SWAT },
    { ACT_HOP, ACT_BAT, ACT_SWAT, ACT_RING },
};
static const int   kActionCount[DIFF_COUNT] = { 2, 3, 4 };
static const char* kDiffName[DIFF_COUNT] = { "Cozy", "Playful", "Zoomies" };

enum State { ST_MENU, ST_LOADING, ST_READY, ST_PLAYING, ST_PAUSED, ST_COUNTDOWN, ST_RESULTS, ST_WARDROBE, ST_SETTINGS };
enum Judgement { J_PERFECT, J_GREAT, J_GOOD, J_MISS };

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

struct Settings {
    int   diff     = DIFF_NORMAL;
    float speed    = 1.0f;    // walking speed multiplier
    int   offsetMs = 0;       // positive = objects arrive later
    bool  watch    = false;   // the cat plays by itself
    bool  hints    = true;    // show the key above each object
    int   sfx      = 7;       // sound effects volume, 0..10
    int   scenery  = 0;       // 0 = a journey through every place, 1.. = stay in one
    int   cat      = 0;       // which cat is walking
    int   hat      = 0;       // what it wears: 0 is nothing in each slot
    int   neck     = 1;       // starts with the bell collar
    int   back     = 0;
    int   music    = 10;      // music volume, 0..10
    int   fps      = 1;       // 0: 60, 1: up to 120, 2: as fast as the display
    int   motion   = 2;       // camera and screen effects. 0: still, 1: gentle, 2: full
};
static Settings gCfg;

static std::string ConfigPath() { return std::string(GetApplicationDirectory()) + "oniria.cfg"; }

static void LoadSettings()
{
    FILE* f = fopen(ConfigPath().c_str(), "r");
    if (!f) return;
    char key[32];
    float v;
    while (fscanf(f, "%31[^=]=%f\n", key, &v) == 2) {
        if (!strcmp(key, "diff")) gCfg.diff = std::clamp((int)v, 0, DIFF_COUNT - 1);
        else if (!strcmp(key, "speed")) gCfg.speed = std::clamp(v, 0.6f, 2.0f);
        else if (!strcmp(key, "offset")) gCfg.offsetMs = std::clamp((int)v, -300, 300);
        else if (!strcmp(key, "watch")) gCfg.watch = v != 0.0f;
        else if (!strcmp(key, "hints")) gCfg.hints = v != 0.0f;
        else if (!strcmp(key, "sfx")) gCfg.sfx = std::clamp((int)v, 0, 10);
        else if (!strcmp(key, "scenery")) gCfg.scenery = std::clamp((int)v, 0, 5);
        else if (!strcmp(key, "cat")) gCfg.cat = std::max(0, (int)v);
        else if (!strcmp(key, "hat")) gCfg.hat = std::max(0, (int)v);
        else if (!strcmp(key, "neck")) gCfg.neck = std::max(0, (int)v);
        else if (!strcmp(key, "back")) gCfg.back = std::max(0, (int)v);
        else if (!strcmp(key, "music")) gCfg.music = std::clamp((int)v, 0, 10);
        else if (!strcmp(key, "fps")) gCfg.fps = std::clamp((int)v, 0, 2);
        else if (!strcmp(key, "motion")) gCfg.motion = std::clamp((int)v, 0, 2);
    }
    fclose(f);
}

static void SaveSettings()
{
    FILE* f = fopen(ConfigPath().c_str(), "w");
    if (!f) return;
    fprintf(f, "diff=%d\nspeed=%.2f\noffset=%d\nwatch=%d\nhints=%d\nsfx=%d\nscenery=%d\ncat=%d\n",
            gCfg.diff, gCfg.speed, gCfg.offsetMs, gCfg.watch ? 1 : 0, gCfg.hints ? 1 : 0, gCfg.sfx, gCfg.scenery, gCfg.cat);
    fprintf(f, "hat=%d\nneck=%d\nback=%d\nmusic=%d\nfps=%d\nmotion=%d\n", gCfg.hat, gCfg.neck, gCfg.back, gCfg.music, gCfg.fps, gCfg.motion);
    fclose(f);
}

// ---------------------------------------------------------------------------
// Song library
// ---------------------------------------------------------------------------
// Every song that has been opened is remembered, most recent first, with the
// best result on each difficulty. Stored as tab-separated lines next to the
// executable.

struct SongEntry {
    std::string path, name;
    float duration = 0.0f;
    float best[DIFF_COUNT] = { -1.0f, -1.0f, -1.0f };   // best accuracy, -1 if never finished
    int   score[DIFF_COUNT] = {};
};
static std::vector<SongEntry> gLibrary;
static const char* const kDemoKey = "<demo>";   // stands in for a path on the built-in track

static std::string LibraryPath() { return std::string(GetApplicationDirectory()) + "oniria.songs"; }

static SongEntry* FindSong(const std::string& path)
{
    for (SongEntry& e : gLibrary) if (e.path == path) return &e;
    return nullptr;
}

static void LoadLibrary()
{
    gLibrary.clear();
    if (FILE* f = fopen(LibraryPath().c_str(), "rb")) {
        char line[4096];
        while (fgets(line, sizeof(line), f)) {
            std::vector<std::string> cols(1);
            for (const char* c = line; *c && *c != '\n' && *c != '\r'; c++) {
                if (*c == '\t') cols.emplace_back(); else cols.back() += *c;
            }
            if (cols.size() < 3 || cols[0].empty()) continue;
            SongEntry e;
            e.path = cols[0];
            e.name = cols[1];
            e.duration = (float)atof(cols[2].c_str());
            for (int d = 0; d < DIFF_COUNT; d++) {
                if (cols.size() > (size_t)(3 + d)) e.best[d] = (float)atof(cols[3 + d].c_str());
                if (cols.size() > (size_t)(6 + d)) e.score[d] = atoi(cols[6 + d].c_str());
            }
            gLibrary.push_back(e);
        }
        fclose(f);
    }
    if (!FindSong(kDemoKey)) {
        SongEntry demo;
        demo.path = kDemoKey;
        demo.name = "Demo track";
        demo.duration = 49.0f;
        gLibrary.push_back(demo);
    }
}

static void SaveLibrary()
{
    FILE* f = fopen(LibraryPath().c_str(), "wb");
    if (!f) return;
    for (const SongEntry& e : gLibrary)
        fprintf(f, "%s\t%s\t%.1f\t%.4f\t%.4f\t%.4f\t%d\t%d\t%d\n", e.path.c_str(), e.name.c_str(), e.duration,
                e.best[0], e.best[1], e.best[2], e.score[0], e.score[1], e.score[2]);
    fclose(f);
}

// Moves a song to the top of the library, adding it if it is new.
static void RememberSong(const std::string& path, const std::string& name, float duration)
{
    SongEntry entry;
    if (SongEntry* old = FindSong(path)) {
        entry = *old;
        gLibrary.erase(gLibrary.begin() + (old - gLibrary.data()));
    }
    entry.path = path;
    entry.name = name;
    entry.duration = duration;
    gLibrary.insert(gLibrary.begin(), entry);
    if (gLibrary.size() > 60) gLibrary.resize(60);
    SaveLibrary();
}

// ---------------------------------------------------------------------------
// Audio playback
// ---------------------------------------------------------------------------
// The whole song sits decoded in memory and is fed to the device from the
// audio thread. Each callback stamps (position, time) so the game can tell
// exactly where playback is between callbacks.

// Decoded stereo 16-bit audio. Owns its buffer, which comes straight from the
// decoder, so a song is never held in memory twice.
struct Pcm {
    int16_t* data = nullptr;
    size_t   frames = 0;
    void Free() { if (data) MemFree(data); data = nullptr; frames = 0; }
};

struct Player {
    Pcm      pcm;               // stereo interleaved
    std::atomic<int> sampleRate{ 48000 };
    int64_t  frames = 0;
    std::mutex mutex;           // guards pcm/frames/pos against song swaps and seeks
    std::atomic<int64_t>  pos{ 0 };
    std::atomic<bool>     playing{ false };
    std::atomic<float>    gain{ 1.0f };      // music volume
    std::atomic<uint32_t> seq{ 0 };
    std::atomic<int64_t>  stampPos{ 0 };
    std::atomic<double>   stampTime{ 0.0 };
    AudioStream stream = {};
    bool hasStream = false;
};
static Player gPlayer;

static void StampLocked(int64_t pos)
{
    gPlayer.seq.fetch_add(1);
    gPlayer.stampPos.store(pos);
    gPlayer.stampTime.store(NowSeconds());
    gPlayer.seq.fetch_add(1);
}

// Sound effects are mixed into the same callback as the song, so they get
// the same low latency. The game thread queues commands; the voices belong to
// the audio thread.
struct SfxCommand { int sound; float rate, gain; };
struct SfxVoice { const float* data = nullptr; int length = 0; double pos = 0.0, step = 1.0; float gain = 0.0f; };
static std::vector<float> gSfxData[SFX_COUNT];
static SfxCommand gSfxQueue[64];
static std::atomic<unsigned> gSfxWrite{ 0 }, gSfxRead{ 0 };
static SfxVoice gSfxVoices[16];

static void MixSfx(float* out, unsigned int frames)
{
    unsigned r = gSfxRead.load(std::memory_order_relaxed);
    const unsigned w = gSfxWrite.load(std::memory_order_acquire);
    for (; r != w; r++) {
        const SfxCommand c = gSfxQueue[r & 63];
        SfxVoice* v = &gSfxVoices[0];
        for (SfxVoice& cand : gSfxVoices) {
            if (!cand.data) { v = &cand; break; }
            if (cand.pos > v->pos) v = &cand;   // all busy: reuse the one that is furthest along
        }
        v->data = gSfxData[c.sound].data();
        v->length = (int)gSfxData[c.sound].size();
        v->pos = 0.0;
        v->step = (double)c.rate * kSfxRate / gPlayer.sampleRate;
        v->gain = c.gain;
    }
    gSfxRead.store(r, std::memory_order_release);

    for (SfxVoice& v : gSfxVoices) {
        if (!v.data) continue;
        for (unsigned int i = 0; i < frames; i++) {
            const int k = (int)v.pos;
            if (k + 1 >= v.length) { v.data = nullptr; break; }
            const float frac = (float)(v.pos - k);
            const float sample = (v.data[k] + (v.data[k + 1] - v.data[k]) * frac) * v.gain;
            out[2 * i] += sample;
            out[2 * i + 1] += sample;
            v.pos += v.step;
        }
    }
}

static void FeedAudio(void* buffer, unsigned int frames)
{
    float* out = (float*)buffer;
    if (gPlayer.playing.load(std::memory_order_relaxed) && gPlayer.mutex.try_lock()) {
        const int64_t pos = gPlayer.pos.load(std::memory_order_relaxed);
        StampLocked(pos);
        const int16_t* data = gPlayer.pcm.data;
        const int64_t total = gPlayer.frames;
        const float k = 0.9f / 32768.0f * gPlayer.gain.load(std::memory_order_relaxed);   // a little headroom for the effects on top
        for (unsigned int i = 0; i < frames; i++) {
            const int64_t f = pos + i;
            if (f >= 0 && f < total) {
                out[2 * i]     = data[2 * f] * k;
                out[2 * i + 1] = data[2 * f + 1] * k;
            } else {
                out[2 * i] = out[2 * i + 1] = 0.0f;
            }
        }
        gPlayer.pos.store(pos + frames, std::memory_order_relaxed);
        gPlayer.mutex.unlock();
    } else {
        memset(out, 0, (size_t)frames * 2 * sizeof(float));
    }
    MixSfx(out, frames);
    for (unsigned int i = 0; i < frames * 2; i++) out[i] = std::clamp(out[i], -1.0f, 1.0f);
}

// Where the audio thread is in the song right now, in seconds.
static bool AudioPosition(double& seconds)
{
    for (int tries = 0; tries < 8; tries++) {
        const uint32_t a = gPlayer.seq.load();
        if (a & 1) continue;
        const int64_t pos = gPlayer.stampPos.load();
        const double  at  = gPlayer.stampTime.load();
        if (gPlayer.seq.load() != a) continue;
        seconds = (double)pos / gPlayer.sampleRate + std::min(0.1, NowSeconds() - at);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Game state
// ---------------------------------------------------------------------------

static State gState = ST_MENU;
static State gFrameState = ST_MENU; // state when the current frame began
static bool  gSim = false;          // screenshot mode: no audio device, fixed time step
static double gSimTime = 0.0;

// A key press belongs to the screen that was showing when the frame began, so
// the Esc that pauses the game is not also read as "resume" by the pause menu.
static bool Key(int key) { return gState == gFrameState && IsKeyPressed(key); }

// Queues a sound effect. `semitones` shifts its pitch; `gain` is relative to the sound's own level.
static void PlaySfx(int sound, float semitones = 0.0f, float gain = 1.0f)
{
    static const float kLevel[SFX_COUNT] = { 0.9f, 0.8f, 0.7f, 0.65f, 0.4f, 0.4f, 0.6f, 0.35f };
    if (gSim || gCfg.sfx <= 0 || gSfxData[sound].empty()) return;
    const unsigned w = gSfxWrite.load(std::memory_order_relaxed);
    if (w - gSfxRead.load(std::memory_order_acquire) >= 64) return;
    gSfxQueue[w & 63] = SfxCommand{ sound, powf(2.0f, semitones / 12.0f), gain * kLevel[sound] * 0.6f * (gCfg.sfx / 10.0f) };
    gSfxWrite.store(w + 1, std::memory_order_release);
}

static SongAnalysis gSong;
static std::string  gSongName;
static std::string  gSongPath;            // library key of the loaded song
static int   gMenuSel = 0, gMenuScroll = 0;
static bool  gNewBest = false;
static std::string  gError;
static std::vector<float> gPathHeights;   // one sample every 0.5 s of song

static const Chart* gChart = nullptr;
static std::vector<uint8_t> gNoteState;   // 0 pending, 1..3 hit (judgement + 1), 4 missed
static std::vector<uint8_t> gNoteAction;
static std::vector<float>   gNoteWhen;    // song time at which the note was judged
static int   gHead = 0;                   // first note that is still pending
static int   gTail = 0;                   // first note that may still be on screen
static int   gScore = 0, gCombo = 0, gMaxCombo = 0, gCount[4] = {};
static float gScroll = kScrollBase;       // world units per second for this run
static double gLatency = kBaseLatency;
static double gClockBase = 0.0;           // song time = NowSeconds() - gClockBase - gLatency
static double gPausedAt = 0.0;
static float  gCountdown = 0.0f;
static bool   gRawKeys = false;           // raw input thread has delivered at least one event
static bool   gHeld[ACT_COUNT] = {};

static int   gJudgeKind = -1;
static float gJudgeAge = 9.0f, gComboPop = 0.0f;

// The cat's current move.
static int   gCatAction = -1;
static float gCatActionAge = 9.0f;

static void CatDo(int action) { gCatAction = action; gCatActionAge = 0.0f; }

// Happiness. Every hit adds to the streak and a miss empties it; `gJoy` eases
// towards a 0..1 value derived from it and drives everything festive: the
// cat's mood, flowers, lights, how much the world dances.
static float gStreak = 0.0f;
static float gJoy = 0.0f;
static float gJoyKick = 0.0f;        // jumps to 1 on every perfect hit, then decays
static float gEndJoy = 0.0f;         // mood on the results screen
static int   gMilestone = 0;
static float gMilestoneAge = 9.0f;
static float gHeartTimer = 0.0f, gStarTimer = 0.0f, gBloomMark = 0.0f;
static float gPreview = 0.0f;         // 0..1: how far the camera has moved in for the wardrobe

// Loading runs on a worker thread so the world keeps moving.
struct LoadJob {
    std::thread thread;
    std::atomic<int> stage{ 0 };   // 0 idle, 1 decoding, 2 analyzing, 3 finished
    bool ok = false;
    std::string error, name, path;
    Pcm pcm;
    int sampleRate = 0;
    SongAnalysis song;
};
static LoadJob gJob;

// Smoothed music levels and scene state.
struct Scene {
    float bass = 0, mid = 0, high = 0, beat = 0;
    int   envIndex = -1;
    float day = 0.1f;        // 0 afternoon .. 1 night
    float idleX = 0;         // where the cat is when no song is running
    float worldX = 0;        // where the cat is this frame
    float camY = 0.5f;
    float time = 0;          // seconds of animation time
    float wag = 0;           // tail phase; runs faster when the cat is happy
    float glow[ACT_COUNT] = {};
};
static Scene gScene;

enum SparkKind { SPARK_SHARD, SPARK_STAR, SPARK_HEART, SPARK_PUFF };
struct Spark { Vector3 pos, vel, col; float life, span, size, gravity; int kind; };
const int kSparkCount = 192;
static Spark gSparks[kSparkCount];
static int   gNextSpark = 0;

// Rings of light: flat ones spread on the path, upright ones burst towards the
// camera. A negative age is a delay before the ring appears.
struct Ring { Vector3 pos; float age, life, size; Vector3 col; bool upright; };
const int kRingCount = 24;
static Ring gRings[kRingCount];
static int  gNextRing = 0;

// A hit seen from the screen's point of view: a burst of streaks, drawn in 2D.
struct Impact { Vector3 pos; float age = 9.0f; int action = 0, judge = 0; };
const int kImpactCount = 8;
static Impact gImpacts[kImpactCount];
static int    gNextImpact = 0;
static Vector3 gEdgeColour = { 1, 1, 1 };   // the screen's edges glow in the colour of the last hit
static float   gEdgePower = 0.0f;

// The camera sits on springs. Every hit kicks it in a way that suits the
// action, and it settles again within about a quarter of a second.
struct CameraRig {
    float x = 0, y = 0, roll = 0, zoom = 0;         // offsets from the resting camera (units, units, radians, degrees)
    float vx = 0, vy = 0, vroll = 0, vzoom = 0;
    int   flip = 1;                                  // alternates so rolls go left, right, left
};
static CameraRig gRig;

// How much the camera and the screen are allowed to move: a setting, for people who prefer it calmer.
static float MotionScale() { return gCfg.motion >= 2 ? 1.0f : gCfg.motion == 1 ? 0.45f : 0.0f; }

static void KickCamera(float x, float y, float roll, float zoom)
{
    const float m = MotionScale();
    gRig.vx += x * m; gRig.vy += y * m; gRig.vroll += roll * m; gRig.vzoom += zoom * m;
}

// Flowers that bloom because of how you are playing.
struct Bloom { float x, z, born; int tint; float size; };
const int kBloomCount = 160;
static Bloom gBlooms[kBloomCount];
static int   gNextBloom = 0;
static uint32_t gFxSeed = 0x1234567u;

static float Rand01()
{
    gFxSeed = gFxSeed * 1664525u + 1013904223u;
    return (float)(gFxSeed >> 8) * (1.0f / 16777216.0f);
}

static uint32_t Hash(int a, int b)
{
    uint32_t h = (uint32_t)a * 0x9E3779B1u ^ ((uint32_t)b + 0x7F4A7C15u) * 0x85EBCA77u;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    return h;
}
static float Hash01(int a, int b) { return (float)(Hash(a, b) >> 8) * (1.0f / 16777216.0f); }

// ---------------------------------------------------------------------------
// Low-poly renderer
// ---------------------------------------------------------------------------
// Shapes are plain triangle lists with one colour per face. Lighting and fog
// are computed per triangle on the CPU and baked into the vertex colour, so
// everything draws through raylib's default shader in a handful of batches.
// The frame is rendered into a small canvas and scaled up by a whole number
// of pixels.

struct Tri { Vector3 a, b, c, col; };
typedef std::vector<Tri> Shape;

static Vector3 Rgb(int r, int g, int b) { return Vector3{ r / 255.0f, g / 255.0f, b / 255.0f }; }
static Vector3 Mix(Vector3 a, Vector3 b, float t) { return Vector3Lerp(a, b, t); }

static Color ToColor(Vector3 v, float alpha = 1.0f)
{
    return Color{ (unsigned char)(std::clamp(v.x, 0.0f, 1.0f) * 255.0f), (unsigned char)(std::clamp(v.y, 0.0f, 1.0f) * 255.0f),
                  (unsigned char)(std::clamp(v.z, 0.0f, 1.0f) * 255.0f), (unsigned char)(std::clamp(alpha, 0.0f, 1.0f) * 255.0f) };
}

// Makes every triangle added since `from` face away from `center`.
static void Orient(Shape& s, size_t from, Vector3 center)
{
    for (size_t i = from; i < s.size(); i++) {
        Tri& t = s[i];
        const Vector3 n = Vector3CrossProduct(Vector3Subtract(t.b, t.a), Vector3Subtract(t.c, t.a));
        const Vector3 mid = Vector3Scale(Vector3Add(Vector3Add(t.a, t.b), t.c), 1.0f / 3.0f);
        if (Vector3DotProduct(n, Vector3Subtract(mid, center)) < 0.0f) std::swap(t.b, t.c);
    }
}

static void AddBox(Shape& s, Vector3 c, Vector3 h, Vector3 col)
{
    const size_t from = s.size();
    Vector3 v[8];
    for (int i = 0; i < 8; i++)
        v[i] = Vector3{ c.x + ((i & 1) ? h.x : -h.x), c.y + ((i & 2) ? h.y : -h.y), c.z + ((i & 4) ? h.z : -h.z) };
    static const int f[6][4] = { { 0, 1, 3, 2 }, { 4, 6, 7, 5 }, { 0, 4, 5, 1 }, { 2, 3, 7, 6 }, { 0, 2, 6, 4 }, { 1, 5, 7, 3 } };
    for (int i = 0; i < 6; i++) {
        s.push_back({ v[f[i][0]], v[f[i][1]], v[f[i][2]], col });
        s.push_back({ v[f[i][0]], v[f[i][2]], v[f[i][3]], col });
    }
    Orient(s, from, c);
}

// A pyramid or cone standing on `base`.
static void AddCone(Shape& s, Vector3 base, float radius, float height, int sides, Vector3 col, float turn = 0.0f)
{
    const size_t from = s.size();
    const Vector3 apex = { base.x, base.y + height, base.z };
    for (int i = 0; i < sides; i++) {
        const float a0 = turn + 2 * kPi * i / sides, a1 = turn + 2 * kPi * (i + 1) / sides;
        const Vector3 p0 = { base.x + cosf(a0) * radius, base.y, base.z + sinf(a0) * radius };
        const Vector3 p1 = { base.x + cosf(a1) * radius, base.y, base.z + sinf(a1) * radius };
        s.push_back({ p0, p1, apex, col });
        s.push_back({ base, p1, p0, col });
    }
    Orient(s, from, Vector3{ base.x, base.y + height * 0.3f, base.z });
}

// A cylinder standing on `base`; `capCol` colours the two ends.
static void AddCylinder(Shape& s, Vector3 base, float radius, float height, int sides, Vector3 col, Vector3 capCol)
{
    const size_t from = s.size();
    const Vector3 top = { base.x, base.y + height, base.z };
    for (int i = 0; i < sides; i++) {
        const float a0 = 2 * kPi * i / sides, a1 = 2 * kPi * (i + 1) / sides;
        const Vector3 d0 = { cosf(a0) * radius, 0, sinf(a0) * radius }, d1 = { cosf(a1) * radius, 0, sinf(a1) * radius };
        const Vector3 b0 = Vector3Add(base, d0), b1 = Vector3Add(base, d1), t0 = Vector3Add(top, d0), t1 = Vector3Add(top, d1);
        s.push_back({ b0, b1, t1, col });
        s.push_back({ b0, t1, t0, col });
        s.push_back({ base, b1, b0, capCol });
        s.push_back({ top, t0, t1, capCol });
    }
    Orient(s, from, Vector3{ base.x, base.y + height * 0.5f, base.z });
}

// A faceted ball (icosahedron) stretched to `radii`.
static void AddBlob(Shape& s, Vector3 c, Vector3 radii, Vector3 col)
{
    static const float t = 1.618034f;
    static const float v[12][3] = { { -1, t, 0 }, { 1, t, 0 }, { -1, -t, 0 }, { 1, -t, 0 }, { 0, -1, t }, { 0, 1, t },
                                    { 0, -1, -t }, { 0, 1, -t }, { t, 0, -1 }, { t, 0, 1 }, { -t, 0, -1 }, { -t, 0, 1 } };
    static const int f[20][3] = { { 0, 11, 5 }, { 0, 5, 1 }, { 0, 1, 7 }, { 0, 7, 10 }, { 0, 10, 11 }, { 1, 5, 9 }, { 5, 11, 4 },
                                  { 11, 10, 2 }, { 10, 7, 6 }, { 7, 1, 8 }, { 3, 9, 4 }, { 3, 4, 2 }, { 3, 2, 6 }, { 3, 6, 8 },
                                  { 3, 8, 9 }, { 4, 9, 5 }, { 2, 4, 11 }, { 6, 2, 10 }, { 8, 6, 7 }, { 9, 8, 1 } };
    const size_t from = s.size();
    const float inv = 1.0f / sqrtf(1.0f + t * t);
    Vector3 p[12];
    for (int i = 0; i < 12; i++)
        p[i] = Vector3{ c.x + v[i][0] * inv * radii.x, c.y + v[i][1] * inv * radii.y, c.z + v[i][2] * inv * radii.z };
    for (int i = 0; i < 20; i++) s.push_back({ p[f[i][0]], p[f[i][1]], p[f[i][2]], col });
    Orient(s, from, c);
}

// A square beam from `a` to `b`.
static void AddBeam(Shape& s, Vector3 a, Vector3 b, float half, Vector3 col)
{
    const size_t from = s.size();
    const Vector3 d = Vector3Normalize(Vector3Subtract(b, a));
    const Vector3 ref = fabsf(d.x) < 0.9f ? Vector3{ 1, 0, 0 } : Vector3{ 0, 0, 1 };
    const Vector3 u = Vector3Scale(Vector3Normalize(Vector3CrossProduct(d, ref)), half);
    const Vector3 v = Vector3Scale(Vector3Normalize(Vector3CrossProduct(d, u)), half);
    Vector3 c[8];
    for (int i = 0; i < 8; i++) {
        const Vector3 end = (i & 4) ? b : a;
        c[i] = Vector3Add(end, Vector3Add(Vector3Scale(u, (i & 1) ? 1.0f : -1.0f), Vector3Scale(v, (i & 2) ? 1.0f : -1.0f)));
    }
    static const int f[6][4] = { { 0, 1, 3, 2 }, { 4, 6, 7, 5 }, { 0, 4, 5, 1 }, { 2, 3, 7, 6 }, { 0, 2, 6, 4 }, { 1, 5, 7, 3 } };
    for (int i = 0; i < 6; i++) {
        s.push_back({ c[f[i][0]], c[f[i][1]], c[f[i][2]], col });
        s.push_back({ c[f[i][0]], c[f[i][2]], c[f[i][3]], col });
    }
    Orient(s, from, Vector3Scale(Vector3Add(a, b), 0.5f));
}

// One cross-section of a lofted shape: an ellipse around `c`.
struct LoftRing { Vector3 c; float ru, rv; };

// Skins a row of rings into a closed surface. `u` and `v` are the axes each
// ring is drawn in. `colour(span, side, lift)` picks the colour of each quad;
// lift runs from -1 on the -v side to 1 on the +v side.
template <class Colour>
static void AddLoft(Shape& s, const LoftRing* rings, int count, int sides, Vector3 u, Vector3 v, Colour colour)
{
    auto point = [&](const LoftRing& r, float a) {
        return Vector3Add(r.c, Vector3Add(Vector3Scale(u, r.ru * cosf(a)), Vector3Scale(v, r.rv * sinf(a))));
    };
    auto face = [&](Vector3 a, Vector3 b, Vector3 c, Vector3 inside, Vector3 col) {   // wound to face away from `inside`
        const Vector3 n = Vector3CrossProduct(Vector3Subtract(b, a), Vector3Subtract(c, a));
        const Vector3 mid = Vector3Scale(Vector3Add(Vector3Add(a, b), c), 1.0f / 3.0f);
        if (Vector3DotProduct(n, Vector3Subtract(mid, inside)) < 0.0f) std::swap(b, c);
        s.push_back({ a, b, c, col });
    };
    for (int i = 0; i + 1 < count; i++) {
        const Vector3 axis = Vector3Lerp(rings[i].c, rings[i + 1].c, 0.5f);
        for (int k = 0; k < sides; k++) {
            const float a0 = 2 * kPi * k / sides, a1 = 2 * kPi * (k + 1) / sides;
            const Vector3 col = colour(i, k, sinf((a0 + a1) * 0.5f));
            const Vector3 p00 = point(rings[i], a0), p01 = point(rings[i], a1), p10 = point(rings[i + 1], a0), p11 = point(rings[i + 1], a1);
            face(p00, p10, p11, axis, col);
            face(p00, p11, p01, axis, col);
        }
    }
    for (int end = 0; end < 2; end++) {   // caps
        const LoftRing& r = rings[end ? count - 1 : 0];
        const Vector3 inside = rings[end ? count - 2 : 1].c;
        for (int k = 0; k < sides; k++) {
            const float a0 = 2 * kPi * k / sides, a1 = 2 * kPi * (k + 1) / sides;
            face(r.c, point(r, a0), point(r, a1), inside, colour(end ? count - 2 : 0, k, sinf((a0 + a1) * 0.5f)));
        }
    }
}

static void AddShape(Shape& dst, const Shape& src, Matrix m)
{
    for (const Tri& t : src)
        dst.push_back({ Vector3Transform(t.a, m), Vector3Transform(t.b, m), Vector3Transform(t.c, m), t.col });
}

// Colours of the world at one time of day. Everything is 0..1.
struct Palette {
    Vector3 skyTop, skyBottom, sun, ambient, grass, path;
    float stars, lamps;
};

struct Lighting {
    Vector3 camera, lightDir, sun, ambient, fog;
    float fogStart = 16.0f, fogEnd = 40.0f, saturation = 1.0f;
};
static Lighting gLight;
static Palette  gPalette;
static Vector3  gShadeGrass = { 1, 1, 1 }, gShadePath = { 1, 1, 1 };   // how the time of day darkens the ground

// Emits one lit, fogged triangle. `glow` 1 ignores lighting (lamps, sparks).
static void EmitTri(Vector3 a, Vector3 b, Vector3 c, Vector3 col, float glow = 0.0f, float alpha = 1.0f)
{
    const Vector3 n = Vector3Normalize(Vector3CrossProduct(Vector3Subtract(b, a), Vector3Subtract(c, a)));
    const float ndl = std::max(0.0f, Vector3DotProduct(n, gLight.lightDir));
    const float sky = 0.72f + 0.28f * n.y + 0.16f * std::max(0.0f, -n.y);   // sky from above, a little bounce from the ground
    float r = col.x * (gLight.ambient.x * sky + gLight.sun.x * ndl * 0.72f);
    float g = col.y * (gLight.ambient.y * sky + gLight.sun.y * ndl * 0.72f);
    float bl = col.z * (gLight.ambient.z * sky + gLight.sun.z * ndl * 0.72f);
    r += (col.x - r) * glow; g += (col.y - g) * glow; bl += (col.z - bl) * glow;
    const float lum = 0.3f * r + 0.59f * g + 0.11f * bl;
    r = lum + (r - lum) * gLight.saturation; g = lum + (g - lum) * gLight.saturation; bl = lum + (bl - lum) * gLight.saturation;

    const float dx = (a.x + b.x + c.x) / 3.0f - gLight.camera.x, dy = (a.y + b.y + c.y) / 3.0f - gLight.camera.y,
                dz = (a.z + b.z + c.z) / 3.0f - gLight.camera.z;
    float fog = (sqrtf(dx * dx + dy * dy + dz * dz) - gLight.fogStart) / (gLight.fogEnd - gLight.fogStart);
    fog = std::clamp(fog, 0.0f, 1.0f) * (1.0f - 0.6f * glow);
    r += (gLight.fog.x - r) * fog; g += (gLight.fog.y - g) * fog; bl += (gLight.fog.z - bl) * fog;

    const Color out = ToColor(Vector3{ r, g, bl }, alpha);
    rlColor4ub(out.r, out.g, out.b, out.a);
    rlVertex3f(a.x, a.y, a.z);
    rlVertex3f(b.x, b.y, b.z);
    rlVertex3f(c.x, c.y, c.z);
}

static void DrawShape(const Shape& s, const Matrix& m, Vector3 tint = Vector3{ 1, 1, 1 }, float glow = 0.0f, float alpha = 1.0f)
{
    for (const Tri& t : s)
        EmitTri(Vector3Transform(t.a, m), Vector3Transform(t.b, m), Vector3Transform(t.c, m),
                Vector3Multiply(t.col, tint), glow, alpha);
}

// Scale, then rotate about Z, then move: enough for nearly everything here.
static Matrix Place(Vector3 pos, float scale = 1.0f, float rotZ = 0.0f, float scaleY = 1.0f)
{
    Matrix m = MatrixScale(scale, scale * scaleY, scale);
    if (rotZ != 0.0f) m = MatrixMultiply(m, MatrixRotateZ(rotZ));
    return MatrixMultiply(m, MatrixTranslate(pos.x, pos.y, pos.z));
}

// Canvas: the low-resolution frame everything is drawn into.
static RenderTexture2D gCanvas = {};
static int gVW = 640, gVH = 360, gPixel = 2;

static void UpdateCanvas()
{
    const int w = std::max(1, GetScreenWidth()), h = std::max(1, GetScreenHeight());
    gPixel = std::max(1, std::min(h / 340, w / 480));   // the canvas is at least 480 x 340, so every panel fits
    const int vw = (w + gPixel - 1) / gPixel, vh = (h + gPixel - 1) / gPixel;
    if (gCanvas.id != 0 && vw == gVW && vh == gVH) return;
    if (gCanvas.id != 0) UnloadRenderTexture(gCanvas);
    gCanvas = LoadRenderTexture(vw, vh);
    SetTextureFilter(gCanvas.texture, TEXTURE_FILTER_POINT);
    gVW = vw;
    gVH = vh;
}

// ---------------------------------------------------------------------------
// Shapes
// ---------------------------------------------------------------------------

static Shape sCatBody, sCatHead, sCatEyes, sCatThigh[2], sCatShin[2], sCatTail[5];   // [0] front leg, [1] hind leg; rebuilt by BuildCat
static Shape sCatHat, sCatNeck, sCatBack, sCatWing;                                   // what it is wearing
static Shape sLog, sYarn, sWing, sButterflyBody, sBellStalk, sBell;
static Shape sTrunk, sCrown, sPine, sBush, sRock, sFlower, sMushroom, sHouse, sWindows, sLampPost, sLamp;
static Shape sSpark, sShadow, sHalo, sStar, sHeart, sRing, sRingUp, sCatBlush;
static Shape sRoof, sSnowCap, sSnowman, sPumpkin, sStump, sLeafPile, sHay, sStoneLantern, sLanternGlow, sBamboo, sGate;
static Shape sPalmTrunk, sPalmLeaves, sUmbrella, sShell, sCastle, sBoat;
static Shape sPoplar, sTuft, sWindmill, sBlades, sFence, sScarecrow, sBarn, sCrop, sReed, sDuck, sLily, sBridge;
static Shape sLanternPole, sPaperLantern, sStall, sKoi, sIgloo, sLighthouse, sLighthouseLamp, sPier, sCrate, sBarrel, sSeaStack, sCrab;

static const Vector3 kActionColor[ACT_COUNT] = { { 0.93f, 0.62f, 0.33f }, { 0.95f, 0.45f, 0.56f }, { 0.45f, 0.78f, 0.95f }, { 0.98f, 0.84f, 0.36f } };

static void BuildShapes()
{
    const Vector3 cream = Rgb(255, 244, 226), ink = Rgb(52, 40, 52);

    // Things on the path.
    {
        Shape log;
        AddCylinder(log, { 0, -0.85f, 0 }, 0.21f, 1.7f, 6, Rgb(150, 100, 70), Rgb(226, 190, 140));
        AddShape(sLog, log, MatrixMultiply(MatrixRotateX(kPi / 2), MatrixTranslate(0, 0.21f, 0)));
        AddBlob(sLog, { 0.02f, 0.42f, 0.35f }, { 0.1f, 0.06f, 0.1f }, Rgb(130, 190, 110));
    }
    AddBlob(sYarn, { 0, 0, 0 }, { 0.25f, 0.25f, 0.25f }, Rgb(242, 115, 143));
    AddBox(sYarn, { 0, 0.02f, 0 }, { 0.262f, 0.03f, 0.1f }, Rgb(255, 170, 185));
    AddBox(sYarn, { -0.3f, -0.22f, 0.1f }, { 0.16f, 0.015f, 0.02f }, Rgb(255, 170, 185));

    sWing.push_back({ { 0, 0, 0 }, { 0.34f, 0.28f, 0 }, { 0.30f, -0.02f, 0 }, Rgb(172, 227, 247) });   // right wing, facing the camera; tinted when drawn
    sWing.push_back({ { 0, 0, 0 }, { 0.30f, -0.02f, 0 }, { 0.20f, -0.22f, 0 }, Rgb(255, 255, 255) });
    AddBox(sButterflyBody, { 0, 0.02f, 0 }, { 0.025f, 0.13f, 0.025f }, ink);

    {   // The stalk grows beside the path and bends over it.
        const Vector3 stem = Rgb(104, 158, 96), leaf = Rgb(132, 190, 110);
        const Vector3 pts[5] = { { 0, 0, -1.05f }, { 0.03f, 1.25f, -1.02f }, { 0, 2.1f, -0.8f }, { 0, 2.46f, -0.4f }, { 0, 2.42f, 0.0f } };
        for (int i = 0; i < 4; i++) AddBeam(sBellStalk, pts[i], pts[i + 1], 0.04f - 0.007f * i, stem);
        AddBlob(sBellStalk, { 0.2f, 0.55f, -1.02f }, { 0.24f, 0.05f, 0.11f }, leaf);
        AddBlob(sBellStalk, { -0.18f, 0.9f, -1.02f }, { 0.2f, 0.045f, 0.1f }, leaf);
    }
    AddBox(sBell, { 0, -0.2f, 0 }, { 0.014f, 0.2f, 0.014f }, Rgb(104, 158, 96));         // hangs from its pivot
    AddCone(sBell, { 0, -0.78f, 0 }, 0.23f, 0.4f, 6, Rgb(250, 214, 92));
    AddBlob(sBell, { 0, -0.82f, 0 }, { 0.06f, 0.06f, 0.06f }, Rgb(190, 140, 60));
    AddBlob(sBell, { 0, -0.39f, 0 }, { 0.09f, 0.06f, 0.09f }, Rgb(104, 158, 96));

    // Scenery. Crowns and flowers are near white so each instance can be tinted.
    AddBox(sTrunk, { 0, 0.4f, 0 }, { 0.11f, 0.42f, 0.11f }, Rgb(126, 92, 76));
    AddBlob(sCrown, { 0, 1.3f, 0 }, { 0.78f, 0.7f, 0.78f }, Rgb(255, 255, 255));
    AddBlob(sCrown, { 0.35f, 1.0f, 0.2f }, { 0.45f, 0.4f, 0.45f }, Rgb(236, 236, 236));
    for (int i = 0; i < 3; i++) AddCone(sPine, { 0, 0.5f + i * 0.55f, 0 }, 0.75f - i * 0.18f, 0.9f, 6, Rgb(255 - i * 12, 255 - i * 12, 255 - i * 12), i * 0.5f);
    AddBlob(sBush, { 0, 0.22f, 0 }, { 0.45f, 0.32f, 0.4f }, Rgb(255, 255, 255));
    AddBlob(sRock, { 0, 0.12f, 0 }, { 0.3f, 0.2f, 0.25f }, Rgb(160, 156, 170));
    AddBox(sFlower, { 0, 0.12f, 0 }, { 0.012f, 0.12f, 0.012f }, Rgb(110, 160, 96));
    AddBlob(sFlower, { 0, 0.27f, 0 }, { 0.085f, 0.06f, 0.085f }, Rgb(255, 255, 255));
    AddBox(sMushroom, { 0, 0.09f, 0 }, { 0.04f, 0.09f, 0.04f }, cream);
    AddCone(sMushroom, { 0, 0.16f, 0 }, 0.15f, 0.14f, 6, Rgb(226, 96, 86));

    AddBox(sHouse, { 0, 0.6f, 0 }, { 0.95f, 0.6f, 0.75f }, Rgb(244, 226, 198));
    AddCone(sRoof, { 0, 1.2f, 0 }, 1.45f, 0.85f, 4, Rgb(255, 255, 255), kPi / 4);   // tinted per place: tiles, or snow
    AddBox(sHouse, { 0.5f, 1.65f, -0.2f }, { 0.12f, 0.3f, 0.12f }, Rgb(168, 130, 120));
    AddBox(sHouse, { -0.45f, 0.36f, 0.76f }, { 0.16f, 0.36f, 0.02f }, Rgb(140, 96, 76));
    AddBox(sWindows, { 0.3f, 0.7f, 0.76f }, { 0.2f, 0.17f, 0.02f }, Rgb(255, 222, 140));
    AddBox(sLampPost, { 0, 0.7f, 0 }, { 0.04f, 0.7f, 0.04f }, Rgb(88, 76, 96));
    AddBox(sLamp, { 0, 1.5f, 0 }, { 0.1f, 0.12f, 0.1f }, Rgb(255, 226, 150));

    sSpark.push_back({ { 0, 0.5f, 0 }, { -0.4f, -0.3f, 0 }, { 0.4f, -0.3f, 0 }, Rgb(255, 255, 255) });
    for (int i = 0; i < 8; i++) {
        const float a0 = 2 * kPi * i / 8, a1 = 2 * kPi * (i + 1) / 8;
        sShadow.push_back({ { 0, 0, 0 }, { cosf(a1), 0, sinf(a1) * 0.6f }, { cosf(a0), 0, sinf(a0) * 0.6f }, Rgb(255, 255, 255) });
        sHalo.push_back({ { 0, 0, 0 }, { cosf(a0), sinf(a0), 0 }, { cosf(a1), sinf(a1), 0 }, Rgb(255, 255, 255) });
    }

    // Celebration: a five-point star and a heart facing the camera, a flat ring for the ground.
    for (int i = 0; i < 10; i++) {
        const float a0 = kPi / 2 + i * kPi / 5, a1 = kPi / 2 + (i + 1) * kPi / 5;
        const float r0 = (i & 1) ? 0.45f : 1.0f, r1 = (i & 1) ? 1.0f : 0.45f;
        sStar.push_back({ { 0, 0, 0 }, { cosf(a0) * r0, sinf(a0) * r0, 0 }, { cosf(a1) * r1, sinf(a1) * r1, 0 }, Rgb(255, 255, 255) });
    }
    auto heartPoint = [](float t) {
        const float x = 16.0f * powf(sinf(t), 3.0f), y = 13.0f * cosf(t) - 5.0f * cosf(2 * t) - 2.0f * cosf(3 * t) - cosf(4 * t);
        return Vector3{ x / 17.0f, y / 17.0f, 0.0f };
    };
    for (int i = 0; i < 18; i++)
        sHeart.push_back({ { 0, 0, 0 }, heartPoint(2 * kPi * i / 18), heartPoint(2 * kPi * (i + 1) / 18), Rgb(255, 255, 255) });
    for (int i = 0; i < 14; i++) {
        const float a0 = 2 * kPi * i / 14, a1 = 2 * kPi * (i + 1) / 14;
        const Vector3 o0 = { cosf(a0), 0, sinf(a0) * 0.6f }, o1 = { cosf(a1), 0, sinf(a1) * 0.6f };
        const Vector3 i0 = Vector3Scale(o0, 0.8f), i1 = Vector3Scale(o1, 0.8f);
        sRing.push_back({ o0, o1, i1, Rgb(255, 255, 255) });
        sRing.push_back({ o0, i1, i0, Rgb(255, 255, 255) });
    }
    for (int i = 0; i < 14; i++) {
        const float a0 = 2 * kPi * i / 14, a1 = 2 * kPi * (i + 1) / 14;
        const Vector3 o0 = { cosf(a0), sinf(a0), 0 }, o1 = { cosf(a1), sinf(a1), 0 };
        sRingUp.push_back({ o0, o1, Vector3Scale(o1, 0.82f), Rgb(255, 255, 255) });
        sRingUp.push_back({ o0, Vector3Scale(o1, 0.82f), Vector3Scale(o0, 0.82f), Rgb(255, 255, 255) });
    }

    // --- Snowy village -----------------------------------------------------
    const Vector3 snow = Rgb(250, 252, 255), wood = Rgb(126, 92, 76);
    for (int i = 0; i < 3; i++)   // sits on the upper part of each pine tier
        AddCone(sSnowCap, { 0, 0.92f + i * 0.55f, 0 }, (0.75f - i * 0.18f) * 0.58f, 0.5f, 6, snow, i * 0.5f);
    AddBlob(sSnowman, { 0, 0.30f, 0 }, { 0.34f, 0.32f, 0.34f }, snow);
    AddBlob(sSnowman, { 0, 0.78f, 0 }, { 0.25f, 0.24f, 0.25f }, snow);
    AddBlob(sSnowman, { 0, 1.13f, 0 }, { 0.18f, 0.18f, 0.18f }, snow);
    AddBox(sSnowman, { 0.20f, 1.13f, 0.05f }, { 0.07f, 0.025f, 0.025f }, Rgb(244, 150, 70));
    AddBox(sSnowman, { 0.15f, 1.19f, 0.10f }, { 0.02f, 0.025f, 0.02f }, ink);
    AddBox(sSnowman, { 0.16f, 1.19f, -0.04f }, { 0.02f, 0.025f, 0.02f }, ink);
    AddBox(sSnowman, { 0, 1.30f, 0 }, { 0.17f, 0.015f, 0.17f }, ink);
    AddBox(sSnowman, { 0, 1.39f, 0 }, { 0.11f, 0.08f, 0.11f }, ink);
    AddBeam(sSnowman, { 0.15f, 0.82f, 0.18f }, { 0.40f, 1.00f, 0.42f }, 0.018f, wood);
    AddBeam(sSnowman, { 0.0f, 0.82f, -0.20f }, { -0.10f, 1.02f, -0.48f }, 0.018f, wood);

    // --- Autumn woods ------------------------------------------------------
    AddBlob(sPumpkin, { 0, 0.20f, 0 }, { 0.30f, 0.22f, 0.30f }, Rgb(238, 140, 60));
    AddBox(sPumpkin, { 0.02f, 0.44f, 0 }, { 0.03f, 0.05f, 0.03f }, Rgb(110, 150, 90));
    AddCylinder(sStump, { 0, 0, 0 }, 0.26f, 0.32f, 6, Rgb(140, 100, 78), Rgb(222, 190, 146));
    AddBlob(sLeafPile, { 0, 0.07f, 0 }, { 0.42f, 0.10f, 0.34f }, Rgb(255, 255, 255));
    AddCylinder(sHay, { 0, 0, 0 }, 0.5f, 0.7f, 8, Rgb(232, 200, 110), Rgb(214, 178, 96));

    // --- Blossom garden ----------------------------------------------------
    const Vector3 stone = Rgb(176, 172, 184), stoneDark = Rgb(140, 136, 152);
    AddBox(sStoneLantern, { 0, 0.09f, 0 }, { 0.20f, 0.09f, 0.20f }, stone);
    AddBox(sStoneLantern, { 0, 0.42f, 0 }, { 0.08f, 0.24f, 0.08f }, stone);
    AddBox(sStoneLantern, { 0, 0.70f, 0 }, { 0.22f, 0.05f, 0.22f }, stoneDark);
    AddCone(sStoneLantern, { 0, 1.00f, 0 }, 0.34f, 0.22f, 4, stoneDark, kPi / 4);
    AddBox(sLanternGlow, { 0, 0.88f, 0 }, { 0.13f, 0.12f, 0.13f }, Rgb(255, 226, 150));
    {
        const Vector3 cane = Rgb(120, 176, 96), leaf = Rgb(156, 204, 116);
        const Vector3 canes[3][2] = { { { -0.15f, 0, 0 }, { -0.22f, 2.6f, 0.05f } }, { { 0.05f, 0, 0.1f }, { 0.10f, 3.0f, 0.1f } }, { { 0.2f, 0, -0.05f }, { 0.34f, 2.3f, -0.05f } } };
        for (int i = 0; i < 3; i++) {
            AddBeam(sBamboo, canes[i][0], canes[i][1], 0.035f, cane);
            AddBlob(sBamboo, Vector3{ canes[i][1].x + 0.12f, canes[i][1].y - 0.1f, canes[i][1].z }, { 0.30f, 0.06f, 0.12f }, leaf);
            AddBlob(sBamboo, Vector3{ canes[i][1].x - 0.10f, canes[i][1].y - 0.5f, canes[i][1].z + 0.05f }, { 0.24f, 0.05f, 0.10f }, leaf);
        }
    }
    {
        const Vector3 red = Rgb(214, 86, 74);
        AddBox(sGate, { -0.9f, 1.0f, 0 }, { 0.09f, 1.0f, 0.09f }, red);
        AddBox(sGate, { 0.9f, 1.0f, 0 }, { 0.09f, 1.0f, 0.09f }, red);
        AddBox(sGate, { 0, 1.70f, 0 }, { 1.0f, 0.06f, 0.08f }, red);
        AddBox(sGate, { 0, 2.05f, 0 }, { 1.25f, 0.08f, 0.10f }, red);
        AddBox(sGate, { 0, 2.17f, 0 }, { 1.36f, 0.04f, 0.14f }, Rgb(90, 60, 70));
    }

    // --- Seaside -----------------------------------------------------------
    {
        const Vector3 bark = Rgb(170, 130, 96);
        const Vector3 spine[4] = { { 0, 0, 0 }, { 0.12f, 0.8f, 0 }, { 0.30f, 1.6f, 0 }, { 0.55f, 2.4f, 0 } };
        for (int i = 0; i < 3; i++) AddBeam(sPalmTrunk, spine[i], spine[i + 1], 0.10f - 0.015f * i, bark);
        AddBlob(sPalmTrunk, { 0.50f, 2.30f, 0.10f }, { 0.08f, 0.08f, 0.08f }, Rgb(120, 86, 70));
        const Vector3 top = spine[3];
        for (int i = 0; i < 7; i++) {   // drooping fronds
            const float a = 2 * kPi * i / 7;
            const Vector3 dir = { cosf(a), 0, sinf(a) }, side = { -sinf(a) * 0.14f, 0, cosf(a) * 0.14f };
            const Vector3 mid = { top.x + dir.x * 0.65f, top.y + 0.12f, top.z + dir.z * 0.65f };
            const Vector3 tip = { top.x + dir.x * 1.3f, top.y - 0.45f, top.z + dir.z * 1.3f };
            sPalmLeaves.push_back({ top, Vector3Add(mid, side), Vector3Subtract(mid, side), Rgb(255, 255, 255) });
            sPalmLeaves.push_back({ Vector3Add(mid, side), tip, Vector3Subtract(mid, side), Rgb(232, 232, 232) });
        }
    }
    {
        AddBeam(sUmbrella, { 0, 0, 0 }, { 0.15f, 1.5f, 0 }, 0.02f, Rgb(255, 250, 240));
        const size_t from = sUmbrella.size();
        const Vector3 top = { 0.15f, 1.62f, 0 };
        for (int i = 0; i < 8; i++) {
            const float a0 = 2 * kPi * i / 8, a1 = 2 * kPi * (i + 1) / 8;
            sUmbrella.push_back({ { 0.15f + cosf(a0) * 0.8f, 1.27f, sinf(a0) * 0.8f }, { 0.15f + cosf(a1) * 0.8f, 1.27f, sinf(a1) * 0.8f }, top,
                                  (i & 1) ? Rgb(255, 246, 230) : Rgb(238, 112, 112) });
        }
        Orient(sUmbrella, from, Vector3{ 0.15f, 1.3f, 0 });
    }
    for (int i = 0; i < 10; i++) {   // a starfish lying on the sand; tinted when drawn
        const float a0 = kPi / 2 + i * kPi / 5, a1 = kPi / 2 + (i + 1) * kPi / 5;
        const float r0 = ((i & 1) ? 0.45f : 1.0f) * 0.22f, r1 = ((i & 1) ? 1.0f : 0.45f) * 0.22f;
        sShell.push_back({ { 0, 0.05f, 0 }, { cosf(a0) * r0, 0.02f, sinf(a0) * r0 }, { cosf(a1) * r1, 0.02f, sinf(a1) * r1 }, Rgb(255, 255, 255) });
    }
    Orient(sShell, 0, Vector3{ 0, -1, 0 });
    {
        const Vector3 sand = Rgb(226, 200, 140), sandLight = Rgb(240, 218, 164);
        AddBox(sCastle, { 0, 0.13f, 0 }, { 0.34f, 0.13f, 0.28f }, sand);
        AddBox(sCastle, { 0, 0.33f, 0 }, { 0.16f, 0.10f, 0.14f }, sandLight);
        for (int i = 0; i < 4; i++) {
            const float x = (i & 1) ? 0.3f : -0.3f, z = (i & 2) ? 0.24f : -0.24f;
            AddCylinder(sCastle, { x, 0, z }, 0.10f, 0.42f, 6, sand, sandLight);
            AddCone(sCastle, { x, 0.42f, z }, 0.13f, 0.16f, 6, sandLight);
        }
    }
    AddBox(sBoat, { 0, 0.10f, 0 }, { 0.6f, 0.14f, 0.22f }, Rgb(168, 116, 96));
    AddBox(sBoat, { 0, 0.26f, 0 }, { 0.64f, 0.03f, 0.25f }, Rgb(250, 240, 226));
    AddBeam(sBoat, { 0, 0.2f, 0 }, { 0, 1.5f, 0 }, 0.025f, wood);
    sBoat.push_back({ { 0.05f, 0.42f, 0 }, { 0.05f, 1.45f, 0 }, { 0.78f, 0.46f, 0 }, Rgb(255, 252, 244) });
    sBoat.push_back({ { -0.05f, 0.50f, 0 }, { -0.50f, 0.52f, 0 }, { -0.05f, 1.20f, 0 }, Rgb(250, 190, 170) });

    // --- Things for the variations ----------------------------------------
    const Vector3 plank = Rgb(196, 160, 124), X = { 1, 0, 0 }, Z = { 0, 0, 1 };
    AddBlob(sPoplar, { 0, 1.75f, 0 }, { 0.42f, 1.15f, 0.42f }, Rgb(255, 255, 255));
    for (int i = 0; i < 3; i++) AddCone(sTuft, { (i - 1) * 0.09f, 0, (i & 1) * 0.06f }, 0.07f, 0.2f + 0.05f * i, 3, Rgb(255, 255, 255), i * 1.1f);

    {   // Windmill: a tapering tower; the blades are a separate shape so they can turn.
        const LoftRing tower[2] = { { { 0, 0, 0 }, 0.6f, 0.6f }, { { 0, 2.0f, 0 }, 0.38f, 0.38f } };
        AddLoft(sWindmill, tower, 2, 6, X, Z, [&](int, int, float) -> Vector3 { return Rgb(246, 236, 220); });
        AddCone(sWindmill, { 0, 2.0f, 0 }, 0.5f, 0.55f, 6, Rgb(170, 110, 96));
        AddBox(sWindmill, { 0, 0.3f, 0.56f }, { 0.14f, 0.3f, 0.03f }, wood);
        Shape blade;
        AddBox(blade, { 0.78f, 0, 0 }, { 0.68f, 0.1f, 0.02f }, Rgb(252, 246, 236));
        AddBox(blade, { 0.5f, 0, -0.01f }, { 0.5f, 0.02f, 0.02f }, wood);
        for (int i = 0; i < 4; i++) AddShape(sBlades, blade, MatrixRotateZ(i * kPi / 2));
        AddBlob(sBlades, { 0, 0, 0 }, { 0.1f, 0.1f, 0.06f }, wood);
    }
    for (int i = 0; i < 2; i++) AddBox(sFence, { 0.2f + i * 1.2f, 0.3f, 0 }, { 0.045f, 0.3f, 0.045f }, plank);
    AddBox(sFence, { 1.2f, 0.44f, 0 }, { 1.2f, 0.03f, 0.02f }, plank);
    AddBox(sFence, { 1.2f, 0.22f, 0 }, { 1.2f, 0.03f, 0.02f }, plank);

    AddBox(sScarecrow, { 0, 0.6f, 0 }, { 0.035f, 0.6f, 0.035f }, wood);
    AddBox(sScarecrow, { 0, 0.88f, 0 }, { 0.46f, 0.035f, 0.035f }, wood);
    AddBox(sScarecrow, { 0, 0.74f, 0 }, { 0.17f, 0.2f, 0.08f }, Rgb(206, 96, 84));
    AddBlob(sScarecrow, { 0, 1.2f, 0 }, { 0.15f, 0.15f, 0.15f }, Rgb(236, 206, 140));
    AddCone(sScarecrow, { 0, 1.28f, 0 }, 0.25f, 0.22f, 6, Rgb(150, 110, 84));

    AddBox(sBarn, { 0, 0.75f, 0 }, { 1.2f, 0.75f, 0.9f }, Rgb(196, 84, 74));
    AddCone(sBarn, { 0, 1.5f, 0 }, 1.75f, 0.8f, 4, Rgb(120, 80, 84), kPi / 4);
    AddBox(sBarn, { 0, 0.5f, 0.91f }, { 0.36f, 0.5f, 0.02f }, Rgb(250, 240, 226));
    AddBox(sBarn, { 0, 1.2f, 0.91f }, { 0.16f, 0.12f, 0.02f }, Rgb(250, 240, 226));

    for (int i = 0; i < 4; i++) AddCone(sCrop, { i * 0.52f - 0.78f, 0, 0 }, 0.17f, 0.32f, 3, Rgb(255, 255, 255), i * 0.8f);
    for (int i = 0; i < 3; i++) {
        const float x = (i - 1) * 0.1f, top = 0.7f + 0.12f * i;
        AddBeam(sReed, { x, 0, i * 0.04f }, { x + 0.04f, top, i * 0.04f }, 0.012f, Rgb(120, 170, 100));
        AddBox(sReed, { x + 0.04f, top, i * 0.04f }, { 0.028f, 0.09f, 0.028f }, Rgb(140, 100, 76));
    }
    AddBlob(sDuck, { 0, 0.09f, 0 }, { 0.16f, 0.1f, 0.11f }, Rgb(252, 250, 244));
    AddBlob(sDuck, { 0.13f, 0.22f, 0 }, { 0.07f, 0.07f, 0.07f }, Rgb(252, 250, 244));
    AddBox(sDuck, { 0.21f, 0.21f, 0 }, { 0.04f, 0.015f, 0.03f }, Rgb(244, 160, 70));
    AddBlob(sLily, { 0, 0.012f, 0 }, { 0.2f, 0.014f, 0.2f }, Rgb(96, 170, 110));
    AddBlob(sLily, { 0.03f, 0.05f, 0 }, { 0.06f, 0.045f, 0.06f }, Rgb(252, 180, 200));

    for (int i = -2; i <= 2; i++) {   // An arched bridge across the river; tinted when drawn.
        const float y = 0.42f - 0.07f * i * i, z = i * 0.84f;
        AddBox(sBridge, { 0, y, z }, { 0.42f, 0.05f, 0.44f }, Rgb(255, 255, 255));
        for (int side = -1; side <= 1; side += 2) {
            AddBox(sBridge, { side * 0.4f, y + 0.22f, z }, { 0.03f, 0.2f, 0.03f }, Rgb(232, 232, 232));
            AddBox(sBridge, { side * 0.4f, y + 0.42f, z }, { 0.025f, 0.025f, 0.44f }, Rgb(232, 232, 232));
        }
    }

    AddBeam(sLanternPole, { 0, 0, 0 }, { 0, 2.3f, 0 }, 0.03f, wood);
    AddBlob(sPaperLantern, { 0, 0, 0 }, { 0.14f, 0.17f, 0.14f }, Rgb(255, 255, 255));
    AddBox(sPaperLantern, { 0, 0.17f, 0 }, { 0.06f, 0.02f, 0.06f }, Rgb(90, 64, 70));
    AddBox(sPaperLantern, { 0, -0.17f, 0 }, { 0.06f, 0.02f, 0.06f }, Rgb(90, 64, 70));
    AddBox(sStall, { 0, 0.4f, 0 }, { 0.6f, 0.4f, 0.3f }, plank);
    for (int side = -1; side <= 1; side += 2) AddBox(sStall, { side * 0.55f, 1.1f, 0.25f }, { 0.03f, 0.4f, 0.03f }, wood);
    for (int i = -1; i <= 1; i++) AddBox(sStall, { i * 0.5f, 1.52f, 0.05f }, { 0.25f, 0.05f, 0.5f }, i ? Rgb(238, 112, 112) : Rgb(255, 246, 230));
    AddBlob(sKoi, { 0, 0, 0 }, { 0.16f, 0.035f, 0.07f }, Rgb(244, 150, 70));
    AddBlob(sKoi, { -0.15f, 0, 0 }, { 0.07f, 0.03f, 0.05f }, Rgb(252, 246, 236));
    AddBlob(sIgloo, { 0, 0, 0 }, { 0.9f, 0.72f, 0.9f }, snow);
    AddBox(sIgloo, { 0, 0.2f, 0.86f }, { 0.22f, 0.22f, 0.14f }, Rgb(96, 110, 150));

    {   // Lighthouse: red and white bands, a gallery, a lamp room and a cap.
        const LoftRing tower[4] = { { { 0, 0, 0 }, 0.5f, 0.5f }, { { 0, 1.2f, 0 }, 0.42f, 0.42f }, { { 0, 2.4f, 0 }, 0.34f, 0.34f }, { { 0, 3.4f, 0 }, 0.28f, 0.28f } };
        AddLoft(sLighthouse, tower, 4, 8, X, Z, [&](int span, int, float) -> Vector3 { return span == 1 ? Rgb(226, 96, 96) : Rgb(252, 248, 240); });
        AddBox(sLighthouse, { 0, 3.45f, 0 }, { 0.4f, 0.05f, 0.4f }, Rgb(90, 80, 100));
        AddCone(sLighthouse, { 0, 4.0f, 0 }, 0.34f, 0.35f, 8, Rgb(226, 96, 96));
        AddBox(sLighthouseLamp, { 0, 3.75f, 0 }, { 0.22f, 0.25f, 0.22f }, Rgb(255, 236, 170));
    }
    AddBox(sPier, { 0, 0.2f, -3.5f }, { 0.5f, 0.05f, 3.5f }, plank);
    for (int i = 0; i < 5; i++)
        for (int side = -1; side <= 1; side += 2)
            AddBox(sPier, { side * 0.45f, -0.3f, -0.3f - i * 1.65f }, { 0.06f, 0.55f, 0.06f }, wood);
    AddBox(sCrate, { 0, 0.2f, 0 }, { 0.2f, 0.2f, 0.2f }, plank);
    AddBox(sCrate, { 0, 0.2f, 0 }, { 0.21f, 0.03f, 0.21f }, wood);
    AddCylinder(sBarrel, { 0, 0, 0 }, 0.18f, 0.45f, 8, Rgb(150, 104, 84), Rgb(196, 160, 124));
    AddBlob(sSeaStack, { 0, 0.6f, 0 }, { 0.7f, 1.0f, 0.6f }, Rgb(150, 140, 146));
    AddBlob(sSeaStack, { 0.3f, 1.5f, 0.1f }, { 0.45f, 0.6f, 0.4f }, Rgb(164, 154, 158));
    AddBlob(sSeaStack, { -0.6f, 0.2f, 0.2f }, { 0.35f, 0.3f, 0.3f }, Rgb(140, 132, 140));
    AddBlob(sCrab, { 0, 0.06f, 0 }, { 0.12f, 0.06f, 0.09f }, Rgb(232, 96, 84));
    for (int side = -1; side <= 1; side += 2) AddBox(sCrab, { side * 0.14f, 0.1f, 0.04f }, { 0.04f, 0.04f, 0.03f }, Rgb(232, 96, 84));
}

// ---------------------------------------------------------------------------
// World
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Cats
// ---------------------------------------------------------------------------
// Seven cats share one skeleton. Each has a coat pattern, colours and a few
// proportions; the shapes are rebuilt when the player picks another.

enum CatPattern { PAT_SOLID, PAT_TABBY, PAT_TUXEDO, PAT_CALICO, PAT_POINT };

struct CatKind {
    const char* name;
    const char* note;
    int     pattern;
    Vector3 base, mark, mark2, light, eye, collar;
    float   chub, ear, fluff;   // body width, ear height, tail and cheek fur
};

static const CatKind kCats[] = {
    { "Mango",    "orange tabby", PAT_TABBY,  Rgb(240, 160, 84),  Rgb(206, 118, 56),  Rgb(206, 118, 56), Rgb(255, 244, 226), Rgb(110, 190, 120), Rgb(80, 186, 190),  1.00f, 1.00f, 1.0f },
    { "Domino",   "tuxedo",       PAT_TUXEDO, Rgb(60, 56, 72),    Rgb(46, 42, 58),    Rgb(46, 42, 58),   Rgb(252, 248, 240), Rgb(250, 214, 90),  Rgb(232, 96, 100),  1.00f, 1.05f, 1.0f },
    { "Mochi",    "calico",       PAT_CALICO, Rgb(252, 246, 236), Rgb(236, 150, 80),  Rgb(104, 84, 86),  Rgb(255, 250, 242), Rgb(240, 180, 80),  Rgb(244, 140, 170), 1.06f, 0.95f, 1.0f },
    { "Luna",     "siamese",      PAT_POINT,  Rgb(240, 226, 202), Rgb(104, 78, 76),   Rgb(104, 78, 76),  Rgb(250, 240, 222), Rgb(110, 184, 244), Rgb(176, 150, 230), 0.94f, 1.15f, 1.0f },
    { "Pebble",   "british blue", PAT_SOLID,  Rgb(142, 152, 174), Rgb(120, 130, 152), Rgb(120, 130, 152), Rgb(196, 204, 220), Rgb(244, 166, 76), Rgb(250, 212, 100), 1.18f, 0.80f, 1.0f },
    { "Cloud",    "fluffy white", PAT_SOLID,  Rgb(250, 250, 252), Rgb(226, 228, 238), Rgb(226, 228, 238), Rgb(255, 255, 255), Rgb(120, 190, 240), Rgb(120, 196, 236), 1.10f, 0.90f, 1.8f },
    { "Midnight", "black cat",    PAT_SOLID,  Rgb(52, 48, 66),    Rgb(40, 36, 52),    Rgb(40, 36, 52),   Rgb(84, 80, 100),   Rgb(196, 232, 96),  Rgb(170, 120, 230), 0.98f, 1.10f, 1.0f },
};
const int kCatCount = (int)(sizeof(kCats) / sizeof(kCats[0]));

// Builds every part of the cat, facing +x with its feet on y = 0, along with
// whatever it is wearing. Head, eyes, legs and tail segments are modelled
// around their own joints so they can be posed. Proportions are deliberately
// those of a kitten: a big head, big low-set eyes, a short body and stubby legs.
static void BuildCat(int kind)
{
    const CatKind& c = kCats[std::clamp(kind, 0, kCatCount - 1)];
    sCatBody.clear(); sCatHead.clear(); sCatEyes.clear(); sCatBlush.clear();
    sCatHat.clear(); sCatNeck.clear(); sCatBack.clear(); sCatWing.clear();
    for (Shape& s : sCatThigh) s.clear();
    for (Shape& s : sCatShin) s.clear();
    for (Shape& s : sCatTail) s.clear();

    const Vector3 X = { 1, 0, 0 }, Y = { 0, 1, 0 }, Z = { 0, 0, 1 };
    const Vector3 pink = Rgb(250, 160, 170), ink = Rgb(44, 34, 46), white = { 1, 1, 1 }, gold = Rgb(250, 214, 92);
    const Vector3 belly = c.pattern == PAT_SOLID ? Mix(c.base, c.light, 0.45f) : c.light;
    const float w = c.chub;

    // Body: rump, haunches, waist, ribs, chest, neck. Short and round.
    const LoftRing body[7] = {
        { { -0.42f, 0.44f, 0 }, 0.07f * w, 0.08f },
        { { -0.33f, 0.44f, 0 }, 0.22f * w, 0.22f },
        { { -0.17f, 0.43f, 0 }, 0.26f * w, 0.26f },
        { {  0.00f, 0.43f, 0 }, 0.25f * w, 0.25f },
        { {  0.15f, 0.45f, 0 }, 0.25f * w, 0.25f },
        { {  0.27f, 0.49f, 0 }, 0.22f * w, 0.22f },
        { {  0.37f, 0.55f, 0 }, 0.15f, 0.15f },
    };
    AddLoft(sCatBody, body, 7, 8, Z, Y, [&](int span, int side, float lift) -> Vector3 {
        const bool top = lift > 0.3f, under = lift < -0.5f;
        switch (c.pattern) {
            case PAT_TABBY:  return under ? c.light : (top && (span & 1)) ? c.mark : c.base;
            case PAT_TUXEDO: return under || (span >= 4 && lift < 0.2f) ? c.light : c.base;
            case PAT_CALICO: {
                if (under) return c.light;
                const uint32_t patch = Hash(span * 7 + 3, side / 2) % 6;
                return patch < 2 ? c.mark : patch == 2 ? c.mark2 : c.base;
            }
            case PAT_POINT:  return under ? c.light : span == 0 ? Mix(c.base, c.mark, 0.5f) : c.base;
            default:         return under ? belly : c.base;
        }
    });

    // Head, around the neck joint: very round, wide cheeks, a tiny muzzle.
    const float hw = 1.0f + (c.chub - 1.0f) * 0.6f + (c.fluff - 1.0f) * 0.12f;
    const LoftRing head[7] = {
        { { -0.19f, 0.03f, 0 }, 0.17f, 0.16f },
        { { -0.07f, 0.04f, 0 }, 0.32f * hw, 0.29f },
        { {  0.08f, 0.04f, 0 }, 0.38f * hw, 0.33f },
        { {  0.21f, 0.02f, 0 }, 0.34f * hw, 0.29f },
        { {  0.30f, -0.01f, 0 }, 0.24f * hw, 0.21f },
        { {  0.345f, -0.05f, 0 }, 0.15f, 0.115f },
        { {  0.375f, -0.065f, 0 }, 0.09f, 0.07f },
    };
    AddLoft(sCatHead, head, 7, 10, Z, Y, [&](int span, int side, float lift) -> Vector3 {
        const bool muzzle = span >= 4, face = span == 3;
        switch (c.pattern) {
            case PAT_TABBY:
                if (muzzle) return lift < 0.6f ? c.light : c.base;
                if (face) return lift < -0.2f ? c.light : c.base;
                return (lift > 0.3f && span == 1) ? c.mark : c.base;
            case PAT_TUXEDO:
                if (muzzle) return c.light;
                return (face && lift < 0.0f) || (span == 2 && lift < -0.5f) ? c.light : c.base;
            case PAT_CALICO: {
                if (muzzle || (face && lift < 0.0f)) return c.light;
                const uint32_t patch = Hash(span * 5 + 11, side / 2) % 5;
                return patch < 2 ? c.mark : patch == 2 ? c.mark2 : c.base;
            }
            case PAT_POINT:  return span >= 3 ? c.mark : span == 2 ? Mix(c.base, c.mark, 0.4f) : c.base;
            default:         return muzzle && lift < 0.4f ? belly : c.base;
        }
    });
    AddBlob(sCatHead, { 0.39f, -0.055f, 0 }, { 0.028f, 0.022f, 0.04f }, c.pattern == PAT_POINT ? Rgb(84, 60, 64) : pink);   // nose
    AddBox(sCatHead, { 0.375f, -0.105f, 0 }, { 0.008f, 0.012f, 0.032f }, ink);                                           // a little mouth
    const Vector3 whisker = Mix(c.light, white, 0.6f);
    for (int side = -1; side <= 1; side += 2) {
        const float z = (float)side;
        // Ear: a three-sided pyramid with a pink panel on its front face.
        const Vector3 tip = { -0.02f, 0.33f + 0.2f * c.ear, z * 0.30f * hw };
        const Vector3 a = { 0.14f, 0.25f, z * 0.11f }, b = { 0.04f, 0.15f, z * 0.39f * hw }, d = { -0.14f, 0.21f, z * 0.25f };
        const Vector3 centre = Vector3Scale(Vector3Add(Vector3Add(a, b), Vector3Add(d, tip)), 0.25f);
        const Vector3 fur = c.pattern == PAT_POINT || c.pattern == PAT_TABBY ? c.mark : c.pattern == PAT_CALICO ? (side < 0 ? c.mark : c.mark2) : c.base;
        const size_t from = sCatHead.size();
        sCatHead.push_back({ a, b, tip, fur });
        sCatHead.push_back({ b, d, tip, fur });
        sCatHead.push_back({ d, a, tip, fur });
        const Vector3 front = Vector3Scale(Vector3Add(Vector3Add(a, b), tip), 1.0f / 3.0f);
        auto inset = [&](Vector3 q) { return Vector3Add(Vector3Lerp(q, front, 0.32f), Vector3{ 0.014f, 0.004f, 0 }); };
        sCatHead.push_back({ inset(a), inset(b), inset(tip), pink });
        Orient(sCatHead, from, centre);

        // Fluffy cheeks and two whiskers a side.
        AddBlob(sCatHead, { 0.10f, -0.17f, z * 0.33f * hw }, { 0.10f * c.fluff, 0.075f, 0.08f }, c.fluff > 1.3f ? c.light : c.pattern == PAT_POINT ? Mix(c.base, c.mark, 0.4f) : c.base);
        AddBeam(sCatHead, { 0.30f, -0.07f, z * 0.20f }, { 0.33f, -0.05f, z * 0.46f }, 0.0035f, whisker);
        AddBeam(sCatHead, { 0.30f, -0.10f, z * 0.20f }, { 0.32f, -0.13f, z * 0.44f }, 0.0035f, whisker);

        // Big eyes set low and wide: iris, pupil and two highlights. Modelled around eye height so they can close.
        AddBlob(sCatEyes, { 0.270f, 0.0f, z * 0.225f * hw }, { 0.070f, 0.115f, 0.085f }, c.eye);
        AddBlob(sCatEyes, { 0.308f, 0.0f, z * 0.230f * hw }, { 0.050f, 0.095f, 0.060f }, ink);
        AddBox(sCatEyes, { 0.356f, 0.05f, z * 0.230f * hw + 0.025f }, { 0.014f, 0.024f, 0.018f }, white);
        AddBox(sCatEyes, { 0.360f, -0.04f, z * 0.230f * hw - 0.02f }, { 0.008f, 0.012f, 0.010f }, white);
        AddBlob(sCatBlush, { 0.215f, -0.115f, z * 0.262f * hw }, { 0.05f, 0.042f, 0.05f }, Rgb(255, 150, 165));   // a rosy patch, set into the cheek
    }

    // Legs hang from their joints: a short thigh, then a shin ending in a big round paw.
    const Vector3 thighCol[2] = { c.base, c.pattern == PAT_CALICO ? c.mark2 : c.base };
    const Vector3 shinCol = c.pattern == PAT_TUXEDO || c.pattern == PAT_CALICO ? c.light : c.pattern == PAT_POINT ? c.mark : c.base;
    const Vector3 pawCol = c.pattern == PAT_POINT ? c.mark : c.pattern == PAT_SOLID ? belly : c.light;
    const LoftRing frontThigh[3] = { { { 0, 0.03f, 0 }, 0.085f, 0.078f }, { { 0, -0.06f, 0 }, 0.078f, 0.070f }, { { 0, -0.13f, 0 }, 0.066f, 0.062f } };
    const LoftRing hindThigh[3] = { { { 0, 0.05f, 0 }, 0.125f, 0.095f }, { { -0.01f, -0.06f, 0 }, 0.105f, 0.082f }, { { 0, -0.13f, 0 }, 0.070f, 0.064f } };
    const LoftRing shin[2] = { { { 0, 0.01f, 0 }, 0.064f, 0.060f }, { { 0, -0.07f, 0 }, 0.058f, 0.056f } };
    for (int leg = 0; leg < 2; leg++) {
        const Vector3 col = thighCol[leg];
        AddLoft(sCatThigh[leg], leg ? hindThigh : frontThigh, 3, 6, X, Z, [&](int span, int, float) -> Vector3 {
            return c.pattern == PAT_TABBY && span == 0 && leg == 1 ? c.mark : col;
        });
        AddLoft(sCatShin[leg], shin, 2, 6, X, Z, [&](int, int, float) -> Vector3 { return shinCol; });
        AddBlob(sCatShin[leg], { 0.035f, -0.105f, 0 }, { 0.095f, 0.05f, 0.078f }, pawCol);
    }

    // Tail: five tapering segments, each around the joint at its base.
    for (int i = 0; i < 5; i++) {
        auto radius = [&](int k) {
            const float taper = 0.07f - 0.008f * k;
            return c.fluff > 1.3f ? taper * (1.0f + 0.9f * sinf(kPi * (k + 0.6f) / 5.6f)) : taper;
        };
        const LoftRing seg[2] = { { { 0, 0, 0 }, radius(i), radius(i) }, { { 0, 0.19f, 0 }, radius(i + 1), radius(i + 1) } };
        Vector3 col = c.base;
        if (c.pattern == PAT_TABBY) col = (i & 1) ? c.mark : c.base;
        else if (c.pattern == PAT_TUXEDO) col = i == 4 ? c.light : c.base;
        else if (c.pattern == PAT_CALICO) col = (i & 1) ? c.mark2 : c.mark;
        else if (c.pattern == PAT_POINT) col = c.mark;
        AddLoft(sCatTail[i], seg, 2, 6, X, Z, [&](int, int, float) -> Vector3 { return col; });
    }

    // ---- On the head (head space; the crown of the head is around y = 0.31) ----
    const Vector3 top = { 0.06f, 0.29f, 0 };
    static const Vector3 kPetals[4] = { { 1.0f, 0.62f, 0.72f }, { 1.0f, 0.88f, 0.45f }, { 1.0f, 1.0f, 1.0f }, { 0.78f, 0.72f, 1.0f } };
    switch (gCfg.hat) {
        case 1: {   // bow, by the near ear
            const Vector3 ribbon = Rgb(242, 112, 142);
            AddBlob(sCatHat, { 0.07f, 0.31f, 0.11f }, { 0.045f, 0.075f, 0.085f }, ribbon);
            AddBlob(sCatHat, { 0.07f, 0.29f, 0.29f }, { 0.045f, 0.075f, 0.085f }, ribbon);
            AddBlob(sCatHat, { 0.085f, 0.30f, 0.20f }, { 0.045f, 0.045f, 0.04f }, Rgb(214, 86, 118));
            break;
        }
        case 2:     // flower crown
            for (int i = 0; i < 9; i++) {
                const float a = 2 * kPi * i / 9;
                AddBlob(sCatHat, { top.x + cosf(a) * 0.27f, 0.255f - 0.03f * cosf(a), sinf(a) * 0.29f * hw }, { 0.065f, 0.05f, 0.065f }, kPetals[i & 3]);
                AddBlob(sCatHat, { top.x + cosf(a + 0.35f) * 0.26f, 0.235f - 0.03f * cosf(a), sinf(a + 0.35f) * 0.28f * hw }, { 0.04f, 0.03f, 0.04f }, Rgb(120, 180, 110));
            }
            break;
        case 3: {   // party hat, striped, with a pom on top
            const size_t from = sCatHat.size();
            const Vector3 tipAt = { top.x - 0.03f, top.y + 0.40f, 0 };
            for (int i = 0; i < 8; i++) {
                const float a0 = 2 * kPi * i / 8, a1 = 2 * kPi * (i + 1) / 8;
                sCatHat.push_back({ { top.x + cosf(a0) * 0.16f, top.y - 0.02f, sinf(a0) * 0.16f }, { top.x + cosf(a1) * 0.16f, top.y - 0.02f, sinf(a1) * 0.16f }, tipAt,
                                    (i & 1) ? Rgb(255, 222, 110) : Rgb(120, 200, 236) });
            }
            Orient(sCatHat, from, Vector3{ top.x, top.y + 0.1f, 0 });
            AddBlob(sCatHat, tipAt, { 0.05f, 0.05f, 0.05f }, Rgb(250, 130, 150));
            break;
        }
        case 4: {   // straw hat
            const Vector3 straw = Rgb(240, 214, 150), strawLight = Rgb(250, 230, 176);
            AddCylinder(sCatHat, { top.x, top.y - 0.04f, 0 }, 0.46f, 0.03f, 10, straw, strawLight);
            AddCylinder(sCatHat, { top.x, top.y - 0.02f, 0 }, 0.23f, 0.16f, 8, straw, strawLight);
            AddCylinder(sCatHat, { top.x, top.y - 0.01f, 0 }, 0.238f, 0.045f, 8, Rgb(226, 100, 100), Rgb(226, 100, 100));
            break;
        }
        case 5: {   // beanie
            const Vector3 wool = Rgb(96, 180, 190), cuff = Rgb(240, 246, 246);
            AddBlob(sCatHat, { top.x, top.y - 0.07f, 0 }, { 0.35f, 0.27f, 0.37f * hw }, wool);
            const LoftRing band[2] = { { { top.x, top.y - 0.10f, 0 }, 0.385f * hw, 0.365f }, { { top.x, top.y - 0.02f, 0 }, 0.36f * hw, 0.345f } };
            AddLoft(sCatHat, band, 2, 10, Z, X, [&](int, int, float) -> Vector3 { return cuff; });
            AddBlob(sCatHat, { top.x, top.y + 0.22f, 0 }, { 0.075f, 0.075f, 0.075f }, cuff);
            break;
        }
        case 6: {   // witch hat
            const Vector3 cloth = Rgb(110, 84, 150);
            AddCylinder(sCatHat, { top.x, top.y - 0.04f, 0 }, 0.44f, 0.03f, 10, cloth, Rgb(128, 100, 170));
            AddCone(sCatHat, { top.x, top.y - 0.02f, 0 }, 0.24f, 0.58f, 7, cloth);
            AddCylinder(sCatHat, { top.x, top.y - 0.01f, 0 }, 0.245f, 0.055f, 7, Rgb(60, 46, 70), Rgb(60, 46, 70));
            AddBox(sCatHat, { top.x + 0.24f, top.y + 0.017f, 0 }, { 0.015f, 0.035f, 0.045f }, gold);
            break;
        }
        case 7: {   // top hat
            const Vector3 felt = Rgb(58, 50, 68);
            AddCylinder(sCatHat, { top.x, top.y - 0.03f, 0 }, 0.32f, 0.03f, 10, felt, Rgb(74, 66, 86));
            AddCylinder(sCatHat, { top.x, top.y - 0.01f, 0 }, 0.20f, 0.34f, 8, felt, Rgb(74, 66, 86));
            AddCylinder(sCatHat, { top.x, top.y, 0 }, 0.207f, 0.06f, 8, Rgb(226, 100, 100), Rgb(226, 100, 100));
            break;
        }
        case 8: {   // crown
            AddCylinder(sCatHat, { top.x, top.y - 0.01f, 0 }, 0.20f, 0.10f, 8, gold, Rgb(255, 232, 140));
            for (int i = 0; i < 6; i++) {
                const float a = 2 * kPi * (i + 0.5f) / 6;
                AddCone(sCatHat, { top.x + cosf(a) * 0.16f, top.y + 0.08f, sinf(a) * 0.16f }, 0.055f, 0.13f, 4, gold);
                AddBlob(sCatHat, { top.x + cosf(a) * 0.20f, top.y + 0.04f, sinf(a) * 0.20f }, { 0.028f, 0.028f, 0.028f }, (i & 1) ? Rgb(236, 96, 110) : Rgb(110, 180, 240));
            }
            break;
        }
        case 9: {   // headphones
            const Vector3 shell = Rgb(240, 128, 144), bandCol = Rgb(70, 60, 80);
            const float ez = 0.39f * hw;
            const Vector3 arc[5] = { { top.x, 0.10f, ez }, { top.x, 0.30f, ez * 0.75f }, { top.x, 0.38f, 0 }, { top.x, 0.30f, -ez * 0.75f }, { top.x, 0.10f, -ez } };
            for (int i = 0; i < 4; i++) AddBeam(sCatHat, arc[i], arc[i + 1], 0.022f, bandCol);
            for (int side = -1; side <= 1; side += 2) {
                AddBlob(sCatHat, { top.x, 0.03f, side * ez }, { 0.12f, 0.14f, 0.07f }, shell);
                AddBlob(sCatHat, { top.x, 0.03f, side * (ez + 0.04f) }, { 0.07f, 0.08f, 0.04f }, bandCol);
            }
            break;
        }
        case 10:    // halo
            for (int i = 0; i < 12; i++) {
                const float a0 = 2 * kPi * i / 12, a1 = 2 * kPi * (i + 1) / 12, y = top.y + 0.34f;
                const Vector3 o0 = { top.x + cosf(a0) * 0.22f, y, sinf(a0) * 0.22f }, o1 = { top.x + cosf(a1) * 0.22f, y, sinf(a1) * 0.22f };
                const Vector3 i0 = { top.x + cosf(a0) * 0.15f, y + 0.02f, sinf(a0) * 0.15f }, i1 = { top.x + cosf(a1) * 0.15f, y + 0.02f, sinf(a1) * 0.15f };
                sCatHat.push_back({ o0, o1, i1, Rgb(255, 236, 150) });
                sCatHat.push_back({ o0, i1, i0, Rgb(255, 246, 190) });
            }
            break;
    }

    // ---- Around the neck (cat space) ----
    const LoftRing collar[2] = { { { 0.285f, 0.502f, 0 }, 0.210f * w + 0.014f, 0.222f }, { { 0.335f, 0.532f, 0 }, 0.176f * w + 0.014f, 0.188f } };
    const Vector3 chin = { 0.37f, 0.365f, 0 };   // just under the neck, at the front
    switch (gCfg.neck) {
        case 1:     // bell collar, in the cat's own colour
            AddLoft(sCatNeck, collar, 2, 8, Z, Y, [&](int, int, float) -> Vector3 { return c.collar; });
            AddBlob(sCatNeck, chin, { 0.048f, 0.048f, 0.048f }, gold);
            break;
        case 2: {   // bow tie
            const Vector3 silk = Rgb(226, 86, 100);
            AddLoft(sCatNeck, collar, 2, 8, Z, Y, [&](int, int, float) -> Vector3 { return Rgb(60, 52, 70); });
            AddBlob(sCatNeck, { chin.x, chin.y, 0.085f }, { 0.04f, 0.065f, 0.075f }, silk);
            AddBlob(sCatNeck, { chin.x, chin.y, -0.085f }, { 0.04f, 0.065f, 0.075f }, silk);
            AddBlob(sCatNeck, { chin.x + 0.015f, chin.y, 0 }, { 0.04f, 0.04f, 0.035f }, Rgb(190, 66, 84));
            break;
        }
        case 3: {   // scarf, striped, with one end hanging
            const LoftRing wrap[2] = { { { 0.265f, 0.495f, 0 }, 0.245f * w + 0.03f, 0.25f }, { { 0.345f, 0.540f, 0 }, 0.20f * w + 0.03f, 0.21f } };
            AddLoft(sCatNeck, wrap, 2, 8, Z, Y, [&](int, int side, float) -> Vector3 { return (side & 1) ? Rgb(236, 110, 110) : Rgb(250, 240, 226); });
            AddBox(sCatNeck, { 0.33f, 0.33f, 0.21f * w }, { 0.035f, 0.12f, 0.05f }, Rgb(236, 110, 110));
            AddBox(sCatNeck, { 0.33f, 0.20f, 0.21f * w }, { 0.037f, 0.025f, 0.052f }, Rgb(250, 240, 226));
            break;
        }
        case 4: {   // bandana
            const Vector3 cloth = Rgb(232, 98, 98);
            AddLoft(sCatNeck, collar, 2, 8, Z, Y, [&](int, int, float) -> Vector3 { return cloth; });
            sCatNeck.push_back({ { 0.385f, 0.47f, 0.18f }, { 0.385f, 0.47f, -0.18f }, { 0.44f, 0.25f, 0 }, cloth });
            sCatNeck.push_back({ { 0.39f, 0.44f, 0.08f }, { 0.39f, 0.44f, -0.08f }, { 0.425f, 0.34f, 0 }, Rgb(250, 240, 226) });
            break;
        }
        case 5:     // flower garland
            for (int i = 0; i < 9; i++) {
                const float a = 2 * kPi * i / 9;
                AddBlob(sCatNeck, { 0.31f + 0.03f * sinf(a), 0.517f + 0.215f * sinf(a), 0.215f * w * cosf(a) }, { 0.055f, 0.055f, 0.055f }, kPetals[i & 3]);
            }
            break;
    }

    // ---- On the back (cat space; the top of the back is around y = 0.69) ----
    switch (gCfg.back) {
        case 1: {   // backpack
            const Vector3 canvas = Rgb(96, 176, 186);
            AddBox(sCatBack, { -0.04f, 0.74f, 0 }, { 0.15f, 0.12f, 0.19f }, canvas);
            AddBox(sCatBack, { -0.04f, 0.83f, 0 }, { 0.155f, 0.04f, 0.195f }, Rgb(70, 140, 156));
            AddBox(sCatBack, { -0.20f, 0.71f, 0 }, { 0.02f, 0.07f, 0.11f }, Rgb(250, 222, 130));
            for (int side = -1; side <= 1; side += 2) AddBox(sCatBack, { 0.02f, 0.60f, side * 0.21f }, { 0.03f, 0.13f, 0.012f }, Rgb(70, 140, 156));
            break;
        }
        case 2:     // balloon; modelled around the knot where its string is tied, so it can sway
            AddBeam(sCatBack, { 0, 0, 0 }, { -0.10f, 0.78f, 0 }, 0.008f, white);
            AddBlob(sCatBack, { -0.12f, 1.02f, 0 }, { 0.21f, 0.25f, 0.21f }, Rgb(240, 110, 120));
            AddCone(sCatBack, { -0.105f, 0.74f, 0 }, 0.04f, 0.05f, 4, Rgb(214, 86, 100));
            AddBox(sCatBack, { -0.05f, 1.10f, 0.15f }, { 0.02f, 0.05f, 0.02f }, Rgb(255, 214, 220));
            break;
        case 3:     // wings; one wing, drawn once for each side
            sCatWing.push_back({ { 0, 0, 0 }, { -0.34f, 0.30f, 0 }, { -0.42f, 0.04f, 0 }, Rgb(255, 255, 255) });
            sCatWing.push_back({ { 0, 0, 0 }, { -0.42f, 0.04f, 0 }, { -0.30f, -0.12f, 0 }, Rgb(236, 240, 252) });
            sCatWing.push_back({ { 0, 0, 0 }, { -0.16f, 0.24f, 0 }, { -0.34f, 0.30f, 0 }, Rgb(246, 248, 255) });
            break;
    }
}

// Sky top, sky bottom, sunlight, ambient light, grass, path, stars, lamps.
static Palette DayKey(int i)
{
    switch (i) {
        case 0:  return { Rgb(112, 180, 232), Rgb(240, 228, 196), Rgb(255, 240, 214), Rgb(112, 128, 160), Rgb(140, 196, 110), Rgb(232, 206, 160), 0.0f, 0.0f };   // afternoon
        case 1:  return { Rgb(92, 104, 178), Rgb(252, 176, 124), Rgb(255, 186, 136), Rgb(128, 108, 140), Rgb(140, 168, 100), Rgb(236, 190, 150), 0.0f, 0.35f };   // sunset
        case 2:  return { Rgb(44, 46, 106), Rgb(196, 116, 146), Rgb(190, 140, 170), Rgb(92, 88, 136), Rgb(88, 128, 110), Rgb(170, 140, 150), 0.5f, 0.9f };        // dusk
        default: return { Rgb(16, 20, 56), Rgb(56, 66, 118), Rgb(130, 150, 215), Rgb(64, 74, 122), Rgb(58, 96, 104), Rgb(112, 114, 150), 1.0f, 1.0f };            // night
    }
}
static const float kDayAt[4] = { 0.0f, 0.42f, 0.7f, 1.0f };

static Palette PaletteAt(float day)
{
    day = std::clamp(day, 0.0f, 1.0f);
    int i = 0;
    while (i < 2 && day > kDayAt[i + 1]) i++;
    const float t = (day - kDayAt[i]) / (kDayAt[i + 1] - kDayAt[i]);
    const Palette a = DayKey(i), b = DayKey(i + 1);
    return Palette{ Mix(a.skyTop, b.skyTop, t), Mix(a.skyBottom, b.skyBottom, t), Mix(a.sun, b.sun, t), Mix(a.ambient, b.ambient, t),
                    Mix(a.grass, b.grass, t), Mix(a.path, b.path, t), a.stars + (b.stars - a.stars) * t, a.lamps + (b.lamps - a.lamps) * t };
}

static bool PathFollowsSong() { return gState == ST_PLAYING || gState == ST_PAUSED || gState == ST_COUNTDOWN; }

// ---------------------------------------------------------------------------
// Places
// ---------------------------------------------------------------------------
// The walk passes through five places. During a song it starts in one chosen
// by the song's name and moves on every half minute or so; the menus stroll
// through all of them. Each place has its own ground, plants, props and weather.

enum Biome { BIOME_MEADOW, BIOME_BLOSSOM, BIOME_AUTUMN, BIOME_SNOW, BIOME_SEASIDE, BIOME_COUNT };
static const char* kBiomeName[BIOME_COUNT] = { "Meadow", "Blossom garden", "Autumn woods", "Snowy village", "Seaside" };
static const float kSeaLevel = -0.35f;

struct BiomeLook { Vector3 grass, path, sky; float skyMix; };

static BiomeLook LookOf(int biome)
{
    switch (biome) {
        case BIOME_BLOSSOM: return { Rgb(142, 200, 150), Rgb(228, 216, 200), Rgb(255, 206, 226), 0.20f };
        case BIOME_AUTUMN:  return { Rgb(198, 172, 98), Rgb(216, 172, 122), Rgb(255, 200, 140), 0.16f };
        case BIOME_SNOW:    return { Rgb(238, 244, 252), Rgb(160, 184, 220), Rgb(214, 226, 244), 0.34f };
        case BIOME_SEASIDE: return { Rgb(242, 224, 172), Rgb(200, 164, 124), Rgb(140, 224, 232), 0.20f };
        default:            return { Rgb(140, 196, 110), Rgb(232, 206, 160), Rgb(255, 255, 255), 0.0f };
    }
}

// Each place comes in three variations with their own landmarks and props:
//   Meadow          flower fields, farmland, riverside
//   Blossom garden  garden, stream and bridges, lantern festival
//   Autumn woods    deep woods, pumpkin patch, harvest village
//   Snowy village   village, frozen river, pine forest under the aurora
//   Seaside         beach, harbour, rocky cove
static const char* kVariantName[BIOME_COUNT][3] = {
    { "flower fields", "farmland", "riverside" },
    { "garden", "stream", "lantern festival" },
    { "deep woods", "pumpkin patch", "harvest village" },
    { "village", "frozen river", "pine forest" },
    { "beach", "harbour", "rocky cove" },
};

struct BiomeMix { int a, b; float t; int va, vb; };   // between place `a` (variation `va`) and place `b` (variation `vb`)

static int   gFirstBiome = 0;        // where the current song's journey starts
static float gBiomeSpan = 150.0f;    // world units spent in each place during a song
static int   gVariantSeed = 0;       // which variation the first stop uses
static int   gForceVariant = -1;     // screenshot option

static BiomeMix BiomeAt(float x)
{
    const bool song = PathFollowsSong();
    const float span = song ? gBiomeSpan : 60.0f;
    const float f = std::max(0.0f, x) / span;
    const int stop = (int)floorf(f), first = song ? gFirstBiome : 0, seed = song ? gVariantSeed : 0;
    const float edge = 9.0f / span;   // the last nine units blend into the next stop
    float t = std::clamp((f - (float)stop - (1.0f - edge)) / edge, 0.0f, 1.0f);
    t = t * t * (3.0f - 2.0f * t);

    BiomeMix m;
    if (song && gCfg.scenery > 0) {
        m.a = m.b = gCfg.scenery - 1;   // one place for the whole song; its variations still take turns
    } else {
        // Stepping by two visits all five places before repeating, and never puts similar ones side by side.
        m.a = (first + stop * 2) % BIOME_COUNT;
        m.b = (first + (stop + 1) * 2) % BIOME_COUNT;
    }
    m.va = gForceVariant >= 0 ? gForceVariant : (seed + stop) % 3;
    m.vb = gForceVariant >= 0 ? gForceVariant : (seed + stop + 1) % 3;
    m.t = t;
    return m;
}

static float BiomeWeight(const BiomeMix& m, int biome) { return (m.a == biome ? 1.0f - m.t : 0.0f) + (m.b == biome ? m.t : 0.0f); }
static int   BiomeNear(float x) { const BiomeMix m = BiomeAt(x); return m.t < 0.5f ? m.a : m.b; }
static int   VariantNear(float x) { const BiomeMix m = BiomeAt(x); return m.t < 0.5f ? m.va : m.vb; }

// Three variations put a river (or a frozen one) behind the path.
static bool  HasRiver(int biome, int variant)
{
    return (biome == BIOME_MEADOW && variant == 2) || (biome == BIOME_BLOSSOM && variant == 1) || (biome == BIOME_SNOW && variant == 1);
}
static float RiverWeight(const BiomeMix& m) { return (HasRiver(m.a, m.va) ? 1.0f - m.t : 0.0f) + (HasRiver(m.b, m.vb) ? m.t : 0.0f); }
static float RiverLevel(float x);

// Ground colours at world x: the place decides the hue, the time of day the shading.
static void GroundColours(float x, Vector3& grass, Vector3& path)
{
    const BiomeMix m = BiomeAt(x);
    const BiomeLook a = LookOf(m.a), b = LookOf(m.b);
    grass = Vector3Multiply(Mix(a.grass, b.grass, m.t), gShadeGrass);
    path = Vector3Multiply(Mix(a.path, b.path, m.t), gShadePath);
}

// ---------------------------------------------------------------------------
// Progress and things to unlock
// ---------------------------------------------------------------------------
// Playing unlocks cats and things for them to wear. Nothing is bought: every
// item has one plain condition, checked against a handful of lifetime numbers,
// so only those numbers are saved.

struct Progress {
    int songs = 0, perfects = 0, bestStreak = 0, bestRank = 9, fullCombos = 0;
    int places = 0;          // bit per place walked through
    float minutes = 0.0f;
    unsigned known = 0;      // bit per item already seen unlocked in the wardrobe
};
static Progress gProgress;
static bool gUnlockAll = false;   // screenshot option

enum Slot { SLOT_CAT, SLOT_HAT, SLOT_NECK, SLOT_BACK, SLOT_COUNT };
enum Need { NEED_NONE, NEED_SONGS, NEED_PERFECTS, NEED_STREAK, NEED_RANK, NEED_PLACE, NEED_MINUTES, NEED_FULLCOMBO };

struct Item { int slot; const char* name; int need, amount; };

// Within a slot, the order here is the value stored in the settings. Cats match kCats.
static const Item kItems[] = {
    { SLOT_CAT, "Mango", NEED_NONE, 0 },
    { SLOT_CAT, "Domino", NEED_NONE, 0 },
    { SLOT_CAT, "Mochi", NEED_NONE, 0 },
    { SLOT_CAT, "Luna", NEED_SONGS, 3 },
    { SLOT_CAT, "Pebble", NEED_PERFECTS, 300 },
    { SLOT_CAT, "Cloud", NEED_STREAK, 60 },
    { SLOT_CAT, "Midnight", NEED_SONGS, 6 },

    { SLOT_HAT, "No hat", NEED_NONE, 0 },
    { SLOT_HAT, "Bow", NEED_NONE, 0 },
    { SLOT_HAT, "Flower crown", NEED_PERFECTS, 50 },
    { SLOT_HAT, "Party hat", NEED_STREAK, 25 },
    { SLOT_HAT, "Straw hat", NEED_PLACE, BIOME_SEASIDE },
    { SLOT_HAT, "Beanie", NEED_PLACE, BIOME_SNOW },
    { SLOT_HAT, "Witch hat", NEED_PLACE, BIOME_AUTUMN },
    { SLOT_HAT, "Top hat", NEED_RANK, 1 },
    { SLOT_HAT, "Crown", NEED_RANK, 0 },
    { SLOT_HAT, "Headphones", NEED_SONGS, 10 },
    { SLOT_HAT, "Halo", NEED_FULLCOMBO, 1 },

    { SLOT_NECK, "Nothing", NEED_NONE, 0 },
    { SLOT_NECK, "Bell collar", NEED_NONE, 0 },
    { SLOT_NECK, "Bow tie", NEED_SONGS, 1 },
    { SLOT_NECK, "Scarf", NEED_PERFECTS, 150 },
    { SLOT_NECK, "Bandana", NEED_STREAK, 40 },
    { SLOT_NECK, "Flower garland", NEED_PLACE, BIOME_BLOSSOM },

    { SLOT_BACK, "Nothing", NEED_NONE, 0 },
    { SLOT_BACK, "Backpack", NEED_MINUTES, 10 },
    { SLOT_BACK, "Balloon", NEED_SONGS, 5 },
    { SLOT_BACK, "Wings", NEED_STREAK, 100 },
};
const int kItemCount = (int)(sizeof(kItems) / sizeof(kItems[0]));

static int RankOf(float accuracy) { return accuracy >= 0.95f ? 0 : accuracy >= 0.88f ? 1 : accuracy >= 0.78f ? 2 : accuracy >= 0.65f ? 3 : 4; }

static int SlotSize(int slot)
{
    int n = 0;
    for (const Item& it : kItems) n += it.slot == slot;
    return n;
}

// Index into kItems of the nth item of a slot.
static int ItemIndex(int slot, int n)
{
    for (int i = 0; i < kItemCount; i++)
        if (kItems[i].slot == slot && n-- == 0) return i;
    return 0;
}

static bool Unlocked(const Item& it)
{
    if (gUnlockAll) return true;
    const Progress& g = gProgress;
    switch (it.need) {
        case NEED_SONGS:     return g.songs >= it.amount;
        case NEED_PERFECTS:  return g.perfects >= it.amount;
        case NEED_STREAK:    return g.bestStreak >= it.amount;
        case NEED_RANK:      return g.bestRank <= it.amount;
        case NEED_PLACE:     return (g.places >> it.amount) & 1;
        case NEED_MINUTES:   return g.minutes >= (float)it.amount;
        case NEED_FULLCOMBO: return g.fullCombos >= it.amount;
        default:             return true;
    }
}

// What it takes, with how far along the player is.
static const char* Requirement(const Item& it)
{
    const Progress& g = gProgress;
    switch (it.need) {
        case NEED_SONGS:     return TextFormat(it.amount == 1 ? "finish a song" : "finish %d songs  %d/%d", it.amount, std::min(g.songs, it.amount), it.amount);
        case NEED_PERFECTS:  return TextFormat("%d purrfect hits  %d/%d", it.amount, std::min(g.perfects, it.amount), it.amount);
        case NEED_STREAK:    return TextFormat("streak of %d  best %d", it.amount, g.bestStreak);
        case NEED_RANK:      return it.amount == 0 ? "earn an S rank" : "earn an A rank";
        case NEED_PLACE:     return TextFormat("visit the %s", kBiomeName[it.amount]);
        case NEED_MINUTES:   return TextFormat("walk %d minutes  %d/%d", it.amount, std::min((int)g.minutes, it.amount), it.amount);
        case NEED_FULLCOMBO: return "a song with no misses";
        default:             return "";
    }
}

static int UnlockedCount()
{
    int n = 0;
    for (const Item& it : kItems) n += Unlocked(it);
    return n;
}

static std::string ProgressPath() { return std::string(GetApplicationDirectory()) + "oniria.save"; }

static void LoadProgress()
{
    FILE* f = fopen(ProgressPath().c_str(), "r");
    if (!f) return;
    char key[32];
    double v;
    while (fscanf(f, "%31[^=]=%lf\n", key, &v) == 2) {
        if (!strcmp(key, "songs")) gProgress.songs = (int)v;
        else if (!strcmp(key, "perfects")) gProgress.perfects = (int)v;
        else if (!strcmp(key, "streak")) gProgress.bestStreak = (int)v;
        else if (!strcmp(key, "rank")) gProgress.bestRank = (int)v;
        else if (!strcmp(key, "full")) gProgress.fullCombos = (int)v;
        else if (!strcmp(key, "places")) gProgress.places = (int)v;
        else if (!strcmp(key, "minutes")) gProgress.minutes = (float)v;
        else if (!strcmp(key, "known")) gProgress.known = (unsigned)v;
    }
    fclose(f);
}

static void SaveProgress()
{
    if (gSim) return;
    FILE* f = fopen(ProgressPath().c_str(), "w");
    if (!f) return;
    fprintf(f, "songs=%d\nperfects=%d\nstreak=%d\nrank=%d\nfull=%d\nplaces=%d\nminutes=%.2f\nknown=%u\n",
            gProgress.songs, gProgress.perfects, gProgress.bestStreak, gProgress.bestRank, gProgress.fullCombos,
            gProgress.places, gProgress.minutes, gProgress.known);
    fclose(f);
}

static std::vector<int> gNewUnlocks;   // items earned by the song that just ended
static bool gSimCounts = false;        // screenshot option: let a simulated run earn progress

// Anything equipped that is not unlocked falls back to the default for its slot.
static void CheckEquipped()
{
    int* field[SLOT_COUNT] = { &gCfg.cat, &gCfg.hat, &gCfg.neck, &gCfg.back };
    static const int kDefault[SLOT_COUNT] = { 0, 0, 1, 0 };
    for (int slot = 0; slot < SLOT_COUNT; slot++)
        if (*field[slot] < 0 || *field[slot] >= SlotSize(slot) || !Unlocked(kItems[ItemIndex(slot, *field[slot])])) *field[slot] = kDefault[slot];
}

// Height of the path at world x. During a song the hills are the song's
// loudness: quiet passages are valleys, loud ones are hilltops.
static float PathHeight(float x)
{
    if (!PathFollowsSong() || gPathHeights.empty())
        return 0.55f + 0.35f * sinf(x * 0.11f) + 0.15f * sinf(x * 0.27f + 1.0f);
    const float f = x / gScroll / 0.5f;
    const int i = (int)floorf(f), last = (int)gPathHeights.size() - 1;
    const float u = f - (float)i;
    const float p0 = gPathHeights[std::clamp(i - 1, 0, last)], p1 = gPathHeights[std::clamp(i, 0, last)],
                p2 = gPathHeights[std::clamp(i + 1, 0, last)], p3 = gPathHeights[std::clamp(i + 2, 0, last)];
    return 0.5f * (2 * p1 + (p2 - p0) * u + (2 * p0 - 5 * p1 + 4 * p2 - p3) * u * u + (3 * p1 - p0 - 3 * p2 + p3) * u * u * u);
}

static float RiverLevel(float x) { return PathHeight(x) - 0.4f; }

static float GroundHeight(float x, float z)
{
    const float path = PathHeight(x);
    const float away = std::clamp((fabsf(z) - 1.0f) / 3.0f, 0.0f, 1.0f);
    const float bumps = 0.32f * sinf(x * 0.31f + z * 0.7f) + 0.22f * sinf(x * 0.13f - z * 0.45f);
    const float slope = z < 0.0f ? std::max(0.0f, -z - 3.0f) * 0.11f : -std::max(0.0f, z - 1.6f) * 0.16f;
    float h = path + away * away * (3 - 2 * away) * bumps + slope;

    if (z < 0.0f) {
        const BiomeMix m = BiomeAt(x);
        // At the seaside the land behind the path runs down a beach to a flat sea.
        const float sea = BiomeWeight(m, BIOME_SEASIDE);
        if (sea > 0.0f) {
            const float k = std::clamp((-z - 1.5f) / 6.0f, 0.0f, 1.0f), shore = k * k * (3 - 2 * k);
            const float beach = path + (kSeaLevel - path) * shore + 0.12f * bumps * (1.0f - shore);
            h += (beach - h) * sea;
        }
        // A river cuts a flat channel between z = -5 and z = -8, with banks on both sides.
        const float river = RiverWeight(m);
        if (river > 0.0f) {
            float in = std::clamp((-z - 2.6f) / 2.0f, 0.0f, 1.0f), out = std::clamp((-z - 8.4f) / 2.6f, 0.0f, 1.0f);
            in = in * in * (3 - 2 * in);
            out = out * out * (3 - 2 * out);
            h += (path - 0.4f - h) * river * in * (1.0f - out);
        }
    }
    return h;
}

static void BuildPathHeights()
{
    const int count = (int)(gSong.duration / 0.5) + 3;
    std::vector<float> level((size_t)count, 0.0f);
    const int n = (int)gSong.low.size();
    if (n == 0) { gPathHeights.assign((size_t)count, 0.5f); return; }
    const int half = (int)(2.0 / gSong.hop);
    for (int k = 0; k < count; k++) {
        const int centre = (int)((k * 0.5 - gSong.envOffset) / gSong.hop);
        const int lo = std::clamp(centre - half, 0, n - 1), hi = std::clamp(centre + half, 0, n - 1);
        float sum = 0.0f;
        for (int i = lo; i <= hi; i += 4) sum += gSong.low[i] + gSong.mid[i] + gSong.high[i];
        level[k] = sum / (float)((hi - lo) / 4 + 1);
    }
    const float lo = *std::min_element(level.begin(), level.end()), hi = *std::max_element(level.begin(), level.end());
    gPathHeights.resize((size_t)count);
    for (int k = 0; k < count; k++) {
        const float a = level[std::max(0, k - 1)], b = level[k], c = level[std::min(count - 1, k + 1)];
        gPathHeights[k] = 0.2f + 1.7f * ((a + 2 * b + c) * 0.25f - lo) / std::max(1e-4f, hi - lo);
    }
}

// ---------------------------------------------------------------------------
// Clock
// ---------------------------------------------------------------------------

// Keeps the smooth game clock locked to the audio thread without inheriting its jitter.
static void SyncClock()
{
    double audio;
    if (!AudioPosition(audio)) return;
    const double drift = audio - (NowSeconds() - gClockBase);
    if (std::fabs(drift) > 0.05) gClockBase -= drift;
    else gClockBase -= drift * 0.02;
}

static double SongTimeAt(double stamp) { return gSim ? gSimTime : stamp - gClockBase - gLatency; }
static double SongTime() { return SongTimeAt(NowSeconds()); }

static void SeekAndPlay(double from)
{
    if (gSim) { gSimTime = from; return; }
    std::lock_guard<std::mutex> lock(gPlayer.mutex);
    const int64_t pos = (int64_t)std::llround(from * gPlayer.sampleRate);
    gPlayer.pos.store(pos);
    StampLocked(pos);
    gClockBase = NowSeconds() - from;
    gPlayer.playing.store(true);
}

static void ResumePlayback()
{
    if (gSim) return;
    std::lock_guard<std::mutex> lock(gPlayer.mutex);
    const int64_t pos = gPlayer.pos.load();
    StampLocked(pos);
    gClockBase = NowSeconds() - (double)pos / gPlayer.sampleRate;
    gPlayer.playing.store(true);
}

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

static void LoadWorker(std::string path, bool demo)
{
    gJob.stage.store(1);
    gJob.error.clear();
    gJob.pcm.Free();
    if (demo) {
        std::vector<int16_t> demoPcm;
        SynthDemoTrack(demoPcm, gJob.sampleRate);
        gJob.pcm.frames = demoPcm.size() / 2;
        gJob.pcm.data = (int16_t*)MemAlloc((unsigned int)(demoPcm.size() * sizeof(int16_t)));
        memcpy(gJob.pcm.data, demoPcm.data(), demoPcm.size() * sizeof(int16_t));
    } else {
        std::vector<uint8_t> bytes;
        std::string ext = GetFileExtension(path.c_str()) ? GetFileExtension(path.c_str()) : "";
        for (char& ch : ext) ch = (char)tolower((unsigned char)ch);
        if (!PlatformReadFile(path, bytes)) {
            gJob.error = "Could not read that file.";
        } else {
            Wave wave = LoadWaveFromMemory(ext.c_str(), bytes.data(), (int)bytes.size());
            bytes = std::vector<uint8_t>();
            if (!IsWaveValid(wave)) {
                gJob.error = "Could not decode that file. Try MP3, OGG or WAV.";
            } else {
                WaveFormat(&wave, wave.sampleRate, 16, 2);
                gJob.pcm.data = (int16_t*)wave.data;   // take the decoder's buffer instead of copying it
                gJob.pcm.frames = wave.frameCount;
                gJob.sampleRate = wave.sampleRate;
            }
        }
    }
    if (gJob.error.empty()) {
        gJob.stage.store(2);
        gJob.song = SongAnalysis();
        if (!AnalyzeSong(gJob.pcm.data, gJob.pcm.frames, 2, gJob.sampleRate, gJob.song))
            gJob.error = "That track is too short to play.";
        else if (gJob.song.charts[DIFF_NORMAL].time.size() < 8)
            gJob.error = "Could not find enough rhythm in that track.";
    }
    gJob.ok = gJob.error.empty();
    gJob.stage.store(3);
}

static void BeginLoad(const std::string& path, bool demo)
{
    if (gState == ST_LOADING) return;
    gPlayer.playing.store(false);
    gError.clear();
    gJob.name = demo ? "Demo track" : GetFileNameWithoutExt(path.c_str());
    gJob.path = demo ? kDemoKey : path;
    gState = ST_LOADING;
    if (gSim) { LoadWorker(path, demo); return; }
    gJob.thread = std::thread(LoadWorker, path, demo);
}

static void FinishLoad()
{
    if (gJob.thread.joinable()) gJob.thread.join();
    gJob.stage.store(0);
    if (!gJob.ok) {
        gError = gJob.error;
        gJob.pcm.Free();
        gState = ST_MENU;
        return;
    }
    {
        Pcm previous;
        {
            std::lock_guard<std::mutex> lock(gPlayer.mutex);
            previous = gPlayer.pcm;
            gPlayer.pcm = gJob.pcm;
            gPlayer.frames = (int64_t)gPlayer.pcm.frames;
        }
        gJob.pcm = Pcm();
        previous.Free();   // the song that was loaded before
    }
    if (!gSim) {
        if (gPlayer.hasStream && gPlayer.sampleRate != gJob.sampleRate) {
            UnloadAudioStream(gPlayer.stream);
            gPlayer.hasStream = false;
        }
        gPlayer.sampleRate = gJob.sampleRate;
        if (!gPlayer.hasStream) {
            gPlayer.stream = LoadAudioStream((unsigned)gPlayer.sampleRate.load(), 32, 2);
            SetAudioStreamCallback(gPlayer.stream, FeedAudio);
            PlayAudioStream(gPlayer.stream);
            gPlayer.hasStream = true;
        }
    }
    gPlayer.sampleRate = gJob.sampleRate;
    gSong = std::move(gJob.song);
    gSongName = gJob.name;
    gSongPath = gJob.path;
    if (!gSim) RememberSong(gSongPath, gSongName, (float)gSong.duration);
    gMenuSel = 0;
    gMenuScroll = 0;
    BuildPathHeights();
    gState = ST_READY;
}

// ---------------------------------------------------------------------------
// Gameplay
// ---------------------------------------------------------------------------

static void StartGame()
{
    gChart = &gSong.charts[gCfg.diff];
    const size_t n = gChart->time.size();
    gNoteState.assign(n, 0);
    gNoteWhen.assign(n, 0.0f);
    gNoteAction.resize(n);
    for (size_t i = 0; i < n; i++) gNoteAction[i] = (uint8_t)kLaneAction[gCfg.diff][gChart->lane[i]];
    gHead = gTail = 0;
    gScore = gCombo = gMaxCombo = 0;
    memset(gCount, 0, sizeof(gCount));
    memset(gHeld, 0, sizeof(gHeld));
    for (Spark& s : gSparks) s.life = 0.0f;
    for (Ring& r : gRings) r.age = 9.0f;
    for (Bloom& b : gBlooms) b.x = -1e9f;
    gStreak = gJoy = gJoyKick = 0.0f;
    gMilestoneAge = 9.0f;
    gBloomMark = (float)(-kLeadIn) * kScrollBase * gCfg.speed;
    for (Impact& hit : gImpacts) hit.age = 9.0f;
    gRig = CameraRig();
    gEdgePower = 0.0f;
    gNewBest = false;

    // The journey: each song starts somewhere of its own and moves on every half minute or so.
    uint32_t nameHash = 2166136261u;
    for (unsigned char c : gSongName) nameHash = (nameHash ^ c) * 16777619u;
    gFirstBiome = (int)(nameHash % BIOME_COUNT);
    gVariantSeed = (int)((nameHash >> 8) % 3);
    const int places = std::clamp((int)std::lround(gSong.duration / 30.0), 1, (int)BIOME_COUNT);
    gBiomeSpan = (float)(gSong.duration / places) * kScrollBase * gCfg.speed;
    gJudgeKind = -1;
    gCatAction = -1;
    gScroll = kScrollBase * gCfg.speed;
    gLatency = kBaseLatency + gCfg.offsetMs / 1000.0;
    gScene.envIndex = -1;
    SeekAndPlay(-kLeadIn);
    gState = ST_PLAYING;
}

// Where a note's object stands in the world.
static Vector3 NotePosition(int i)
{
    static const float kHeight[ACT_COUNT] = { 0.0f, 0.25f, 1.5f, 1.62f };
    const int action = gNoteAction[i];
    const float x = (float)gChart->time[i] * gScroll + kReach + (action == ACT_HOP ? 0.15f : 0.0f);
    return Vector3{ x, PathHeight(x) + kHeight[action], 0.0f };
}

static void AddSpark(Vector3 pos, Vector3 vel, Vector3 col, float life, int kind, float size, float gravity)
{
    gSparks[gNextSpark++ % kSparkCount] = Spark{ pos, vel, col, life, life, size, gravity, kind };
}

static void AddBloom(float x, float z)
{
    // Flowers in front of the path stay small so they never cover what is on it.
    const float size = z > 0.0f ? 0.55f + Rand01() * 0.3f : 1.1f + Rand01() * 0.9f;
    gBlooms[gNextBloom++ % kBloomCount] = Bloom{ x, z, gScene.time, (int)(Rand01() * 4.0f) & 3, size };
}

static void Burst(Vector3 at, Vector3 col, int count)
{
    for (int i = 0; i < count; i++) {
        const float a = Rand01() * 2 * kPi, speed = 1.2f + Rand01() * 2.2f;
        AddSpark(at, Vector3{ cosf(a) * speed + 1.5f, fabsf(sinf(a)) * speed + 1.0f, (Rand01() - 0.5f) * 1.5f },
                 Mix(col, Vector3{ 1, 1, 1 }, Rand01() * 0.5f), 0.35f + Rand01() * 0.35f, SPARK_SHARD, 0.1f, 7.0f);
    }
}

// Stars that pop outwards and hearts that float up, around `at`.
static void Celebrate(Vector3 at, int stars, int hearts)
{
    static const Vector3 kStar[3] = { { 1.0f, 0.93f, 0.55f }, { 1.0f, 0.8f, 0.86f }, { 0.8f, 0.93f, 1.0f } };
    for (int i = 0; i < stars; i++) {
        const float a = Rand01() * 2 * kPi, speed = 1.4f + Rand01() * 2.4f;
        AddSpark(at, Vector3{ cosf(a) * speed + 1.2f, fabsf(sinf(a)) * speed + 0.8f, (Rand01() - 0.5f) * 1.2f },
                 kStar[i % 3], 0.5f + Rand01() * 0.35f, SPARK_STAR, 0.13f + Rand01() * 0.1f, 4.5f);
    }
    for (int i = 0; i < hearts; i++)
        AddSpark(Vector3{ at.x + (Rand01() - 0.5f) * 0.7f, at.y + Rand01() * 0.3f, at.z + 0.2f },
                 Vector3{ (Rand01() - 0.5f) * 1.2f, 1.1f + Rand01() * 0.9f, 0 }, Vector3{ 1.0f, 0.52f, 0.64f },
                 0.9f + Rand01() * 0.5f, SPARK_HEART, 0.12f + Rand01() * 0.06f, -0.5f);
}

static void AddRing(Vector3 pos, Vector3 col, float size, float life, bool upright, float delay = 0.0f)
{
    gRings[gNextRing++ % kRingCount] = Ring{ pos, -delay, life, size, col, upright };
}

static void Judge(int note, double error, double t)
{
    const double e = std::fabs(error);
    const int j = e <= kPerfect ? J_PERFECT : e <= kGreat ? J_GREAT : J_GOOD;
    const int action = gNoteAction[note];
    gNoteState[note] = (uint8_t)(j + 1);
    gNoteWhen[note] = (float)t;
    gCount[j]++;
    gCombo++;
    gMaxCombo = std::max(gMaxCombo, gCombo);
    gScore += (int)std::lround(kPoints[j] * (1.0 + std::min(gCombo, 100) * 0.02));
    gStreak += j == J_PERFECT ? 1.0f : j == J_GREAT ? 0.6f : 0.3f;
    gJudgeKind = j;
    gJudgeAge = 0.0f;
    gComboPop = 1.0f;

    const float power = j == J_PERFECT ? 1.0f : j == J_GREAT ? 0.65f : 0.4f;
    const Vector3 colour = kActionColor[action], white = { 1, 1, 1 };
    Vector3 at = NotePosition(note);
    const float ground = PathHeight(at.x);
    at.y += 0.2f;
    Burst(at, colour, j == J_PERFECT ? 8 : j == J_GREAT ? 6 : 4);

    // Each action has its own instrument, and the note climbs with the streak.
    const float semitones = SfxStreakSemitones(gCombo);
    PlaySfx(SFX_HOP + action, semitones, j == J_PERFECT ? 1.0f : j == J_GREAT ? 0.75f : 0.5f);

    // The camera answers each action differently: it bounces with a hop,
    // lunges with a bat, tips with a swat and sways with a bell.
    gRig.flip = -gRig.flip;
    switch (action) {
        case ACT_HOP:  KickCamera(0.0f, 2.4f * power, 0.0f, -7.0f * power); break;
        case ACT_BAT:  KickCamera(2.8f * power, 0.0f, 0.0f, 17.0f * power); break;
        case ACT_SWAT: KickCamera(0.0f, 1.6f * power, 0.3f * power * gRig.flip, 9.0f * power); break;
        case ACT_RING: KickCamera(0.0f, 0.0f, 0.5f * power * gRig.flip, 11.0f * power); break;
    }

    // On screen: a burst of streaks, a ring bursting towards the camera, glowing edges.
    gImpacts[gNextImpact++ % kImpactCount] = Impact{ at, 0.0f, action, j };
    gEdgeColour = colour;
    gEdgePower = std::max(gEdgePower, power);
    AddRing(at, Mix(colour, white, 0.4f), 0.45f + 0.5f * power, 0.28f, true);
    switch (action) {
        case ACT_HOP:    // dust where the cat took off
            for (int i = 0; i < 5; i++)
                AddSpark(Vector3{ at.x - 0.9f + Rand01() * 0.5f, ground + 0.1f, (Rand01() - 0.5f) * 0.8f },
                         Vector3{ -0.4f - Rand01() * 0.8f, 0.3f + Rand01() * 0.5f, 0 }, Vector3{ 0.93f, 0.86f, 0.74f }, 0.5f, SPARK_PUFF, 0.16f + Rand01() * 0.1f, -0.3f);
            break;
        case ACT_BAT:    // a puff of wool
            for (int i = 0; i < 4; i++)
                AddSpark(at, Vector3{ 0.5f + Rand01() * 1.5f, (Rand01() - 0.3f) * 1.2f, 0 }, Mix(colour, white, 0.6f), 0.4f, SPARK_PUFF, 0.12f + Rand01() * 0.08f, 0.0f);
            break;
        case ACT_SWAT:   // sparkling dust off the wings
            Celebrate(at, 3, 0);
            break;
        case ACT_RING:   // the sound itself, as rings
            AddRing(at, colour, 1.0f, 0.5f, true, 0.06f);
            AddRing(at, colour, 1.5f, 0.6f, true, 0.14f);
            break;
    }

    if (j == J_PERFECT) {
        // A perfect hit always gets a twinkle, stars, rings of light on the path and two new flowers.
        PlaySfx(SFX_TWINKLE, semitones);
        gJoyKick = 1.0f;
        Celebrate(at, 5 + (int)(gJoy * 4.0f), gJoy > 0.3f ? 1 + (int)(gJoy * 2.0f) : 0);
        AddRing(Vector3{ at.x, ground + 0.04f, 0 }, Vector3{ 1.0f, 0.96f, 0.8f }, 1.8f, 0.45f, false);
        AddRing(Vector3{ at.x, ground + 0.04f, 0 }, Vector3{ 1.0f, 0.96f, 0.8f }, 2.6f, 0.6f, false, 0.08f);
        AddBloom(at.x + (Rand01() - 0.5f) * 0.8f, -1.25f - Rand01() * 0.7f);
        AddBloom(at.x + (Rand01() - 0.5f) * 0.8f, 1.8f + Rand01() * 0.6f);
    }

    if (gCombo == 10 || gCombo == 25 || gCombo == 50 || (gCombo >= 100 && gCombo % 50 == 0)) {
        gMilestone = gCombo;
        gMilestoneAge = 0.0f;
        PlaySfx(SFX_MILESTONE);
        KickCamera(0.0f, 0.0f, 0.0f, 42.0f);   // lean in for the big moments
        Celebrate(Vector3{ gScene.worldX + 0.4f, PathHeight(gScene.worldX) + 1.4f, 0.2f }, 10, 8);
    }
}

static void Miss(int note, double t)
{
    gNoteState[note] = 4;
    gNoteWhen[note] = (float)t;
    gCount[J_MISS]++;
    gCombo = 0;
    gStreak = 0.0f;
    gJudgeKind = J_MISS;
    gJudgeAge = 0.0f;
    CatDo(ACT_STUMBLE);
    PlaySfx(SFX_MISS);
    gRig.flip = -gRig.flip;
    KickCamera(1.6f * gRig.flip, 0.0f, 0.35f * gRig.flip, -4.0f);   // a little wobble
    gEdgeColour = Vector3{ 0.6f, 0.62f, 0.75f };
    gEdgePower = std::max(gEdgePower, 0.6f);
}

// The player asked for `action` at song time `t`: the cat does it, and the
// earliest matching object within reach is judged.
static void Press(int action, double t)
{
    gScene.glow[action] = 1.0f;
    CatDo(action);
    const int n = (int)gChart->time.size();
    for (int i = gHead; i < n; i++) {
        const double d = gChart->time[i] - t;
        if (d > kGood) break;
        if (gNoteState[i] || gNoteAction[i] != action || d < -kGood) continue;
        Judge(i, d, t);
        return;
    }
}

static float Accuracy()
{
    const int judged = gCount[0] + gCount[1] + gCount[2] + gCount[3];
    if (judged == 0) return 1.0f;
    return (gCount[0] * kAccWeight[0] + gCount[1] * kAccWeight[1] + gCount[2] * kAccWeight[2]) / (float)judged;
}

static void FinishGame()
{
    gPlayer.playing.store(false);
    for (size_t i = 0; i < gNoteState.size(); i++)
        if (!gNoteState[i]) { gNoteState[i] = 4; gCount[J_MISS]++; }
    gEndJoy = std::clamp((Accuracy() - 0.6f) / 0.35f, 0.0f, 1.0f) * 0.85f;
    gNewBest = false;
    gNewUnlocks.clear();
    if ((!gCfg.watch && !gSim) || gSimCounts) {
        // Lifetime numbers, and whatever they have just unlocked.
        std::vector<bool> before;
        for (const Item& it : kItems) before.push_back(Unlocked(it));
        const int notes = gCount[0] + gCount[1] + gCount[2] + gCount[3];
        gProgress.songs++;
        gProgress.perfects += gCount[J_PERFECT];
        gProgress.bestStreak = std::max(gProgress.bestStreak, gMaxCombo);
        gProgress.bestRank = std::min(gProgress.bestRank, RankOf(Accuracy()));
        if (notes > 0 && gCount[J_MISS] == 0) gProgress.fullCombos++;
        gProgress.minutes += (float)(gSong.duration / 60.0);
        for (int i = 0; i < kItemCount; i++)
            if (!before[i] && Unlocked(kItems[i])) gNewUnlocks.push_back(i);
        SaveProgress();
    }
    if (!gCfg.watch && !gSim) {
        if (SongEntry* e = FindSong(gSongPath)) {
            if (Accuracy() > e->best[gCfg.diff]) {
                gNewBest = e->best[gCfg.diff] >= 0.0f;
                e->best[gCfg.diff] = Accuracy();
                e->score[gCfg.diff] = gScore;
                SaveLibrary();
            }
        }
    }
    for (Bloom& b : gBlooms) b.x = -1e9f;
    gState = ST_RESULTS;
    SaveSettings();
}

static void UpdatePlaying(double t)
{
    // Timestamped key events from the raw input thread; per-frame polling until it proves itself.
    LaneKeyEvent events[32];
    const int n = PlatformPollKeys(events, 32);
    if (n > 0) gRawKeys = true;
    const int actions = kActionCount[gCfg.diff];
    if (!gCfg.watch) {
        for (int i = 0; i < n; i++) {
            if (events[i].lane >= actions) continue;
            gHeld[events[i].lane] = events[i].down;
            if (events[i].down) Press(events[i].lane, SongTimeAt(events[i].time));
        }
        if (!gRawKeys) {
            for (int a = 0; a < actions; a++) {
                if (Key(kKeyA[a]) || Key(kKeyB[a])) Press(a, t);
                gHeld[a] = IsKeyDown(kKeyA[a]) || IsKeyDown(kKeyB[a]);
            }
        }
    }

    const int total = (int)gChart->time.size();
    if (gCfg.watch) {
        for (int i = gHead; i < total && gChart->time[i] <= t; i++)
            if (!gNoteState[i]) { gScene.glow[gNoteAction[i]] = 1.0f; CatDo(gNoteAction[i]); Judge(i, 0.0, t); }
    }
    while (gHead < total && (gNoteState[gHead] || gChart->time[gHead] < t - kGood)) {
        if (!gNoteState[gHead]) Miss(gHead, t);
        gHead++;
    }
    while (gTail < total && gChart->time[gTail] < t - 3.5) gTail++;
    if (!gCfg.watch || gSimCounts) gProgress.places |= 1 << BiomeNear(gScene.worldX);   // remember where the walk has been
    if (t > gSong.duration + 0.6) FinishGame();
}

// ---------------------------------------------------------------------------
// Scene update
// ---------------------------------------------------------------------------

static void UpdateScene(float dt, double t)
{
    Scene& s = gScene;
    float bass, mid, high, pulse = 0.0f;
    const bool live = gState == ST_PLAYING && !gSong.low.empty();
    if (live) {
        const int count = (int)gSong.low.size();
        int i = (int)std::floor((t - gSong.envOffset) / gSong.hop);
        if (i < 0) {
            bass = mid = high = 0.0f;
            s.envIndex = -1;
        } else {
            i = std::min(i, count - 1);
            bass = gSong.low[i]; mid = gSong.mid[i]; high = gSong.high[i];
            // Frames are longer than envelope samples: take the strongest pulse since last frame.
            for (int k = std::max(s.envIndex + 1, i - 12); k <= i; k++) pulse = std::max(pulse, gSong.pulse[k]);
            s.envIndex = i;
        }
    } else if (gState == ST_PAUSED || gState == ST_COUNTDOWN) {
        bass = mid = high = 0.1f;
    } else {
        // No music: a slow heartbeat keeps the world gently alive.
        bass = 0.4f; mid = 0.4f; high = 0.4f + 0.2f * sinf(s.time * 1.7f);
        pulse = expf(-fmodf(s.time, 0.9f) / 0.9f * 6.0f) * 0.5f;
    }
    auto follow = [dt](float& cur, float target, float up, float down) {
        cur += (target - cur) * std::min(1.0f, dt * (target > cur ? up : down));
    };
    follow(s.bass, bass, 30.0f, 8.0f);
    follow(s.mid, mid, 20.0f, 6.0f);
    follow(s.high, high, 25.0f, 9.0f);
    s.beat = std::max(s.beat * expf(-dt * 6.0f), pulse);

    const bool frozen = gState == ST_PAUSED || gState == ST_COUNTDOWN;
    if (!frozen) s.time += dt;
    if (PathFollowsSong()) {
        s.worldX = (float)t * gScroll;
    } else {
        s.idleX += dt * 2.2f;
        s.worldX = s.idleX;
    }

    // The day goes by over the length of the song and ends at night.
    float day = 0.12f;
    if (PathFollowsSong()) day = (float)std::clamp(t / std::max(1.0, gSong.duration), 0.0, 1.0);
    else if (gState == ST_RESULTS) day = 1.0f;
    s.day += (day - s.day) * std::min(1.0f, dt * 1.5f);
    s.camY += (PathHeight(s.worldX) - s.camY) * std::min(1.0f, dt * 3.0f);

    // Happiness follows the streak: it builds with every hit and drains fast after a miss.
    float joyTarget = gJoy;
    if (gState == ST_PLAYING) joyTarget = 1.0f - expf(-gStreak / 18.0f);
    else if (gState == ST_RESULTS) joyTarget = gEndJoy;
    else if (gState == ST_WARDROBE) joyTarget = 0.5f;   // dressing up is fun
    else if (!PathFollowsSong()) joyTarget = 0.22f;   // menus keep a little sparkle
    gJoy += (joyTarget - gJoy) * std::min(1.0f, dt * (joyTarget > gJoy ? 3.0f : 5.0f));
    gJoyKick *= expf(-dt * 7.0f);
    gEdgePower *= expf(-dt * 6.0f);
    gPreview += ((gState == ST_WARDROBE ? 1.0f : 0.0f) - gPreview) * std::min(1.0f, dt * 5.0f);
    gMilestoneAge += dt;

    for (int a = 0; a < ACT_COUNT; a++) s.glow[a] = gHeld[a] ? 1.0f : s.glow[a] * expf(-dt * 9.0f);
    if (!frozen) {
        gCatActionAge += dt;
        s.wag += dt * (2.6f + 5.0f * gJoy);
        for (Ring& r : gRings) r.age += dt;
        for (Impact& hit : gImpacts) hit.age += dt;
        for (int step = 0; step < 4; step++) {   // camera springs, in small steps so they stay stable
            const float h = dt / 4.0f, stiff = 110.0f, damp = 13.0f;
            gRig.vx += (-stiff * gRig.x - damp * gRig.vx) * h;             gRig.x += gRig.vx * h;
            gRig.vy += (-stiff * gRig.y - damp * gRig.vy) * h;             gRig.y += gRig.vy * h;
            gRig.vroll += (-stiff * gRig.roll - damp * gRig.vroll) * h;    gRig.roll += gRig.vroll * h;
            gRig.vzoom += (-stiff * gRig.zoom - damp * gRig.vzoom) * h;    gRig.zoom += gRig.vzoom * h;
        }
        for (Spark& p : gSparks) {
            if (p.life <= 0.0f) continue;
            p.pos = Vector3Add(p.pos, Vector3Scale(p.vel, dt));
            p.vel.y -= p.gravity * dt;
            p.life -= dt;
        }

        if (gState == ST_PLAYING || gState == ST_RESULTS) {
            const float catX = s.worldX, catY = PathHeight(catX);
            // A happy cat gives off hearts, and at full tilt it trails stars.
            gHeartTimer -= dt;
            if (gJoy > 0.3f && gHeartTimer <= 0.0f) {
                gHeartTimer = 1.2f - 0.85f * gJoy;
                AddSpark(Vector3{ catX + 0.45f, catY + 1.75f, 0.25f }, Vector3{ -0.5f - Rand01() * 0.6f, 0.9f + Rand01() * 0.5f, 0 },
                         Vector3{ 1.0f, 0.52f, 0.64f }, 1.2f, SPARK_HEART, 0.11f + 0.07f * gJoy, -0.4f);
            }
            gStarTimer -= dt;
            if (gJoy > 0.6f && gStarTimer <= 0.0f) {
                gStarTimer = 0.11f;
                AddSpark(Vector3{ catX - 0.9f, catY + 0.8f + Rand01() * 0.6f, (Rand01() - 0.5f) * 0.4f },
                         Vector3{ -1.2f, 0.3f + Rand01() * 0.5f, 0 }, Vector3{ 1.0f, 0.93f, 0.6f }, 0.5f, SPARK_STAR, 0.09f + Rand01() * 0.05f, 1.0f);
            }
            // Flowers pop up ahead of the cat; the happier it is, the more of them.
            if (gState == ST_PLAYING) {
                if (catX - gBloomMark > 20.0f) gBloomMark = catX;
                while (catX - gBloomMark > 0.55f) {
                    gBloomMark += 0.55f;
                    if (Rand01() < gJoy * 1.15f)
                        AddBloom(gBloomMark + 2.5f + Rand01() * 4.5f, Rand01() < 0.62f ? -1.25f - Rand01() * 1.7f : 1.8f + Rand01() * 1.1f);
                }
            }
        }
    }
    gJudgeAge += dt;
    gComboPop *= expf(-dt * 12.0f);
}

// ---------------------------------------------------------------------------
// Scene drawing
// ---------------------------------------------------------------------------

static void Poly2(float cx, float cy, float rx, float ry, int sides, float turn, Color c)
{
    Vector2 prev = { cx + cosf(turn) * rx, cy + sinf(turn) * ry };
    for (int i = 1; i <= sides; i++) {
        const float a = turn + 2 * kPi * i / sides;
        const Vector2 p = { cx + cosf(a) * rx, cy + sinf(a) * ry };
        DrawTriangle(Vector2{ cx, cy }, p, prev, c);
        prev = p;
    }
}

static float Wrap(float v, float m)
{
    v = fmodf(v, m);
    return v < 0.0f ? v + m : v;
}

static void Heart2(float cx, float cy, float size, Color c)
{
    Poly2(cx - size * 0.48f, cy - size * 0.25f, size * 0.55f, size * 0.55f, 8, 0, c);
    Poly2(cx + size * 0.48f, cy - size * 0.25f, size * 0.55f, size * 0.55f, 8, 0, c);
    DrawTriangle(Vector2{ cx - size, cy - size * 0.05f }, Vector2{ cx, cy + size * 1.05f }, Vector2{ cx + size, cy - size * 0.05f }, c);
}

// Rays turning slowly around the sun or moon. They stretch on every perfect hit.
static void Rays(float cx, float cy, float inner, float outer, int count, Vector3 col, float alpha)
{
    if (alpha <= 0.01f) return;
    const Color c = ToColor(col, alpha);
    for (int i = 0; i < count; i++) {
        const float a = gScene.time * 0.3f + i * 2 * kPi / count;
        const float len = outer + 5.0f * sinf(gScene.time * 3.0f + i * 1.7f) + 9.0f * gJoyKick;
        DrawTriangle(Vector2{ cx + cosf(a - 0.09f) * inner, cy + sinf(a - 0.09f) * inner }, Vector2{ cx + cosf(a) * len, cy + sinf(a) * len },
                     Vector2{ cx + cosf(a + 0.09f) * inner, cy + sinf(a + 0.09f) * inner }, c);
    }
}

static void Rainbow(float horizonY, float alpha)
{
    static const Color kBands[6] = { { 255, 150, 160, 255 }, { 255, 196, 140, 255 }, { 255, 236, 150, 255 },
                                     { 170, 232, 170, 255 }, { 150, 206, 255, 255 }, { 196, 170, 250, 255 } };
    const float cx = gVW * 0.40f, cy = horizonY + 34.0f, outer = horizonY * 0.92f + 34.0f;
    const int segments = 26;
    for (int b = 0; b < 6; b++) {
        const float r0 = outer - b * 5.0f, r1 = r0 - 5.0f;
        const Color c = Fade(kBands[b], alpha * 0.55f);
        for (int i = 0; i < segments; i++) {
            const float a0 = kPi + kPi * i / segments, a1 = kPi + kPi * (i + 1) / segments;
            const Vector2 p00 = { cx + cosf(a0) * r0, cy + sinf(a0) * r0 }, p01 = { cx + cosf(a1) * r0, cy + sinf(a1) * r0 };
            const Vector2 p10 = { cx + cosf(a0) * r1, cy + sinf(a0) * r1 }, p11 = { cx + cosf(a1) * r1, cy + sinf(a1) * r1 };
            DrawTriangle(p00, p10, p01, c);
            DrawTriangle(p01, p10, p11, c);
        }
    }
}

// Soft lights that drift up through the sky, and petals on the wind. Both
// thicken as the streak grows. `front` draws the few petals that pass in
// front of the world.
static void JoyParticles(float horizonY, bool front)
{
    static const Color kLights[4] = { { 255, 236, 170, 255 }, { 255, 190, 210, 255 }, { 190, 226, 255, 255 }, { 214, 196, 255, 255 } };
    static const Color kPetals[3] = { { 255, 190, 205, 255 }, { 255, 240, 236, 255 }, { 255, 214, 170, 255 } };
    const float time = gScene.time, joy = gJoy;
    if (joy < 0.03f) return;

    if (!front) {
        const float span = horizonY * 1.3f, lights = joy * 18.0f;
        for (int i = 0; i < (int)lights + 1; i++) {
            const float x = Wrap(Hash01(i, 101) * gVW + sinf(time * 0.4f + i) * 12.0f, (float)gVW);
            const float y = span - Wrap(Hash01(i, 102) * span + time * (5.0f + Hash01(i, 103) * 8.0f), span);
            const float r = 3.0f + Hash01(i, 104) * 5.0f;
            const float a = (0.13f + 0.08f * sinf(time * 2.0f + i)) * std::clamp(lights - i, 0.0f, 1.0f);
            Poly2(x, y, r, r, 8, 0, Fade(kLights[i & 3], a));
            Poly2(x, y, r * 0.45f, r * 0.45f, 6, 0, Fade(kLights[i & 3], a * 1.5f));
        }
    }
    const float petals = joy * (front ? 10.0f : 30.0f);
    const int seed = front ? 140 : 110;
    for (int i = 0; i < (int)petals + 1; i++) {
        const float x = Wrap(Hash01(i, seed + 1) * gVW - time * (14.0f + Hash01(i, seed + 2) * 12.0f) + sinf(time * 1.3f + i) * 6.0f, (float)gVW);
        const float y = Wrap(Hash01(i, seed + 3) * gVH + time * (16.0f + Hash01(i, seed + 4) * 14.0f), gVH * (front ? 0.92f : 0.8f));
        Poly2(x, y, 2.4f, 1.3f, 4, time * 3.0f + i, Fade(kPetals[i % 3], (front ? 0.75f : 0.9f) * std::clamp(petals - i, 0.0f, 1.0f)));
    }
}

// What stands on the horizon in each place, drawn flat behind the hills:
// balloons over the meadow, a great mountain behind the garden, a flight of
// birds in autumn, snowy peaks (and at night an aurora) in the snow, islands at sea.
static void PlaceBackdrop(float horizonY)
{
    const Palette& p = gPalette;
    const Scene& s = gScene;
    const BiomeMix m = BiomeAt(s.worldX + 2.5f);
    const float base = horizonY + 4.0f, time = s.time;
    const Vector3 distant = Mix(p.skyBottom, p.skyTop, 0.45f);

    const float meadow = BiomeWeight(m, BIOME_MEADOW);
    if (meadow > 0.02f) {   // hot-air balloons
        static const Color kStripe[3][2] = { { { 240, 120, 120, 255 }, { 255, 240, 220, 255 } }, { { 110, 180, 230, 255 }, { 255, 236, 170, 255 } }, { { 250, 190, 90, 255 }, { 250, 250, 250, 255 } } };
        for (int i = 0; i < 3; i++) {
            const float x = Wrap(Hash01(i, 301) * gVW + time * (2.0f + i) - s.worldX * 1.2f, gVW + 80.0f) - 40.0f;
            const float y = horizonY * (0.2f + 0.3f * Hash01(i, 302)) + 3.0f * sinf(time * 0.5f + i * 2.0f);
            Poly2(x, y, 9, 11, 10, 0, Fade(kStripe[i][0], meadow));
            Poly2(x, y, 4, 11, 8, 0, Fade(kStripe[i][1], meadow));
            DrawLineEx(Vector2{ x - 5, y + 9 }, Vector2{ x - 2, y + 15 }, 1.0f, Fade(Color{ 92, 64, 72, 255 }, meadow));
            DrawLineEx(Vector2{ x + 5, y + 9 }, Vector2{ x + 2, y + 15 }, 1.0f, Fade(Color{ 92, 64, 72, 255 }, meadow));
            DrawRectangle((int)x - 2, (int)y + 15, 5, 4, Fade(Color{ 150, 106, 84, 255 }, meadow));
        }
    }

    const float blossom = BiomeWeight(m, BIOME_BLOSSOM);
    if (blossom > 0.02f) {  // one great snow-capped mountain
        const float x = Wrap(gVW * 0.62f - s.worldX * 0.8f, gVW + 420.0f) - 210.0f, height = horizonY * 0.66f, half = 150.0f;
        const Vector2 peak = { x, base - height };
        DrawTriangle(Vector2{ x - half, base }, Vector2{ x + half, base }, peak, ToColor(Mix(distant, Vector3{ 0.5f, 0.52f, 0.72f }, 0.35f), blossom));
        const float cap = 0.3f;   // the snow line, as a fraction of the height
        const Vector2 l = { x - half * cap, base - height * (1 - cap) }, r = { x + half * cap, base - height * (1 - cap) };
        const Color snow = ToColor(Mix(Vector3{ 1, 1, 1 }, p.skyTop, 0.15f), blossom);
        DrawTriangle(l, r, peak, snow);
        for (int i = 0; i < 3; i++) {   // a ragged lower edge
            const float u0 = i / 3.0f, u1 = (i + 1) / 3.0f;
            DrawTriangle(Vector2{ l.x + (r.x - l.x) * u0, l.y }, Vector2{ l.x + (r.x - l.x) * (u0 + u1) * 0.5f, l.y + 7.0f }, Vector2{ l.x + (r.x - l.x) * u1, l.y }, snow);
        }
    }

    const float snowy = BiomeWeight(m, BIOME_SNOW);
    if (snowy > 0.02f) {    // a jagged range with white tops
        const float width = 130.0f, scroll = s.worldX * 1.2f;
        const int first = (int)floorf(scroll / width) - 1;
        for (int j = first; j < first + (int)(gVW / width) + 4; j++) {
            const float x = j * width - scroll, height = horizonY * (0.34f + 0.3f * Hash01(j, 311)), half = width * 0.72f;
            const Vector2 peak = { x + (Hash01(j, 312) - 0.5f) * 30.0f, base - height };
            DrawTriangle(Vector2{ x - half, base }, Vector2{ x + half, base }, peak, ToColor(Mix(distant, Vector3{ 0.56f, 0.62f, 0.8f }, 0.4f), snowy));
            const Vector2 l = { peak.x - (peak.x - (x - half)) * 0.34f, peak.y + height * 0.34f }, r = { peak.x + ((x + half) - peak.x) * 0.34f, peak.y + height * 0.34f };
            DrawTriangle(l, r, peak, ToColor(Mix(Vector3{ 1, 1, 1 }, p.skyTop, 0.2f), snowy));
        }
        // In the pine forest, once it is dark, the aurora comes out.
        const float aurora = p.stars * ((m.a == BIOME_SNOW && m.va == 2 ? 1.0f - m.t : 0.0f) + (m.b == BIOME_SNOW && m.vb == 2 ? m.t : 0.0f));
        if (aurora > 0.02f) {
            for (int band = 0; band < 2; band++) {
                const Color glow = band ? Color{ 190, 150, 255, 255 } : Color{ 130, 255, 196, 255 };
                for (int i = 0; i * 6 < gVW; i++) {
                    const float y = horizonY * (0.3f + 0.16f * band) + 12.0f * sinf(i * 0.21f + time * 0.5f + band * 2.0f) + 6.0f * sinf(i * 0.53f - time * 0.3f);
                    const float h = 24.0f + 10.0f * sinf(i * 0.37f + time * 0.8f + band);
                    DrawRectangleGradientV(i * 6, (int)(y - h), 6, (int)h, Fade(glow, 0.0f), Fade(glow, 0.34f * aurora * (0.7f + 0.3f * s.high)));
                }
            }
        }
    }

    const float sea = BiomeWeight(m, BIOME_SEASIDE);
    if (sea > 0.02f) {      // low islands on the horizon
        for (int i = 0; i < 3; i++) {
            const float x = Wrap(Hash01(i, 321) * (gVW + 200.0f) - s.worldX * 1.0f, gVW + 200.0f) - 100.0f;
            Poly2(x, base - 1.0f, 34.0f + 26.0f * Hash01(i, 322), 9.0f + 5.0f * Hash01(i, 323), 9, 0, ToColor(Mix(distant, Vector3{ 0.3f, 0.5f, 0.56f }, 0.4f), sea));
        }
    }

    const float autumn = BiomeWeight(m, BIOME_AUTUMN);
    if (autumn > 0.02f) {   // birds heading south in a V
        const float lead = Wrap(time * 9.0f - s.worldX * 0.5f, gVW + 160.0f) - 80.0f, y0 = horizonY * 0.3f + 4.0f * sinf(time * 0.4f);
        const Color c = Fade(Color{ 70, 50, 60, 255 }, 0.8f * autumn);
        for (int i = -3; i <= 3; i++) {
            const float x = lead - abs(i) * 9.0f, y = y0 + i * 5.0f, flap = 1.5f + 1.5f * sinf(time * 6.0f + i);
            DrawLineEx(Vector2{ x - 4, y - flap }, Vector2{ x, y }, 1.0f, c);
            DrawLineEx(Vector2{ x, y }, Vector2{ x + 4, y - flap }, 1.0f, c);
        }
    }
}

// Sky, sun and moon, stars, clouds and two rows of distant hills, all flat 2D.
static void DrawSky(float horizonY)
{
    const Palette& p = gPalette;
    const Scene& s = gScene;
    DrawRectangleGradientV(0, 0, gVW, (int)horizonY + 2, ToColor(p.skyTop), ToColor(p.skyBottom));
    DrawRectangle(0, (int)horizonY + 2, gVW, gVH, ToColor(p.skyBottom));

    if (p.stars > 0.01f) {
        for (int i = 0; i < 70; i++) {
            const float x = Hash01(i, 11) * gVW, y = Hash01(i, 12) * horizonY * 0.85f;
            const float twinkle = 0.55f + 0.45f * sinf(s.time * (1.0f + Hash01(i, 13) * 2.0f) + i);
            const float a = p.stars * twinkle * (0.6f + 0.6f * s.high);
            const int size = i % 7 == 0 ? 2 : 1;
            DrawRectangle((int)x, (int)y, size, size, Color{ 255, 250, 235, (unsigned char)(std::clamp(a, 0.0f, 1.0f) * 255) });
        }
    }

    Rainbow(horizonY, std::clamp((gJoy - 0.65f) / 0.25f, 0.0f, 1.0f));

    // The sun sinks during the first part of the song; the moon rises for the last.
    const float sunT = std::clamp(s.day / 0.62f, 0.0f, 1.0f), moonT = std::clamp((s.day - 0.6f) / 0.4f, 0.0f, 1.0f);
    const float pulse = 1.0f + 0.05f * s.bass;
    if (sunT < 1.0f) {
        const float y = horizonY * (0.22f + 0.82f * sunT * sunT);
        Rays(gVW * 0.76f, y, 21 * pulse, 40.0f, 12, Mix(p.sun, Vector3{ 1, 1, 0.9f }, 0.5f), 0.5f * gJoy);
        Poly2(gVW * 0.76f, y, 26 * pulse, 26 * pulse, 12, 0, ToColor(Mix(p.sun, Vector3{ 1, 1, 1 }, 0.2f), 0.25f));
        Poly2(gVW * 0.76f, y, 17 * pulse, 17 * pulse, 10, 0, ToColor(Mix(p.sun, Vector3{ 1, 1, 0.9f }, 0.5f)));
    }
    if (moonT > 0.0f) {
        const float y = horizonY * (1.05f - 0.8f * moonT * (2 - moonT));
        Rays(gVW * 0.27f, y, 16 * pulse, 30.0f, 10, Vector3{ 0.94f, 0.95f, 1.0f }, 0.35f * gJoy * moonT);
        Poly2(gVW * 0.27f, y, 13 * pulse, 13 * pulse, 9, 0.3f, Color{ 240, 242, 255, 255 });
        Poly2(gVW * 0.27f + 5, y - 3, 10, 10, 8, 0.3f, ToColor(Mix(p.skyTop, p.skyBottom, std::clamp(y / horizonY, 0.0f, 1.0f))));
    }

    const Vector3 cloud = Mix(Mix(p.skyTop, p.skyBottom, 0.6f), Vector3{ 1, 1, 1 }, 0.55f - 0.35f * p.stars);
    for (int i = 0; i < 5; i++) {
        const float span = gVW + 160.0f;
        float x = fmodf(Hash01(i, 21) * span + s.time * (2.0f + i) - s.worldX * 1.5f, span);
        if (x < 0) x += span;
        x -= 80.0f;
        const float y = horizonY * (0.12f + 0.5f * Hash01(i, 22)), w = 26.0f + 22.0f * Hash01(i, 23);
        Poly2(x, y, w, w * 0.32f, 7, 0.2f, ToColor(cloud, 0.9f));
        Poly2(x + w * 0.5f, y + 3, w * 0.7f, w * 0.25f, 7, 0.9f, ToColor(cloud, 0.9f));
        Poly2(x - w * 0.45f, y + 4, w * 0.6f, w * 0.22f, 7, 0.5f, ToColor(cloud, 0.9f));
    }

    PlaceBackdrop(horizonY);

    for (int layer = 0; layer < 2; layer++) {
        const float width = layer == 0 ? 96.0f : 64.0f, scroll = s.worldX * (layer == 0 ? 2.0f : 4.5f);
        const Vector3 col = Mix(p.skyBottom, Mix(p.grass, p.skyTop, 0.5f), layer == 0 ? 0.3f : 0.55f);
        const int first = (int)floorf(scroll / width) - 1;
        for (int j = first; j < first + (int)(gVW / width) + 4; j++) {
            const float x = j * width - scroll, peak = (layer == 0 ? 46.0f : 30.0f) * (0.45f + 0.55f * Hash01(j, 31 + layer));
            const float base = horizonY + 4;
            DrawTriangle(Vector2{ x - width * 0.75f, base }, Vector2{ x + width * 0.75f, base },
                         Vector2{ x + (Hash01(j, 41 + layer) - 0.5f) * width * 0.4f, base - peak }, ToColor(col));
        }
    }
    JoyParticles(horizonY, false);
}

static Camera3D gCamera;
static bool gCloseup = false;      // screenshot option: bring the camera right up to the cat
static float gViewCentre = 0.0f;   // world x at the middle of the screen
static float gViewHalf = 5.3f;     // half the visible width of the path, in world units

static void EmitGround(Vector3 a, Vector3 b, Vector3 c, Vector3 col, float glow = 0.0f)
{
    // Ground triangles must face up whatever order they were built in.
    const float ny = (b.z - a.z) * (c.x - a.x) - (b.x - a.x) * (c.z - a.z);
    if (ny < 0.0f) EmitTri(a, c, b, col, glow); else EmitTri(a, b, c, col, glow);
}

static void DrawTerrain()
{
    static const float rows[] = { 40.0f, 16.0f, 7.0f, 3.6f, 1.0f, -1.0f, -2.6f, -5.0f, -8.0f, -12.0f, -17.0f, -24.0f };
    const int rowCount = (int)(sizeof(rows) / sizeof(rows[0]));
    const int x0 = (int)floorf(gViewCentre - gViewHalf * 3.5f) - 2, x1 = (int)floorf(gViewCentre + gViewHalf * 3.5f) + 3;
    for (int x = x0; x < x1; x++) {
        Vector3 grass, pathCol;
        GroundColours((float)x + 0.5f, grass, pathCol);
        const BiomeMix mix = BiomeAt((float)x + 0.5f);
        const int here = mix.t < 0.5f ? mix.a : mix.b;
        const float sea = BiomeWeight(mix, BIOME_SEASIDE), river = RiverWeight(mix);
        // Paths have a rhythm of their own: boardwalk planks by the sea, stone slabs in the garden.
        const float pathBeat = (here == BIOME_SEASIDE || here == BIOME_BLOSSOM) && (x & 1) ? 0.9f : 1.0f;
        for (int r = 0; r + 1 < rowCount; r++) {
            const float za = rows[r], zb = rows[r + 1];
            const Vector3 v00 = { (float)x, GroundHeight((float)x, za), za }, v10 = { (float)x + 1, GroundHeight((float)x + 1, za), za };
            const Vector3 v01 = { (float)x, GroundHeight((float)x, zb), zb }, v11 = { (float)x + 1, GroundHeight((float)x + 1, zb), zb };
            const bool path = za == 1.0f, seaCell = sea > 0.5f && za <= -8.0f, riverCell = river > 0.5f && za == -5.0f;
            const float shade = 0.92f + 0.16f * Hash01(x, r);
            Vector3 colA, colB;
            float glow = 0.0f;
            if (seaCell || riverCell) {
                // Bands of lighter water drift across; ice just glints.
                const float wave = 0.5f + 0.5f * sinf(x * 0.7f + r * 1.9f + gScene.time * 1.3f);
                const bool ice = riverCell && here == BIOME_SNOW;
                const Vector3 deep = ice ? Rgb(190, 222, 242) : riverCell ? Rgb(84, 172, 196) : Rgb(64, 164, 198);
                const Vector3 pale = ice ? Rgb(232, 244, 252) : Rgb(150, 222, 232);
                colA = Vector3Multiply(Mix(deep, pale, ice ? 0.5f * Hash01(x, 77) : wave * 0.6f), gShadePath);
                colB = Vector3Scale(colA, 0.94f);
                glow = ice ? 0.2f : 0.35f;
            } else {
                const Vector3 base = path ? Vector3Scale(pathCol, pathBeat) : Mix(grass, Vector3Scale(grass, 0.82f), (float)r / rowCount);
                colA = Vector3Scale(base, shade);
                colB = Vector3Scale(base, shade * (path ? 0.96f : 0.93f));
            }
            if ((x + r) & 1) { EmitGround(v00, v10, v11, colA, glow); EmitGround(v00, v11, v01, colB, glow); }
            else             { EmitGround(v00, v10, v01, colA, glow); EmitGround(v10, v11, v01, colB, glow); }
        }
    }
}

// Soft discs of light around lamps, windows and rung bells. Collected while
// the solid world is drawn and blended on top afterwards.
struct Halo { Vector3 pos; float radius; Vector3 col; float alpha; };
static Halo gHalos[220];
static int  gHaloCount = 0;
static const Vector3 kWarm = { 1.0f, 0.86f, 0.55f };

static void AddHalo(Vector3 pos, float radius, Vector3 col, float alpha)
{
    if (gHaloCount < 220 && alpha > 0.01f) gHalos[gHaloCount++] = { pos, radius, col, alpha };
}

// Trees, houses and small things, placed by hashing the position so the same
// stretch of path always looks the same. What grows there depends on the
// place and on which of its three variations this stretch is. Everything bops
// a little on the beat.
static void DrawScenery()
{
    static const Vector3 kLeaf[5] = { { 0.47f, 0.72f, 0.42f }, { 0.38f, 0.64f, 0.44f }, { 0.60f, 0.76f, 0.40f }, { 0.96f, 0.66f, 0.42f }, { 0.96f, 0.62f, 0.68f } };
    static const Vector3 kPetal[4] = { { 1.0f, 0.86f, 0.4f }, { 1.0f, 0.62f, 0.7f }, { 0.75f, 0.7f, 1.0f }, { 1.0f, 1.0f, 1.0f } };
    static const Vector3 kFall[4] = { { 0.98f, 0.62f, 0.30f }, { 0.90f, 0.38f, 0.30f }, { 0.98f, 0.80f, 0.36f }, { 0.72f, 0.46f, 0.30f } };
    static const Vector3 kBlossom[3] = { { 1.0f, 0.72f, 0.82f }, { 1.0f, 0.88f, 0.92f }, { 0.98f, 0.62f, 0.78f } };
    static const Vector3 kShellTint[3] = { { 1.0f, 0.6f, 0.56f }, { 1.0f, 0.82f, 0.5f }, { 0.96f, 0.7f, 0.86f } };
    static const Vector3 kLantern[4] = { { 1.0f, 0.46f, 0.42f }, { 1.0f, 0.86f, 0.46f }, { 1.0f, 0.66f, 0.80f }, { 1.0f, 0.96f, 0.86f } };
    const Palette& p = gPalette;
    const float bop = 1.0f + (0.07f + 0.18f * gJoy) * gScene.beat;   // everything dances harder on a streak
    const float lit = std::max(p.lamps, gJoy * 0.7f);                // and lamps come on even by day
    const Vector3 white = { 1, 1, 1 };
    const Vector3 night = Mix(white, Vector3{ 0.7f, 0.8f, 1.0f }, p.stars);   // leaves cool down after dark
    const Vector3 tiles = { 0.80f, 0.42f, 0.36f }, snowTint = { 0.95f, 0.97f, 1.0f }, pineGreen = { 0.30f, 0.50f, 0.42f };
    const Vector3 redPaint = { 0.86f, 0.36f, 0.30f }, timber = { 0.76f, 0.60f, 0.46f };
    const float spacing = 2.4f, time = gScene.time;
    const int s0 = (int)floorf((gViewCentre - gViewHalf * 2.7f - 3.0f) / spacing), s1 = (int)floorf((gViewCentre + gViewHalf * 2.7f + 3.0f) / spacing);

    for (int s = s0; s <= s1; s++) {
        const float fx = s * spacing;
        const float sway = 0.08f * gJoy * sinf(time * 3.2f + fx);
        const BiomeMix mix = BiomeAt(fx + 1.2f);
        const int biome = mix.t < 0.5f ? mix.a : mix.b, variant = mix.t < 0.5f ? mix.va : mix.vb;
        const bool river = HasRiver(biome, variant);

        auto ground = [](float x, float z) { return Vector3{ x, GroundHeight(x, z), z }; };
        auto afloat = [](float x, float z, float lift) { return Vector3{ x, RiverLevel(x) + lift, z }; };
        auto tree = [&](Vector3 at, float size, const Shape& top, Vector3 leaf) {
            DrawShape(sTrunk, Place(at, size, sway));
            DrawShape(top, Place(at, size, sway, bop), Vector3Multiply(leaf, night));
        };
        auto lamp = [&](Vector3 at) {
            DrawShape(sLampPost, Place(at));
            DrawShape(sLamp, Place(at), white, 0.25f + 0.75f * lit);
            AddHalo(Vector3{ at.x, at.y + 1.5f, at.z + 0.15f }, 0.5f + 0.06f * gScene.beat, kWarm, 0.13f * lit);
        };
        auto house = [&](Vector3 at, Vector3 roof, float glow) {
            DrawShape(sHouse, Place(at, 1.25f));
            DrawShape(sRoof, Place(at, 1.25f), Vector3Multiply(roof, night));
            DrawShape(sWindows, Place(at, 1.25f), white, 0.2f + 0.8f * glow);
            AddHalo(Vector3{ at.x + 0.38f, at.y + 0.88f, at.z + 1.0f }, 0.55f, kWarm, 0.12f * glow);
            for (int k = 0; k < 3; k++) {   // smoke curling out of the chimney
                const float puff = fmodf(time * 0.22f + k / 3.0f + Hash01(s, 33), 1.0f);
                AddHalo(Vector3{ at.x + 0.62f + 0.3f * puff, at.y + 2.5f + 1.3f * puff, at.z - 0.25f }, 0.14f + 0.24f * puff, Vector3{ 0.96f, 0.96f, 1.0f }, 0.3f * (1.0f - puff));
            }
        };
        auto flowers = [&](Vector3 at, int count, float spread) {
            for (int k = 0; k < count; k++) {
                const float ox = at.x + (Hash01(s, 50 + k) - 0.5f) * spread, oz = at.z + (Hash01(s, 60 + k) - 0.5f) * spread * 0.75f;
                DrawShape(sFlower, Place(ground(ox, oz), 1.0f, sway, bop), kPetal[Hash(s, 70 + k) & 3]);
            }
        };
        auto fence = [&](float z) { DrawShape(sFence, Place(ground(fx, z), 1.0f, atanf((GroundHeight(fx + 2.4f, z) - GroundHeight(fx, z)) / 2.4f))); };

        // Bands by depth: beside the path, the middle distance, far away, and in front.
        // With a river behind the path the middle band moves up onto the near bank.
        const float rNear = Hash01(s, 2), rMid = Hash01(s, 8), rFar = Hash01(s, 14), rFront = Hash01(s, 24), rExtra = Hash01(s, 30);
        const Vector3 nearAt = ground(fx + Hash01(s, 3) * 1.8f, -1.9f - Hash01(s, 4) * 1.3f);
        const Vector3 midAt = river ? ground(fx + Hash01(s, 9) * 2.0f, -3.0f - Hash01(s, 10) * 1.0f)
                                    : ground(fx + Hash01(s, 9) * 2.0f, -4.2f - Hash01(s, 10) * 3.4f);
        const Vector3 farAt = ground(fx + Hash01(s, 15) * 2.0f, -10.0f - Hash01(s, 16) * 5.5f);
        const Vector3 homeAt = ground(farAt.x, -10.0f - Hash01(s, 16) * 2.0f);
        const Vector3 frontAt = ground(fx + Hash01(s, 25) * 2.0f, 2.0f + Hash01(s, 26) * 1.6f);
        const float waterX = fx + Hash01(s, 31) * 2.0f, waterZ = -5.5f - Hash01(s, 32) * 2.0f;
        const float size = 0.95f + Hash01(s, 11) * 0.5f, farSize = 1.3f + Hash01(s, 17) * 0.8f, small = 0.8f + Hash01(s, 5) * 0.5f;

        // Along the edges of the path: little stones everywhere, tufts of grass where grass grows.
        Vector3 grassTint, pathTint;
        GroundColours(fx + 1.2f, grassTint, pathTint);
        if (Hash01(s, 40) < 0.6f) DrawShape(sRock, Place(ground(fx + Hash01(s, 41) * 2.2f, -1.12f), 0.3f), Vector3Scale(pathTint, 1.05f));
        if (Hash01(s, 42) < 0.6f) DrawShape(sRock, Place(ground(fx + Hash01(s, 43) * 2.2f, 1.12f), 0.3f), Vector3Scale(pathTint, 1.05f));
        if (biome != BIOME_SNOW && biome != BIOME_SEASIDE) {
            DrawShape(sTuft, Place(ground(fx + Hash01(s, 44) * 2.2f, -1.3f - Hash01(s, 45) * 0.5f), 1.0f, sway, bop), Vector3Scale(grassTint, 1.2f));
            DrawShape(sTuft, Place(ground(fx + Hash01(s, 46) * 2.2f, 1.3f + Hash01(s, 47) * 0.5f), 1.0f, sway, bop), Vector3Scale(grassTint, 1.2f));
        }

        switch (biome) {
            case BIOME_MEADOW: {
                if (variant == 0) {          // flower fields, with a windmill now and then
                    if (rNear < 0.5f) flowers(nearAt, 4, 1.2f);
                    else if (rNear < 0.6f) DrawShape(sBush, Place(nearAt, small, sway, bop), Vector3Multiply(kLeaf[Hash(s, 6) % 3], night));
                    else if (rNear < 0.66f) lamp(nearAt);
                    if (rMid < 0.5f) flowers(midAt, 5, 2.2f);
                    else if (rMid < 0.72f) tree(midAt, size, sCrown, kLeaf[Hash(s, 12) % 5]);
                    if (rFar < 0.09f) {
                        const Vector3 at = ground(farAt.x, -10.5f);
                        DrawShape(sWindmill, Place(at, 1.5f));
                        DrawShape(sBlades, MatrixMultiply(MatrixRotateZ(time * 0.8f + s), MatrixTranslate(at.x, at.y + 2.85f, at.z + 0.95f)), Vector3{ 1.5f, 1.5f, 1.5f });
                    } else if (rFar < 0.5f) tree(farAt, farSize, (Hash(s, 18) & 1) ? sPoplar : sCrown, Vector3Scale(kLeaf[Hash(s, 19) % 3], 0.88f));
                    if (rFront < 0.4f) DrawShape(sFlower, Place(frontAt, 1.2f, sway, bop), kPetal[Hash(s, 27) & 3]);
                } else if (variant == 1) {   // farmland: fences, rows of crops, a barn
                    fence(-1.75f);
                    for (int row = 0; row < 3; row++)
                        if (Hash01(s, 34 + row) < 0.8f)
                            DrawShape(sCrop, Place(ground(fx + 1.2f, -3.1f - row * 1.25f), 1.0f, sway, bop), Vector3Multiply(kLeaf[row == 1 ? 2 : 0], night));
                    if (rExtra < 0.1f) DrawShape(sScarecrow, Place(ground(fx + 1.2f, -6.9f), 1.2f, 0.06f * sinf(time * 2.0f + s) + sway));
                    else if (rExtra < 0.24f) DrawShape(sHay, Place(ground(fx + 0.6f, -7.2f), 0.9f));
                    if (rFar < 0.12f) DrawShape(sBarn, Place(homeAt, 1.2f));
                    else if (rFar < 0.6f) tree(farAt, farSize, sPoplar, Vector3Scale(kLeaf[Hash(s, 19) % 3], 0.9f));
                    if (rFront < 0.2f) DrawShape(sFlower, Place(frontAt, 1.2f, sway, bop), kPetal[Hash(s, 27) & 3]);
                    else if (rFront < 0.3f) DrawShape(sRock, Place(frontAt, 0.7f));
                } else {                     // riverside: reeds on the bank, ducks and lilies on the water, wooden bridges
                    if (rNear < 0.3f) flowers(nearAt, 3, 0.8f);
                    else if (rNear < 0.42f) DrawShape(sBush, Place(nearAt, small, sway, bop), Vector3Multiply(kLeaf[Hash(s, 6) % 3], night));
                    else if (rNear < 0.5f) lamp(nearAt);
                    if (rMid < 0.35f) DrawShape(sReed, Place(ground(midAt.x, -4.2f), 1.2f, sway * 2.0f, bop));
                    else if (rMid < 0.62f) tree(midAt, size * 0.9f, sCrown, kLeaf[Hash(s, 12) % 3]);
                    if (rExtra < 0.09f) DrawShape(sBridge, Place(afloat(fx + 1.2f, -6.5f, 0.02f)), timber);
                    else if (rExtra < 0.34f) DrawShape(sDuck, Place(afloat(waterX + 0.4f * sinf(time * 0.4f + s), waterZ, 0.02f * sinf(time * 2.0f + s))));
                    else if (rExtra < 0.62f) DrawShape(sLily, Place(afloat(waterX, waterZ, 0.01f), 1.0f + Hash01(s, 5) * 0.6f));
                    if (rFar < 0.12f) house(homeAt, tiles, lit);
                    else if (rFar < 0.62f) tree(farAt, farSize, (Hash(s, 18) % 3) ? sCrown : sPoplar, Vector3Scale(kLeaf[Hash(s, 19) % 3], 0.88f));
                    if (rFront < 0.2f) DrawShape(sFlower, Place(frontAt, 1.2f, sway, bop), kPetal[Hash(s, 27) & 3]);
                    else if (rFront < 0.32f) DrawShape(sBush, Place(frontAt, 0.6f, sway, bop), Vector3Multiply(kLeaf[Hash(s, 28) % 3], night));
                }
                break;
            }
            case BIOME_BLOSSOM: {
                auto stoneLantern = [&](Vector3 at) {
                    DrawShape(sStoneLantern, Place(at, 1.1f));
                    DrawShape(sLanternGlow, Place(at, 1.1f), white, 0.3f + 0.7f * lit);
                    AddHalo(Vector3{ at.x, at.y + 0.97f, at.z + 0.2f }, 0.42f, kWarm, 0.14f * lit);
                };
                if (variant == 2) {          // lantern festival: strings of paper lanterns and stalls
                    const float z = -1.85f, top = 2.15f, y0 = GroundHeight(fx, z) + top, y1 = GroundHeight(fx + spacing, z) + top;
                    DrawShape(sLanternPole, Place(ground(fx, z)));
                    for (int k = 0; k < 3; k++) {   // the string sags between this pole and the next
                        const float u = (k + 0.5f) / 3.0f;
                        const Vector3 at = { fx + spacing * u, y0 + (y1 - y0) * u - 0.32f * sinf(u * kPi), z };
                        const Vector3 tint = kLantern[(s * 3 + k) & 3];
                        DrawShape(sPaperLantern, Place(at, 1.0f, 0.12f * sinf(time * 1.8f + k + s) + sway, bop), tint, 0.45f + 0.5f * lit);
                        AddHalo(Vector3{ at.x, at.y, at.z + 0.15f }, 0.32f, tint, 0.16f * lit);
                    }
                    if (rMid < 0.26f) DrawShape(sStall, Place(ground(midAt.x, -3.6f), 1.15f));
                    else if (rMid < 0.72f) tree(midAt, size * 1.12f, sCrown, kBlossom[Hash(s, 12) % 3]);
                } else if (variant == 1) {   // a stream with red bridges, koi and lilies
                    if (rNear < 0.24f) flowers(nearAt, 3, 0.8f);
                    else if (rNear < 0.4f) stoneLantern(nearAt);
                    else if (rNear < 0.48f) DrawShape(sRock, Place(nearAt, small + 0.3f));
                    if (rMid < 0.5f) tree(midAt, size, sCrown, kBlossom[Hash(s, 12) % 3]);
                    else if (rMid < 0.7f) DrawShape(sReed, Place(ground(midAt.x, -4.2f), 1.1f, sway * 2.0f, bop));
                    if (rExtra < 0.12f) DrawShape(sBridge, Place(afloat(fx + 1.2f, -6.5f, 0.02f)), redPaint);
                    else if (rExtra < 0.5f)    // koi circling just under the surface
                        DrawShape(sKoi, Place(afloat(waterX + 0.5f * cosf(time * 0.8f + s), waterZ + 0.3f * sinf(time * 0.8f + s), 0.02f)), (s & 1) ? white : Vector3{ 1.0f, 0.9f, 0.9f });
                    else if (rExtra < 0.8f) DrawShape(sLily, Place(afloat(waterX, waterZ, 0.01f), 1.0f + Hash01(s, 5) * 0.6f));
                } else {                     // the garden: stone lanterns, bamboo, gates
                    if (rNear < 0.26f) flowers(nearAt, 3, 0.8f);
                    else if (rNear < 0.34f) DrawShape(sBush, Place(nearAt, small, sway, bop), Vector3Multiply(kLeaf[1], night));
                    else if (rNear < 0.42f) DrawShape(sRock, Place(nearAt, small + 0.3f));
                    else if (rNear < 0.58f) stoneLantern(nearAt);
                    if (rMid < 0.62f) tree(midAt, size * 1.12f, sCrown, kBlossom[Hash(s, 12) % 3]);
                    else if (rMid < 0.84f) DrawShape(sBamboo, Place(midAt, 0.9f + Hash01(s, 11) * 0.4f, sway * 0.6f), night);
                }
                if (rFar < 0.10f) DrawShape(sGate, Place(ground(farAt.x, -10.0f), 1.35f));
                else if (rFar < 0.62f) {
                    if (Hash(s, 18) % 4) tree(farAt, farSize, sCrown, Vector3Scale(kBlossom[Hash(s, 19) % 3], 0.94f));
                    else tree(farAt, farSize, sPine, Vector3Scale(kLeaf[1], 0.85f));
                }
                if (rFront < 0.22f) DrawShape(sFlower, Place(frontAt, 1.2f, sway, bop), kPetal[1 + (Hash(s, 27) & 1) * 2]);
                else if (rFront < 0.3f) DrawShape(sRock, Place(frontAt, 0.7f));
                break;
            }
            case BIOME_AUTUMN: {
                if (variant == 1) {          // pumpkin patch: rows of pumpkins behind a fence, a scarecrow keeping watch
                    fence(-1.75f);
                    for (int row = 0; row < 3; row++)
                        if (Hash01(s, 34 + row) < 0.75f)
                            DrawShape(sPumpkin, Place(ground(fx + 0.4f + Hash01(s, 37 + row) * 1.6f, -3.0f - row * 1.3f), 0.7f + Hash01(s, 5 + row) * 0.5f, 0.0f, bop));
                    if (rExtra < 0.12f) DrawShape(sScarecrow, Place(ground(fx + 1.2f, -7.0f), 1.2f, 0.06f * sinf(time * 2.0f + s) + sway));
                    if (rFar < 0.2f) DrawShape(sHay, Place(farAt, 1.3f));
                    else if (rFar < 0.68f) tree(farAt, farSize, sCrown, Vector3Scale(kFall[Hash(s, 19) % 4], 0.9f));
                } else if (variant == 2) {   // harvest village: houses with smoking chimneys, hay, lamps
                    if (rNear < 0.16f) DrawShape(sPumpkin, Place(nearAt, 0.7f + Hash01(s, 5) * 0.5f, 0.0f, bop));
                    else if (rNear < 0.32f) lamp(nearAt);
                    else if (rNear < 0.46f) DrawShape(sLeafPile, Place(nearAt, small + 0.2f), Vector3Multiply(kFall[Hash(s, 6) % 4], night));
                    else if (rNear < 0.56f) DrawShape(sBarrel, Place(nearAt, 1.0f));
                    if (rMid < 0.26f) house(ground(midAt.x, -5.6f), Vector3{ 0.62f, 0.40f, 0.34f }, lit);
                    else if (rMid < 0.42f) DrawShape(sHay, Place(midAt, 0.9f));
                    else if (rMid < 0.72f) tree(midAt, size, sCrown, kFall[Hash(s, 12) % 4]);
                    if (rFar < 0.16f) house(homeAt, tiles, lit);
                    else if (rFar < 0.66f) tree(farAt, farSize, (Hash(s, 18) % 3) ? sCrown : sPoplar, Vector3Scale(kFall[Hash(s, 19) % 4], 0.9f));
                } else {                     // deep woods: trees close together, mushrooms, stumps, leaf piles
                    if (rNear < 0.26f) DrawShape(sMushroom, Place(nearAt, 1.3f, sway, bop));
                    else if (rNear < 0.4f) DrawShape(sStump, Place(nearAt, small));
                    else if (rNear < 0.5f) DrawShape(sRock, Place(nearAt, small + 0.3f));
                    else if (rNear < 0.7f) DrawShape(sLeafPile, Place(nearAt, small + 0.2f), Vector3Multiply(kFall[Hash(s, 6) % 4], night));
                    if (rMid < 0.85f) {
                        if (Hash(s, 13) % 5) tree(midAt, size, sCrown, kFall[Hash(s, 12) % 4]);
                        else tree(midAt, size, sPine, Vector3Scale(kLeaf[1], 0.8f));
                    }
                    if (rExtra < 0.6f) tree(ground(fx + Hash01(s, 38) * 2.0f, -6.6f - Hash01(s, 39) * 2.0f), size * 1.15f, sCrown, Vector3Scale(kFall[Hash(s, 36) % 4], 0.95f));
                    if (rFar < 0.8f) tree(farAt, farSize, sCrown, Vector3Scale(kFall[Hash(s, 19) % 4], 0.9f));
                }
                if (rFront < 0.16f) DrawShape(sPumpkin, Place(frontAt, 0.6f, 0.0f, bop));
                else if (rFront < 0.28f) DrawShape(sLeafPile, Place(frontAt, 0.7f), Vector3Multiply(kFall[Hash(s, 28) % 4], night));
                else if (rFront < 0.36f) DrawShape(sRock, Place(frontAt, 0.7f));
                break;
            }
            case BIOME_SNOW: {
                auto snowyPine = [&](Vector3 at, float scale, float shade) {
                    tree(at, scale, sPine, Vector3Scale(pineGreen, shade));
                    DrawShape(sSnowCap, Place(at, scale, sway, bop), Vector3Multiply(snowTint, night));
                };
                if (rNear < 0.14f) DrawShape(sSnowman, Place(nearAt, 1.0f, sway, bop));
                else if (rNear < 0.28f) DrawShape(sRock, Place(nearAt, small + 0.3f), Vector3{ 0.9f, 0.94f, 1.0f });
                else if (rNear < 0.40f) lamp(nearAt);
                else if (rNear < 0.52f) DrawShape(sBush, Place(nearAt, small), Vector3Multiply(snowTint, night));   // a snowdrift
                if (variant == 1) {          // frozen river: skating marks on the ice, igloos on the far bank
                    if (rMid < 0.5f) snowyPine(midAt, size * 0.9f, 1.0f);
                    if (rExtra < 0.4f) DrawShape(sRing, Place(afloat(waterX, waterZ, 0.02f), 0.5f + Hash01(s, 5) * 0.5f), Vector3Multiply(white, night));
                    if (rFar < 0.14f) DrawShape(sIgloo, Place(ground(farAt.x, -10.4f), 1.1f), Vector3Multiply(snowTint, night));
                    else if (rFar < 0.66f) snowyPine(farAt, farSize, 0.9f);
                } else if (variant == 2) {   // pine forest: trees close together, the odd lonely cabin
                    if (rMid < 0.88f) snowyPine(midAt, size, 1.0f);
                    if (rExtra < 0.7f) snowyPine(ground(fx + Hash01(s, 38) * 2.0f, -6.8f - Hash01(s, 39) * 2.0f), size * 1.2f, 0.95f);
                    if (rFar < 0.06f) house(homeAt, snowTint, std::max(lit, 0.55f));
                    else if (rFar < 0.85f) snowyPine(farAt, farSize, 0.9f);
                } else {                     // the village: cabins that keep a light on
                    if (rMid < 0.62f) snowyPine(midAt, size, 1.0f);
                    if (rFar < 0.20f) house(homeAt, snowTint, std::max(lit, 0.55f));
                    else if (rFar < 0.66f) snowyPine(farAt, farSize, 0.9f);
                }
                if (rFront < 0.2f) DrawShape(sBush, Place(frontAt, 0.6f), Vector3Multiply(snowTint, night));
                else if (rFront < 0.3f) DrawShape(sRock, Place(frontAt, 0.7f), Vector3{ 0.9f, 0.94f, 1.0f });
                break;
            }
            case BIOME_SEASIDE: {
                auto palm = [&](float chance) {   // palms stay on the sand, above the waterline
                    if (rMid >= chance) return;
                    const Vector3 at = ground(midAt.x, -4.4f - Hash01(s, 10) * 1.8f);
                    DrawShape(sPalmTrunk, Place(at, size * 0.9f, sway * 0.7f));
                    DrawShape(sPalmLeaves, Place(at, size * 0.9f, sway * 0.7f, bop), Vector3Multiply(kLeaf[Hash(s, 12) % 3], night));
                };
                auto boat = [&](float chance) {
                    if (rFar >= chance) return;
                    const Vector3 at = { farAt.x + 0.8f * sinf(time * 0.3f + s), kSeaLevel + 0.05f * sinf(time * 1.7f + s), -11.0f - Hash01(s, 16) * 6.0f };
                    DrawShape(sBoat, Place(at, 1.5f, 0.06f * sinf(time * 1.3f + s)));
                };
                if (variant == 1) {          // harbour: a pier, crates and barrels, now and then a lighthouse
                    if (rNear < 0.16f) DrawShape(sCrate, Place(nearAt, 1.0f + Hash01(s, 5) * 0.4f));
                    else if (rNear < 0.30f) DrawShape(sBarrel, Place(nearAt, 1.0f));
                    else if (rNear < 0.44f) lamp(nearAt);
                    else if (rNear < 0.54f) DrawShape(sRock, Place(nearAt, small + 0.2f));
                    if (rExtra < 0.12f) DrawShape(sPier, Place(Vector3{ fx + 1.2f, kSeaLevel + 0.1f, -6.4f }));
                    else if (rExtra < 0.19f) {
                        const Vector3 at = ground(fx + 1.2f, -6.2f);
                        const float beam = 0.5f + 0.5f * sinf(time * 2.2f + s);   // the lamp turns
                        DrawShape(sLighthouse, Place(at, 1.25f));
                        DrawShape(sLighthouseLamp, Place(at, 1.25f), white, 0.3f + 0.7f * lit * beam);
                        AddHalo(Vector3{ at.x, at.y + 4.7f, at.z + 0.4f }, 0.9f + 0.5f * beam, kWarm, 0.22f * lit * beam);
                    }
                    palm(0.15f);
                    boat(0.36f);
                } else if (variant == 2) {   // rocky cove: stacks of rock in the water, crabs on the sand
                    if (rNear < 0.3f) DrawShape(sRock, Place(nearAt, small + 0.5f));
                    else if (rNear < 0.5f) DrawShape(sCrab, Place(ground(nearAt.x + 0.35f * sinf(time * 2.4f + s), nearAt.z), 1.0f, 0.0f, bop));
                    else if (rNear < 0.62f) DrawShape(sShell, Place(nearAt, 1.0f + Hash01(s, 5)), kShellTint[Hash(s, 6) % 3]);
                    if (rExtra < 0.4f) DrawShape(sSeaStack, Place(Vector3{ fx + Hash01(s, 38) * 2.0f, kSeaLevel - 0.15f, -10.0f - Hash01(s, 39) * 6.0f }, 1.0f + Hash01(s, 5) * 0.9f));
                    palm(0.25f);
                    boat(0.1f);
                } else {                     // the beach: umbrellas, sandcastles, starfish
                    if (rNear < 0.16f) DrawShape(sUmbrella, Place(nearAt, 1.1f, sway * 0.5f));
                    else if (rNear < 0.28f) DrawShape(sCastle, Place(nearAt, 1.0f));
                    else if (rNear < 0.46f) DrawShape(sShell, Place(nearAt, 1.0f + Hash01(s, 5)), kShellTint[Hash(s, 6) % 3]);
                    else if (rNear < 0.56f) DrawShape(sRock, Place(nearAt, small + 0.2f));
                    else if (rNear < 0.62f) lamp(nearAt);
                    palm(0.4f);
                    boat(0.22f);
                }
                if (rFront < 0.24f) DrawShape(sShell, Place(frontAt, 1.2f), kShellTint[Hash(s, 27) % 3]);
                else if (rFront < 0.34f) DrawShape(sRock, Place(frontAt, 0.7f));
                break;
            }
        }

        // Small pastel butterflies join in behind the path once the streak is going.
        if (gJoy > 0.4f && Hash01(s, 90) < (gJoy - 0.4f) * 1.4f) {
            static const Vector3 kPastel[3] = { { 1.0f, 0.72f, 0.84f }, { 1.0f, 0.92f, 0.6f }, { 0.82f, 0.74f, 1.0f } };
            const float phase = Hash01(s, 91) * 6.283f;
            const float x = fx + Hash01(s, 92) * 2.0f + 0.5f * sinf(time * 0.9f + phase), z = -2.2f - Hash01(s, 93) * 3.2f;
            const float y = std::max(GroundHeight(x, z), kSeaLevel) + 1.0f + Hash01(s, 94) * 1.3f + 0.25f * sinf(time * 2.1f + phase);
            const float flap = 0.1f + 1.0f * (0.5f + 0.5f * sinf(time * 16.0f + phase));
            const Matrix body = Place(Vector3{ x, y, z }, 0.5f);
            DrawShape(sWing, MatrixMultiply(MatrixRotateY(-flap), body), kPastel[Hash(s, 95) % 3], 0.6f);
            DrawShape(sWing, MatrixMultiply(MatrixRotateY(kPi + flap), body), kPastel[Hash(s, 95) % 3], 0.6f);
        }
    }

    // Flowers that bloomed because of the streak: they spring up with a bounce.
    for (const Bloom& b : gBlooms) {
        if (b.x < gViewCentre - gViewHalf * 1.6f - 2.0f || b.x > gViewCentre + gViewHalf * 1.6f + 2.0f) continue;
        const float k = std::clamp((time - b.born) * 3.5f, 0.0f, 1.0f);
        const float size = b.size * k * (1.0f + 0.45f * sinf(k * kPi));
        DrawShape(sFlower, Place(Vector3{ b.x, GroundHeight(b.x, b.z), b.z }, size, 0.1f * gJoy * sinf(time * 3.2f + b.x), bop), kPetal[b.tint]);
    }
}

static void DrawCat(float x, bool walking)
{
    const float joy = gJoy;
    const float ground = PathHeight(x);
    const float u = gCatAction >= 0 ? gCatActionAge / kActionTime[gCatAction] : 1.0f;
    const float arc = u < 1.0f ? sinf(kPi * u) : 0.0f;
    float lift = 0, pitch = 0, roll = 0, frontLegs = 0, paw = 0, tuck = 0;
    if (u < 1.0f) switch (gCatAction) {
        case ACT_HOP:     lift = 1.0f * 4 * u * (1 - u); pitch = 0.3f * cosf(kPi * u); tuck = arc; break;
        case ACT_BAT:     lift = 0.12f * arc; pitch = -0.32f * arc; frontLegs = 0.9f * arc; break;
        case ACT_SWAT:    lift = 0.5f * arc; pitch = 0.45f * arc; paw = 2.0f * arc; break;
        case ACT_RING:    lift = 0.22f * arc; pitch = 0.95f * arc; frontLegs = 1.6f * arc; break;
        case ACT_STUMBLE: roll = 0.35f * sinf(u * kPi * 4) * (1 - u); pitch = -0.15f * arc; break;
    }
    const float phase = x * 3.0f;   // short legs take quick little steps
    const bool stepping = walking && lift < 0.05f;
    // A happy cat prances: a bigger bounce in its step, and a little hop on every beat.
    const float bob = walking ? (0.03f + 0.06f * joy) * fabsf(cosf(phase)) + 0.06f * joy * gScene.beat : 0.0f;
    const float slope = atanf((PathHeight(x + 0.4f) - PathHeight(x - 0.4f)) / 0.8f);

    // The whole cat pivots around its back hips, so pitching up reads as standing.
    // It stretches upwards for an instant on every perfect hit.
    const Vector3 hips = { -0.24f, 0.31f, 0.0f };
    Matrix root = MatrixTranslate(-hips.x, -hips.y, -hips.z);
    root = MatrixMultiply(root, MatrixRotateX(roll));
    root = MatrixMultiply(root, MatrixRotateZ(pitch + slope * 0.7f));
    root = MatrixMultiply(root, MatrixTranslate(hips.x, hips.y, 0.0f));
    root = MatrixMultiply(root, MatrixScale(kCatSize, kCatSize * (1.0f + 0.1f * gJoyKick), kCatSize));
    root = MatrixMultiply(root, MatrixTranslate(x, ground + lift + bob, 0.0f));

    const Vector3 plain = { 1, 1, 1 };
    const float lift2 = 0.3f;    // how far the cat's shading is pulled towards its true colours
    DrawShape(sCatBody, root, plain, lift2);
    DrawShape(sCatNeck, root, plain, lift2);
    if (gCfg.back == 1) {
        DrawShape(sCatBack, root, plain, lift2);
    } else if (gCfg.back == 2) {   // the balloon trails behind and bobs
        const Matrix tied = MatrixMultiply(MatrixRotateZ(0.25f + 0.1f * sinf(gScene.time * 1.7f) + 0.05f * gScene.beat), MatrixTranslate(-0.08f, 0.66f, 0));
        DrawShape(sCatBack, MatrixMultiply(tied, root), plain, 0.35f);
    } else if (gCfg.back == 3) {   // wings flutter, faster when happy
        const float flap = 0.35f + 0.45f * sinf(gScene.time * (7.0f + 6.0f * joy));
        for (int side = -1; side <= 1; side += 2) {
            const Matrix wing = MatrixMultiply(MatrixRotateX(-side * (0.5f + flap)), MatrixTranslate(0.06f, 0.66f, side * 0.14f));
            DrawShape(sCatWing, MatrixMultiply(wing, root), plain, 0.5f);
        }
    }

    // The head is big, stays roughly level, is turned a little towards the camera, and nods along when happy.
    Matrix head = MatrixScale(1.22f, 1.22f, 1.22f);
    head = MatrixMultiply(head, MatrixRotateX(0.05f * sinf(gScene.time * 0.9f)));   // a slow curious tilt
    head = MatrixMultiply(head, MatrixRotateY(-0.78f));                              // three-quarter view: both eyes show
    head = MatrixMultiply(head, MatrixRotateZ(-pitch * 0.55f + 0.05f * sinf(gScene.time * 1.3f) + 0.09f * joy * sinf(gScene.wag * 1.5f)));
    head = MatrixMultiply(head, MatrixTranslate(0.33f, 0.76f + 0.02f * sinf(phase * 2), 0.04f));
    head = MatrixMultiply(head, root);
    DrawShape(sCatHead, head, plain, 0.48f);
    DrawShape(sCatHat, head, plain, gCfg.hat == 10 ? 0.85f : 0.3f);
    // Rosy cheeks all the time, rosier when happy.
    DrawShape(sCatBlush, head, plain, joy > 0.3f ? 0.9f : 0.45f);
    const bool blinking = fmodf(gScene.time, 3.7f) < 0.12f;
    const bool pleased = (gJudgeKind == J_PERFECT && gJudgeAge < 0.3f) || joy > 0.6f;   // eyes smile shut
    const Matrix eyes = MatrixMultiply(MatrixScale(1.0f, blinking ? 0.12f : pleased ? 0.42f : 1.0f, 1.0f), MatrixTranslate(0, 0.0f, 0));
    DrawShape(sCatEyes, MatrixMultiply(eyes, head), plain, 0.55f);

    // Legs: diagonal pairs step together, and each paw lifts as its leg swings forward.
    for (int i = 0; i < 4; i++) {
        const bool front = i < 2, nearSide = (i & 1) == 0;
        const float legPhase = phase + ((front == nearSide) ? 0.0f : kPi);
        float upper = stepping ? 0.55f * sinf(legPhase) : 0.0f;
        float lower = stepping ? -0.8f * std::max(0.0f, cosf(legPhase)) : 0.0f;
        if (front) {
            upper += frontLegs + tuck + (nearSide ? paw : 0.0f);
            lower -= 0.8f * tuck + 0.3f * frontLegs + (nearSide ? 0.25f * paw : 0.0f);
        } else {
            upper -= tuck * 0.9f;
            lower += 0.3f * tuck;
        }
        const Vector3 hip = front ? Vector3{ 0.22f, 0.30f, nearSide ? 0.14f : -0.14f } : Vector3{ -0.24f, 0.31f, nearSide ? 0.16f : -0.16f };
        const Matrix thigh = MatrixMultiply(MatrixRotateZ(upper), MatrixTranslate(hip.x, hip.y, hip.z));
        const Matrix shin = MatrixMultiply(MatrixMultiply(MatrixRotateZ(lower), MatrixTranslate(0, -0.13f, 0)), thigh);
        DrawShape(sCatThigh[front ? 0 : 1], MatrixMultiply(thigh, root), plain, lift2);
        DrawShape(sCatShin[front ? 0 : 1], MatrixMultiply(shin, root), plain, lift2);
    }

    // The tail goes up and wags faster with the mood; the wave travels from base to tip.
    const float amp = 0.12f + 0.14f * joy;
    Matrix seg = MatrixMultiply(MatrixRotateZ(0.95f - 0.4f * joy + amp * sinf(gScene.wag) + 0.1f * gScene.beat), MatrixTranslate(-0.40f, 0.50f, 0));
    for (int i = 0; i < 5; i++) {
        if (i > 0) seg = MatrixMultiply(MatrixMultiply(MatrixRotateZ(-0.28f + amp * sinf(gScene.wag - i * 0.7f)), MatrixTranslate(0, 0.18f, 0)), seg);
        DrawShape(sCatTail[i], MatrixMultiply(seg, root), plain, lift2);
    }
}

// The objects the chart placed on the path, each in its current state.
static void DrawNotes(double t)
{
    if (!gChart || !PathFollowsSong()) return;
    const int total = (int)gChart->time.size();
    const float limit = gViewCentre + gViewHalf + 4.0f;
    const Vector3 white = { 1, 1, 1 }, dull = { 0.6f, 0.6f, 0.66f };
    for (int i = gTail; i < total; i++) {
        const Vector3 at = NotePosition(i);
        if (at.x > limit) break;
        const int state = gNoteState[i], action = gNoteAction[i];
        const bool hit = state >= 1 && state <= 3;
        const float since = state ? (float)t - gNoteWhen[i] : 0.0f;
        const Vector3 tint = state == 4 ? dull : white;

        switch (action) {
            case ACT_HOP:
                DrawShape(sLog, Place(at, 1.0f, 0.0f, hit ? 1.0f + 0.25f * expf(-since * 8.0f) * cosf(since * 30.0f) : 1.0f), tint);
                break;
            case ACT_BAT: {
                if (hit && since > 0.7f) break;
                // Batted yarn rolls off ahead of the cat and shrinks away.
                const float roll = hit ? since : 0.0f;
                const float bounce = hit ? fabsf(sinf(roll * 9.0f)) * 0.5f * (1 - roll / 0.7f) : 0.03f * sinf(gScene.time * 3 + i);
                DrawShape(sYarn, Place(Vector3{ at.x + roll * 9.0f, at.y + bounce, 0 }, hit ? 1.0f - roll / 0.7f : 1.0f, -roll * 14.0f), tint);
                break;
            }
            case ACT_SWAT: {
                if (hit && since > 0.9f) break;
                // A caught butterfly flutters up and away.
                const Vector3 pos = { at.x + (hit ? since * 2.5f : 0.0f), at.y + 0.1f * sinf(gScene.time * 4 + i) + (hit ? since * since * 5.0f : 0.0f), 0 };
                const float flap = 0.1f + 1.0f * (0.5f + 0.5f * sinf(gScene.time * (hit ? 40.0f : 14.0f + 10.0f * gScene.high) + i));
                const Matrix body = Place(pos, 1.35f * (hit ? 1.0f - since / 0.9f : 1.0f));
                DrawShape(sButterflyBody, body, tint);
                DrawShape(sWing, MatrixMultiply(MatrixRotateY(-flap), body), Vector3Multiply(tint, Vector3{ 0.667f, 0.882f, 0.98f }), 0.75f);
                DrawShape(sWing, MatrixMultiply(MatrixRotateY(kPi + flap), body), Vector3Multiply(tint, Vector3{ 0.667f, 0.882f, 0.98f }), 0.75f);
                break;
            }
            case ACT_RING: {
                const Vector3 base = { at.x, PathHeight(at.x), 0 };
                DrawShape(sBellStalk, Place(base), tint);
                const float swing = hit ? 0.9f * expf(-since * 2.5f) * cosf(since * 14.0f) : 0.08f * sinf(gScene.time * 2 + i) + 0.1f * gScene.beat;
                const Matrix bell = MatrixMultiply(MatrixRotateZ(swing), MatrixTranslate(base.x, base.y + 2.4f, 0));
                DrawShape(sBell, bell, tint, hit ? 0.5f + 0.5f * expf(-since * 1.5f) : 0.0f);
                // A rung bell flashes, then stays softly lit like a lantern once it gets dark.
                if (hit) AddHalo(Vector3{ base.x, base.y + 1.75f, 0.15f }, 0.45f, kWarm, 0.10f * gPalette.lamps + 0.25f * expf(-since * 2.0f));
                break;
            }
        }
    }
}

// Translucent things, drawn last without writing depth.
static void DrawSparksAndShadows()
{
    const Vector3 shade = { 0.08f, 0.06f, 0.16f };
    const float catX = gScene.worldX;
    DrawShape(sShadow, Place(Vector3{ catX, PathHeight(catX) + 0.03f, 0 }, 0.7f), shade, 1.0f, 0.28f);

    // A soft spot of light on the path marks where things should be answered.
    if (PathFollowsSong()) {
        const float px = catX + kReach;
        const Vector3 cream = { 1.0f, 0.97f, 0.88f };
        DrawShape(sShadow, Place(Vector3{ px, PathHeight(px) + 0.035f, 0 }, 0.5f), cream, 1.0f, 0.22f + 0.25f * gScene.beat);
        DrawShape(sShadow, Place(Vector3{ px, PathHeight(px) + 0.04f, 0 }, 0.26f), cream, 1.0f, 0.3f + 0.3f * gScene.beat);
    }
    if (gChart && PathFollowsSong()) {
        const int total = (int)gChart->time.size();
        for (int i = gTail; i < total; i++) {
            const Vector3 at = NotePosition(i);
            if (at.x > gViewCentre + gViewHalf + 4.0f) break;
            if (gNoteState[i] && gNoteAction[i] != ACT_HOP) continue;
            DrawShape(sShadow, Place(Vector3{ at.x, PathHeight(at.x) + 0.03f, 0 }, gNoteAction[i] == ACT_HOP ? 0.5f : 0.3f), shade, 1.0f, 0.22f);
        }
    }
    for (int i = 0; i < gHaloCount; i++) {
        const Halo& h = gHalos[i];
        DrawShape(sHalo, Place(h.pos, h.radius), h.col, 1.0f, h.alpha);
        DrawShape(sHalo, Place(h.pos, h.radius * 0.45f), h.col, 1.0f, h.alpha * 1.3f);
    }

    // Fireflies come out after dark and blink faster when the music sparkles.
    const float dark = (gPalette.lamps - 0.3f) / 0.7f;
    if (dark > 0.0f) {
        const float time = gScene.time, spacing = 3.1f;
        const int s0 = (int)floorf((gViewCentre - gViewHalf * 1.6f) / spacing), s1 = (int)floorf((gViewCentre + gViewHalf * 1.6f) / spacing);
        for (int s = s0; s <= s1; s++) {
            for (int k = 0; k < 2; k++) {
                const float phase = Hash01(s, 80 + k) * 6.283f;
                const float x = s * spacing + Hash01(s, 82 + k) * 3.0f + 0.4f * sinf(time * 0.7f + phase);
                const float z = k == 0 ? -1.4f - Hash01(s, 84) * 5.0f : 1.4f + Hash01(s, 85) * 1.6f;
                const float y = GroundHeight(x, z) + 0.5f + Hash01(s, 86 + k) * 0.9f + 0.3f * sinf(time * 1.1f + phase * 2.0f);
                const float blink = std::max(0.0f, sinf(time * (1.6f + 1.2f * gScene.high) + phase));
                const Vector3 glow = { 1.0f, 0.96f, 0.55f };
                DrawShape(sHalo, Place(Vector3{ x, y, z }, 0.035f), glow, 1.0f, dark * blink);
                DrawShape(sHalo, Place(Vector3{ x, y, z }, 0.10f), glow, 1.0f, dark * blink * 0.2f);
            }
        }
    }

    for (const Ring& ring : gRings) {
        if (ring.age < 0.0f || ring.age >= ring.life) continue;
        const float k = ring.age / ring.life, grow = 1.0f - (1.0f - k) * (1.0f - k);   // fast at first, then easing out
        DrawShape(ring.upright ? sRingUp : sRing, Place(ring.pos, ring.size * (0.2f + 0.8f * grow)), ring.col, 1.0f, (ring.upright ? 0.75f : 0.5f) * (1.0f - k) * (1.0f - k));
    }

    for (const Spark& s : gSparks) {
        if (s.life <= 0.0f) continue;
        const float k = s.life / s.span;   // 1 when born, 0 when gone
        if (s.kind == SPARK_PUFF) {        // a soft cloud that swells as it fades
            DrawShape(sHalo, Place(s.pos, s.size * (1.0f + 2.2f * (1.0f - k))), s.col, 1.0f, 0.45f * k);
            continue;
        }
        if (s.kind == SPARK_SHARD) {
            DrawShape(sSpark, Place(s.pos, s.size + s.life * 0.3f, s.life * 9.0f), s.col, 1.0f, std::min(1.0f, s.life * 4.0f));
        } else if (s.kind == SPARK_STAR) {
            const float pop = sinf(std::min(1.0f, (1.0f - k) * 6.0f) * kPi * 0.5f);   // springs open, then shrinks
            DrawShape(sStar, Place(s.pos, s.size * pop * (0.45f + 0.55f * k), (1.0f - k) * 4.0f), s.col, 1.0f, std::min(1.0f, k * 3.0f));
        } else {
            const float pop = sinf(std::min(1.0f, (1.0f - k) * 5.0f) * kPi * 0.5f);
            DrawShape(sHeart, Place(s.pos, s.size * pop, 0.25f * sinf(s.life * 7.0f)), s.col, 1.0f, std::min(1.0f, k * 2.5f));
        }
    }
}


// Keeps a colour's brightness but pulls its hue towards `hue`.
static Vector3 TintTowards(Vector3 col, Vector3 hue, float amount)
{
    const float lum = 0.3f * col.x + 0.59f * col.y + 0.11f * col.z, hueLum = 0.3f * hue.x + 0.59f * hue.y + 0.11f * hue.z;
    return Mix(col, Vector3Scale(hue, lum / std::max(0.05f, hueLum)), amount);
}

// Weather and wildlife of the place, drawn flat. `front` is the layer that passes in front of the world.
static void PlaceParticles(float horizonY, bool front)
{
    const BiomeMix m = BiomeAt(gViewCentre);
    const float time = gScene.time;
    if (!front) {
        // Seagulls drift over the sea.
        const float gulls = BiomeWeight(m, BIOME_SEASIDE) * 4.0f;
        for (int i = 0; i < (int)gulls + 1; i++) {
            const float a = std::clamp(gulls - i, 0.0f, 1.0f);
            if (a <= 0.0f) break;
            const float x = Wrap(Hash01(i, 201) * gVW + time * (7.0f + i * 3.0f), gVW + 40.0f) - 20.0f;
            const float y = horizonY * (0.18f + 0.4f * Hash01(i, 202)) + 3.0f * sinf(time * 0.8f + i);
            const float flap = 2.0f + 1.5f * sinf(time * 5.0f + i * 2.0f);
            const Color c = Color{ 255, 255, 255, (unsigned char)(220 * a) };
            DrawLineEx(Vector2{ x - 5, y - flap }, Vector2{ x, y }, 1.0f, c);
            DrawLineEx(Vector2{ x, y }, Vector2{ x + 5, y - flap }, 1.0f, c);
        }
        return;
    }
    const float snow = BiomeWeight(m, BIOME_SNOW) * 60.0f;
    for (int i = 0; i < (int)snow + 1; i++) {
        const float a = std::clamp(snow - i, 0.0f, 1.0f);
        if (a <= 0.0f) break;
        const float x = Wrap(Hash01(i, 211) * gVW + sinf(time * 0.8f + i) * 10.0f - time * 6.0f, (float)gVW);
        const float y = Wrap(Hash01(i, 212) * gVH + time * (22.0f + Hash01(i, 213) * 18.0f), (float)gVH);
        const int size = i % 5 == 0 ? 2 : 1;
        DrawRectangle((int)x, (int)y, size, size, Color{ 255, 255, 255, (unsigned char)(225 * a) });
    }
    const float petals = BiomeWeight(m, BIOME_BLOSSOM) * 22.0f;
    for (int i = 0; i < (int)petals + 1; i++) {
        const float a = std::clamp(petals - i, 0.0f, 1.0f);
        if (a <= 0.0f) break;
        const float x = Wrap(Hash01(i, 221) * gVW - time * (12.0f + Hash01(i, 222) * 10.0f) + sinf(time * 1.2f + i) * 8.0f, (float)gVW);
        const float y = Wrap(Hash01(i, 223) * gVH + time * (14.0f + Hash01(i, 224) * 12.0f), gVH * 0.86f);
        Poly2(x, y, 2.4f, 1.3f, 4, time * 2.6f + i, Color{ 255, (unsigned char)(190 + (i % 3) * 22), (unsigned char)(210 + (i % 2) * 26), (unsigned char)(215 * a) });
    }
    static const Color kLeaves[3] = { { 244, 150, 70, 255 }, { 226, 96, 76, 255 }, { 250, 204, 96, 255 } };
    const float leaves = BiomeWeight(m, BIOME_AUTUMN) * 18.0f;
    for (int i = 0; i < (int)leaves + 1; i++) {
        const float a = std::clamp(leaves - i, 0.0f, 1.0f);
        if (a <= 0.0f) break;
        const float x = Wrap(Hash01(i, 231) * gVW - time * (9.0f + Hash01(i, 232) * 9.0f) + sinf(time * 1.6f + i) * 14.0f, (float)gVW);
        const float y = Wrap(Hash01(i, 233) * gVH + time * (12.0f + Hash01(i, 234) * 10.0f), gVH * 0.86f);
        Poly2(x, y, 3.0f, 1.8f, 4, time * 2.0f + i * 1.3f, Fade(kLeaves[i % 3], 0.9f * a));
    }
}

// What a hit looks like from the screen: streaks flying out from where it
// landed, and the edges of the frame glowing in its colour.
static void DrawHitOverlay()
{
    for (const Impact& hit : gImpacts) {
        if (hit.age >= 0.18f) continue;
        const Vector2 p = GetWorldToScreenEx(hit.pos, gCamera, gVW, gVH);
        const float k = hit.age / 0.18f, reach = hit.judge == J_PERFECT ? 1.0f : 0.65f;
        const int rays = hit.judge == J_PERFECT ? 12 : 8;
        const Color c = ToColor(Mix(kActionColor[hit.action], Vector3{ 1, 1, 1 }, 0.55f), 1.0f - k);
        for (int i = 0; i < rays; i++) {
            const float a = i * 2 * kPi / rays + hit.action * 0.4f;
            const float r0 = (10.0f + 30.0f * k) * reach, r1 = r0 + (14.0f * (1.0f - k) + 3.0f) * reach;
            const Vector2 dir = { cosf(a), sinf(a) }, across = { -sinf(a) * 1.2f, cosf(a) * 1.2f };
            DrawTriangle(Vector2{ p.x + dir.x * r0 + across.x, p.y + dir.y * r0 + across.y },
                         Vector2{ p.x + dir.x * r0 - across.x, p.y + dir.y * r0 - across.y },
                         Vector2{ p.x + dir.x * r1, p.y + dir.y * r1 }, c);
        }
    }
    if (gEdgePower > 0.02f) {
        const Color c = ToColor(gEdgeColour, 0.32f * gEdgePower * (0.35f + 0.65f * MotionScale())), none = ToColor(gEdgeColour, 0.0f);
        const int w = gVW / 9, h = gVH / 7;
        DrawRectangleGradientH(0, 0, w, gVH, c, none);
        DrawRectangleGradientH(gVW - w, 0, w, gVH, none, c);
        DrawRectangleGradientV(0, 0, gVW, h, c, none);
        DrawRectangleGradientV(0, gVH - h, gVW, h, none, c);
    }
}

static void RenderWorld(double t)
{
    const Scene& s = gScene;
    gPalette = PaletteAt(s.day);
    gHaloCount = 0;

    // The camera rests a quarter of the way ahead of the cat. On top of that
    // come the hit springs, a breath on every kick drum, and a slow drift that
    // grows with the streak.
    const float aspect = (float)gVW / (float)gVH, tanHalf = 0.3057f;   // tan(34 deg / 2)
    const float wide = std::clamp(4.3f / (tanHalf * aspect), 9.8f, 24.0f);
    const float dist = gCloseup ? 4.2f : wide + (5.2f - wide) * gPreview;
    const float reach = dist / 9.8f;
    gViewHalf = dist * tanHalf * aspect;
    gViewCentre = s.worldX + (gCloseup ? 0.2f : gViewHalf * 0.47f);
    const float motion = MotionScale();
    const float driftX = 0.3f * gJoy * motion * sinf(s.time * 0.7f), driftY = 0.1f * gJoy * motion * sinf(s.time * 1.1f);
    const float camX = gViewCentre + (gRig.x + driftX) * reach, baseY = s.camY + (gCloseup ? 0.8f : 1.3f - 0.5f * gPreview);
    gCamera.position = Vector3{ camX, baseY + 1.3f * reach + (gRig.y + driftY) * reach, dist };
    gCamera.target = Vector3{ gViewCentre + gRig.x * 0.5f * reach, baseY + gRig.y * 0.5f * reach, 0.0f };
    gCamera.up = Vector3{ sinf(gRig.roll), cosf(gRig.roll), 0 };
    gCamera.fovy = 34.0f - gRig.zoom - 0.45f * motion * s.beat * (0.3f + 0.7f * gJoy);
    gCamera.projection = CAMERA_PERSPECTIVE;

    // The time of day shades the ground; the place decides its colour and tints the sky.
    // Only part of the palette's colour cast is kept, so pale ground (snow, sand) darkens at night without turning violet.
    auto shadeOf = [](Vector3 now, Vector3 day) {
        const Vector3 ratio = Vector3Divide(now, day);
        const float lum = 0.3f * ratio.x + 0.59f * ratio.y + 0.11f * ratio.z;
        return Mix(Vector3{ lum, lum, lum }, ratio, 0.45f);
    };
    gShadeGrass = shadeOf(gPalette.grass, Rgb(140, 196, 110));
    gShadePath = shadeOf(gPalette.path, Rgb(232, 206, 160));
    {
        const BiomeMix m = BiomeAt(gViewCentre);
        const BiomeLook a = LookOf(m.a), b = LookOf(m.b);
        const Vector3 hue = Mix(a.sky, b.sky, m.t);
        const float amount = a.skyMix + (b.skyMix - a.skyMix) * m.t;
        gPalette.skyTop = TintTowards(gPalette.skyTop, hue, amount * 0.6f);
        gPalette.skyBottom = TintTowards(gPalette.skyBottom, hue, amount);
        GroundColours(gViewCentre, gPalette.grass, gPalette.path);   // the far hills take the local colour
    }

    gLight.camera = gCamera.position;
    gLight.lightDir = Vector3Normalize(Vector3{ 0.4f, 0.8f, 0.5f });   // from ahead of the cat, where the sun is drawn, so its face is lit
    gLight.sun = gPalette.sun;
    gLight.ambient = Vector3Scale(gPalette.ambient, 1.0f + 0.12f * gJoy);
    gLight.saturation = 1.0f + 0.35f * gJoy;
    {   // The sky gets richer along with everything else.
        Vector3* sky[2] = { &gPalette.skyTop, &gPalette.skyBottom };
        for (Vector3* c : sky) {
            const float lum = 0.3f * c->x + 0.59f * c->y + 0.11f * c->z;
            *c = Vector3{ lum + (c->x - lum) * gLight.saturation, lum + (c->y - lum) * gLight.saturation, lum + (c->z - lum) * gLight.saturation };
        }
    }
    gLight.fog = Mix(gPalette.skyBottom, gPalette.skyTop, 0.12f);
    gLight.fogStart = dist + 6.0f;
    gLight.fogEnd = dist + 30.0f;

    // Where the far edge of the ground meets the sky on the canvas.
    Camera3D level = gCamera;
    level.up = Vector3{ 0, 1, 0 };
    const float horizonY = GetWorldToScreenEx(Vector3{ gCamera.target.x, s.camY + 2.3f, -24.0f }, level, gVW, gVH).y;
    ClearBackground(BLACK);
    DrawSky(horizonY);
    PlaceParticles(horizonY, false);

    BeginMode3D(gCamera);
    rlBegin(RL_TRIANGLES);
    DrawTerrain();
    DrawScenery();
    DrawNotes(t);
    DrawCat(s.worldX, gState != ST_PAUSED && gState != ST_COUNTDOWN);
    rlEnd();
    rlDrawRenderBatchActive();
    rlDisableDepthMask();
    rlBegin(RL_TRIANGLES);
    DrawSparksAndShadows();
    rlEnd();
    rlDrawRenderBatchActive();
    rlEnableDepthMask();
    EndMode3D();

    PlaceParticles(horizonY, true);
    JoyParticles(horizonY, true);
    DrawHitOverlay();
    if (gJoyKick > 0.02f) DrawRectangle(0, 0, gVW, gVH, Color{ 255, 244, 210, (unsigned char)(26.0f * gJoyKick) });   // a soft warm glow, not a flash
}

// ---------------------------------------------------------------------------
// UI (drawn on the low-resolution canvas with raylib's built-in pixel font)
// ---------------------------------------------------------------------------
// Two looks: light text with a shadow for things that sit directly on the
// world (title, HUD), and cream paper cards with cocoa text for panels.

static const Color kInk       = { 255, 246, 230, 255 };
static const Color kSoft      = { 255, 246, 230, 170 };
static const Color kShade     = { 30, 22, 44, 150 };
static const Color kPaper     = { 255, 247, 233, 244 };
static const Color kPaperDeep = { 247, 232, 210, 255 };
static const Color kLine      = { 206, 172, 148, 255 };
static const Color kCocoa     = { 92, 64, 72, 255 };
static const Color kCocoaSoft = { 92, 64, 72, 150 };
static const Color kCoral     = { 240, 128, 144, 255 };
static const Color kCoralHot  = { 248, 152, 164, 255 };
static const Color kCoralLine = { 204, 96, 116, 255 };
static const Color kPeach     = { 252, 204, 150, 255 };
static const Color kPeachLine = { 226, 160, 104, 255 };
static const Color kCream     = { 255, 252, 246, 255 };

static const char* kRankLetter[5] = { "S", "A", "B", "C", "D" };
static const Color kRankColour[5] = { { 244, 178, 60, 255 }, { 92, 188, 140, 255 }, { 96, 168, 226, 255 }, { 160, 132, 220, 255 }, { 226, 122, 142, 255 } };
// align: 0 left, 1 centre, 2 right
static void Text(const char* text, int x, int y, int size, Color color, int align = 1, bool shadow = true)
{
    const int w = MeasureText(text, size);
    if (align == 1) x -= w / 2;
    else if (align == 2) x -= w;
    if (shadow) DrawText(text, x + size / 10, y + size / 10, size, Color{ 30, 22, 44, (unsigned char)(color.a * 0.6f) });
    DrawText(text, x, y, size, color);
}

static Vector2 Mouse()
{
    const Vector2 m = GetMousePosition();
    return Vector2{ m.x / gPixel, m.y / gPixel };
}

// A rectangle with its corner pixels cut, an outline and a soft drop shadow.
static void Card(int x, int y, int w, int h, Color fill, Color line, bool shadow = true)
{
    if (shadow) DrawRectangle(x + 2, y + 3, w, h - 1, Color{ 60, 36, 56, 60 });
    DrawRectangle(x + 1, y, w - 2, h, line);
    DrawRectangle(x, y + 1, w, h - 2, line);
    DrawRectangle(x + 1, y + 1, w - 2, h - 2, fill);
}

// style: 0 quiet, 1 primary, 2 selected. Buttons lift under the pointer and sink when pressed.
static bool Button(int x, int y, int w, int h, const char* label, int style = 0)
{
    const Rectangle r = { (float)x, (float)y, (float)w, (float)h };
    const bool hot = CheckCollisionPointRec(Mouse(), r), down = hot && IsMouseButtonDown(MOUSE_BUTTON_LEFT);
    Color fill = hot ? Color{ 255, 240, 222, 255 } : kPaperDeep, line = kLine, text = kCocoa;
    if (style == 1) { fill = hot ? kCoralHot : kCoral; line = kCoralLine; text = kCream; }
    else if (style == 2) { fill = kPeach; line = kPeachLine; }
    const int lift = down ? 1 : hot ? -1 : 0;
    Card(x, y + lift, w, h, fill, line, !down);
    Text(label, x + w / 2, y + lift + (h - 10) / 2, 10, text, 1, false);
    const bool clicked = hot && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && gState == gFrameState;
    if (clicked) PlaySfx(SFX_CLICK);
    return clicked;
}

static void Panel(int w, int h, int& left, int& top, bool right = false)
{
    left = right && gVW >= w + 260 ? gVW - w - 14 : (gVW - w) / 2;
    top = (gVH - h) / 2;
    Card(left, top, w, h, kPaper, kLine);
}

// Shortens `text` with an ellipsis until it fits `maxWidth`.
static std::string Fit(const std::string& text, int size, int maxWidth)
{
    if (MeasureText(text.c_str(), size) <= maxWidth) return text;
    std::string s = text;
    while (s.size() > 1) {
        s.pop_back();
        while (!s.empty() && ((unsigned char)s.back() & 0xC0) == 0x80) s.pop_back();   // do not split UTF-8
        if (MeasureText((s + "...").c_str(), size) <= maxWidth) break;
    }
    return s + "...";
}

static void Paw2(float cx, float cy, float size, Color c)
{
    Poly2(cx, cy + size * 0.25f, size * 0.62f, size * 0.5f, 8, 0, c);
    Poly2(cx - size * 0.62f, cy - size * 0.4f, size * 0.26f, size * 0.3f, 6, 0, c);
    Poly2(cx, cy - size * 0.68f, size * 0.26f, size * 0.3f, 6, 0, c);
    Poly2(cx + size * 0.62f, cy - size * 0.4f, size * 0.26f, size * 0.3f, 6, 0, c);
}

// Small picture of what each action answers, used in the key legend.
static void ActionIcon(int action, int cx, int cy)
{
    const Color c = ToColor(kActionColor[action]);
    const float x = (float)cx, y = (float)cy;
    switch (action) {
        case ACT_HOP:
            DrawRectangle(cx - 7, cy - 3, 14, 6, c);
            DrawRectangle(cx - 7, cy - 3, 3, 6, Color{ 255, 226, 180, 255 });
            break;
        case ACT_BAT:
            Poly2(x, y, 6, 6, 8, 0.3f, c);
            DrawLine(cx - 4, cy - 1, cx + 4, cy + 2, Color{ 255, 200, 210, 255 });
            break;
        case ACT_SWAT:
            DrawTriangle(Vector2{ x, y }, Vector2{ x - 7, y + 5 }, Vector2{ x - 6, y - 6 }, c);
            DrawTriangle(Vector2{ x, y }, Vector2{ x + 6, y - 6 }, Vector2{ x + 7, y + 5 }, c);
            break;
        case ACT_RING:
            DrawTriangle(Vector2{ x, y - 7 }, Vector2{ x - 6, y + 4 }, Vector2{ x + 6, y + 4 }, c);
            DrawRectangle(cx - 1, cy + 4, 3, 3, Color{ 190, 140, 60, 255 });
            break;
    }
}

static void OpenSongDialog()
{
    std::string path;
    if (PlatformOpenAudioDialog(GetWindowHandle(), path)) BeginLoad(path, false);
}

// The title: each letter bobs on its own, and the O has cat ears.
static void DrawTitle(int cx, int y, int size)
{
    static const char* kLetters[6] = { "O", "N", "I", "R", "I", "A" };
    const int gap = size / 10 + 2;
    int widths[6], total = 0;
    for (int i = 0; i < 6; i++) { widths[i] = MeasureText(kLetters[i], size); total += widths[i] + (i ? gap : 0); }
    const float u = size / 50.0f;
    int x = cx - total / 2;
    for (int i = 0; i < 6; i++) {
        const int ly = y + (int)lroundf(3.0f * sinf(gScene.time * 2.2f - i * 0.7f)) - (int)(3.0f * gScene.beat);
        if (i == 0) {
            for (int side = 0; side < 2; side++) {
                const float edge = side ? (float)(x + widths[0]) : (float)x, in = side ? -1.0f : 1.0f, top = (float)ly;
                const Vector2 a = { edge + in * 1 * u, top + 9 * u }, b = { edge + in * 5 * u, top - 13 * u }, c = { edge + in * 17 * u, top + 5 * u };
                DrawTriangle(Vector2{ a.x - in * 3, a.y + 2 }, Vector2{ b.x - in * 1, b.y - 4 }, Vector2{ c.x + in * 4, c.y + 1 }, kCocoa);
                DrawTriangle(a, b, c, kInk);
                DrawTriangle(Vector2{ a.x + in * 4 * u, a.y - 2 * u }, Vector2{ b.x + in * 1 * u, b.y + 7 * u }, Vector2{ c.x - in * 4 * u, c.y - 1 * u }, Color{ 250, 170, 180, 255 });
            }
        }
        for (int ox = -2; ox <= 2; ox++)
            for (int oy = -2; oy <= 3; oy++)
                if (ox || oy) DrawText(kLetters[i], x + ox, ly + oy, size, kCocoa);
        DrawText(kLetters[i], x, ly, size, kInk);
        x += widths[i] + gap;
    }
}

static void PickSong(int index)
{
    if (index < 0 || index >= (int)gLibrary.size()) return;
    const SongEntry entry = gLibrary[index];   // a copy: loading reorders the library
    if (entry.path == kDemoKey) BeginLoad("", true);
    else BeginLoad(entry.path, false);
}

static State gSettingsFrom = ST_MENU;   // where the settings screen returns to
static int   gWardrobeTab = SLOT_HAT, gWardrobeSel = -1, gSettingsTab = 0;
static float gResetArmedUntil = -1.0f;

static void OpenSettings()
{
    gSettingsFrom = gState;
    gState = ST_SETTINGS;
}

// True while something has been unlocked that the player has not looked at yet.
static bool HasNewItems()
{
    for (int i = 0; i < kItemCount; i++)
        if (kItems[i].need != NEED_NONE && Unlocked(kItems[i]) && !((gProgress.known >> i) & 1)) return true;
    return false;
}

// Title on the left over the strolling cat; on the right, a shelf with every song that has been opened.
static void DrawMenu()
{
    const int shelfW = 240, shelfX = gVW - shelfW - 12, shelfY = 16, shelfH = gVH - 32;
    const int leftCx = shelfX / 2, titleSize = shelfX >= 330 ? 50 : 40;
    DrawTitle(leftCx, 34, titleSize);
    Text("a cozy walk through your music", leftCx, 40 + titleSize, 10, kInk);
    if (!gError.empty()) {
        const std::string msg = Fit(gError, 10, shelfX - 24);
        const int w = MeasureText(msg.c_str(), 10) + 16;
        Card(leftCx - w / 2, 58 + titleSize, w, 18, kPaper, kCoralLine);
        Text(msg.c_str(), leftCx, 62 + titleSize, 10, kCoralLine, 1, false);
    }

    // Dress the cat, or change how the game behaves.
    const int by0 = gVH - 62;
    if (Button(leftCx - 100, by0, 96, 22, "Wardrobe") || Key(KEY_C)) { gState = ST_WARDROBE; gWardrobeSel = -1; }
    if (HasNewItems()) {   // something new is waiting in there
        Poly2((float)leftCx - 8, (float)by0 + 1, 4.5f, 4.5f, 8, 0, kCoralLine);
        Poly2((float)leftCx - 8, (float)by0 + 1, 3.5f, 3.5f, 8, 0, kCoral);
    }
    if (Button(leftCx + 4, by0, 96, 22, "Settings") || Key(KEY_S)) OpenSettings();
    Text(TextFormat("now passing: %s, %s", kBiomeName[BiomeNear(gScene.worldX)], kVariantName[BiomeNear(gScene.worldX)][VariantNear(gScene.worldX)]), leftCx, gVH - 30, 10, kInk);
    Text("Esc pauses  -  F11 fullscreen", leftCx, gVH - 16, 10, kSoft);
    if (gState != ST_MENU) return;

    Card(shelfX, shelfY, shelfW, shelfH, kPaper, kLine);
    Paw2((float)shelfX + 19, (float)shelfY + 20, 6, kCoral);
    Text("Your songs", shelfX + 32, shelfY + 9, 20, kCocoa, 0, false);

    const int count = (int)gLibrary.size(), rowH = 24, footer = 62;
    const int listX = shelfX + 10, listY = shelfY + 38, rowW = shelfW - 22;
    const int visible = std::max(1, (shelfH - 38 - footer) / rowH);

    if (count > 0) {
        bool moved = false;
        if (Key(KEY_DOWN)) { gMenuSel++; moved = true; }
        if (Key(KEY_UP)) { gMenuSel--; moved = true; }
        gMenuSel = std::clamp(gMenuSel, 0, count - 1);
        if (moved) {
            PlaySfx(SFX_CLICK, 5.0f, 0.7f);
            if (gMenuSel < gMenuScroll) gMenuScroll = gMenuSel;
            if (gMenuSel >= gMenuScroll + visible) gMenuScroll = gMenuSel - visible + 1;
        }
    }
    const float wheel = gSim ? 0.0f : GetMouseWheelMove();
    if (wheel > 0.0f) gMenuScroll--;
    if (wheel < 0.0f) gMenuScroll++;
    gMenuScroll = std::clamp(gMenuScroll, 0, std::max(0, count - visible));

    int pick = -1;
    for (int row = 0; row < visible && gMenuScroll + row < count; row++) {
        const int i = gMenuScroll + row, y = listY + row * rowH;
        const SongEntry& e = gLibrary[i];
        const Rectangle r = { (float)listX, (float)y, (float)rowW, (float)(rowH - 2) };
        const bool hot = CheckCollisionPointRec(Mouse(), r), selected = i == gMenuSel;
        if (selected) Card(listX, y, rowW, rowH - 2, kPeach, kPeachLine, false);
        else if (hot) Card(listX, y, rowW, rowH - 2, kPaperDeep, kLine, false);

        // Best result on any difficulty, or a little note for songs not finished yet.
        const float best = std::max(e.best[0], std::max(e.best[1], e.best[2]));
        if (best >= 0.0f) {
            const int rank = RankOf(best);
            Card(listX + 4, y + 3, 16, 16, kRankColour[rank], Fade(kCocoa, 0.35f), false);
            Text(kRankLetter[rank], listX + 12, y + 6, 10, kCream, 1, false);
        } else {
            Poly2((float)listX + 10, (float)y + 14, 3.0f, 2.5f, 6, 0, kCoral);
            DrawRectangle(listX + 12, y + 5, 1, 9, kCoral);
            DrawRectangle(listX + 12, y + 5, 4, 2, kCoral);
        }
        const int secs = (int)e.duration;
        Text(TextFormat("%d:%02d", secs / 60, secs % 60), listX + rowW - 6, y + 6, 10, kCocoaSoft, 2, false);
        Text(Fit(e.name, 10, rowW - 30 - 40).c_str(), listX + 26, y + 6, 10, kCocoa, 0, false);
        if (hot && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && gState == gFrameState) { gMenuSel = i; pick = i; PlaySfx(SFX_CLICK); }
    }
    if (count > visible) {   // scroll bar
        const int trackH = visible * rowH - 2, thumbH = std::max(8, trackH * visible / count);
        const int thumbY = listY + (trackH - thumbH) * gMenuScroll / std::max(1, count - visible);
        DrawRectangle(shelfX + shelfW - 9, listY, 2, trackH, Fade(kLine, 0.5f));
        DrawRectangle(shelfX + shelfW - 9, thumbY, 2, thumbH, kCoral);
    }

    const int by = shelfY + shelfH - footer + 8;
    if (Button(shelfX + 14, by, shelfW - 28, 24, "+ Add a song", 1) || Key(KEY_O)) OpenSongDialog();
    Text("or drop a file on the window", shelfX + shelfW / 2, by + 34, 10, kCocoaSoft, 1, false);

    if (gState != ST_MENU) return;   // the dialog started a load
    if (Key(KEY_ENTER) || Key(KEY_SPACE)) pick = gMenuSel;
    if (Key(KEY_DELETE) && count > 0 && gLibrary[gMenuSel].path != kDemoKey) {
        gLibrary.erase(gLibrary.begin() + gMenuSel);   // forgets the song; the file itself is untouched
        SaveLibrary();
        return;
    }
    PickSong(pick);
}

static void DrawLoading()
{
    const int cx = gVW / 2, y = gVH / 2 - 34;
    Card(cx - 124, y, 248, 70, kPaper, kLine);
    for (int i = 0; i < 3; i++) {
        const float k = 0.5f + 0.5f * sinf(gScene.time * 6.0f - i * 1.1f);
        Paw2((float)cx - 22 + i * 22, (float)y + 20 - 3.0f * k, 5.0f, Fade(kCoral, 0.35f + 0.65f * k));
    }
    Text(gJob.stage.load() >= 2 ? "Listening to the music..." : "Opening the song...", cx, y + 38, 10, kCocoa, 1, false);
    Text(Fit(gJob.name, 10, 224).c_str(), cx, y + 52, 10, kCocoaSoft, 1, false);
}

static bool Stepper(int left, int y, int w, const char* label, const char* value, int& delta, int valueW = 50)
{
    delta = 0;
    Text(label, left, y + 4, 10, kCocoa, 0, false);
    const int right = left + w;
    if (Button(right - 18, y, 18, 18, "+")) delta = 1;
    if (Button(right - 36 - valueW, y, 18, 18, "-")) delta = -1;
    Text(value, right - 18 - valueW / 2, y + 4, 10, kCocoa, 1, false);
    return delta != 0;
}

// Small controls shared by the settings screen.

// A row of pips for a value from 0 to `count`. Clicking the first lit pip again turns it off.
static bool Pips(int right, int y, int count, int& value)
{
    bool changed = false;
    const int w = 8, gap = 2, x0 = right - count * (w + gap) + gap;
    for (int i = 0; i < count; i++) {
        const int x = x0 + i * (w + gap);
        const bool hot = CheckCollisionPointRec(Mouse(), Rectangle{ (float)x - 1, (float)y - 2, (float)w + 2, 16 }), on = i < value;
        Card(x, y + (hot ? -1 : 0), w, 12, on ? kCoral : kPaperDeep, on ? kCoralLine : kLine, false);
        if (hot && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && gState == gFrameState) {
            value = (value == 1 && i == 0) ? 0 : i + 1;
            changed = true;
        }
    }
    if (changed) PlaySfx(SFX_CLICK, (float)value);
    return changed;
}

// An on/off switch with a sliding knob.
static bool Switch(int right, int y, bool on)
{
    const int w = 28, h = 14, x = right - w;
    const bool hot = CheckCollisionPointRec(Mouse(), Rectangle{ (float)x, (float)y, (float)w, (float)h });
    Card(x, y, w, h, on ? (hot ? kCoralHot : kCoral) : kPaperDeep, on ? kCoralLine : kLine, false);
    Card(on ? x + w - 13 : x + 1, y + 1, 12, 12, kCream, on ? kCoralLine : kLine, false);
    const bool clicked = hot && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && gState == gFrameState;
    if (clicked) PlaySfx(SFX_CLICK, on ? 0.0f : 4.0f);
    return clicked;
}

// A few choices side by side; the chosen one is highlighted.
static bool Choice(int right, int y, const char* const* labels, int count, int& value, int cell = 50)
{
    bool changed = false;
    for (int i = 0; i < count; i++)
        if (Button(right - (count - i) * (cell + 2) + 2, y, cell, 18, labels[i], value == i ? 2 : 0) && value != i) { value = i; changed = true; }
    return changed;
}

static void Lock2(int x, int y, Color c)
{
    DrawRectangleLines(x + 1, y, 5, 6, c);
    DrawRectangle(x, y + 4, 7, 6, c);
}

static void Gear2(float cx, float cy, Color c)
{
    for (int i = 0; i < 6; i++) Poly2(cx + cosf(i * kPi / 3) * 5.5f, cy + sinf(i * kPi / 3) * 5.5f, 2.2f, 2.2f, 4, i * kPi / 3, c);
    Poly2(cx, cy, 5.5f, 5.5f, 10, 0, c);
    Poly2(cx, cy, 2.2f, 2.2f, 8, 0, kPaper);
}

static void DrawReady()
{
    int left, top;
    const int pw = 300, ph = 244;
    Panel(pw, ph, left, top, true);
    const int cx = left + pw / 2, x0 = left + 16, inner = pw - 32;

    Text(Fit(gSongName, 20, inner).c_str(), cx, top + 12, 20, kCocoa, 1, false);
    const int secs = (int)gSong.duration;
    Text(TextFormat("%d:%02d  -  %d things on the path", secs / 60, secs % 60, (int)gSong.charts[gCfg.diff].time.size()), cx, top + 38, 10, kCocoaSoft, 1, false);
    const SongEntry* entry = FindSong(gSongPath);
    if (entry && entry->best[gCfg.diff] >= 0.0f) {
        const int rank = RankOf(entry->best[gCfg.diff]);
        Text(TextFormat("best on %s:  %s  %.1f%%", kDiffName[gCfg.diff], kRankLetter[rank], entry->best[gCfg.diff] * 100.0f), cx, top + 51, 10, kRankColour[rank], 1, false);
    } else {
        Text(TextFormat("not played yet on %s", kDiffName[gCfg.diff]), cx, top + 51, 10, kCocoaSoft, 1, false);
    }

    const int dw = (inner - 8) / 3;
    for (int d = 0; d < DIFF_COUNT; d++)
        if (Button(x0 + d * (dw + 4), top + 68, dw, 22, kDiffName[d], gCfg.diff == d ? 2 : 0)) gCfg.diff = d;
    if (Key(KEY_LEFT)) gCfg.diff = std::max(0, gCfg.diff - 1);
    if (Key(KEY_RIGHT)) gCfg.diff = std::min(DIFF_COUNT - 1, gCfg.diff + 1);

    // What this difficulty asks of the player.
    const int actions = kActionCount[gCfg.diff], slot = 66;
    for (int a = 0; a < actions; a++) {
        const int x = cx - actions * slot / 2 + a * slot;
        ActionIcon(a, x + 10, top + 106);
        Text(TextFormat("%s %s", kActionKey[a], kActionName[a]), x + 22, top + 102, 10, kCocoa, 0, false);
    }

    int delta;
    if (Stepper(x0, top + 124, inner, "Scenery", gCfg.scenery == 0 ? "Journey" : kBiomeName[gCfg.scenery - 1], delta, 112))
        gCfg.scenery = (gCfg.scenery + delta + BIOME_COUNT + 1) % (BIOME_COUNT + 1);

    const int tw = (inner - 4) / 2;
    if (Button(x0, top + 152, tw, 20, gCfg.watch ? "Watch mode: on" : "Watch mode: off", gCfg.watch ? 2 : 0)) gCfg.watch = !gCfg.watch;
    if (Button(x0 + tw + 4, top + 152, tw, 20, "Settings")) OpenSettings();

    if (gState != ST_READY) return;
    if (Button(cx - 70, top + 184, 140, 26, "Play", 1) || Key(KEY_ENTER) || Key(KEY_SPACE)) {
        SaveSettings();
        StartGame();
    } else if (Button(cx - 70, top + 216, 140, 18, "Back to your songs") || Key(KEY_ESCAPE)) {
        gState = ST_MENU;
    }
}

static void DrawHud(double t)
{
    const int cx = gVW / 2;

    // Progress: a little cat face walking along a line.
    const int lineW = gVW / 3, lineX = cx - lineW / 2, lineY = 14;
    DrawRectangle(lineX, lineY, lineW, 1, kSoft);
    const int px = lineX + (int)(lineW * std::clamp(t / gSong.duration, 0.0, 1.0));
    const float fx = (float)px, fy = (float)lineY;
    DrawTriangle(Vector2{ fx - 4, fy - 2 }, Vector2{ fx - 1, fy - 2 }, Vector2{ fx - 3, fy - 6 }, kInk);
    DrawTriangle(Vector2{ fx + 1, fy - 2 }, Vector2{ fx + 4, fy - 2 }, Vector2{ fx + 3, fy - 6 }, kInk);
    DrawRectangle(px - 4, lineY - 3, 8, 6, kInk);

    Text(TextFormat("%06d", gScore), 10, 8, 20, kInk, 0);

    // Mood: five hearts that fill as the streak grows.
    for (int i = 0; i < 5; i++) {
        const float fill = std::clamp(gJoy * 5.0f - i, 0.0f, 1.0f), cx0 = 17.0f + i * 14.0f, cy0 = 38.0f;
        Heart2(cx0, cy0, 4.5f, Color{ 30, 22, 44, 110 });
        if (fill > 0.05f) Heart2(cx0, cy0 - (i == (int)(gJoy * 5.0f) ? 1.5f * gJoyKick : 0.0f), 4.5f * fill, Color{ 255, 130, 160, 255 });
    }

    // Streak milestones get a banner.
    if (gMilestoneAge < 1.6f) {
        const char* word = gMilestone >= 100 ? "MEOWGNIFICENT!" : gMilestone >= 50 ? "PAWSOME!" : gMilestone >= 25 ? "LOVELY!" : "CUTE!";
        static const Color kCheer[3] = { { 255, 214, 230, 255 }, { 255, 240, 170, 255 }, { 200, 232, 255, 255 } };
        const float in = std::min(1.0f, gMilestoneAge * 6.0f), out = std::clamp((1.6f - gMilestoneAge) / 0.4f, 0.0f, 1.0f);
        Text(TextFormat("x%d  %s", gMilestone, word), cx, 44 - (int)(8.0f * sinf(in * kPi)), 20, Fade(kCheer[(int)(gMilestoneAge * 8.0f) % 3], out));
    }
    Text(TextFormat("%.1f%%", Accuracy() * 100.0f), gVW - 10, 8, 10, kInk, 2);
    if (gCombo >= 3) Text(TextFormat("x%d", gCombo), gVW - 10, 22, gComboPop > 0.3f ? 20 : 10, kInk, 2);

    // Key hints above the next objects.
    if (gCfg.hints && gChart && gState == ST_PLAYING) {
        const int total = (int)gChart->time.size();
        for (int i = gHead; i < total; i++) {
            if (gNoteState[i]) continue;
            Vector3 at = NotePosition(i);
            if (at.x > gViewCentre + gViewHalf + 0.5f) break;
            at.y += gNoteAction[i] == ACT_RING ? 1.0f : 0.45f;
            const Vector2 p = GetWorldToScreenEx(at, gCamera, gVW, gVH);
            const int x = (int)p.x, y = (int)p.y - 14;
            DrawRectangle(x - 6, y - 1, 12, 12, kShade);
            DrawRectangleLines(x - 6, y - 1, 12, 12, ToColor(kActionColor[gNoteAction[i]]));
            Text(kActionKey[gNoteAction[i]], x + 1, y + 1, 10, kInk, 1, false);
        }
    }

    // Key legend along the bottom; a key lights up while it is held.
    const int actions = kActionCount[gCfg.diff], slot = 46, y = gVH - 26;
    for (int a = 0; a < actions; a++) {
        const int x = cx - actions * slot / 2 + a * slot;
        const float g = gScene.glow[a];
        DrawRectangle(x + 2, y, slot - 4, 20, Color{ 30, 22, 44, (unsigned char)(150 + 60 * g) });
        DrawRectangleLines(x + 2, y, slot - 4, 20, ToColor(kActionColor[a], 0.45f + 0.55f * g));
        ActionIcon(a, x + 14, y + 10);
        Text(kActionKey[a], x + 32, y + 5, 10, kInk, 1, false);
    }

    if (gJudgeKind >= 0 && gJudgeAge < 0.5f) {
        static const char* kNames[4] = { "PURRFECT", "NICE", "OK", "MISS" };
        static const Color kColors[4] = { { 255, 236, 150, 255 }, { 170, 236, 255, 255 }, { 190, 255, 200, 255 }, { 255, 160, 170, 255 } };
        const Vector2 p = GetWorldToScreenEx(Vector3{ gScene.worldX + 0.3f, PathHeight(gScene.worldX) + 2.3f, 0 }, gCamera, gVW, gVH);
        const float k = gJudgeAge / 0.5f;
        const int size = gJudgeKind == J_PERFECT && gJudgeAge < 0.12f ? 20 : 10;   // a perfect pops big for a moment
        Text(kNames[gJudgeKind], (int)p.x, (int)(p.y - 14 * k) - (size - 10) / 2, size, Fade(kColors[gJudgeKind], 1.0f - k * k));
    }
    if (gCfg.watch) Text("WATCH MODE", cx, 24, 10, kSoft);
}

static void DrawPause()
{
    const int cx = gVW / 2;
    int left, top;
    Panel(190, 150, left, top);
    Text("Paused", cx, top + 14, 20, kCocoa, 1, false);
    if (Button(cx - 65, top + 48, 130, 26, "Resume", 1) || Key(KEY_ESCAPE) || Key(KEY_ENTER)) {
        gCountdown = 3.0f;
        gState = ST_COUNTDOWN;
    } else if (Button(cx - 65, top + 82, 130, 20, "Restart") || Key(KEY_R)) {
        StartGame();
    } else if (Button(cx - 65, top + 108, 130, 20, "Quit to menu") || Key(KEY_Q)) {
        gState = ST_READY;
    }
}

static void DrawResults()
{
    int left, top;
    const int pw = 300, ph = 272;
    Panel(pw, ph, left, top, true);
    const int cx = left + pw / 2;

    const float acc = Accuracy();
    const int rank = RankOf(acc);
    static const char* kWords[5] = { "A purrfect evening", "A lovely walk", "A nice stroll", "A sleepy stroll", "More of a nap" };
    for (int ox = -2; ox <= 2; ox++)
        for (int oy = -2; oy <= 3; oy++)
            if (ox || oy) DrawText(kRankLetter[rank], cx - MeasureText(kRankLetter[rank], 50) / 2 + ox, top + 12 + oy, 50, kCocoa);
    Text(kRankLetter[rank], cx, top + 12, 50, kRankColour[rank], 1, false);
    if (gNewBest) {
        Card(left + pw - 82, top + 12, 68, 18, kCoral, kCoralLine, false);
        Text("new best!", left + pw - 48, top + 16, 10, kCream, 1, false);
    }
    Text(kWords[rank], cx, top + 66, 10, kCocoa, 1, false);
    Text(Fit(gSongName, 10, pw - 32).c_str(), cx, top + 80, 10, kCocoaSoft, 1, false);
    Text(TextFormat("%d", gScore), cx, top + 98, 20, kCocoa, 1, false);
    Text(TextFormat("%.1f%% in time  -  best streak %d", acc * 100.0f, gMaxCombo), cx, top + 124, 10, kCocoa, 1, false);

    static const char* kLabel[4] = { "Purrfect", "Nice", "Ok", "Miss" };
    const int colW = (pw - 32) / 4;
    for (int i = 0; i < 4; i++) {
        const int x = left + 16 + colW * i + colW / 2;
        Text(TextFormat("%d", gCount[i]), x, top + 146, 10, kCocoa, 1, false);
        Text(kLabel[i], x, top + 159, 10, kCocoaSoft, 1, false);
    }

    // What this walk unlocked, or failing that, the next thing within reach.
    if (!gNewUnlocks.empty()) {
        const int more = (int)gNewUnlocks.size() - 1;
        const char* line = more > 0 ? TextFormat("unlocked: %s  +%d more", kItems[gNewUnlocks[0]].name, more) : TextFormat("unlocked: %s", kItems[gNewUnlocks[0]].name);
        const int w = std::min(pw - 28, MeasureText(line, 10) + 34);
        Card(cx - w / 2, top + 178, w, 20, kPeach, kPeachLine, false);
        Paw2((float)(cx - w / 2 + 12), (float)top + 189, 4.5f, kCoral);
        Text(line, cx + 8, top + 183, 10, kCocoa, 1, false);
    } else {
        for (const Item& it : kItems) {
            if (Unlocked(it)) continue;
            Text(Fit(TextFormat("next: %s - %s", it.name, Requirement(it)), 10, pw - 28).c_str(), cx, top + 183, 10, kCocoaSoft, 1, false);
            break;
        }
    }

    if (Button(cx - 70, top + 208, 140, 24, gCfg.watch ? "Watch again" : "Walk again", 1) || Key(KEY_ENTER) || Key(KEY_R)) {
        StartGame();
    } else if (Button(cx - 70, top + 240, 140, 18, "Back") || Key(KEY_ESCAPE)) {
        gState = ST_READY;
    }
}

static void Equip(int slot, int n)
{
    int* field[SLOT_COUNT] = { &gCfg.cat, &gCfg.hat, &gCfg.neck, &gCfg.back };
    *field[slot] = n;
    BuildCat(gCfg.cat);
    CatDo(ACT_HOP);   // a little hop to show it off
    PlaySfx(SFX_TWINKLE, 0.0f, 0.8f);
}

// The wardrobe: the camera comes in close on the cat, and a panel lists the
// cats and everything they can wear, with what each locked item is waiting for.
static void DrawWardrobe()
{
    int left, top;
    const int pw = 300, ph = 306;
    Panel(pw, ph, left, top, true);
    const int x0 = left + 12, inner = pw - 24;
    const int equipped[SLOT_COUNT] = { gCfg.cat, gCfg.hat, gCfg.neck, gCfg.back };

    Paw2((float)left + 19, (float)top + 20, 6, kCoral);
    Text("Wardrobe", left + 32, top + 9, 20, kCocoa, 0, false);
    Text(TextFormat("%d of %d", UnlockedCount(), kItemCount), left + pw - 14, top + 14, 10, kCocoaSoft, 2, false);

    static const char* kTabs[SLOT_COUNT] = { "Cat", "Hat", "Neck", "Back" };
    const int tw = (inner - 9) / 4;
    for (int i = 0; i < SLOT_COUNT; i++)
        if (Button(x0 + i * (tw + 3), top + 36, tw, 20, kTabs[i], gWardrobeTab == i ? 2 : 0)) { gWardrobeTab = i; gWardrobeSel = -1; }
    if (Key(KEY_RIGHT)) { gWardrobeTab = (gWardrobeTab + 1) % SLOT_COUNT; gWardrobeSel = -1; }
    if (Key(KEY_LEFT)) { gWardrobeTab = (gWardrobeTab + SLOT_COUNT - 1) % SLOT_COUNT; gWardrobeSel = -1; }

    const int count = SlotSize(gWardrobeTab), rowH = 19, listY = top + 62;
    if (gWardrobeSel < 0) gWardrobeSel = equipped[gWardrobeTab];
    if (Key(KEY_DOWN)) gWardrobeSel = std::min(count - 1, gWardrobeSel + 1);
    if (Key(KEY_UP)) gWardrobeSel = std::max(0, gWardrobeSel - 1);

    int pick = Key(KEY_ENTER) || Key(KEY_SPACE) ? gWardrobeSel : -1;
    for (int n = 0; n < count; n++) {
        const int index = ItemIndex(gWardrobeTab, n), y = listY + n * rowH;
        const Item& it = kItems[index];
        const bool open = Unlocked(it), worn = equipped[gWardrobeTab] == n;
        const bool hot = CheckCollisionPointRec(Mouse(), Rectangle{ (float)x0, (float)y, (float)inner, (float)(rowH - 2) });
        if (worn) Card(x0, y, inner, rowH - 2, kPeach, kPeachLine, false);
        else if (hot || n == gWardrobeSel) Card(x0, y, inner, rowH - 2, kPaperDeep, kLine, false);

        if (worn) Paw2((float)x0 + 10, (float)y + 9, 4.5f, kCoral);
        else if (open) Poly2((float)x0 + 10, (float)y + 8, 3.0f, 3.0f, 8, 0, Fade(kLine, 0.9f));
        else Lock2(x0 + 7, y + 3, kCocoaSoft);

        const char* note = gWardrobeTab == SLOT_CAT ? kCats[n].note : nullptr;
        Text(it.name, x0 + 22, y + 4, 10, open ? kCocoa : kCocoaSoft, 0, false);
        if (!open) {
            Text(Requirement(it), x0 + inner - 6, y + 4, 10, kCocoaSoft, 2, false);
        } else if (it.need != NEED_NONE && !((gProgress.known >> index) & 1)) {
            Card(x0 + inner - 34, y + 2, 30, 13, kCoral, kCoralLine, false);
            Text("new", x0 + inner - 19, y + 3, 10, kCream, 1, false);
        } else if (note) {
            Text(note, x0 + inner - 6, y + 4, 10, kCocoaSoft, 2, false);
        }
        if (hot && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && gState == gFrameState) { gWardrobeSel = n; pick = n; }
    }
    if (pick >= 0) {
        if (Unlocked(kItems[ItemIndex(gWardrobeTab, pick)])) { if (equipped[gWardrobeTab] != pick) Equip(gWardrobeTab, pick); }
        else PlaySfx(SFX_MISS, 0.0f, 0.7f);
    }

    if (Button(left + pw - 14 - 90, top + ph - 30, 90, 22, "Done", 1) || Key(KEY_ESCAPE)) {
        for (int i = 0; i < kItemCount; i++)   // everything unlocked has now been seen
            if (Unlocked(kItems[i])) gProgress.known |= 1u << i;
        SaveProgress();
        SaveSettings();
        gState = ST_MENU;
    }
}

// Settings: four tabs down the left, one setting per row with a short note under it.
static void DrawSettings()
{
    int left, top;
    const int pw = std::min(gVW - 20, 440), ph = 290;
    Panel(pw, ph, left, top);
    Gear2((float)left + 20, (float)top + 20, kCoral);
    Text("Settings", left + 34, top + 9, 20, kCocoa, 0, false);

    static const char* kTabs[4] = { "Sound", "Game", "Display", "Progress" };
    for (int i = 0; i < 4; i++)
        if (Button(left + 12, top + 44 + i * 28, 86, 24, kTabs[i], gSettingsTab == i ? 2 : 0)) gSettingsTab = i;
    if (Key(KEY_DOWN)) gSettingsTab = std::min(3, gSettingsTab + 1);
    if (Key(KEY_UP)) gSettingsTab = std::max(0, gSettingsTab - 1);
    DrawRectangle(left + 106, top + 44, 1, ph - 86, Fade(kLine, 0.6f));

    const int x0 = left + 118, right = left + pw - 16;
    int y = top + 48;
    // Each row: a name, a control on the right, a note underneath, then a dotted rule.
    auto row = [&](const char* name, const char* note) {
        Text(name, x0, y + 3, 10, kCocoa, 0, false);
        Text(note, x0, y + 19, 10, kCocoaSoft, 0, false);
    };
    auto next = [&]() {
        y += 36;
        for (int x = x0; x < right; x += 4) DrawRectangle(x, y - 4, 2, 1, Fade(kLine, 0.7f));
        y += 4;
    };
    auto stepper = [&](const char* value, int& delta) {
        delta = 0;
        if (Button(right - 18, y, 18, 18, "+")) delta = 1;
        if (Button(right - 90, y, 18, 18, "-")) delta = -1;
        Text(value, right - 45, y + 4, 10, kCocoa, 1, false);
    };

    int delta;
    switch (gSettingsTab) {
        case 0: {
            row("Music", "how loud your song plays");
            Pips(right, y, 10, gCfg.music);
            next();
            row("Sound effects", "the little notes when you hit");
            Pips(right, y, 10, gCfg.sfx);
            next();
            row("Audio offset", "raise it if things arrive before the beat");
            stepper(TextFormat("%+d ms", gCfg.offsetMs), delta);
            gCfg.offsetMs = std::clamp(gCfg.offsetMs + delta * 5, -300, 300);
            break;
        }
        case 1: {
            row("Walking speed", "faster gives you less warning");
            stepper(TextFormat("%.1fx", gCfg.speed), delta);
            if (delta) gCfg.speed = std::clamp(std::round((gCfg.speed + delta * 0.1f) * 10.0f) / 10.0f, 0.6f, 2.0f);
            next();
            row("Key hints", "show the key above each thing");
            if (Switch(right, y + 1, gCfg.hints)) gCfg.hints = !gCfg.hints;
            next();
            static const char* kMotion[3] = { "Still", "Gentle", "Full" };
            row("Camera and effects", "how much the screen moves");
            Choice(right, y, kMotion, 3, gCfg.motion);
            break;
        }
        case 2: {
            const bool full = !gSim && IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE);
            row("Fullscreen", "F11 does this too");
            if (Switch(right, y + 1, full) && !gSim) ToggleBorderlessWindowed();
            next();
            static const char* kRates[3] = { "60", "120", "Max" };
            row("Frame rate", "60 is the lightest on your computer");
            Choice(right, y, kRates, 3, gCfg.fps, 40);
            break;
        }
        case 3: {
            auto line = [&](const char* name, const char* value) {
                Text(name, x0, y, 10, kCocoa, 0, false);
                Text(value, right, y, 10, kCocoa, 2, false);
                y += 17;
            };
            line("Songs finished", TextFormat("%d", gProgress.songs));
            line("Purrfect hits", TextFormat("%d", gProgress.perfects));
            line("Best streak", TextFormat("%d", gProgress.bestStreak));
            line("Time walked", TextFormat("%d min", (int)gProgress.minutes));
            line("Best rank", gProgress.bestRank <= 4 ? kRankLetter[gProgress.bestRank] : "-");
            line("Unlocked", TextFormat("%d of %d", UnlockedCount(), kItemCount));
            y += 8;
            const bool armed = gScene.time < gResetArmedUntil;
            if (Button(x0, y, 170, 20, armed ? "Click again to erase it all" : "Reset progress", armed ? 2 : 0)) {
                if (armed) {   // forgets unlocks and lifetime numbers; songs and their best ranks stay
                    gProgress = Progress();
                    gResetArmedUntil = -1.0f;
                    CheckEquipped();
                    BuildCat(gCfg.cat);
                    SaveProgress();
                } else {
                    gResetArmedUntil = gScene.time + 3.0f;
                }
            }
            break;
        }
    }

    if (Button(right - 90, top + ph - 32, 90, 22, "Done", 1) || Key(KEY_ESCAPE)) {
        SaveSettings();
        gState = gSettingsFrom;
    }
}

static void DrawFrame(double t)
{
    RenderWorld(t);
    switch (gState) {
        case ST_MENU:    DrawMenu(); break;
        case ST_LOADING: DrawLoading(); break;
        case ST_READY:   DrawReady(); break;
        case ST_PLAYING: DrawHud(t); break;
        case ST_PAUSED:
            DrawHud(t);
            DrawRectangle(0, 0, gVW, gVH, Color{ 20, 14, 34, 110 });
            DrawPause();
            break;
        case ST_COUNTDOWN:
            DrawHud(t);
            Text(TextFormat("%d", (int)std::ceil(gCountdown)), gVW / 2, gVH / 2 - 60, 50, kInk);
            break;
        case ST_RESULTS:  DrawResults(); break;
        case ST_WARDROBE: DrawWardrobe(); break;
        case ST_SETTINGS: DrawSettings(); break;
    }
}

// ---------------------------------------------------------------------------
// Self-test and entry point
// ---------------------------------------------------------------------------

// Analyzes the demo track, whose notes sit on a known grid, and reports how
// far detected onsets land from it. Used to calibrate the analysis.
static int SelfTest()
{
    std::vector<int16_t> pcm;
    int rate = 0;
    SynthDemoTrack(pcm, rate);
    SongAnalysis song;
    const double start = NowSeconds();
    if (!AnalyzeSong(pcm.data(), pcm.size() / 2, 2, rate, song)) { printf("analysis failed\n"); return 1; }
    const double elapsed = NowSeconds() - start;
    printf("demo track: %.1f s, analyzed in %.0f ms\n", song.duration, elapsed * 1000.0);

    static const char* kSfxName[SFX_COUNT] = { "hop", "bat", "swat", "ring", "twinkle", "miss", "milestone", "click" };
    std::vector<float> sfx[SFX_COUNT];
    BuildSfx(sfx);
    for (int i = 0; i < SFX_COUNT; i++) {
        double peak = 0.0, energy = 0.0;
        for (float v : sfx[i]) { peak = std::max(peak, (double)std::fabs(v)); energy += (double)v * v; }
        printf("sfx %-9s %4.0f ms  peak %.2f  rms %.3f\n", kSfxName[i], sfx[i].size() * 1000.0 / kSfxRate, peak, std::sqrt(energy / sfx[i].size()));
    }
    const double grid = DemoSixteenthSeconds();
    for (int d = 0; d < DIFF_COUNT; d++) {
        const Chart& c = song.charts[d];
        double sum = 0.0, sumAbs = 0.0, worst = 0.0;
        int lanes[4] = {};
        for (size_t i = 0; i < c.time.size(); i++) {
            const double e = c.time[i] - std::round(c.time[i] / grid) * grid;
            sum += e; sumAbs += std::fabs(e); worst = std::max(worst, std::fabs(e));
            lanes[c.lane[i]]++;
        }
        const double n = (double)std::max<size_t>(1, c.time.size());
        printf("%-8s %4d notes (%.2f/s)  lanes %d/%d/%d/%d  grid error mean %+.1f ms, mean abs %.1f ms, worst %.1f ms\n",
               kDiffName[d], (int)c.time.size(), c.time.size() / song.duration,
               lanes[0], lanes[1], lanes[2], lanes[3], sum / n * 1000.0, sumAbs / n * 1000.0, worst * 1000.0);
    }
    return 0;
}

int main(int argc, char** argv)
{
    // <audio file>                    start with that song loaded
    // --selftest                      analysis and sound check, no window
    // --sfx-demo <wav>                writes a short audition of the sound effects
    // --shot <png> [--view menu|ready|play|pause|results|wardrobe|settings] [--at <seconds>] [--size WxH]
    //              [--diff 0..2] [--scenery 0..5] [--variant 0..2] [--cat 0..6] [--hat n] [--neck n] [--back n]
    //              [--tab 0..3] [--closeup] [--unlock-all] [--count]
    //                                 renders offscreen with a simulated clock and saves one frame
    const char* shotPath = nullptr;
    std::string songPath;
    std::string shotView = "play";
    double shotAt = 12.0;
    int winW = 1280, winH = 720, shotDiff = -1, shotScenery = -1, shotCat = -1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--selftest")) return SelfTest();
        if (!strcmp(argv[i], "--sfx-demo") && i + 1 < argc) return WriteSfxDemo(argv[i + 1]) ? 0 : 1;
        if (!strcmp(argv[i], "--shot") && i + 1 < argc) shotPath = argv[++i];
        else if (!strcmp(argv[i], "--view") && i + 1 < argc) shotView = argv[++i];
        else if (!strcmp(argv[i], "--at") && i + 1 < argc) shotAt = atof(argv[++i]);
        else if (!strcmp(argv[i], "--diff") && i + 1 < argc) shotDiff = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--scenery") && i + 1 < argc) shotScenery = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--cat") && i + 1 < argc) shotCat = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--variant") && i + 1 < argc) gForceVariant = std::clamp(atoi(argv[++i]), 0, 2);
        else if (!strcmp(argv[i], "--hat") && i + 1 < argc) gCfg.hat = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--neck") && i + 1 < argc) gCfg.neck = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--back") && i + 1 < argc) gCfg.back = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--tab") && i + 1 < argc) gWardrobeTab = gSettingsTab = std::clamp(atoi(argv[++i]), 0, 3);
        else if (!strcmp(argv[i], "--unlock-all")) gUnlockAll = true;
        else if (!strcmp(argv[i], "--count")) gSimCounts = true;
        else if (!strcmp(argv[i], "--closeup")) gCloseup = true;
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &winW, &winH);
        else if (argv[i][0] != '-') songPath = argv[i];
    }
    gSim = shotPath != nullptr;

    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(gSim ? FLAG_WINDOW_HIDDEN : (FLAG_VSYNC_HINT | FLAG_WINDOW_RESIZABLE));
    InitWindow(winW, winH, "ONIRIA");
    SetWindowMinSize(480, 360);
    SetExitKey(KEY_NULL);
    rlDisableBackfaceCulling();

    if (!gSim) {
        InitAudioDevice();
        // The stream runs from the start so menus have sound; it is rebuilt if a song needs another rate.
        BuildSfx(gSfxData);
        gPlayer.stream = LoadAudioStream((unsigned)gPlayer.sampleRate.load(), 32, 2);
        SetAudioStreamCallback(gPlayer.stream, FeedAudio);
        PlayAudioStream(gPlayer.stream);
        gPlayer.hasStream = true;
        LoadSettings();
        LoadLibrary();
        LoadProgress();
        PlatformStartKeyInput(GetWindowHandle());
    } else {
        if (shotDiff >= 0) gCfg.diff = std::clamp(shotDiff, 0, DIFF_COUNT - 1);
        if (shotScenery >= 0) gCfg.scenery = std::clamp(shotScenery, 0, (int)BIOME_COUNT);
        if (shotCat >= 0) gCfg.cat = shotCat;
        // Screenshots never touch the real library; the menu gets a sample shelf instead.
        static const char* kSample[6] = { "Sunday Morning Tea", "Rooftop Lullaby", "Paper Lanterns", "Tiny Planet Waltz", "Rain on the Greenhouse", "Marmalade Skies" };
        static const float kSampleBest[6] = { 0.97f, 0.91f, -1.0f, 0.82f, 0.70f, -1.0f };
        for (int i = 0; i < 6; i++) {
            SongEntry e;
            e.path = kSample[i];
            e.name = kSample[i];
            e.duration = 150.0f + 23.0f * i;
            e.best[1] = kSampleBest[i];
            gLibrary.push_back(e);
        }
        SongEntry demo;
        demo.path = kDemoKey;
        demo.name = "Demo track";
        demo.duration = 49.0f;
        gLibrary.push_back(demo);
        gMenuSel = 1;
    }

    if (!gSim || !gUnlockAll) CheckEquipped();
    gCfg.cat = std::clamp(gCfg.cat, 0, kCatCount - 1);
    BuildShapes();
    BuildCat(gCfg.cat);
    UpdateCanvas();

    // Frame pacing is done here rather than left to v-sync (which drivers can
    // switch off, letting the loop spin a whole core) or to raylib's limiter
    // (which busy-waits). The game sleeps precisely until the next frame is due.
    int refresh = gSim ? 0 : GetMonitorRefreshRate(GetCurrentMonitor());
    if (refresh < 30) refresh = 60;
    double frameStart = NowSeconds(), nextFrame = frameStart;

    int shotFrames = 0, shotHold = -1;
    if (gSim) {
        gCfg.watch = true;
        if (shotView == "wardrobe") gState = ST_WARDROBE;
        else if (shotView == "settings") gState = ST_SETTINGS;
        else if (shotView != "menu") {
            BeginLoad(songPath, songPath.empty());
            FinishLoad();
            if (gState != ST_READY) printf("load failed: %s\n", gError.c_str());
            else if (shotView != "ready") StartGame();
        }
    } else if (!songPath.empty()) {
        BeginLoad(songPath, false);
    }

    bool quit = false;
    while (!quit && (gSim || !WindowShouldClose())) {
        float dt = 1.0f / 60.0f;
        if (!gSim) {
            // Wait until this frame is due: up to 120 a second while playing, 60 in
            // menus, 30 when the window is in the background.
            const bool playing = gState == ST_PLAYING || gState == ST_COUNTDOWN;
            const int most = gCfg.fps == 0 ? 60 : gCfg.fps == 1 ? 120 : 240;
            const int cap = !IsWindowFocused() ? 30 : std::min(refresh, playing ? most : 60);
            gPlayer.gain.store(gCfg.music / 10.0f, std::memory_order_relaxed);
            nextFrame += 1.0 / cap;
            double now = NowSeconds();
            if (nextFrame < now - 0.05) nextFrame = now;   // fell behind: start over instead of catching up
            PlatformSleep(nextFrame - now);
            now = NowSeconds();
            dt = (float)std::clamp(now - frameStart, 0.0005, 0.1);
            frameStart = now;
        }
        UpdateCanvas();
        gFrameState = gState;

        if (!gSim) {
            if (IsKeyPressed(KEY_F11)) ToggleBorderlessWindowed();
            if (IsFileDropped()) {
                FilePathList files = LoadDroppedFiles();
                if (files.count > 0 && (gState == ST_MENU || gState == ST_READY || gState == ST_RESULTS))
                    BeginLoad(files.paths[0], false);
                UnloadDroppedFiles(files);
            }
        }

        double t = gPausedAt;
        switch (gState) {
            case ST_LOADING:
                if (gJob.stage.load() == 3) FinishLoad();
                break;
            case ST_PLAYING:
                if (gSim) gSimTime += dt; else SyncClock();
                t = SongTime();
                UpdatePlaying(t);
                if (gState == ST_PLAYING && !gSim && (Key(KEY_ESCAPE) || Key(KEY_P) || !IsWindowFocused())) {
                    gPlayer.playing.store(false);
                    gPausedAt = t;
                    memset(gHeld, 0, sizeof(gHeld));
                    gState = ST_PAUSED;
                }
                break;
            case ST_COUNTDOWN:
                gCountdown -= dt;
                if (gCountdown <= 0.0f) { ResumePlayback(); gState = ST_PLAYING; }
                break;
            default: break;
        }
        if (gState != ST_PLAYING) { LaneKeyEvent drop[32]; while (PlatformPollKeys(drop, 32) == 32) {} }

        UpdateScene(dt, t);

        if (!gSim && (IsWindowMinimized() || PlatformWindowMinimized(GetWindowHandle()))) {   // nothing to show: keep the events flowing and doze
            PollInputEvents();
            PlatformSleep(0.1);
            continue;
        }

        BeginTextureMode(gCanvas);
        DrawFrame(t);
        EndTextureMode();

        if (gSim) {
            shotFrames++;
            if (shotHold < 0) {
                const bool timed = shotView == "play" || shotView == "pause" || shotView == "results";
                if (gState == ST_MENU || gState == ST_WARDROBE || gState == ST_SETTINGS || !timed ? shotFrames >= 180 : gSimTime >= shotAt) {
                    if (shotView == "pause") { gPausedAt = t; gState = ST_PAUSED; shotHold = 40; }
                    else if (shotView == "results") { FinishGame(); shotHold = 90; }
                    else shotHold = 0;
                }
            } else if (shotHold > 0) {
                shotHold--;
            }
            if (shotHold == 0) {
                Image image = LoadImageFromTexture(gCanvas.texture);
                ImageFlipVertical(&image);
                ImageResizeNN(&image, image.width * gPixel, image.height * gPixel);
                ExportImage(image, shotPath);
                UnloadImage(image);
                quit = true;
            }
        } else {
            BeginDrawing();
            ClearBackground(BLACK);
            DrawTexturePro(gCanvas.texture, Rectangle{ 0, 0, (float)gVW, -(float)gVH },
                           Rectangle{ 0, 0, (float)(gVW * gPixel), (float)(gVH * gPixel) }, Vector2{ 0, 0 }, 0.0f, WHITE);
            EndDrawing();
        }
    }

    gPlayer.playing.store(false);
    if (gJob.thread.joinable()) gJob.thread.join();
    gPlayer.pcm.Free();
    gJob.pcm.Free();
    if (!gSim) {
        SaveSettings();
        PlatformStopKeyInput();
        if (gPlayer.hasStream) UnloadAudioStream(gPlayer.stream);
        CloseAudioDevice();
    }
    CloseWindow();
    return 0;
}
