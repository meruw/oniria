# Devlog

Newest entries first.

## 2026-11-13: six things that were missing

A brainstorm produced six gaps, and this pass fills them.

**Calibration by ear.** The audio offset was a number to adjust by hand. Now a tick plays and the player taps along. The tick is started by the audio thread itself at an exact sample, because a tick started from the game loop would carry the jitter of the frames, which is the size of the thing being measured. Tested by tapping 45 ms late on purpose: the game measured +45.5 ms.

**A guided first walk.** One part per key, then all together, then long notes, over the built-in track. The part that mattered was "the music waits for you": rather than stopping the song, a part that goes mostly unanswered is rewound and played again. That needed a seek in the middle of a song, which had never been exercised; the end-to-end run that ignores the first part shows the timing after the seek is as tight as before it (0.4 ms).

**Long notes and breathers.** Charts were all single taps. The analysis now turns notes over sustained sound into long ones and thins what they overlap, and opens a gap in stretches that never rest. On the demo track that is five long notes and 17 fewer taps on Playful. In the game they are all yarn with a thread to reel in, and they cannot be failed: letting go early only forgoes a bonus.

**Daily goals.** Three a day, chosen from the date. The first version did not count the places walked through, because the day's numbers were only set up when the menu was drawn; the end-to-end run, which never shows the menu, caught it.

**The cat's room.** Something for fish to go to after the wardrobe: seven spots, 21 things to buy, 4210 fish in all. The first render showed an empty purple screen: the room skipped the call that clears the depth buffer, so everything failed the depth test against the previous frame. The bed you start with is a cardboard box.

**Getting it, and other inputs.** GitHub Actions builds every push on Windows and Linux and publishes a release on a version tag, so nobody needs a compiler to try it. A gamepad now works for play and menus. The Linux build is honest about being experimental: it shares the game code but has no timestamped key input. Both builds compiled and passed the self-test on the first automatic run; on Linux that is all that has been checked.

**Not verified:** the gamepad (no pad here to press), and as always, how any of this feels to a person.

## 2026-11-13: fish and levels

The previous entry measured that the unlock conditions went by too fast, and the entry before it said a cozy game should not have an economy. The second opinion lost: unlocking by one-off conditions gave no reason to come back to a song, and what was wanted is a game that rewards playing again and again.

**Fish.** Every finished walk pays fish, and ten of the things to wear are now bought with them, from 80 for the flower crown to 400 for the top hat (1950 for everything). Pay follows the length of the song and the accuracy, with small bonuses for rank, a clean walk and a new best. It deliberately does not follow the number of notes, so Cozy pays nearly what Zoomies does.

**Levels.** Experience comes from the same walks and raises a level. Each level pays a few fish, and ten levels between 2 and 20 bring a reward that fish cannot buy: the four cats that were locked, and the bow tie, party hat, headphones, wings, crown and halo. The results screen counts the fish up and fills the bar, rolling over with a little fanfare when a level is reached; the menu always shows both.

**Trying things on.** Selecting something not owned puts it on the cat without equipping it, so a price is never paid blind. The model now wears "what it is told to" rather than reading the settings directly, which is what made that a few lines.

**Numbers.** A three-minute song at 92% on Playful pays about 105 fish and 330 experience. Level 5 is three or four such songs away, level 10 about twelve, level 20 about fifty; buying everything takes roughly fifteen. These are first guesses and easy to move: they are six constants and one table.

**Checked.** The end-to-end test now goes shopping after the song. With a save seeded at 100 fish: the walk paid 41 fish and 96 experience, reached level 2 (30 more fish and the bow tie), the wardrobe opened, the flower crown was tried on while not owned, Enter bought it (171 fish to 91), and the save file held all of it. The first attempt at this run never started the song: the Enter that should have started it did not reach the game, for a reason that was not found. The test now retries that key press, and the run after that was clean.

**A small fix on the way.** Screenshot mode wrote the settings file when a simulated song ended. It no longer writes anything.

## 2026-11-13: clothes that fit, and a test that presses real keys

**Cosmetics.** Hats were placed by eye at fixed heights and several sank into the head or hovered over it; the beanie's cuff was a plate. They are now built from ellipses that follow the head's own shape. Collars, the bow tie, the scarf, the bandana and the garland were level rings, which the oversized head simply hid; they now lie in a plane that leans forward, so the front half shows under the chin. The backpack was a box mostly behind the head; it is a rounded bag further back, with a flap, pockets and a button. The scarf's hanging end reached the paws and looked like a post; it is a short tail on the chest. Wings got some colour so they show on the white cat.

