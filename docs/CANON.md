# GhettoRack canon: how a module and its screens are made

Read this before you add a module, a page or a view, or change how one looks
or behaves. It is the rule book, not a description: `t95/ui/VIEWS.md` lists
what each screen shows, `AGENTS.md` is the project brief. Where a page and
this file disagree, the page is wrong, or this file needs changing first.

Every rule here was paid for with a round of rework. The reason is given so
you can judge the cases the rule does not cover.

## 1. What a module is

A module is one way to use the box: ApcBox, MIDI hub, Clock. One is active at
a time. It is the whole of this list, not just a page:

| Piece | Where | Rule |
|---|---|---|
| Entry | `MODULES` in `t95/bridge/rackbridge/common.py` | `id`, `name`, `about` (one line, what it is for), `url`, `ready`, `needs` (what must be plugged in), `profile` (its remote profile), `routes` (the MIDI routing it starts with) |
| Page | `t95/ui/<id>.html`, served by the bridge at `url` | one self-contained file; three layouts and the small one (section 2) |
| Routing | `routes` in the entry | applied when the module is selected. A module never grabs devices by another way |
| Remote | a name in `IR_PROFILES`, handled in the page's `action()`; in the small layout by `Small.remote` (section 3) | arrows walk the controls, ok presses, back and home leave to the start screen |
| Tile | the start screen builds it from the entry | nothing to add by hand |
| State | the bridge's state file, or the Daisy | never only in the browser, except view choices (which tab, zoom) |
| Tests | `t95/bridge/test_bridge.py` | section 9 |
| Words | a row in `t95/ui/VIEWS.md` | what each layout shows, and what is not there |

Tools that are not modules (Jam recorder, Settings) follow everything here
except the entry: they are tiles the start screen adds itself, and opening
one does not change the active module or its routing.

The Daisy keeps time and makes sound. A page shows and asks; it never
schedules a step, and it works with no APC40 plugged in.

A module may bring a sound engine of its own on the box, with its own sound
card and its own clock: the decks (`decks/`, through the Numark's card at
44.1 kHz). The same split holds there: the engine keeps its time and makes
its sound in one audio thread, the bridge and the page show and ask. Two
engines share a tempo only by one following the other through the Clock
module, never by a page or the bridge timing either of them.

## 2. Layouts

Every page has all four. A page that lacks one is not done.

| Layout | Drawn for | Chosen when |
|---|---|---|
| wide | 1920 px: TV, Mac, iPad on its side | the default |
| upright | 390 px: a phone, an iPad upright | the window is taller than wide |
| short | 844 px: a phone on its side | wide and under 520 px high |
| small | 480x320: the 3.5" touch screen on the box | `?small=1` |

- **Nothing scrolls, in any layout.** What does not fit is paged, or moved to
  another tab. A view has one purpose.
- **Small sticks to the browser.** Opened once with `?small=1`, every page
  stays small there (`localStorage`, key `gr-small`); `?small=0` ends it.
  Reason: links between pages are plain (`/`, `/settings`), and one that
  forgot the marker dropped the 3.5" screen into the TV layout.
- **A link to another page of the box is a plain path.** Do not thread
  `?small=1` or `?tv=1` through links to keep a layout alive: small sticks,
  and `tv` is only for what a TV needs on top (remote focus, pointer hiding).
- The TV and phone layouts are not to change when the small one is added.
  Each small test ends by checking that.

## 3. The small layout (480x320)

Built from `t95/ui/small.css` and `t95/ui/small.js`, which the bridge serves
at `/small.css` and `/small.js`. A page loads `small.js` and, when
`Small.on`, calls its own `small()` instead of its big code. Do not copy the
frame into a page. The ApcBox page is the one exception (it is one built
file with its own copy, class prefix `s-`); it is the model the shared files
were taken from, and the two must look the same.

### Frame

| Part | Size | Holds |
|---|---|---|
| Top bar | 480 x 24 | the page's name on the left; on the right one status with a dot, and at most two small buttons |
| Rail | 40 x 296, on the left | the page's sections, top to bottom, HOME last. Labels of 5 letters at most (6 are clipped). No rail when the page is one screen |
| Content | 439 x 296 (480 x 296 with no rail) | one section |
| Message line | the bottom 18 px of the content | section 5 |

`Small.frame({ title, tabs, onTab })` makes it. Chromium's window is never
under 500 px wide; the frame is drawn in the left 480 and the panel shows
exactly that.

### Sizes

| Thing | Rule |
|---|---|
| A target for a finger | 40 px or more in both directions. Buttons, list rows, keys |
| A dense target (stylus, fingernail) | down to 14 px, only in a view that also has a ZOOM with finger targets |
| Text | 11 px mono for labels, 12 for list rows, 14 for a title in the content, 40 for the one big number of a screen. Nothing under 10 |
| Gaps | 4 px between parts, 3 px between keys or cells |
| A list | 4 or 5 rows a page, a column of 118 px beside it for what acts on the picked row, `‹ 1 / 3 ›` under that column |

