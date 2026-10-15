// Virtual game controllers for tests of the real controller path (SDL's joystick
// driver reading Linux input devices): tools/vpad.c through uinput, built on
// first use into out/vpad. Every controller is vendor 0x1209 product 0x7764;
// VPAD_ENV makes SDL see only those, so a pad plugged into the machine never
// joins a run.
//
//   const v = await openVpad(OUT);
//   await v.send('add 0', 'btn 0 south 1');   // each command answers ok or error
//   v.close();                                 // unplugs every controller
import { spawn, spawnSync } from 'node:child_process';
import { accessSync, constants, existsSync, mkdirSync, statSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..', '..');
const SRC = join(ROOT, 'tools', 'vpad.c');

/** SDL environment: only the virtual controllers */
export const VPAD_ENV = { SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT: '0x1209/0x7764' };

/** build out/vpad when missing or older than its source; throws when /dev/uinput cannot be written */
function build(outDir) {
  try { accessSync('/dev/uinput', constants.W_OK); } catch {
    throw new Error('/dev/uinput is not writable: virtual controllers need it (a desktop session grants it; CI sets it up)');
  }
  mkdirSync(outDir, { recursive: true });
  const exe = join(outDir, 'vpad');
  if (existsSync(exe) && statSync(exe).mtimeMs >= statSync(SRC).mtimeMs) return exe;
  const r = spawnSync(process.env.CC || 'cc', ['-O2', '-o', exe, SRC], { encoding: 'utf8' });
  if (r.status !== 0) throw new Error(`vpad did not build: ${r.stderr}`);
  return exe;
}

/** a running vpad: send(...commands) resolves when each has answered, rejecting on an error */
export async function openVpad(outDir) {
  const p = spawn(build(outDir), [], { stdio: ['pipe', 'pipe', 'inherit'] });
  const waiting = [];
  let buf = '';
  p.stdout.on('data', (d) => {
    buf += d;
    let i;
    while ((i = buf.indexOf('\n')) >= 0) {
      const line = buf.slice(0, i);
      buf = buf.slice(i + 1);
      const w = waiting.shift();
      if (w) (line === 'ok' ? w.res() : w.rej(new Error(`vpad ${w.cmd}: ${line}`)));
    }
  });
  const one = (cmd) => new Promise((res, rej) => { waiting.push({ cmd, res, rej }); p.stdin.write(cmd + '\n'); });
  return {
    async send(...cmds) { for (const c of cmds) await one(c); },
    close() { try { p.stdin.end(); } catch { /* gone */ } },
    exited: new Promise((res) => p.on('close', res)),
  };
}
