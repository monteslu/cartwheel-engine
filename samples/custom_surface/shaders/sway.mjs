// The "sway" surface (cartwheel/surface.h): wind on the vertices. Each vertex leans with a gust that
// travels over the world, more the higher it sits on its mesh, so a field of reeds moves as one field
// (the shadow moves with it: the caster's program has the same positions).
//   time     seconds
//   wind     x: how far the top leans (metres), y: gusts a second, z: the lean along the world's z as a share of x
export const name = 'sway';
export const params = { time: 'float', wind: 'vec3' };

export function surface({ TSL, p, base, toLocal, worldPosition }) {
  const { vec3, float, sin, positionGeometry } = TSL;
  const h = positionGeometry.y.add(0.5).clamp(); // 0 at the foot, 1 at the top of a unit-high mesh
  const phase = worldPosition.x.mul(0.9).add(worldPosition.z.mul(0.6));
  const push = sin(p.time.mul(p.wind.y).mul(6.2832).add(phase)).mul(p.wind.x).mul(h.mul(h));
  return { position: base.position.add(toLocal(vec3(push, float(0), push.mul(p.wind.z)))) };
}
