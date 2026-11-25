#!/usr/bin/env node
// Compiles a story script (.story text) into the data the story runtime loads (cartwheel/story.h): a .cwstory file
// packed with the game's assets, and optionally the same bytes as a C array to link into the game.
//
//   node tools/story-compile.mjs story/main.story -o assets/main.cwstory [--c src/story_data.c] [--lines out/lines.json]
//
// --lines writes every voiced line ({key, who, voice, speed, text}) for tools/speech-bake.mjs.
// --c writes `const unsigned char <name>[]` and `const unsigned long <name>_size` (name: --c-name, default story_data).
// Errors name the file and line and exit 1. The script format is in docs/guides/story-voice-director.md:
//
//   cast guide "The Guide" voice=af_sky speed=1 color=#ffd27a
//   stat mercy = 0
//   flag spared
//   scene intro save
//     shot push guide dur=3
//     say guide calm "Wake, wanderer."
//     choice "Answer her" timeout=10 default=1
//       option "Who are you?"
//         say guide "A friend."
//       option "Leave me." if stat mercy < 2
//         add mercy -1
//     end
//     if flag spared and stat mercy >= 1
//       say guide joy "Good."
//     else
//       aside guide "Hm."
//     end
//     emit open_gate 1
//   end
//   pool guide.idle cooldown=20 once
//     line guide "The keep waits." if not flag spared
//   end
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { dirname, basename } from 'node:path';
import { fileURLToPath } from 'node:url';

export const OPS = ['end', 'say', 'aside', 'choice', 'option', 'jump', 'jump_not', 'set', 'clear', 'add', 'setstat', 'wait', 'shot',
  'emit', 'goto', 'call', 'save', 'ret'];
export const EMOTIONS = ['calm', 'joy', 'anger', 'sorrow', 'fear', 'surprise'];
const CMPS = ['>=', '<=', '>', '<', '==', '!='];
export const SHOTS = ['wide', 'establish', 'two', 'ots', 'mcu', 'cu', 'push', 'react', 'insert', 'hero', 'whip', 'top', 'release'];
const PARTS = ['head', 'hand_l', 'hand_r', 'point', 'chest'];

/** FNV-1a of "who|text": the key a voice pack files a line under (speech.c computes the same) */
export function lineKey(who, text) {
  let h = 2166136261;
  for (const b of Buffer.from(who + '|' + text, 'utf8')) { h ^= b; h = Math.imul(h, 16777619) >>> 0; }
  return h >>> 0;
}
export function nameHash(s) { return lineKey('', s); }

/** split a line into words, keeping "quoted strings" whole (\" escapes a quote) */
function tokens(line, err) {
  const out = []; let i = 0;
  while (i < line.length) {
    const c = line[i];
    if (c === ' ' || c === '\t') { i++; continue; }
    if (c === '#') break;
    if (c === '"') {
      let s = ''; i++;
      while (i < line.length && line[i] !== '"') { if (line[i] === '\\' && i + 1 < line.length) i++; s += line[i++]; }
      if (i >= line.length) err('a string is not closed');
      i++; out.push({ str: s }); continue;
    }
    let w = ''; while (i < line.length && line[i] !== ' ' && line[i] !== '\t') w += line[i++];
    out.push(w);
  }
  return out;
}
const opts = (toks) => { const o = {}; for (const t of toks) if (typeof t === 'string' && t.includes('=') && !CMPS.some((c) => t === c)) { const k = t.slice(0, t.indexOf('=')); o[k] = t.slice(k.length + 1); } return o; };

