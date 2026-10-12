// A debug-channel client for a native cartwheel app the TEST launches
// (editor or game): it listens on 127.0.0.1 (loopback only), starts the app
// with CW_REMOTE=tcp:127.0.0.1:<port> and speaks the socket framing of
// cartwheel/debug.h ([u32 len LE][u32 id LE][text]; id 0 = an event).
//   const app = await launch(exe, args, env);  await app.call('ed editor.state');
//   const png = await app.screenshot('all', 1);  app.close();
import { createServer } from 'node:net';
import { spawn } from 'node:child_process';

export async function launch(exe, args = [], env = {}, { timeoutMs = 30000, log } = {}) {
  const server = createServer();
  await new Promise((r) => server.listen(0, '127.0.0.1', r));
  const port = server.address().port;
  const child = spawn(exe, args, { env: { ...process.env, ...env, CW_REMOTE: `tcp:127.0.0.1:${port}` }, stdio: ['ignore', 'pipe', 'pipe'] });
  let out = '';
  const onData = (d) => { out += d; if (log) log(String(d)); if (out.length > 200000) out = out.slice(-100000); };
  child.stdout.on('data', onData);
  child.stderr.on('data', onData);
  const sock = await new Promise((resolve, reject) => {
    const t = setTimeout(() => reject(new Error(`${exe} did not connect in ${timeoutMs} ms: ${out.slice(-2000)}`)), timeoutMs);
    server.once('connection', (s) => { clearTimeout(t); resolve(s); });
    child.once('exit', (code, sig) => reject(new Error(`${exe} exited (${code ?? sig}) before connecting: ${out.slice(-2000)}`)));
  });
  server.close();
  let buf = Buffer.alloc(0), nextId = 1;
  const waiting = new Map();
  let hello;
  sock.on('data', (d) => {
    buf = Buffer.concat([buf, d]);
    while (buf.length >= 8) {
      const len = buf.readUInt32LE(0), id = buf.readUInt32LE(4);
      if (buf.length < 8 + len) break;
      const text = buf.subarray(8, 8 + len).toString('utf8');
      buf = buf.subarray(8 + len);
      if (id === 0) { hello = text; continue; }
      const w = waiting.get(id);
      if (w) { waiting.delete(id); w(text); }
    }
  });
  const exited = new Promise((r) => child.once('exit', (code, sig) => r(code ?? sig)));
  const app = {
    child, port, exited,
    output: () => out,
    hello: () => hello,
    /** one request; resolves with the parsed JSON reply */
    call(text, ms = 60000) {
      const id = nextId++;
      const payload = Buffer.from(text, 'utf8');
      const hdr = Buffer.alloc(8);
      hdr.writeUInt32LE(payload.length, 0);
      hdr.writeUInt32LE(id, 4);
      sock.write(Buffer.concat([hdr, payload]));
      return new Promise((resolve, reject) => {
        const t = setTimeout(() => { waiting.delete(id); reject(new Error(`no reply to "${text.slice(0, 80)}" in ${ms} ms`)); }, ms);
        waiting.set(id, (s) => { clearTimeout(t); try { resolve(JSON.parse(s)); } catch { resolve({ raw: s }); } });
      });
    },
    /** an editor command line through `ed64` (quotes and spaces survive) */
    ed(line, ms) { return app.call('ed64 ' + Buffer.from(line, 'utf8').toString('base64'), ms); },
    /** a PNG of the app's next frame (layers: world, ui, debug, all) */
    async screenshot(layers = 'all', scale = 1) {
      const s = await app.call(`screenshot ${layers} ${scale}`);
      if (s.error) throw new Error(s.error);
      const parts = [];
      let off = 0;
      for (;;) {
        const g = await app.call(`screenshot_get ${s.id} ${off}`);
        if (g.error) throw new Error(g.error);
        const b = Buffer.from(g.base64, 'base64');
        parts.push(b);
        off += b.length;
        if (!g.more) break;
      }
      return Buffer.concat(parts);
    },
    close() {
      sock.destroy();
      setTimeout(() => { try { child.kill('SIGKILL'); } catch { /* gone */ } }, 3000).unref();
      return exited;
    },
  };
  return app;
}
