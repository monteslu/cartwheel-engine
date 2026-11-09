#!/usr/bin/env node
// A cart's manifest version, every build (cmake/app.cmake cw_add_app runs it;
// the rule is tools/lib/app-version.mjs).
//
//   node tools/cart-version.mjs update --out <file> --dir <src> [--set <v>] [--fallback <v>] --wasc <cart> --record <json>
//     before the app links: resolve the version (--set, else the git checkout
//     of --dir, else --fallback, else 0.1.0) and write it to <file> when it
//     changed. When <cart> exists and was packed with another version (a new
//     tag, a commit, a tagged tree with changes), pack it again with the same
//     arguments and the new version: the manifest follows the version without
//     a relink, and a link that follows packs with it too.
//   node tools/cart-version.mjs pack --version-file <file> --wasc <cart> --record <json> [--reuse-assets] -- <pack command...>
//     after the link: run the pack command with --version <the file's> and
//     --output <cart>.tmp, rename it over <cart> (a running host keeps its
//     open archive), and remember the command and version in <json> (beside the
//     build's other files, not in the carts directory).
//     --reuse-assets (dev carts, CW_CART_DEV): when the pack command, the version and every file under its
//     --assets and --art directories (path and bytes; size and time above 1 MiB) are what the last pack had, the new cart.wasm is
//     spliced into the last archive instead (tools/lib/wasc-splice.mjs): the same entries and contents
//     without deflating every asset again, the slowest step of a dev cart's link after a code change.
// A version that is not semver fails either step.
import { spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { existsSync, readFileSync, readdirSync, renameSync, statSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { resolveAppVersion, semverProblem } from './lib/app-version.mjs';
import { spliceWasm } from './lib/wasc-splice.mjs';

const argv = process.argv.slice(2);
const mode = argv[0];
const dash = argv.indexOf('--');
const opts = dash >= 0 ? argv.slice(1, dash) : argv.slice(1);
const command = dash >= 0 ? argv.slice(dash + 1) : [];
const opt = (k) => { const i = opts.indexOf(k); return i >= 0 ? opts[i + 1] ?? '' : undefined; };
const die = (m) => { console.error(`cart-version: ${m}`); process.exit(1); };
const read = (f) => { try { return readFileSync(f, 'utf8'); } catch { return null; } };

/** what a pack's archive holds besides cart.wasm: the command, the version, and every file the
 *  command's --assets and --art directories hold (path and bytes, or size and modification time above
 *  1 MiB; links followed) */
function inputsKey(cmd, version) {
  const h = createHash('sha256');
  h.update(JSON.stringify({ cmd, version }));
  const walk = (dir, rel) => {
    let names;
    try { names = readdirSync(dir).sort(); } catch { h.update(`missing ${dir}\n`); return; }
    for (const n of names) {
      const p = join(dir, n), st = statSync(p, { throwIfNoEntry: false });
      if (!st) { h.update(`gone ${rel}${n}\n`); continue; }
      if (st.isDirectory()) walk(p, `${rel}${n}/`);
      // a small file by its bytes (stage-assets.mjs writes cwfiles.txt again every link), a big one by size and time
      else if (st.size <= 1 << 20) h.update(`${rel}${n} `).update(readFileSync(p)).update('\n');
      else h.update(`${rel}${n} ${st.size} ${st.mtimeMs}\n`);
    }
  };
  for (const flag of ['--assets', '--art']) {
    const i = cmd.indexOf(flag);
    if (i >= 0) { h.update(flag + '\n'); walk(cmd[i + 1], ''); }
  }
  return h.digest('hex');
}

/** run the pack command with this version into <cart>, and remember both; with reuse, splice the new
 *  cart.wasm into the last archive when nothing else it holds has changed */
function pack(cmd, version, wasc, record, reuse = false) {
  const tmp = `${wasc}.tmp`;
  const key = reuse ? inputsKey(cmd, version) : undefined;
  const last = reuse && existsSync(wasc) ? JSON.parse(read(record) || 'null') : null;
  const w = cmd.indexOf('--wasm');
  if (last && last.key === key && w >= 0) {
    const ok = spliceWasm(wasc, cmd[w + 1], tmp);
    if (ok === true) {
      renameSync(tmp, wasc);
      writeFileSync(record, JSON.stringify({ version, command: cmd, key }) + '\n');
      if (process.env.CW_CART_PACK_VERBOSE) console.log(`cart-version: ${wasc}: cart.wasm spliced in, assets kept`);
      return;
    }
    console.log(`cart-version: ${wasc}: packing in full (${ok})`);
  }
  const r = spawnSync(cmd[0], [...cmd.slice(1), '--version', version, '--output', tmp], { stdio: ['ignore', 'ignore', 'inherit'] });
  if (r.status !== 0) die(`packing ${wasc} failed (${r.error ? r.error.message : `exit ${r.status}`})`);
  renameSync(tmp, wasc);
  writeFileSync(record, JSON.stringify({ version, command: cmd, ...(key ? { key } : {}) }) + '\n');
}

if (mode === 'update') {
  const out = opt('--out'), wasc = opt('--wasc'), record = opt('--record');
  if (!out || !wasc || !record) die('update needs --out, --wasc and --record');
  const { version, error } = resolveAppVersion({ set: opt('--set'), dir: opt('--dir'), fallback: opt('--fallback') });
  if (error) die(`${wasc}: ${error}`);
  if (read(out) !== version + '\n') writeFileSync(out, version + '\n');
  const last = existsSync(wasc) && JSON.parse(read(record) || 'null');
  if (last && last.version !== version) {
    pack(last.command, version, wasc, record);
    console.log(`cart-version: ${wasc} repacked as ${version} (was ${last.version})`);
  }
} else if (mode === 'pack') {
  const file = opt('--version-file'), wasc = opt('--wasc'), record = opt('--record');
  if (!file || !wasc || !record || !command.length) die('pack needs --version-file, --wasc, --record and -- <pack command>');
  const version = (read(file) || '').trim();
  const why = semverProblem(version);
  if (why) die(`${file}: version "${version}" is not semver (${why})`);
  pack(command, version, wasc, record, opts.includes('--reuse-assets'));
} else {
  die('usage: cart-version.mjs update --out <file> --dir <src> [--set <v>] [--fallback <v>] --wasc <cart> --record <json> | pack --version-file <file> --wasc <cart> --record <json> [--reuse-assets] -- <pack command...>');
}
