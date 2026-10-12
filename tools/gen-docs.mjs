// The docs site: a static, offline site generated from this repository.
//
//   node tools/gen-docs.mjs [--out build/docs]   write the site
//   node tools/gen-docs.mjs --check              build in memory and fail on any
//                                                 broken include, region, link, a
//                                                 pasted code listing in a tutorial,
//                                                 a guide missing from docs/site.json,
//                                                 a header no guide mentions, a
//                                                 template no tutorial covers, or the
//                                                 product name written outside
//                                                 docs/site.json
//
// Inputs (docs/AUTHORING.md): docs/site.json, docs/guides/*.md, docs/tutorials/*,
// docs/api.json (headers), docs/nodes.json (reflection),
// editor/templates/*/template.cfg, tests/gates.json. No dependencies: plain
// node, so it runs anywhere the engine builds. The output works from file://
// (the search index is a script, not a fetch) and from any static server.
import { readFileSync, writeFileSync, readdirSync, existsSync, mkdirSync, rmSync, statSync } from 'node:fs';
import { join, resolve, dirname, extname, relative } from 'node:path';
import { fileURLToPath } from 'node:url';
import { parseHeader } from './gen-api.mjs';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const args = process.argv.slice(2);
const CHECK = args.includes('--check');
const OUTDIR = resolve(ROOT, args.includes('--out') ? args[args.indexOf('--out') + 1] : 'build/docs');
const rd = (p) => readFileSync(join(ROOT, p), 'utf8');
const site = JSON.parse(rd('docs/site.json'));
const NAME = site.name;

/** problems found while building; --check fails on any */
const problems = [];
const problem = (where, what) => problems.push(`${where}: ${what}`);

// ── data ─────────────────────────────────────────────────────────────────
const api = JSON.parse(rd('docs/api.json'));
const nodes = JSON.parse(rd('docs/nodes.json'));
// three.c (the 3D API games call directly) is a pinned dependency: its header is
// parsed here for the reference pages, not kept in docs/api.json
const T3INC = process.env.CW_THREE_DIR ? join(process.env.CW_THREE_DIR, 'include') : join(ROOT, 'deps/three.c/include'); // CW_THREE_DIR: a worktree's pin (cmake/deps.cmake)
const three = existsSync(join(T3INC, 'three.h')) ? ['three.h'].map((f) => parseHeader(f, T3INC, 'three.c')) : [];
if (!three.length) problem('deps/three.c/include/three.h', 'missing (scripts/fetch-deps.sh): the three.c reference page cannot be generated');
const allHeaders = { headers: [...api.headers, ...three] };
const hbase = (h) => h.split('/').pop().replace(/\.h$/, '');

