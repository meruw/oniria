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

enum State { ST_MENU, ST_LOADING, ST_READY, ST_PLAYING, ST_PAUSED, ST_COUNTDOWN, ST_RESULTS };
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
    }
    fclose(f);
}

static void SaveSettings()
{
    FILE* f = fopen(ConfigPath().c_str(), "w");
    if (!f) return;
    fprintf(f, "diff=%d\nspeed=%.2f\noffset=%d\nwatch=%d\nhints=%d\nsfx=%d\n",
            gCfg.diff, gCfg.speed, gCfg.offsetMs, gCfg.watch ? 1 : 0, gCfg.hints ? 1 : 0, gCfg.sfx);
    fclose(f);
}

// ---------------------------------------------------------------------------
// Audio playback
// ---------------------------------------------------------------------------
// The whole song sits decoded in memory and is fed to the device from the
// audio thread. Each callback stamps (position, time) so the game can tell
// exactly where playback is between callbacks.

struct Player {
    std::vector<int16_t> pcm;   // stereo interleaved
    std::atomic<int> sampleRate{ 48000 };
    int64_t  frames = 0;
    std::mutex mutex;           // guards pcm/frames/pos against song swaps and seeks
    std::atomic<int64_t>  pos{ 0 };
    std::atomic<bool>     playing{ false };
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
        const int16_t* data = gPlayer.pcm.data();
        const int64_t total = gPlayer.frames;
        const float k = 0.9f / 32768.0f;   // a little headroom for the effects on top
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

// Loading runs on a worker thread so the world keeps moving.
struct LoadJob {
    std::thread thread;
    std::atomic<int> stage{ 0 };   // 0 idle, 1 decoding, 2 analyzing, 3 finished
    bool ok = false;
    std::string error, name;
    std::vector<int16_t> pcm;
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

enum SparkKind { SPARK_SHARD, SPARK_STAR, SPARK_HEART };
struct Spark { Vector3 pos, vel, col; float life, span, size, gravity; int kind; };
const int kSparkCount = 192;
static Spark gSparks[kSparkCount];
static int   gNextSpark = 0;

// Rings of light that spread on the path where a perfect hit landed.
struct Ring { Vector3 pos; float age; };
const int kRingCount = 8;
static Ring gRings[kRingCount];
static int  gNextRing = 0;

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

// Emits one lit, fogged triangle. `glow` 1 ignores lighting (lamps, sparks).
static void EmitTri(Vector3 a, Vector3 b, Vector3 c, Vector3 col, float glow = 0.0f, float alpha = 1.0f)
{
    const Vector3 n = Vector3Normalize(Vector3CrossProduct(Vector3Subtract(b, a), Vector3Subtract(c, a)));
    const float ndl = std::max(0.0f, Vector3DotProduct(n, gLight.lightDir));
    const float sky = 0.65f + 0.35f * n.y;
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
    gPixel = std::max(1, std::min((int)lroundf((float)h / 360.0f), w / 480));
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

static Shape sCatBody, sCatHead, sCatEyes, sCatLeg, sCatTail, sCatTailTip;
static Shape sLog, sYarn, sWing, sButterflyBody, sBellStalk, sBell;
static Shape sTrunk, sCrown, sPine, sBush, sRock, sFlower, sMushroom, sHouse, sWindows, sLampPost, sLamp;
static Shape sSpark, sShadow, sHalo, sStar, sHeart, sRing, sCatBlush;

static const Vector3 kActionColor[ACT_COUNT] = { { 0.93f, 0.62f, 0.33f }, { 0.95f, 0.45f, 0.56f }, { 0.45f, 0.78f, 0.95f }, { 0.98f, 0.84f, 0.36f } };

static void BuildShapes()
{
    const Vector3 fur = Rgb(240, 160, 84), furDark = Rgb(206, 120, 58), cream = Rgb(255, 244, 226), pink = Rgb(250, 160, 170), ink = Rgb(52, 40, 52);

    // Cat, facing +x. Each part is modelled around its own pivot.
    AddBlob(sCatBody, { 0, 0, 0 }, { 0.52f, 0.30f, 0.27f }, fur);
    AddBlob(sCatBody, { 0.30f, -0.08f, 0 }, { 0.22f, 0.20f, 0.20f }, cream);
    AddBox(sCatBody, { -0.20f, 0.20f, 0 }, { 0.05f, 0.06f, 0.20f }, furDark);
    AddBox(sCatBody, { 0.02f, 0.21f, 0 }, { 0.05f, 0.06f, 0.21f }, furDark);

    AddBlob(sCatHead, { 0, 0, 0 }, { 0.36f, 0.32f, 0.36f }, fur);
    AddBlob(sCatHead, { 0.24f, -0.10f, 0 }, { 0.15f, 0.12f, 0.20f }, cream);
    AddBox(sCatHead, { 0.385f, -0.05f, 0 }, { 0.03f, 0.025f, 0.04f }, pink);
    for (int side = -1; side <= 1; side += 2) {
        AddCone(sCatHead, { -0.04f, 0.20f, side * 0.21f }, 0.15f, 0.30f, 4, fur, kPi / 4);
        AddCone(sCatHead, { 0.0f, 0.22f, side * 0.21f }, 0.085f, 0.19f, 4, pink, kPi / 4);
        AddBox(sCatEyes, { 0.30f, 0.0f, side * 0.17f }, { 0.04f, 0.065f, 0.045f }, ink);   // modelled around eye height
    }

    AddBox(sCatLeg, { 0, -0.15f, 0 }, { 0.062f, 0.16f, 0.062f }, fur);
    AddBox(sCatLeg, { 0.01f, -0.34f, 0 }, { 0.072f, 0.055f, 0.068f }, cream);
    AddBox(sCatTail, { 0, 0.16f, 0 }, { 0.05f, 0.17f, 0.05f }, fur);
    AddBox(sCatTailTip, { 0, 0.14f, 0 }, { 0.055f, 0.15f, 0.055f }, cream);

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
    AddCone(sHouse, { 0, 1.2f, 0 }, 1.45f, 0.85f, 4, Rgb(204, 108, 92), kPi / 4);
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
    for (int side = -1; side <= 1; side += 2)
        AddBox(sCatBlush, { 0.27f, -0.07f, side * 0.24f }, { 0.035f, 0.04f, 0.05f }, Rgb(255, 150, 165));
}

// ---------------------------------------------------------------------------
// World
// ---------------------------------------------------------------------------

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

static float GroundHeight(float x, float z)
{
    const float away = std::clamp((fabsf(z) - 1.0f) / 3.0f, 0.0f, 1.0f);
    const float bumps = 0.32f * sinf(x * 0.31f + z * 0.7f) + 0.22f * sinf(x * 0.13f - z * 0.45f);
    const float slope = z < 0.0f ? std::max(0.0f, -z - 3.0f) * 0.11f : -std::max(0.0f, z - 1.6f) * 0.16f;
    return PathHeight(x) + away * away * (3 - 2 * away) * bumps + slope;
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
    gJob.pcm.clear();
    if (demo) {
        SynthDemoTrack(gJob.pcm, gJob.sampleRate);
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
                const int16_t* data = (const int16_t*)wave.data;
                gJob.pcm.assign(data, data + (size_t)wave.frameCount * 2);
                gJob.sampleRate = wave.sampleRate;
                UnloadWave(wave);
            }
        }
    }
    if (gJob.error.empty()) {
        gJob.stage.store(2);
        gJob.song = SongAnalysis();
        if (!AnalyzeSong(gJob.pcm.data(), gJob.pcm.size() / 2, 2, gJob.sampleRate, gJob.song))
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
        gState = ST_MENU;
        return;
    }
    {
        std::lock_guard<std::mutex> lock(gPlayer.mutex);
        gPlayer.pcm.swap(gJob.pcm);
        gPlayer.frames = (int64_t)(gPlayer.pcm.size() / 2);
    }
    gJob.pcm = std::vector<int16_t>();
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

