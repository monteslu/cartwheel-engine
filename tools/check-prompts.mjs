// Fails on text that names a controller button by one maker's label or colour ("A: jump", "(B) back", "press X",
// "LB / RB", "RT: gas", "Cross", "green button"). On another maker's pad those labels point at the wrong button, and on
// a keyboard or a phone they mean nothing. Prompts name the ACTION and let the engine draw the glyph for the device the
// player holds (cartwheel/prompt.h): "{jump}: hop a gap". The word "Start" is allowed: every pad has that button.
//
// It reads the string literals of .c and .h files (comments and log calls left out), the quoted strings of .cwscene
// files and the values of .cwtr catalogs. A line that really means it carries `prompt-ok: <reason>` on it or on the
// line just above (a guide explaining what the makers call their buttons, a key literally labelled X on a keyboard).
// It is a floor, not a proof: it cannot see letters drawn in art or glyphs drawn in code (review those by hand: look
// for functions that draw a letter in a circle), and keyboard hints such as "W / S: gas" belong in the markup too.
//
//   node tools/check-prompts.mjs [dir or file ...]      (default: the current directory)
//   node tools/check-prompts.mjs --list                 every finding, one per line, and exit 0
import { readdirSync, readFileSync, statSync } from 'node:fs';
import { join, relative, resolve } from 'node:path';

const SKIP = new Set(['node_modules', 'deps', 'build', 'out', '.git', 'vendor', 'third_party', 'dist']);
const argv = process.argv.slice(2);
const listOnly = argv.includes('--list');
const roots = argv.filter((a) => !a.startsWith('--'));
const base = process.cwd();
const targets = (roots.length ? roots : ['.']).map((d) => resolve(d));

// what a finding looks like, and what to write instead
const L = '[ABXY]'; // the face-button letters
const RULES = [
  { re: new RegExp(`(?<![\\w{:])${L}\\s*:(?![\\w/])`), why: 'a button letter before a colon ("A: jump")' },
  { re: new RegExp(`\\(\\s*${L}\\s*\\)`), why: 'a button letter in brackets ("(A)")' },
  { re: new RegExp(`\\[\\s*${L}\\s*\\]`), why: 'a button letter in square brackets ("[A]")' },
  { re: new RegExp(`(?<![\\w{])${L}\\s*/\\s*${L}(?![\\w}])`), why: 'two button letters ("A / X")' },
  { re: new RegExp(`(?<![\\w{])${L}(?:[ ,/]+${L}){2,}(?![\\w}])`), why: 'a row of button letters ("A B X Y")' },
  { re: new RegExp(`\\b(?:[Pp]ress(?:es|ed|ing)?|[Hh]old(?:s|ing)?|[Tt]ap(?:s|ping)?|[Pp]ush(?:es)?|[Mm]ash(?:es)?)\\s+(?:the\\s+)?${L}\\b(?!')`), why: 'press, hold or tap a button letter ("press A")' },
  { re: new RegExp(`(?<![\\w{])${L}\\s+or\\b`), why: 'a button letter offered with another ("A or Enter")' },
  { re: new RegExp(`\\b(?:[Tt]he|[Yy]our)\\s+${L}\\s+button\\b`), why: '"the A button"' },
  { re: new RegExp(`(?<![\\w{])[BXY]\\s+button\\b`), why: 'a button letter ("B button")' },
  { re: new RegExp(`(?<![\\w{])${L}\\s+(?:to|joins?|confirms?|selects?|jumps?|fires?|leaves?|backs?)\\b`), why: 'a button letter and what it does ("A to jump")', article: true },
  // a maker's shoulder, trigger and stick names as a label: alone, or before ":" "/" "+" ")" "or" "to", or after "(" "/"
  // or press / hold ("RT: gas", "LB / RB", "(R3)"); "L2 norm" and "RB%u" in a debug line are not prompts
  { re: /(?:^|[(/]\s*|\b(?:[Pp]ress|[Hh]old|[Tt]ap)\s+|\s)(?:LB|RB|LT|RT|L1|L2|L3|R1|R2|R3|ZL|ZR|LS|RS)(?=\s*(?:$|[:/+)]|or\b|to\b|\+))|^(?:LB|RB|LT|RT|L1|L2|L3|R1|R2|R3|ZL|ZR|LS|RS)\b(?!\s+[a-z])/, why: 'a shoulder, trigger or stick by one maker\'s name ("LB", "RT", "L1", "ZR")' },
  { re: /\b(?:Cross|Circle|Square|Triangle)(?:\s+button|\s*:|\s+to\b|\s+or\b|\s*\/)/, why: 'a face button by one maker\'s symbol name ("Cross")' },
  { re: /\b(?:press(?:es)?|hold|tap)\s+(?:the\s+)?(?:Cross|Circle|Square|Triangle)\b/i, why: 'a face button by one maker\'s symbol name ("press Cross")' },
  { re: /\bSELECT\s*[:/]|\b(?:SELECT|Select|Share|Options|View|Minus|Plus|Menu|Home|Guide)\s+button\b/, why: 'the back or menu button by one maker\'s name ("SELECT:", "Share button"); say what it does and let {action} show it' },
  { re: /\b(?:green|red|blue|yellow)\s+(?:button|one)\b|\b(?:press|hold|tap)\s+(?:the\s+)?(?:green|red|blue|yellow)\b/i, why: 'a button by a maker\'s colour ("green button")' },
];