**An end-to-end test.** Every devlog entry so far ended with "not checked with real input and real audio". `--robot` closes most of that: the game runs normally but silently, and a thread presses real keys through the operating system following the chart, some on time, some deliberately late, some skipped, with a pause and resume in the middle. Four runs, 492 key presses:

- every key arrived through the raw input thread; on the two runs that logged it (306 presses), the time the game gave each press was 0.5 ms from when it was sent on average and 1.5 ms at worst;
- all 492 hits were judged in the window their timing falls in, and the 18 misses were exactly the 18 planned ones;
- the audio callback ran 100 times a second and delivered the whole song; the game clock stayed 0.2 ms from the audio thread on average (6 to 10 ms at its worst moment in each run) and never had to resync;
- 120 frames a second, roughly 10 to 15% of one core on Playful and 20 to 25% on Zoomies, 80 to 90 MB;
- scores, the library and lifetime progress were written to disk and read back by the next run; unlocks were announced;
- on the run that logged it, the camera never moved more than 0.2 units, 2 degrees of roll or 2.7 degrees of zoom, and was back at rest at the end.

**What the test found.** The first run finished the song and then never exited. The frame limiter slept on one waitable timer shared by the whole process; with a second thread sleeping too, each re-armed the timer under the other and one of them waited forever. The game alone has a single sleeping thread, so it had not bitten yet, but it would have the day anything else slept. Each thread now has its own timer and the wait is bounded. That first run also had five frames over 20 ms; the three runs after the fix had two, none and one.

**Rounder peaks.** Looking at the audio path for the test showed that the final stage clamped the mix. A loud song plus a few effects would have clipped flat. It now eases into the top with a `tanh` curve above 0.8; on the demo track at the highest difficulty that touched 0.03% of samples.

**The sounds, measured.** Each effect was written out and analysed: peak 0.85 with no clipped samples, no DC offset, attacks between 3 and 13 ms (the milestone arpeggio swells over 98 ms), and essentially nothing above 4 kHz. They are close to pure tones: 262 Hz for the hop and the bat, 523 Hz for the swat and the bell, 1046 Hz for the twinkle. Soft is confirmed; whether they are *satisfying* still needs ears.

**Unlock pace, with numbers.** Four short walks (three minutes of music) unlocked six of the 20 things to earn. The items tied to perfect hits go fastest: at the demo track's density a three-minute song on Playful has about 430 notes, so one good walk can clear the 50, 150 and 300 thresholds together. Those three numbers are probably too low; they are unchanged for now.

**Still needs a person:** how the timing feels on a real keyboard and through real speakers (injected keys skip the keyboard, and the delay after the audio callback cannot be measured from inside), whether the sounds please, and charts on real music.

## 2026-11-13: kittens, a wardrobe and a settings screen

**Cuter cats.** The cats were recognisable but not cute. What changed the read most was not detail but proportion and angle: a head nearly as big as the body, eyes much larger and set low and wide, a short round body, stubby legs with big paws, and the head turned to a three-quarter view so both eyes always face the player. Then the small things: two highlights in each eye, rosy cheeks, a mouth, whiskers, cheek fur. The first cheeks were boxes and stuck out like bricks in the close-up; they are flattened blobs set into the cheek now. The face is also lit flatter than the body so it never goes grey in shade.

**A wardrobe.** Hats, things for the neck and things for the back, 21 in all, plus four of the seven cats, are unlocked by playing. Each has one plain condition (songs finished, perfect hits, best streak, a rank, a place visited, time walked) and shows its progress. There is no shop and no randomness on purpose: a cozy game should not have an economy to manage. The wardrobe moves the camera in close, the cat hops when it changes clothes, and the results screen says what a walk unlocked or what is nearest.

**A settings screen.** Four tabs on cream paper: sound, game, display, progress. It took over the settings that had piled up on the song screen, which is now just difficulty, scenery, watch mode and Play. New settings: music volume, a frame rate choice (60 is offered as the lightest), and how much the camera and screen effects move, for anyone who finds the motion too much.

**Not checked by a person yet:** everything here in real time, and whether the unlock pace feels right. The numbers were chosen so the first few items come within a song or two; that is a guess.

## 2026-11-13: fifteen places instead of five

