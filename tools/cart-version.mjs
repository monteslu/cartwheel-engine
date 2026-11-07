#!/usr/bin/env node
// A cart's manifest version, every build (cmake/app.cmake cw_add_app runs it;
// the rule is tools/lib/app-version.mjs).
//
//   node tools/cart-version.mjs update --out <file> --dir <src> [--set <v>] [--fallback <v>] --wasc <cart>
//     before the app links: resolve the version (--set, else the git checkout
//     of --dir, else --fallback, else 0.1.0) and write it to <file> when it
//     changed. When <cart> exists and was packed with another version (a new
//     tag, a commit, a tagged tree with changes), pack it again with the same
//     arguments and the new version: the manifest follows the version without
//     a relink, and a link that follows packs with it too.
//   node tools/cart-version.mjs pack --version-file <file> --wasc <cart> -- <pack command...>
//     after the link: run the pack command with --version <the file's> and
//     --output <cart>.tmp, rename it over <cart> (a running host keeps its
//     open archive), and remember the command and version in <cart>.pack.json.
// A version that is not semver fails either step.
import { spawnSync } from 'node:child_process';
import { existsSync, readFileSync, renameSync, writeFileSync } from 'node:fs';
import { resolveAppVersion, semverProblem } from './lib/app-version.mjs';

const argv = process.argv.slice(2);
const mode = argv[0];
const dash = argv.indexOf('--');
const opts = dash >= 0 ? argv.slice(1, dash) : argv.slice(1);
const command = dash >= 0 ? argv.slice(dash + 1) : [];
const opt = (k) => { const i = opts.indexOf(k); return i >= 0 ? opts[i + 1] ?? '' : undefined; };
const die = (m) => { console.error(`cart-version: ${m}`); process.exit(1); };
const read = (f) => { try { return readFileSync(f, 'utf8'); } catch { return null; } };

/** run the pack command with this version into <cart>, and remember both */
function pack(cmd, version, wasc) {
  const tmp = `${wasc}.tmp`;
  const r = spawnSync(cmd[0], [...cmd.slice(1), '--version', version, '--output', tmp], { stdio: ['ignore', 'ignore', 'inherit'] });
  if (r.status !== 0) die(`packing ${wasc} failed (${r.error ? r.error.message : `exit ${r.status}`})`);
  renameSync(tmp, wasc);
  writeFileSync(`${wasc}.pack.json`, JSON.stringify({ version, command: cmd }) + '\n');
}

if (mode === 'update') {
  const out = opt('--out'), wasc = opt('--wasc');
  if (!out || !wasc) die('update needs --out and --wasc');
  const { version, error } = resolveAppVersion({ set: opt('--set'), dir: opt('--dir'), fallback: opt('--fallback') });
  if (error) die(`${wasc}: ${error}`);
  if (read(out) !== version + '\n') writeFileSync(out, version + '\n');
  const last = existsSync(wasc) && JSON.parse(read(`${wasc}.pack.json`) || 'null');
  if (last && last.version !== version) {
    pack(last.command, version, wasc);
    console.log(`cart-version: ${wasc} repacked as ${version} (was ${last.version})`);
  }
} else if (mode === 'pack') {
  const file = opt('--version-file'), wasc = opt('--wasc');
  if (!file || !wasc || !command.length) die('pack needs --version-file, --wasc and -- <pack command>');
  const version = (read(file) || '').trim();
  const why = semverProblem(version);
  if (why) die(`${file}: version "${version}" is not semver (${why})`);
  pack(command, version, wasc);
} else {
  die('usage: cart-version.mjs update --out <file> --dir <src> [--set <v>] [--fallback <v>] --wasc <cart> | pack --version-file <file> --wasc <cart> -- <pack command...>');
}
