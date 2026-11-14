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

### 4b. Long notes and breathers

A second pass shapes each chart once the loudness envelopes exist.

About once every seven to nine seconds (depending on the difficulty) it looks for the note with the most *sustained* sound after it: the summed band envelopes must stay above 55% of their level just after the onset for the whole length of the long note (1.0 to 1.4 s). The best candidate in each window becomes a long note, and the notes it would overlap are dropped. Music with no sustained sounds simply gets none.

Then it looks for stretches of 45 seconds without a two-second gap, and opens one: the notes in the quietest 2.6 seconds of the stretch are dropped.

In the game every long note is yarn, whatever lane it came from, so there is only ever one at a time and it is always the same key. Holding pays a few points every 150 ms and a bonus at the end; releasing early just stops the points. There is deliberately no way to fail one.

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

## Fish, levels and unlocks

When a walk ends (never in watch mode) it pays fish and experience. Both are computed from the same three things: the length of the song in minutes, the accuracy, and a small factor for the difficulty (1.0, 1.15, 1.3).

- Experience is `10 + minutes x 100 x accuracy x difficulty`.
- Fish is `5 + minutes x 30 x accuracy^2 x difficulty`, plus 20, 12, 6 or 3 for an S, A, B or C, plus 15 for a walk with no misses and 5 for a new best on that song.

Paying by time rather than by notes hit means a long relaxed walk on Cozy earns about as much as a short busy one on Zoomies, so nobody is pushed towards a difficulty they do not enjoy. Accuracy is squared for fish so that playing well matters more than leaving the game running.

The level is not stored; it is derived from the experience. Going from level `n` to the next takes `150 + 75 x (n - 1)`, and arriving at level `n` pays `20 + 5 x n` fish.

Every item in the table in the code is one of three kinds: owned from the start, bought for a price, or the reward for a level. `oniria.save` holds the fish, the experience, one bit per item bought, one bit per item already seen in the wardrobe, and a handful of lifetime numbers shown on the Progress tab. Whether a level reward is unlocked is computed from the experience each time it is asked, so changing a level requirement can never corrupt a save.

In the wardrobe, the model normally wears what is equipped. Selecting something that is not owned rebuilds the model wearing it, without touching the settings, which is all "trying it on" is.

What the cat wears is three more shapes built by `BuildCat`: one in head space (so hats follow the head as it turns and nods), and two in body space. Wings and the balloon are posed separately so they can flutter and sway.

The head is close to an ellipsoid, so hats are lofts of ellipses that follow it: a brim is a ring at the height where the head is that wide, a beanie's cuff hugs it just above the eyes. The head is also so large that a level collar would be hidden inside it, so everything worn round the neck lies in a plane that leans forward, high behind the head and low under the chin, where it shows as a curve across the chest. Things on the back sit behind the point where the head stops covering it.

## Audio calibration

The offset between when sound leaves the program and when it reaches the ears cannot be measured from inside, so the player measures it. The audio thread runs a metronome itself: it starts a tick every 0.6 s at an exact sample and records the time that sample went out. The player taps along with the game keys, which come through the same timestamped input path as in play. Each tap is compared with the nearest tick (plus the same base latency the game assumes while playing), and the median of sixteen taps is the offset. The median, not the mean, so one stray tap does not matter; the median distance from the median is shown as a hint when the taps were uneven. Nothing visual moves with the tick on purpose, since the screen has its own, different delay.

## The guided walk

*How to play* is not a separate mode. It loads the built-in track through the normal path, replaces the analysed charts with a hand-written one (six parts of sixteen beats, the first four beats of each left empty for reading), and starts the game with a flag set. The flag makes the chart's lanes mean actions directly, shows the line of advice, and adds one rule: when a part ends with fewer than half its things answered, those notes are reset and playback seeks back to the start of the part, twice at most.

## Today's goals

The three goals are a function of the date: the day number seeds a small generator that picks three different kinds out of seven and a smaller or larger version of each. Progress towards them is a handful of numbers for the current day in `oniria.save`; when the date changes they are cleared. No clock is trusted beyond the local date, and nothing is fetched.

## The cat's room