/** symbol -> { url (from site root), kind, header } */
const sym = new Map();
for (const h of allHeaders.headers) {
  const page = `api/${hbase(h.header)}.html`;
  sym.set(h.header, { url: page, kind: 'header' });
  sym.set(h.header.split('/').pop(), { url: page, kind: 'header' });
  for (const k of ['functions', 'types', 'macros']) for (const e of h[k]) {
    const n = e.name.replace(/\(.*$/, '');
    if (!sym.has(n)) sym.set(n, { url: `${page}#${n}`, kind: k.slice(0, -1), header: h.header });
  }
}
const nodeTypes = new Map(nodes.types.map((t) => [t.name, t]));

// templates
const templates = readdirSync(join(ROOT, 'editor/templates')).filter((t) => existsSync(join(ROOT, 'editor/templates', t, 'template.cfg'))).map((id) => {
  const cfg = Object.fromEntries(rd(`editor/templates/${id}/template.cfg`).split('\n').filter((l) => l.includes('=')).map((l) => [l.slice(0, l.indexOf('=')).trim(), l.slice(l.indexOf('=') + 1).trim()]));
  return { id, title: cfg.title || id, description: cfg.description || '', order: +cfg.order || 0 };
}).sort((a, b) => a.order - b.order);

// guides and tutorials
const guideFile = (slug) => site.files?.[slug] || `docs/guides/${slug}.md`;
const guideSlugs = site.sections.flatMap((s) => s.guides);
for (const f of existsSync(join(ROOT, 'docs/guides')) ? readdirSync(join(ROOT, 'docs/guides')) : []) {
  if (f.endsWith('.md') && !guideSlugs.includes(f.slice(0, -3))) problem(`docs/guides/${f}`, 'not listed in docs/site.json sections');
}
const TUTDIR = join(ROOT, 'docs/tutorials');
const tutorials = (existsSync(TUTDIR) ? readdirSync(TUTDIR) : []).filter((d) => existsSync(join(TUTDIR, d, 'tutorial.json')) && existsSync(join(TUTDIR, d, 'index.md')))
  .map((slug) => ({ slug, ...JSON.parse(readFileSync(join(TUTDIR, slug, 'tutorial.json'), 'utf8')) })).sort((a, b) => (a.order || 0) - (b.order || 0));
const titleOf = (md) => (md.match(/^#\s+(.+)$/m) || [])[1]?.replace(/\{\{name\}\}/g, NAME) || null;
const guides = new Map();
for (const slug of guideSlugs) {
  const f = guideFile(slug);
  if (!existsSync(join(ROOT, f))) { problem(f, `guide "${slug}" is listed in docs/site.json but the file does not exist`); continue; }
  const md = rd(f);
  guides.set(slug, { slug, file: f, md, title: titleOf(md) || slug });
}

// ── html helpers ─────────────────────────────────────────────────────────
const esc = (s) => String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
const slugify = (s) => s.toLowerCase().replace(/<[^>]+>/g, '').replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '');

/** resolve a link target to a site-root-relative url (or an absolute one); null = broken */
function resolveLink(target, where) {
  const m = target.match(/^(guide|tutorial|api|header|node|repo):(.+)$/);
  if (/^(https?:|mailto:|#)/.test(target)) return target;
  if (!m) {
    // a relative .md link between guides / tutorials
    const mm = target.match(/^(?:\.\.\/)*(?:guides\/|docs\/guides\/)?([\w-]+)\.md(#.*)?$/);
    if (mm && guides.has(mm[1])) return `guides/${mm[1]}.html${mm[2] || ''}`;
    const tm = target.match(/tutorials\/([\w-]+)(?:\/index\.md)?(#.*)?$/);
    if (tm && tutorials.some((t) => t.slug === tm[1])) return `tutorials/${tm[1]}.html${tm[2] || ''}`;
    problem(where, `link "${target}" does not resolve (use guide:, tutorial:, api:, node:, header:, repo:)`);
    return null;
  }
  const [, kind, rest0] = m;
  const [rest, hash] = rest0.split('#');
  const frag = hash ? `#${hash}` : '';
  switch (kind) {
    case 'guide': if (guides.has(rest)) return `guides/${rest}.html${frag}`; break;
    case 'tutorial': if (tutorials.some((t) => t.slug === rest)) return `tutorials/${rest}.html${frag}`; break;
    case 'api': if (sym.has(rest)) return sym.get(rest).url; break;
    case 'header': { const h = sym.get(rest) || sym.get(`cartwheel/${rest}`); if (h?.kind === 'header') return h.url + frag; break; }
    case 'node': if (nodeTypes.has(rest)) return `nodes/${rest}.html${frag}`; break;
    case 'repo': if (existsSync(join(ROOT, rest))) return `repo:${rest}`; break;
  }
  problem(where, `link "${target}" does not resolve`);
  return null;
}

/** link a bare identifier in code if it is a known symbol */
const symLink = (name, rel, cls = '') => {
  const s = sym.get(name) || (nodeTypes.has(name) ? { url: `nodes/${name}.html` } : null);
  return s ? `<a class="sym${cls}" href="${rel}${s.url}">${esc(name)}</a>` : null;
};

// ── syntax highlighting (light: comments, strings, keywords, symbols) ────
const CKW = new Set('auto break case char const continue default do double else enum extern float for goto if inline int long register restrict return short signed sizeof static struct switch typedef union unsigned void volatile while bool true false NULL uint8_t uint16_t uint32_t uint64_t int8_t int16_t int32_t int64_t size_t _Atomic #include #define #ifdef #ifndef #endif #if #else #pragma'.split(' '));
const JSKW = new Set('import export from const let var function return if else for while of in new await async true false null undefined class extends'.split(' '));
function highlight(code, lang, rel) {
  if (!['c', 'h', 'js', 'mjs', 'json', 'sh'].includes(lang)) {
    // scene / project / ini-like: comments and links of node types
    return code.split('\n').map((l) => {
      if (/^\s*[#;]/.test(l)) return `<span class="cm">${esc(l)}</span>`;
      return esc(l).replace(/type=&quot;(\w+)&quot;/g, (all, t) => nodeTypes.has(t) ? `type=&quot;<a class="sym" href="${rel}nodes/${t}.html">${t}</a>&quot;` : all)
        .replace(/^(\s*)(\[[^\]]*\])/, '$1<span class="kw">$2</span>');
    }).join('\n');
  }
  const kw = lang === 'js' || lang === 'mjs' || lang === 'json' ? JSKW : CKW;
  const re = lang === 'sh'
    ? /(#[^\n]*)|("(?:[^"\\\n]|\\.)*"|'[^'\n]*')|([A-Za-z_][\w.-]*)/g
    : /(\/\*[\s\S]*?\*\/|\/\/[^\n]*)|("(?:[^"\\\n]|\\.)*"|'(?:[^'\\\n]|\\.)*'|`[^`]*`)|(#?[A-Za-z_]\w*)|(\b\d[\w.]*\b)/g;
  let out = '', last = 0, mm;
  while ((mm = re.exec(code))) {
    out += esc(code.slice(last, mm.index));
    last = re.lastIndex;
    const [all, com, str, id, num] = mm;
    if (com) out += `<span class="cm">${esc(com)}</span>`;
    else if (str) out += `<span class="st">${esc(str)}</span>`;
    else if (id) out += kw.has(id) ? `<span class="kw">${esc(id)}</span>` : (lang !== 'sh' && symLink(id, rel)) || esc(id);
    else if (num) out += `<span class="nu">${esc(num)}</span>`;
    else out += esc(all);
  }
  return out + esc(code.slice(last));
}
const langOf = (path) => ({ '.c': 'c', '.h': 'c', '.mjs': 'js', '.js': 'js', '.json': 'json', '.sh': 'sh' })[extname(path)] || 'text';

// ── includes ─────────────────────────────────────────────────────────────
/**
 * A .cwscene selector -> [from, to) line range. "ext_scene" = the ext_scene
 * lines; "Node" = that node's section and its whole subtree (Node is a path
 * from the scene root, "Goal/Lamp", or a name that is unique in the file;
 * [animation name=...] and other named sections match by name too);
 * "A..B" = from A's section through the end of B's selection.
 * @param {string[]} lines
 * @param {string} sel
 * @returns {[number, number] | string}
 */
function sceneRange(lines, sel) {
  const attr = (l, k) => (l.match(new RegExp(`\\s${k}="([^"]*)"`)) || [])[1];
  // top-level section heads: a [header] with a name, [connection], [ext_scene], [scene]
  const heads = [];
  lines.forEach((l, i) => {
    if (!/^\[/.test(l)) return;
    const name = attr(l, 'name');
    if (name == null && !/^\[(connection|ext_scene|scene)\b/.test(l)) return; // a subsection ([track ...])
    const parent = attr(l, 'parent');
    const isNode = /^\[node\b/.test(l);
    heads.push({ i, name, isNode, path: isNode && parent ? (parent === '.' ? name : `${parent}/${name}`) : name, ext: /^\[ext_scene\b/.test(l) });
  });
  const endOf = (k) => (k + 1 < heads.length ? heads[k + 1].i : lines.length);
  const trim = (a, b) => { while (b > a && !lines[b - 1].trim()) b--; return [a, b]; };
  if (sel === 'ext_scene') {
    const ks = heads.map((h, k) => (h.ext ? k : -1)).filter((k) => k >= 0);
    return ks.length ? trim(heads[ks[0]].i, heads[ks[ks.length - 1]].i + 1) : 'no [ext_scene] lines';
  }
  const find = (s) => {
    let k = heads.findIndex((h) => h.path === s);
    if (k < 0) {
      const byName = heads.map((h, j) => (h.name === s ? j : -1)).filter((j) => j >= 0);
      if (byName.length > 1) return `"${s}" names ${byName.length} sections: use its path from the scene root`;
      k = byName.length ? byName[0] : -1;
    }
    if (k < 0) return `no section named "${s}"`;
    let e = k; // the subtree: following nodes whose path is under this one
    if (heads[k].isNode) while (e + 1 < heads.length && heads[e + 1].isNode && heads[e + 1].path.startsWith(`${heads[k].path}/`)) e++;
    return [k, e];
  };
  const [a, b = a] = sel.split('..');
  const fa = find(a), fb = find(b);
  if (typeof fa === 'string') return fa;
  if (typeof fb === 'string') return fb;
  if (fb[1] < fa[0]) return `"${b}" comes before "${a}"`;
  return trim(heads[fa[0]].i, endOf(fb[1]));
}

const MARK = /#(end)?region\b/;
/** @returns {{text: string, from: number, to: number} | null} */
function extract(path, region, where) {
  const full = join(ROOT, path);
  if (!existsSync(full) || !statSync(full).isFile()) { problem(where, `@include ${path}: no such file`); return null; }
  const lines = readFileSync(full, 'utf8').replace(/\n$/, '').split('\n');
  let from = 0, to = lines.length;
  if (path.endsWith('.cwscene')) {
    // Scene files are canonical: the editor (cw_scene_save) rewrites them
    // from the tree and keeps no comments, so a region marker would vanish
    // on the next save. Listings select sections by node path instead.
    if (region) {
      const r = sceneRange(lines, region);
      if (typeof r === 'string') { problem(where, `@include ${path}#${region}: ${r}`); return null; }
      [from, to] = r;
    }
  } else if (region) {
    const re = new RegExp(`#region\\s+${region.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')}\\b`);
    const s = lines.findIndex((l) => re.test(l));
    if (s < 0) { problem(where, `@include ${path}#${region}: no "#region ${region}" marker in the file`); return null; }
    let depth = 0, e = -1;
    for (let i = s + 1; i < lines.length; i++) {
      if (/#region\s+\S/.test(lines[i])) depth++;
      else if (/#endregion\b/.test(lines[i])) { if (depth === 0) { e = i; break; } depth--; }
    }
    if (e < 0) { problem(where, `@include ${path}#${region}: region is not closed by #endregion`); return null; }
    from = s + 1; to = e;
  }
  const body = lines.slice(from, to).filter((l) => !MARK.test(l));
  const ind = Math.min(...body.filter((l) => l.trim()).map((l) => l.match(/^\s*/)[0].length));
  const text = body.map((l) => l.slice(Number.isFinite(ind) ? ind : 0)).join('\n').replace(/^\n+|\n+$/g, '');
  if (!text.trim()) problem(where, `@include ${path}${region ? '#' + region : ''}: empty`);
  return { text, from: from + 1, to };
}

// ── markdown ─────────────────────────────────────────────────────────────
function inline(s, ctx) {
  const parts = s.split(/(`+[^`]*?`+)/);
  return parts.map((p, i) => {
    if (i % 2) {
      const code = p.replace(/^`+|`+$/g, '');
      const c = code.trim();
      const link = symLink(c.replace(/\(\)$/, ''), ctx.rel);
      if (link) return `<code>${link}</code>`;
      return `<code>${esc(code)}</code>`;
    }
    let t = esc(p.replace(/\{\{name\}\}/g, NAME));
    t = t.replace(/\[([^\]]+)\]\(([^)\s]+)\)/g, (all, text, target) => {
      const u = resolveLink(target.replace(/&amp;/g, '&'), ctx.where);
      if (!u) return text;
      if (u.startsWith('repo:')) return `<span class="repo" title="file in the repository">${text}</span>`;
      return `<a href="${/^(https?:|mailto:|#)/.test(u) ? u : ctx.rel + u}">${text}</a>`;
    });
    t = t.replace(/\*\*([^*]+)\*\*/g, '<strong>$1</strong>');
    t = t.replace(/(^|[\s(])\*([^*\s][^*]*?)\*(?=[\s).,;:!?]|$)/g, '$1<em>$2</em>');
    return t;
  }).join('');
}

function codeBlock(text, lang, rel, caption) {
  return `<figure class="code">${caption ? `<figcaption>${caption}</figcaption>` : ''}<pre><code class="lang-${esc(lang)}">${highlight(text, lang, rel)}</code></pre></figure>`;
}

/** markdown -> { html, toc } */
function markdown(src, ctx) {
  const lines = src.replace(/\{\{name\}\}/g, NAME).replace(/\r/g, '').split('\n');
  const out = [], toc = [];
  let i = 0;
  const isBlockStart = (l) => /^(#{1,6}\s|```|>|\s*([-*]|\d+\.)\s|\|)/.test(l) || /^@\w+/.test(l);
  while (i < lines.length) {
    const l = lines[i];
    if (!l.trim()) { i++; continue; }
    let m;
    if ((m = l.match(/^```\s*(\w*)/))) {
      const lang = m[1] || 'text', body = [];
      i++;
      while (i < lines.length && !/^```\s*$/.test(lines[i])) body.push(lines[i++]);
      i++;
      if (ctx.tutorial && !['sh', 'text'].includes(lang)) problem(ctx.where, `a \`\`\`${lang} block is pasted code: tutorials take code only through @include`);
      out.push(codeBlock(body.join('\n'), lang, ctx.rel));
      continue;
    }
    if ((m = l.match(/^@include\s+(\S+)\s*$/))) {
      const [path, region] = m[1].split('#');
      const x = extract(path, region, ctx.where);
      ctx.includes.push(m[1]);
      if (x) out.push(codeBlock(x.text, langOf(path), ctx.rel, `<span class="path">${esc(path)}</span>${region ? ` <span class="region">${esc(region)}</span>` : ''} <span class="lines">lines ${x.from}-${x.to}</span>`));
      i++;
      continue;
    }
    if (/^@templates\s*$/.test(l)) {
      ctx.usesTemplates = true;
      out.push(`<table><thead><tr><th>Template</th><th>id</th><th>What it is</th><th>Tutorials</th></tr></thead><tbody>${templates.map((t) => `<tr><td>${esc(t.title)}</td><td><code>${t.id}</code></td><td>${inline(t.description, ctx)}</td><td>${tutorials.filter((x) => x.template === t.id).map((x) => `<a href="${ctx.rel}tutorials/${x.slug}.html">${esc(x.title.replace(/\{\{name\}\}/g, NAME))}</a>`).join(', ') || '-'}</td></tr>`).join('')}</tbody></table>`);
      i++;
      continue;
    }
    if (/^@headers\s*$/.test(l)) {
      out.push(`<table><thead><tr><th>Header</th><th>Functions</th><th>What it covers</th></tr></thead><tbody>${api.headers.map((h) => `<tr><td><a href="${ctx.rel}api/${hbase(h.header)}.html"><code>${h.header}</code></a></td><td>${h.functions.length}</td><td>${esc(firstSentence(h.summary))}</td></tr>`).join('')}</tbody></table>`);
      i++;
      continue;
    }
    if (/^@gates\s*$/.test(l)) {
      const gates = JSON.parse(rd('tests/gates.json'));
      out.push(`<table><thead><tr><th>Gate</th><th>Runs</th><th>Note</th></tr></thead><tbody>${gates.map((g) => `<tr><td><code>${esc(g.name)}</code></td><td>${g.script ? `<code>${esc(g.script)}</code>` : `app <code>${esc(g.app)}</code>, ${g.frames} frames${g.input ? ', input' : ''}`}</td><td>${esc(g.note || '')}</td></tr>`).join('')}</tbody></table>`);
      i++;
      continue;
    }
    if (/^@\w+/.test(l)) { problem(ctx.where, `unknown directive "${l.trim()}"`); i++; continue; }
    if ((m = l.match(/^(#{1,6})\s+(.+?)\s*#*$/))) {
      const level = m[1].length, text = m[2];
      if (level === 1) { i++; continue; } // the page title is rendered by the layout
      const id = slugify(text);
      if (level <= 3) toc.push({ level, id, text: text.replace(/`/g, '') });
      out.push(`<h${level} id="${id}"><a class="anchor" href="#${id}">#</a>${inline(text, ctx)}</h${level}>`);
      i++;
      continue;
    }
    if (l.startsWith('>')) {
      const body = [];
      while (i < lines.length && lines[i].startsWith('>')) body.push(lines[i++].replace(/^>\s?/, ''));
      out.push(`<blockquote>${markdown(body.join('\n'), { ...ctx, nested: true }).html}</blockquote>`);
      continue;
    }
    if (l.startsWith('|') && i + 1 < lines.length && /^\|?\s*:?-+/.test(lines[i + 1])) {
      const row = (r) => r.replace(/^\||\|\s*$/g, '').split(/(?<!\\)\|/).map((c) => c.trim().replace(/\\\|/g, '|'));
      const head = row(l);
      i += 2;
      const rows = [];
      while (i < lines.length && lines[i].startsWith('|')) rows.push(row(lines[i++]));
      out.push(`<div class="table"><table><thead><tr>${head.map((c) => `<th>${inline(c, ctx)}</th>`).join('')}</tr></thead><tbody>${rows.map((r) => `<tr>${r.map((c) => `<td>${inline(c, ctx)}</td>`).join('')}</tr>`).join('')}</tbody></table></div>`);
      continue;
    }
    if (/^\s*([-*]|\d+\.)\s/.test(l)) {
      const block = [];
      while (i < lines.length && (lines[i].trim() ? (/^\s*([-*]|\d+\.)\s/.test(lines[i]) || /^\s+\S/.test(lines[i])) : (i + 1 < lines.length && /^\s+\S|^\s*([-*]|\d+\.)\s/.test(lines[i + 1])))) block.push(lines[i++]);
      out.push(list(block, ctx));
      continue;
    }
    const para = [];
    while (i < lines.length && lines[i].trim() && !(para.length && isBlockStart(lines[i]))) para.push(lines[i++]);
    out.push(`<p>${inline(para.join('\n'), ctx)}</p>`);
  }
  return { html: out.join('\n'), toc };
}

function list(block, ctx) {
  const ind = block[0].match(/^\s*/)[0].length;
  const ordered = /^\s*\d+\./.test(block[0]);
  const items = [];
  for (const l of block) {
    const own = l.match(/^\s*/)[0].length;
    if (own === ind && /^\s*([-*]|\d+\.)\s/.test(l)) items.push([l.replace(/^\s*([-*]|\d+\.)\s+/, '')]);
    else items[items.length - 1].push(l);
  }
  const li = items.map((it) => {
    const head = [], rest = [];
    for (const l of it) (rest.length || (head.length && /^\s*([-*]|\d+\.)\s/.test(l)) ? rest : head).push(l);
    const inner = rest.length ? markdown(rest.map((l) => l.slice(Math.min(ind + 2, l.match(/^\s*/)[0].length))).join('\n'), { ...ctx, nested: true }).html : '';
    return `<li>${inline(head.map((s) => s.trim()).join('\n'), ctx)}${inner}</li>`;
  });
  return ordered ? `<ol>${li.join('')}</ol>` : `<ul>${li.join('')}</ul>`;
}

function firstSentence(s) {
  const t = (s || '').replace(/\s+/g, ' ').replace(/^cartwheel\s+/i, '').trim();
  const m = t.match(/^(.{20,240}?[.:])(\s|$)/);
  return (m ? m[1] : t.slice(0, 200)).replace(/:$/, '.');
}

/** a header doc comment as html: prose paragraphs, indented/aligned blocks as pre, symbols linked */
function docText(s, rel) {
  if (!s) return '';
  const blocks = s.replace(/\r/g, '').split(/\n\s*\n/);
  return blocks.map((b) => {
    const lines = b.split('\n');
    const rule = lines[0].match(/^[─-]{2,}\s*(.+?)\s*[─-]*$/);
    let head = '';
    if (rule) { head = `<h4>${esc(rule[1])}</h4>`; lines.shift(); if (!lines.length) return head; }
    const pre = lines.some((l) => /^\s{2,}\S/.test(l) || /\S {3,}\S/.test(l) || /^\s*[-*]\s/.test(l) || /[{};]\s*$/.test(l));
    const linked = (t) => esc(t).replace(/\b(cwed_\w+|cw_\w+|CW_\w+|t3_\w+)\b/g, (n) => symLink(n, rel) || n)
      .replace(/`([^`]+)`/g, '<code>$1</code>');
    return head + (pre ? `<pre class="doc">${linked(lines.join('\n'))}</pre>` : `<p>${linked(lines.join(' '))}</p>`);
  }).join('');
}

// ── layout ───────────────────────────────────────────────────────────────
const pages = []; // { path, html }
const search = []; // { t: title, u: url, k: kind, d: detail }

function nav(rel, current) {
  const sec = site.sections.map((s) => `<div class="navsec"><div class="navh">${esc(s.title)}</div>${s.guides.filter((g) => guides.has(g)).map((g) => `<a href="${rel}guides/${g}.html"${current === `guides/${g}.html` ? ' class="cur"' : ''}>${esc(guides.get(g).title)}</a>`).join('')}</div>`).join('');
  const tut = `<div class="navsec"><div class="navh">Tutorials</div>${tutorials.map((t) => `<a href="${rel}tutorials/${t.slug}.html"${current === `tutorials/${t.slug}.html` ? ' class="cur"' : ''}>${esc(t.title.replace(/\{\{name\}\}/g, NAME))}</a>`).join('')}</div>`;
  const ref = `<div class="navsec"><div class="navh">Reference</div><a href="${rel}api/index.html"${current?.startsWith('api/') ? ' class="cur"' : ''}>C API</a><a href="${rel}nodes/index.html"${current?.startsWith('nodes/') ? ' class="cur"' : ''}>Node types</a></div>`;
  return tut + sec + ref;
}

function page(path, title, body, { toc = [], lead = '' } = {}) {
  const rel = '../'.repeat(path.split('/').length - 1);
  const tocHtml = toc.length > 2 ? `<nav class="toc"><div class="navh">On this page</div>${toc.map((t) => `<a class="l${t.level}" href="#${t.id}">${esc(t.text)}</a>`).join('')}</nav>` : '';
  const html = `<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>${esc(title)}${title === NAME ? '' : ` - ${esc(NAME)}`}</title>
<link rel="stylesheet" href="${rel}assets/site.css">
<script>try{var t=localStorage.getItem('cw-docs-theme');if(t)document.documentElement.dataset.theme=t}catch(e){}</script>
</head>
<body>
<header class="top">
<button class="menu" aria-label="Menu" onclick="document.body.classList.toggle('navopen')">&#9776;</button>
<a class="brand" href="${rel}index.html">${esc(NAME)} <span>docs</span></a>
<div class="search"><input id="q" type="search" placeholder="Search (press /)" autocomplete="off" aria-label="Search the docs"><div id="results" role="listbox"></div></div>
<button class="theme" id="theme" aria-label="Toggle dark or light theme" title="Dark / light"><svg width="16" height="16" viewBox="0 0 16 16" aria-hidden="true"><circle cx="8" cy="8" r="6.5" fill="none" stroke="currentColor" stroke-width="1.5"/><path d="M8 1.5a6.5 6.5 0 0 1 0 13z" fill="currentColor"/></svg></button>
</header>
<div class="wrap">
<nav class="side">${nav(rel, path)}</nav>
<main class="content${tocHtml ? ' hastoc' : ''}">
<article>
<h1>${esc(title)}</h1>
${lead ? `<p class="lead">${lead}</p>` : ''}
${body}
</article>
${tocHtml}
</main>
</div>
<footer>Generated from the repository by <code>tools/gen-docs.mjs</code>. ${esc(NAME)} is a working name.</footer>
<script>window.CW_ROOT=${JSON.stringify(rel)};</script>
<script src="${rel}search-index.js"></script>
<script src="${rel}assets/site.js"></script>
</body>
</html>
`;
  pages.push({ path, html });
}

// ── guides ───────────────────────────────────────────────────────────────
const mentionedHeaders = new Set();
const allMd = [];
for (const g of guides.values()) {
  const path = `guides/${g.slug}.html`;
  const ctx = { rel: '../', where: g.file, includes: [] };
  const { html, toc } = markdown(g.md, ctx);
  page(path, g.title, html, { toc });
  allMd.push({ file: g.file, md: g.md });
  search.push({ t: g.title, u: path, k: 'guide', s: g.slug.replace(/-/g, ' '), d: firstSentence(g.md.replace(/^#.*$/m, '').replace(/[`*#>|[\]]/g, '')) });
  for (const t of toc) if (t.level === 2) search.push({ t: `${t.text}`, u: `${path}#${t.id}`, k: 'section', d: g.title });
}

// ── tutorials ────────────────────────────────────────────────────────────
for (const t of tutorials) {
  const path = `tutorials/${t.slug}.html`;
  const file = `docs/tutorials/${t.slug}/index.md`;
  const md = rd(file);
  const ctx = { rel: '../', where: file, includes: [], tutorial: true };
  const { html, toc } = markdown(md, ctx);
  if (!ctx.includes.length) problem(file, 'a tutorial with no @include: its code must come from the project');
  const title = (titleOf(md) || t.title).replace(/\{\{name\}\}/g, NAME);
  const tested = `<div class="tested"><strong>Tested.</strong> <code>node tests/tutorials.mjs ${t.slug}</code> builds <span class="repo">docs/tutorials/${t.slug}/${esc(t.project || 'project')}</span> natively and as a cart and runs it on ${esc((t.hosts || ['native', 'romdev']).join(', '))}${t.gate ? ` (driven by <code>${esc(t.gate)}</code>)` : ''}${t.template ? `. It starts from the <code>${esc(t.template)}</code> template` : ''}.</div>`;
  page(path, title, tested + html, { toc, lead: t.summary ? inline(t.summary, ctx) : '' });
  allMd.push({ file, md });
  search.push({ t: title, u: path, k: 'tutorial', d: t.summary || '' });
}
page('tutorials/index.html', 'Tutorials', `<ul class="cards">${tutorials.map((t) => `<li><a href="${t.slug}.html"><strong>${esc(t.title.replace(/\{\{name\}\}/g, NAME))}</strong><span>${esc((t.summary || '').replace(/\{\{name\}\}/g, NAME))}</span></a></li>`).join('')}</ul>`,
  { lead: 'Each tutorial ends in a real project in the repository, and a gate builds and runs it on every host, so what you read is what works.' });

// ── C API reference ──────────────────────────────────────────────────────
function apiPages(data, dir, title, lead, symbolKind) {
  const index = [];
  for (const h of data.headers) {
    const base = hbase(h.header);
    const path = `${dir}/${base}.html`;
    const rel = '../';
    const sect = (name, items, render) => items.length ? `<h2 id="${name.toLowerCase()}">${name}</h2>${items.map(render).join('')}` : '';
    const fn = (f) => `<div class="entry" id="${esc(f.name)}"><pre class="sig"><code>${highlight(f.signature + ';', 'c', rel)}</code></pre>${f.doc ? docText(f.doc, rel) : h.header.startsWith('three.c/') ? '<p class="meta">No comment in three.h: it mirrors the three.js r128 method of the same name (t3_object_add is Object3D.add).</p>' : '<p class="undoc">Undocumented.</p>'}${f.inline ? '<p class="meta">static inline</p>' : ''}</div>`;
    const ty = (t) => `<div class="entry" id="${esc(t.name)}"><h3><code>${esc(t.name)}</code></h3>${docText(t.doc, rel)}<details${t.decl.split('\n').length < 12 ? ' open' : ''}><summary>declaration</summary><pre class="sig"><code>${highlight(t.decl, 'c', rel)}</code></pre></details></div>`;
    const ma = (m) => `<div class="entry" id="${esc(m.name.replace(/\(.*$/, ''))}"><pre class="sig"><code>#define ${esc(m.name)}</code></pre>${docText(m.doc, rel)}</div>`;
    const toc = [{ level: 2, id: 'functions', text: `Functions (${h.functions.length})` }, { level: 2, id: 'types', text: `Types (${h.types.length})` }, { level: 2, id: 'macros', text: `Macros (${h.macros.length})` }].filter((x) => !/\(0\)/.test(x.text));
    const fnIndex = h.functions.length ? `<div class="fnindex">${h.functions.map((f) => `<a href="#${f.name}"><code>${esc(f.name)}</code></a>`).join(' ')}</div>` : '';
    page(path, h.header, `<p class="meta">#include "${esc(h.header)}"</p>${docText(h.summary, rel)}${fnIndex}${sect('Functions', h.functions, fn)}${sect('Types', h.types, ty)}${sect('Macros', h.macros, ma)}`, { toc });
    search.push({ t: h.header, u: path, k: 'header', d: firstSentence(h.summary) });
    for (const f of h.functions) search.push({ t: f.name, u: `${path}#${f.name}`, k: symbolKind, d: firstSentence(f.doc) || f.signature });
    for (const t of h.types) search.push({ t: t.name, u: `${path}#${t.name}`, k: 'type', d: firstSentence(t.doc) || h.header });
    for (const m of h.macros) search.push({ t: m.name.replace(/\(.*$/, ''), u: `${path}#${m.name.replace(/\(.*$/, '')}`, k: 'macro', d: firstSentence(m.doc) || h.header });
    index.push(`<tr><td><a href="${base}.html"><code>${esc(h.header)}</code></a></td><td>${h.functions.length}</td><td>${h.types.length}</td><td>${esc(firstSentence(h.summary))}</td></tr>`);
  }
  page(`${dir}/index.html`, title, `<div class="table"><table><thead><tr><th>Header</th><th>Functions</th><th>Types</th><th>Summary</th></tr></thead><tbody>${index.join('')}</tbody></table></div>`, { lead });
}
apiPages(allHeaders, 'api', 'C API reference', `Generated from the public headers (<code>include/cartwheel/*.h</code>, via <code>docs/api.json</code>), plus <code>three.c/three.h</code>: the three.js r128 API in C that 3D code calls directly (a pinned dependency; see the 3D rendering guide). The comments in the headers are the documentation; <code>node tools/gen-api.mjs --check</code> fails on a new undocumented function.`, 'function');

// ── node reference ───────────────────────────────────────────────────────
const children = new Map();
for (const t of nodes.types) { if (!children.has(t.parent)) children.set(t.parent, []); children.get(t.parent).push(t); }
const chain = (t) => { const c = []; for (let p = nodeTypes.get(t.parent); p; p = nodeTypes.get(p.parent)) c.push(p); return c; };
for (const t of nodes.types) {
  const path = `nodes/${t.name}.html`, rel = '../';
  const up = chain(t);
  const propRow = (p) => `<tr id="${esc(p.name)}"><td><code>${esc(p.name)}</code>${p.readonly ? ' <span class="badge">read-only</span>' : ''}${p.stored === false ? ' <span class="badge">not saved</span>' : ''}</td><td><code>${esc(p.type)}</code>${p.enum ? `<div class="enum">${p.enum.map((e) => `<code>${esc(e)}</code>`).join(' ')}</div>` : ''}</td><td><code>${esc(p.default ?? '')}</code></td><td>${p.min !== undefined || p.max !== undefined ? `${p.min ?? ''} .. ${p.max ?? ''}` : ''}</td><td>${esc(p.doc || '')}</td></tr>`;
  const table = (props) => `<div class="table"><table><thead><tr><th>Property</th><th>Type</th><th>Default</th><th>Range</th><th>Description</th></tr></thead><tbody>${props.map(propRow).join('')}</tbody></table></div>`;
  const inherited = up.filter((p) => p.properties.length || p.signals.length).map((p) => `<details><summary>From <a href="${p.name}.html">${esc(p.name)}</a> (${p.properties.length} properties${p.signals.length ? `, ${p.signals.length} signals` : ''})</summary>${p.properties.length ? table(p.properties) : ''}${p.signals.length ? `<p>Signals: ${p.signals.map((s) => `<code>${esc(s)}</code>`).join(' ')}</p>` : ''}</details>`).join('');
  const subs = (children.get(t.name) || []).map((c) => `<a href="${c.name}.html">${esc(c.name)}</a>`).join(', ');
  const body = `<p class="meta">Inherits: ${up.length ? up.map((p) => `<a href="${p.name}.html">${esc(p.name)}</a>`).join(' &lt; ') : '(root)'}${subs ? `<br>Inherited by: ${subs}` : ''}</p>
<p>${esc(t.doc || '')}</p>
<p class="meta">In C: <code><a class="sym" href="${rel}api/scene.html#cw_node_new">cw_node_new</a>("${esc(t.name)}", "Name")</code>. In a scene file: <code>[node name="Name" parent="." type="${esc(t.name)}"]</code>. Properties are set by name: <a href="${rel}api/scene.html#cw_node_set">cw_node_set</a>, a <code>key = value</code> line in the scene, or <code>set &lt;node&gt; &lt;prop&gt; &lt;value&gt;</code> over the debug channel.</p>
<h2 id="properties">Properties (${t.properties.length})</h2>${t.properties.length ? table(t.properties) : '<p>None of its own.</p>'}
<h2 id="signals">Signals (${t.signals.length})</h2>${t.signals.length ? `<ul>${t.signals.map((s) => `<li id="signal-${esc(s)}"><code>${esc(s)}</code></li>`).join('')}</ul>` : '<p>None of its own.</p>'}
${inherited ? `<h2 id="inherited">Inherited</h2>${inherited}` : ''}`;
  page(path, t.name, body, { toc: [{ level: 2, id: 'properties', text: 'Properties' }, { level: 2, id: 'signals', text: 'Signals' }, ...(inherited ? [{ level: 2, id: 'inherited', text: 'Inherited' }] : [])] });
  search.push({ t: t.name, u: path, k: 'node', d: t.doc || '' });
  for (const p of t.properties) search.push({ t: `${t.name}.${p.name}`, u: `${path}#${p.name}`, k: 'property', d: p.doc || p.type });
}
function tree(parent, rel) {
  const kids = (children.get(parent) || []).sort((a, b) => a.name.localeCompare(b.name));
  return kids.length ? `<ul class="tree">${kids.map((k) => `<li><a href="${k.name}.html">${esc(k.name)}</a> <span class="dim">${esc(firstSentence(k.doc))}</span>${tree(k.name, rel)}</li>`).join('')}</ul>` : '';
}
const roots = nodes.types.filter((t) => !nodeTypes.has(t.parent)).map((t) => t.parent);
page('nodes/index.html', 'Node types', [...new Set(roots)].map((r) => tree(r, '../')).join(''),
  { lead: `Every node type the engine registers, with its properties and signals, generated from the engine's reflection data (<code>docs/nodes.json</code>; a running game answers the same with the <code>types</code> and <code>props</code> debug commands).` });

// ── home ─────────────────────────────────────────────────────────────────
{
  const rel = '';
  const card = (href, title, text) => `<li><a href="${href}"><strong>${esc(title)}</strong><span>${esc(text)}</span></a></li>`;
  const body = `<ul class="cards">
${card('guides/overview.html', 'Overview', 'What the engine is, the stack, and how agents and humans work on a game together.')}
${card('guides/getting-started.html', 'Getting started', 'Build the engine, run a sample on every host, make a project from a template.')}
${card('guides/agent-workflow.html', 'The agent workflow', 'romdev, the debug channel, the editor\'s ed protocol and the agent panel inbox.')}
${card('api/index.html', 'C API reference', `${api.headers.length} headers, ${api.headers.reduce((a, h) => a + h.functions.length, 0)} functions, generated from the headers.`)}
${card('nodes/index.html', 'Node types', `${nodes.types.length} node types with every property and signal, from reflection.`)}
</ul>
<h2 id="tutorials">Tutorials</h2>
<ul class="cards">${tutorials.map((t) => card(`tutorials/${t.slug}.html`, t.title.replace(/\{\{name\}\}/g, NAME), (t.summary || '').replace(/\{\{name\}\}/g, NAME))).join('')}</ul>
${site.sections.map((s) => `<h2 id="${slugify(s.title)}">${esc(s.title)}</h2><ul class="links">${s.guides.filter((g) => guides.has(g)).map((g) => `<li><a href="guides/${g}.html">${esc(guides.get(g).title)}</a></li>`).join('')}</ul>`).join('\n')}`;
  page('index.html', NAME, body, { lead: esc(site.tagline || '') });
}

// ── checks over the whole set ────────────────────────────────────────────
const corpus = allMd.map((x) => x.md).join('\n');
for (const h of api.headers) {
  const base = h.header.split('/').pop();
  if (!corpus.includes(base)) problem('docs/guides', `no guide mentions ${h.header} (link it with (header:${base}) where the module is explained)`);
}
for (const t of templates) {
  const covered = tutorials.some((x) => x.template === t.id) || tutorials.some((x) => rd(`docs/tutorials/${x.slug}/index.md`).includes(t.id));
  if (!covered) problem('docs/tutorials', `no tutorial starts from or uses the template "${t.id}"`);
}
if (!allMd.some((x) => /^@templates\s*$/m.test(x.md))) problem('docs/guides', 'no page lists the templates (@templates)');
for (const { file, md } of allMd) {
  // the product name lives in docs/site.json: prose says {{name}}
  const prose = md.replace(/```[\s\S]*?```/g, '').replace(/`[^`]*`/g, '').replace(/\]\([^)]*\)/g, ']').replace(/^@include.*$/gm, '');
  const hit = prose.match(/^.*\bcartwheel\b(?![/\-_.]\w).*$/im);
  if (hit && !file.endsWith('CONVENTIONS.md') && !file.endsWith('export.md')) problem(file, `writes the product name in prose ("${hit[0].trim().slice(0, 80)}"): write {{name}} (docs/site.json "name")`);
  if (/—/.test(md)) problem(file, 'contains an em-dash');
}

// ── assets ───────────────────────────────────────────────────────────────
const CSS = `:root{--bg:#fbfbfa;--fg:#1d1f21;--dim:#61666b;--line:#e3e4e2;--side:#f3f3f1;--accent:#2a62c9;--code:#f1f2f0;--kw:#8a3fb5;--st:#2d7a3a;--cm:#7a7f85;--nu:#b5541c;--warn:#a5420f;--card:#ffffff}
@media (prefers-color-scheme:dark){:root:not([data-theme=light]){--bg:#17191c;--fg:#e1e3e6;--dim:#9aa0a6;--line:#2c3036;--side:#1d2024;--accent:#7aa7ff;--code:#202328;--kw:#d19bf2;--st:#8fd18f;--cm:#8b9096;--nu:#f0a86c;--warn:#f0a86c;--card:#1d2024}}
:root[data-theme=dark]{--bg:#17191c;--fg:#e1e3e6;--dim:#9aa0a6;--line:#2c3036;--side:#1d2024;--accent:#7aa7ff;--code:#202328;--kw:#d19bf2;--st:#8fd18f;--cm:#8b9096;--nu:#f0a86c;--warn:#f0a86c;--card:#1d2024}
*{box-sizing:border-box}html{-webkit-text-size-adjust:100%}
body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.6 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif}
a{color:var(--accent);text-decoration:none}a:hover{text-decoration:underline}
code,pre{font-family:ui-monospace,"SF Mono",Menlo,Consolas,monospace;font-size:.88em}
:not(pre)>code{background:var(--code);padding:.1em .35em;border-radius:4px;overflow-wrap:anywhere}
header.top{position:sticky;top:0;z-index:5;display:flex;align-items:center;gap:12px;height:54px;padding:0 16px;background:var(--bg);border-bottom:1px solid var(--line)}
.brand{font-weight:700;color:var(--fg);white-space:nowrap}.brand span{font-weight:400;color:var(--dim)}
.theme svg{display:block;margin:auto}.menu,.theme{background:none;border:1px solid var(--line);color:var(--fg);border-radius:6px;height:32px;min-width:32px;cursor:pointer;font-size:16px}
.menu{display:none}
.search{position:relative;flex:1;max-width:520px;margin-left:auto}
.search input{width:100%;height:34px;padding:0 10px;border:1px solid var(--line);border-radius:6px;background:var(--side);color:var(--fg);font-size:15px}
#results{display:none;position:absolute;top:38px;left:0;right:0;max-height:70vh;overflow:auto;background:var(--card);border:1px solid var(--line);border-radius:8px;box-shadow:0 8px 30px rgba(0,0,0,.18)}
#results.open{display:block}#results a{display:block;padding:7px 12px;color:var(--fg);border-bottom:1px solid var(--line)}
#results a.sel,#results a:hover{background:var(--side);text-decoration:none}
#results .k{display:inline-block;min-width:70px;font-size:12px;color:var(--dim)}#results .d{display:block;font-size:13px;color:var(--dim);white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.wrap{display:flex;max-width:1400px;margin:0 auto}
nav.side{position:sticky;top:54px;align-self:flex-start;width:250px;flex:none;height:calc(100vh - 54px);overflow:auto;padding:16px 12px 40px;border-right:1px solid var(--line);background:var(--side)}
.navsec{margin-bottom:16px}.navh{font-size:12px;font-weight:600;text-transform:uppercase;letter-spacing:.04em;color:var(--dim);margin:0 0 4px 8px}
nav.side a{display:block;padding:3px 8px;border-radius:5px;color:var(--fg);font-size:14.5px}nav.side a.cur{background:var(--line);font-weight:600}
main.content{flex:1;min-width:0;display:flex;gap:24px;padding:24px 32px 64px}
article{flex:1;min-width:0;max-width:880px}
nav.toc{position:sticky;top:78px;align-self:flex-start;width:210px;flex:none;font-size:13.5px;max-height:calc(100vh - 100px);overflow:auto}
nav.toc a{display:block;color:var(--dim);padding:2px 0}nav.toc a.l3{padding-left:12px}
h1{font-size:2em;line-height:1.2;margin:.2em 0 .4em;overflow-wrap:anywhere}h2{margin-top:2em;padding-bottom:.2em;border-bottom:1px solid var(--line)}h3{margin-top:1.6em}
h2,h3,h4{position:relative}.anchor{position:absolute;left:-1em;opacity:0;color:var(--dim)}h2:hover .anchor,h3:hover .anchor{opacity:1}
.lead{font-size:1.12em;color:var(--dim)}
figure.code{margin:1em 0;border:1px solid var(--line);border-radius:8px;overflow:hidden}
figcaption{font:13px ui-monospace,Menlo,Consolas,monospace;padding:6px 12px;background:var(--side);border-bottom:1px solid var(--line);color:var(--dim)}
figcaption .path{color:var(--fg)}figcaption .region{color:var(--kw)}figcaption .lines{float:right}
pre{margin:0;padding:12px 14px;overflow:auto;background:var(--code);line-height:1.45}
pre.sig{border-radius:6px;margin:.4em 0}pre.doc{background:none;padding:4px 0;white-space:pre-wrap;font-size:.84em}
.kw{color:var(--kw)}.st{color:var(--st)}.cm{color:var(--cm);font-style:italic}.nu{color:var(--nu)}a.sym{color:inherit;border-bottom:1px dotted var(--dim)}
.table{overflow-x:auto}table{border-collapse:collapse;margin:1em 0;font-size:.93em;width:100%}th,td{border:1px solid var(--line);padding:6px 9px;text-align:left;vertical-align:top}th{background:var(--side)}td code{overflow-wrap:normal;word-break:normal}td:nth-child(2) code,td:nth-child(3) code{white-space:nowrap}
blockquote{margin:1em 0;padding:.4em 1em;border-left:4px solid var(--accent);background:var(--side);border-radius:0 6px 6px 0}
.entry{padding:10px 0 14px;border-bottom:1px solid var(--line)}.entry:target{background:var(--side);outline:6px solid var(--side)}
.undoc{color:var(--warn)}.meta,.dim{color:var(--dim);font-size:.92em}.badge{font-size:11px;border:1px solid var(--line);border-radius:4px;padding:0 4px;color:var(--dim)}
.enum{margin-top:4px;line-height:1.9}.fnindex{columns:2 260px;font-size:.9em;margin:1em 0}.fnindex a{display:block}
ul.cards{list-style:none;padding:0;display:grid;grid-template-columns:repeat(auto-fill,minmax(250px,1fr));gap:12px}
ul.cards a{display:block;height:100%;padding:14px 16px;border:1px solid var(--line);border-radius:10px;background:var(--card);color:var(--fg)}ul.cards a:hover{border-color:var(--accent);text-decoration:none}
ul.cards strong{display:block;margin-bottom:4px}ul.cards span{color:var(--dim);font-size:.93em}
ul.tree{list-style:none;padding-left:18px;border-left:1px solid var(--line)}ul.tree li{margin:2px 0}
.tested{padding:10px 14px;border:1px solid var(--line);border-left:4px solid var(--st);border-radius:6px;background:var(--side);font-size:.93em}
.repo{font-family:ui-monospace,Menlo,Consolas,monospace;font-size:.88em}
details{margin:.6em 0}summary{cursor:pointer;color:var(--dim)}
footer{border-top:1px solid var(--line);padding:16px;color:var(--dim);font-size:13px;text-align:center}
@media (max-width:1100px){nav.toc{display:none}}
@media (max-width:800px){.menu{display:block}nav.side{display:none;position:fixed;top:54px;left:0;bottom:0;z-index:4;width:80%;max-width:300px;height:auto}body.navopen nav.side{display:block}main.content{padding:16px}.brand span{display:none}h1{font-size:1.6em}.anchor{display:none}}
`;
const JS = `(function(){
var root=document.documentElement,btn=document.getElementById('theme');
btn.onclick=function(){var dark=root.dataset.theme?root.dataset.theme==='dark':matchMedia('(prefers-color-scheme: dark)').matches;root.dataset.theme=dark?'light':'dark';try{localStorage.setItem('cw-docs-theme',root.dataset.theme)}catch(e){}};
var q=document.getElementById('q'),box=document.getElementById('results'),idx=window.CW_SEARCH||[],sel=-1,hits=[];
var KW={guide:0,tutorial:0,section:1,header:1,node:1,function:2,'editor function':2,type:3,macro:3,property:4};
function esc(s){return String(s).replace(/[&<>"]/g,function(c){return{'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]})}
function score(e,t,words){var n=e.t.toLowerCase(),s=0;for(var i=0;i<words.length;i++){var w=words[i],p=n.indexOf(w);if(p===0)s+=n===w?100:60;else if(p>0)s+=(n.charAt(p-1)==='_'||n.charAt(p-1)==='.'||n.charAt(p-1)===' ')?40:25;else if(((e.d||'')+' '+(e.s||'')).toLowerCase().indexOf(w)>=0)s+=8;else return 0}return s-(KW[e.k]||0)*3-n.length*0.05}
function render(){var t=q.value.trim().toLowerCase();if(!t){box.className='';return}var words=t.split(/\\s+/);hits=[];for(var i=0;i<idx.length;i++){var s=score(idx[i],t,words);if(s>0)hits.push([s,idx[i]])}hits.sort(function(a,b){return b[0]-a[0]});hits=hits.slice(0,40);sel=hits.length?0:-1;
box.innerHTML=hits.length?hits.map(function(h,i){var e=h[1];return '<a href="'+(window.CW_ROOT||'')+e.u+'"'+(i===sel?' class="sel"':'')+'><span class="k">'+esc(e.k)+'</span>'+esc(e.t)+'<span class="d">'+esc(e.d||'')+'</span></a>'}).join(''):'<a>No results</a>';box.className='open'}
function move(d){var a=box.querySelectorAll('a');if(!a.length)return;if(sel>=0&&a[sel])a[sel].className='';sel=(sel+d+a.length)%a.length;a[sel].className='sel';a[sel].scrollIntoView({block:'nearest'})}
q.addEventListener('input',render);q.addEventListener('focus',render);
q.addEventListener('keydown',function(e){if(e.key==='ArrowDown'){move(1);e.preventDefault()}else if(e.key==='ArrowUp'){move(-1);e.preventDefault()}else if(e.key==='Enter'){var a=box.querySelectorAll('a')[sel];if(a&&a.href)location.href=a.href}else if(e.key==='Escape'){q.blur();box.className=''}});
document.addEventListener('keydown',function(e){if(e.key==='/'&&document.activeElement!==q){e.preventDefault();q.focus()}});
document.addEventListener('click',function(e){if(!e.target.closest('.search'))box.className=''});
})();
`;

// ── write / check ────────────────────────────────────────────────────────
// No .cwscene anywhere may carry region markers (see extract): an editor
// save would drop them and break whatever listing pointed at them later.
{
  const walk = (d) => {
    for (const e of readdirSync(join(ROOT, d), { withFileTypes: true })) {
      const p = d ? `${d}/${e.name}` : e.name;
      if (e.isDirectory()) { if (!/^(\.|build|deps|node_modules|out$)/.test(e.name)) walk(p); } // out/: test scratch copies
      else if (e.name.endsWith(".cwscene")) {
        const ls = readFileSync(join(ROOT, p), "utf8").split("\n");
        const k = ls.findIndex((l) => /^\s*;.*#(end)?region\b/.test(l));
        if (k >= 0) problem(p, `line ${k + 1}: a #region marker in a .cwscene is dropped by an editor save; docs select scene sections by node`);
      }
    }
  };
  walk("");
}
if (CHECK) {
  if (problems.length) {
    console.log(`gen-docs --check: ${problems.length} problem(s)\n  ` + problems.join('\n  '));
    process.exit(1);
  }
  console.log(`gen-docs --check: ok (${pages.length} pages, ${search.length} search entries, ${guides.size} guides, ${tutorials.length} tutorials)`);
  process.exit(0);
}
rmSync(OUTDIR, { recursive: true, force: true });
for (const p of pages) { mkdirSync(dirname(join(OUTDIR, p.path)), { recursive: true }); writeFileSync(join(OUTDIR, p.path), p.html); }
mkdirSync(join(OUTDIR, 'assets'), { recursive: true });
writeFileSync(join(OUTDIR, 'assets/site.css'), CSS);
writeFileSync(join(OUTDIR, 'assets/site.js'), JS);
writeFileSync(join(OUTDIR, 'search-index.js'), `window.CW_SEARCH=${JSON.stringify(search)};\n`);
console.log(`${relative(ROOT, OUTDIR)}/: ${pages.length} pages, ${search.length} search entries${problems.length ? `; ${problems.length} problem(s) (run --check)` : ''}`);
if (problems.length) for (const p of problems.slice(0, 30)) console.log('  ' + p);
