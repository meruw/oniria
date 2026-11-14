#define _CRT_SECURE_NO_WARNINGS
#include "sfx.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace {

const double kTau = 6.283185307179586;
const double kC4 = 261.63, kC5 = 523.25, kE5 = 659.25, kG5 = 783.99, kC6 = 1046.50;

struct Partial { double ratio, amp, decay; };

// A struck or plucked tone: decaying sine partials behind a soft attack.
// Each partial is a rotating phasor with a decaying amplitude, so the inner
// loop is a handful of multiplies and no calls to sin or exp.
void AddTone(std::vector<float>& out, double start, double freq, double gain, const Partial* parts, int count, double attack = 0.004)
{
    const size_t first = (size_t)(start * kSfxRate);
    if (first >= out.size()) return;
    const size_t n = out.size() - first;
    std::vector<double> sum(n, 0.0);
    for (int p = 0; p < count; p++) {
        const double w = kTau * freq * parts[p].ratio / kSfxRate;
        const double cw = std::cos(w), sw = std::sin(w), fade = std::exp(-parts[p].decay / kSfxRate);
        double s = 0.0, c = 1.0, amp = parts[p].amp;
        for (size_t i = 0; i < n; i++) {
            sum[i] += amp * s;
            const double next = s * cw + c * sw;
            c = c * cw - s * sw;
            s = next;
            amp *= fade;
        }
    }
    const double rise = std::exp(-1.0 / (attack * kSfxRate));
    double closed = 1.0;   // 1 - attack envelope
    for (size_t i = 0; i < n; i++) {
        out[first + i] += (float)(sum[i] * gain * (1.0 - closed));
        closed *= rise;
    }
}

// Rounds off the top end, adds a quiet two-tap echo, and levels the result.
void Finish(std::vector<float>& s)
{
    // One-pole low-pass around 5.5 kHz: keeps the body, drops the fizz.
    float lp = 0.0f;
    for (float& v : s) { lp += (v - lp) * 0.51f; v = lp; }

    // Echo taps are filtered darker than the direct sound so they sit behind it.
    const std::vector<float> dry = s;
    const size_t d1 = (size_t)(0.13 * kSfxRate), d2 = (size_t)(0.27 * kSfxRate);
    float dark = 0.0f;
    for (size_t i = 0; i < s.size(); i++) {
        dark += (dry[i] - dark) * 0.18f;
        if (i + d1 < s.size()) s[i + d1] += dark * 0.20f;
        if (i + d2 < s.size()) s[i + d2] += dark * 0.08f;
    }

    float peak = 1e-6f;
    for (float v : s) peak = std::max(peak, std::fabs(v));
    const size_t fade = (size_t)(0.02 * kSfxRate);
    for (size_t i = 0; i < s.size(); i++) {
        const size_t left = s.size() - 1 - i;
        s[i] *= 0.85f / peak * (left < fade ? (float)left / (float)fade : 1.0f);
    }
}

std::vector<float> Buffer(double seconds) { return std::vector<float>((size_t)(seconds * kSfxRate), 0.0f); }

const Partial kMarimba[] = { { 1.0, 1.0, 7.0 }, { 4.0, 0.26, 24.0 }, { 9.2, 0.05, 60.0 } };
const Partial kThump[]   = { { 1.0, 1.0, 45.0 } };
const Partial kKalimba[] = { { 1.0, 1.0, 5.5 }, { 2.0, 0.30, 10.0 }, { 5.4, 0.10, 48.0 } };
const Partial kBell[]    = { { 1.0, 1.0, 2.6 }, { 1.004, 0.45, 2.6 }, { 2.0, 0.16, 4.5 }, { 2.76, 0.30, 6.5 }, { 5.4, 0.10, 15.0 } };
const Partial kMusicBox[] = { { 1.0, 1.0, 8.0 }, { 2.0, 0.14, 14.0 }, { 3.0, 0.05, 22.0 } };
const Partial kTick[]    = { { 1.0, 1.0, 60.0 }, { 2.0, 0.2, 90.0 } };

} // namespace

