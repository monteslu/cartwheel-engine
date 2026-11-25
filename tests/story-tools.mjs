// The story and speech tools (tools/story-compile.mjs, tools/speech-bake.mjs), no GPU:
//   - the committed story_scene outputs (assets/main.cwstory, lines.json, tests/unit/story_data.c) are what the script
//     compiles to now (--check); control: the script changed by one word, --check must fail;
//   - script errors name the line: an unknown speaker, an option outside a choice, a scene not closed, a choice with no
//     options, a goto to no scene, an "or" with nothing after it; each must be refused;
//   - a speech pack (--tone, the model's stand-in) holds one entry per voiced line under the key the runtime computes,
//     sorted, its ADPCM within 3% of the source and its mouth closed at both ends; the committed main.cwspeech has a
//     voice for every line of the script.
import { spawnSync } from 'node:child_process';
import { mkdtempSync, readFileSync, writeFileSync, rmSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { tmpdir } from 'node:os';
import { compile, voicedLines, lineKey } from '../tools/story-compile.mjs';
import { adpcmEncode, adpcmDecode, tone, envelope } from '../tools/speech-bake.mjs';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const S = join(ROOT, 'samples/story_scene');
let bad = 0;
const ok = (cond, what) => { console.log(`${cond ? 'ok  ' : 'FAIL'} ${what}`); if (!cond) bad++; };
const node = (args) => spawnSync(process.execPath, args, { encoding: 'utf8' });
const tmp = mkdtempSync(join(tmpdir(), 'cw-story-'));
try {
  const args = ['tools/story-compile.mjs', join(S, 'story.story'), '-o', join(S, 'assets/main.cwstory'), '--lines', join(S, 'lines.json'), '--c', join(ROOT, 'tests/unit/story_data.c'), '--check'];
  const r = spawnSync(process.execPath, args, { cwd: ROOT, encoding: 'utf8' });
  ok(r.status === 0, `the committed story_scene outputs are the script's (${(r.stderr || '').trim() || 'fresh'})`);
  // the control: one word changed
  const changed = join(tmp, 'changed.story');
  writeFileSync(changed, readFileSync(join(S, 'story.story'), 'utf8').replace('Safe roads.', 'Safe roads, friend.'));
  const c = node(['tools/story-compile.mjs', changed, '-o', join(S, 'assets/main.cwstory'), '--check'].map((a, i) => (i === 0 ? join(ROOT, a) : a)));
  ok(c.status === 1 && /stale/.test(c.stderr), 'control: a changed script makes --check fail');
  // errors that name their line
  const head = 'cast a "A" voice=af_heart\nflag f\n';
  const cases = [
    ['an unknown speaker', head + 'scene s\n  say b "hi"\nend\n', 4],
    ['an option outside a choice', head + 'scene s\n  option "x"\nend\n', 4],
    ['a scene not closed', head + 'scene s\n  say a "hi"\n', 3],
    ['a choice with no options', head + 'scene s\n  choice "p"\n  end\nend\n', 5],
    ['a goto to no scene', head + 'scene s\n  goto t\nend\n', 4],
    ['an or with nothing after it', head + 'scene s\n  if flag f or\n    say a "x"\n  end\nend\n', 4],
  ];
  for (const [what, src, line] of cases) {
    let msg = '';
    try { compile(src, 'x.story'); } catch (e) { msg = e.message; }
    ok(msg.startsWith(`x.story:${line}:`), `${what} is refused at its line (${msg || 'accepted'})`);
  }
  // a pack: keys, order, ADPCM, the mouth
  const st = compile(readFileSync(join(S, 'story.story'), 'utf8'));
  const lines = voicedLines(st);
  ok(lines.every((l) => l.key === lineKey(l.who, l.text)) && lines.length === 15, `${lines.length} voiced lines keyed "who|text"`);
  writeFileSync(join(tmp, 'lines.json'), JSON.stringify(lines));
  const b = node([join(ROOT, 'tools/speech-bake.mjs'), join(tmp, 'lines.json'), '-o', join(tmp, 'p.cwspeech'), '--tone', '--cache', join(tmp, 'cache')]);
  ok(b.status === 0, 'speech-bake --tone packs the lines');
  const p = readFileSync(join(tmp, 'p.cwspeech')), n = p.readUInt32LE(8);
  const keys = Array.from({ length: n }, (_, i) => p.readUInt32LE(20 + i * 28));
  ok(n === lines.length && keys.every((k, i) => !i || k > keys[i - 1]) && lines.every((l) => keys.includes(l.key)), `the pack holds ${n} lines, sorted by key, every line found`);
  ok((p.readUInt32LE(16) & 1) === 1, 'the pack says it holds tone stand-ins');
  const pcm = tone('Mind the third plank.', 1), dec = adpcmDecode(adpcmEncode(pcm), pcm.length);
  let e = 0, s = 0; for (let i = 0; i < pcm.length; i++) { e += (pcm[i] - dec[i]) ** 2; s += pcm[i] ** 2; }
  ok(Math.sqrt(e / s) < 0.03, `ADPCM round trip within 3% (${(100 * Math.sqrt(e / s)).toFixed(2)}%)`);
  const env = envelope(pcm, 24000);
  ok(env[0] < 30 && env[env.length - 2] < 60 && Math.max(...env.filter((_, i) => i % 2 === 0)) > 180, 'the mouth opens with the sound and is closed at both ends');
  // the committed pack has every line of the script, spoken (not tone)
  const m = readFileSync(join(S, 'assets/main.cwspeech')), mn = m.readUInt32LE(8);
  const mk = Array.from({ length: mn }, (_, i) => m.readUInt32LE(20 + i * 28));
  ok(lines.every((l) => mk.includes(l.key)) && (m.readUInt32LE(16) & 1) === 0, `samples/story_scene/assets/main.cwspeech voices every line (${mn}) with the model`);
} finally { rmSync(tmp, { recursive: true, force: true }); }
console.log(bad ? `story-tools: ${bad} FAIL` : 'story-tools: ok');
process.exit(bad ? 1 : 0);