A fixed diorama: floorboards, three walls, a window whose glass is drawn in the colour of the sky outside, and one object for each of seven spots, all built from the same boxes, blobs, cones and beams as the rest of the world into a single shape that is rebuilt only when a choice changes. The cat is drawn with the same code as outdoors with the ground forced level. What is owned is one bit per choice, and what is placed is two bits per spot.

## Gamepad

raylib reports the first connected pad in a standard layout. While playing, its d-pad and face buttons are the four actions and are read once a frame, so unlike the keyboard they are stamped with the frame's time (at 120 frames a second that is at most 8 ms late). In menus they stand in for the arrow keys, Enter and Esc.

## Staying light

- **Frame pacing.** The game sleeps on a high-resolution waitable timer (one per thread) until the next frame is due: 120 frames a second while playing, 60 in menus, 30 in the background, and no drawing at all while minimized. V-sync is still requested, but it is not relied on: a driver can switch it off, and the first measurement of the menu showed the loop spinning more than a whole core for exactly that reason. raylib's own limiter was avoided because it busy-waits part of every frame.
- **Memory.** A song is decoded once into 16-bit stereo and that buffer is used directly for playback; it is never copied. The analysis reads it in place through a sliding window instead of first building a mono copy of the whole track.
- **Startup.** Sound effects are synthesized with phasor recurrences rather than calling `sin` and `exp` per sample.
- **Drawing.** A few thousand flat triangles lit on the CPU, into a 640x360 canvas, in a handful of batches. No textures, no model files, no custom shaders.

## Sound effects

`src/sfx.cpp` builds eight short sounds at startup from decaying sine partials: a marimba bar, a pitch-sliding "bloop", a kalimba tine, a small bell with two detuned fundamentals, a two-note music-box twinkle, a muted thud for misses, a music-box arpeggio for milestones and a menu tick. Each one goes through a low-pass filter around 5.5 kHz and gets a quiet, darker two-tap echo, which is what keeps them soft.

They are mixed inside the same audio callback that plays the song, so they have the same latency as the music. The game thread only pushes small commands (sound, pitch, gain) onto a lock-free queue; up to sixteen voices are resampled and summed on the audio thread.

A loud song with a few effects on top can pass full scale. The last step of the callback leaves everything below 0.8 untouched and eases anything above it towards 1.0 with a `tanh` curve, so peaks are rounded off instead of clipped flat.

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
- `oniria.exe --sfx-dump folder` writes each sound effect as its own WAV, for measuring them.
- `oniria.exe --robot log.txt --calibrate` runs the calibration screen and taps along 45 ms late on purpose; `--robot log.txt --lesson` takes the guided walk, ignoring its first part once; `--day n` pretends it is day `n` for the daily goals.
- More `--shot` views: `calibrate` (with `--tab 0..2` for the phase), `tutorial`, `home` (with `--home 1232103` for what is placed, `--home-tab`, `--home-sel`), and `--today` for a half-finished day on the menu.
- `oniria.exe --robot log.txt [--diff 0..2] [song file]` is the end-to-end test. The game runs for real, with its window, its audio thread and its raw input thread, but with the master volume at zero. A second thread waits for the song screen, presses Enter, and then presses D F J K through `SendInput` at the times the chart asks for, deliberately a little early or late on some notes and skipping every twenty-fifth; a third of the way in it pauses with Esc and resumes with Enter. Keys are only sent while the game window is in front, so they cannot land in another program. When the song ends the game writes what it measured: for each note the timing that was meant, when the key was really sent and what the game measured; then totals for input, audio, clock drift, frame times, camera travel, what the walk paid and what was saved. After the song it goes back to the menu, opens the wardrobe, moves down to the first hat for sale, checks that the model is trying it on, buys it with Enter if there are fish enough, and logs the fish and what is owned before and after. It does not write the settings file.
- `oniria.exe --shot out.png [--view menu|ready|play|pause|results|wardrobe|settings] [--at seconds] [--diff 0..2] [--scenery 0..5] [--variant 0..2] [--cat 0..6] [--hat n] [--neck n] [--back n] [--tab 0..3] [--unlock-all] [--count] [--closeup] [--size WxH] [--seq n] [--seq-step frames] [--fish n] [--xp n] [--sel n] [song file]` renders offscreen with a simulated clock, in watch mode, and saves one frame, or with `--seq` a run of frames a few steps apart (`out_0.png`, `out_1.png`, ...) for looking at motion. Without a song file it uses the demo track.