void BuildSfx(std::vector<float> out[SFX_COUNT])
{
    // Hop: a marimba bar with a little body thump under it.
    out[SFX_HOP] = Buffer(1.0);
    AddTone(out[SFX_HOP], 0.0, kC4, 1.0, kMarimba, 3);
    AddTone(out[SFX_HOP], 0.0, 92.0, 0.3, kThump, 1, 0.002);

    // Bat: a sine that slides up into its note, like a drop landing in water.
    out[SFX_BAT] = Buffer(0.8);
    {
        std::vector<float>& s = out[SFX_BAT];
        double phase = 0.0;
        for (size_t i = 0; i < s.size(); i++) {
            const double t = (double)i / kSfxRate;
            const double f = kC4 * (0.70 + 0.30 * (1.0 - std::exp(-t * 38.0)));
            phase += kTau * f / kSfxRate;
            const double v = std::sin(phase) * std::exp(-t * 10.0) + 0.16 * std::sin(2.0 * phase) * std::exp(-t * 18.0);
            s[i] = (float)(v * (1.0 - std::exp(-t / 0.003)));
        }
    }

    // Swat: a kalimba tine an octave up.
    out[SFX_SWAT] = Buffer(1.2);
    AddTone(out[SFX_SWAT], 0.0, kC5, 1.0, kKalimba, 3, 0.003);

    // Ring: a small bell with two slightly detuned fundamentals, for a slow warm shimmer.
    out[SFX_RING] = Buffer(2.0);
    AddTone(out[SFX_RING], 0.0, kC5, 1.0, kBell, 5, 0.005);

    // Twinkle: two music-box notes, a fifth then the octave.
    out[SFX_TWINKLE] = Buffer(1.1);
    AddTone(out[SFX_TWINKLE], 0.0, kG5, 1.0, kMusicBox, 3, 0.003);
    AddTone(out[SFX_TWINKLE], 0.07, kC6, 0.8, kMusicBox, 3, 0.003);

    // Miss: filtered noise and a falling low sine. Meant to be felt more than heard.
    out[SFX_MISS] = Buffer(0.5);
    {
        std::vector<float>& s = out[SFX_MISS];
        uint32_t seed = 22222u;
        float lp = 0.0f;
        double phase = 0.0;
        for (size_t i = 0; i < s.size(); i++) {
            const double t = (double)i / kSfxRate;
            seed = seed * 1664525u + 1013904223u;
            const float noise = (float)(seed >> 8) * (2.0f / 16777216.0f) - 1.0f;
            lp += (noise - lp) * 0.05f;
            phase += kTau * (80.0 + 40.0 * std::exp(-t * 25.0)) / kSfxRate;
            s[i] = (float)((lp * 2.5 * std::exp(-t * 28.0) + 0.6 * std::sin(phase) * std::exp(-t * 18.0)) * (1.0 - std::exp(-t / 0.004)));
        }
    }

    // Milestone: the music box runs up a major chord and lands on the octave.
    out[SFX_MILESTONE] = Buffer(2.2);
    {
        const double notes[5] = { kC5, kE5, kG5, kC6, kE5 * 2.0 };
        for (int i = 0; i < 5; i++) AddTone(out[SFX_MILESTONE], 0.09 * i, notes[i], i == 4 ? 0.7 : 0.85, kMusicBox, 3, 0.003);
        AddTone(out[SFX_MILESTONE], 0.36, kC5, 0.35, kBell, 5, 0.01);
    }

    // Click: a short wooden tick.
    out[SFX_CLICK] = Buffer(0.35);
    AddTone(out[SFX_CLICK], 0.0, 700.0, 1.0, kTick, 2, 0.002);

    for (int i = 0; i < SFX_COUNT; i++) Finish(out[i]);
}

