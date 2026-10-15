// Export smoke test: export platformer3d through
// tools/cwexport (the library the CLI and the editor use) for every platform,
// run each export wherever this machine can run it, and probe the running
// game's state, with must-fail controls. Prints one status line per platform
// and game: built+run, built-only, or not possible here (and why).
//
//   node tests/export.mjs [linux-x64 linux-arm64 windows-x64 android-arm64 web wasc macos ios project]
//
// Per platform:
//   linux-x64     tarball: the gate's input script played through the exported
//                 binary (CW_INPUT test hook), the gate's expected state checked
//                 (CW_DEBUG_DUMP); the AppImage gives the same state; a live run
//                 answers the debug channel (CW_REMOTE socket: fields, status,
//                 quit). Control: the export with its .cwpak removed must FAIL.
//   linux-arm64   built-only (no arm64 machine or emulator here): ELF machine,
//                 interpreter, libraries, pack identical to x64.
//   windows-x64   under Wine (deps/export/wine) on ANGLE D3D11 over wined3d:
//                 the input script + expected state, and the live debug channel
//                 over Winsock. NOT verified on real Windows.
//   android-arm64 the APK on the AVD (arm64 via NDK translation, guest GLES on
//                 SwiftShader): debug channel over adb reverse, a screenshot
//                 through the channel, real key events (adb input) changing
//                 game state (platformer3d: start from the title).
//   web           the static site served by its own serve.mjs in Chromium
//                 (CW_CHROMIUM_GPU pin): crossOriginIsolated, frames run, real
//                 keyboard input changes state via the page's debug
//                 channel. Control: the same site without COOP/COEP must show
//                 the isolation error and not start.
//   wasc          romdev wasm({op:'conformance'}), the input script on romdev
//                 (expected state), and a run on wcnative (frames render).
//                 Control: a truncated cart must fail conformance.
//   macos         built on a Mac only (an arm64 .app); elsewhere reported as not built
//   ios           NOT built (no Apple SDK here): template structure,
//                 universal ANGLE, pack identical, scripts parse.
//   project       a game project outside the tree (CMakeLists.txt +
//                 project.cwproj, CW_PROJECT_DIR) exported for linux-x64 and run.
//                 Control: the same project with a broken asset path must fail
//                 to export.
import { spawnSync, spawn } from 'node:child_process';
import { existsSync, readFileSync, writeFileSync, mkdirSync, rmSync, readdirSync, statSync, cpSync, renameSync, openSync, readSync, closeSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { exportPresets } from '../tools/cwexport/index.mjs';
import { listenChannel } from './export/channel-server.mjs';
import { readImage, uniqueColors } from '../tools/lib/png.mjs';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
await enterGpuSandbox(); // only the test GPU is visible, no desktop (tools/lib/gpu-sandbox.mjs)

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out/export-test');
const ALL = ['linux-x64', 'linux-arm64', 'windows-x64', 'android-arm64', 'web', 'wasc', 'macos', 'ios', 'project'];
const want = process.argv.slice(2).length ? process.argv.slice(2) : ALL;
const gates = JSON.parse(readFileSync(join(ROOT, 'tests/gates.json'), 'utf8'));
const GAMES = [
  { game: 'platformer3d', project: join(ROOT, 'games/platformer3d/assets'), gate: gates.find((g) => g.name === 'platformer3d') },
];
mkdirSync(OUT, { recursive: true });

const status = []; // {platform, game, result, detail}
let failures = 0;
const report = (platform, game, result, detail) => {
  status.push({ platform, game, result, detail });
  if (result === 'FAIL') failures++;
  console.log(`${result === 'FAIL' ? 'FAIL' : 'ok  '} export ${platform} ${game}: ${result}${detail ? ' - ' + detail : ''}`);
};
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const step = (...a) => { if (process.env.CW_EXPORT_TEST_VERBOSE) console.error('  ..', ...a); };

async function exportOne(project, preset, out) {
  const [r] = await exportPresets({ project, presets: [preset], out });
  if (!r.ok) throw new Error(`export ${preset}: ${r.error}`);
  return r.files;
}

/** the gate's expected fields vs a state; mismatches as text */
function mismatches(state, expect) {
  const bad = [];
  for (const [k, v] of Object.entries(expect)) if (!state || JSON.stringify(state[k]) !== JSON.stringify(v)) bad.push(`${k}=${JSON.stringify(state && state[k])} (want ${JSON.stringify(v)})`);
  return bad;
}

/** play the gate's input script through an exported executable (run-cart native = the CW_FRAMES/CW_INPUT hooks) */
function playScript(exe, g, tag, env = {}) {
  const st = join(OUT, `${tag}.state.json`), shot = join(OUT, `${tag}.png`);
  rmSync(st, { force: true });
  // an AppImage mounts itself with FUSE through the setuid fusermount, which cannot work inside the GPU
  // sandbox's user namespace: there the runtime extracts and runs the same payload instead
  const ai = exe.endsWith('.AppImage') && process.env.CW_GPU_SANDBOXED ? { APPIMAGE_EXTRACT_AND_RUN: '1' } : {};
  const r = spawnSync('node', [join(ROOT, 'tools/run-cart.mjs'), '--host', 'native', '--app', g.game, '--exe', exe, '--assets', join(OUT, 'no-loose-assets'),
    '--frames', String(g.gate.frames), '--input', join(ROOT, 'tests', g.gate.input), '--state', st, '--shot', shot],
  { encoding: 'utf8', timeout: 600000, env: { ...process.env, SDL_VIDEODRIVER: 'offscreen', ...ai, ...env } });
  const state = existsSync(st) ? JSON.parse(readFileSync(st, 'utf8')) : null;
  return { ok: r.status === 0, state, shot, err: (r.stderr || '').trim().split('\n').slice(-3).join(' | ') };
}

/** a picture with real content: enough distinct colours */
function picture(png) {
  if (!existsSync(png)) return 0;
  return uniqueColors(readImage(png));
}

/** launch an exported game live (real clock) with CW_REMOTE and probe it over the debug channel */
async function liveProbe(cmd, args, g, env = {}, cwd) {
  const ch = await listenChannel(0);
  const child = spawn(cmd, args, { cwd, env: { ...process.env, SDL_AUDIODRIVER: 'dummy', ...env, CW_REMOTE: `tcp:127.0.0.1:${ch.port}` }, stdio: ['ignore', 'ignore', 'pipe'] });
  let err = '';
  child.stderr.on('data', (d) => (err += d));
  const exited = new Promise((r) => child.on('close', r));
  try {
    const hello = await ch.hello(120000);
    await sleep(1500);
    const st1 = await ch.call('status', 60000);
    await sleep(1500);
    const st2 = await ch.call('status', 60000);
    const fields = await ch.call(`fields ${Object.keys(g.gate.expect)[0]}`, 60000);
    const bad = await ch.call('fields no.such.field', 60000); // a request that must fail
    await ch.call('quit', 30000).catch(() => null);
    const code = await Promise.race([exited, sleep(30000).then(() => 'timeout')]);
    if (code === 'timeout') child.kill('SIGKILL');
    return { hello, advanced: st2.frame > st1.frame, frames: [st1.frame, st2.frame], fields, badFails: !!(bad && bad.error), quit: code !== 'timeout' };
  } catch (e) {
    child.kill('SIGKILL');
    throw new Error(`${e.message}; stderr: ${err.split('\n').slice(-5).join(' | ')}`);
  } finally {
    ch.close();
  }
}

function untar(file, dir) {
  rmSync(dir, { recursive: true, force: true });
  mkdirSync(dir, { recursive: true });
  const r = spawnSync('tar', ['-xzf', file, '-C', dir, '--strip-components=1']);
  if (r.status !== 0) throw new Error(`untar ${file}`);
  return dir;
}
function unzip(file, dir) {
  rmSync(dir, { recursive: true, force: true });
  mkdirSync(dir, { recursive: true });
  const r = spawnSync('unzip', ['-q', file, '-d', dir]);
  if (r.status !== 0) throw new Error(`unzip ${file}`);
  return join(dir, readdirSync(dir)[0]);
}
const elfInfo = (f) => {
  const r = spawnSync('readelf', ['-h', '-d', '-l', f], { encoding: 'utf8' });
  return { machine: (/Machine:\s+(.*)/.exec(r.stdout) || [])[1], needed: [...r.stdout.matchAll(/NEEDED.*\[(.*)\]/g)].map((m) => m[1]),
    interp: (/interpreter: (.*)\]/.exec(r.stdout) || [])[1] };
};
const sameFile = (a, b) => existsSync(a) && existsSync(b) && readFileSync(a).equals(readFileSync(b));

