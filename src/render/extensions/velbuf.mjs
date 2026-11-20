// three.c generator extension "velbuf" (states basic+ds+xvelbuf, basic+skin+ds+xvelbuf,
// basic+instbig+ds+xvelbuf and their morph / instance colour forms, in the engine's own table):
// the motion of a mesh that moved since the last frame, drawn over the camera's motion in the
// velocity target (src/render/velocity.c). While the velocity pass draws, every mesh that moved
// carries one plain material with this extension; the program writes
//
//   (clip_now.xy / clip_now.w - clip_before.xy / clip_before.w) * 0.5
//
// in uv units, the velocity target's convention (cartwheel/pipeline.h CW_RES_VELOCITY), where
// clip_now is the vertex through this frame's unjittered view-projection and clip_before the
// same vertex, posed as it was a frame ago, through the last frame's. It is r187's VelocityNode
// (previous model matrix, previous camera) with the previous pose taken from data the engine
// keeps: r187 reads the previous bone matrices and instance matrices only under an MRT with a
// velocity output, which three.c does not capture.
//
// Values (filled per draw by src/render/velocity.c):
//   ext.velViewProj      mat4, this frame's view-projection without the TAA jitter (frame group)
//   ext.velPrevViewProj  mat4, the last frame's (frame group)
//   ext.velModel         mat4, object to world now (a skinned mesh: world x bindMatrixInverse)
//   ext.velPrevModel     mat4, the same a frame ago
//   ext.velBind          mat4, a skinned mesh's bindMatrix
//   ext.velBones         RGBA32F, a skinned mesh's bone matrices: bone b's column c at texel
//                        (b * 4 + c, row), row 0 this frame's, row 1 the last frame's
//   ext.velInst          RGBA32F, an instanced mesh's instance matrices a frame ago: instance i's
//                        column c at texel index i * 4 + c, rows of ext.velInfo.x texels
//   ext.velInfo          vec4: x the width of ext.velInst in texels
// A morphed mesh moves here by its node and bones only: both positions take the same shape.
export const name = 'velbuf';

export async function apply({ TSL, THREE, mat, props, sentinel, kind, features }) {
  if (kind === 'depth') return;
  const { uniform, sharedUniformGroup, textureLoad, attribute, positionLocal, positionGeometry, instanceIndex, varying,
    vec4, mat4, ivec2, int } = TSL;
  const frame = sharedUniformGroup('velFrame', 0, THREE.NodeUpdateType.RENDER);
  const m4 = (n, group) => {
    const s = Array.from({ length: 16 }, () => sentinel());
    const u = uniform(new THREE.Matrix4().fromArray(s)).setName(n);
    if (group) u.setGroup(group);
    props['ext.' + n] = { kind: 'mat4', value: s };
    return u;
  };
  const data = (n, w, h) => {
    const t = new THREE.DataTexture(new Float32Array(w * h * 4), w, h, THREE.RGBAFormat, THREE.FloatType);
    t.name = 'ext.' + n;
    t.magFilter = t.minFilter = THREE.NearestFilter;
    t.generateMipmaps = false;
    t.wrapS = t.wrapT = THREE.ClampToEdgeWrapping;
    t.needsUpdate = true;
    return t;
  };
  const viewProj = m4('velViewProj', frame), prevViewProj = m4('velPrevViewProj', frame);
  const model = m4('velModel'), prevModel = m4('velPrevModel');

  let now, before;
  if (features.includes('skin')) {
    const bind = m4('velBind');
    const bones = data('velBones', 8, 2);
    const index = attribute('skinIndex', 'uvec4'), weight = attribute('skinWeight', 'vec4');
    const bone = (i, row) => {
      const x = int(i).mul(4);
      return mat4(textureLoad(bones, ivec2(x, row)), textureLoad(bones, ivec2(x.add(1), row)),
        textureLoad(bones, ivec2(x.add(2), row)), textureLoad(bones, ivec2(x.add(3), row)));
    };
    // r187's getSkinnedPosition, before its bindMatrixInverse (folded into ext.velModel)
    const rest = bind.mul(vec4(positionGeometry, 1));
    const posed = (row) => bone(index.x, row).mul(rest).mul(weight.x)
      .add(bone(index.y, row).mul(rest).mul(weight.y))
      .add(bone(index.z, row).mul(rest).mul(weight.z))
      .add(bone(index.w, row).mul(rest).mul(weight.w));
    now = model.mul(vec4(posed(0).xyz, 1));
    before = prevModel.mul(vec4(posed(1).xyz, 1));
  } else if (features.includes('instbig') || features.includes('inst')) {
    const inst = data('velInst', 8, 2);
    const s = [sentinel(), sentinel(), sentinel(), sentinel()];
    const info = uniform(new THREE.Vector4(...s)).setName('velInfo');
    props['ext.velInfo'] = { kind: 'vec4', value: s };
    const w = int(info.x);
    const col = (c) => {
      const k = int(instanceIndex).mul(4).add(c);
      return textureLoad(inst, ivec2(k.mod(w), k.div(w)));
    };
    const prevInst = mat4(col(0), col(1), col(2), col(3));
    now = model.mul(vec4(positionLocal, 1)); // r187's instancing has placed positionLocal
    before = prevModel.mul(prevInst.mul(vec4(positionGeometry, 1)));
  } else {
    now = model.mul(vec4(positionLocal, 1));
    before = prevModel.mul(vec4(positionLocal, 1));
  }
  const clipNow = varying(viewProj.mul(now), 'vVelNow');
  const clipBefore = varying(prevViewProj.mul(before), 'vVelBefore');
  const v = clipNow.xy.div(clipNow.w).sub(clipBefore.xy.div(clipBefore.w)).mul(0.5);
  mat.outputNode = vec4(v, 0, 1);
}
