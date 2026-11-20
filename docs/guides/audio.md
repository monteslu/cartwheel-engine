# Audio

{{name}}'s audio module is a mixer with sounds, voices, a bus graph with effects, and 3D positional sound. The same calls produce the same samples on every host, bit for bit, when the mix is rendered offline.

The module is `cartwheel/audio.h` ([reference](header:audio.h)). It is a C API used from the game thread; there are no audio node types. A game calls [cw_audio_init](api:cw_audio_init) once, [cw_audio_update](api:cw_audio_update) once per rendered frame, and [cw_audio_shutdown](api:cw_audio_shutdown) at exit. Nothing in the engine calls these for you, scene apps included.

## The model

| Object | What it is | Lifetime |
|---|---|---|
| `cw_sound` | audio data: decoded PCM (a *sample*) or the encoded file decoded while it plays (a *stream*) | until [cw_sound_free](api:cw_sound_free) or shutdown; the memory goes once the mixer lets go of it |
| `cw_voice` | one playing instance of a sound | ends by itself (end of data, stop, steal); the handle then goes stale and every call on it is a no-op |
| `cw_bus` | a mixing bus with volume, mute, solo, effects and sends | fixed for the session (an index; bus 0 is the master) |

Sounds and voices are generational handles, so holding a voice after it ends is safe: [cw_voice_is_playing](api:cw_voice_is_playing) returns false and setters do nothing.

## Threads and latency

The mixer runs in the platform's audio fill callback. Natively that is SDL's audio thread. In a cart it runs as a job on the engine job pool, started when the app's update returns and run while the frame renders; `wc_render` waits for it at its end and hands the wasmcart audio ring as many frames as the elapsed time requires. Commands sent during the render wait for the next frame's mix. The game thread and the mixer talk only through lock-free single-producer, single-consumer queues:

- Every call (play, set volume, move a voice) enqueues a command. Commands apply at the start of the next mixed block, so latency is at most `CW_AUDIO_BLOCK` (256) frames, about 5 ms at 48 kHz.
- The mixer posts events back: a voice finished, a buffer is no longer in use. [cw_audio_update](api:cw_audio_update) applies them, refreshes the stats and starts stream decode jobs. That is why voice state is "as of the last update".
- The fill callback never locks, allocates, frees or waits. Everything the mixer will use is allocated in [cw_audio_init](api:cw_audio_init) (`max_voices` 64, `max_streams` 8, a 16384-frame ring per stream and a 4096-command queue by default). A full command queue drops the call and counts it in `dropped_commands`.

`samples/audio_play` is the real output path: a looping Vorbis stream, a FLAC blip panned left and right every half second, and a generated tone circling the listener as a 3D voice.

@include samples/audio_play/main.c#init

## Sounds: samples and streams

[cw_sound_load](api:cw_sound_load) reads an asset and detects the format from its contents: WAV, FLAC, MP3, Ogg Vorbis, Ogg Opus, and the tracker formats MOD, XM, S3M and IT. Mono or stereo at any rate; extra channels are dropped. The mode decides the cost:

- **Sample**: decoded at load, on the calling thread, and resampled once to the mix rate. Costs memory (stereo float) and load time, nothing per voice beyond mixing. Right for short effects and loops.
- **Stream**: the encoded bytes stay in memory and a decode job on the engine job pool keeps a ring per playing stream ahead of the mixer. Right for music and ambience. Tracker modules always stream.
- **Auto** (default): a sample when the decoded size is at most 2 MB, else a stream.

When a stream's ring drops below 1/8 full and its job has not finished, [cw_audio_update](api:cw_audio_update) waits for it (running it itself if no worker started it). A ring that still runs dry plays silence for that voice and counts an underrun; `underruns` in the stats should stay at 0.

Generated sound goes through [cw_sound_from_pcm](api:cw_sound_from_pcm). Sounds loaded through the [resource system](guide:resources) come from [cw_res_audio_sound](api:cw_res_audio_sound), which makes the sound once per resource version; `games/platformer3d` loads all its audio that way.