// ── platforms ──────────────────────────────────────────────────────────

async function linuxX64(g) {
  const files = await exportOne(g.project, 'linux', join(OUT, g.game));
  const tgz = files.find((f) => f.endsWith('.tar.gz')), appimage = files.find((f) => f.endsWith('.AppImage'));
  const dir = untar(tgz, join(OUT, g.game, 'linux-run'));
  const exe = join(dir, g.game);
  const run = playScript(exe, g, `${g.game}-linux`);
  const bad = mismatches(run.state, g.gate.expect);
  const colors = picture(run.shot);
  if (!run.ok || bad.length || colors < 64) return report('linux-x64', g.game, 'FAIL', `tarball run: ${run.err} ${bad.join(', ')} colours=${colors}`);
  // the AppImage is the same game
  // (the gate's fields: audio counters follow the audio device's real clock)
  const pick = (st) => JSON.stringify(g.gate.fields.map((k) => st && st[k]));
  const ai = playScript(appimage, g, `${g.game}-appimage`);
  if (!ai.ok || pick(ai.state) !== pick(run.state)) return report('linux-x64', g.game, 'FAIL', `AppImage state differs from the tarball's: ${ai.err}`);
  // live: the launcher, real clock, debug channel socket
  const live = await liveProbe(join(dir, 'run.sh'), [], g, { SDL_VIDEODRIVER: 'offscreen' });
  if (!live.advanced || live.fields.error || !live.badFails || !live.quit) return report('linux-x64', g.game, 'FAIL', `live probe ${JSON.stringify(live)}`);
  // control: the same export without its .cwpak must fail the same checks
  const ctl = join(OUT, g.game, 'linux-control');
  rmSync(ctl, { recursive: true, force: true });
  cpSync(dir, ctl, { recursive: true });
  rmSync(join(ctl, `${g.game}.cwpak`));
  const c = playScript(join(ctl, g.game), g, `${g.game}-linux-control`);
  if (c.ok && !mismatches(c.state, g.gate.expect).length) return report('linux-x64', g.game, 'FAIL', 'CONTROL PASSED: the export without its asset pack still met the expected state');
  report('linux-x64', g.game, 'built+run', `${Object.entries(g.gate.expect).map(([k, v]) => `${k}=${v}`).join(' ')} after ${g.gate.frames} frames of input; AppImage state identical (the gate's fields); live channel frames ${live.frames.join('->')}, ${JSON.stringify(live.fields)}; control (no .cwpak) fails`);
}

