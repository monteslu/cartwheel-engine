# Procedural noise

Noise is a smooth random pattern: the raw material for clouds, rolling terrain, rock and wood textures, water ripples, fog density and anything else that should look natural without being painted by hand. {{name}} ships FastNoiseLite (MIT) as `cw_noise_*`, a texture generator that makes noise images as resources, and GPU versions of the same functions for your own shaders.

Header: [cartwheel/noise.h](header:noise.h). Sample: `noise_demo`, every pattern on the CPU and the GPU side by side with their difference. Gates: `unit-noise`, `noise-demo` and `noise-gpu`.

## Sampling noise

A [cw_noise](api:cw_noise) is a plain struct of settings. Start from [cw_noise_default](api:cw_noise_default), change what you need, and sample with [cw_noise_2d](api:cw_noise_2d) or [cw_noise_3d](api:cw_noise_3d). Values come out roughly between -1 and 1.

```c
cw_noise n = cw_noise_default();      // OpenSimplex2, frequency 0.01, seed 1337
n.fractal = CW_NOISE_FRACTAL_FBM;     // stack octaves
n.octaves = 5;
float height = cw_noise_2d(&n, x, z) * 40.0f;
```

`frequency` is features per input unit: 0.01 gives a hill or a cloud about every 100 units. The settings:

| Setting | What it does |
|---|---|
| `type` | the pattern: `OPENSIMPLEX2` (smooth, the usual choice), `OPENSIMPLEX2S` (smoother), `CELLULAR` (cells: stones, scales, cracks), `PERLIN`, `VALUE_CUBIC`, `VALUE` (blocky, cheapest) |
| `fractal` | `FBM` adds octaves (clouds, hills), `RIDGED` folds them into sharp crests (mountain ridges, veins), `PINGPONG` makes bands (terraces, wood rings) |
| `octaves`, `lacunarity`, `gain` | how many layers, how much finer each is (2) and how much weaker (0.5) |
| `weighted_strength` | 0..1: quieter detail where the layer below is low (FBM, ridged) |
| `cellular_distance`, `cellular_return`, `cellular_jitter` | how cell distance is measured, what is returned (the cell's value, the distance to the nearest point, or a mix of the nearest two: `DISTANCE2_SUB` draws the cracks between cells) and how irregular the cells are |
| `rotation` | 3D only: `XZ` makes horizontal slices look better, for terrain sampled at (x, height, z) |
| `warp`, `warp_amp` | domain warp ([cw_noise_warp_2d](api:cw_noise_warp_2d)): bends the input coordinates before sampling, for swirls, marble and eroded-looking terrain |

For a whole grid, [cw_noise_fill_2d](api:cw_noise_fill_2d) and [cw_noise_fill_3d_slice](api:cw_noise_fill_3d_slice) spread rows over the job pool and give exactly what single samples would. `noise_demo` fills its CPU panels one sample at a time:

@include samples/noise_demo/main.c#cpu

## The same numbers everywhere

The noise code uses only additions, multiplications, divisions, integer hashing and conversions (no math library at all, not even for its square root), so a given setting and position give the same bits natively and in a cart, on every host. You can use it for anything the game state depends on: terrain heights the physics collides with, spawn maps, a buoyancy force. The `unit-noise` gate pins a checksum of every pattern, fractal and mode and compares it across native, romdev, Chromium and wasmcart-native.

## Noise textures

`gen://noise?...` is a texture resource made on the fly. Load it like any image, by path; the path is the cache key, so the same path is generated once and shared:

```c
cw_res clouds = cw_res_load("gen://noise?size=512&fractal=fbm&octaves=6&freq=0.004&seamless=1", CW_RES_TEXTURE);
t3_texture *tex = cw_res_texture(clouds);
```

It is generated on a worker like an image decode, so `cw_res_load_async` works too. The texture is RGBA8, linear data (not sRGB), repeat-wrapped and mipmapped. The keys, all optional, are listed in [cartwheel/noise.h](header:noise.h); the useful ones:

| Key | Meaning |
|---|---|
| `size=256` or `size=512x256` | texels |
| `type`, `seed`, `freq`, `fractal`, `octaves`, `lacunarity`, `gain` | the settings above; `freq` is per texel |
| `seamless=1` | the texture tiles without a visible seam |
| `out=gray` | the noise in r, g and b |
| `out=rgba` | four independent noise layers, one per channel |
| `out=normal` | a normal map of the noise as a height field (`strength=` sets how bumpy), with the height in alpha |
| `warp=opensimplex2&warp_amp=30` | domain warp in texels |
| `z=` | a slice of 3D noise |
| `min=-1&max=1` | the range mapped to 0..255 |

Examples: water ripples `gen://noise?size=256&type=perlin&fractal=fbm&octaves=4&freq=0.03&out=normal&seamless=1`, cobblestones `gen://noise?type=cellular&cell_return=distance2_sub&freq=0.03`. [cw_noise_image](api:cw_noise_image) makes the same pixels into memory, for a tool or a CPU-side lookup.

## Noise in shaders

[cw_noise_glsl](api:cw_noise_glsl) and [cw_noise_wgsl](api:cw_noise_wgsl) return shader source (GLSL ES 3.00 and WGSL) that defines `cw_noise2` and `cw_noise3`, ported line for line from the C code. Paste it in front of a pass program's own source. A `CwNoise` struct carries the settings; [cw_noise_gpu_params](api:cw_noise_gpu_params) turns a `cw_noise` into four vec4 uniforms that `cw_noise_unpack` reads back, so the CPU and the GPU draw the same pattern from the same settings:

@include samples/noise_demo/main.c#gpu

GPUs round floating point a little differently from the CPU, so the GPU result matches the CPU to about one part in ten thousand, not bit for bit. In `noise_demo` fewer than 0.01% of texels differ by one 8-bit level, on GL and WebGPU alike; the `noise-gpu` gate holds every host to 0.5%. Use the CPU result for game state and the GPU result for pixels. Domain warp is CPU only.

## Costs

On the CPU, one octave of 2D noise costs about 25 ns (Perlin) to 110 ns (OpenSimplex2) a sample on one core of the 890M machine, 3D 90 to 160 ns, cellular 190 ns (2D) and 600 ns (3D); fractals multiply by the octave count. Filling a 256 x 256 OpenSimplex2 image over the job pool takes under 1 ms. A `gen://noise` texture costs that once, at load.

On the GPU (the 890M, GL, measured over 1440 x 1080 pixels with `NOISE_DEMO_BENCH`), per million pixels: one octave of OpenSimplex2 0.18 ms, 5-octave fBm 0.8 ms, 5-octave ridged Perlin 0.7 ms, cellular 0.45 ms, 4-octave ridged 3D OpenSimplex2 1.3 ms. A Vega 8 has about the same shader throughput, so expect the same to 1.5 times that. Sample noise once into a texture (or a `gen://noise` resource) when a pattern does not change every frame.