    Vector3 at = NotePosition(note);
    at.y += 0.2f;
    Burst(at, kActionColor[action], j == J_PERFECT ? 8 : j == J_GREAT ? 6 : 4);

    // Each action has its own instrument, and the note climbs with the streak.
    const float semitones = SfxStreakSemitones(gCombo);
    PlaySfx(SFX_HOP + action, semitones, j == J_PERFECT ? 1.0f : j == J_GREAT ? 0.75f : 0.5f);

    if (j == J_PERFECT) {
        // A perfect hit always gets a twinkle, stars, a ring of light and two new flowers.
        PlaySfx(SFX_TWINKLE, semitones);
        gJoyKick = 1.0f;
        Celebrate(at, 5 + (int)(gJoy * 4.0f), gJoy > 0.3f ? 1 + (int)(gJoy * 2.0f) : 0);
        gRings[gNextRing++ % kRingCount] = Ring{ Vector3{ at.x, PathHeight(at.x) + 0.04f, 0 }, 0.0f };
        AddBloom(at.x + (Rand01() - 0.5f) * 0.8f, -1.25f - Rand01() * 0.7f);
        AddBloom(at.x + (Rand01() - 0.5f) * 0.8f, 1.8f + Rand01() * 0.6f);
    }

    if (gCombo == 10 || gCombo == 25 || gCombo == 50 || (gCombo >= 100 && gCombo % 50 == 0)) {
        gMilestone = gCombo;
        gMilestoneAge = 0.0f;
        PlaySfx(SFX_MILESTONE);
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
    else if (!PathFollowsSong()) joyTarget = 0.0f;
    gJoy += (joyTarget - gJoy) * std::min(1.0f, dt * (joyTarget > gJoy ? 3.0f : 5.0f));
    gJoyKick *= expf(-dt * 7.0f);
    gMilestoneAge += dt;

    for (int a = 0; a < ACT_COUNT; a++) s.glow[a] = gHeld[a] ? 1.0f : s.glow[a] * expf(-dt * 9.0f);
    if (!frozen) {
        gCatActionAge += dt;
        s.wag += dt * (2.6f + 5.0f * gJoy);
        for (Ring& r : gRings) r.age += dt;
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
static float gViewCentre = 0.0f;   // world x at the middle of the screen
static float gViewHalf = 5.3f;     // half the visible width of the path, in world units

static void EmitGround(Vector3 a, Vector3 b, Vector3 c, Vector3 col)
{
    // Ground triangles must face up whatever order they were built in.
    const float ny = (b.z - a.z) * (c.x - a.x) - (b.x - a.x) * (c.z - a.z);
    if (ny < 0.0f) EmitTri(a, c, b, col); else EmitTri(a, b, c, col);
}

static void DrawTerrain()
{
    static const float rows[] = { 40.0f, 16.0f, 7.0f, 3.6f, 1.0f, -1.0f, -2.6f, -5.0f, -8.0f, -12.0f, -17.0f, -24.0f };
    const int rowCount = (int)(sizeof(rows) / sizeof(rows[0]));
    const int x0 = (int)floorf(gViewCentre - gViewHalf * 3.5f) - 2, x1 = (int)floorf(gViewCentre + gViewHalf * 3.5f) + 3;
    const Palette& p = gPalette;
    for (int x = x0; x < x1; x++) {
        for (int r = 0; r + 1 < rowCount; r++) {
            const float za = rows[r], zb = rows[r + 1];
            const Vector3 v00 = { (float)x, GroundHeight((float)x, za), za }, v10 = { (float)x + 1, GroundHeight((float)x + 1, za), za };
            const Vector3 v01 = { (float)x, GroundHeight((float)x, zb), zb }, v11 = { (float)x + 1, GroundHeight((float)x + 1, zb), zb };
            const bool path = za == 1.0f;
            const float shade = 0.92f + 0.16f * Hash01(x, r);
            const Vector3 base = path ? p.path : Mix(p.grass, Vector3Scale(p.grass, 0.82f), (float)r / rowCount);
            const Vector3 colA = Vector3Scale(base, shade), colB = Vector3Scale(base, shade * (path ? 0.96f : 0.93f));
            if ((x + r) & 1) { EmitGround(v00, v10, v11, colA); EmitGround(v00, v11, v01, colB); }
            else             { EmitGround(v00, v10, v01, colA); EmitGround(v10, v11, v01, colB); }
        }
    }
}

// Soft discs of light around lamps, windows and rung bells. Collected while
// the solid world is drawn and blended on top afterwards.
struct Halo { Vector3 pos; float radius; Vector3 col; float alpha; };
static Halo gHalos[96];
static int  gHaloCount = 0;
static const Vector3 kWarm = { 1.0f, 0.86f, 0.55f };

static void AddHalo(Vector3 pos, float radius, Vector3 col, float alpha)
{
    if (gHaloCount < 96 && alpha > 0.01f) gHalos[gHaloCount++] = { pos, radius, col, alpha };
}

// Trees, houses and small things, placed by hashing the position so the same
// stretch of path always looks the same. Everything bops a little on the beat.
static void DrawScenery()
{
    static const Vector3 kLeaf[5] = { { 0.47f, 0.72f, 0.42f }, { 0.38f, 0.64f, 0.44f }, { 0.60f, 0.76f, 0.40f }, { 0.96f, 0.66f, 0.42f }, { 0.96f, 0.62f, 0.68f } };
    static const Vector3 kPetal[4] = { { 1.0f, 0.86f, 0.4f }, { 1.0f, 0.62f, 0.7f }, { 0.75f, 0.7f, 1.0f }, { 1.0f, 1.0f, 1.0f } };
    const Palette& p = gPalette;
    const float bop = 1.0f + (0.07f + 0.18f * gJoy) * gScene.beat;   // everything dances harder on a streak
    const float lit = std::max(p.lamps, gJoy * 0.7f);                // and lamps come on even by day
    const Vector3 white = { 1, 1, 1 };
    const Vector3 night = Mix(white, Vector3{ 0.7f, 0.8f, 1.0f }, p.stars);   // leaves cool down after dark
    const float spacing = 2.4f;
    const int s0 = (int)floorf((gViewCentre - gViewHalf * 2.7f - 3.0f) / spacing), s1 = (int)floorf((gViewCentre + gViewHalf * 2.7f + 3.0f) / spacing);

    for (int s = s0; s <= s1; s++) {
        const float fx = s * spacing;
        const float sway = 0.08f * gJoy * sinf(gScene.time * 3.2f + fx);
        {   // beside the path: flowers, bushes, rocks, mushrooms, lamps
            const float r = Hash01(s, 2), x = fx + Hash01(s, 3) * 1.8f, z = -1.9f - Hash01(s, 4) * 1.3f;
            const Vector3 at = { x, GroundHeight(x, z), z };
            if (r < 0.24f) {
                for (int k = 0; k < 3; k++) {
                    const float ox = x + (Hash01(s, 50 + k) - 0.5f) * 0.8f, oz = z + (Hash01(s, 60 + k) - 0.5f) * 0.6f;
                    DrawShape(sFlower, Place(Vector3{ ox, GroundHeight(ox, oz), oz }, 1.0f, sway, bop), kPetal[Hash(s, 70 + k) & 3]);
                }
            } else if (r < 0.38f) DrawShape(sBush, Place(at, 0.8f + Hash01(s, 5) * 0.5f, sway, bop), Vector3Multiply(kLeaf[Hash(s, 6) % 3], night));
            else if (r < 0.46f) DrawShape(sRock, Place(at, 0.8f + Hash01(s, 5)));
            else if (r < 0.52f) DrawShape(sMushroom, Place(at, 1.0f, sway, bop));
            else if (r < 0.60f) {
                DrawShape(sLampPost, Place(at));
                DrawShape(sLamp, Place(at), white, 0.25f + 0.75f * lit);
                AddHalo(Vector3{ at.x, at.y + 1.5f, at.z + 0.15f }, 0.5f + 0.06f * gScene.beat, kWarm, 0.13f * lit);
            }
        }
        {   // middle distance: round trees and pines
            const float r = Hash01(s, 8), x = fx + Hash01(s, 9) * 2.0f, z = -4.2f - Hash01(s, 10) * 3.4f;
            if (r < 0.6f) {
                const Vector3 at = { x, GroundHeight(x, z), z };
                const float size = 0.95f + Hash01(s, 11) * 0.5f;
                const Vector3 leaf = Vector3Multiply(kLeaf[Hash(s, 12) % 5], night);
                DrawShape(sTrunk, Place(at, size, sway));
                if (Hash(s, 13) & 1) DrawShape(sCrown, Place(at, size, sway, bop), leaf);
                else DrawShape(sPine, Place(at, size, sway, bop), Vector3Multiply(kLeaf[Hash(s, 12) % 3], Vector3Scale(night, 0.85f)));
            }
        }
        {   // far: bigger trees and the odd house with a lit window
            const float r = Hash01(s, 14), x = fx + Hash01(s, 15) * 2.0f, z = -9.5f - Hash01(s, 16) * 6.0f;
            const Vector3 at = { x, GroundHeight(x, z), z };
            if (r < 0.14f) {
                const float hz = -9.5f - Hash01(s, 16) * 2.0f;
                const Vector3 home = { x, GroundHeight(x, hz), hz };
                DrawShape(sHouse, Place(home, 1.25f));
                DrawShape(sWindows, Place(home, 1.25f), white, 0.2f + 0.8f * lit);
                AddHalo(Vector3{ home.x + 0.38f, home.y + 0.88f, home.z + 1.0f }, 0.55f, kWarm, 0.12f * lit);
            } else if (r < 0.62f) {
                const float size = 1.3f + Hash01(s, 17) * 0.8f;
                DrawShape(sTrunk, Place(at, size, sway));
                const bool pine = (Hash(s, 18) % 3) != 0;
                DrawShape(pine ? sPine : sCrown, Place(at, size, sway, bop), Vector3Multiply(kLeaf[Hash(s, 19) % (pine ? 3 : 5)], Vector3Scale(night, 0.88f)));
            }
        }
        {   // in front of the path: only low things, so nothing hides the cat
            const float r = Hash01(s, 24), x = fx + Hash01(s, 25) * 2.0f, z = 2.0f + Hash01(s, 26) * 1.6f;
            const Vector3 at = { x, GroundHeight(x, z), z };
            if (r < 0.2f) DrawShape(sFlower, Place(at, 1.2f, sway, bop), kPetal[Hash(s, 27) & 3]);
            else if (r < 0.3f) DrawShape(sRock, Place(at, 0.7f));
            else if (r < 0.38f) DrawShape(sBush, Place(at, 0.6f, sway, bop), Vector3Multiply(kLeaf[Hash(s, 28) % 3], night));
        }
        // Small pastel butterflies join in behind the path once the streak is going.
        if (gJoy > 0.4f && Hash01(s, 90) < (gJoy - 0.4f) * 1.4f) {
            static const Vector3 kPastel[3] = { { 1.0f, 0.72f, 0.84f }, { 1.0f, 0.92f, 0.6f }, { 0.82f, 0.74f, 1.0f } };
            const float phase = Hash01(s, 91) * 6.283f, time = gScene.time;
            const float x = fx + Hash01(s, 92) * 2.0f + 0.5f * sinf(time * 0.9f + phase), z = -2.2f - Hash01(s, 93) * 3.2f;
            const float y = GroundHeight(x, z) + 1.0f + Hash01(s, 94) * 1.3f + 0.25f * sinf(time * 2.1f + phase);
            const float flap = 0.1f + 1.0f * (0.5f + 0.5f * sinf(time * 16.0f + phase));
            const Matrix body = Place(Vector3{ x, y, z }, 0.5f);
            DrawShape(sWing, MatrixMultiply(MatrixRotateY(-flap), body), kPastel[Hash(s, 95) % 3], 0.6f);
            DrawShape(sWing, MatrixMultiply(MatrixRotateY(kPi + flap), body), kPastel[Hash(s, 95) % 3], 0.6f);
        }
    }

    // Flowers that bloomed because of the streak: they spring up with a bounce.
    for (const Bloom& b : gBlooms) {
        if (b.x < gViewCentre - gViewHalf * 1.6f - 2.0f || b.x > gViewCentre + gViewHalf * 1.6f + 2.0f) continue;
        const float k = std::clamp((gScene.time - b.born) * 3.5f, 0.0f, 1.0f);
        const float size = b.size * k * (1.0f + 0.45f * sinf(k * kPi));
        DrawShape(sFlower, Place(Vector3{ b.x, GroundHeight(b.x, b.z), b.z }, size, 0.1f * gJoy * sinf(gScene.time * 3.2f + b.x), bop), kPetal[b.tint]);
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
    const float phase = x * 2.4f;
    const float swing = walking && lift < 0.05f ? 0.55f * sinf(phase) : 0.0f;
    // A happy cat prances: a bigger bounce in its step, and a little hop on every beat.
    const float bob = walking ? (0.035f + 0.06f * joy) * fabsf(cosf(phase)) + 0.06f * joy * gScene.beat : 0.0f;
    const float slope = atanf((PathHeight(x + 0.4f) - PathHeight(x - 0.4f)) / 0.8f);

    // The whole cat pivots around its back hips, so pitching up reads as standing.
    // It stretches upwards for an instant on every perfect hit.
    const Vector3 hips = { -0.33f, 0.42f, 0.0f };
    Matrix root = MatrixTranslate(-hips.x, -hips.y, -hips.z);
    root = MatrixMultiply(root, MatrixRotateX(roll));
    root = MatrixMultiply(root, MatrixRotateZ(pitch + slope * 0.7f));
    root = MatrixMultiply(root, MatrixTranslate(hips.x, hips.y, 0.0f));
    root = MatrixMultiply(root, MatrixScale(kCatSize, kCatSize * (1.0f + 0.1f * gJoyKick), kCatSize));
    root = MatrixMultiply(root, MatrixTranslate(x, ground + lift + bob, 0.0f));

    DrawShape(sCatBody, MatrixMultiply(MatrixTranslate(0, 0.62f, 0), root));

    // The head stays roughly level, is turned a little towards the camera, and nods along when happy.
    Matrix head = MatrixRotateY(-0.35f);
    head = MatrixMultiply(head, MatrixRotateZ(-pitch * 0.55f + 0.05f * sinf(gScene.time * 1.3f) + 0.09f * joy * sinf(gScene.wag * 1.5f)));
    head = MatrixMultiply(head, MatrixTranslate(0.52f, 1.0f + 0.02f * sinf(phase * 2), 0));
    DrawShape(sCatHead, MatrixMultiply(head, root));
    if (joy > 0.3f) DrawShape(sCatBlush, MatrixMultiply(head, root));
    const bool blinking = fmodf(gScene.time, 3.7f) < 0.12f;
    const bool pleased = (gJudgeKind == J_PERFECT && gJudgeAge < 0.3f) || joy > 0.6f;   // eyes smile shut
    const Matrix eyes = MatrixMultiply(MatrixScale(1.0f, blinking ? 0.15f : pleased ? 0.4f : 1.0f, 1.0f), MatrixTranslate(0, 0.07f, 0));
    DrawShape(sCatEyes, MatrixMultiply(MatrixMultiply(eyes, head), root));

    for (int i = 0; i < 4; i++) {
        const bool front = i < 2, nearSide = (i & 1) == 0;
        float a = swing * ((front == nearSide) ? 1.0f : -1.0f);
        if (front) a += frontLegs + tuck * 1.0f + (nearSide ? paw : 0.0f);
        else a -= tuck * 0.9f;
        const Matrix leg = MatrixMultiply(MatrixRotateZ(a), MatrixTranslate(front ? 0.33f : -0.33f, 0.42f, nearSide ? 0.15f : -0.15f));
        DrawShape(sCatLeg, MatrixMultiply(leg, root));
    }

    // The tail goes up and wags faster with the mood.
    const float sway = (0.18f + 0.25f * joy) * sinf(gScene.wag) + 0.12f * gScene.beat;
    const Matrix t1 = MatrixMultiply(MatrixRotateZ(0.95f - 0.4f * joy + sway), MatrixTranslate(-0.55f, 0.72f, 0));
    const Matrix t2 = MatrixMultiply(MatrixMultiply(MatrixRotateZ(-0.5f + sway), MatrixTranslate(0, 0.32f, 0)), t1);
    const Matrix t3 = MatrixMultiply(MatrixMultiply(MatrixRotateZ(-0.55f + sway), MatrixTranslate(0, 0.32f, 0)), t2);
    DrawShape(sCatTail, MatrixMultiply(t1, root));
    DrawShape(sCatTail, MatrixMultiply(t2, root));
    DrawShape(sCatTailTip, MatrixMultiply(t3, root));
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
        if (ring.age > 0.45f) continue;
        const float k = ring.age / 0.45f;
        DrawShape(sRing, Place(ring.pos, 0.35f + 2.0f * k), Vector3{ 1.0f, 0.96f, 0.8f }, 1.0f, 0.7f * (1.0f - k) * (1.0f - k));
    }

    for (const Spark& s : gSparks) {
        if (s.life <= 0.0f) continue;
        const float k = s.life / s.span;   // 1 when born, 0 when gone
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


static void RenderWorld(double t)
{
    const Scene& s = gScene;
    gPalette = PaletteAt(s.day);
    gHaloCount = 0;

    const float aspect = (float)gVW / (float)gVH, tanHalf = 0.3057f;   // tan(34 deg / 2)
    const float dist = std::clamp(4.3f / (tanHalf * aspect), 9.8f, 24.0f);
    gViewHalf = dist * tanHalf * aspect;
    gViewCentre = s.worldX + gViewHalf * 0.47f;                        // keeps the cat a quarter of the way in
    gCamera.position = Vector3{ gViewCentre, s.camY + 1.3f + 1.3f * dist / 9.8f + 0.05f * gJoyKick, dist };
    gCamera.target = Vector3{ gViewCentre, s.camY + 1.3f, 0.0f };
    gCamera.up = Vector3{ 0, 1, 0 };
    gCamera.fovy = 34.0f - 0.7f * gJoyKick;   // a tiny punch-in on every perfect hit
    gCamera.projection = CAMERA_PERSPECTIVE;

    gLight.camera = gCamera.position;
    gLight.lightDir = Vector3Normalize(Vector3{ -0.45f, 0.8f, 0.5f });
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
    const float horizonY = GetWorldToScreenEx(Vector3{ gCamera.target.x, s.camY + 2.3f, -24.0f }, gCamera, gVW, gVH).y;
    ClearBackground(BLACK);
    DrawSky(horizonY);

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

    JoyParticles(horizonY, true);
    if (gJoyKick > 0.02f) DrawRectangle(0, 0, gVW, gVH, Color{ 255, 244, 210, (unsigned char)(26.0f * gJoyKick) });   // a soft warm glow, not a flash
}

// ---------------------------------------------------------------------------
// UI (drawn on the low-resolution canvas with raylib's built-in pixel font)
// ---------------------------------------------------------------------------

static const Color kInk    = { 255, 246, 230, 255 };
static const Color kSoft   = { 255, 246, 230, 170 };
static const Color kShade  = { 30, 22, 44, 150 };
static const Color kPanel  = { 34, 26, 50, 214 };
static const Color kBorder = { 255, 246, 230, 70 };

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

// style: 0 quiet, 1 primary, 2 selected
static bool Button(int x, int y, int w, int h, const char* label, int style = 0)
{
    const Rectangle r = { (float)x, (float)y, (float)w, (float)h };
    const bool hot = CheckCollisionPointRec(Mouse(), r);
    Color bg, fg = kInk;
    if (style == 1) { bg = hot ? Color{ 255, 252, 244, 255 } : Color{ 255, 240, 214, 255 }; fg = Color{ 70, 46, 60, 255 }; }
    else if (style == 2) { bg = hot ? Color{ 255, 190, 120, 255 } : Color{ 246, 172, 104, 255 }; fg = Color{ 70, 40, 44, 255 }; }
    else bg = hot ? Color{ 60, 46, 76, 235 } : Color{ 44, 34, 62, 215 };
    DrawRectangle(x, y, w, h, bg);
    DrawRectangleLines(x, y, w, h, style == 0 ? kBorder : Color{ 70, 46, 60, 120 });
    Text(label, x + w / 2, y + (h - 10) / 2, 10, fg, 1, false);
    const bool clicked = hot && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    if (clicked) PlaySfx(SFX_CLICK);
    return clicked;
}

static void Panel(int w, int h, int& left, int& top, bool right = false)
{
    left = right && gVW >= w + 260 ? gVW - w - 20 : (gVW - w) / 2;
    top = (gVH - h) / 2;
    DrawRectangle(left, top, w, h, kPanel);
    DrawRectangleLines(left, top, w, h, kBorder);
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

static void DrawMenu()
{
    const int cx = gVW / 2;
    Text("ONIRIA", cx, 34, 50, kInk);
    Text("a cozy walk through your music", cx, 90, 10, kInk);

    const int y = gVH - 98;
    if (!gError.empty()) Text(gError.c_str(), cx, y - 16, 10, Color{ 255, 190, 190, 255 });
    if (Button(cx - 80, y, 160, 24, "Open a song", 1) || Key(KEY_O)) OpenSongDialog();
    Text("or drop an audio file on the window (MP3, OGG, WAV)", cx, y + 30, 10, kInk);
    if (Button(cx - 80, y + 46, 160, 20, "Play the demo track") || Key(KEY_ENTER)) BeginLoad("", true);
    Text("Esc pauses  -  F11 fullscreen", cx, gVH - 16, 10, kSoft);
}

static void DrawLoading()
{
    const int cx = gVW / 2;
    const char* msg = gJob.stage.load() >= 2 ? "Listening to the music..." : "Opening the song...";
    Text(msg, cx, 60, 10, kInk);
    Text(Fit(gJob.name, 10, gVW - 80).c_str(), cx, 76, 10, kSoft);
}

static bool Stepper(int left, int y, int w, const char* label, const char* value, int& delta)
{
    delta = 0;
    Text(label, left, y + 4, 10, kInk, 0, false);
    const int right = left + w;
    if (Button(right - 18, y, 18, 18, "+")) delta = 1;
    if (Button(right - 86, y, 18, 18, "-")) delta = -1;
    Text(value, right - 43, y + 4, 10, kInk, 1, false);
    return delta != 0;
}

static void DrawReady()
{
    int left, top;
    const int pw = 300, ph = 284;
    Panel(pw, ph, left, top, true);
    const int cx = left + pw / 2;
    const int x0 = left + 16, inner = pw - 32;

    Text(Fit(gSongName, 20, inner).c_str(), cx, top + 14, 20, kInk);
    const int secs = (int)gSong.duration;
    Text(TextFormat("%d:%02d  -  %d things on the path", secs / 60, secs % 60, (int)gSong.charts[gCfg.diff].time.size()), cx, top + 40, 10, kSoft, 1, false);

    const int dw = (inner - 8) / 3;
    for (int d = 0; d < DIFF_COUNT; d++)
        if (Button(x0 + d * (dw + 4), top + 60, dw, 22, kDiffName[d], gCfg.diff == d ? 2 : 0)) gCfg.diff = d;
    if (Key(KEY_LEFT)) gCfg.diff = std::max(0, gCfg.diff - 1);
    if (Key(KEY_RIGHT)) gCfg.diff = std::min(DIFF_COUNT - 1, gCfg.diff + 1);

    // What this difficulty asks of the player.
    const int actions = kActionCount[gCfg.diff], slot = 66;
    for (int a = 0; a < actions; a++) {
        const int x = cx - actions * slot / 2 + a * slot;
        ActionIcon(a, x + 10, top + 100);
        Text(TextFormat("%s %s", kActionKey[a], kActionName[a]), x + 22, top + 96, 10, kInk, 0, false);
    }

    int delta;
    if (Stepper(x0, top + 120, inner, "Walking speed", TextFormat("%.1fx", gCfg.speed), delta))
        gCfg.speed = std::clamp(std::round((gCfg.speed + delta * 0.1f) * 10.0f) / 10.0f, 0.6f, 2.0f);
    if (Stepper(x0, top + 142, inner, "Audio offset", TextFormat("%+d ms", gCfg.offsetMs), delta))
        gCfg.offsetMs = std::clamp(gCfg.offsetMs + delta * 5, -300, 300);
    if (Stepper(x0, top + 164, inner, "Sound effects", TextFormat("%d%%", gCfg.sfx * 10), delta))
        gCfg.sfx = std::clamp(gCfg.sfx + delta, 0, 10);

    const int tw = (inner - 4) / 2;
    if (Button(x0, top + 190, tw, 20, gCfg.watch ? "Watch mode: on" : "Watch mode: off", gCfg.watch ? 2 : 0)) gCfg.watch = !gCfg.watch;
    if (Button(x0 + tw + 4, top + 190, tw, 20, gCfg.hints ? "Key hints: on" : "Key hints: off", gCfg.hints ? 2 : 0)) gCfg.hints = !gCfg.hints;

    if (Button(cx - 70, top + 220, 140, 26, "Play", 1) || Key(KEY_ENTER) || Key(KEY_SPACE)) {
        SaveSettings();
        StartGame();
    } else if (Button(cx - 70, top + 252, 140, 18, "Choose another song") || Key(KEY_ESCAPE)) {
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
    Text("Paused", cx, top + 14, 20, kInk);
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
    const int pw = 300, ph = 236;
    Panel(pw, ph, left, top, true);
    const int cx = left + pw / 2;

    const float acc = Accuracy();
    const int rank = acc >= 0.95f ? 0 : acc >= 0.88f ? 1 : acc >= 0.78f ? 2 : acc >= 0.65f ? 3 : 4;
    static const char* kRank[5] = { "S", "A", "B", "C", "D" };
    static const char* kWords[5] = { "A purrfect evening", "A lovely walk", "A nice stroll", "A sleepy stroll", "More of a nap" };
    static const Color kRankColor[5] = { { 255, 236, 150, 255 }, { 170, 255, 205, 255 }, { 160, 226, 255, 255 }, { 220, 190, 255, 255 }, { 255, 170, 180, 255 } };
    Text(kRank[rank], cx, top + 12, 50, kRankColor[rank]);
    Text(kWords[rank], cx, top + 64, 10, kInk, 1, false);
    Text(Fit(gSongName, 10, pw - 32).c_str(), cx, top + 80, 10, kSoft, 1, false);
    Text(TextFormat("%d", gScore), cx, top + 98, 20, kInk);
    Text(TextFormat("%.1f%% in time  -  best streak %d", acc * 100.0f, gMaxCombo), cx, top + 124, 10, kInk, 1, false);

    static const char* kLabel[4] = { "Purrfect", "Nice", "Ok", "Miss" };
    const int colW = (pw - 32) / 4;
    for (int i = 0; i < 4; i++) {
        const int x = left + 16 + colW * i + colW / 2;
        Text(TextFormat("%d", gCount[i]), x, top + 144, 10, kInk, 1, false);
        Text(kLabel[i], x, top + 157, 10, kSoft, 1, false);
    }

    if (Button(cx - 70, top + 178, 140, 24, gCfg.watch ? "Watch again" : "Walk again", 1) || Key(KEY_ENTER) || Key(KEY_R)) {
        StartGame();
    } else if (Button(cx - 70, top + 208, 140, 18, "Back") || Key(KEY_ESCAPE)) {
        gState = ST_READY;
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
        case ST_RESULTS: DrawResults(); break;
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
    // --shot <png> [--view menu|ready|play|pause|results] [--at <seconds>] [--size WxH] [--diff 0..2]
    //                                 renders offscreen with a simulated clock and saves one frame
    const char* shotPath = nullptr;
    std::string songPath;
    std::string shotView = "play";
    double shotAt = 12.0;
    int winW = 1280, winH = 720, shotDiff = -1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--selftest")) return SelfTest();
        if (!strcmp(argv[i], "--sfx-demo") && i + 1 < argc) return WriteSfxDemo(argv[i + 1]) ? 0 : 1;
        if (!strcmp(argv[i], "--shot") && i + 1 < argc) shotPath = argv[++i];
        else if (!strcmp(argv[i], "--view") && i + 1 < argc) shotView = argv[++i];
        else if (!strcmp(argv[i], "--at") && i + 1 < argc) shotAt = atof(argv[++i]);
        else if (!strcmp(argv[i], "--diff") && i + 1 < argc) shotDiff = atoi(argv[++i]);
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
        PlatformStartKeyInput(GetWindowHandle());
    } else if (shotDiff >= 0) {
        gCfg.diff = std::clamp(shotDiff, 0, DIFF_COUNT - 1);
    }

    BuildShapes();
    UpdateCanvas();

    int shotFrames = 0, shotHold = -1;
    if (gSim) {
        gCfg.watch = true;
        if (shotView != "menu") {
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
        const float dt = gSim ? 1.0f / 60.0f : std::clamp(GetFrameTime(), 0.0005f, 0.1f);
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

        BeginTextureMode(gCanvas);
        DrawFrame(t);
        EndTextureMode();

        if (gSim) {
            shotFrames++;
            if (shotHold < 0) {
                const bool timed = shotView == "play" || shotView == "pause" || shotView == "results";
                if (gState == ST_MENU || !timed ? shotFrames >= 180 : gSimTime >= shotAt) {
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
    if (!gSim) {
        SaveSettings();
        PlatformStopKeyInput();
        if (gPlayer.hasStream) UnloadAudioStream(gPlayer.stream);
        CloseAudioDevice();
    }
    CloseWindow();
    return 0;
}