async function linuxArm64(g) {
  const files = await exportOne(g.project, 'linux-arm64', join(OUT, g.game));
  const dir = untar(files.find((f) => f.endsWith('.tar.gz')), join(OUT, g.game, 'arm64-check'));
  const e = elfInfo(join(dir, g.game));
  const allowed = new Set(['libdl.so.2', 'libGLESv2.so.2', 'libwebgpu_dawn.so', 'libm.so.6', 'libpthread.so.0', 'libstdc++.so.6', 'libgcc_s.so.1', 'libc.so.6', 'ld-linux-aarch64.so.1']);
  const extra = e.needed.filter((n) => !allowed.has(n));
  const x64pak = join(OUT, g.game, 'linux-run', `${g.game}.cwpak`);
  const ai = files.find((f) => f.endsWith('.AppImage'));
  const aiHead = Buffer.alloc(20);
  const fd = openSync(ai, 'r'); readSync(fd, aiHead, 0, 20, 0); closeSync(fd);
  const aiArm = aiHead.readUInt16LE(18) === 183; // EM_AARCH64
  if (!/AArch64/.test(e.machine || '') || e.interp !== '/lib/ld-linux-aarch64.so.1' || extra.length || !aiArm)
    return report('linux-arm64', g.game, 'FAIL', `machine=${e.machine} interp=${e.interp} extra libs=${extra} appimage arm64=${aiArm}`);
  if (existsSync(x64pak) && !sameFile(x64pak, join(dir, `${g.game}.cwpak`))) return report('linux-arm64', g.game, 'FAIL', 'the asset pack differs from the x64 export');
  report('linux-arm64', g.game, 'built-only', `AArch64 ELF (${e.needed.join(' ')}), AppImage with the aarch64 runtime; not run: no arm64 machine, qemu-user or arm64 Linux emulator on this host`);
}

