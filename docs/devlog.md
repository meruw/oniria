# Devlog

Newest entries first.

## 2026-11-13: first prototype, and a change of direction

**Where it started.** The brief was a highly optimized rhythm game that builds itself from any song you give it, with a psychedelic look.

**Web or native.** The first plan was a single web page. It was dropped before any code was written: for a rhythm game the things that matter most are audio latency and input timing, and a native program controls both far better than a browser does. The stack became C++ with raylib, built with MSVC.

**What was built first.** The parts that do not depend on the look:

- Decoding and in-memory playback through an audio callback that timestamps every buffer.
- The analysis: spectral-flux onset detection in three bands, chart generation for three difficulties, envelopes for the visuals.
- A raw-input keyboard thread on Windows so key presses are timestamped independently of the frame rate.
- A synthesized demo track, which doubles as ground truth for testing: its notes sit on a known grid, so the analysis can be measured. The first measurement showed onsets detected 8 ms early; after correcting that constant the average error is 3 ms.
- Offscreen screenshot and self-test modes, so the game can be checked without a person at the keyboard.

**The psychedelic tunnel.** The first visual pass was a kaleidoscopic tunnel drawn in a fragment shader, with notes flying out of the centre along four lanes. It worked, and it did not look good: thin noisy outlines on a dark background, little colour, no sense of place. It was rejected outright.

**The new direction.** Simpler, cozy and nostalgic: a character walking along a path that the music builds, in low-poly, with cats. The inspiration is the idea of a minimalist walk through your own music; the look, the character and the objects are this game's own.

What changed:

- The tunnel and its shaders were removed. The new renderer has no custom shaders at all: flat-shaded triangles lit on the CPU, drawn into a low-resolution canvas.
- The four lanes became four actions (hop, bat, swat, ring), each with its own object on the path. Easier difficulties use fewer of them.
- The song now shapes the world: loudness becomes hills, and the day runs from afternoon to night over the length of the track.

Everything underneath (analysis, audio, input, loading, scoring) carried over unchanged.

**Open questions.**

- Nobody has played it with real audio and real key presses yet. Timing feel and the default latency are unverified.
- Charts have only been judged on the demo track. Real music, especially without drums, will need tuning.
- On the hardest difficulty the bell posts crowd the screen.
- The name predates the cats.
