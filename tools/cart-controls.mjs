#!/usr/bin/env node
// The manifest `controls` of a project's cart (tools/lib/cart-controls.mjs has the rule; cmake/app.cmake
// cw_add_app runs it at configure time and hands the result to the pack as --controls).
//
//   node tools/cart-controls.mjs <project.cwproj>...        a,b,start... | none | (nothing: leave it out)
//
// Several projects are read as one, in order: cw_add_app passes the packed project (the game's input map)
// then the one beside CMakeLists.txt, so a `controls = ...` key there wins.
import { readFileSync } from 'node:fs';
import { projectControls } from './lib/cart-controls.mjs';

const files = process.argv.slice(2);
if (!files.length) {
  console.error('usage: cart-controls.mjs <project.cwproj>...');
  process.exit(1);
}
let text = '';
for (const file of files) {
  try { text += readFileSync(file, 'utf8') + '\n'; } catch (e) {
    console.error(`cart-controls: ${e.message}`);
    process.exit(1);
  }
}
const c = projectControls(text);
if (c) process.stdout.write((c.length ? c.join(',') : 'none') + '\n');