const WINE = join(ROOT, 'deps/export/wine/bin/wine');
function wineEnv() {
  const auth = spawnSync('sh', ['-c', 'ls -t /run/user/$(id -u)/.mutter-Xwaylandauth.* 2>/dev/null | head -1'], { encoding: 'utf8' }).stdout.trim();
  return { WINEPREFIX: join(OUT, 'wineprefix'), WINEDEBUG: '-all', WINEDLLOVERRIDES: 'mscoree,mshtml=', ...(auth ? { XAUTHORITY: auth } : {}) };
}
async function windowsX64(g) {
  const files = await exportOne(g.project, 'windows', join(OUT, g.game));
  const dir = unzip(files.find((f) => f.endsWith('.zip')), join(OUT, g.game, 'windows-run'));
  for (const f of [`${g.game}.exe`, `${g.game}.cwpak`, 'libEGL.dll', 'libGLESv2.dll', 'd3dcompiler_47.dll', 'webgpu_dawn.dll', 'dxcompiler.dll', 'dxil.dll'])
    if (!existsSync(join(dir, f))) return report('windows-x64', g.game, 'FAIL', `the zip lacks ${f}`);
  if (!existsSync(WINE)) return report('windows-x64', g.game, 'built-only', 'no Wine: scripts/fetch-deps.sh --only export/wine to run it');
  if (!process.env.DISPLAY) return report('windows-x64', g.game, 'built-only', 'no DISPLAY for Wine');
  const env = wineEnv();
  // the exe under Wine, wrapped so run-cart can launch it like a native binary
  const wrap = join(dir, 'run-under-wine.sh');
  writeFileSync(wrap, `#!/bin/sh\ncd "$(dirname "$0")"\nexec "${WINE}" ${g.game}.exe "$@"\n`, { mode: 0o755 });
  const run = playScript(wrap, g, `${g.game}-windows`, env);
  const bad = mismatches(run.state, g.gate.expect);
  const colors = picture(run.shot);
  if (!run.ok || bad.length || colors < 64) return report('windows-x64', g.game, 'FAIL', `wine run: ${run.err} ${bad.join(', ')} colours=${colors}`);
  const live = await liveProbe(WINE, [`${g.game}.exe`], g, env, dir);
  if (!live.advanced || live.fields.error || !live.badFails) return report('windows-x64', g.game, 'FAIL', `live probe under Wine ${JSON.stringify(live)}`);
  spawnSync(join(ROOT, 'deps/export/wine/bin/wineserver'), ['-k'], { env: { ...process.env, ...env } });
  report('windows-x64', g.game, 'built+run (Wine)', `ANGLE D3D11 on Wine's wined3d: expected state after ${g.gate.frames} frames of input, live channel over Winsock frames ${live.frames.join('->')}; not verified on real Windows`);
}

