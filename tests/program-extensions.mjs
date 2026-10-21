// A project extension must compose with engine shadows without modifying src/.
// Uses three.c's mock GPU; no native renderer or graphics device is required.
import assert from 'node:assert/strict';
import {mkdtempSync,writeFileSync,readFileSync,rmSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {resolve,join} from 'node:path';
import {spawnSync} from 'node:child_process';
const root=resolve(import.meta.dirname,'..'),scratch=mkdtempSync(join(tmpdir(),'cw-project-extension-'));
const tool=join(root,'tools/programs.mjs'),manifest=join(scratch,'probe.txt'),extension=join(scratch,'releaseprobe.mjs');
function run(...args){return spawnSync(process.execPath,[tool,'--table',manifest,'--build',join(scratch,'build'),...args],{cwd:root,encoding:'utf8',timeout:120000});}
try {
 writeFileSync(manifest,'standard+l0001+xreleaseprobe_csm\n');
 writeFileSync(extension,"export const name='releaseprobe'; export async function apply({TSL,mat}) { mat.colorNode=TSL.materialColor.mul(.9); }\n");
 const result=run('--extend',extension);assert.equal(result.status,0,result.stdout+result.stderr);
 for(const backend of ['gl','wgpu']){const generated=readFileSync(join(scratch,`probe_${backend}.c`),'utf8');assert(generated.includes('xreleaseprobe_csm'));assert(generated.includes('ext.csmMap'),'engine shadow bindings survive project composition');assert(generated.includes('material.color'),'project material bindings survive composition');}
 const composed=readFileSync(join(scratch,'build/programs-ext/releaseprobe_csm.mjs'),'utf8');assert(composed.includes('releaseprobe.mjs'));assert(composed.includes('/src/render/extensions/csm.mjs'));
 const missing=run('--extend',join(scratch,'missing.mjs'));assert.notEqual(missing.status,0);assert.match(missing.stderr,/extension module not found/);
 const duplicate=join(scratch,'csm.mjs');writeFileSync(duplicate,"export const name='csm'; export async function apply() {}\n");const collision=run('--extend',duplicate);assert.notEqual(collision.status,0);assert.match(collision.stderr,/duplicate extension csm/);
 console.log('PASS project shader extension composes with engine shadows on GL/WebGPU; missing modules and name collisions rejected');
} finally {rmSync(scratch,{recursive:true,force:true});}
