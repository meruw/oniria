// Offline music analysis: turns decoded PCM into playable charts and
// per-frame envelopes that drive the visuals. No dependency on raylib.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

enum Difficulty { DIFF_GENTLE = 0, DIFF_NORMAL = 1, DIFF_INTENSE = 2, DIFF_COUNT = 3 };

// Notes sorted by time. Chord members share the same time and sit next to each other.
struct Chart {
    std::vector<double>  time;   // seconds from the start of the song
    std::vector<uint8_t> lane;   // 0..3
};

struct SongAnalysis {
    Chart charts[DIFF_COUNT];

    // Envelopes sampled every `hop` seconds; sample i describes time envOffset + i * hop.
    std::vector<float> low, mid, high;   // band loudness, roughly 0..1
    std::vector<float> pulse;            // bass onset strength, 0..1 (kick drum hits)
    double hop       = 0.008;
    double envOffset = 0.0;
    double duration  = 0.0;
};

// `pcm` is interleaved 16-bit audio. Returns false if the audio is too short to analyze.
bool AnalyzeSong(const int16_t* pcm, size_t frames, int channels, int sampleRate, SongAnalysis& out);

// Built-in synthesized track (stereo interleaved), so the game is playable without any file.
void SynthDemoTrack(std::vector<int16_t>& stereo, int& sampleRate);

// Length of a sixteenth note in the demo track; used by the timing self-test.
double DemoSixteenthSeconds();
