# Video

{{name}} plays MPEG-1 video for cutscenes, intros and screens inside the game world. The module is `cartwheel/video.h` ([reference](header:video.h)): open a file, tell it which moment to show each tick, and draw its texture.

The decoder is [pl_mpeg](https://github.com/phoboslab/pl_mpeg) (MIT): MPEG-1 video and MP2 sound in an MPEG program stream (`.mpg`). MPEG-1 is old and not the most efficient codec, but its decoding is integer arithmetic, small and fast, so every host decodes exactly the same frames.

## Making a video file

Any video tool that writes MPEG-1 works. With ffmpeg:

```sh
ffmpeg -i intro.mp4 -c:v mpeg1video -q:v 4 -c:a mp2 -b:a 128k -f mpeg assets/intro.mpg
```

`-q:v` trades size for quality (2 is near lossless, 8 is small). Keep the frame rate the source has; 24, 25, 30 and 60 fps all work. `samples/video_cutscene/make-clip.mjs` draws and encodes the sample's clip this way.

## Playing it

[cw_video_open](api:cw_video_open) reads the file into memory and decodes its first frame. `cw_video_desc` sets looping, the sound's volume and bus, or `mute`:

@include samples/video_cutscene/main.c#open

Each tick, [cw_video_update](api:cw_video_update) takes the time since the video started, in seconds, and shows the frame for it: `floor(t * fps)`. Take that time from the tick count, never from a clock, so a replay, a slow machine and every host show the same frame on the same tick:

@include samples/video_cutscene/main.c#time

@include samples/video_cutscene/main.c#update

Decoding moves forward one frame at a time; a time earlier than the current frame (a seek back, or a loop) starts again from the beginning and decodes forward. A video that does not loop holds its last frame and reports [cw_video_ended](api:cw_video_ended) once the time passes its end.

## Drawing it

[cw_video_texture](api:cw_video_texture) returns a render target with the current frame in RGB, ready to draw as a sprite ([2D rendering](guide:rendering-2d)) on GL or WebGPU. The decoder produces Y, Cb and Cr planes; they are uploaded as three textures and converted in a fragment shader, only when a new frame was decoded and only when the texture is asked for. Call it where you draw, before the 2D pass:

@include samples/video_cutscene/main.c#draw

On GL the conversion changes GL state the way a 2D pass does; if three.c draws next, reset its bindings as after any 2D pass (`t3_renderer_reset_bindings`).

## Sound

When the file has sound and audio is running ([audio](guide:audio)), the first [cw_video_update](api:cw_video_update) starts it as a stream voice ([cw_video_voice](api:cw_video_voice)); its MP2 frames are decoded on the job pool like any stream, and `cw_sound_load` plays the sound of an `.mpg` on its own too. The sound runs on the mixer's clock and the picture on the tick clock: over a long video they can drift apart by the difference between the two clocks, a few milliseconds a minute.

## Tests

The `video-cutscene` gate plays the sample's 4-second clip to its end, holds past it, then jumps back to 1 s, on every host and on GL and WebGPU. It compares the frame shown and a hash of its decoded planes on every tick, the picture (the shader's output, GL against WebGPU too), and the hash of the sound mixed offline. [cw_video_frame_hash](api:cw_video_frame_hash) is the planes' hash.

## Cost

Decoding is on the game thread, inside [cw_video_update](api:cw_video_update): the sample's 320x180 clip takes about 0.22 ms per frame on a Ryzen AI 9 HX 370 core (the sample publishes `video.decode_ms`), so a 640x360 clip takes about 0.9 ms and a 1280x720 one about 3.5 ms: pick the size by the frame budget of the slowest machine you ship to. The conversion is one triangle over the target.
