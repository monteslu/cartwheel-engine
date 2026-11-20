// The "dissolve" surface (cartwheel/surface.h): the mesh burns away along a noise field, with a glowing
// edge where it is about to go. The holes are cut from the picture and from the shadow alike.
//   amount   0 = whole, 1 = gone
//   edge     rgb: the edge's glow (linear, above 1 blooms), a: its width in noise units
//   scale    noise cells a metre of the mesh's own space
export const name = 'dissolve';
export const params = { amount: 'float', edge: 'color', scale: 'float' };

export function surface({ TSL, p, base, caster }) {
  const { float, vec3, floor, fract, sin, dot, mix, smoothstep, positionGeometry } = TSL;
  // value noise of the mesh's own position: it stays put on the mesh as it moves
  const hash = (q) => fract(sin(dot(q, vec3(127.1, 311.7, 74.7))).mul(43758.5453));
  const noise = (q) => {
    const i = floor(q), f = fract(q), w = f.mul(f).mul(float(3).sub(f.mul(2)));
    const c = (x, y, z) => hash(i.add(vec3(x, y, z)));
    return mix(mix(mix(c(0, 0, 0), c(1, 0, 0), w.x), mix(c(0, 1, 0), c(1, 1, 0), w.x), w.y),
      mix(mix(c(0, 0, 1), c(1, 0, 1), w.x), mix(c(0, 1, 1), c(1, 1, 1), w.x), w.y), w.z);
  };
  const n = noise(positionGeometry.mul(p.scale)).mul(0.98).add(0.01).toVar(); // never exactly 0: amount 0 cuts nothing
  const mask = n.greaterThan(p.amount);
  if (caster) return { mask }; // the shadow has the same holes
  // the edge: brightest at the cut, gone one width further in; nothing at amount 0
  const glow = float(1).sub(smoothstep(p.amount, p.amount.add(p.edge.a), n)).mul(smoothstep(0, 0.02, p.amount));
  return { mask, emissive: base.emissive.add(p.edge.rgb.mul(glow)) };
}
