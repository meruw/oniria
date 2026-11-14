// Synthesized sound effects. Everything is generated at startup; there are no
// audio files. The palette is deliberately soft: mallets, plucks and a music
// box, rounded off with a low-pass filter and a short warm echo.
#pragma once

#include <vector>

// The first four match the game's four actions, in the same order.
enum Sfx {
    SFX_HOP,         // soft marimba note (log)
    SFX_BAT,         // round woolly "bloop" (yarn)
    SFX_SWAT,        // kalimba pluck (butterfly)
    SFX_RING,        // handbell struck with a soft mallet (bell)
    SFX_TWINKLE,     // two music-box notes layered on perfect hits
    SFX_MISS,        // muted felt thud
    SFX_MILESTONE,   // music-box arpeggio for streak milestones
    SFX_CLICK,       // menu tick
    SFX_COUNT
};

const int kSfxRate = 48000;   // mono float samples at this rate

void BuildSfx(std::vector<float> out[SFX_COUNT]);

// Pitch offset for the hit sound at a given streak length. It climbs a
// pentatonic scale for the first hits, then keeps playing a little tune near
// the top, so a long streak sounds like a melody instead of a siren.
float SfxStreakSemitones(int streak);

// Renders a short audition (a rising streak, a milestone, a miss) to a 16-bit WAV.
bool WriteSfxDemo(const char* path);

// Writes every sound on its own (hop.wav, bat.wav, ...) into `folder`, which must exist.
bool WriteSfxFiles(const char* folder);
