// A game's surface module as a shading part (cartwheel/surface.h, cartwheel/shading.h).
//
// A surface module, shaders/<name>.mjs, exports
//   name     the surface's name ([a-z0-9], at most 10 characters; the file is <name>.mjs)
//   params   { amount: 'float', tint: 'color', spots: { type: 'vec4', count: 8 },
//              albedo: { texture: true, wrap: 'repeat', filter: 'linear', mipmaps: true, srgb: true } }
//            types: float, vec2, vec3, vec4, color (linear rgba, a vec4), mat4, an array of vec4, a texture
//   surface(s)   returns the surface's inputs as r187 TSL nodes, any of
//            color      vec3 or vec4 (alpha is the opacity)      -> material.colorNode
//            normal     the shading normal, view space           -> material.normalNode
//            roughness, metalness   float                        -> roughnessNode, metalnessNode
//            emissive   vec3                                     -> emissiveNode
//            ao         float                                    -> aoNode
//            opacity    float                                    -> opacityNode
//            mask       bool: false cuts the pixel out, of the picture and of the shadow -> maskNode
//            position   vec3, the vertex in the mesh's own space  -> positionNode
//            output     vec4, the last word on the lit, fogged colour (read TSL.output) -> outputNode
//   and gets s = {
//     TSL, THREE   three.js r187's TSL and core
//     p            the parameters as nodes: p.amount, texture(p.albedo, uv); an array is a JavaScript array of
//                  nodes, p.spots[i] or p.spots.element(i) with a number (write the loop in JavaScript)
//     base         what the material (or an earlier part) gives: color, normal, roughness, metalness, emissive,
//                  opacity; position is the vertex before this surface moved it
//     kind         the program's material kind (standard, physical, lambert, phong), or 'depth'
//     caster       true for the shadow caster's program: only position and mask count there
//     instanced    the program draws instances (an InstancedMesh, or meshes the renderer batched)
//     toLocal(v)   a displacement in world metres as one in the mesh's own space (add it to base.position)
//     worldPosition   the vertex in the world before this surface moved it
//   }
// tools/programs.mjs wraps every module that exports `surface` with surfaceApply, so the module needs no import
// from the engine. Parameters reach the program as uniforms and textures named ext.<name>_<param>, which
// src/render/surface.c fills for each draw from the values C set (cw_surface_set...).
const LIT = new Set(['standard', 'physical', 'lambert', 'phong', 'toon']);
const OUTPUTS = new Set(['color', 'normal', 'roughness', 'metalness', 'emissive', 'ao', 'opacity', 'mask', 'position', 'output']);
const WIDTH = { float: 1, vec2: 2, vec3: 3, vec4: 4, color: 4, mat4: 16 };

/** the declared parameter as { type, count } or { texture } (throws on anything else) */
function describe(surface, key, decl) {
  if (!/^[a-zA-Z][a-zA-Z0-9]*$/.test(key)) throw new Error(`surface ${surface}: parameter name '${key}' must be letters and digits`);
  if (typeof decl === 'string') decl = { type: decl };
  if (decl && decl.texture) return { texture: decl };
  if (!decl || !(decl.type in WIDTH)) throw new Error(`surface ${surface}: parameter ${key} has no type (float, vec2, vec3, vec4, color, mat4, { type: 'vec4', count }, { texture: true })`);
  const count = decl.count ?? 1;
  if (count !== 1 && (decl.type !== 'vec4' || !(count >= 2 && count <= 32))) throw new Error(`surface ${surface}: parameter ${key}: only a vec4 may be an array, of 2 to 32`);
  return { type: decl.type, count };
}