The five places each looked the same every time. Now each has three variations with their own landmarks: flower fields with a windmill, farmland, a riverside; a garden, a stream with red bridges, a lantern festival; deep woods, a pumpkin patch, a harvest village; a village, a frozen river, a pine forest under the aurora; a beach, a harbour with a lighthouse, a rocky cove.

- A stop on the journey is now a place and a variation. The variation counts up from a seed, so coming back to a place shows another side of it.
- Rivers are a terrain feature, not a prop: two rows behind the path are pressed flat and drawn as water or ice, which costs nothing extra to draw.
- Each place got a flat backdrop behind the hills (balloons, a mountain, birds, peaks and aurora, islands), small stones along the path everywhere, tufts of grass where grass grows, and smoke from every chimney.
- The snow path was almost the same colour as the snow around it, which made the path hard to see at night. It is darker and bluer now.

Cost: a frame went from about 1.1 ms to about 1.3 ms of CPU in the simulated run. Crops and tufts were first built from the faceted ball used for bushes, which would have added thousands of triangles per frame; they are three-sided cones instead.

## 2026-11-13: seven cats, a better cat, and a measured diet

**"It must be very lightweight, remember."** A fair reminder, and measuring instead of assuming paid off at once: with the menu open the game was using 123% of a CPU core. V-sync was requested but not being applied, so the loop was drawing thousands of frames a second. Nothing in the screenshot-based checks could have shown that.

What changed:

- The game now paces its own frames, sleeping on a high-resolution timer: 120 a second while playing, 60 in menus, 30 in the background, nothing drawn while minimized. Capping with raylib's limiter came first and still cost a quarter of a core, because it busy-waits part of each frame and the display here runs at 240 Hz.
- A window that starts minimized never reports being minimized to the framework, so the OS is asked directly.
- A song is no longer held twice during loading, and the analysis no longer builds a mono copy of the whole track.
- Sound synthesis at startup uses recurrences instead of a `sin` and an `exp` per sample.

After: about 6% of one core with the window open, of which half is the audio device thread and outside the game's control. The self-test prints the same charts and the same sound levels as before these changes.

**The cat.** The old one was two stretched balls on four boxes. The new one is built from lofts (rings skinned into a surface), which gives it a real torso, a round head with a short muzzle, big eyes with a highlight, proper ears, two-part legs with paws, a five-segment tail, and a collar with a bell. It took three rounds: the first version had a long snout and long legs and looked like a fox, and its face was in shadow because the scene's light came from behind it. Shorter muzzle, shorter legs, bigger head, and the light moved to where the sun is actually drawn.

**Seven cats.** Because every coat is a colour rule over the same lofts, variety was nearly free: tabby, tuxedo, calico, siamese, british blue, fluffy white and black, each with its own eyes and collar and slightly different proportions. They are picked from the main menu, where the cat strolling behind the title doubles as the preview.

## 2026-11-13: places, a camera that reacts, and a song shelf

Three requests in one go.

**A dynamic camera and more impact.** The camera now sits on four springs (sideways, vertical, roll, zoom) and each action kicks them differently, so a hop, a bat, a swat and a bell each feel distinct through the camera alone. Hits also throw 2D streaks out from where they landed, burst a ring towards the viewer and tint the edges of the frame in the action's colour. Everything is short and small on purpose: the largest motion is about a tenth of a world unit and under two degrees, and it is over in a quarter of a second, so the path stays readable.

**Five places.** Meadow, blossom garden, autumn woods, snowy village and seaside. Each is a ground colour, a sky tint, a prop mix per depth band and a weather layer; the seaside also reshapes the terrain into a beach and a sea. A song is a journey through them by default, starting somewhere chosen by its name, or it can stay in one. The menus walk through all five in the background.

Two things learned while building them:

- Reusing the meadow's day-to-night palette as a plain multiplier turned snow bright violet at night. Keeping the darkening but only part of the colour cast fixed it for every place at once.
- Palms were first placed where trees go in the other places and ended up crossing the play area. Props that lean need to sit further back than props that stand straight.

**A song shelf.** Every song opened is remembered with its length and best rank, and the main menu became a shelf to pick from, beside a title whose O has cat ears. The whole interface moved from dark translucent panels to cream paper cards with cocoa text, buttons that lift under the pointer, and a paw-print loading indicator.

The screenshot mode gained `--scenery` and fills the shelf with sample entries, so none of this needs a real library to check.

Still unverified by a person: all of the above in real time, with real audio.

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
