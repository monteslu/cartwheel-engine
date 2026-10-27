// scripts/fetch-deps.sh and shared deps/: a worktree whose deps/NAME is a symlink to another checkout
// (which another branch may have moved) must be caught by --verify and healed by a plain fetch into a
// private copy, without ever moving the shared checkout.
//
//   node tests/deps-pins.mjs        (no GPU; makes a throw-away fixture under out/deps-pins)
import { execFileSync, spawnSync } from 'node:child_process';
import { cpSync, mkdirSync, rmSync, writeFileSync, symlinkSync, lstatSync, readFileSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const fx = join(ROOT, 'out', 'deps-pins');
rmSync(fx, { recursive: true, force: true });
mkdirSync(join(fx, 'scripts'), { recursive: true });
cpSync(join(ROOT, 'scripts', 'fetch-deps.sh'), join(fx, 'scripts', 'fetch-deps.sh'));
mkdirSync(join(fx, 'scripts', 'lib'));
cpSync(join(ROOT, 'scripts', 'lib', 'check-deps.sh'), join(fx, 'scripts', 'lib', 'check-deps.sh'));
const git = (cwd, ...a) => execFileSync('git', ['-c', 'user.name=t', '-c', 'user.email=t@t', ...a], { cwd, encoding: 'utf8' }).trim();

// an upstream with two commits, and the "shared checkout" another branch moved to the second one
const up = join(fx, 'upstream'); mkdirSync(up);
git(up, 'init', '-q'); writeFileSync(join(up, 'f'), '1'); git(up, 'add', 'f'); git(up, 'commit', '-qm', 'one');
const pin = git(up, 'rev-parse', 'HEAD');
writeFileSync(join(up, 'f'), '2'); git(up, 'commit', '-qam', 'two');
const moved = git(up, 'rev-parse', 'HEAD');
const shared = join(fx, 'shared'); git(fx, 'clone', '-q', up, shared);
writeFileSync(join(fx, 'scripts', 'deps.lock'), `libx ${up} ${pin}\n`);
mkdirSync(join(fx, 'deps'));
symlinkSync(shared, join(fx, 'deps', 'libx'));
const run = (...a) => spawnSync('bash', [join(fx, 'scripts', 'fetch-deps.sh'), ...a], { cwd: fx, encoding: 'utf8' });
let bad = 0;
const check = (name, ok, extra = '') => { console.log(`${ok ? 'ok  ' : 'FAIL'} ${name} ${ok ? '' : extra}`); if (!ok) bad++; };

// control: the drifted symlink MUST be reported (this is the check that has to be able to fail)
let r = run('--verify');
check('verify flags the drifted shared symlink (control)', r.status === 1 && /DRIFT libx/.test(r.stdout), r.stdout + r.stderr);
// the build's guard: a dep OFF its pin stops it and names the fix; a shared symlink at its pin does not
const guard = () => spawnSync('bash', [join(fx, 'scripts', 'lib', 'check-deps.sh'), fx], { cwd: fx, encoding: 'utf8' });
r = guard();
check('build guard stops on a dependency off its pin and names the fix (control)', r.status === 1 && /DRIFT libx/.test(r.stderr) && /fetch-deps\.sh/.test(r.stderr), r.stdout + r.stderr);
r = spawnSync('bash', [join(fx, 'scripts', 'lib', 'check-deps.sh'), fx], { cwd: fx, encoding: 'utf8', env: { ...process.env, CW_SKIP_DEPS_CHECK: '1' } });
check('CW_SKIP_DEPS_CHECK=1 skips the guard', r.status === 0);
// heal
r = run();
check('fetch succeeds', r.status === 0, r.stdout + r.stderr);
check('deps/libx is a private directory now', !lstatSync(join(fx, 'deps', 'libx')).isSymbolicLink());
check('deps/libx is at the pin', git(join(fx, 'deps', 'libx'), 'rev-parse', 'HEAD') === pin);
check('the shared checkout was not moved', git(shared, 'rev-parse', 'HEAD') === moved);
check('file content is the pinned one', readFileSync(join(fx, 'deps', 'libx', 'f'), 'utf8') === '1');
check('build guard passes at the pin', guard().status === 0);
r = run('--verify');
check('verify is clean after the fetch', r.status === 0, r.stdout + r.stderr);
// a shared checkout that is already AT the pin: the private copy must still hold the files (a copy made without a
// checkout and already at the pin was left empty, and the build then found no sources)
rmSync(join(fx, 'deps', 'libx'), { recursive: true, force: true });
const atpin = join(fx, 'atpin'); git(fx, 'clone', '-q', up, atpin); git(atpin, '-c', 'advice.detachedHead=false', 'checkout', '-q', pin);
symlinkSync(atpin, join(fx, 'deps', 'libx'));
r = run();
check('fetch succeeds from a shared checkout at the pin', r.status === 0, r.stdout + r.stderr);
let content = '';
try { content = readFileSync(join(fx, 'deps', 'libx', 'f'), 'utf8'); } catch {}
check('the private copy of a checkout at the pin has its files', content === '1', JSON.stringify(content));
rmSync(fx, { recursive: true, force: true });
process.exit(bad ? 1 : 0);
