#!/usr/bin/env node
// The manifest `controls` of a project's cart (tools/lib/cart-controls.mjs has the rule; cmake/app.cmake
// cw_add_app runs it at configure time and hands the result to the pack as --controls).
//
//   node tools/cart-controls.mjs [--bindings <input.cfg>]... <project.cwproj>...
//                                                           a,b,start... | none | (nothing: leave it out)
//
// Several projects are read as one, in order: cw_add_app passes the packed project (the game's input map)
// then the one beside CMakeLists.txt, so a `controls = ...` key there wins. A --bindings file is a
// bindings file a game loads itself (cw_input_load_bindings_asset: `action = bindings` lines); its lines
// count as the input map's.
import { readFileSync } from 'node:fs';
import { projectControls, bindingsAsInputMap } from './lib/cart-controls.mjs';

const args = process.argv.slice(2), projects = [], bindings = [];
for (let i = 0; i < args.length; i++) {
  if (args[i] === '--bindings') { if (!args[i + 1]) usage(); bindings.push(args[++i]); } else projects.push(args[i]);
}
if (!projects.length) usage();
function usage() {
  console.error('usage: cart-controls.mjs [--bindings <input.cfg>]... <project.cwproj>...');
  process.exit(1);
}
const read = (file) => {
  try { return readFileSync(file, 'utf8') + '\n'; } catch (e) {
    console.error(`cart-controls: ${e.message}`);
    process.exit(1);
  }
};
let text = bindings.map((f) => bindingsAsInputMap(read(f))).join('');
for (const file of projects) text += read(file);
const c = projectControls(text);
if (c) process.stdout.write((c.length ? c.join(',') : 'none') + '\n');
