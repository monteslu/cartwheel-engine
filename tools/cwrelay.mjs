#!/usr/bin/env node
// cwrelay: the room relay cartwheel's cw_net_relay_new transport talks to.
// A WebSocket server (RFC 6455, no dependencies): every URL path is a room;
// the relay gives each member a slot (the lowest free one), tells it its slot
// and who else is there, and forwards datagrams between members. It never
// parses the game's traffic.
//
//   server -> client  [0xFF 'W' slot count]  welcome: you are `slot`
//                     [0xFF 'J' slot]        a member joined
//                     [0xFF 'L' slot]        a member left
//                     [src_slot ...payload]  a datagram from src_slot
//   client -> server  [dst_slot ...payload]  to one member (0xFE = every other member)
//
//   node tools/cwrelay.mjs [--port 47731] [--host 127.0.0.1] [--slots 8] [--verbose]
//   import { startRelay } from './cwrelay.mjs'; const r = await startRelay({ port: 0 }); r.port; await r.close();
import { createServer } from 'node:http';
import { createHash } from 'node:crypto';
import { fileURLToPath } from 'node:url';

const GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11';

function frame(opcode, payload) {
  const n = payload.length;
  const head = n < 126 ? Buffer.from([0x80 | opcode, n])
    : n < 65536 ? Buffer.from([0x80 | opcode, 126, n >> 8, n & 255])
    : (() => { const b = Buffer.alloc(10); b[0] = 0x80 | opcode; b[1] = 127; b.writeBigUInt64BE(BigInt(n), 2); return b; })();
  return Buffer.concat([head, payload]);
}

/** Start a relay. Resolves once listening; `port` is the bound port. */
export function startRelay({ port = 47731, host = '127.0.0.1', slots = 8, verbose = false } = {}) {
  const rooms = new Map(); // path -> Map(slot -> client)
  const log = (...a) => { if (verbose) console.error('cwrelay:', ...a); };
  const stats = { joined: 0, left: 0, forwarded: 0, bytes: 0 };
  const server = createServer((req, res) => { res.statusCode = 426; res.end('cwrelay: WebSocket only\n'); });
  server.on('upgrade', (req, socket) => {
    const key = req.headers['sec-websocket-key'];
    if (!key || (req.headers.upgrade || '').toLowerCase() !== 'websocket') { socket.destroy(); return; }
    const path = (req.url || '/').split('?')[0];
    const room = rooms.get(path) || new Map();
    rooms.set(path, room);
    let slot = 0;
    while (room.has(slot) && slot < slots) slot++;
    if (slot >= slots) { socket.end('HTTP/1.1 503 Room Full\r\n\r\n'); return; }
    const accept = createHash('sha1').update(key + GUID).digest('base64');
    socket.write(`HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ${accept}\r\n\r\n`);
    socket.setNoDelay(true);
    const client = { slot, socket, send: (bytes) => { if (!socket.destroyed) socket.write(frame(2, bytes)); } };
    client.send(Buffer.from([0xff, 0x57, slot, room.size + 1]));
    for (const [s, other] of room) {
      client.send(Buffer.from([0xff, 0x4a, s]));
      other.send(Buffer.from([0xff, 0x4a, slot]));
    }
    room.set(slot, client);
    stats.joined++;
    log(`${path}: slot ${slot} joined (${room.size} in the room)`);

    let buf = Buffer.alloc(0), parts = [];
    const leave = () => {
      if (room.get(slot) !== client) return;
      room.delete(slot);
      stats.left++;
      for (const other of room.values()) other.send(Buffer.from([0xff, 0x4c, slot]));
      if (!room.size) rooms.delete(path);
      log(`${path}: slot ${slot} left`);
    };
    socket.on('close', leave);
    socket.on('error', leave);
    socket.on('data', (chunk) => {
      buf = Buffer.concat([buf, chunk]);
      for (;;) {
        if (buf.length < 2) return;
        const fin = buf[0] & 0x80, op = buf[0] & 15, masked = buf[1] & 0x80;
        let len = buf[1] & 127, at = 2;
        if (len === 126) { if (buf.length < 4) return; len = buf.readUInt16BE(2); at = 4; }
        else if (len === 127) { if (buf.length < 10) return; len = Number(buf.readBigUInt64BE(2)); at = 10; }
        const maskAt = at;
        if (masked) at += 4;
        if (buf.length < at + len) return;
        const payload = Buffer.from(buf.subarray(at, at + len));
        if (masked) for (let i = 0; i < len; i++) payload[i] ^= buf[maskAt + (i & 3)];
        buf = buf.subarray(at + len);
        if (op === 8) { socket.end(frame(8, Buffer.alloc(0))); leave(); return; }
        if (op === 9) { socket.write(frame(10, payload)); continue; }
        if (op === 10) continue;
        parts.push(payload);
        if (!fin) continue;
        const msg = Buffer.concat(parts);
        parts = [];
        if (msg.length < 1) continue;
        const dst = msg[0], out = Buffer.concat([Buffer.from([slot]), msg.subarray(1)]);
        for (const [s, other] of room) {
          if (s === slot || (dst !== 0xfe && dst !== s)) continue;
          other.send(out);
          stats.forwarded++;
          stats.bytes += out.length;
        }
      }
    });
  });
  return new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(port, host, () => {
      const r = {
        port: server.address().port,
        stats,
        rooms,
        close: () => new Promise((res) => {
          for (const room of rooms.values()) for (const c of room.values()) c.socket.destroy();
          server.close(() => res());
        }),
      };
      log(`listening on ws://${host}:${r.port}/<room>`);
      resolve(r);
    });
  });
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
  const argv = process.argv.slice(2);
  const opt = (name, def) => (argv.includes(name) ? argv[argv.indexOf(name) + 1] : def);
  const r = await startRelay({ port: +opt('--port', 47731), host: opt('--host', '127.0.0.1'), slots: +opt('--slots', 8),
    verbose: argv.includes('--verbose') });
  console.log(`cwrelay: ws://${opt('--host', '127.0.0.1')}:${r.port}/<room>`);
}
