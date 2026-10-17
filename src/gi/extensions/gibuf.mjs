// three.c generator extension "gibuf" (states like standard+shadow+senv+l1101+xgibuf):
// the screen-space passes' G-buffer (src/gi/ssr.c). While the gi pass draws the G-buffer it
// swaps each material's extension to this one. The program writes what r186's SSR and SSGI
// examples take from their MRT attachments instead of the lit colour: the view space normal
// (after normal maps) octahedral-encoded in r and g, roughness in b, metalness in a; with
// giMode.x = 2 the diffuse colour times 1 - metalness (SSGI's albedo; alpha 1 marks a
// surface). Nothing in its output reads the lighting, so the driver drops it: the draw costs
// a surface pass, not a lit one. (The lit "gi" program has no G-buffer branch.)
export const name = 'gibuf';
export async function apply({ TSL, THREE, mat, props, sentinel, kind }) {
  if (kind === 'depth') return;
  const { Fn, If, uniform, vec2, vec4, abs, sign, select, float, normalView, roughness, metalness, diffuseColor } = TSL;
  const s = [sentinel(), sentinel(), sentinel(), sentinel()];
  const mode = uniform(new THREE.Vector4(...s)).setName('giMode');
  props['ext.giMode'] = { kind: 'vec4', value: s };
  mat.outputNode = Fn(() => {
    const o = vec4(0).toVar();
    If(mode.x.lessThan(1.5), () => {
      const n = normalView.normalize().toVar();
      const p = n.xy.div(abs(n.x).add(abs(n.y)).add(abs(n.z))).toVar();
      const q = select(n.z.lessThan(0), vec2(float(1).sub(abs(p.y)), float(1).sub(abs(p.x))).mul(vec2(sign(p.x), sign(p.y))), p);
      o.assign(vec4(q, roughness, metalness));
    }).Else(() => {
      o.assign(vec4(diffuseColor.rgb.mul(float(1).sub(metalness)), 1));
    });
    return o;
  })();
}
