# Story, voice and cinematics

Samples: `story_scene`, `speech_face`, `director_shots`. Gates: `unit-story`, `unit-director`, `unit-director-shake`, `story-scene`, `story-skip`, `story-barks`, `speech-face`, `director-shots`, `director-shake`.

Three modules a game with characters can use together or alone:

- **Story** ([cartwheel/story.h](header:story.h)): scenes of spoken lines, choices on a pad, flags and stats that change what comes next, barks and banter over gameplay, subtitles, skip and save points. Written as a text script, compiled ahead of time.
- **Speech** ([cartwheel/speech.h](header:speech.h)): every line voiced ahead of time by a text-to-speech model run from node, packed into one file per game with each line's mouth movement; faces that speak them on the character's glTF morph targets, with emotions and blinks.
- **Director** ([cartwheel/director.h](header:director.h)): a cinematic camera. Shots named by type and subjects (push in, react, insert, hero, whip, top and the usual coverage), solved against the subjects every frame so a close-up never ends on the back of a head, a shoulder or somebody walking through the lens. It never shakes.

Everything runs on the fixed tick from the input snapshot and the pack's numbers, never the mixer or the wall clock: a scene plays the same on every host and with the sound off, so gates compare it.

## The script

A `.story` file is lines of words; a `"quoted string"` is one word, `#` starts a comment. At the top level it declares the cast, stats and flags, then scenes and pools.

```text
cast keeper "The Keeper" voice=bm_george speed=0.95 color=#ffd27a
cast traveller "The Traveller" voice=af_heart color=#9fe3ff
stat trust = 0
flag gave_name gate_open

scene gate save
  shot two keeper traveller
  say keeper calm "Halt. Nobody crosses the bridge after dark."
  say traveller "It is barely dusk."
  shot react keeper traveller
  say keeper anger:0.6 "Then you have a little time to tell me who you are."
  choice "Answer the keeper" timeout=12 default=1
    option "I am a courier from the coast."
      add trust 2
      set gave_name
    option "That is my business."
      add trust -1
    option "Say nothing." if not flag gave_name
  end
  if flag gave_name and stat trust >= 2
    say keeper joy "Go on, then."
    set gate_open
    emit open_gate 1
  elif stat trust < 0
    shot push keeper dur=2.5
    say keeper anger "Then your business waits here until morning."
  else
    say keeper sorrow "Sit by the fire until you find your tongue."
  end
  call farewell
end

pool keeper.idle cooldown=4
  line keeper "The river is loud tonight."
  line keeper "Mind the third plank."
end
```

| Word | What it does |
|---|---|
| `cast <id> "Name" voice= speed= color=` | a speaker: `voice` is the text-to-speech voice (Kokoro voice names: `af_heart`, `am_michael`, `bf_emma`, `bm_george`, ...), `speed` its pace, `color` the name's colour in subtitles |
| `stat <name> [= n]`, `flag <names...>` | whole-number stats and on/off flags |
| `scene <name> [save]` ... `end` | a scene; `save` makes its start a save point |
| `say <who> [emotion[:amount]] "text" [shot=<type>] [to=<who>]` | a line; it waits until it ends (its voice, or a reading time) or the player advances. Emotions: calm, joy, anger, sorrow, fear, surprise |
| `aside <who> [emotion] "text"` | a line said over what follows: it does not wait |
| `choice "prompt" [timeout=s] [default=n]` with `option "text" [if ...]` ... `end` | a menu of up to six options; an option with `if` is shown greyed out when its condition fails; on a timeout the default (counted from 1) is taken |
| `if <cond>` / `elif <cond>` / `else` / `end` | branches. A condition is atoms joined by `and`, groups of them by `or` (`and` binds tighter): `flag x`, `not flag x`, `stat x >= n` (`>= <= > < == !=`), `seen scene`, `not seen scene` |
| `set` / `clear <flag>`, `add <stat> n`, `setstat <stat> n` | change the state |
| `wait <seconds>` | a pause (advance ends it) |
| `shot <type> <who...> [dur= dist= fov= height= part=]` | a camera shot: `on_shot` gets it, the director plays it (below) |
| `emit <name> [n]` | the game's own event: `on_emit` gets it (open a gate, start a fight, give an item) |
| `goto <scene>`, `call <scene>`, `save` | jump, call and return, a save point here |
| `pool <name> [cooldown=s] [once] [priority=n]` with `line <who> [emotion] "text" [if ...]` ... `end` | lines the game asks for: barks, banter, reactive lines |

Compile it:

