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

Stopping takes a fade time ([cw_voice_stop](api:cw_voice_stop)); [cw_voice_fade](api:cw_voice_fade) ramps to any level.

## Buses and effects

Bus 0 is the master. Every other bus outputs into a parent created before it and may send post-fader into up to two buses created before it, so the graph is always a DAG and is processed in reverse creation order. Up to 16 buses, each with up to 4 effects run in order:

| Effect | Notes |
|---|---|
| `LOWPASS`, `HIGHPASS` | biquad, cutoff and Q |
| `DELAY` | feedback echo; the delay time is fixed when the effect is added |
| `REVERB` | Freeverb (8 combs and 4 allpasses per channel) |
| `COMPRESSOR` | feed-forward peak compressor |
| `LIMITER` | instant attack, hard ceiling; a good last stage on the master |

Adding an effect allocates on the game thread, so build the chain at startup and change parameters later with [cw_bus_set_fx](api:cw_bus_set_fx) or bypass it. Bus volume changes ramp over one block, so they do not click. Solo keeps soloed buses, their ancestors and their descendants audible. The bus setup of `samples/audio_mix`:

@include samples/audio_mix/main.c#buses

and the sounds that feed it, each with a default bus:

@include samples/audio_mix/main.c#sounds

A common game layout is a music bus and an effects bus under the master, so the options menu can set each volume.

## 3D sound

There is one listener ([cw_audio_set_listener](api:cw_audio_set_listener): position, velocity, forward, up). A voice played with `spatial = true` takes a `cw_voice_3d`: position and velocity, an attenuation model (`INVERSE`, `LINEAR`, `EXPONENTIAL`, with reference and maximum distance and rolloff, distance clamped as OpenAL's clamped models do), an optional cone, a doppler factor, and an occlusion amount the game sets (0 clear, 1 blocked; a low-pass). Spatial sources are mixed to mono and panned with an equal-power law. Engine units are the world's units; set [cw_audio_set_speed_of_sound](api:cw_audio_set_speed_of_sound) if yours are not metres.

The engine does not trace occlusion for you: cast a ray with the [physics](guide:physics) module between voice and listener and pass the result to [cw_voice_set_occlusion](api:cw_voice_set_occlusion). Moving voices need [cw_voice_set_position](api:cw_voice_set_position) every tick; the listener usually follows the camera. In `audio_mix`, an engine loop flies past the listener with a cone, doppler and occlusion switched on halfway:

@include samples/audio_mix/main.c#voice3d

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

- No audio node types: a scene cannot declare a sound player, and the editor has no audio UI. Audio is set up from code.
- Effects are the six above; there is no per-voice effect chain (route a voice to a bus instead).
- One listener; no split-screen listeners.
- Microphone input and audio capture are not part of the API.
