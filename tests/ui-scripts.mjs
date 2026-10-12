// Writes the input scripts of the UI interaction gates (tests/ui-*.json)
// from readable steps, so a change of the sample's layout is a change here,
// not a hand edit of hundreds of frame entries.
//   node tests/ui-scripts.mjs
// Coordinates are ui_interact's fixed rects (samples/ui_interact/main.c);
// popup item positions come from the theme (item height = line height +
// item margins) and are cross-checked by the sample's `probe` debug field.
import { writeFileSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const DIR = dirname(fileURLToPath(import.meta.url));

function script() {
  const out = [];
  let f = 3, mouse = null;
  const at = (e) => { out.push({ frame: f, ...e }); f += 1; };
  const s = {
    wait(n) { f += n; return s; },
    move(x, y) { mouse = { x, y }; at({ mouse }); return s; },
    click(x, y, n = 1) {
      for (let i = 0; i < n; i++) {
        mouse = { x, y };
        at({ mouse: { x, y, buttons: ['left'] } });
        at({ mouse });
      }
      f += 1;
      return s;
    },
    dblclick(x, y) { return s.click(x, y, 2); },
    drag(x0, y0, x1, y1, steps = 6) {
      at({ mouse: { x: x0, y: y0, buttons: ['left'] } });
      for (let i = 1; i <= steps; i++)
        at({ mouse: { x: Math.round(x0 + (x1 - x0) * i / steps), y: Math.round(y0 + (y1 - y0) * i / steps), buttons: ['left'] } });
      mouse = { x: x1, y: y1 };
      at({ mouse });
      f += 1;
      return s;
    },
    wheel(x, y, n) { mouse = { x, y }; at({ mouse, wheel: n }); at({ mouse }); return s; },
    touchDrag(x0, y0, x1, y1, steps = 8) {
      at({ ...(mouse ? { mouse } : {}), touches: [{ id: 1, x: x0, y: y0 }] });
      for (let i = 1; i <= steps; i++)
        at({ ...(mouse ? { mouse } : {}), touches: [{ id: 1, x: Math.round(x0 + (x1 - x0) * i / steps), y: Math.round(y0 + (y1 - y0) * i / steps) }] });
      at({ ...(mouse ? { mouse } : {}) });
      f += 1;
      return s;
    },
    tap(x, y) { at({ ...(mouse ? { mouse } : {}), touches: [{ id: 1, x, y }] }); at({ ...(mouse ? { mouse } : {}) }); f += 1; return s; },
    key(...keys) { at({ ...(mouse ? { mouse } : {}), keys }); at({ ...(mouse ? { mouse } : {}) }); return s; },
    type(text) { at({ ...(mouse ? { mouse } : {}), text }); return s; },
    pad(...buttons) { at({ ...(mouse ? { mouse } : {}), buttons }); at({ ...(mouse ? { mouse } : {}) }); f += 2; return s; },
    get frames() { return f + 2; },
    out,
  };
  return s;
}

// popup geometry from the dark theme: menu panel margin 4, item 26 tall
const ITEM = 26, PM = 4;

function mouseScript(control) {
  const s = script();
  if (!control) s.click(80, 37, 3);              // Click me x3
  else s.click(80, 37, 2);
  s.click(180, 37);                              // CheckBox on
  s.drag(330, 37, 420, 37);                      // slider to 50
  s.click(360, 87);                              // OptionButton opens below (280, 104)
  s.move(320, 104 + PM + ITEM * 3 + 13);         // hover Delta
  s.click(320, 104 + PM + ITEM * 2 + 13);        // pick Gamma
  s.click(480, 87);                              // File
  s.move(500, 104 + PM + ITEM + 13);             // hover Recent: submenu opens to the right
  s.move(560, 104 + PM + ITEM + 13);             // slide right into the submenu's first item
  s.move(620, 104 + PM + ITEM + 13);
  s.click(620, 104 + PM + ITEM * 2 + 13);        // Recent > Two (id 12)
  s.wheel(100, 200, -2);                         // list scrolls 6 rows
  s.click(100, 200);                             // select the row under the pointer
  s.dblclick(100, 230);                          // select + activate the next one
  s.touchDrag(650, 220, 650, 140);               // touch-scroll the button column (no press)
  s.drag(84, 330, 184, 330);                     // split separator (at x 80..88) +100
  s.click(372, 331);                             // collapse Player in the tree
  s.click(400, 306);                             // select Level
  s.click(730, 87);                              // Delete... opens the modal dialog
  s.click(400, 400);                             // outside the dialog: swallowed (modal)
  s.click(445, 271);                             // Cancel
  s.click(130, 398);                             // the RichTextLabel's [url] (meta_clicked)
  s.move(70, 40).wait(45);                       // rest on Click me: its tooltip shows
  return s;
}

const write = (name, s) => writeFileSync(join(DIR, name), JSON.stringify(s.out, null, 0).replace(/\},\{/g, '},\n{') + '\n');
const m = mouseScript(false);
write('ui-mouse.json', m);
write('ui-mouse-control.json', mouseScript(true));
console.log('ui-mouse frames', m.frames);

function keysScript(control) {
  const s = script();
  s.click(100, 87);                              // focus the LineEdit
  s.type('hello world');
  for (let i = 0; i < 5; i++) s.key('lshift', 'left'); // select "world"
  s.type(control ? 'here' : 'there');            // replaces the selection
  s.key(74).type('> ');                          // Home, then insert at the start
  s.key('lctrl', 'a').key('lctrl', 'c');         // copy everything
  s.key(77).key('backspace');                    // End, delete the last character
  s.key('return');                               // text_submitted
  s.key('tab');                                  // focus -> OptionButton (tree order)
  s.key('lshift', 'tab');                        // and back
  s.click(300, 140);                             // into the TextEdit
  s.key(77).key('return').type('int y = 2;');    // a new line
  s.key('return').key('lctrl', 'v');             // paste the LineEdit's copy
  s.key('up').key(74).type('// ');               // comment the line above
  s.click(600, 37).type('42').key('return');     // SpinBox typed value
  s.click(730, 87);                              // dialog
  s.key('right').key('left').key('return');      // OK by keyboard
  s.wait(4);
  return s;
}

function padScript(control) {
  const s = script();
  s.pad('down');                                 // nothing focused: the first focusable (Click me)
  s.pad('south');                                // press it
  s.pad('right');                                // CheckBox
  s.pad('south');
  s.pad('right');                                // Slider
  s.pad('right').pad('right').pad('right');      // +3
  if (!control) s.pad('down');                   // OptionButton (below the slider)
  s.pad('south');                                // open its list
  s.pad('down').pad('down').pad('down');         // hover the third item
  s.pad('south');                                // choose it
  s.pad('left');                                 // LineEdit (empty: left is not a caret move)
  s.pad('down');                                 // ItemList
  s.pad('down').pad('down');                     // select rows 0, 1
  s.pad('right');                                // TextEdit
  s.pad('down').pad('right');                    // the Scroll's buttons
  s.pad('down').pad('down').pad('down').pad('down').pad('down'); // focus walks down, scrolling into view
  s.pad('south');
  s.wait(4);
  return s;
}

const k = keysScript(false);
write('ui-keys.json', k);
write('ui-keys-control.json', keysScript(true));
const p = padScript(false);
write('ui-pad.json', p);
write('ui-pad-control.json', padScript(true));
console.log('ui-keys frames', k.frames, 'ui-pad frames', p.frames);

// ui_hud: window coordinates; the HUD is mapped through a 2x virtual canvas
// and a UI scale of 2 (one UI unit = 4 window pixels)
function hudScript(control) {
  const s = script();
  s.wait(10);
  s.click(1040, 673);                            // Pause (toggle on): the game stops ticking
  s.wait(10);
  s.click(600, 300);                             // empty HUD space: the game's click
  if (!control) s.click(1193, 673);              // Menu
  s.wait(10);
  s.click(1040, 673);                            // Pause off
  s.wait(5);
  return s;
}
const h = hudScript(false);
write('ui-hud.json', h);
write('ui-hud-control.json', hudScript(true));
console.log('ui-hud frames', h.frames);