const adb = (...a) => spawnSync('adb', a, { encoding: 'utf8', timeout: 180000 });
async function androidDevice() {
  if (/\tdevice/.test(adb('devices').stdout)) return true;
  const sdk = process.env.ANDROID_HOME || join(process.env.HOME, 'Android/Sdk');
  const emu = join(sdk, 'emulator/emulator');
  if (!existsSync(emu)) return false;
  const avds = spawnSync(emu, ['-list-avds'], { encoding: 'utf8' }).stdout.trim().split('\n').filter(Boolean);
  if (!avds.length) return false;
  // headless, guest GL on SwiftShader (the host GPU is the user's desktop GPU)
  spawn(emu, ['-avd', avds[0], '-no-window', '-no-snapshot', '-no-audio', '-gpu', 'swiftshader_indirect', '-no-boot-anim'],
    { detached: true, stdio: 'ignore' }).unref();
  for (let i = 0; i < 120; i++) {
    await sleep(3000);
    if (adb('shell', 'getprop', 'sys.boot_completed').stdout.trim() === '1') return true;
  }
  return false;
}
async function androidArm64(g) {
  const files = await exportOne(g.project, 'android', join(OUT, g.game));
  const apk = files.find((f) => f.endsWith('.apk'));
  const list = spawnSync('unzip', ['-l', apk], { encoding: 'utf8' }).stdout;
  if (!/lib\/arm64-v8a\/libmain\.so/.test(list) || !/assets\/game\.cwpak/.test(list)) return report('android-arm64', g.game, 'FAIL', 'the APK lacks libmain.so or game.cwpak');
  if (!(await androidDevice())) return report('android-arm64', g.game, 'built-only', 'no device or AVD');
  const pkg = /^org\.[\w.]+/.exec(readFileSync(join(g.project, 'project.cwproj'), 'utf8').match(/export\.android\.app_id\s*=\s*"([^"]+)"/)[1])[0];
  if (adb('install', '-r', apk).status !== 0) return report('android-arm64', g.game, 'FAIL', 'adb install failed');
  const ch = await listenChannel(0);
  try {
    adb('reverse', `tcp:${ch.port}`, `tcp:${ch.port}`);
    adb('shell', 'am', 'force-stop', pkg);
    adb('shell', 'am', 'start', '-n', `${pkg}/org.cartwheel.runtime.CartwheelActivity`, '--es', 'cw_env', `CW_REMOTE=tcp:127.0.0.1:${ch.port}`);
    await ch.hello(180000);
    await sleep(4000);
    const shotFile = join(OUT, `${g.game}-android.png`);
    writeFileSync(shotFile, await ch.screenshot(120000));
    const colors = picture(shotFile);
    // real input through Android's input stack: the key the game binds
    let before, after, key, field;
    field = 'game.state';
    before = (await ch.call(`fields ${field}`))[field];
    key = 'KEYCODE_ENTER';
    adb('shell', 'input', 'keyevent', key);
    for (let i = 0; i < 60; i++) {
      await sleep(1000);
      after = (await ch.call(`fields ${field}`, 60000))[field];
      if (after !== before) break;
    }
    adb('shell', 'am', 'force-stop', pkg);
    if (colors < 64) return report('android-arm64', g.game, 'FAIL', `screenshot through the channel has ${colors} colours`);
    if (after === before) return report('android-arm64', g.game, 'FAIL', `${key} did not change ${field} (${before})`);
    report('android-arm64', g.game, 'built+run (AVD)', `x86_64 AVD running the arm64 APK via NDK translation, guest GLES on SwiftShader; channel over adb reverse; ${key} changed ${field} ${before}->${after}; screenshot ${colors} colours`);
  } finally {
    ch.close();
  }
}

