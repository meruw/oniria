# How it works

This document describes the prototype as it is in the code today.

## From a file to a level

Everything in `src/analysis.cpp` runs once per song, on a worker thread, and does not depend on raylib.

### 1. Preparing the signal

The decoded audio (16-bit stereo) is mixed to mono and decimated by an integer factor to roughly 22 kHz. Rhythm lives well below 10 kHz, so nothing useful is lost and the rest of the analysis does half the work.

### 2. Spectral flux

The signal is cut into 1024-sample windows, one every 8 ms, and each goes through an FFT. For every frequency bin we take the log of the magnitude and compare it with the previous frame; the sum of all the *increases* is the spectral flux. A peak in the flux means energy just appeared, which is what the start of a drum hit or a note looks like.

Flux is computed separately for three bands:

| Band | Range | Typically |
| --- | --- | --- |
| Low | up to 180 Hz | kick drum, bass |
| Mid | 180 Hz to 2.5 kHz | snare, vocals, melody |
| High | 2.5 to 10 kHz | hi-hats, cymbals |

Alongside it we keep the spectral centroid of the energy that appeared (a rough "how high-pitched was this sound") and each band's loudness.

### 3. Picking onsets

A frame is an onset candidate when it is a local maximum of a band's flux and stands out from the average of the surrounding second. Its strength is how much it stands out. Candidates that several bands report within 45 ms are merged into one.

The log compression makes peaks appear slightly before the centre of the analysis window reaches the attack. That bias was measured against the demo track, where the true note times are known, and is corrected by a constant (`kOnsetBias`, 8 ms). After the correction detected onsets sit 3 ms from the grid on average.

### 4. Building the charts

Each difficulty takes the strongest candidates first, skipping any that would land too close to one already taken, until it reaches its target density:

| Difficulty | Min gap | Target density |
| --- | --- | --- |
| Cozy | 0.42 s | 1.2 per second |
| Playful | 0.24 s | 2.2 per second |
| Zoomies | 0.13 s | 3.6 per second |

Notes are then spread over four lanes by pitch: the centroid's quartiles decide the lane, so similar sounds land together. The mapping is reshuffled every sixteen notes, and fast repeats of the same lane are nudged to a neighbour, to keep patterns moving. On the harder difficulties, the strongest accents with room around them become two notes at once.

The game maps lanes to actions per difficulty (`kLaneAction` in `main.cpp`) in a way that guarantees a double always pairs something on the ground with something in the air.

### 5. The landscape

The path height is the song's loudness averaged over four-second windows, so quiet passages are valleys and loud ones are hilltops. The band loudness and the bass flux are also kept as envelopes, sampled every 8 ms, which the scene reads while playing: trees bop on the kick, stars and fireflies react to the highs.

## Keeping time

A rhythm game is only as good as its clock, so there are three pieces:

- **Audio.** The whole song is in memory and is fed to the device from raylib's audio thread callback. Each callback records the playback position together with a timestamp. Between callbacks the position is extrapolated from that pair.
- **Game clock.** The game runs its own smooth clock and nudges it 2% of the way towards the audio position every frame (or snaps if it is more than 50 ms off). That removes the stair-stepping of the audio buffer without letting the two drift.
- **Input.** On Windows a small thread owns a message-only window registered for Raw Input, so every key change gets a timestamp the moment the OS delivers it. A hit is judged at the song time of that timestamp, not at the time of the frame that noticed it. If raw input never delivers anything, the game falls back to polling once per frame.

Output latency cannot be measured reliably, so the game assumes 30 ms and exposes an offset setting.

Hit windows are 45 ms (purrfect), 90 ms (nice) and 140 ms (ok) either side of the note.

## Drawing

The renderer is deliberately small.

- A **shape** is a list of triangles with one colour each. Shapes are built in code from boxes, cones, cylinders and stretched icosahedra; there are no model or texture files.
- Each frame, every visible triangle is transformed on the CPU, lit with one directional light plus a sky-tinted ambient term, fogged by distance, and sent to raylib's batch with the final colour baked in. That is a few thousand triangles, which costs next to nothing and needs no custom shader.
- The frame is drawn into a canvas about 360 pixels tall (never narrower than 480) and scaled up by a whole number of pixels, which gives the chunky look and keeps the fill cost tiny on any GPU.
- The camera adapts to the window shape: in narrow windows it pulls back so the same stretch of path stays visible ahead of the cat.
- Sky, sun, moon, stars, clouds and far hills are flat 2D shapes behind the 3D scene. Lamps, lit windows and fireflies are translucent discs blended on top.
- The time of day is a palette interpolated between four keys (afternoon, sunset, dusk, night) by the song's progress.
- Scenery is placed by hashing the position along the path, so the same song always produces the same walk.

## Checking it without playing

Two command-line modes exist for automated checks:

- `oniria.exe --selftest` analyzes the demo track and prints note counts and timing error against its known grid.
- `oniria.exe --shot out.png [--view menu|ready|play|pause|results] [--at seconds] [--diff 0..2] [--size WxH] [song file]` renders offscreen with a simulated clock, in watch mode, and saves one frame. Without a song file it uses the demo track.
