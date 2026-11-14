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

## Places

A place (biome) is a small set of choices: ground and path colour, a sky tint, which props grow in each of four depth bands (beside the path, middle distance, far away, in front), and a flat 2D weather layer (snow, petals, leaves, seagulls).

`BiomeAt(x)` answers "where are we at world position x" as a blend between two places. During a song the path is divided into equal stretches, one per roughly thirty seconds of music up to five; the first place comes from a hash of the song's name and each next one is two steps further round the list, which visits all five and never puts similar places side by side. The last nine units of a stretch blend into the next: ground colours interpolate, props switch at the midpoint. In the menus the same function runs on the idle walk, so the background strolls through every place.

Every place has three variations. A stop on the journey is a place plus a variation; the variation simply counts up from a seed taken from the song's name, so returning to a place shows a different one, and a song fixed to one place cycles through all three. A variation changes the prop mix of each depth band and brings its own landmarks: a windmill whose blades turn, fences that follow the slope, a string of paper lanterns that sags from pole to pole, a pier, a lighthouse whose lamp turns after dark.

Three variations (riverside, stream, frozen river) add a river: the terrain is pressed flat between two rows behind the path, those cells are drawn as water or ice, and the middle band of props moves up onto the near bank. Things on the water (ducks, lilies, koi, bridges) are placed at the river's level instead of on the ground.

Each place also has a flat backdrop drawn behind the hills: balloons, a mountain, a flight of birds, a range of peaks with an aurora at night, islands.

The time of day is applied on top as a shading factor, so every place goes from afternoon to night without needing its own palette keys. Only part of the palette's colour cast is used for that factor, otherwise pale ground such as snow turns violet at night.

The seaside is the one place that changes the terrain: behind the path the ground runs down to a fixed sea level, and the rows beyond the shoreline are drawn as water with bands of lighter colour drifting across.

## Camera

The camera rests a quarter of the view ahead of the cat. Four spring-damped values sit on top of that rest pose: sideways, vertical, roll and zoom. A hit adds velocity to them depending on the action (up for a hop, forward and in for a bat, up and tilted for a swat, a roll for a bell), scaled by how accurate the hit was, and the springs bring them back in about a quarter of a second. They are integrated in four sub-steps per frame so a slow frame cannot make them unstable. A milestone adds a larger zoom kick, a miss a small wobble. The camera also zooms a fraction of a degree on each kick drum and drifts slowly once the streak is high.

Screen-space effects are drawn after the 3D pass: streaks radiating from the projected hit position, and four gradients along the edges of the canvas in the action's colour.

## Song library

`oniria.songs`, next to the executable, holds one tab-separated line per song: path, name, length, and best accuracy and score for each difficulty. A song moves to the top when it is loaded. Results are only recorded when a person played (not in watch mode). The built-in track has an entry too, under a placeholder key.

## The cat

All seven cats are the same skeleton built by `BuildCat` from a small description: a coat pattern (solid, tabby, tuxedo, calico, point), six colours and three proportions. The shapes are rebuilt when the player picks another cat; nothing is stored per cat.

Body, head, legs and tail are lofts: a row of elliptical rings skinned into a closed surface. A colour callback decides each quad from its position along the loft and how high on the ring it sits, which is enough to express every coat: tabby stripes are alternate spans on the upper faces, a tuxedo is the underside and the front of the chest, calico patches come from a hash of the quad, and siamese points are the last spans of the head plus the legs and tail.

The parts are modelled around their own joints so they can be posed: the head around the neck, each leg as a thigh and a shin with a paw, the tail as five tapering segments. Walking moves diagonal pairs of legs together and lifts each paw as its leg swings forward; a wave runs down the tail from base to tip. The cat is drawn with its shading pulled partly towards its true colours, and the scene's light comes from ahead of it, so its face is never in shadow.

## Progress and unlocks

