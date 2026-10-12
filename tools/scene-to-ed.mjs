// Turn a .cwscene into the editor command lines that build it from nothing
// (scene.new, node.add / node.instance, node.set, node.group, node.connect,
// anim.new / anim.track_add / anim.key_add), so a scene can be rebuilt by
// driving ONLY editor commands: over the debug channel (`ed64 <base64>`), in
// a session, or by an agent. The lines are what a person clicking through
// the editor produces; the editor's history replays them.
//
//   node tools/scene-to-ed.mjs <scene.cwscene> [--save <project path>] [--ed64] [--drop <node path>]...
//
// Prints one command line per line (with --ed64: `ed64 <base64>` requests for
// the debug channel). --drop leaves a node (and its subtree) out: a control
// whose result must differ. Exported for tests: sceneToEd(text, opts).
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

// Quote one argument like cwed_quote: bare when it is a plain token.
export function quoteArg(s) {
  if (s !== '' && /^[A-Za-z0-9_.+\-/:,]+$/.test(s)) return s;
  return '"' + s.replace(/\\/g, '\\\\').replace(/"/g, '\\"').replace(/\n/g, '\\n') + '"';
}

// `key=value` / `key="value"` attributes of a section header
function attrs(header) {
  const out = {};
  const re = /([A-Za-z_][A-Za-z0-9_]*)=("((?:[^"\\]|\\.)*)"|[^\s\]]+)/g;
  let m;
  while ((m = re.exec(header))) out[m[1]] = m[3] !== undefined ? m[3].replace(/\\(.)/g, '$1') : m[2];
  return out;
}

export function sceneToEd(text, { save = null, drop = [] } = {}) {
  const lines = [];
  const ext = {};
  const dropped = (path) => drop.some((d) => path === d || path.startsWith(d + '/'));
  let cur = null; // the node path props apply to ('.' root), or an animation key context
  let anim = null; // { player, name, track }
  let rootDone = false;
  const pathOf = (a) => (a.parent === undefined ? '.' : a.parent === '.' ? a.name : `${a.parent}/${a.name}`);
  for (const raw of text.split('\n')) {
    const line = raw.trim();
    if (!line || line.startsWith(';')) continue;
    if (line.startsWith('[')) {
      const kind = line.slice(1).split(/[\s\]]/)[0];
      const a = attrs(line);
      if (kind === 'scene') { cur = null; continue; }
      if (kind === 'ext_scene') { ext[a.id] = a.path; continue; }
      if (kind === 'node') {
        anim = null;
        const path = pathOf(a);
        if (a.parent === undefined) {
          if (!a.type) throw new Error('the root has no type (an inherited scene root is not supported)');
          lines.push(`scene.new type=${quoteArg(a.type)} name=${quoteArg(a.name)}`);
          rootDone = true;
        } else if (!rootDone) {
          throw new Error('a node before the root');
        } else if (dropped(path)) {
          cur = null;
          continue;
        } else if (a.instance) {
          if (!ext[a.instance]) throw new Error(`instance=${a.instance} has no ext_scene`);
          lines.push(`node.instance scene=${quoteArg(ext[a.instance])} parent=${quoteArg(a.parent)} name=${quoteArg(a.name)}`);
        } else if (a.type) {
          lines.push(`node.add type=${quoteArg(a.type)} parent=${quoteArg(a.parent)} name=${quoteArg(a.name)}`);
        } // else: an override of an instanced node: only its properties
        cur = path;
        if (a.groups) for (const g of a.groups.split(',')) lines.push(`node.group path=${quoteArg(path)} group=${quoteArg(g)}`);
        continue;
      }
      if (kind === 'connection') {
        anim = null;
        cur = null;
        if (dropped(a.from) || dropped(a.to)) continue;
        lines.push(`node.connect from=${quoteArg(a.from)} signal=${quoteArg(a.signal)} to=${quoteArg(a.to)} method=${quoteArg(a.method)}` +
          (a.flags && a.flags !== '0' ? ` flags=${a.flags}` : ''));
        continue;
      }
      if (kind === 'animation') {
        cur = null;
        if (dropped(a.player)) { anim = { skip: true }; continue; }
        anim = { player: a.player, name: a.name, track: -1 };
        lines.push(`anim.new player=${quoteArg(a.player)} name=${quoteArg(a.name)}` + (a.length ? ` length=${a.length}` : '') +
          (a.loop ? ` loop=${a.loop}` : ''));
        continue;
      }
      if (kind === 'track') {
        if (!anim) throw new Error('[track] outside an [animation]');
        if (anim.skip) continue;
        anim.track++;
        let l = `anim.track_add player=${quoteArg(anim.player)} anim=${quoteArg(anim.name)}`;
        if (a.type && a.type !== 'value') l += ` kind=${a.type}`;
        if (a.path !== undefined) l += ` path=${quoteArg(a.path)}`;
        if (a.interp) l += ` interp=${a.interp}`;
        lines.push(l);
        if (a.rot || a.enabled) {
          lines.push(`anim.track_set player=${quoteArg(anim.player)} anim=${quoteArg(anim.name)} track=${anim.track}` +
            (a.interp ? ` interp=${a.interp}` : '') + (a.rot ? ` rot=${a.rot}` : '') + (a.enabled ? ` enabled=${a.enabled}` : ''));
        }
        continue;
      }
      throw new Error(`section [${kind}] has no editor command here yet`);
    }
    const eq = line.indexOf(' = ');
    if (eq < 0) throw new Error(`not a property line: ${line}`);
    const key = line.slice(0, eq), value = line.slice(eq + 3);
    if (anim) {
      if (anim.skip) continue;
      lines.push(`anim.key_add player=${quoteArg(anim.player)} anim=${quoteArg(anim.name)} track=${anim.track} time=${key} value=${quoteArg(value)}`);
      continue;
    }
    if (cur === null) continue; // a dropped node's properties
    lines.push(`node.set path=${quoteArg(cur)} prop=${quoteArg(key)} value=${quoteArg(value)}`);
  }
  if (save) lines.push(`scene.save path=${quoteArg(save)}`);
  return lines;
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
  const argv = process.argv.slice(2);
  const file = argv.find((a, i) => !a.startsWith('--') && !['--save', '--drop'].includes(argv[i - 1]));
  if (!file) {
    console.error('usage: scene-to-ed.mjs <scene.cwscene> [--save <path>] [--ed64] [--drop <node path>]...');
    process.exit(2);
  }
  const save = argv.includes('--save') ? argv[argv.indexOf('--save') + 1] : null;
  const drop = argv.flatMap((a, i) => (a === '--drop' ? [argv[i + 1]] : []));
  const out = sceneToEd(readFileSync(file, 'utf8'), { save, drop });
  for (const l of out) console.log(argv.includes('--ed64') ? 'ed64 ' + Buffer.from(l, 'utf8').toString('base64') : l);
}