float SfxStreakSemitones(int streak)
{
    static const int kScale[8] = { 0, 2, 4, 7, 9, 12, 14, 16 };   // major pentatonic, C to E an octave up
    static const int kTune[8]  = { 7, 6, 7, 5, 6, 7, 4, 6 };      // what a long streak keeps playing
    if (streak < 1) streak = 1;
    return (float)kScale[streak <= 8 ? streak - 1 : kTune[streak % 8]];
}

namespace {
bool WriteWav(const char* path, const std::vector<float>& samples)
{
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    const uint32_t dataBytes = (uint32_t)(samples.size() * 2), rate = kSfxRate, byteRate = rate * 2, riffSize = 36 + dataBytes, fmtSize = 16;
    const uint16_t pcm = 1, channels = 1, blockAlign = 2, bits = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&riffSize, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmtSize, 4, 1, f); fwrite(&pcm, 2, 1, f); fwrite(&channels, 2, 1, f);
    fwrite(&rate, 4, 1, f); fwrite(&byteRate, 4, 1, f); fwrite(&blockAlign, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&dataBytes, 4, 1, f);
    for (float v : samples) {
        const int16_t sample = (int16_t)(std::clamp(v, -1.0f, 1.0f) * 32767.0f);
        fwrite(&sample, 2, 1, f);
    }
    fclose(f);
    return true;
}
} // namespace

bool WriteSfxFiles(const char* folder)
{
    static const char* kNames[SFX_COUNT] = { "hop", "bat", "swat", "ring", "twinkle", "miss", "milestone", "click" };
    std::vector<float> sfx[SFX_COUNT];
    BuildSfx(sfx);
    bool ok = true;
    for (int i = 0; i < SFX_COUNT; i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s.wav", folder, kNames[i]);
        ok = WriteWav(path, sfx[i]) && ok;
    }
    return ok;
}

bool WriteSfxDemo(const char* path)
{
    std::vector<float> sfx[SFX_COUNT];
    BuildSfx(sfx);

    std::vector<float> mix((size_t)(9.5 * kSfxRate), 0.0f);
    auto play = [&](int sound, double at, float semitones, float gain) {
        const double step = std::pow(2.0, semitones / 12.0);
        const std::vector<float>& s = sfx[sound];
        const size_t first = (size_t)(at * kSfxRate);
        for (size_t i = 0; first + i < mix.size(); i++) {
            const double pos = i * step;
            const size_t k = (size_t)pos;
            if (k + 1 >= s.size()) break;
            mix[first + i] += (s[k] + (s[k + 1] - s[k]) * (float)(pos - k)) * gain;
        }
    };

    // Sixteen perfect hits in a row, cycling through the four actions, then a milestone and a miss.
    for (int n = 0; n < 16; n++) {
        const double at = 0.3 + n * 0.32;
        const float semis = SfxStreakSemitones(n + 1);
        play(n % 4, at, semis, 0.55f);
        play(SFX_TWINKLE, at, semis, 0.22f);
    }
    play(SFX_MILESTONE, 5.7, 0.0f, 0.4f);
    play(SFX_MISS, 8.2, 0.0f, 0.3f);

    FILE* f = fopen(path, "wb");
    if (!f) return false;
    const uint32_t dataBytes = (uint32_t)(mix.size() * 2), rate = kSfxRate, byteRate = rate * 2, riffSize = 36 + dataBytes, fmtSize = 16;
    const uint16_t pcm = 1, channels = 1, blockAlign = 2, bits = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&riffSize, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmtSize, 4, 1, f); fwrite(&pcm, 2, 1, f); fwrite(&channels, 2, 1, f);
    fwrite(&rate, 4, 1, f); fwrite(&byteRate, 4, 1, f); fwrite(&blockAlign, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&dataBytes, 4, 1, f);
    for (float v : mix) {
        const int16_t sample = (int16_t)(std::clamp(v, -1.0f, 1.0f) * 32767.0f);
        fwrite(&sample, 2, 1, f);
    }
    fclose(f);
    return true;
}
