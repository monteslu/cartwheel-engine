// Seeded random numbers for generator scripts (audio, textures, levels).
//
// JavaScript numbers are doubles: `seed * 1103515245` overflows 2^53 for most
// 32-bit seeds, silently drops low bits, and the "random" sequence collapses
// into a short cycle. One such LCG in make-audio.mjs fell into a 419-sample
// loop, so every noise sound was really a 114.6 Hz buzz, and it was copied
// into every game. Multiply 32-bit integers with Math.imul (exact), or use
// these. tests/js-int-math.mjs fails the suite on the overflowing pattern.
//
//   import { rng } from '../../tools/lib/rng.mjs';
//   const r = rng(1234);  r() -> [0, 1)   r.noise() -> [-1, 1)   r.u32() -> uint32

/** mulberry32: a full-period 32-bit generator, exact in JS. */
export function rng(seed = 1) {
  let s = seed >>> 0;
  const u32 = () => {
    s = (s + 0x6d2b79f5) >>> 0;
    let t = Math.imul(s ^ (s >>> 15), 1 | s);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return (t ^ (t >>> 14)) >>> 0;
  };
  const r = () => u32() / 4294967296;
  r.u32 = u32;
  r.noise = () => (u32() / 2147483648) - 1;
  return r;
}

/** The classic LCG (glibc constants), computed exactly: for scripts that must
 *  keep that sequence's character. Same API as rng(). */
export function lcg(seed = 1) {
  let s = seed >>> 0;
  const u32 = () => (s = (Math.imul(s, 1103515245) + 12345) >>> 0);
  const r = () => u32() / 4294967296;
  r.u32 = u32;
  r.noise = () => (u32() / 2147483648) - 1;
  return r;
}