```sh
node tools/story-compile.mjs story/main.story -o assets/main.cwstory --lines out/lines.json
node tools/story-compile.mjs story/main.story -o assets/main.cwstory --check   # CI: the committed file is current
```

`--c src/story_data.c` writes the same bytes as a C array to link instead (`cw_story_desc.data`). Errors name the file and line: an unknown cast member, flag, stat or scene, an option outside a choice, a choice with no options, an unclosed block.

## Playing it

```c
cw_story *st = cw_story_new(&(cw_story_desc){ .path = "main.cwstory", .speech = sp,
                                              .on_shot = shot, .on_emit = event, .on_scene_end = ended });
cw_story_play(st, "gate");                       /* when the scene should start */
/* fixed_update */
cw_story_tick(st, dt);
/* render, inside cw_r2d_begin / cw_r2d_end */
cw_story_draw(st, cw_ui_font_sans(), (cw_rect){ 0 });   /* the whole canvas */
```

Play is pad only, through the shell's menu actions (`ui_accept`, `ui_back`, `ui_up`, `ui_down`; `cw_story_desc` names others): accept ends a line early and picks the option under the cursor, up and down move it, holding back for 0.8 s skips the rest of the scene. The skip hint draws in the corner as a prompt (`{ui_back}: hold to skip`), never a button name. A skipped scene still runs every flag, stat, emit and shot beat on the way and takes each choice's default, so the story is in the state a player who chose the defaults would leave it in. A line ends by itself after its voice plus a short pause, so a scene plays through with no input; `wait_for_advance` holds each line until accept instead.

Gameplay keeps running under a scene unless the game stops it; most games freeze their world while `cw_story_active` and let barks play over play.

### Barks, banter and reactive lines

`cw_story_bark(st, "thorn.phase2")` picks a line from that pool whose condition holds, never the line the pool said last, preferring the one said longest ago, and plays it as an aside: a small subtitle at the top and the voice, no camera, no input. `cooldown=` holds the pool after it speaks; `once` says each line at most once in the whole story (and it is saved), which is what reactive lines want:

```text
pool keeper.rest once
  line keeper "You fell to the Tidecaller three times. The tide does not tire." if stat deaths_tide >= 3
  line keeper "The marsh took you. It takes everyone once." if stat deaths_tide >= 1
end
```

A bark never interrupts a scene's line (it is dropped, counted in `story.asides_dropped`); a bark from a pool with a higher `priority` replaces a lower aside.

### Saves

`cw_story_save` writes the flags, stats, once-lines said, scenes seen and the last save point into a buffer, as named entries: adding or reordering flags never loses a save, renaming one loses only that flag. Keep it in the game's save (`cartwheel/save.h`); `cw_story_load` puts it back and `cw_story_resume_scene` names the scene to play to resume.

## Voices

`tools/speech-bake.mjs` voices every line the compiler listed and packs them:

```sh
npm install --prefix ~/tools/kokoro kokoro-js@1.2.1     # once; the model downloads on the first run
node tools/speech-bake.mjs out/lines.json -o assets/main.cwspeech --kokoro ~/tools/kokoro/node_modules --asr
```

The voices are Kokoro-82M (Apache-2.0) run by kokoro-js on the CPU: no Python, no GPU, about a second a line. Takes are cached by voice, speed and text, so a rerun speaks only what changed. `--asr` transcribes each take back with Whisper base.en and lists any line not heard as written (a mispronounced name shows here; spell it the way it should sound). `--tone` voices lines with a synthetic stand-in, for a build machine without the model.

