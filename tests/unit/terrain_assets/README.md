Import fixtures for tests/unit/test_terrain.c (65 x 65 samples, made by a short
Node script with zlib: no third-party data):

- heights.png: 16-bit grey PNG, sample (x, y) = (x * 1009 + y * 523 + ((x * y) % 97) * 13) % 65536
- heights.r16: the same values, little-endian uint16, row-major
- splat.png: RGBA8 weights, R = 3x, G = 3y, B = 250 where |x - 32| < 4, A = 0
- control.png: Terrain3D control words, big end in R: (x & 7) << 27 | (y & 7) << 22 | 100 << 14