There is no currency and no list of unlocked things on disk. `oniria.save` holds eight lifetime numbers (songs finished, perfect hits, best streak, best rank, songs without a miss, minutes walked, a bit per place visited, a bit per item already seen), and whether an item is unlocked is computed from them each time it is asked. Every item has exactly one condition from a small set, so the table in the code is the whole design, and changing a requirement cannot corrupt anyone's save.

When a song ends, the unlock state of every item is taken before and after adding that song's numbers; the difference is what the results screen announces. Watch mode never counts.

What the cat wears is three more shapes built by `BuildCat`: one in head space (so hats follow the head as it turns and nods), and two in body space. Wings and the balloon are posed separately so they can flutter and sway.

## Staying light

- **Frame pacing.** The game sleeps on a high-resolution waitable timer until the next frame is due: 120 frames a second while playing, 60 in menus, 30 in the background, and no drawing at all while minimized. V-sync is still requested, but it is not relied on: a driver can switch it off, and the first measurement of the menu showed the loop spinning more than a whole core for exactly that reason. raylib's own limiter was avoided because it busy-waits part of every frame.
- **Memory.** A song is decoded once into 16-bit stereo and that buffer is used directly for playback; it is never copied. The analysis reads it in place through a sliding window instead of first building a mono copy of the whole track.
- **Startup.** Sound effects are synthesized with phasor recurrences rather than calling `sin` and `exp` per sample.
- **Drawing.** A few thousand flat triangles lit on the CPU, into a 640x360 canvas, in a handful of batches. No textures, no model files, no custom shaders.

## Sound effects

`src/sfx.cpp` builds eight short sounds at startup from decaying sine partials: a marimba bar, a pitch-sliding "bloop", a kalimba tine, a small bell with two detuned fundamentals, a two-note music-box twinkle, a muted thud for misses, a music-box arpeggio for milestones and a menu tick. Each one goes through a low-pass filter around 5.5 kHz and gets a quiet, darker two-tap echo, which is what keeps them soft.

They are mixed inside the same audio callback that plays the song, so they have the same latency as the music. The game thread only pushes small commands (sound, pitch, gain) onto a lock-free queue; up to sixteen voices are resampled and summed on the audio thread.

The hit sound's pitch depends on the streak: the first eight hits climb a major pentatonic scale, and from then on a fixed eight-note figure near the top keeps repeating, so a long streak sounds like a tune and never keeps rising. All four instruments share the same pitch, so two notes at once always sound in unison or an octave apart.

## Happiness

Every hit adds to a streak value (a perfect counts 1, a nice 0.6, an ok 0.3) and a miss empties it. A single number, `gJoy = 1 - exp(-streak / 18)`, eased over time, drives everything festive:

- the cat: bounce in its step, a hop on each beat, tail up and wagging faster, blush, smiling eyes, hearts, a trail of stars;
- the ground: flowers spawned ahead of the cat with a probability that follows the number, in a ring buffer so they stay where they bloomed;
- the scenery: how far trees squash and sway on the beat, lamps lit even by day, small pastel butterflies;
- the sky: sun or moon rays, drifting lights, petals, and a rainbow above 0.65;
- colour: a saturation and ambient boost applied in the per-triangle lighting.

A perfect hit additionally sets a short pulse that stretches the cat, nudges the camera in by under a degree and lays a faint warm glow over the frame.

## Checking it without playing

Two command-line modes exist for automated checks:

- `oniria.exe --selftest` analyzes the demo track and prints note counts and timing error against its known grid, plus the length and level of every sound effect.
- `oniria.exe --sfx-demo out.wav` writes sixteen perfect hits in a row, a milestone and a miss, for listening to the sounds outside the game.
- `oniria.exe --shot out.png [--view menu|ready|play|pause|results|wardrobe|settings] [--at seconds] [--diff 0..2] [--scenery 0..5] [--variant 0..2] [--cat 0..6] [--hat n] [--neck n] [--back n] [--tab 0..3] [--unlock-all] [--count] [--closeup] [--size WxH] [song file]` renders offscreen with a simulated clock, in watch mode, and saves one frame. Without a song file it uses the demo track.