// the article "A" ("A scrapyard bowl", "A button that opens a list"): a capital A starting the text or a sentence,
// followed by a lower-case word, reads as an article for the rules marked `article`
const articleAt = (s, i) => s[i] === 'A' && (i === 0 || /[.!?]\s+$|\n$/.test(s.slice(0, i)));

// the string literals of a line of C, given whether a block comment is open; returns [strings, stillInComment]
function cStrings(line, inComment) {
  const out = [];
  let i = 0, cur = null;
  while (i < line.length) {
    if (inComment) {
      const e = line.indexOf('*/', i);
      if (e < 0) return [out, true];
      inComment = false;
      i = e + 2;
      continue;
    }
    const ch = line[i];
    if (cur !== null) {
      if (ch === '\\') { cur += line.slice(i, i + 2); i += 2; continue; }
      if (ch === '"') { out.push(cur); cur = null; i++; continue; }
      cur += ch;
      i++;
      continue;
    }
    if (ch === '/' && line[i + 1] === '/') break;
    if (ch === '/' && line[i + 1] === '*') { inComment = true; i += 2; continue; }
    if (ch === '\'') { const e = line.indexOf('\'', i + (line[i + 1] === '\\' ? 3 : 2)); i = e < 0 ? line.length : e + 1; continue; }
    if (ch === '"') { cur = ''; i++; continue; }
    i++;
  }
  if (cur !== null) out.push(cur);
  return [out, inComment];
}

// a call whose text goes to the developer, not the player (logs, asserts, the debug channel's usage lines)
const LOG = /\b(?:cw_(?:error|warn|info|log|fatal|debug_command)|CW_(?:ASSERT|FATAL)|fprintf\s*\(\s*stderr|cw_reply_(?:error|printf))\s*\(/;

function scanText(rel, lines, strings) {
  const found = [];
  let inComment = false, inLog = false;
  lines.forEach((line, i) => {
    let strs;
    if (strings === 'c') {
      [strs, inComment] = cStrings(line, inComment);
      if (inLog) { /* a log call over several lines: until its ");" */
        if (/\)\s*;/.test(line)) inLog = false;
        return;
      }
      const lm = LOG.exec(line);
      if (lm) {
        inLog = !/\)\s*;/.test(line.slice(lm.index));
        return;
      }
      if (/^\s*#\s*include/.test(line)) return;
    } else if (strings === 'cwtr') {
      if (/^\s*(#|@)/.test(line) || !line.includes('=')) return;
      strs = [line.slice(line.indexOf('=') + 1)];
    } else {
      strs = [...line.matchAll(/"((?:[^"\\]|\\.)*)"/g)].map((m) => m[1]);
    }
    if (!strs.length) return;
    if (line.includes('prompt-ok') || (i > 0 && lines[i - 1].includes('prompt-ok'))) return;
    for (const s of strs) {
      // markup and placeholders are not text a player reads as written: {jump}, {pad:west}, {n}
      const t = s.replace(/\{\{|\}\}/g, '').replace(/\{[^{}]*\}/g, '•').replace(/\\n/g, '\n').replace(/%[-+ #0]*\d*(?:\.\d+)?[hlLzjt]*[diouxXeEfgGcsp%]/g, '•');
      for (const r of RULES) {
        const m = r.re.exec(t);
        if (!m) continue;
        const at = m.index + (m[0].search(/[ABXY]/) >= 0 ? m[0].search(/[ABXY]/) : 0);
        if (r.article && articleAt(t, at) && /^A\s+[a-z]/.test(t.slice(at))) continue;
        found.push(`${rel}:${i + 1}: ${r.why}: "${s.length > 90 ? s.slice(0, 87) + '...' : s}"`);
        break;
      }
    }
  });
  return found;
}

const bad = [];
function scanFile(p) {
  const kind = /\.(c|h|cc|cpp|inc)$/.test(p) ? 'c' : p.endsWith('.cwtr') ? 'cwtr' : p.endsWith('.cwscene') ? 'scene' : null;
  if (!kind) return;
  bad.push(...scanText(relative(base, p) || p, readFileSync(p, 'utf8').split('\n'), kind));
}
function walk(dir) {
  for (const name of readdirSync(dir).sort()) {
    if (SKIP.has(name)) continue;
    const p = join(dir, name);
    let st;
    try { st = statSync(p); } catch { continue; }
    if (st.isDirectory()) walk(p);
    else scanFile(p);
  }
}
for (const t of targets) {
  let st;
  try { st = statSync(t); } catch { console.error(`check-prompts: ${t}: no such file or directory`); process.exit(2); }
  if (st.isDirectory()) walk(t);
  else scanFile(t);
}
if (listOnly) {
  for (const b of bad) console.log(b);
  process.exit(0);
}
if (bad.length) {
  console.error(`check-prompts: ${bad.length} prompt(s) name a button by one maker's label or colour. Name the action and let the`);
  console.error('engine draw it for the player\'s device: "{jump}: hop a gap" with cw_prompt_text (cartwheel/prompt.h). A line that');
  console.error('really means it carries `prompt-ok: <reason>` on it or on the line above:');
  for (const b of bad) console.error('  ' + b);
  process.exit(1);
}
console.log('check-prompts: ok');