async function web(g) {
  step('web export');
  const files = await exportOne(g.project, 'web', join(OUT, g.game));
  step('web exported');
  const site = files.find((f) => !f.endsWith('.zip'));
  const { launchChromium, chromiumPinned } = await import('../tools/lib/chromium.mjs');
  if (!chromiumPinned()) return report('web', g.game, 'built-only', 'Chromium is not pinned to the test GPU (chromium-gpu config or CW_CHROMIUM_GPU)');
  const { chromium } = await import(join(ROOT, 'deps/wasmcart/node_modules/playwright/index.mjs'));
  const serve = (headers) => {
    // the site's own server (headers on), or a bare one for the control
    const port = 18000 + Math.floor(Math.random() * 2000);
    const args = headers ? [join(site, 'serve.mjs'), String(port)] : ['-e', `require('http').createServer((q,s)=>{const f=require('path').join(${JSON.stringify(site)},decodeURIComponent(q.url.split('?')[0]).replace(/\\/$/,'/index.html'));try{const b=require('fs').readFileSync(f);s.setHeader('Content-Type',f.endsWith('.js')?'text/javascript':f.endsWith('.html')?'text/html':'application/octet-stream');s.end(b)}catch{s.statusCode=404;s.end()}}).listen(${port},'127.0.0.1')`];
    const p = spawn('node', args, { stdio: 'ignore' });
    return { p, url: `http://127.0.0.1:${port}/` };
  };
  step('web launch');
  const { browser, verify } = await launchChromium(chromium, join(ROOT, 'out'));
  step('web launched');
  try {
    const page = await browser.newPage({ viewport: { width: 1280, height: 800 } });
    const logs = [];
    page.on('console', (m) => logs.push(m.text()));
    page.on('pageerror', (e) => logs.push('pageerror: ' + e.message));
    // a standard-mapping gamepad the page reads through navigator.getGamepads
    step('web 1');
    await page.addInitScript(() => {
      window.__pad = { buttons: Array.from({ length: 17 }, () => ({ pressed: false, value: 0 })), axes: [0, 0, 0, 0] };
      navigator.getGamepads = () => [{ id: 'test pad', index: 0, connected: true, mapping: 'standard', ...window.__pad }];
    });
    const s = serve(true);
    step('web 2');
    await sleep(800);
    step('web 3');
    await page.goto(s.url);
    step('web 4');
    await verify(page);
    step('web 5');
    await page.waitForFunction('window.cwStatus && (window.cwStatus.ok === false || window.cwStatus.frames > 30)', null, { timeout: 120000 });
    step('web 6');
    // the web player spreads a scene app's warm-up (shader phase) over frames
    // behind its loading screen, where no ticks run and input is not read:
    // wait for cw.loading to finish before pressing anything
    // (read with the synchronous fields(): the debug mailbox takes one request at a time)
    await page.waitForFunction(() => {
      if (!window.cwDebug || (window.cwStatus && window.cwStatus.ok === false)) return true;
      const f = window.cwDebug.fields(), l = f && f['cw.loading'];
      return !l || !l[1] || l[0] >= l[1];
    }, null, { timeout: 120000, polling: 250 });
    await sleep(300);
    const st = await page.evaluate(() => ({ ...window.cwStatus, coi: window.crossOriginIsolated }));
    if (!st.ok || !st.coi) { s.p.kill(); return report('web', g.game, 'FAIL', `page did not run: ${JSON.stringify(st)} ${logs.slice(-5).join(' | ')}`); }
    step('web 7');
    let field, before, after, how;
    field = 'game.state';
    before = (await page.evaluate((f) => window.cwDebug.command(`fields ${f}`), field))[field];
    await page.keyboard.down('Enter'); await sleep(300); await page.keyboard.up('Enter');
    how = 'keyboard Enter';
    step('web 8');
    for (let i = 0; i < 40; i++) {
      await sleep(500);
      after = (await page.evaluate((f) => window.cwDebug.command(`fields ${f}`), field))[field];
      if (after !== before) break;
    }
    step('web 9');
    const shot = join(OUT, `${g.game}-web.png`);
    step('web 10');
    await page.locator('#screen').screenshot({ path: shot });
    const colors = picture(shot);
    step('web 11');
    s.p.kill();
    // control: the same files without COOP/COEP must refuse to start, explaining why
    step('web 12');
    const c = serve(false);
    step('web 13');
    await sleep(800);
    const page2 = await browser.newPage();
    step('web 14');
    await page2.goto(c.url);
    step('web 15');
    await page2.waitForFunction('window.cwStatus', null, { timeout: 30000 });
    step('web 16');
    const cst = await page2.evaluate(() => ({ ...window.cwStatus, msg: document.getElementById('msg-title').textContent, coi: window.crossOriginIsolated }));
    c.p.kill();
    if (after === before) return report('web', g.game, 'FAIL', `${how} did not change ${field} (${before}) ${logs.slice(-5).join(' | ')}`);
    if (colors < 64) return report('web', g.game, 'FAIL', `canvas has ${colors} colours`);
    if (cst.ok !== false || cst.coi || !/cross-origin isolated/.test(cst.msg)) return report('web', g.game, 'FAIL', `CONTROL PASSED: without COOP/COEP the page ${JSON.stringify(cst)}`);
    report('web', g.game, 'built+run (Chromium)', `${st.frames}+ frames, crossOriginIsolated; ${how} changed ${field} ${before}->${after} (page debug channel); canvas ${colors} colours; control without COOP/COEP shows "${cst.msg}"`);
  } finally {
    step('web 17');
    await browser.close();
  }
}

