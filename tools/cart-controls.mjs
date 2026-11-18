#!/usr/bin/env node
// The manifest `controls` of a project's cart (tools/lib/cart-controls.mjs has the rule; cmake/app.cmake
// cw_add_app runs it at configure time and hands the result to the pack as --controls).
//
//   node tools/cart-controls.mjs <project.cwproj>           a,b,start... | none | (nothing: leave it out)
import { readFileSync } from 'node:fs';
import { projectControls } from './lib/cart-controls.mjs';

const [file] = process.argv.slice(2);
if (!file) {
  console.error('usage: cart-controls.mjs <project.cwproj>');
  process.exit(1);
}
let text;
try { text = readFileSync(file, 'utf8'); } catch (e) {
  console.error(`cart-controls: ${e.message}`);
  process.exit(1);
}
const c = projectControls(text);
if (c) process.stdout.write((c.length ? c.join(',') : 'none') + '\n');