Per-sound defaults (`cw_sound_desc`) set the bus, volume, priority and a voice limit. Playing past `max_voices` steals that sound's oldest voice, which is what keeps a rapid-fire effect from piling up.

## Voices

[cw_audio_play](api:cw_audio_play) starts a voice from a `cw_play_desc`: volume, pitch (a rate multiplier), pan, loop with optional loop points, a fade-in, the bus, a priority, a start offset in the source, and `start_frame`. When the mixer is full, the new voice steals the oldest voice of the lowest priority at or below its own, or fails (null handle) if every voice outranks it.

Two clocks make audio timing exact:

- [cw_audio_mixed_frames](api:cw_audio_mixed_frames) is the mixer's output clock. Passing a future value as `start_frame` starts the voice on exactly that frame, which is how rhythm and music cues line up regardless of frame timing.
- [cw_voice_frames_played](api:cw_voice_frames_played) is a voice's source clock, exact to the sample and counting every loop pass. Divide by [cw_sound_source_rate](api:cw_sound_source_rate) for seconds. Use it, not the frame counter, to sync gameplay to music.

Stopping takes a fade time ([cw_voice_stop](api:cw_voice_stop)); [cw_voice_fade](api:cw_voice_fade) ramps to any level. [cw_voice_stop_at](api:cw_voice_stop_at) ends a voice on a given mixer frame, to the sample, or starts its fade in the block that holds that frame, and [cw_voice_started_at](api:cw_voice_started_at) says which mixer frame a voice really started on: together they make a seam with nothing missing and nothing doubled.

## A sound's own loop

