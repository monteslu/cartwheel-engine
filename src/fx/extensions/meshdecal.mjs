// three.c generator extension "meshdecal" (states basic+map+trans+xmeshdecal and
// basic+map+trans+skin+xmeshdecal): mesh-mode Decal3D pieces (src/fx/decals.c).
// The decal image is the material's map, tinted by its colour; the x of the
// colour attribute (read here: the material's vertexColors stays off) is the
// fade the CPU baked per vertex (normal_fade, edge_fade), and the opacity is
// the decal colour's alpha. The output is premultiplied, (rgb * a, a), as
// the screen-space decal pass writes it: the material's CustomBlending factors
// make MIX (one, 1 - src alpha), MULTIPLY (dst colour, 1 - src alpha) or ADD
// (one, one) from it and keep the target's alpha (zero, one).
export const name = 'meshdecal';
export async function apply({ TSL, THREE, mesh, mat }) {
  const { vec4, float, attribute, materialColor, materialOpacity } = TSL;
  const g = mesh.geometry;
  if (!g.attributes.color) g.setAttribute('color', new THREE.BufferAttribute(new Float32Array(g.attributes.position.count * 3).fill(1), 3));
  const c = vec4(materialColor);
  const a = c.a.mul(materialOpacity).mul(attribute('color', 'vec3').x);
  mat.colorNode = vec4(c.rgb.mul(a), a);
  mat.opacityNode = float(1);
  mat.blending = THREE.CustomBlending;
  mat.depthWrite = false;
}