### Parts, all from `small.js`

| Need | Use | Not |
|---|---|---|
| A button | `Small.btn(text, fn, cls)`; `cls`: `on`, `go`, `warn`, `rec` | a styled `div`, a link |
| A list | `Small.pager(per, row)` | a scrolling box |
| Text from the user | `Small.keyboard({ title, value, max, secret })`; on the ApcBox page, which is its own file, `smKeyboard({ title, value, max })` with only the keys a name on the Daisy's card may have | an `<input>`: there is no keyboard on the box |
| A PIN or a number to type | `Small.numpad({ title, max })` | the keyboard |
| A number to nudge | a stepper, `‹ value ›`, applied at each tap | a field and a SAVE |
| One of a few | a row of buttons, the chosen one `on` | a dropdown: `<select>` does not work by touch here |
| On or off | one button that says the state (`BY ITSELF: ON`) and is `on` when on | a checkbox |
| A remote or a game controller | `Small.remote(es, { act, back })`, once, with the page's own event stream: arrows move a cursor to the nearest button that way, OK taps it, BACK cancels the keyboard or number pad, then goes one step back in the page (`back()`), then to the start screen; HOME and MENU go there. `act(a)` is for what belongs to the page alone (PLAY and STOP on the clock and the jam recorder) | a walk of its own in a page. A control must be a `button` with an `onclick` to be reached |

A part that is missing goes into `small.js` and `small.css`, with a line
here, before a second page needs it. Class names start with `g-`.

## 4. Touch

- **A touch acts when the finger lands.** `small.js` does this for every
  button with an `onclick`. Reason: a press on the resistive panel lasts 0.2
  to 0.3 s and a click only comes at its end. So: set `onclick`, do not
  listen for `click` yourself, and never make a press-and-release pair mean
  something a press alone does not.
- No hover, no long-press, no double-tap, no swipe. Each is either absent on
  this panel or costs the time just won.
- Dragging is for faders and pots only, and they take the whole strip as the
  target.
- A touch is not a mouse. The pointer shows only while a real mouse moves
  and goes after 5 s.
- Calibration is `pi4/screen/calibrate.py`, five crosses. Do not tune
  targets around a bad calibration; run it.

## 5. How things behave

| Situation | Rule |
|---|---|
| An action that cannot be undone, or that takes the box away (delete, forget, reboot, shut down, turn the hotspot on) | the button turns to `SURE?` in the `warn` colour; the second tap does it; picking something else cancels. No dialog |
| An action that needs the PIN | ask on the number pad at that moment, once; afterwards the top bar says UNLOCKED and nothing asks again. A wrong PIN sends nothing |
| A result | one line in the message line. Good: 3 s. Bad or a note: 6 s, in red if bad. A standing state stays until it changes. Say what happened and what the box did about it ("wrong password. Back on Home Net") |
| A control that does not apply now | disabled, in place. Not hidden: the screen must not rearrange itself |
| Nothing to show | one line saying what to do ("SCAN shows the networks in range"), where the list would be |
| Something long (a scan, a transfer, a join on trial) | the button says so (`SCANNING`), the message line counts or names the step. Never a spinner alone |
| A change made on the page | shown at once, sent, and not overwritten by the box's own report for half a second (`hold()` in `apcbox.js`). The box stays the truth: its report wins after that |
| The box is not answering | the status in the top bar says so, in red. Controls stay; they fail with a line, they do not vanish |
| What was last used | the start screen outlines the tile opened last, in green. The box boots into the last module, never into a tool |
| A setting | applied at each tap. A SAVE button only where a half-typed value would be harmful |

## 6. Colour

One meaning each. A new meaning needs a line here, not a new shade.

