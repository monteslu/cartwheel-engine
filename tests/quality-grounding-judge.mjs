// The grounding judge (tools/lib/grounding.mjs) on made-up frames, no GPU: the old rule, every caster's base under the
// middle of its box, misjudges casters that do not stand there. The frames hold four props, each with its shadow at
// its base on High and on the subject preset, and:
//   a ring of walls around the camera (around), whose box's middle lands on a patch another light shades on High only
//   (a spot the preset does not cast);
//   a long wall (along) whose box's middle lands on another such patch, while the point of its base line nearest the
//   camera has its own shadow on both.
// Checks:
//   1. the judge passes the frame (the four props and the wall's nearest point kept, the ring not judged);
//   2. control: the old rule (centre) judges both at their boxes' middles and fails the frame, a wrong verdict;
//   3. control: the subject with no shadows fails;
//   4. control: a wide caster that is neither around nor along is still judged at its middle (its missing shadow fails).
//   node tests/quality-grounding-judge.mjs
import { judge } from '../tools/lib/grounding.mjs';

const W = 400, H = 400;
function frame(dark) {
  const rgba = new Uint8Array(W * H * 4).fill(120);
  for (const [cx, cy] of dark)
    for (let y = cy - 12; y < cy + 12; y++) for (let x = cx - 12; x < cx + 12; x++) { const i = 4 * (y * W + x); rgba[i] = rgba[i + 1] = rgba[i + 2] = 50; }
  return { w: W, h: H, rgba };
}
const at = (x, y) => ({ x: x / W, y: y / H, cx: x / W, cy: y / H, front: true, cfront: true, culled: false, around: false, along: false });
const props = [[80, 300], [160, 320], [240, 300], [320, 320]];
const ringMid = [200, 220], wallMid = [300, 120], wallNear = [120, 150];
const contacts = [
  ...props.map(([x, y]) => ({ ...at(x, y), size: 0.1 })),
  { ...at(...ringMid), size: 0.8, around: true },
  { ...at(...wallNear), cx: wallMid[0] / W, cy: wallMid[1] / H, size: 0.7, along: true },
];
const flat = frame([]);
const high = { contacts, img: frame([...props, ringMid, wallMid, wallNear]) }, highRef = { contacts, img: flat };
const subject = { contacts, img: frame([...props, wallNear]) }, ref = { contacts, img: flat };
const pass = (j) => j.n >= 2 && j.shadowed / j.n >= 0.9;
let fail = 0;
const check = (ok, what) => { if (!ok) fail++; console.log(`${ok ? 'ok  ' : 'FAIL'} ${what}`); };

const j = judge(high, highRef, subject, ref);
check(pass(j) && j.n === 5 && j.around === 1, `the frame passes: the wall at its nearest point, the ring around the camera not judged (${j.shadowed} of ${j.n}, ${j.around} around)`);
const old = judge(high, highRef, subject, ref, { centre: true });
check(!pass(old) && old.n === 6 && old.shadowed === 4, `control: the old rule judges the ring and the wall at their boxes' middles and fails the frame wrongly (${old.shadowed} of ${old.n}: ${old.missing.join(', ')})`);
const none = judge(high, highRef, { contacts, img: flat }, ref);
check(!pass(none), `control: no shadows fails (${none.shadowed} of ${none.n})`);
const wide = [...contacts.slice(0, 4), { ...contacts[4], around: false }];
const w = judge({ contacts: wide, img: high.img }, highRef, { contacts: wide, img: subject.img }, ref);
check(!pass(w) && w.n === 5, `control: a wide caster neither around nor along is still judged (${w.shadowed} of ${w.n})`);
console.log(fail ? `quality-grounding-judge: ${fail} failures` : 'ok   quality-grounding-judge');
process.exit(fail ? 1 : 0);
