// A survey whose runs failed must not read as "no missing states": a project with a gate whose input is missing stops
// tools/programs.mjs --gates with exit 1, naming the file. Control: the same project with only its good gate passes.
// The project has its own tests/ and --gates-file, so a survey reading the engine's gates or inputs fails this too.
import { spawnSync } from 'node:child_process';
import { mkdirSync, rmSync, symlinkSync, writeFileSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const P = join(ROOT, 'out', 'programs-survey-fails');
rmSync(P, { recursive: true, force: true });
mkdirSync(join(P, 'tests'), { recursive: true });
symlinkSync(join(ROOT, 'build'), join(P, 'build'));
const good = { name: 'good', app: 'hello', frames: 2, input: 'good-input.json' };
writeFileSync(join(P, 'tests', 'good-input.json'), '[]\n');
writeFileSync(join(P, 'tests', 'good.json'), JSON.stringify([good]));
writeFileSync(join(P, 'tests', 'bad.json'), JSON.stringify([good, { ...good, name: 'bad', input: 'no-such-input.json' }]));
const run = (file) => spawnSync(process.execPath, [join(ROOT, 'tools/programs.mjs'), 'hello', '--gates', '--gates-file', file, '--gl-only', '--dry', '--frames', '2'],
  { encoding: 'utf8', timeout: 300000, env: { ...process.env, CW_PROJECT_ROOT: P } });
const bad = run('bad.json'), ok = run('good.json');
let fail = 0;
if (bad.status !== 1) { fail++; console.log(`FAIL a failed gate run exited ${bad.status}`); }
if (!/gate bad \(gl\): run failed: .*no-such-input/.test(bad.stdout + bad.stderr)) { fail++; console.log(`FAIL no reason naming the input:\n${bad.stdout}${bad.stderr}`); }
if (ok.status !== 0) { fail++; console.log(`CONTROL FAILED: the good gate exited ${ok.status}:\n${ok.stdout}${ok.stderr}`); }
rmSync(P, { recursive: true, force: true });
console.log(fail ? `programs-survey-fails: ${fail} failures` : 'ok   programs-survey-fails (a failed gate run stopped the survey, the good gate passed)');
process.exit(fail ? 1 : 0);
