# Localize your game

You take a tiny game, Coin Counter (press a button, collect a coin), and
ship it in English, Arabic and Japanese. Along the way: translation
catalogs and PO files, plural forms, switching the language while the game
runs, pseudo-locales that show layout problems before a translator does,
right-to-left and CJK text with the right fonts, larger text and captions
for accessibility, and a test that fails when a translation goes missing.

The API is [l10n.h](header:l10n.h) and [a11y.h](header:a11y.h). The
finished project is `docs/tutorials/localize/project/`.

## Start from the template

```sh
cp -r editor/templates/empty_2d coins
```

In `project.cwproj` name the game and add two actions besides `ui_accept`
(collect a coin): one changes the language, one turns on larger text.

@include docs/tutorials/localize/project/project.cwproj

`CMakeLists.txt` and `.gitignore` stay as the template made them.

## Catalogs

A catalog (`.cwtr`) is one locale: a line per entry, `key = text`. The
English one is the source the others translate:

@include docs/tutorials/localize/project/l10n/en.cwtr

- A comment right before an entry is the translator's note.
- `{n}` and `{name}` are placeholders, filled in by the code.
- `hud.coins[one]` and `hud.coins[other]` are plural forms. Which form a
  number takes comes from the CLDR rules of the active language, not from
  the code.

Arabic has six plural forms, reads right to left and needs a font with
Arabic letters, which `@font` names:

@include docs/tutorials/localize/project/l10n/ar.cwtr

Translators often work in gettext PO files instead. A PO file can be the
source of a catalog. This one was made from a Japanese `.cwtr` with
cwpack, which carries the `@` fields as `X-Cartwheel-*` headers so the round
trip loses nothing:

```sh
build/native/tools/cwpack/cwpack --po-export ja.cwtr --po-source l10n/en.cwtr > po/ja.po
build/native/tools/cwpack/cwpack --po-import po/ja.po      # back to a canonical .cwtr
build/native/tools/cwpack/cwpack --cwtr-check l10n/*.cwtr  # exit 1 on an error
```

@include docs/tutorials/localize/project/po/ja.po

Japanese has a single plural form, so `msgstr[0]` is all of them.

The localization project lists what the game ships: the source and
default locale, every catalog, and the pseudo-locales to offer.

@include docs/tutorials/localize/project/l10n/project.cwl10n

`roots.txt` tells cwpack to pack it. cwpack follows the project to each
catalog, converts the PO file into `l10n/ja.cwtr`, and packs the fonts the
catalogs name:

@include docs/tutorials/localize/project/roots.txt

The Noto fonts are SIL OFL 1.1 and come from the engine's `deps/fonts`
(`scripts/fetch-deps.sh` fetches them, pinned). A source path starting with
`@engine/` is relative to the engine checkout, so these lines work wherever the
project lives; other source paths are relative to `roots.txt`. Ship each
font's license text beside it as this does. Noto Sans JP is 9 MB. A `.cwimport` sidecar
subsets it at pack time to the glyphs the catalogs can reach (here 9 MB
becomes under 400 KB):

@include docs/tutorials/localize/project/fonts/NotoSansJP.ttf.cwimport

## Keys in the scene

The HUD in `main.cwscene` is a column of labels. A label whose text is a
key (`hud.title`) translates itself, and again whenever the locale changes.
Text that is not a key passes through. Two labels are filled from code
instead, so they turn `auto_translate` off:

@include docs/tutorials/localize/project/main.cwscene

The `CaptionPanel` at the bottom shows captions (below).

## Load, translate, format

@include docs/tutorials/localize/project/src/main.c#load

`cw_l10n_debug_register` publishes the catalog stats as debug values
(`l10n.missing` is the one tests care about) and adds an `l10n` debug
command. `cw_l10n_on_change` calls `refresh` after every locale change,
which builds the two texts the code owns:

@include docs/tutorials/localize/project/src/main.c#refresh

- `cw_trn` picks the plural form for the number and formats the number
  for the locale: `3 coins`, `٣ عملات` (Arabic-Indic digits, the "few"
  form), `コイン3枚`.
- `cw_trf` fills named placeholders. Word order is the translator's: the
  Japanese line puts the name where Japanese grammar wants it.
- Never build a sentence by gluing translated pieces together in code; give
  the translator the whole sentence with placeholders.

## Switch the language at runtime

@include docs/tutorials/localize/project/src/main.c#locales

@include docs/tutorials/localize/project/src/main.c#set_locale

`cw_l10n_set_locale` picks the catalog (or the closest: `ja-JP` finds
`ja`), installs the locale's fonts as the global fallback chain, lays the
UI out again, mirrored for a right-to-left language, and notifies
listeners. Nothing else has to be reloaded. A key the chosen catalog lacks
is looked up in the fallback chain (parent tag, `@fallback`, then the
default locale), so an incomplete translation shows English for the
missing lines instead of a key.

The input handler collects coins, switches the language and toggles
larger text:

