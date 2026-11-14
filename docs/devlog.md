# Devlog

Newest entries first.

## 2026-11-13: sound, and a world that gets happier

Two requests: hit sounds that feel very satisfying but stay cozy, and a visible reward for every perfect hit that keeps growing for as long as the streak lasts.

**Sound.** All synthesized, no files. The palette is mallets, plucks and a music box, low-passed and given a quiet echo. Each action has its own instrument, and the pitch climbs a pentatonic scale with the streak before settling into a small repeating tune, which turns a good run into a melody on top of the song. Effects are mixed in the song's own audio callback so they are as tight as the music.

**Every perfect hit** gets a music-box twinkle, a burst of stars, a ring of light spreading on the path, two new flowers, a stretch from the cat and a faint warm glow over the screen.

**The streak** feeds one happiness value that everything listens to: the cat prances, blushes, smiles and sheds hearts; flowers spring up ahead of it; trees dance harder; lamps light up; lights and petals drift across the sky; a rainbow appears. Milestones at 10, 25, 50 and 100 get a banner and a fanfare. A miss drains it all within a fraction of a second, which is the point: the contrast is what makes the streak worth protecting.

Decisions worth recording:

- Decorations stay behind the path or beside the cat. Flowers in front of the path are kept short so nothing ever covers an object that has to be read.
- The perfect-hit glow is a few percent of brightness, not a flash.
- The effects have been checked for level and length only. Nobody has listened to them in the game yet, so the instrument choices are a first guess.

## 2026-11-13: polish pass on the cat world

The cozy direction was approved, so this pass cleaned up what the first screenshots showed:

- **Bells.** Each bell hung from a dark wooden post, and on the hardest difficulty the posts crowded the screen. Bells now hang from tall green flower stalks that arch over the path. They blend into the grass, so the bells stand out and the stalks do not.
- **Where to hit.** A soft spot of light on the path, just ahead of the cat, marks the point where objects should be answered. It pulses with the kick drum.
- **The cat.** It blinks, and squints for a moment after a perfect hit.
- **Any window shape.** The camera pulls back in narrow windows so there is always the same amount of path ahead of the cat, the ground extends further towards the camera, and the canvas is never narrower than 480 pixels so the menus always fit. Checked at 16:9, ultrawide, portrait and the 480x360 minimum.
- **Starting with a song.** `oniria.exe song.mp3` opens the game with that file loaded, which also lets the screenshot mode test real files.

Measured while doing this: a full simulated run of the demo track at a 1920x1080 window (about 2760 frames, hardest difficulty) renders in roughly 2.5 seconds including startup, under a millisecond per frame.

FLAC turned out not to be compiled into the prebuilt raylib release, so it was removed from the file dialog and the docs.

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
- The name predates the cats.