/** the parameter's node, made on first use (a program holds only what its nodes read) */
function makeParam(ctx, surface, key, d) {
  const { TSL, THREE, props, sentinel } = ctx;
  const id = `${surface}_${key}`;
  if (d.texture) {
    const o = d.texture;
    const t = new THREE.DataTexture(new Uint8Array(16).fill(255), 2, 2, THREE.RGBAFormat, THREE.UnsignedByteType);
    t.name = 'ext.' + id;
    const nearest = o.filter === 'nearest', mips = o.mipmaps !== false && !nearest;
    t.magFilter = nearest ? THREE.NearestFilter : THREE.LinearFilter;
    t.minFilter = nearest ? THREE.NearestFilter : mips ? THREE.LinearMipmapLinearFilter : THREE.LinearFilter;
    t.generateMipmaps = mips;
    t.wrapS = t.wrapT = o.wrap === 'clamp' ? THREE.ClampToEdgeWrapping : o.wrap === 'mirror' ? THREE.MirroredRepeatWrapping : THREE.RepeatWrapping;
    if (o.srgb) t.colorSpace = THREE.SRGBColorSpace;
    t.needsUpdate = true;
    return t;
  }
  // (optional: a program whose nodes do not read the parameter, a caster's say, has no such uniform)
  if (d.count > 1) {
    // an array: a vec4 uniform per element, in the draw's own uniform block with the other parameters (r187's
    // uniformArray is a buffer of its own, one more binding a draw)
    const el = Array.from({ length: d.count }, (_, i) => {
      const s = Array.from({ length: 4 }, () => sentinel());
      props[`ext.${id}_${i}`] = { kind: 'vec4', value: s, optional: true };
      return TSL.uniform(new THREE.Vector4(...s)).setName(`${id}_${i}`);
    });
    el.element = (i) => {
      if (!Number.isInteger(i) || i < 0 || i >= d.count) throw new Error(`surface ${surface}: ${key}.element(${i}): the index is a whole number from 0 to ${d.count - 1} (loop in JavaScript)`);
      return el[i];
    };
    return el;
  }
  const s = Array.from({ length: WIDTH[d.type] }, () => sentinel());
  props['ext.' + id] = { kind: d.type === 'float' ? 'f32' : d.type === 'color' ? 'vec4' : d.type, value: s, optional: true };
  const v = d.type === 'float' ? s[0] : d.type === 'vec2' ? new THREE.Vector2(...s) : d.type === 'vec3' ? new THREE.Vector3(...s)
    : d.type === 'mat4' ? new THREE.Matrix4().fromArray(s) : new THREE.Vector4(...s);
  return TSL.uniform(v).setName(id);
}

/**
 * The shading part's apply() for a surface module.
 * @param {{ name: string, params?: object, surface: Function }} mod
 */
export function surfaceApply(mod) {
  const surface = mod.name;
  if (!/^[a-z0-9]{1,10}$/.test(surface || '')) throw new Error(`surface '${surface}': the name must be [a-z0-9], at most 10 characters`);
  const decl = Object.fromEntries(Object.entries(mod.params || {}).map(([k, v]) => [k, describe(surface, k, v)]));
  return async function apply(ctx, next) {
    const { TSL, THREE, mat, props, sentinel, kind, features = [] } = ctx;
    const caster = kind === 'depth';
    if (!caster && !LIT.has(kind)) return next ? next(ctx) : undefined;
    const made = {};
    const p = new Proxy({}, {
      get(_, key) {
        if (typeof key !== 'string') return undefined;
        if (!(key in decl)) throw new Error(`surface ${surface}: no parameter '${key}' in params`);
        return (made[key] ??= makeParam(ctx, surface, key, decl[key]));
      },
    });
    const instanced = features.includes('inst') || features.includes('instbig');
    let inv = null;
    const toLocal = (v) => {
      if (instanced) return v; // instances are placed after the mesh's own space: the offset is already in metres
      if (!inv) {
        const s = Array.from({ length: 16 }, () => sentinel());
        props[`ext.${surface}_objinv`] = { kind: 'mat4', value: s, optional: true };
        inv = TSL.uniform(new THREE.Matrix4().fromArray(s)).setName(`${surface}_objinv`);
      }
      return inv.mul(TSL.vec4(v, 0)).xyz;
    };
    const base = {
      color: mat.colorNode ?? TSL.materialColor,
      normal: mat.normalNode ?? TSL.materialNormal,
      roughness: mat.roughnessNode ?? TSL.materialRoughness,
      metalness: mat.metalnessNode ?? TSL.materialMetalness,
      emissive: mat.emissiveNode ?? TSL.materialEmissive,
      opacity: mat.opacityNode ?? TSL.materialOpacity,
      position: mat.positionNode ?? TSL.positionLocal,
    };
    const worldPosition = TSL.modelWorldMatrix.mul(TSL.vec4(base.position, 1)).xyz;
    const out = (await mod.surface({ TSL, THREE, p, base, kind, caster, instanced, toLocal, worldPosition, features })) || {};
    for (const k of Object.keys(out)) if (!OUTPUTS.has(k)) throw new Error(`surface ${surface}: surface() returned '${k}' (it may return ${[...OUTPUTS].join(', ')})`);
    if (out.position) mat.positionNode = out.position;
    if (out.mask) mat.maskNode = mat.maskNode ? mat.maskNode.and(out.mask) : out.mask;
    if (!caster) {
      if (out.color) mat.colorNode = out.color;
      if (out.normal) mat.normalNode = out.normal;
      if (out.roughness && 'roughness' in mat) mat.roughnessNode = out.roughness;
      if (out.metalness && 'metalness' in mat) mat.metalnessNode = out.metalness;
      if (out.emissive) mat.emissiveNode = out.emissive;
      if (out.ao) mat.aoNode = out.ao;
      if (out.opacity) mat.opacityNode = out.opacity;
      if (out.output) mat.outputNode = out.output;
    }
    return next ? next(ctx) : undefined;
  };
}
