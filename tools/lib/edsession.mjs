// A live session with a native cartwheel app over the debug channel's
// socket transport (CW_REMOTE=tcp:HOST:PORT, src/debug/channel.c): frames of
// [u32 length LE][u32 id LE][text]; replies carry the request's id; id 0 is
// an event from the app. The app runs in real time (headless: SDL offscreen
// video, dummy audio), so background work (builds, exports) progresses while
// a test waits.
//
//   const s = await launchApp(exe, { env, args, cwd });
//   const r = await s.ed('project.new name="My Game" template=empty_2d dir=/x');   // JSON reply
//   await s.request('status');                  // any debug-channel request
//   await s.screenshot('ui', 'shot.png');
//   await s.close();                             // editor.quit, then kill
import { spawn } from 'node:child_process';
import { createServer } from 'node:net';
import { writeFileSync } from 'node:fs';

/** @param {string} exe @param {{env?:Record<string,string>, args?:string[], cwd?:string, log?:(l:string)=>void, timeoutMs?:number}} [o] */
export async function launchApp(exe, o = {}) {
  const server = createServer();
  await new Promise((r) => server.listen(0, '127.0.0.1', r));
  const port = /** @type {import('node:net').AddressInfo} */ (server.address()).port;
  const proc = spawn(exe, o.args || [], {
    cwd: o.cwd,
    env: { ...process.env, SDL_VIDEODRIVER: 'offscreen', SDL_AUDIODRIVER: 'dummy', ...(o.env || {}), CW_REMOTE: `tcp:127.0.0.1:${port}` },
    stdio: ['ignore', 'pipe', 'pipe'],
  });
  let output = '';
  const onOut = (d) => {
    output += d;
    if (o.log) for (const l of String(d).split('\n')) if (l) o.log(l);
  };
  proc.stdout.on('data', onOut);
  proc.stderr.on('data', onOut);
  let exited = null;
  proc.on('exit', (code, sig) => { exited = { code, sig }; });
  const sock = await new Promise((res, rej) => {
    const t = setTimeout(() => rej(new Error(`${exe} never connected (CW_REMOTE): ${output.slice(-2000)}`)), o.timeoutMs || 60000);
    server.once('connection', (s) => { clearTimeout(t); res(s); });
    proc.once('exit', () => { clearTimeout(t); rej(new Error(`${exe} exited before connecting: ${output.slice(-2000)}`)); });
  });
  server.close();
  let buf = Buffer.alloc(0), next = 1;
  const waiting = new Map();
  const events = [];
  sock.on('data', (d) => {
    buf = Buffer.concat([buf, d]);
    while (buf.length >= 8) {
      const len = buf.readUInt32LE(0), id = buf.readUInt32LE(4);
      if (buf.length < 8 + len) break;
      const text = buf.subarray(8, 8 + len).toString('utf8');
      buf = buf.subarray(8 + len);
      if (id === 0) events.push(text);
      else if (waiting.has(id)) { waiting.get(id)(text); waiting.delete(id); }
    }
  });
  /** @param {string} text @param {number} [ms] @returns {Promise<string>} */
  const raw = (text, ms = 30000) => new Promise((res, rej) => {
    const id = next++;
    const payload = Buffer.from(text, 'utf8');
    const head = Buffer.alloc(8);
    head.writeUInt32LE(payload.length, 0);
    head.writeUInt32LE(id, 4);
    const t = setTimeout(() => { waiting.delete(id); rej(new Error(`no reply in ${ms} ms to: ${text.slice(0, 200)}${exited ? ` (app exited ${JSON.stringify(exited)})` : ''}`)); }, ms);
    waiting.set(id, (r) => { clearTimeout(t); res(r); });
    sock.write(Buffer.concat([head, payload]));
  });
  const json = async (text, ms) => {
    const r = await raw(text, ms);
    try { return JSON.parse(r); } catch { throw new Error(`not JSON: ${r.slice(0, 500)}`); }
  };
  return {
    proc, events,
    get output() { return output; },
    get exited() { return exited; },
    request: json,
    /** run an editor command line exactly (ed64: quotes and spaces survive) */
    ed: (line, ms) => json('ed64 ' + Buffer.from(line, 'utf8').toString('base64'), ms),
    /** @param {string} layers @param {string} path */
    async screenshot(layers, path) {
      const s = await json(`screenshot ${layers} 1`);
      if (s.error) throw new Error(s.error);
      const parts = [];
      for (let off = 0; ;) {
        const g = await json(`screenshot_get ${s.id} ${off}`);
        if (g.error) throw new Error(g.error);
        parts.push(Buffer.from(g.base64, 'base64'));
        off += g.bytes;
        if (!g.more) break;
      }
      writeFileSync(path, Buffer.concat(parts));
      return s;
    },
    async close() {
      if (!exited) await raw('ed editor.quit', 5000).catch(() => {});
      for (let i = 0; i < 50 && !exited; i++) await new Promise((r) => setTimeout(r, 100));
      if (!exited) proc.kill('SIGKILL');
      sock.destroy();
    },
  };
}

/** poll fn until it returns a truthy value (or throw after ms) */
export async function until(fn, ms, what) {
  const end = Date.now() + ms;
  for (;;) {
    const v = await fn();
    if (v) return v;
    if (Date.now() > end) throw new Error(`timed out after ${ms} ms waiting for ${what}`);
    await new Promise((r) => setTimeout(r, 250));
  }
}