async function wasc(g) {
  const [cart] = await exportOne(g.project, 'cart', join(OUT, g.game));
  const S = `cw-export-test-${process.pid}`;
  const call = async (tool, a) => {
    const res = await fetch(`http://127.0.0.1:7331/tool/${tool}`, { method: 'POST', headers: { 'Content-Type': 'application/json', 'x-romdev-session': S }, body: JSON.stringify(a) });
    return JSON.parse(await res.text());
  };
  let conf;
  try {
    await call('loadMedia', { platform: 'wasmcart', path: cart, deterministicSeed: 1 });
    conf = await call('wasm', { op: 'conformance' });
    await call('host', { op: 'shutdown' });
  } catch (e) {
    return report('wasc', g.game, 'built-only', `romdev not reachable on 7331 (${e.message})`);
  }
  if (!conf.conforms) return report('wasc', g.game, 'FAIL', `conformance: ${JSON.stringify(conf.issues)}`);
  // control: a truncated cart must not conform (or load)
  const broken = join(OUT, `${g.game}-truncated.wasc`);
  writeFileSync(broken, readFileSync(cart).subarray(0, 4096));
  const lc = await call('loadMedia', { platform: 'wasmcart', path: broken, deterministicSeed: 1 });
  const cc = lc.error || lc.isError ? { conforms: false } : await call('wasm', { op: 'conformance' });
  await call('host', { op: 'shutdown' }).catch(() => {});
  if (cc.conforms) return report('wasc', g.game, 'FAIL', 'CONTROL PASSED: a truncated cart conforms');
  // the input script on romdev, with the gate's expected state
  const st = join(OUT, `${g.game}-romdev.state.json`);
  const r = spawnSync('node', [join(ROOT, 'tools/run-cart.mjs'), '--host', 'romdev', '--app', g.game, '--cart', cart, '--frames', String(g.gate.frames),
    '--input', join(ROOT, 'tests', g.gate.input), '--state', st], { encoding: 'utf8', timeout: 900000 });
  const bad = r.status === 0 ? mismatches(JSON.parse(readFileSync(st, 'utf8')), g.gate.expect) : [r.stderr.slice(-300)];
  if (bad.length) return report('wasc', g.game, 'FAIL', `romdev run: ${bad.join(', ')}`);
  // wcnative: renders (it takes no input)
  const shot = join(OUT, `${g.game}-wcnative.png`);
  const auth = spawnSync('sh', ['-c', 'ls -t /run/user/$(id -u)/.mutter-Xwaylandauth.* 2>/dev/null | head -1'], { encoding: 'utf8' }).stdout.trim();
  const w = spawnSync('node', [join(ROOT, 'tools/run-cart.mjs'), '--host', 'wcnative', '--app', g.game, '--cart', cart, '--frames', '120', '--shot', shot],
    { encoding: 'utf8', timeout: 300000, env: { ...process.env, ...(auth ? { XAUTHORITY: auth } : {}) } });
  const colors = picture(shot);
  if (w.status !== 0 || colors < 64) return report('wasc', g.game, 'FAIL', `wcnative: ${w.stderr.slice(-300)} colours=${colors}`);
  report('wasc', g.game, 'built+run', `romdev conformance ok (${conf.issues.length} warnings), expected state on romdev after the input script, wcnative renders (${colors} colours); control (truncated cart) fails`);
}

async function macos(g) {
  if (process.platform !== 'darwin') return report('macos', g.game, 'not built (needs a Mac)', 'the macOS export builds with Xcode\'s clang and SDK; CI builds it on a macOS runner');
  const files = await exportOne(g.project, 'macos', join(OUT, g.game));
  const dir = files[files.length - 1];
  const app = join(dir, readdirSync(dir).find((f) => f.endsWith('.app')));
  const plist = readFileSync(join(app, 'Contents/Info.plist'), 'utf8');
  const pak = join(app, 'Contents/Resources', `${g.game}.cwpak`);
  const okPlist = /<key>CFBundleExecutable<\/key><string>/.test(plist) && /CFBundleIdentifier/.test(plist);
  const okLibs = ['libEGL.dylib', 'libGLESv2.dylib'].every((d) => existsSync(join(app, 'Contents/Frameworks', d)));
  const sig = spawnSync('codesign', ['--verify', '--deep', app]);
  if (!okPlist || !okLibs || sig.status !== 0 || !existsSync(pak))
    return report('macos', g.game, 'FAIL', `plist=${okPlist} angle=${okLibs} codesign=${sig.status} pak=${existsSync(pak)}`);
  report('macos', g.game, 'built', 'arm64 .app (Info.plist, icon, pack, Dawn + ANGLE dylibs), signed ad hoc');
}
async function ios(g) {
  const files = await exportOne(g.project, 'ios', join(OUT, g.game));
  const dir = files[0];
  const okPlist = /LSRequiresIPhoneOS/.test(readFileSync(join(dir, 'Info.plist'), 'utf8'));
  const sh = spawnSync('sh', ['-n', join(dir, 'build-on-mac.sh')]);
  if (!okPlist || sh.status !== 0 || !existsSync(join(dir, 'Resources', `${g.game}.cwpak`))) return report('ios', g.game, 'FAIL', `plist=${okPlist} script=${sh.status}`);
  report('ios', g.game, 'not built (needs a Mac)', 'Xcode project generator (cmake -G Xcode, cmake/export/ios.cmake) + Info.plist + pack; Apple SDK/Xcode and signing (your Apple account) are not available here');
}

