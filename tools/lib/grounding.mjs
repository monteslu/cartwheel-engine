// The judge of tests/quality-grounding.mjs: which shadows a preset keeps at the bases of the casters High shades.
// Pure (images and the shadow_contacts list in, counts out), so tests/quality-grounding-judge.mjs can check it
// without a GPU.

/** a pixel's luminance */
const lum = (img, x, y) => { const i = 4 * (y * img.w + x); return 0.2126 * img.rgba[i] + 0.7152 * img.rgba[i + 1] + 0.0722 * img.rgba[i + 2]; };

/** the share of a window around (x, y), a twentieth of the frame high, where a is 12% darker than b */
export function darker(a, b, cx, cy) {
  const r = Math.max(4, Math.round(a.h / 40));
  let n = 0, d = 0;
  for (let y = Math.max(0, cy - r); y < Math.min(a.h, cy + r); y++)
    for (let x = Math.max(0, cx - r); x < Math.min(a.w, cx + r); x++) {
      n++;
      const la = lum(a, x, y), lb = lum(b, x, y);
      if (lb > 8 && la < lb * 0.88) d++;
    }
  return n ? d / n : 0;
}

/**
 * The bases High shades, and which of them the subject shades too. Each frame is { contacts, img } (contacts from the
 * shadow_contacts request, one per caster, in the same order on both runs); highRef and ref are the same frames with no
 * light casting. A base counts when it is in front of the camera, inside the frame, of a caster at least minSize on
 * screen and darker on High than High's shadowless frame. The base is the one shadow_contacts gives: the lowest point
 * under the middle of the caster's box, or for a long caster (along: a wall, a fence, a row of stands) the point of
 * its base line nearest the camera; a caster whose box stands around the camera (around: a ring of walls, a floor)
 * has no single base and is not judged. centre asks for the old rule (every caster judged at its box's middle, cx and
 * cy), which tests/quality-grounding-judge.mjs uses to show that rule wrong.
 * @returns {{ n: number, shadowed: number, missing: string[], around: number }}
 */
export function judge(high, highRef, subject, ref, { minSize = 0.025, centre = false } = {}) {
  const pt = (c) => (centre && 'cx' in c ? { x: c.cx, y: c.cy, front: c.cfront } : c);
  const at = (c, img) => [Math.round(pt(c).x * img.w), Math.round(pt(c).y * img.h)];
  const seen = high.contacts.map((c, i) => ({ c, v: subject.contacts[i] }))
    .filter(({ c }) => pt(c).front && c.size >= minSize && pt(c).x > 0.02 && pt(c).x < 0.98 && pt(c).y > 0.02 && pt(c).y < 0.98);
  const around = seen.filter(({ c }) => c.around).length;
  const list = seen.filter(({ c }) => centre || !c.around).filter(({ c }) => darker(high.img, highRef.img, ...at(c, high.img)) >= 0.04);
  let shadowed = 0;
  const missing = [];
  for (const { c, v } of list) {
    const [px, py] = at(c, subject.img);
    const culled = v && v.culled;
    if (!culled && darker(subject.img, ref.img, px, py) >= 0.04) shadowed++;
    else missing.push(`(${px},${py}) size ${c.size.toFixed(3)}${culled ? ' culled' : ''}${c.around ? ' around' : ''}${c.along ? ' along' : ''}`);
  }
  return { n: list.length, shadowed, missing, around };
}