export function compile(src, file = 'story') {
  const lines = src.split(/\r?\n/);
  let ln = 0;
  const err = (m) => { const e = new Error(`${file}:${ln}: ${m}`); e.story = true; throw e; };
  const cast = [], stats = [], flags = [], scenes = [], pools = [], conds = [];
  const beats = [], poolLines = [];
  const castIx = (id) => { const i = cast.findIndex((c) => c.id === id); if (i < 0) err(`no cast member "${id}" (declare it with cast)`); return i; };
  const flagIx = (n, add = false) => { let i = flags.findIndex((f) => f.name === n); if (i < 0) { if (!add) err(`no flag "${n}" (declare it with flag)`); flags.push({ name: n }); i = flags.length - 1; } return i; };
  const statIx = (n) => { const i = stats.findIndex((s) => s.name === n); if (i < 0) err(`no stat "${n}" (declare it with stat)`); return i; };
  const sceneRefs = []; // [beatIndex, name, line]
  const seenRefs = [];
  // a condition: atoms joined by "and", groups of them by "or" (and binds tighter); each atom {kind 0 flag, 1 not flag, 2 stat cmp,
  // 3 seen scene, 4 not seen, 5 or}
  function cond(toks) {
    const atoms = []; let i = 0;
    while (i < toks.length) {
      const t = toks[i];
      if (t === 'and') { i++; continue; }
      if (t === 'or') { if (!atoms.length || atoms[atoms.length - 1].kind === 5) err('or needs a condition on each side'); atoms.push({ kind: 5, idx: 0, cmp: 0, val: 0 }); i++; continue; }
      let neg = false; let k = t; if (k === 'not') { neg = true; k = toks[++i]; }
      if (k === 'flag') { atoms.push({ kind: neg ? 1 : 0, idx: flagIx(toks[i + 1]), cmp: 0, val: 0 }); i += 2; }
      else if (k === 'seen') { const a = { kind: neg ? 4 : 3, idx: -1, cmp: 0, val: 0 }; seenRefs.push([a, toks[i + 1], ln]); atoms.push(a); i += 2; }
      else if (k === 'stat') {
        const c = CMPS.indexOf(toks[i + 2]); if (c < 0) err(`a stat condition needs one of ${CMPS.join(' ')}`);
        const v = Number(toks[i + 3]); if (!Number.isInteger(v)) err('a stat condition compares with a whole number');
        if (neg) err('"not stat" is not allowed: use the opposite comparison');
        atoms.push({ kind: 2, idx: statIx(toks[i + 1]), cmp: c, val: v }); i += 4;
      } else err(`a condition is flag NAME, not flag NAME, stat NAME >= N, seen SCENE or not seen SCENE ("${k}")`);
    }
    if (!atoms.length || atoms[atoms.length - 1].kind === 5) err('an empty condition');
    conds.push(atoms); return conds.length - 1;
  }
  const emit = (b) => { beats.push({ op: 'end', who: -1, a: 0, b: 0, c: 0, f: 0, s: null, s2: null, key: 0, target: 0, cond: -1, line: ln, ...b }); return beats.length - 1; };
  const emo = (t) => { if (typeof t !== 'string') return null; const [n, amt] = t.split(':'); const e = EMOTIONS.indexOf(n); return e < 0 ? null : { e, amt: amt ? Number(amt) : 1 }; };
  // block stack while inside a scene
  let scene = null, pool = null;
  const stack = []; // {kind: 'if'|'choice'|'option', ...}
  function closeOption(ch) { if (ch.open >= 0) { ch.ends.push(emit({ op: 'jump' })); ch.open = -1; } }
  for (ln = 1; ln <= lines.length; ln++) {
    const raw = lines[ln - 1];
    const toks = tokens(raw, err);
    if (!toks.length) continue;
    const [w, ...rest] = toks;
    if (typeof w !== 'string') err('a line starts with a word, not a string');
    const o = opts(rest);
    if (!scene && !pool) {
      if (w === 'cast') {
        const id = rest[0]; if (typeof id !== 'string' || !/^[a-z_][a-z0-9_]*$/.test(id)) err('cast <id> "Name": an id of a-z, 0-9 and _');
        if (cast.some((c) => c.id === id)) err(`cast "${id}" twice`);
        const name = rest.find((t) => typeof t === 'object');
        cast.push({ id, name: name ? name.str : id, voice: o.voice || '', speed: o.speed ? Number(o.speed) : 1,
          color: o.color ? parseInt(o.color.replace('#', ''), 16) : 0xffffff });
      } else if (w === 'stat') {
        if (stats.some((s) => s.name === rest[0])) err(`stat "${rest[0]}" twice`);
        stats.push({ name: rest[0], init: rest[1] === '=' ? Number(rest[2]) : 0 });
      } else if (w === 'flag') { for (const n of rest) if (typeof n === 'string') { if (flags.some((f) => f.name === n)) err(`flag "${n}" twice`); flagIx(n, true); } }
      else if (w === 'scene') {
        const name = rest[0]; if (scenes.some((s) => s.name === name)) err(`scene "${name}" twice`);
        scene = { name, start: beats.length, save: rest.includes('save'), line: ln }; scenes.push(scene);
        if (scene.save) emit({ op: 'save' });
      } else if (w === 'pool') {
        const name = rest[0]; if (pools.some((p) => p.name === name)) err(`pool "${name}" twice`);
        pool = { name, cooldown: o.cooldown ? Number(o.cooldown) : 0, once: rest.includes('once'), priority: o.priority ? Number(o.priority) : 0, start: poolLines.length, count: 0 };
        pools.push(pool);
      } else err(`expected cast, stat, flag, scene or pool, not "${w}"`);
      continue;
    }
    if (pool) {
      if (w === 'end') { pool.count = poolLines.length - pool.start; if (!pool.count) err(`pool "${pool.name}" has no lines`); pool = null; continue; }
      if (w !== 'line') err('a pool holds line <who> [emotion] "text" [if ...] entries');
      const who = castIx(rest[0]); const e = emo(rest[1]);
      const ti = rest.findIndex((t) => typeof t === 'object'); if (ti < 0) err('a line needs its "text"');
      const ifi = rest.indexOf('if');
      poolLines.push({ who, emo: e ? e.e : 0, amt: e ? e.amt : 0, text: rest[ti].str, key: lineKey(cast[who].id, rest[ti].str), cond: ifi >= 0 ? cond(rest.slice(ifi + 1)) : -1, line: ln });
      continue;
    }
    // inside a scene
    switch (w) {
      case 'say': case 'aside': {
        const who = castIx(rest[0]); const e = emo(rest[1]);
        const ti = rest.findIndex((t) => typeof t === 'object'); if (ti < 0) err(`${w} needs its "text"`);
        const text = rest[ti].str; if (!text.trim()) err('an empty line');
        const shot = o.shot ? SHOTS.indexOf(o.shot) : -1; if (o.shot && shot < 0) err(`shot= is one of ${SHOTS.join(' ')}`);
        emit({ op: w, who, a: e ? e.e : 0, f: e ? e.amt : 0, s: text, key: lineKey(cast[who].id, text), b: shot, c: o.to ? castIx(o.to) : -1 });
        break;
      }
      case 'choice': {
        const p = rest.find((t) => typeof t === 'object'); if (!p) err('choice "prompt"');
        const at = emit({ op: 'choice', s: p.str, f: o.timeout ? Number(o.timeout) : 0, b: o.default ? Number(o.default) - 1 : 0 });
        stack.push({ kind: 'choice', at, opts: [], ends: [], open: -1, line: ln });
        break;
      }
      case 'option': {
        const ch = stack[stack.length - 1]; if (!ch || ch.kind !== 'choice') err('option outside a choice');
        closeOption(ch);
        const t = rest.find((x) => typeof x === 'object'); if (!t) err('option "text"');
        const ifi = rest.indexOf('if');
        // an option beat: its text and condition; the choice beat lists them (a = first option beat, c = count)
        const ob = emit({ op: 'option', s: t.str, cond: ifi >= 0 ? cond(rest.slice(ifi + 1)) : -1 });
        ch.opts.push(ob); ch.open = ob;
        break;
      }
      case 'if': case 'elif': case 'else': {
        if (w === 'if') { const j = emit({ op: 'jump_not', cond: cond(rest) }); stack.push({ kind: 'if', pend: j, ends: [], line: ln }); break; }
        const f = stack[stack.length - 1]; if (!f || f.kind !== 'if') err(`${w} without an if`);
        if (f.pend < 0) err(`${w} after else`);
        f.ends.push(emit({ op: 'jump' }));
        beats[f.pend].target = beats.length;
        f.pend = w === 'elif' ? emit({ op: 'jump_not', cond: cond(rest) }) : -1;
        if (w === 'else') f.hadElse = true;
        break;
      }
      case 'end': {
        const f = stack.pop();
        if (!f) { // the scene ends
          emit({ op: 'end' }); scene = null; break;
        }
        if (f.kind === 'if') { if (f.pend >= 0) beats[f.pend].target = beats.length; for (const e of f.ends) beats[e].target = beats.length; }
        else if (f.kind === 'choice') {
          closeOption(f);
          if (!f.opts.length) err('a choice with no options');
          if (f.opts.length > 6) err('a choice holds at most 6 options');
          const cb = beats[f.at]; cb.a = f.opts[0]; cb.c = f.opts.length;
          if (cb.b < 0 || cb.b >= f.opts.length) err('default= names an option by its number, from 1');
          // each option beat's target is the beat after it (where its beats start); the jumps at their ends go past the choice
          for (const e of f.ends) beats[e].target = beats.length;
          f.opts.forEach((ob, k) => { beats[ob].target = k + 1 < f.opts.length ? f.opts[k + 1] : -1; });
          // options are laid out as: option beat, its beats, jump; the runtime finds option i's start as option beat + 1
        }
        break;
      }
      case 'set': case 'clear': emit({ op: w, a: flagIx(rest[0]) }); break;
      case 'add': { const v = Number(rest[1]); if (!Number.isInteger(v)) err('add <stat> <whole number>'); emit({ op: 'add', a: statIx(rest[0]), b: v }); break; }
      case 'setstat': { const v = Number(rest[1]); if (!Number.isInteger(v)) err('setstat <stat> <whole number>'); emit({ op: 'setstat', a: statIx(rest[0]), b: v }); break; }
      case 'wait': { const t = Number(rest[0]); if (!(t >= 0)) err('wait <seconds>'); emit({ op: 'wait', f: t }); break; }
      case 'shot': {
        const ty = SHOTS.indexOf(rest[0]); if (ty < 0) err(`shot is one of ${SHOTS.join(' ')}`);
        const subj = rest.slice(1).filter((t) => typeof t === 'string' && !t.includes('=')).map(castIx);
        if (subj.length > 3) err('a shot frames at most three cast members');
        const part = o.part ? PARTS.indexOf(o.part) : 0; if (part < 0) err(`part= is one of ${PARTS.join(' ')}`);
        emit({ op: 'shot', a: ty, b: subj[0] ?? -1, c: subj[1] ?? -1, target: subj[2] ?? -1, f: o.dur ? Number(o.dur) : 0,
          s2: JSON.stringify({ dist: Number(o.dist || 0), fov: Number(o.fov || 0), part, height: Number(o.height || 0) }) });
        break;
      }
      case 'emit': { if (!rest[0]) err('emit <name> [whole number]'); emit({ op: 'emit', s: rest[0], b: rest[1] ? Number(rest[1]) : 0 }); break; }
      case 'goto': case 'call': { const i = emit({ op: w }); sceneRefs.push([i, rest[0], ln]); break; }
      case 'save': emit({ op: 'save' }); break;
      default: err(`unknown beat "${w}" (say, aside, choice, option, if, elif, else, end, set, clear, add, setstat, wait, shot, emit, goto, call, save)`);
    }
  }
  ln = lines.length;
  if (scene) { ln = scene.line; err(`scene "${scene.name}" is not closed with end`); }
  if (pool) err(`pool "${pool.name}" is not closed with end`);
  for (const [i, n, l] of sceneRefs) { const s = scenes.findIndex((x) => x.name === n); if (s < 0) { ln = l; err(`no scene "${n}"`); } beats[i].a = s; }
  for (const [a, n, l] of seenRefs) { const s = scenes.findIndex((x) => x.name === n); if (s < 0) { ln = l; err(`no scene "${n}"`); } a.idx = s; }
  if (!scenes.length && !pools.length) err('no scenes or pools');
  return { cast, stats, flags, scenes, pools, poolLines, beats, conds };
}

