// three.c generator extension "meshdecal" (states basic+map+trans+xmeshdecal and
// basic+map+trans+skin+xmeshdecal, and the lit ones standard+map+trans+<lights>+xmeshdecal
// and its composed forms xcluster_csm_meshdecal_gi): mesh-mode Decal3D pieces (src/fx/decals.c).
// The decal image is the material's map, tinted by its colour; the x of the
// colour attribute (read here: the material's vertexColors stays off) is the
// fade the CPU baked per vertex (normal_fade, edge_fade), and the opacity is
// the decal colour's alpha.
//
// Unlit (a basic material: MULTIPLY and ADD decals, and any decal on an unlit
// surface): the output is premultiplied, (rgb * a, a), as the screen-space
// decal pass writes it: the material's CustomBlending factors make MIX (one,
// 1 - src alpha), MULTIPLY (dst colour, 1 - src alpha) or ADD (one, one) from
// it and keep the target's alpha (zero, one).
//
// Lit (a standard material: a MIX decal on a lit surface): the colour is the
// surface colour of a sticker, so the material's own lighting shades it as it
// shades the surface under it (the scene's lights, and the cluster, csm and gi
// shading parts it composes with) and its fog applies; the output is straight
// alpha (lit rgb, a) blended (src alpha, 1 - src alpha).
export const name = 'meshdecal';
export async function apply({ TSL, THREE, mesh, mat, kind }) {
  const { vec4, float, attribute, materialColor, materialOpacity } = TSL;
  const g = mesh.geometry;
  if (!g.attributes.color) g.setAttribute('color', new THREE.BufferAttribute(new Float32Array(g.attributes.position.count * 3).fill(1), 3));
  const c = vec4(materialColor);
  const a = c.a.mul(materialOpacity).mul(attribute('color', 'vec3').x);
  mat.colorNode = kind === 'basic' ? vec4(c.rgb.mul(a), a) : vec4(c.rgb, a);
  mat.opacityNode = float(1);
  mat.blending = THREE.CustomBlending;
  mat.depthWrite = false;
}
