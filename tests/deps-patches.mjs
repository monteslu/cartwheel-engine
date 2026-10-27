// scripts/fetch-deps.sh and patches/<dep>/: a dependency checkout with patches applied (by an earlier run, at an
// older pin, or by a patch since dropped from patches/) must move to a new pin and take the current patches cleanly,
// and a patch that is already applied is skipped, not an error. A patch that neither applies nor is applied must
// still stop the fetch (the control).
//
//   node tests/deps-patches.mjs        (no GPU; makes a throw-away fixture under out/deps-patches)
import { execFileSync, spawnSync } from 'node:child_process';
import { cpSync, mkdirSync, rmSync, writeFileSync, readFileSync, existsSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const fx = join(ROOT, 'out', 'deps-patches');
rmSync(fx, { recursive: true, force: true });
mkdirSync(join(fx, 'scripts'), { recursive: true });
cpSync(join(ROOT, 'scripts', 'fetch-deps.sh'), join(fx, 'scripts', 'fetch-deps.sh'));
const git = (cwd, ...a) => execFileSync('git', ['-c', 'user.name=t', '-c', 'user.email=t@t', ...a], { cwd, encoding: 'utf8' }).trim();
let bad = 0;
const check = (name, ok, extra = '') => { console.log(`${ok ? 'ok  ' : 'FAIL'} ${name} ${ok ? '' : extra}`); if (!ok) bad++; };
const run = () => spawnSync('bash', [join(fx, 'scripts', 'fetch-deps.sh')], { cwd: fx, encoding: 'utf8' });

// upstream: two commits that both touch the file the patches change
const up = join(fx, 'upstream'); mkdirSync(up);
git(up, 'init', '-q');
writeFileSync(join(up, 'a.c'), 'one\ntwo\nthree\nfour\nfive\nsix\n'); writeFileSync(join(up, 'b.c'), 'b1\n');
git(up, 'add', '.'); git(up, 'commit', '-qm', 'one');
const pin1 = git(up, 'rev-parse', 'HEAD');
writeFileSync(join(up, 'b.c'), 'b2\n'); writeFileSync(join(up, 'a.c'), 'one\ntwo\nthree\nfour\nfive\nSIX\n'); git(up, 'commit', '-qam', 'two');
const pin2 = git(up, 'rev-parse', 'HEAD');
const lock = (rev) => writeFileSync(join(fx, 'scripts', 'deps.lock'), `libx ${up} ${rev}\n`);
// patch 1 edits a tracked file, patch 2 creates a new one
const pdir = join(fx, 'patches', 'libx'); mkdirSync(pdir, { recursive: true });
const p1 = '--- a/a.c\n+++ b/a.c\n@@ -1,3 +1,3 @@\n one\n-two\n+TWO\n three\n';
const p2 = '--- /dev/null\n+++ b/new.c\n@@ -0,0 +1 @@\n+added\n';
writeFileSync(join(pdir, '0001-edit.patch'), p1);
writeFileSync(join(pdir, '0002-add.patch'), p2);
const dep = join(fx, 'deps', 'libx');
const content = () => [readFileSync(join(dep, 'a.c'), 'utf8'), existsSync(join(dep, 'new.c')) ? readFileSync(join(dep, 'new.c'), 'utf8') : null];

lock(pin1);
let r = run();
check('first fetch applies both patches', r.status === 0 && /TWO/.test(content()[0]) && content()[1] === 'added\n', r.stdout + r.stderr);
r = run();
check('a second fetch at the same pin (patches already applied) succeeds', r.status === 0 && /TWO/.test(content()[0]) && content()[1] === 'added\n', r.stdout + r.stderr);
// the pin moves while the patches are applied (what stopped a build after a three.c repin)
lock(pin2);
r = run();
check('moving the pin with the patches applied succeeds', r.status === 0, r.stdout + r.stderr);
check('the checkout is at the new pin', git(dep, 'rev-parse', 'HEAD') === pin2);
check('the patches are applied on the new pin', content()[0] === 'one\nTWO\nthree\nfour\nfive\nSIX\n' && content()[1] === 'added\n', JSON.stringify(content()));
// a patch dropped from patches/ (folded upstream) whose change is still in the checkout: the next pin move restores it
rmSync(join(pdir, '0001-edit.patch'));
writeFileSync(join(dep, 'b.c'), 'left over from a dropped patch\n');
lock(pin1);
r = run();
check('a dropped patch\'s leftover change does not block the move', r.status === 0 && git(dep, 'rev-parse', 'HEAD') === pin1, r.stdout + r.stderr);
check('the leftover is gone and the dropped patch is not applied', readFileSync(join(dep, 'b.c'), 'utf8') === 'b1\n' && !/TWO/.test(content()[0]));
// an already-applied patch that the reset cannot undo (its created file is tracked nowhere and still there)
writeFileSync(join(pdir, '0001-edit.patch'), p1);
writeFileSync(join(dep, 'other-untracked.txt'), 'keep me\n');
r = run();
check('untracked files no patch creates are kept', r.status === 0 && existsSync(join(dep, 'other-untracked.txt')), r.stdout + r.stderr);
// control: a patch that does not apply and is not applied must stop the fetch
writeFileSync(join(pdir, '0003-bad.patch'), '--- a/a.c\n+++ b/a.c\n@@ -1,2 +1,2 @@\n nothing\n-like\n+this\n');
r = run();
check('a patch that does not apply stops the fetch (control)', r.status !== 0 && /patch failed/.test(r.stderr), r.stdout + r.stderr);

rmSync(fx, { recursive: true, force: true });
if (bad) { console.log(`deps-patches: ${bad} failed`); process.exit(1); }
console.log('deps-patches: all ok');