/** a game project outside the tree: CMakeLists.txt + project.cwproj + assets */
async function project() {
  const dir = join(OUT, 'project');
  mkdirSync(dir, { recursive: true });
  writeFileSync(join(dir, 'CMakeLists.txt'), `# a game project built with the engine (-DCW_PROJECT_DIR)
cw_add_app(projgame SOURCES \${CMAKE_SOURCE_DIR}/games/action2d/main.c \${CMAKE_SOURCE_DIR}/games/action2d/actors.c \${CMAKE_SOURCE_DIR}/games/action2d/bot.c ASSETS \${CMAKE_CURRENT_SOURCE_DIR}/assets)
`);
  rmSync(join(dir, 'assets'), { recursive: true, force: true });
  cpSync(join(ROOT, 'games/action2d/assets'), join(dir, 'assets'), { recursive: true });
  writeFileSync(join(dir, 'project.cwproj'), `name = "Project Game"\napp = "projgame"\nexport.linux.platform = "linux-x64"\nexport.linux.appimage = false\n`);
  const g = { game: 'projgame', gate: gates.find((x) => x.name === 'action2d') };
  const [r] = await exportPresets({ project: dir, presets: ['linux'] });
  if (!r.ok) return report('project', 'projgame', 'FAIL', r.error);
  const exe = join(untar(join(dir, r.files.find((f) => f.endsWith('.tar.gz'))), join(OUT, 'project-run')), 'projgame');
  const run = playScript(exe, g, 'projgame');
  const bad = mismatches(run.state, g.gate.expect);
  if (!run.ok || bad.length) return report('project', 'projgame', 'FAIL', `${run.err} ${bad.join(', ')}`);
  // control: a broken asset path must fail the export, naming the directory
  renameSync(join(dir, 'assets'), join(dir, 'assets-moved'));
  let c;
  try { [c] = await exportPresets({ project: dir, presets: ['linux'] }); } finally { renameSync(join(dir, 'assets-moved'), join(dir, 'assets')); }
  if (c.ok || !/asset/.test(c.error)) return report('project', 'projgame', 'FAIL', `CONTROL PASSED: export with a missing asset directory: ${JSON.stringify(c)}`);
  report('project', 'projgame', 'built+run', `CW_PROJECT_DIR build in <project>/.cwbuild, expected state met; control (broken asset path) fails: "${c.error.split('\n')[0]}"`);
}

const RUN = { 'linux-x64': linuxX64, 'linux-arm64': linuxArm64, 'windows-x64': windowsX64, 'android-arm64': androidArm64, web, wasc, macos, ios };
for (const p of want) {
  if (p === 'project') { await project().catch((e) => report('project', 'projgame', 'FAIL', e.message)); continue; }
  if (!RUN[p]) { console.error(`unknown platform ${p} (${ALL.join(' ')})`); process.exit(2); }
  for (const g of GAMES) await RUN[p](g).catch((e) => report(p, g.game, 'FAIL', e.message.split('\n').slice(0, 6).join(' | ')));
}
writeFileSync(join(OUT, 'status.json'), JSON.stringify(status, null, 1) + '\n');
console.log(failures ? `export: ${failures} failures` : `ok   export (${status.length} platform x game results, out/export-test/status.json)`);
process.exit(failures ? 1 : 0);
