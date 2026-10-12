// The editor's end of cartwheel's debug channel socket (src/debug/channel.c),
// for probing an exported game: listen, launch the game with
// CW_REMOTE=tcp:HOST:PORT, wait for its hello, send requests.
// Frames: [u32 length LE][u32 id LE][payload]; id 0 = an event from the game.
import { createServer } from 'node:net';

/**
 * @param {number} [port] 0 = any free port
 * @returns {Promise<{port: number, hello: (ms: number) => Promise<object>, call: (text: string, ms?: number) => Promise<any>, close: () => void}>}
 */
export async function listenChannel(port = 0) {
  let sock = null, buf = Buffer.alloc(0), nextId = 1;
  const waiting = new Map();
  let helloRes = null, hello = null;
  const server = createServer((s) => {
    if (sock) { s.destroy(); return; }
    sock = s;
    s.on('data', (d) => {
      buf = Buffer.concat([buf, d]);
      while (buf.length >= 8) {
        const len = buf.readUInt32LE(0), id = buf.readUInt32LE(4);
        if (buf.length < 8 + len) break;
        const text = buf.subarray(8, 8 + len).toString('utf8');
        buf = buf.subarray(8 + len);
        let msg;
        try { msg = JSON.parse(text); } catch { msg = text; }
        if (id === 0) { if (msg && msg.event === 'hello') { hello = msg; helloRes && helloRes(msg); } continue; }
        const w = waiting.get(id);
        if (w) { waiting.delete(id); w(msg); }
      }
    });
    s.on('error', () => {});
  });
  await new Promise((r) => server.listen(port, '127.0.0.1', r));
  const timeout = (ms, what) => new Promise((_, rej) => setTimeout(() => rej(new Error(`debug channel: no ${what} in ${ms} ms`)), ms));
  return {
    port: server.address().port,
    hello: (ms = 60000) => (hello ? Promise.resolve(hello) : Promise.race([new Promise((r) => (helloRes = r)), timeout(ms, 'hello (did the game start and connect?)')])),
    call(text, ms = 20000) {
      if (!sock) return Promise.reject(new Error('debug channel: the game is not connected'));
      const id = nextId++, p = Buffer.from(text, 'utf8'), h = Buffer.alloc(8);
      h.writeUInt32LE(p.length, 0);
      h.writeUInt32LE(id, 4);
      sock.write(Buffer.concat([h, p]));
      return Promise.race([new Promise((r) => waiting.set(id, r)), timeout(ms, `reply to "${text}"`)]);
    },
    /** the next frame as PNG bytes (the channel's screenshot + screenshot_get) */
    async screenshot(ms = 60000) {
      const r = await this.call('screenshot', ms);
      if (!r || r.error) throw new Error(`screenshot: ${JSON.stringify(r)}`);
      const parts = [];
      for (let off = 0; ;) {
        const g = await this.call(`screenshot_get ${r.id} ${off}`, ms);
        if (!g || g.error) throw new Error(`screenshot_get: ${JSON.stringify(g)}`);
        parts.push(Buffer.from(g.base64, 'base64'));
        off += g.bytes;
        if (!g.more) break;
      }
      return Buffer.concat(parts);
    },
    close() { if (sock) sock.destroy(); server.close(); },
  };
}