/** the voiced lines: every say, aside and pool line, once per (who, text) */
export function voicedLines(st) {
  const m = new Map();
  const add = (who, text) => { const c = st.cast[who]; const key = lineKey(c.id, text); if (!m.has(key)) m.set(key, { key, who: c.id, voice: c.voice, speed: c.speed, text }); };
  for (const b of st.beats) if (b.op === 'say' || b.op === 'aside') add(b.who, b.s);
  for (const l of st.poolLines) add(l.who, l.text);
  return [...m.values()];
}

// ---- the binary (story.c reads it; every number little-endian) ----
// "CWST" u32 version(1) u32 counts[9]: cast stats flags scenes pools poollines beats conds atoms, u32 strings_size
// cast   : u32 id, u32 name, u32 voice, u32 color, f32 speed                 (strings are offsets into the table)
// stat   : u32 name, i32 init
// flag   : u32 name
// scene  : u32 name, u32 start, u32 flags(1 save)
// pool   : u32 name, u32 start, u32 count, f32 cooldown, u32 flags(1 once), i32 priority
// pline  : i32 who, i32 emotion, f32 amount, u32 text, u32 key, i32 cond
// beat   : u16 op, i16 who, i32 a, i32 b, i32 c, f32 f, u32 s, u32 s2, u32 key, i32 target, i32 cond   (40 bytes)
// cond   : u32 first atom, u32 count
// atom   : u8 kind, u8 cmp, u16 pad, i32 idx, i32 value
// strings: NUL-terminated UTF-8; offset 0 is the empty string ("none")
export function pack(st) {
  const strs = [Buffer.from([0])]; let strLen = 1; const strMap = new Map();
  const S = (s) => { if (s === null || s === undefined || s === '') return 0; if (strMap.has(s)) return strMap.get(s); const b = Buffer.from(s + '\0', 'utf8'); strMap.set(s, strLen); strs.push(b); strLen += b.length; return strMap.get(s); };
  const atoms = []; const condRows = st.conds.map((c) => { const first = atoms.length; atoms.push(...c); return [first, c.length]; });
  const parts = [];
  const u32 = (v) => { const b = Buffer.alloc(4); b.writeUInt32LE(v >>> 0); return b; };
  const i32 = (v) => { const b = Buffer.alloc(4); b.writeInt32LE(v | 0); return b; };
  const f32 = (v) => { const b = Buffer.alloc(4); b.writeFloatLE(v); return b; };
  for (const c of st.cast) parts.push(u32(S(c.id)), u32(S(c.name)), u32(S(c.voice)), u32(c.color), f32(c.speed));
  for (const s of st.stats) parts.push(u32(S(s.name)), i32(s.init));
  for (const f of st.flags) parts.push(u32(S(f.name)));
  for (const s of st.scenes) parts.push(u32(S(s.name)), u32(s.start), u32(s.save ? 1 : 0));
  for (const p of st.pools) parts.push(u32(S(p.name)), u32(p.start), u32(p.count), f32(p.cooldown), u32(p.once ? 1 : 0), i32(p.priority));
  for (const l of st.poolLines) parts.push(i32(l.who), i32(l.emo), f32(l.amt), u32(S(l.text)), u32(l.key), i32(l.cond));
  for (const b of st.beats) {
    const h = Buffer.alloc(4); h.writeUInt16LE(OPS.indexOf(b.op)); h.writeInt16LE(b.who, 2);
    parts.push(h, i32(b.a), i32(b.b), i32(b.c), f32(b.f), u32(S(b.s)), u32(S(b.s2)), u32(b.key), i32(b.target), i32(b.cond));
  }
  for (const [first, n] of condRows) parts.push(u32(first), u32(n));
  for (const a of atoms) { const b = Buffer.alloc(12); b.writeUInt8(a.kind, 0); b.writeUInt8(a.cmp, 1); b.writeInt32LE(a.idx, 4); b.writeInt32LE(a.val, 8); parts.push(b); }
  const strBuf = Buffer.concat(strs);
  const head = Buffer.concat([Buffer.from('CWST'), u32(1), ...[st.cast.length, st.stats.length, st.flags.length, st.scenes.length, st.pools.length,
    st.poolLines.length, st.beats.length, condRows.length, atoms.length].map(u32), u32(strBuf.length)]);
  return Buffer.concat([head, ...parts, strBuf]);
}

