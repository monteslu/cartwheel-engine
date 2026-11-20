// The "rock" surface (cartwheel/surface.h): a stone texture laid on from three sides (triplanar), so a
// mesh needs no uv layout and the stone does not stretch on any face, with patches of moss.
//   amount   0 = the material's own colour, 1 = the stone
//   repeat   texture repeats a metre
//   sharp    how hard the three projections cut over (4 is a soft blend)
//   albedo   the stone (sRGB, tiling)
//   moss     up to 4 patches: xyz the centre in the world, w the radius (0 = none)
//   mossTint the moss colour (linear)
export const name = 'rock';
export const params = {
  amount: 'float', repeat: 'float', sharp: 'float',
  albedo: { texture: true, wrap: 'repeat', filter: 'linear', mipmaps: true, srgb: true },
  moss: { type: 'vec4', count: 4 },
  mossTint: 'color',
};

export function surface({ TSL, p, base, caster }) {
  if (caster) return {}; // nothing of this surface changes the shadow
  const { texture, vec4, float, mix, abs, pow, max, smoothstep, length, positionWorld, normalWorld } = TSL;
  const q = positionWorld.mul(p.repeat);
  const w0 = pow(abs(normalWorld), p.sharp);
  const w = w0.div(w0.x.add(w0.y).add(w0.z));
  const stone = texture(p.albedo, q.zy).rgb.mul(w.x).add(texture(p.albedo, q.xz).rgb.mul(w.y)).add(texture(p.albedo, q.xy).rgb.mul(w.z));
  let cover = float(0);
  for (let i = 0; i < 4; i++) {
    const m = p.moss[i], r = max(m.w, 0.0001);   // (a radius of 0 is no patch; never smoothstep between equal edges)
    cover = max(cover, float(1).sub(smoothstep(r.mul(0.6), r, length(positionWorld.sub(m.xyz)))));
  }
  const rock = mix(base.color.rgb, base.color.rgb.mul(stone).mul(2.2), p.amount);
  const color = mix(rock, p.mossTint.rgb, cover.mul(p.amount).mul(0.85));
  return { color: vec4(color, 1), roughness: mix(base.roughness, float(1), cover.mul(p.amount)) };
}