Each line is stored as 4-bit ADPCM (about 12 KB a second) with its mouth: how open (from the loudness against the line's loud parts, eased the way a jaw moves) and its shape (round to wide, from how bright the sound is), 100 times a second. Credit the model and the voices in the game's credits.

```c
cw_speech *sp = cw_speech_load("main.cwspeech");
cw_speech_tick(sp, dt);                       /* every tick: the music duck and the Voices volume */
cw_music_set_volume(music, cw_speech_duck(sp)); /* music dips under speech */
```

`cw_speech_settings(rows)` gives two Settings rows, Voices (volume) and Subtitles (on, off); put them in `cw_shell_desc.settings_items` and the shell saves them. A story given the pack plays each line's voice itself on the `speech` bus.

## Faces

```c
cw_face *face = cw_face_new(model_root, NULL);   /* the morph targets found by name */
cw_speech_drive_faces(st, sp, faces, cast_count, dt, true); /* every tick */
```

`cw_face` drives a glTF character's morph targets ([cartwheel/morph.h](header:morph.h)) from a mouth and an emotion: the jaw opens with the sound, rounds or widens by its shape; joy smiles, anger lowers the brows and sneers, sorrow pulls the inner brows up and the corners down, fear and surprise lift the brows and widen the eyes; blinks every one to four and a half seconds, each face on its own seed. The mouth eases fast, the emotion slowly. Targets are found by the usual names (ARKit style `jawOpen`, `mouthSmile_L`/`_R`, `browDown_L`/`_R`, `eyeBlink_L`/`_R`; VRM `aa`, `oh`, `happy`, `angry`; plain `smile`, `frown`, `browUp`, `blink`); `cw_face_map` names a model's own. A channel the model lacks lends its weight to a near one (inner brows to brows up, a sneer to a frown), so a face with only a jaw, a smile and brows still shows every emotion. `cw_speech_drive_faces` gives the speaking cast member the line's mouth and emotion and eases everyone else back to calm.

A character made in Blender needs shape keys on its head mesh with those names (export with shape keys on); the jaw key should drop the chin and lower lip and open the mouth's inside.

## The director

```c
cw_director *d = cw_director_new(&(cw_director_desc){ .aspect = 16.f / 9, .ray = world_ray, .user = world });
/* every frame, each person who may be framed or get in the way */
cw_director_set_subject(d, i, &(cw_dir_subject){ .eye = eyes, .chest = chest, .feet = feet, .face = facing,
                                                 .hand_l = lh, .hand_r = rh, .visible = true });
cw_director_cut(d, &(cw_shot){ .type = CW_SHOT_PUSH, .subj = { boss }, .nsubj = 1, .dur = 3 });
if (cw_director_update(d, dt, &cam)) { /* cam.pos, cam.look, cam.fov to the Camera3D */ }
cw_director_release(d, 0.6f);   /* ease back to the game's camera (cw_director_set_gameplay) */
```

A story's shot beats go straight in: `on_shot` calls `cw_director_cut_story(d, shot, cast_to_subject)`.

| Type | The shot |
|---|---|
| `WIDE`, `ESTABLISH` | everyone; establish is high and far |
| `TWO` | two people side on, the 180-degree line kept from the last cut |
| `OTS` | over subj[0]'s shoulder onto subj[1]'s face |
| `MCU`, `CU` | head and shoulders; the face |
| `PUSH` | a slow dolly in on subj[0] over `dur` |
| `REACT` | a close-up on subj[0] listening to subj[1] |
| `INSERT` | subj[0]'s hand (raised to work height; a hand at the hip becomes a medium close-up), head, or a point |
| `HERO` | low, looking up at subj[0] |
| `WHIP` | a fast pan from the current framing onto subj[0], landing as a medium close-up or a two-shot |
| `TOP` | straight down over everyone |
| `RELEASE` | back to the game's camera |

Every shot type has a set of candidate placements around its subjects. Each is scored: the face (or the hand of an insert) in clear view of the lens, the face turned toward it, no other subject's head, hair or body on the line to the face, the subject's own hands and a blade held up off the face, nothing solid in front of the lens, the key point inside the frame and not too close. While a shot runs it is scored again every tenth of a second against where everyone is now; when it fails (a passer-by walks into the lens, the subject turns away) it solves again near the lens and eases there, or cuts when the move would be long or pass through someone. A close-up with no clean placement steps back to a medium close-up, then a wide shot. `ray` is the game's world (walls, pillars, terrain): true and the hit's fraction for a solid thing on a segment.

The camera never shakes: no handheld noise, no roll, every move eased. Hits and blasts in a cinematic go to the pad (`cw_input_rumble`). The measures the gates read are also `cw_director_measure`: frames something blocked the face (`CW_DIR_M_BLOCKED`), close-up frames on the back of a head (`AWAY`), frames the key point left the picture (`OUT`), and shake (`REVERSALS`: the camera's motion turning back on itself faster than 5 Hz, which a smooth move never does).

## Checks a game should run

- `node tools/story-compile.mjs <script> -o <out> --check` in CI: the committed story is the script's.
- `node tools/speech-bake.mjs ... --asr` after changing lines: every line heard as written.
- A gate that plays a scene on the pad alone (lines, a choice, the end state) with a control that picks another option; a skip gate with a control that taps instead of holding.
- A reactive line gate: the state that should fire it (three falls to one boss) and a control one short of it.
- A director gate on the game's own cinematics: `dir.blocked_frames`, `dir.away_frames` and `dir.reversals` all 0 through every boss intro and finisher, with a no-solve control (`cw_director_desc.no_solve`) that must fail.
