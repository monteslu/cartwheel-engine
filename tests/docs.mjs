// M5.3 docs gate: the generated site is whole and works offline.
//   1. tools/gen-api.mjs --check (references current, no new undocumented function)
//   2. tools/gen-docs.mjs --check (every include, region and link resolves, no
//      pasted code in tutorials, every header has a guide, every template a tutorial)
//   3. build the site into out/docs/site and open it from file:// in headless
//      Chromium (tools/lib/chromium.mjs: the machine's pinned GPU): no page
//      errors, search finds a function / a node type / a guide and its result
//      links open, the theme toggle switches and survives a reload, every
//      relative link of a sample of pages points at a file that exists.
//      Screenshots (light, dark, search, phone width) in out/docs/.
//   Controls that MUST fail: --check on a copy of a tutorial with a pasted C
//   block and on a guide with a broken include; a search for nonsense finds
//   nothing.
//
//   node tests/docs.mjs [--no-browser]
import { spawnSync } from 'node:child_process';
import { cpSync, existsSync, mkdirSync, readFileSync, readdirSync, rmSync, writeFileSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { launchChromium } from '../tools/lib/chromium.mjs';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
import { testEnv } from '../tools/lib/wgpu-pin.mjs';
await enterGpuSandbox(); // only the test GPU is visible, no desktop (tools/lib/gpu-sandbox.mjs)
testEnv(process.env); // what it starts renders offscreen and cannot reach the desktop

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out/docs');
const SITE = join(OUT, 'site');
let failed = 0;
const check = (name, ok, detail = '') => {
  console.log(ok ? `ok   ${name}` : `FAIL ${name}${detail ? ': ' + String(detail).slice(0, 1500) : ''}`);
  if (!ok) failed++;
  return ok;
};
const node = (args, opts = {}) => spawnSync('node', args, { encoding: 'utf8', timeout: 300000, ...opts });

rmSync(OUT, { recursive: true, force: true });
mkdirSync(OUT, { recursive: true });

const ga = node([join(ROOT, 'tools/gen-api.mjs'), '--check']);
check('gen-api --check', ga.status === 0, ga.stdout + ga.stderr);
const gc = node([join(ROOT, 'tools/gen-docs.mjs'), '--check']);
check('gen-docs --check', gc.status === 0, gc.stdout + gc.stderr);

// controls: a scratch copy of the docs tree with one defect each must fail --check
function control(name, mutate) {
  const tmp = join(OUT, `control-${name}`);
  mkdirSync(tmp, { recursive: true });
  const files = spawnSync('git', ['-C', ROOT, 'ls-files', '-co', '--exclude-standard'], { encoding: 'utf8' }).stdout.split('\n').filter(Boolean);
  for (const p of files) if (existsSync(join(ROOT, p))) { mkdirSync(dirname(join(tmp, p)), { recursive: true }); cpSync(join(ROOT, p), join(tmp, p)); }
  mutate(tmp);
  const r = node([join(tmp, 'tools/gen-docs.mjs'), '--check']);
  check(`control (${name}) must fail gen-docs --check`, r.status !== 0, r.stdout);
  rmSync(tmp, { recursive: true, force: true });
}
const tut = readdirSync(join(ROOT, 'docs/tutorials')).find((d) => existsSync(join(ROOT, 'docs/tutorials', d, 'tutorial.json')));
if (tut) control('pasted code in a tutorial', (t) => { const f = join(t, 'docs/tutorials', tut, 'index.md'); writeFileSync(f, readFileSync(f, 'utf8') + '\n```c\nint main(void) { return 0; }\n```\n'); });
control('broken include', (t) => { const f = join(t, 'docs/guides/core.md'); writeFileSync(f, readFileSync(f, 'utf8') + '\n@include samples/hello/main.c#no_such_region\n'); });
control('broken link', (t) => { const f = join(t, 'docs/guides/core.md'); writeFileSync(f, readFileSync(f, 'utf8') + '\nSee [this](api:cw_no_such_function).\n'); });

const gb = node([join(ROOT, 'tools/gen-docs.mjs'), '--out', SITE]);
check('gen-docs builds the site', gb.status === 0 && existsSync(join(SITE, 'index.html')), gb.stdout + gb.stderr);

// every relative href/src of every page resolves to a file of the site
{
  const bad = [];
  let pages = 0, links = 0;
  const walk = (d) => readdirSync(d, { withFileTypes: true }).flatMap((e) => e.isDirectory() ? walk(join(d, e.name)) : e.name.endsWith('.html') ? [join(d, e.name)] : []);
  for (const f of walk(SITE)) {
    pages++;
    const html = readFileSync(f, 'utf8');
    for (const m of html.matchAll(/(?:href|src)="([^"#:]+)(#[^"]*)?"/g)) {
      links++;
      const target = resolve(dirname(f), m[1]);
      if (!existsSync(target)) bad.push(`${f.slice(SITE.length + 1)} -> ${m[1]}`);
      else if (m[2] && m[2].length > 1 && target.endsWith('.html')) {
        const id = decodeURIComponent(m[2].slice(1));
        const t = readFileSync(target, 'utf8');
        if (!t.includes(`id="${id}"`)) bad.push(`${f.slice(SITE.length + 1)} -> ${m[1]}${m[2]} (no such anchor)`);
      }
    }
  }
  check(`every relative link of ${pages} pages resolves (${links} links)`, !bad.length, `${bad.length} broken: ` + bad.slice(0, 20).join('; '));
}

if (!process.argv.includes('--no-browser')) {
  const { chromium } = await import(join(ROOT, 'deps/wasmcart/node_modules/playwright/index.mjs'));
  const { browser } = await launchChromium(chromium, OUT);
  try {
    const ctx = await browser.newContext({ viewport: { width: 1360, height: 860 }, colorScheme: 'light' });
    const page = await ctx.newPage();
    const errors = [];
    page.on('pageerror', (e) => errors.push(e.message));
    page.on('console', (m) => { if (m.type() === 'error') errors.push(m.text()); });
    const url = (p) => pathToFileURL(join(SITE, p)).href;
    const sample = ['index.html', 'guides/overview.html', 'guides/physics.html', 'tutorials/index.html', 'api/index.html', 'api/physics2d.html', 'nodes/index.html', 'nodes/CharacterBody2D.html'];
    for (const p of sample) {
      if (!existsSync(join(SITE, p))) { check(`page ${p} exists`, false); continue; }
      await page.goto(url(p));
      const h1 = await page.textContent('h1');
      check(`${p} renders from file:// (h1 "${(h1 || '').trim().slice(0, 50)}")`, !!h1 && !errors.length, errors.join('; '));
    }
    await page.goto(url('index.html'));
    await page.screenshot({ path: join(OUT, 'home-light.png') });
    // search
    const search = async (q) => {
      await page.fill('#q', q);
      await page.waitForTimeout(50);
      return page.$$eval('#results a', (as) => as.map((a) => ({ text: a.textContent, href: a.getAttribute('href') })));
    };
    for (const [q, want] of [['cw_body2d_create', 'cw_body2d_create'], ['CharacterBody2D', 'CharacterBody2D'], ['rollback', null], ['porting godot', null]]) {
      const r = await search(q);
      check(`search "${q}" finds ${want || 'something'} (${r.length} results, first "${(r[0]?.text || '').slice(0, 60)}")`, r.length > 0 && r[0].href && (!want || r[0].text.includes(want)));
    }
    await search('cw_body2d_create');
    await page.screenshot({ path: join(OUT, 'search.png') });
    await Promise.all([page.waitForURL(/physics2d\.html/, { timeout: 5000 }).catch(() => {}), page.keyboard.press('Enter')]);
    check(`Enter on the first result opens it (${page.url().split('/').slice(-2).join('/')})`, /api\/physics2d\.html#cw_body2d_create$/.test(page.url()));
    const none = await search('zzqxv nothing matches this');
    check('control: a nonsense search finds nothing', none.length === 1 && none[0].href === null);
    // theme: toggle, persists across a reload
    await page.goto(url('guides/physics.html'));
    const bg = () => page.evaluate(() => getComputedStyle(document.body).backgroundColor);
    const light = await bg();
    await page.click('#theme');
    const dark = await bg();
    await page.reload();
    const after = await bg();
    check(`theme toggles (${light} -> ${dark}) and survives a reload (${after})`, light !== dark && dark === after);
    await page.screenshot({ path: join(OUT, 'guide-dark.png') });
    // dark by system preference, no stored choice
    const dctx = await browser.newContext({ viewport: { width: 1360, height: 860 }, colorScheme: 'dark' });
    const dp = await dctx.newPage();
    await dp.goto(url('api/physics2d.html'));
    const sysDark = await dp.evaluate(() => getComputedStyle(document.body).backgroundColor);
    check(`prefers-color-scheme: dark gives the dark theme (${sysDark})`, sysDark === dark);
    await dp.screenshot({ path: join(OUT, 'api-dark.png') });
    // phone width: no horizontal page scroll
    const pctx = await browser.newContext({ viewport: { width: 390, height: 844 }, colorScheme: 'light' });
    const pp = await pctx.newPage();
    for (const p of ['index.html', 'guides/physics.html', 'nodes/CharacterBody2D.html', 'api/scene.html']) {
      await pp.goto(url(p));
      const over = await pp.evaluate(() => document.documentElement.scrollWidth - window.innerWidth);
      check(`${p} at phone width has no horizontal scroll (${over}px)`, over <= 1);
    }
    await pp.goto(url('nodes/CharacterBody2D.html'));
    await pp.screenshot({ path: join(OUT, 'phone-node.png') });
    const tutPage = readdirSync(join(SITE, 'tutorials')).find((f) => f !== 'index.html');
    if (tutPage) { await page.goto(url(`tutorials/${tutPage}`)); await page.screenshot({ path: join(OUT, 'tutorial.png'), fullPage: false }); }
  } finally {
    await browser.close();
  }
}
console.log(failed ? `docs: ${failed} check(s) failed` : 'docs: all checks passed');
process.exit(failed ? 1 : 0);
