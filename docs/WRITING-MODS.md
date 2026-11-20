[dirtyloader docs](README.md) › writing a mod

# Writing a mod

A dirtyloader mod is a small C program for the M8's processor (a Cortex-M7). You compile it into an `.m8x` file,
put it on the M8's SD card, and **core** loads it at the next boot. Core is a small addition to Dirtywave's firmware,
put on the M8 once (the shop's Get started page). Core calls your functions when things happen on the M8: a key, a
frame, a note, a block of audio. It also gives you a page of your own and settings that the M8 keeps.

This page takes you from nothing to a mod running on your M8. It starts from [`examples/hello`](../examples/hello/).

## Contents

1. [What you need](#1-what-you-need)
2. [Build hello and run it](#2-build-hello-and-run-it)
3. [Your own mod](#3-your-own-mod)
4. [How a mod is put together](#4-how-a-mod-is-put-together)
5. [Core's API](#5-cores-api)
6. [Drawing](#6-drawing)
7. [One source, four firmware targets](#7-one-source-four-firmware-targets)
8. [Memory, and where your code runs](#8-memory-and-where-your-code-runs)
9. [Rules](#9-rules)
10. [Installing, testing, debugging](#10-installing-testing-debugging)
11. [When something is refused](#11-when-something-is-refused)
12. [Putting it in the shop](#12-putting-it-in-the-shop)
13. [Done?](#13-done)

## 1. What you need

- **Python 3.10 or later**, and this repository: `git clone https://github.com/irpina/dirtyloader`. Run every command
  below from its folder.
- **The ARM compiler**, `arm-none-eabi-gcc`:

  | system | how |
  |---|---|
  | Windows | in WSL (Ubuntu): `sudo apt install gcc-arm-none-eabi`. dirtyloader finds it there by itself. |
  | Linux | `sudo apt install gcc-arm-none-eabi` (or your distribution's package) |
  | macOS | `brew install --cask gcc-arm-embedded` |

  Check: `python -m dirtyloader.cc targets` lists the firmwares, and a build (next step) says which compiler failed
  if it can't run one.
- **An M8 with core on it.** The shop's [Get started](https://irpina.github.io/dirtyloader/shop/#start) page builds
  core into Dirtywave's firmware file in the browser and flashes it. Core runs on the **M8 Headless 6.6.4A**, the
  **Model:02 6.6.4A** and the **Model:02 6.6.2A**. For **Model:01 6.6.4A**, use the [command-line build and SD setup](MODEL01-664-PORT.md). See the [supported Model:01 hooks](CORE-EVENTS.md#model01-664a).
- **For the USB workflow on Headless and Model:02**, the tools open the serial port themselves. Close m8c and the
  shop's browser tab while you use them. On Model:01, copy mods to `dirtyloader/mods` on the SD card and use MOD LOADER.

## 2. Build hello and run it

```bash
python -m dirtyloader.cc build examples/hello
```

That builds hello for every firmware, into `build/mods/<target>/hello.m8x`, and prints what's in each file:

```
m8-headless: build/mods/m8-headless/hello.m8x
  hello 0.1.0 "Hello": 1020 bytes, code+data 832, bss 24, 41 relocations, firmware 0x00060604, runs from flash or ITCM (DLX2)
  hooks: ev_boot@50, ev_tick@50, ev_play@50, ev_stop@50
m8-model02: ...
m8-model02-664: ...
m8-model01: ...
```

On Headless and Model:02, pick the file for your M8 (`python -m dirtyloader.unit info` says which firmware it runs: `0x00060604` is the
Headless 6.6.4A, `0x02060604` the Model:02 6.6.4A, `0x02060602` the Model:02 6.6.2A), then:

```bash
python -m dirtyloader.unit add build/mods/m8-model02-664/hello.m8x
python -m dirtyloader.unit restart
python -m dirtyloader.unit status
```

`add` copies the file to `/dirtyloader/mods/` on the SD card, checks it arrived intact, and switches it on (it goes
into `/dirtyloader/active.txt`). Mods load at boot, so `restart`. `status` shows what core loaded:

```
hello.m8x: loaded hello 0.1.0 at 0x0002D340, 856 bytes, 4 hooks
```

On the M8:

- **Model:02:** all four directions at once (UP+DOWN+LEFT+RIGHT) open the MOD LOADER. HELLO is a card there
  (a mod core doesn't know shows under UTIL with a cube). **EDIT** opens its page: `HELLO, M8!`, a few
  counters, and UP / DOWN change COUNT. **SHIFT+RIGHT** opens its settings: one knob, WORD. **SHIFT+LEFT** opens
  its INFO page: who made it and what it does (its `DL_INFO`), and whether it runs. **SHIFT+EDIT** switches it
  off; leaving the MOD LOADER then offers to restart.
- **Headless:** LEFT+RIGHT opens the mods' pages (its settings as knobs). Its own page is in SYSTEM SETTINGS >
  MODS: RIGHT on its row.
- **Both:** the Mod Menu (SYSTEM SETTINGS > MODS > MOD MENU) has every mod's page and setting.

On **Model:01**, copy `build/mods/m8-model01/hello.m8x` to
`dirtyloader/mods/` on the SD card. Open MOD LOADER with all four
directions and use EDIT to enable the mod, then reboot. The Model:01
Hello build exercises TICK. Read `dirtyloader/status.txt` over USB DRIVE
to confirm it loaded. EDIT toggles it off for the next boot.

On Headless and Model:02, `python -m dirtyloader.unit off hello.m8x`
switches it off for the next boot.

## 3. Your own mod

1. Copy `examples/hello` to a folder of your own (in `examples/` if it's going into this repository).
2. Give it an **id**: lower case, digits and `-`, unique among mods (core refuses a second mod with the same id). It
   names the file, `<id>.m8x`. Change it in `DL_MOD(...)` and in the page's first field, which says which mod's row
   opens the page.
3. Change the title and the version. Raise the version whenever the file changes: the shop and the MODS page show
   it. Put your name and a sentence or two about the mod in its `DL_INFO`: the MOD LOADER's INFO page shows them.
4. Build, `add`, `restart`, `status`, as above.

`python -m dirtyloader.cc build <folder>` compiles every `.c` file in the folder into one mod. `--target
m8-headless` (or `m8-model02`, `m8-model02-664`, `m8-model01`) builds only that one; `-o file.m8x` names the file when you build
one target; `-D NAME=VALUE` adds a define. `python -m dirtyloader.cc info file.m8x` checks a file as core will and
shows its header.

A single command can build one source for all four firmware targets.
If the Model:01 build fails, the default command reports the reason
and keeps the successful outputs. An explicit `--target m8-model01`
returns an error. See [supported hooks](CORE-EVENTS.md#model01-664a).

A mod that includes only `dl/mod.h` and uses shared hooks can keep
`DL_FW` at its default of 0. If its code has no firmware-specific
addresses or model conditionals, the same .m8x can be used on every
core that provides the hooks it subscribes to. Including the selected
firmware header pins the output to that firmware, which is necessary
when the source calls stock routines or accesses firmware state.


## 4. How a mod is put together

```c
#include "dl/mod.h"            /* DL_MOD, DL_ON, the events */
#include "m8-headless/fw.h"    /* the firmware: addresses, keys, the screen (every target: see section 7) */
#include "dl/core.h"           /* core's API (section 5) */

static void on_tick(void) { ticks++; }

DL_MOD("hello", "0.1.0", "Hello",
       DL_INFO("you", "What it does, in a sentence or two."),
       DL_ON(EV_BOOT, 50, on_boot),
       DL_ON(EV_TICK, 50, on_tick));
```

- **`DL_MOD(id, version, title, ...)`** is the mod's header. It must be in the mod exactly once; the build puts it
  at the start of the file, and core reads it from there.
- **`DL_INFO(author, about)`** (optional) says who made the mod and what it does. The MOD LOADER's INFO page
  (SHIFT+LEFT on its card) shows them, read from the `.m8x` on the card, so a mod that is off shows them too. It is
  drawn in upper case, about 38 characters a line, six lines: keep it to two or three sentences. It is not a hook
  (an older core gives it a hook slot that never runs).
- **`DL_ON(event, order, handler)`** subscribes a function to an event. When several mods handle the same event,
  lower `order` runs first; use 50 unless you have a reason not to.
- **Events** are listed with their prototypes in [CORE-EVENTS.md](CORE-EVENTS.md). The ones most mods start with:

  | event | prototype | when | thread |
  |---|---|---|---|
  | `EV_BOOT` | `void f(void)` | once, at boot, after core has loaded every mod | UI |
  | `EV_TICK` | `void f(void)` | every pass of the firmware's main loop: thousands a second | UI |
  | `EV_DRAW` | `void f(void *ui)` | each screen frame (about 66 a second), before it is shown | UI |
  | `EV_KEY` | `int f(void *ui, int action)` | each key chord (`FW_KEY_*` bits), before the screen sees it | UI |
  | `EV_PLAY` / `EV_STOP` | `int f(void *seq, int mode)` / `int f(void)` | the sequencer starts / stops | UI |
  | `EV_MIDI` | `int f(uint32_t packet)` | USB MIDI coming in | UI |
  | `EV_AUDIO_OUT` | see CORE-EVENTS.md | each block of the master output | audio |

- **Taking an event.** Many handlers return `int`. Return 0 to let the M8 carry on as it would. Return nonzero to
  *take* the event: the firmware doesn't do what it was about to (a key the screen doesn't see, a note that isn't
  played, a play that doesn't start). Take only what you mean to.
- **Two threads.** The *UI thread* runs the screens, keys and SD card. The *audio thread* interrupts it every audio
  block (64 frames on Headless, 32 on Model:01 and Model:02, all at 44.1 kHz) and must finish quickly. A variable that both threads touch should be `volatile`, and
  should be one word (a `uint32_t` or a `float`) so neither side sees half of it.
- **State** lives in `static` variables. Zeroed ones (`.bss`) are zeroed by core before `EV_BOOT`; ones with a value
  start with it. There's no `malloc`.

## 5. Core's API

```c
static const struct dl_core *core;
static void on_boot(void)
{
    core = dl_core();          /* 0 if the M8's core is older than the API: check once */
    if (!core) return;
    core->page_add(&page);
    if (core->version >= 0x500) core->setting_add(&my_setting);
}
```

Everything is in [`dl/core.h`](../dirtyloader/sdk/include/dl/core.h), with the details in
[LOADER.md, "The core API"](LOADER.md#the-core-api-core-04). Call it from the UI thread (`midi_send` is the
exception). Check `core->version` before using anything added after 0.4: `0x500` for settings and the Mod Menu,
`0x600` for `ram_claim`.

On Model:01, `dl_core()` returns null. The shared
`dl_core_file_write(path, buf, n)` accessor writes an absolute SD path
from the UI thread on all four targets. It returns the byte count on
success or `-1` on failure. SD heartbeat uses it from BOOT.

| what | how | limit |
|---|---|---|
| a page of your own | `page_add(&page)`: `draw()` each frame, `key()` for keys (return 1 to keep a key, 0 to let core have it: OPT closes) | 8 pages |
| a setting | `setting_add(&s)`: a byte between `min` and `max`, with names to show. Core keeps it in `/dirtyloader/settings.txt` and puts it back at boot | 24 |
| data kept with the song | `song_data(&d)`: saved beside the song as `<song>.dldata`, put back when it loads | 8 |
| files on the card | `file_read`, `file_write` (absolute paths; not right after a song save or load) | |
| MIDI | `midi_send` (to the computer), `midi_receive` (into the M8, as if it came from the computer) | |
| a big buffer | `ram_claim(bytes)` at boot: RAM the firmware never uses (about 40 KB in all, first come first served, not zeroed) | |
| restart | `restart()` | |
| instruments | `inst_load`, `inst_save` | |
| modulation | `mod_dest_add`, `mod_dest_of` ([MOD-DESTS.md](MOD-DESTS.md)) | 16 a synth (31 on the Headless) |

## 6. Drawing

- **Text on full-core mod pages:** `fw_text(fw_grid, col, row, "TEXT")` writes into the firmware's 40 × 24 character grid. On
  your page, core clears the screen when it opens and then calls `draw()` every frame: write every row you use,
  padded with spaces so last frame's text goes. Columns 34 and on are the firmware's (tempo, tracks). Letters show
  in capitals.
- **Pixels, Model:02:** `fw_rect(fw_grid, x, y, w, h, colour)` fills a rectangle on the 480 × 320 screen (colours are
  RGB565: `FW_RGB(r, g, b)`). Draw in `EV_DRAW` (or `EV_VIS_DRAW` in the visualizer). What you draw stays until
  something draws over it, and every rectangle costs time (about 0.55 µs a pixel), so redraw only what changed.
  [`dl/pixui.h`](../dirtyloader/sdk/include/dl/pixui.h) has the font, icons and boxes core's own screens use.
  `#ifdef fw_rect` around pixel code keeps the Headless build compiling.
- **m8c and screenshots:** what the M8 draws also goes to m8c over USB. `python -m dirtyloader.m8screen shot.png`
  saves a screenshot of the M8 (needs Pillow: `pip install pillow`); `--keys "SHIFT+UP,DOWN"` presses keys first.

## 7. One source, four firmware targets

A mod is built once per firmware, because the firmware's addresses differ between versions. Every `.m8x` records
the firmware it was built for (`DL_FW`), and core refuses one built for another firmware ("built for other
firmware" in `status`).

- **`#include "m8-headless/fw.h"`** selects the device header through the builder's target alias.
- **`M8_MODEL01`** identifies Model:01 6.6.4A. **`M8_MODEL02`** identifies both Model:02 targets.
  Test for the binding your code uses. Both handheld targets define `fw_touch`,
  and the Model:02 targets also expose `fw_rect` and `FW_UI_VISUALIZER`.
- Everything you use from the firmware has to be in `fw.h`. Something that isn't (a new address) has to be found
  in every firmware first: [FIRMWARE-MAP.md](FIRMWARE-MAP.md) is the map, and
  [MODEL02-664-PORT.md](MODEL02-664-PORT.md) shows how addresses were found again for a new version.

## 8. Memory, and where your code runs

- **Your code and data** go into the M8's fast RAM (ITCM), after core. `status` shows how much there is ("mods area":
  about 19 KB on the Headless, 10 KB on the Model:02 6.6.4A), shared by every mod that's on. When a mod doesn't fit,
  core 0.7 puts its code in flash instead (a few times slower, still fine for most things) and only its data in
  RAM. `status` says "code in flash" when it did.
- **Small is good.** The build uses `-Os`. Keep big tables `const` (they go with the code).
- **Big buffers** (audio history, say) come from `core->ram_claim` at boot, not from static arrays. There's about 40
  KB of it in all, for every mod: CHAOS CONTROL takes nearly all of it, so plan for `ram_claim` returning 0 and run
  without the buffer then.
- **The C library** is only `memcpy`, `memmove`, `memset`, `memcmp` and `strlen`
  ([`sdk/lib/dl_libc.c`](../dirtyloader/sdk/lib/dl_libc.c)), plus the compiler's own helpers. No `printf`: hello
  formats its numbers itself. Floats are fine (the M7 has a single-precision FPU; prefer `float` and `f` constants,
  `double` is slow).
- **Warnings are errors** (`-Wall -Wextra -Werror`): fix them.

## 9. Rules

Each of these comes from something that went wrong on a real M8.

1. **Keep `EV_TICK` and `EV_DRAW` tiny.** They run thousands of times a second (`EV_TICK`) or every frame. Count,
   compare, set a flag; do the work when the flag says so.
2. **The audio thread** (`EV_AUDIO_OUT`, `EV_TRACK_AUDIO`, `EV_NOTE`, `EV_MOD_APPLY`, ...) must finish within its
   block. No SD card, no drawing, no core API except `midi_send`. [PERF-MON.md](PERF-MON.md) measured how much time
   there is.
3. **Don't touch the Teensy's pins on a Model:02.** Its board uses pins a bare Teensy 4.1 leaves free: sd-heartbeat
   blinking pin 13 hung the Model:02 within seconds. Leave pins alone under `M8_MODEL02` unless you have checked the
   pad is free.
4. **Settings and pages are `const`**, and their `value` byte a `static` of your mod: core finds which mod a
   setting belongs to by where its `dl_setting` lives.
5. **Take events only when you mean to** (return nonzero). A key you take is a key the M8 doesn't get.
6. **Check `core` and `core->version`** before using the API.
7. **Don't reach into the firmware past `fw.h`.** An address that works on one version is wrong on the next.
8. **Every global is reached through an address in a constant pool** (the build checks): don't use
   `-mslow-flash-data` or anything else that makes `movw`/`movt` address pairs. The default flags are right.

## 10. Installing, testing, debugging

| command | what |
|---|---|
| `python -m dirtyloader.unit info` | core's version, the firmware, whether the card is in, what loaded |
| `python -m dirtyloader.unit ls` | the mods on the card, and which are on |
| `python -m dirtyloader.unit add [--off] mod.m8x` | copy to the card (checked), and switch on |
| `python -m dirtyloader.unit on NAME` / `off NAME` | switch a mod on / off (the file name, `hello.m8x`) |
| `python -m dirtyloader.unit remove NAME` | delete it from the card |
| `python -m dirtyloader.unit restart` | reboot: mods load again |
| `python -m dirtyloader.unit status` | `status.txt`: each mod's result at the last boot, and a crash if there was one |
| `python -m dirtyloader.unit safe` | boot once without mods: run it, then reset or replug the M8 |
| `python -m dirtyloader.unit good` | clear the crash guard |
| `python -m dirtyloader.m8screen shot.png` | a screenshot of the M8 |

Add `--port COM11` (or `/dev/ttyACM0`) when more than one M8 is plugged in.

**When a mod crashes the M8,** the M8 restarts and core's **crash guard** loads no mods at the next boot.
`status` shows the crash and the address it happened at, and the MODS page title says `SKIPPED: A CRASH`. A
boot that hangs is caught too: core marks a boot with mods as `ARMD` and clears it after 10 s, so a boot that never
got there loads nothing next time. Fix the mod (or switch it off), then `good` or RESTART on the MODS page. If the M8
won't boot far enough to talk, `python -m dirtyloader.unit safe` and replug it. The shop's
[Recover](https://irpina.github.io/dirtyloader/shop/#recover) page does the same from a browser, and can put
Dirtywave's own firmware back.

**Model:01:** Use USB DRIVE for mod files and to read
`dirtyloader/status.txt`. MOD LOADER shows the saved enable setting.
If the crash guard is at `SKIP`, see the
[Model:01 recovery steps](MODEL01-664-PORT.md#current-build-and-installation).

**Seeing inside:** a `volatile` counter in your mod, read back with the unit's PEEK, tells you whether a handler
runs (`Unit().peek(address, n)` in Python; your variables' addresses are your mod's load address in `status` plus
their offset in the image). Your own page is usually quicker: draw the numbers you want to watch.

**Tests:** `python -m unittest discover -s tests -p "test_*.py"` (no firmware or compiler needed), and
`node tests/test_shop.mjs` for the shop. Both run in CI.

## 11. When something is refused

| message | where | fix |
|---|---|---|
| `arm-none-eabi-gcc failed: ...` | build | the compiler's own error: fix the code. Warnings count as errors |
| `the image does not start with DL_MOD(...)` | build | `#include "dl/mod.h"` and declare the mod with `DL_MOD` once |
| `word at +0x... moves by ...: not a plain address` | build | something is addressed with `movw`/`movt`: don't add flags that do that (rule 8) |
| `... failed` with "not found" | build | the compiler isn't installed (section 1) |
| `built for other firmware` | `status` | you added the `.m8x` for another firmware: pick the file for this M8 |
| `id already loaded` | `status` | two mods share an id: rename one |
| `no room` | `status` | out of RAM for mods: switch others off, or make yours smaller |
| `hook table full` / `too many mods` | `status` | too many mods on at once: switch some off |
| `checksum`, `not an .m8x`, `read failed` | `status` | the file on the card is damaged: `add` it again |
| `SKIPPED: A CRASH` / `SKIPPED: NO 10 S RUN` | MODS page, `status` | the crash guard (section 10) |
| nothing on screen, no `status` line | | it's off: `ls`, then `on NAME` and restart |

## 12. Putting it in the shop

The shop ([`site/shop`](../site/shop/), [SHOP.md](SHOP.md)) lists the mods in
[`site/shop/meta.json`](../site/shop/meta.json):

1. Add an entry: `id` (the mod's), `sources` (its `.c` files), `category`, `title`, `summary`, `details`, `keys`
   (how to use it, one line each) and `on_unit` (what it shows on the M8). Copy one that's there.
2. `python site/shop/make_shop.py` builds it for every target into `site/shop/mods/<target>/<id>.m8x` and writes
   `catalog.json` with each file's sha256.
3. Commit the `.m8x` files and `catalog.json` with your source. The site is published from `main`.

On the Model:02, core's MOD LOADER shows a mod it doesn't know under UTIL, with a cube and its file name. To give it
a name, category and icon there, add it to `lm_known` in [`core03.c`](../dirtyloader/core/c/core03.c) (a core
change: the firmware patches have to be rebuilt, see SHOP.md).

## 13. Done?

- [ ] It builds for every target with no warnings: `python -m dirtyloader.cc build <folder>`.
- [ ] It loaded on a real M8: `status` says `loaded`, with the hooks you expect.
- [ ] It does what it says, and the M8 still does everything else: play a song, change screens, save, load.
- [ ] Ten minutes with the song playing, and restarts, without a crash (`status` would show one).
- [ ] It does nothing harmful when its setting is off, or when core is older (`dl_core()` 0).
- [ ] On the Model:02: no pins touched, and drawing only where your screen is.
- [ ] Its header comment says what it does and how to use it, like `examples/hello`.
- [ ] Its `DL_INFO` names you and says what it does in two or three sentences (the MOD LOADER's INFO page).
- [ ] The version is raised if the file changed.
