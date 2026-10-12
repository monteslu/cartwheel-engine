// WebGL2 draw auditor (run-cart --gl-audit, chromium host). Wraps the
// WebGL2RenderingContext prototype before the cart loads and, at every draw,
// checks what the GPU will read: each attribute the current program uses
// against the buffer behind it (vertex range, instance range, offsets), the
// index range against the element buffer and the vertex buffers, and query
// begin/end pairing. Robust buffer access is supposed to make an out-of-range
// read harmless; a driver that gets it wrong faults the GPU instead, so a
// cart must never issue one. window.__glAudit holds the counts and the first
// distinct findings (with the JS/wasm stack that issued the call).
(() => {
  const P = WebGL2RenderingContext.prototype;
  const A = (window.__glAudit = { draws: 0, findings: 0, kinds: {}, first: [], hash: 0 });
  const seen = new Set();
  const report = (kind, msg) => {
    A.findings++;
    A.kinds[kind] = (A.kinds[kind] || 0) + 1;
    const key = kind + '|' + msg.replace(/\d+/g, '#');
    if (seen.has(key) || A.first.length >= 40) return;
    seen.add(key);
    const st = (new Error().stack || '').split('\n').slice(3, 14).map((s) => s.trim()).join(' < ');
    A.first.push({ kind, msg, draw: A.draws, stack: st });
  };
  const BYTES = { 0x1400: 1, 0x1401: 1, 0x1402: 2, 0x1403: 2, 0x1404: 4, 0x1405: 4, 0x1406: 4, 0x140b: 2, 0x8d61: 2, 0x8368: 4, 0x8d9f: 4 };
  const PACKED = new Set([0x8368, 0x8d9f]); // INT_2_10_10_10_REV, UNSIGNED_INT_2_10_10_10_REV: 4 bytes per vertex
  const bufs = new WeakMap(); // WebGLBuffer -> { size, data: Uint8Array|null, deleted }
  const progs = new WeakMap(); // WebGLProgram -> [{ name, loc }]
  const vaos = new WeakMap();
  const defVao = { attribs: [], element: null };
  const st = new WeakMap(); // per context
  const S = (gl) => {
    let s = st.get(gl);
    if (!s) st.set(gl, (s = { array: null, vao: defVao, prog: null, queries: new Map(), qstate: new WeakMap() }));
    return s;
  };
  const wrap = (name, after) => {
    const orig = P[name];
    if (!orig) return;
    P[name] = function (...a) {
      const r = orig.apply(this, a);
      after.call(this, S(this), a, r);
      return r;
    };
  };
  const wrapBefore = (name, before) => {
    const orig = P[name];
    if (!orig) return;
    P[name] = function (...a) {
      before.call(this, S(this), a);
      return orig.apply(this, a);
    };
  };
  const vaoOf = (v) => {
    if (!v) return defVao;
    let o = vaos.get(v);
    if (!o) vaos.set(v, (o = { attribs: [], element: null }));
    return o;
  };
  const attr = (s, i) => (s.vao.attribs[i] ||= { enabled: false, buf: null, size: 4, type: 0x1406, stride: 0, offset: 0, divisor: 0 });
  const bound = (s, gl, target) => {
    if (target === gl.ARRAY_BUFFER) return s.array;
    if (target === gl.ELEMENT_ARRAY_BUFFER) return s.vao.element;
    return gl.getParameter(target === gl.COPY_READ_BUFFER ? gl.COPY_READ_BUFFER_BINDING : target === gl.COPY_WRITE_BUFFER ? gl.COPY_WRITE_BUFFER_BINDING
      : target === gl.UNIFORM_BUFFER ? gl.UNIFORM_BUFFER_BINDING : target === gl.PIXEL_PACK_BUFFER ? gl.PIXEL_PACK_BUFFER_BINDING
      : target === gl.PIXEL_UNPACK_BUFFER ? gl.PIXEL_UNPACK_BUFFER_BINDING : gl.TRANSFORM_FEEDBACK_BUFFER_BINDING);
  };
  const info = (b) => {
    let i = bufs.get(b);
    if (!i) bufs.set(b, (i = { size: 0, data: null, deleted: false }));
    return i;
  };
  const bytesOf = (src, srcOffset, length) => {
    if (src == null) return null;
    if (typeof src === 'number') return new Uint8Array(src);
    const u8 = ArrayBuffer.isView(src) ? new Uint8Array(src.buffer, src.byteOffset, src.byteLength) : new Uint8Array(src);
    const el = ArrayBuffer.isView(src) && src.BYTES_PER_ELEMENT ? src.BYTES_PER_ELEMENT : 1;
    const off = (srcOffset || 0) * el;
    const len = length ? length * el : u8.byteLength - off;
    return u8.slice(off, off + len);
  };

  wrap('bindBuffer', function (s, [target, b]) {
    if (target === this.ARRAY_BUFFER) s.array = b;
    else if (target === this.ELEMENT_ARRAY_BUFFER) s.vao.element = b;
  });
  wrap('bindVertexArray', function (s, [v]) { s.vao = vaoOf(v); });
  wrap('deleteBuffer', function (s, [b]) {
    if (!b) return;
    info(b).deleted = true;
    if (s.array === b) s.array = null;
    if (s.vao.element === b) s.vao.element = null;
    for (const a of s.vao.attribs) if (a && a.buf === b) a.buf = null;
  });
  wrap('bufferData', function (s, [target, src, usage, srcOffset, length]) {
    const b = bound(s, this, target);
    if (!b) return;
    const i = info(b);
    i.data = bytesOf(src, srcOffset, length);
    i.size = i.data ? i.data.byteLength : 0;
  });
  wrap('bufferSubData', function (s, [target, dst, src, srcOffset, length]) {
    const b = bound(s, this, target);
    if (!b) return;
    const i = info(b);
    const d = bytesOf(src, srcOffset, length);
    if (dst + d.byteLength > i.size) report('subdata-oob', `bufferSubData ${dst}+${d.byteLength} past size ${i.size}`);
    else if (i.data) i.data.set(d, dst);
  });
  wrap('copyBufferSubData', function (s, [rt, wt, ro, wo, size]) {
    const r = bound(s, this, rt), w = bound(s, this, wt);
    if (r && w && info(r).data && info(w).data && ro + size <= info(r).size && wo + size <= info(w).size)
      info(w).data.set(info(r).data.subarray(ro, ro + size), wo);
  });
  wrap('vertexAttribPointer', function (s, [i, size, type, norm, stride, offset]) {
    Object.assign(attr(s, i), { buf: s.array, size, type, stride, offset, integer: false });
    if (!s.array && offset) report('client-array', `vertexAttribPointer(${i}) offset ${offset} with no ARRAY_BUFFER`);
  });
  wrap('vertexAttribIPointer', function (s, [i, size, type, stride, offset]) {
    Object.assign(attr(s, i), { buf: s.array, size, type, stride, offset, integer: true });
  });
  wrap('enableVertexAttribArray', function (s, [i]) { attr(s, i).enabled = true; });
  wrap('disableVertexAttribArray', function (s, [i]) { attr(s, i).enabled = false; });
  wrap('vertexAttribDivisor', function (s, [i, d]) { attr(s, i).divisor = d; });
  wrap('linkProgram', function (s, [p]) {
    const list = [];
    const n = this.getProgramParameter(p, this.ACTIVE_ATTRIBUTES) || 0;
    for (let k = 0; k < n; k++) {
      const a = this.getActiveAttrib(p, k);
      if (!a || a.name.startsWith('gl_')) continue;
      const loc = this.getAttribLocation(p, a.name);
      // mat4 attributes take 4 consecutive locations, mat3 3, mat2 2
      const cols = { 0x8b5c: 4, 0x8b5b: 3, 0x8b5a: 2 }[a.type] || 1;
      for (let c = 0; c < cols * a.size; c++) list.push({ name: a.name, loc: loc + c });
    }
    progs.set(p, list);
  });
  wrap('useProgram', function (s, [p]) { s.prog = p; });

  // queries: same-target nesting and never-begun reads
  wrapBefore('beginQuery', function (s, [target, q]) {
    if (s.queries.get(target)) report('query-nest', `beginQuery(0x${target.toString(16)}) while one is active`);
  });
  wrap('beginQuery', function (s, [target, q]) {
    if (!s.queries.get(target)) { s.queries.set(target, q); s.qstate.set(q, true); }
  });
  wrapBefore('endQuery', function (s, [target]) {
    if (!s.queries.get(target)) report('query-end', `endQuery(0x${target.toString(16)}) with none active`);
    s.queries.delete(target);
  });
  wrapBefore('getQueryParameter', function (s, [q]) {
    if (!s.qstate.get(q)) report('query-read', 'getQueryParameter on a query never begun');
  });

  const checkDraw = function (s, what, first, count, instances, idx) {
    A.draws++;
    // a hash of the draw stream (calls, counts, instances): equal runs issue equal streams
    A.hash = (Math.imul(A.hash ^ (count * 31 + instances * 7 + first + what.length), 0x9e3779b1) >>> 0);
    const p = s.prog;
    if (!p) return report('no-program', `${what} with no program`);
    const list = progs.get(p) || [];
    let maxV = first + count - 1;
    if (idx) {
      const eb = s.vao.element;
      if (!eb) return report('no-element', `${what} with no ELEMENT_ARRAY_BUFFER`);
      const e = info(eb);
      const bpe = BYTES[idx.type];
      if (e.deleted) report('deleted-buffer', `${what} element buffer deleted`);
      if (idx.offset + count * bpe > e.size) report('index-oob', `${what} reads indices ${idx.offset}+${count}x${bpe} past element buffer size ${e.size}`);
      if (e.data) {
        const n = Math.max(0, Math.min(count, Math.floor((e.size - idx.offset) / bpe)));
        const v = bpe === 1 ? new Uint8Array(e.data.buffer, idx.offset, n) : bpe === 2 ? new Uint16Array(e.data.buffer.slice(idx.offset, idx.offset + n * 2)) : new Uint32Array(e.data.buffer.slice(idx.offset, idx.offset + n * 4));
        maxV = 0;
        for (let k = 0; k < v.length; k++) if (v[k] > maxV) maxV = v[k];
      } else maxV = -1;
    }
    for (const { name, loc } of list) {
      const a = s.vao.attribs[loc];
      if (!a || !a.enabled) continue;
      if (!a.buf) { report('attrib-nobuf', `${what}: attribute ${name}@${loc} enabled with no buffer`); continue; }
      const b = info(a.buf);
      if (b.deleted) report('deleted-buffer', `${what}: attribute ${name}@${loc} reads a deleted buffer`);
      const el = PACKED.has(a.type) ? 4 : a.size * (BYTES[a.type] || 4);
      const stride = a.stride || el;
      const last = a.divisor ? Math.floor((Math.max(1, instances) - 1) / a.divisor) : maxV;
      if (last < 0) continue;
      const need = a.offset + last * stride + el;
      if (need > b.size)
        report('attrib-oob', `${what}: attribute ${name}@${loc} ${a.divisor ? `instance ${last}` : `vertex ${last}`} reads byte ${need} of a ${b.size}-byte buffer (offset ${a.offset}, stride ${stride}, divisor ${a.divisor})`);
    }
  };
  // a hash of the data the cart hands the GPU (uniforms, buffer and texture
  // uploads), per frame-independent call order: equal runs upload equal data
  A.dhash = 0;
  const mix = (v) => { A.dhash = Math.imul(A.dhash ^ v, 0x9e3779b1) >>> 0; };
  const mixBytes = (x) => {
    if (x == null || typeof x !== 'object') { mix(typeof x === 'number' ? Math.round(x * 4096) | 0 : 0); return; }
    if (!ArrayBuffer.isView(x) && !(x instanceof ArrayBuffer)) { if (Array.isArray(x)) for (const v of x) mixBytes(v); return; }
    const u = ArrayBuffer.isView(x) ? new Uint8Array(x.buffer, x.byteOffset, x.byteLength) : new Uint8Array(x);
    let h = 0;
    for (let i = 0; i < u.length; i++) h = (Math.imul(h, 31) + u[i]) | 0;
    mix(h);
  };
  A.dcalls = {};
  for (const name of Object.getOwnPropertyNames(P)) {
    if (!/^(uniform[1-4]|uniformMatrix|bufferData|bufferSubData|texImage2D|texSubImage2D|texImage3D|texSubImage3D|compressedTex)/.test(name)) continue;
    const orig = P[name];
    P[name] = function (...a) {
      A.dcalls[name] = (A.dcalls[name] || 0) + 1;
      mix(name.length);
      for (const v of a) if (!(v instanceof WebGLUniformLocation)) mixBytes(v);
      return orig.apply(this, a);
    };
  }
  // CW_GL_AUDIT_ERRORS (window.__glAuditErrors): glGetError after every call
  // (slow: a GPU-process round trip each), so errors past WebGL's console cap show
  if (window.__glAuditErrors) {
    const getError = P.getError;
    for (const name of Object.getOwnPropertyNames(P)) {
      const d = Object.getOwnPropertyDescriptor(P, name);
      if (name === 'getError' || name === 'constructor' || typeof d.value !== 'function') continue;
      const orig = d.value;
      P[name] = function (...a) {
        const r = orig.apply(this, a);
        const e = getError.call(this);
        if (e) report('gl-error', `${name} -> 0x${e.toString(16)}`);
        return r;
      };
    }
  }
  wrapBefore('drawArrays', function (s, [m, first, count]) { checkDraw(s, 'drawArrays', first, count, 1, null); });
  wrapBefore('drawArraysInstanced', function (s, [m, first, count, n]) { checkDraw(s, 'drawArraysInstanced', first, count, n, null); });
  wrapBefore('drawElements', function (s, [m, count, type, offset]) { checkDraw(s, 'drawElements', 0, count, 1, { type, offset }); });
  wrapBefore('drawElementsInstanced', function (s, [m, count, type, offset, n]) { checkDraw(s, 'drawElementsInstanced', 0, count, n, { type, offset }); });
  wrapBefore('drawRangeElements', function (s, [m, start, end, count, type, offset]) { checkDraw(s, 'drawRangeElements', 0, count, 1, { type, offset }); });
})();