export function toC(bin, name) {
  let s = `/* GENERATED by tools/story-compile.mjs: the compiled story (cw_story_desc.data / .size). Do not edit. */\n`;
  s += `const unsigned long ${name}_size = ${bin.length};\nconst unsigned char ${name}[] = {`;
  for (let i = 0; i < bin.length; i++) s += (i % 24 ? '' : '\n ') + bin[i] + ',';
  return s + '\n};\n';
}

if (process.argv[1] && fileURLToPath(import.meta.url) === process.argv[1]) {
  const a = process.argv.slice(2);
  const arg = (k) => { const i = a.indexOf(k); return i >= 0 ? a[i + 1] : null; };
  const input = a.find((x, i) => !x.startsWith('-') && !['-o', '--c', '--lines', '--c-name'].includes(a[i - 1]));
  if (!input) { console.error('usage: story-compile.mjs <file.story> -o <out.cwstory> [--c out.c] [--c-name name] [--lines lines.json]'); process.exit(2); }
  let st;
  try { st = compile(readFileSync(input, 'utf8'), input); } catch (e) { if (e.story) { console.error(e.message); process.exit(1); } throw e; }
  const bin = pack(st);
  const vl = voicedLines(st);
  // --check: write nothing; exit 1 when a committed output is not what the script compiles to now
  const check = a.includes('--check');
  let stale = 0;
  const put = (file, data) => {
    if (check) { let cur = null; try { cur = readFileSync(file); } catch {} if (!cur || !Buffer.from(data).equals(cur)) { console.error(`stale: ${file} (run tools/story-compile.mjs without --check)`); stale++; } return; }
    mkdirSync(dirname(file), { recursive: true }); writeFileSync(file, data);
  };
  if (arg('-o')) put(arg('-o'), bin);
  if (arg('--c')) put(arg('--c'), toC(bin, arg('--c-name') || 'story_data'));
  if (arg('--lines')) put(arg('--lines'), JSON.stringify(vl, null, 1) + '\n');
  if (stale) process.exit(1);
  console.log(`${basename(input)}: ${st.scenes.length} scenes, ${st.beats.length} beats, ${st.pools.length} pools, ${st.poolLines.length} pool lines, ${st.flags.length} flags, ${st.stats.length} stats, ${vl.length} voiced lines, ${bin.length} bytes`);
}