| Colour | Variable | Means |
|---|---|---|
| Green | `--green` | on, playing, connected, the thing to tap to go ahead (`go`) |
| Yellow | `--yellow` | selected, queued, the playhead, `SURE?` |
| Red | `--red` | recording, an error, an accented step, overload |
| Amber | `--amber` | muted, a default the module brought, a folder |
| Blue | `--blue` | solo, a value (a waveform, a pot's ring) |
| Grey | `--dim`, `--off` | a label, an empty cell, something off |
| White outline | `--text`, `.g-cur` | where a remote's or a controller's cursor is. Gone at the next touch |

No glows, shadows or gradients in the small layout (section 7). The TV and
phone layouts have them; that is the one visual difference allowed.

## 7. What the 3.5" screen can draw

The panel hangs on a 48 MHz SPI link: about 1.6 MB/s, and each frame sends
one rectangle around everything that changed.

- Things that change often must sit close together. A step counter in the
  top bar plus a playhead at the bottom made every frame a full redraw.
  Leave the counter out; the playhead says it.
- A row of meters across the whole width is the whole width, every time:
  update it 7 times a second, not 30.
- Elements, not a canvas, for anything that changes: a canvas is repainted
  whole.
- No animation, no blinking, no transitions.
- Measure, do not guess: `pi4/screen/framerate.py` (every ApcBox view at
  about 30 frames a second is the bar), `pi4/screen/touchlag.py`.

## 8. Words

- Buttons and rail labels in capitals, short, a verb where it acts: `SCAN`,
  `TO TRACK 3`, `LEARN TAP`. Status and messages in plain lower case.
- A label says what the thing is called on the box, not how it is built: "box
  SD card", not "mmcblk0".
- **The companion is "the box"**, on every page and in every message the bridge
  or the helper sends to one. Never "the T95" or "the Pi": the same pages run
  on both, and a Pi that says "no SD card in the T95" is wrong twice. The
  Seed is "the Daisy". A model's name belongs in docs and in comments.
- No "please", no "successfully", no exclamation marks.
- Name the thing a button will act on when it is not obvious (`TO TRACK 3`,
  `HEAR TRACK 3`, "opening ApcBox in 4 s").
- The same thing has the same name on every page and in the firmware's own
  screen. Check `VIEWS.md` before inventing a word.

## 9. Done means

A page change is done when all of these are true, and the write-up says
which were seen and which were not:

1. **A browser test** in `t95/bridge/test_bridge.py`, for the small layout
   at 500x320 (the kiosk's window): every tab; every control inside 480x320
   (`getBoundingClientRect`, not a look); no scrolling; no page error; one
   real round trip per kind of control, read back from the bridge or the
   firmware's control code, not from the page; and the TV page still the TV
   page. `test_p55` to `test_p57`, `test_r65` and `test_s35` are the models.
2. **The class it is in passes as a class**, not only the new test alone. A
   test must not assume it runs first: other tests leave presets, routes and
   a PIN behind.
3. **Screenshots from the Pi** of every tab: `pi4/screen/shot.py` drives the
   kiosk's own browser. Look at them yourself before showing them.
4. **Rene has seen it on the panel.** A screenshot comes from the browser,
   not from the glass: it cannot show the pointer, the frame rate, a
   mis-tap or what a finger covers.
5. **`VIEWS.md` has the row**, with what is not there.
6. **Left as found.** A test on the rig that toggles a step, loads a sample
   or sets a tempo puts it back, and says so.

On the rig, look but do not change: no scan, join, rename, restart or
record without being asked. Those have their own tests with stand-ins.

Someone may be playing. Before anything that interrupts or writes on a rig
(restarting the bridge, which every `install.sh` does; flashing or resetting
a Seed; saving, loading or deleting a set, kit or take):

- ask the bridge first: `/jam?json=1` (is a take recording), the `S` line
  (is it playing, is the set unsaved: counting the first number after `S` as 0, playing is
  number 0, the open set 53, unsaved 54, busy 55; one off and "unsaved" reads the open slot),
  the `J` lines (which slots are taken);
- never save over a slot, a kit or a file that exists without having read
  what is there, in a step of its own, before the write;
- a Seed with an unsaved set is not flashed or reset. Save it to a free slot
  first, or wait.

Both were learned on 2026-11-26: a bridge restart 76 s after a 49 minute
take ended, and a set saved over one already in slot 1.

## 10. Adding a module, in order

1. Agree what it is for and what its views are, with a drawing at 480x320.
   One purpose a view.
2. The entry in `MODULES`, `ready: False` until the page exists. Add its
   profile name to `IR_PROFILES`.
3. The page: `t95/ui/<id>.html`, loading `/small.js`, with `small()` and
   `big()` and `if (Small.on) small(); else big();` as its last line. Start
   from `clock.html`, the smallest complete one.
4. The bridge: a route for the page in `rackbridge/web.py`, an argument for
   its file in `rackbridge/bridge.py`, a line in `t95/bridge/install.sh` and
   in `apcbox-bridge.service`. Anything the page needs from the box is a
   JSON route on the bridge; anything needing root goes through the helper.
5. The four layouts.
6. The tests (section 9), then `ready: True`.
7. The row in `VIEWS.md`.

## 11. Mistakes already made, so they are not made again

| What happened | The rule it became |
|---|---|
| The start screen was right at boot and the TV page after HOME | small sticks to the browser (2) |
| A tap took over a second | act on press (4); keep changes close together (7) |
| 1 frame a second with the link idle | the screen driver must declare a real refresh rate (`pi4/screen/mkdriver.py`) |
| The old driver came back after a reboot | build for every installed kernel and refresh the boot image (`pi4/screen/install.sh`) |
| An arrow that never went away | no device may pose as a mouse to the compositor (`97-ghettorack-nopointer.rules`) |
| A page's styles leaked into the new layout (`.b`, `.v`, `.cap`) | a prefix on every class of a layout (`g-`, `s-`) |
| "Last used" stayed on the old tile | it follows the tile opened last (5) |
| A rail label cut off | 5 letters (3) |
| A test passed alone and failed in the suite | section 9, point 2 |
| A screenshot looked right and the panel did not | section 9, point 4 |
| Wrapping a page's code in a function broke a test that reached into it | the big layout's state stays reachable (`settings.html` ends by exposing what its test uses) |