A sound can carry its loop: set `loop`, `loop_start` and `loop_end` in its `cw_sound_desc` (source frames at the file's own rate; end 0 is the end of the file), or call [cw_sound_set_loop](api:cw_sound_set_loop) after loading. Every voice of it then loops: the first pass plays from the start (an intro), and each pass after it from `loop_start` to `loop_end`. A play that gives its own points uses those, and `cw_play_desc.once` plays it through once. A WAV file's sampler chunk (`smpl`, written by most audio editors) gives a sound its points with no code. Samples and streams loop alike: a stream's decoder seeks back to `loop_start`, a sample's points are moved to the mix rate it was resampled to.

## Randomized sounds

A [cw_randomizer](api:cw_randomizer_new) holds several sounds and picks one per play: weighted with no sound twice in a row (`CW_RANDOM_NO_REPEAT`, the default), weighted with repeats (`CW_RANDOM_ANY`), or in order (`CW_RANDOM_SEQUENCE`), with each play's pitch scaled by a value in `[1 / random_pitch, random_pitch]` and its volume moved by up to `random_volume_db` either way. Footsteps, impacts and barks stop sounding like a loop of one recording:

```c
cw_randomizer steps = cw_randomizer_new(&(cw_randomizer_desc){ .random_pitch = 1.1f, .random_volume_db = 2, .seed = 7 });
for (int i = 0; i < 4; i++) cw_randomizer_add(steps, step_sound[i], 1);
cw_randomizer_play(steps, &(cw_play_desc){ .bus = cw_shell_sfx_bus() });   /* one step: a new pick, pitch and volume */
```

The picks come from the randomizer's own `cw_rng`, seeded from `seed`, so the same calls pick the same sounds on every host and in a replay.

## Interactive music

A [cw_music](api:cw_music_new) is a set of clips (sounds with a beat grid: `bpm`, `bar_beats`, a length in `beats`, `loop`) and the transitions between them. [cw_music_play](api:cw_music_play) asks for a clip; the transition from the clip playing to it says where the switch happens (`CW_MUSIC_AT_NOW`, `_BEAT`, `_BAR` or `_END` of the clip), how (a cut, a fade in, a fade out, or a cross fade over `fade_beats`), whether the new clip starts from its start or at the same position, and an optional filler clip played between the two. A clip can advance by itself at its end, to the clip named in `next` or back to the clip a transition held (`hold_previous`). The most specific transition wins: from and to, then any clip to this one, then this one to any, then any to any; with none, the switch waits for the next bar and cross fades over a beat.

The switches are on the mixer's frame clock: a clip's beat 0 is the frame its voice started on, a beat is `60 * rate / bpm` frames from there, and a switch is the new clip's voice started on that frame with the old one ended on it. They are scheduled at least `CW_MUSIC_LEAD` frames (4096, 85 ms at 48 kHz) ahead, so they stay exact with the device's buffering; that is also the latency of a switch `AT_NOW`. [cw_music_current](api:cw_music_current), [cw_music_pending](api:cw_music_pending) and [cw_music_switch_frame](api:cw_music_switch_frame) say where it is.

## Stream resources: .cwaudio

A randomizer or an interactive music can be a text asset, so a scene's player names it as its `stream`. One statement a line, `key=value` pairs, `#` comments. From the `audio_streams` sample:

@include samples/audio_streams/assets/steps.cwaudio

@include samples/audio_streams/assets/match.cwaudio

A clip's `loop_start=` and `loop_end=` set its sound's own points (for an Ogg or MP3 file, which carries none). A statement or value the format does not know is a load error naming the line. In C, [cw_audio_stream_load](api:cw_audio_stream_load) loads one (cached by path) and [cw_audio_stream_music](api:cw_audio_stream_music) makes a music from it.

## Buses and effects

Bus 0 is the master. Every other bus outputs into a parent created before it and may send post-fader into up to two buses created before it, so the graph is always a DAG and is processed in reverse creation order. Up to 16 buses, each with up to 4 effects run in order:

| Effect | Notes |
|---|---|
| `LOWPASS`, `HIGHPASS` | biquad, cutoff and Q |
| `DELAY` | feedback echo; the delay time is fixed when the effect is added |
| `REVERB` | Freeverb (8 combs and 4 allpasses per channel) |
| `FDN_REVERB` | feedback delay network (8 lines, Hadamard feedback): set by decay time (RT60), high-frequency damping, pre-delay and room size; the reverb zones below use it |
| `COMPRESSOR` | feed-forward peak compressor |
| `LIMITER` | instant attack, hard ceiling; a good last stage on the master |

Adding an effect allocates on the game thread, so build the chain at startup and change parameters later with [cw_bus_set_fx](api:cw_bus_set_fx) or bypass it. Bus volume changes ramp over one block, so they do not click. Solo keeps soloed buses, their ancestors and their descendants audible. The bus setup of `samples/audio_mix`:

@include samples/audio_mix/main.c#buses

and the sounds that feed it, each with a default bus:

@include samples/audio_mix/main.c#sounds

A common game layout is a music bus and an effects bus under the master, so the options menu can set each volume; both games in `games/` do that.

## 3D sound

There is one listener ([cw_audio_set_listener](api:cw_audio_set_listener): position, velocity, forward, up). A voice played with `spatial = true` takes a `cw_voice_3d`: position and velocity, an attenuation model (`INVERSE`, `LINEAR`, `EXPONENTIAL`, with reference and maximum distance and rolloff, distance clamped as OpenAL's clamped models do), an optional cone, a doppler factor, and an occlusion amount the game sets (0 clear, 1 blocked; a low-pass). Spatial sources are mixed to mono and panned with an equal-power law. Engine units are the world's units; set [cw_audio_set_speed_of_sound](api:cw_audio_set_speed_of_sound) if yours are not metres.

The engine does not trace occlusion for you: cast a ray with the [physics](guide:physics) module between voice and listener and pass the result to [cw_voice_set_occlusion](api:cw_voice_set_occlusion). Moving voices need [cw_voice_set_position](api:cw_voice_set_position) every tick; the listener usually follows the camera. In `audio_mix`, an engine loop flies past the listener with a cone, doppler and occlusion switched on halfway:

@include samples/audio_mix/main.c#voice3d

`games/platformer3d` attaches looping 3D voices to level objects and moves the listener with the camera.

A far voice can also lose its highs with distance: `air_absorption` in `cw_voice_3d` halves the low-pass cutoff (from 20 kHz) every `1 / air_absorption` units beyond the reference distance (0.02 halves it every 50 units). It stacks with occlusion.

## Headphones: sound from every direction

Panning places a sound only between left and right. On headphones the mixer can do better: with [cw_audio_set_output](api:cw_audio_set_output)`(CW_AUDIO_HEADPHONES)` spatial voices are filtered with a head-related transfer function (HRTF), the way a head and ears colour sound arriving from each direction, so a source is heard in front, behind, above or below. Speakers (`CW_AUDIO_SPEAKERS`, the default) keep the equal-power pan: the filtering is meant for headphones and sounds wrong through speakers, so offer the choice in the game's audio settings.

For each filtered voice and each 256-frame block:

- the direction to the voice, in the listener's frame, picks a triangle of three measured directions and their blend weights;
- each ear gets the voice delayed by its interaural delay (up to about 0.7 ms: the far ear hears it later) and a 64-tap filter blended from the three measurements;
- when the voice or the listener moved, the old and new filters both run and crossfade across the block, so a source sweeping around the head stays smooth.

The built-in table is the MIT Media Lab KEMAR dummy-head set (Bill Gardner and Keith Martin, 1994), diffuse-field equalized, 710 measured directions from 40 degrees below the horizon to straight up, plus one made-up direction straight down (the average of the lowest ring). Switching to headphones loads it (about 360 KB). [cw_audio_load_hrtf](api:cw_audio_load_hrtf) loads another `.cwhrtf` table from the bundle; `tools/hrtf/import-kemar.mjs` shows how one is made (resampling, minimum phase with the delay split out, 64 taps, a triangulation of the measurement grid).

Filtering costs more than panning, so only the nearest voices get it: [cw_audio_set_hrtf_voice_limit](api:cw_audio_set_hrtf_voice_limit) (16 by default). The nearest voices to their listener are filtered and the rest pan; a voice that enters or leaves the set crossfades between the two over one block. A voice whose direction does not matter (a sound attached to the player, say) can opt out with `mode = CW_SPATIAL_PAN`. To scale the limit with the quality preset:

```c
cw_audio_set_hrtf_voice_limit(cw_quality_count(16, "hrtf_voices")); /* Low 4, Medium 8, High and Ultra 16 */
```

`samples/audio_3d` orbits a bell around the listener's head horizontally (front, right, behind, left) and a hum vertically (in front, overhead, behind, below), places a shaker behind and above, and scatters 24 crickets with an HRTF voice limit of 8:

@include samples/audio_3d/main.c#setup

The stats ([cw_audio_get_stats](api:cw_audio_get_stats)) report `hrtf_voices`, how many voices were filtered in the last block. Listen to the result natively (`CW_FRAMES=722 CW_AUDIO_3D_WAV=out/a3d.wav build/native/samples/audio_3d`) with headphones.

**Cost**, measured by `audio_3d` (32 voices on one core of a Ryzen AI 9 HX 370; the sample logs its own numbers): per voice per 256-frame block, about 2 microseconds panned, 5 with the HRTF, and 8 when the voice or listener moves every block (both filters run for the crossfade). The wasm cart in Chromium measures about the same (2.2 and 5.5). A block is 5.3 ms of sound, so 16 moving HRTF voices cost about 130 microseconds per block, under 3% of one core; a four-core laptop chip of a few years ago (a Ryzen 3 with Vega 8 graphics, say) runs it at roughly half that speed, still a few percent. Every operation is plain float arithmetic in a fixed order (the filter runs four output samples at once with SSE2, wasm SIMD128 or NEON, and the plain C path gives the same bits), so the `audio-3d` gate requires every host to reproduce the native mix exactly.

## Places: reverb zones

A cave rings short and dark, a hall long and bright, a field hardly at all. A reverb zone is a place with its own reverb: an axis-aligned box or a sphere, a blend distance over which it fades out beyond its edge, a priority, and the reverb's decay time, high-frequency damping, pre-delay and room size ([cw_audio_add_reverb_zone](api:cw_audio_add_reverb_zone), up to 8). Every spatial voice feeds the zones through its `reverb_send` (`cw_voice_3d`; 1 by default, after distance attenuation and occlusion, negative for none), and each zone takes the share listener 0's position gives it:

- inside the shape the share is 1, falling to 0 across the blend distance outside it;
- where zones overlap, the highest priority takes its share first and leaves the rest to the others, so a cave inside a big valley zone sounds only like the cave once you are in it, and like a mix of both at its mouth;
- shares ramp across each block, and a zone keeps ringing after the listener leaves it: the input fades, not the tail.

The reverb is an FDN (feedback delay network, Jot and Chaigne): 8 delay lines with mutually prime lengths fed back through a Hadamard matrix, a gain per line that sets the decay time and a low-pass per line for damping. It is also an ordinary bus effect, `CW_FX_FDN_REVERB`. A zone whose input has been silent long enough for its tail to die costs nothing; one that is audible or still ringing costs about 11 microseconds per 256-frame block on one Ryzen AI 9 HX 370 core (0.2% of it; `audio_3d` measures it). Freeverb (`CW_FX_REVERB`) is unchanged.

In `samples/audio_3d` the listener walks out of a cave, along a passage with no reverb, into a hall, with footsteps at their feet, water dripping behind them and an organ at the far end:

@include samples/audio_3d/main.c#walk

In a scene, a `ReverbZone3D` node does the same ([node reference](node:ReverbZone3D)): its world position is the zone's centre (rotation and scale are not applied), its properties are the zone's, and the zone exists while the node is in a tree and audio is running.

## Players in a scene

Sounds can be nodes ([cartwheel/audio_nodes.h](header:audio_nodes.h)), so a scene file says what plays where and a game writes no voice bookkeeping:

| Node | What it does |
|---|---|
| [AudioStreamPlayer](node:AudioStreamPlayer) | plays a sound as it is: music, UI, anything without a place |
| [AudioStreamPlayer2D](node:AudioStreamPlayer2D) | pans and fades it by where the node is on the 2D view |
| [AudioStreamPlayer3D](node:AudioStreamPlayer3D) | a spatial voice at the node: distance, cone, doppler, reverb zones |
| [AudioListener3D](node:AudioListener3D) | the ears; with none in the tree the current camera listens |

From the `gameplay_nodes` sample, a looping sound on a beacon, heard from the camera that rides the cart:

@include samples/gameplay_nodes/assets/main.cwscene#Beacon/Horn

Every player has `stream` (an asset path, loaded on first play and shared, or a `.cwaudio` stream resource), `volume_db`, `pitch_scale`, `autoplay`, `loop`, `bus` (a bus by name), `max_polyphony` (how many copies may sound at once: one more stops the oldest; the default 1 makes `play` restart the sound), `stream_paused` and `playing`, the methods `play` and `stop`, and the signal `finished`. In C: `cw_audio_player_play`, `cw_audio_player_stop`, `cw_audio_player_is_playing`, `cw_audio_player_position`, and `cw_audio_player_voice` for anything a property does not cover. `cw_audio_player_set_sound` gives a player a sound made in code (`cw_sound_from_pcm`) instead of a file.

The 3D player's properties are the fields of `cw_voice_3d`: `attenuation_model`, `unit_size` (the distance of full volume), `max_distance`, `rolloff`, the cone (`cone_inner_angle`, `cone_outer_angle`, `cone_outer_gain`, along the node's -Z), `doppler`, `air_absorption`, `reverb_send` and `occlusion`. Every fixed tick it gives its voices the node's world position, its velocity (its movement since the last tick) and its facing. The 2D player has `max_distance` (pixels from the middle of the view to silence), `attenuation` and `panning_strength`.

**The listener.** A tree with a 3D player or an `AudioListener3D` sets the mixer's listeners each tick: from its current listener nodes (several for split screen, up to four, each voice heard by the nearest), else from the current [Camera3D](node:Camera3D). `cw_audio_nodes_set_auto_listener(tree, false)` hands the listener back to the game.

**Timing.** Players are stepped on the fixed tick, after physics. `finished` is counted in ticks from the sound's length and the pitch, not read back from the mixer, so it arrives on the same tick on every host and in a replay, with or without an audio device (a tracker module, whose length is unknown, finishes when its voice ends). A paused tree holds the voices of the players it pauses, by their pause mode: music on a player set to run always keeps playing under a pause menu.

**Stream resources.** A player of a randomizer resource picks a sound, pitch and volume on every play (each copy up to `max_polyphony`). A player of an interactive resource has its own music: `play` starts it on its `clip` (empty: the resource's initial clip), setting `clip` while it plays switches by the resource's transitions, `current_clip` reads the clip sounding now, and [cw_audio_player_music](api:cw_audio_player_music) gives the music to C. A 3D player places every voice of its music at the node. A sound that loops by itself never counts as finished; an interactive player emits `finished` when its music stops by itself.

**The mixer.** A game that calls `cw_audio_init`, or uses the game shell, owns the mixer as before. If nothing started it by the time a player first plays, the player starts it with the defaults and the tree runs `cw_audio_update` each frame.

Checked by the `unit_audio_nodes` suite (the offline mixer: side, distance, cone, decibels, polyphony, pause) and the `gameplay-nodes` gate, which holds every host to the same samples. The `audio-streams` gates (the `audio_streams` sample, offline) hold the loop points (a sampler chunk's loop as a sample and as a stream, every frame past the loop end repeating the one a loop length before, to the bit), the randomizer (no repeats, pitch and volume in range, the same picks everywhere) and the interactive music (calm to battle on calm's next bar through a one-beat filler, battle starting exactly a beat after it, back to calm on battle's next beat), each with a control that must fail.

## Sample-exact across hosts

The mixer uses only arithmetic, `sqrt` and the deterministic functions in `cartwheel/math.h`, never libm. In offline mode (`offline = true`, no device) the game pulls samples with [cw_audio_render](api:cw_audio_render), which applies every queued command first and waits for stream decoding before each block, so the output depends only on the calls made, never on timing. `audio_mix` renders one 1/60 s slice per frame:

@include samples/audio_mix/main.c#render

After four seconds it publishes a hash of all 192000 stereo frames, per-second hashes, each decoder's own hash, and the music clock checked against exact 32.32 fixed-point arithmetic. The `audio-mix` gate requires every host to reproduce the native hash. The picture is the waveform and the hash bits, so even a host that only compares frames sees a mismatch:

```sh
node tests/gate.mjs audio-mix audio-play unit-audio
CW_FRAMES=240 CW_AUDIO_MIX_WAV=out/mix.wav build/native/samples/audio_mix   # write the mix as a WAV
```

Real-time output is the same mix, but how many frames get mixed by a given game frame depends on the host's audio clock, and natively the device chooses the rate (carts mix at the rate the wasmcart audio ring declares, 48 kHz). So the `audio-play` gate checks only what must hold everywhere: a device is open, the mixer produces non-silent frames, the stream plays and its clock moves.

## Analysis and stats

[cw_audio_get_stats](api:cw_audio_get_stats) reports mixed frames, blocks, underruns, dropped commands, voice and stream counts, the master peak of the last block, and whether a device is open; init registers the `audio.*` debug fields, and the [profiler](guide:profiling) reads the same numbers. For tests, [cw_audio_analyze](api:cw_audio_analyze) gives a hash, peak and RMS of a buffer, and [cw_audio_decode_asset](api:cw_audio_decode_asset) decodes a whole file without the mixer.

## Not available yet

- Ogg, MP3 and FLAC files carry no loop points the loader reads (a WAV's sampler chunk only): give them with `cw_sound_set_loop` or a `.cwaudio` clip's `loop_start` / `loop_end`.
- Interactive music switches between clips; it does not mix layers of one clip in and out: play the layers as voices started on the same frame.
- Effects are the six above; there is no per-voice effect chain (route a voice to a bus instead).
- Microphone input and audio capture are not part of the API.