@include docs/tutorials/localize/project/src/main.c#input

A shipping game starts from `cw_l10n_system_locale()` (the player's
platform language) or a saved setting. Coin Counter starts in English so a
test run is the same on every machine.

## Pseudo-locales

Before any translator is involved, two pseudo-locales derived from the
English catalog show what translation will do to the game:

- `en-XA` accents every letter, pads text by 40% and brackets it:
  `[Çöîñ Çöûñţéŕ ·····]`. Text that overflows or gets cut off at 140% will
  break in German or Finnish too. A key with no entry shows bare and
  unaccented (`hud.hnit`), so it stands out.
- `ar-XB` turns the English text right to left and mirrors the UI, so
  layout that breaks in Arabic shows up without reading Arabic.

Only text that came from a catalog is pseudo-localized. Plain text
written in the code stays exactly as written, so in `en-XA` a hard-coded
English string is the one line without accents and brackets. The test
below also catches it in a real translation (Japanese).

## Right to left, and CJK

Nothing in the game code is specific to Arabic or Japanese. The engine
shapes complex scripts with kb_text_shape: Arabic letters join, runs
reorder right to left, digits follow `@number.digits`. Japanese wraps
between characters and keeps closing punctuation off the start of a line.
What the game must supply is fonts that have the glyphs: the built-in UI
font covers Latin, `@font` adds the rest per locale. A character no font
in the chain has is drawn as the font's `.notdef` (usually a box) and
counted in
`cw_r2d_get_stats()->glyphs_missing`, which the test checks.

## Larger text and captions

@include docs/tutorials/localize/project/src/main.c#large_text

`cw_ui_set_text_scale` scales every UI text and re-lays the UI out, so the
hint wraps onto more lines instead of running off the screen.
`cw_theme_high_contrast` is a built-in theme; `cw_theme_colorblind` is the
other.

Captions show speech and sounds as text. A track is a small text file
with times in seconds, and both the speaker and the text are catalog keys:

@include docs/tutorials/localize/project/captions/intro.cwcap

`set_locale` restarts the intro track, so it plays in the new language. A
one-off sound caption is `cw_captions_show("caption.coin", "", 1.5f)` in
the input handler. With no voice attached a track runs on the engine's
frame clock, the same on every host; with a voice it follows the sound's
played frames. Players can turn captions off with
`cw_captions_set_enabled(false)`.

## Test it

What the UI shows is in its accessibility tree (`cw_a11y_snapshot`), the
same text a screen reader reads, translated. The game hashes it per
locale, and in Japanese counts texts with Latin letters in them: in this
game those never went through a catalog.

@include docs/tutorials/localize/project/src/main.c#measure

Build and run 240 frames with `input.json`: two coins, then the language
key through `en-XA`, Arabic, Japanese (a third coin there), `ar-XB`,
English, larger text on, `en-XA` and back to Arabic.

```sh
node tools/cwproject.mjs build coins all
node tools/run-cart.mjs --host native --app coin_counter \
  --exe coins/.cwbuild/native/project/coin_counter \
  --cart coins/.cwbuild/cart/carts/coin_counter.wasc \
  --frames 240 --input docs/tutorials/localize/input.json \
  --state out/coins.json --shot out/coins.png
```

```text
"l10n.missing":0,"game.coins":3,"game.locale":2,"game.visited":5,
"text.hash":[443668265,-2131796030,-1377602793,-574626675,1005248296],
"text.untranslated":0,"text.glyphs_missing":0,"text.scale_pct":125
```

Every locale was shown, no key was missing, no glyph was missing, nothing
untranslated showed in Japanese, and each locale's texts hash to a fixed
value. The last frame is the Arabic screen at 125%: right-aligned, shaped,
`٣ عملات`. The same command with `--host romdev` gives the same
values and the same pixels from the cart.

The hashes make any text change visible: a reworded catalog line, a
plural that picks another form, a number formatted differently. When the
change is intended, update the expected values with it.

Two bugs, injected to prove the test can see them
(`LOCALIZE_TEST_FAULT`, read once in `init`):

- `missing_key`: the hint uses a mistyped key, `hud.hnit`. It shows as the
  key itself in every locale, the log says which key once, and
  `l10n.missing` becomes 1. The gate's control.
- `hardcoded`: the hint is English text written in the code. Nothing is
  missing, so `l10n.missing` stays 0, but in Japanese
  `text.untranslated` becomes 2.

`node tests/tutorials.mjs localize` runs this on native and romdev: every
value above equal on both hosts and equal to `tutorial.json`'s
expectations, the frames equal, and the `missing_key` control, which must
fail.

## Where to go next

- `samples/l10n_demo` localizes a whole settings screen: number and date
  formats, per-locale images (`cw_l10n_asset_path`), input binding names,
  a language menu, focus navigation with a gamepad.
- [The localization and accessibility guide](guide:l10n-a11y) covers the
  rest of both headers.
