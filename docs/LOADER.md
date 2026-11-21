# Loading mods on the unit (core 0.3 to 0.5)

Core 0.3 is dirtyloader's answer to Elekloader's TELOS: the firmware is patched once, and from then on mods
are files on the M8's SD card. Core 0.4 adds what Elekloader's Digitone mk1 core gives mods beyond the hook bus
(an API, MIDI, pages, song data). Core 0.5 adds the sequencer, mixer, instrument and audio hooks, and a safe boot.
[PARITY-DN1.md](PARITY-DN1.md) compares the two feature by feature. You put them there over USB, switch them on or off, and the next boot loads
them. No more rebuilding and reflashing the firmware for every change of mods.

First build: **M8 Headless 6.6.4A** on a Teensy 4.1. Everything below has run on hardware (a spare Teensy 4.1
with an SD card): upload over USB, loading at boot, the MODS page, per-song mod lists, the crash guard, and
every implemented event, counted by `examples/core-selftest`.

The Model:01 port uses manual SD installation and a text MOD LOADER.
See [current Model:01 setup](MODEL01-664-PORT.md#current-build-and-installation)
for its firmware build, supported workflow, and hardware results.

| TELOS (Digitone mk1) | core 0.3 (M8 headless) |
|---|---|
| mods are files on the +Drive, loaded at boot | `.m8x` files on the SD card, loaded into free ITCM at boot |
| transfer over USB (SysEx) | transfer over the m8c USB serial port (`ev_serial`) |
| Mod Menu: list, on/off | SYSTEM SETTINGS > MODS (TELOS): list, on/off, apply and restart |
| projects remember their mods | songs remember their mods (`<song>.mods`); LOAD SONG MODS? on load |
| crash guard: skip mods after a crash | the same, from Teensyduino's crash record |
| core-dn1's Mod Menu, mod pages, project data, MIDI hooks | `dl/core.h` (core 0.4): pages, song data, files, MIDI in and out |

## What is where

| | where | what |
|---|---|---|
| hook bus | flash `0x60000200` | core 0.2's dispatcher, table and site stubs (layout 3) |
| loader | flash `0x601E0000` | `core03.c`, compiled with arm-none-eabi-gcc, in free flash past the image (`0x601DD400`) |
| core state | ITCM `0x29B40` | magic, hook table (64), the API pointer (`+0x208`), loaded mods, the MODS page, pages, song state; ITCM is MPU read-only until `core_early` opens it |
| mods | ITCM `0x2A4D0..0x2F980` | 21,680 bytes; mods run from ITCM, so they are fast and safe on the audio thread |
| scratch | ITCM `0x2F980..0x2FF80` | status text (1 KB) and a list buffer (512 B): `active.txt`, song lists, USB requests |
| veneers | ITCM `0x2FF80..0x30000` | up to 16 long branches from ITCM hook sites to their flash stubs |
| files | SD `/dirtyloader/` | `mods/*.m8x`, `active.txt`, `guard`, `status.txt`; each saved song gets a `<song>.mods` (and `<song>.dldata` when mods keep data) beside it |

The ITCM tail is free because the firmware's ITCM code ends at `0x29B40` and the FlexRAM gives ITCM six
banks (192 KB). The firmware never touches it (checked on hardware by filling it before `setup()`).

## Boot

1. `core_early`, first in `main()`: make ITCM writable (MPU region 1, RASR AP bits), write the ITCM veneers,
   retire the old table.
2. `core_load`, inside `setup()` at its call to `0x6008FDA8`: the card is mounted, and the firmware has not
   used it yet (no autosave, no samples, no audio). Core reads `guard` and `active.txt`, loads each listed
   `.m8x` into ITCM, relocates it, zeroes its `.bss`, adds its hooks in `order`, and writes `status.txt`.
3. `core_boot`, after `setup()`: put the MODS page in the free view slot (7), wrap the audio interrupt's
   vector for `ev_render_in` / `ev_render_out`, and set the magic so the dispatcher uses the new table; then
   `ev_boot` runs, loaded mods included.

Core uses the card in step 2, in answer to a USB request, from the MODS page, and after a song save or load
(below); never while the firmware has a file open.

`status.txt` after a boot with the heartbeat mod:

    dirtyloader core 0.4
    mods area 21680 bytes at 0x0002A4D0
    core-selftest.m8x: loaded core-selftest 0.1.0 at 0x0002A4D0, 624 bytes, 16 hooks
    core-api-demo.m8x: loaded core-api-demo 0.1.0 at 0x0002A740, 1120 bytes, 3 hooks
    hooks 24

Load errors are `E1` cannot open, `E2` not an .m8x, `E3` no room, `E4` read failed, `E5` checksum,
`E6` not a dl_mod, `E7` built for other firmware, `E8` id already loaded, `E9` hook table full, `E10` too many
mods (16).

## Crash guard

When a boot with mods crashes, Teensyduino's fault handler saves a crash record at `0x2027FF80` and reboots;
the record and ITCM survive that reset (not a power cycle). The next `core_load` sees a crash after a boot with
mods, writes `SKIP` to `guard`, loads nothing, and says so in `status.txt` with the crash pc. Nothing loads
while `guard` says `SKIP`. The full-core USB tool clears it with
`python -m dirtyloader.unit good`.

On Model:01, Model:02, and Headless, the mod screen shows **SAFE MODE - MODS NOT LOADED** when the guard
blocks loading. The mod list shows which mods are enabled for the next boot. Press PLAY to clear the guard. When the screen says
**REBOOT TO RE-ENABLE MODS**, reboot manually. If clearing fails, write exactly four bytes `GOOD` to `dirtyloader/guard` on the SD card
and restart. To recover after a freeze without a crash record, write `SKIP` to the same file before booting.

The core accepts a leftover `ARMD` from older builds and loads the enabled mods. It no longer writes a
startup marker or depends on an SD write after a timed startup window. `core->last_crash` gives mods the crash
record the boot found.

**Safe boot** (core 0.5), TELOS's missing "skip key at power-on" for a unit without keys:

    python -m dirtyloader.unit safe        then reset or replug the unit

The tool sends a 6-byte marker (all bytes >= 0x80, so no m8c command) over and over. core_load runs ~0.3 s after
reset, waits up to 250 ms for USB to be configured, then listens 350 ms. If the marker comes, it loads nothing
and writes `SKIP`. Measured on the Teensy: USB was configured within 10 ms of core_load, and the tool's first
bytes arrived 156 ms later. The cost is a boot ~0.35 s longer on a computer and 0.25 s longer on a charger.

## The MODS page

SYSTEM SETTINGS gets a row, **MODS (TELOS) OPEN** (core patches the settings screen's text and colour
templates, using a copy past the image). EDIT on it opens the MODS page, a screen of the firmware's own kind in
its free view slot: one row per `.m8x` on the card, then APPLY & RESTART.

| state | meaning |
|---|---|
| `ON` | loaded and running |
| `+ON` | switched on; loads at the next boot |
| `OFF` | not loaded |
| `-OFF` | running now; switched off from the next boot |
| `E<n>` | listed in `active.txt`, but the boot refused it (load error n) |
| `E<n>!` | not switched on: core 0.5 checks a mod when you switch it on (header, size, CRC, `dl_mod`, firmware) and refuses what the boot would |

UP / DOWN move, EDIT switches the mod under the cursor (written to `active.txt` at once), RIGHT switches it on
and LEFT off (as TELOS), EDIT on the last row restarts, OPT goes back to SYSTEM SETTINGS. The last row reads
RESTART when nothing has changed; the row after it, MOD MENU, opens the Mod Menu (every mod page as a grid, and the mods' settings). A running mod that added a page shows `>` after its state, and RIGHT opens
the page (OPT comes back). The firmware draws its track list from column 34, so core's text and pages keep to
columns 0-33.

## Songs keep their mods

TELOS projects remember their mods; on the M8 it is songs. Core subscribes to two events at the firmware's song
routines:

- **`ev_song_save`**: after a save, core writes `<song path>.mods`, the mods running then, one per line as
  `file id version` under a `#` comment.
- **`ev_song_load`**: after a load (not the AUTOSAVE load during boot), core reads the song's `.mods`. If it
  names a different set from the one running, the MODS page opens as **LOAD SONG MODS?**, with rows marked `+`
  (the song wants it, not running), `-` (running, the song does not want it) or `?` (the song names a file that
  is not on the card). YES writes the song's set to `active.txt` and restarts; NO keeps the running set.

The firmware keeps the card busy for a while after its save or load routine is entered, so core does this from
`ev_tick`, 300 ms later, retrying up to ten times. Mods' song data (below) is saved and loaded at the same
moments, into `<song>.dldata`.

## The core API (core 0.4)

What core does for a mod beyond calling its handlers: the M8 counterpart of core-dn1's exports. A pointer to
`struct dl_core` sits after core's hook table (`DL_CORE_PTR`, ITCM `0x29D48`); `dl_core()` in `dl/core.h`
checks it and returns 0 under an older core. Take it at `ev_boot`.

| member | does | core-dn1 |
|---|---|---|
| `restart()` | reboot now (clears the hang guard first) | `core_restart` |
| `page_add(page)`, `page_open(page)`, `page_close()` | a screen of the mod's own: `{mod id, title, open, draw, key}`. Opened from the MODS page (RIGHT on the mod's `>` row) or by the mod; core clears it, then `draw()` runs every frame and `key(action)` gets each chord (return nonzero to keep it; OPT closes) | the Mod Menu, mod pages |
| `song_data(block)` | `{id, data, size, loaded(n)}`: saved with each song as `<song>.dldata`, put back on load (the boot's AUTOSAVE included); `loaded(0)` when the song has none | project data |
| `file_read(path, off, buf, n)`, `file_write(path, buf, n)` | files on the card by absolute path, from the UI thread | `fw_card_read` / `_write` |
| `midi_receive(packet)` | a USB MIDI packet played into the M8 as if from the host (ev_midi sees it first) | `core_midi_receive` |
| `midi_send(packet)` | a USB MIDI packet to the host (ev_midi_out sees it) | `fw_sysex_send` |
| `last_crash` | Teensyduino's crash record as the boot found it (`[0]` 0: none, `[6]` the pc) | `ev_crash` |
| `inst_load(path, slot)`, `inst_save(path, slot)` (0.5) | an `.m8i` into a slot, or a slot to a file, through the firmware's own routines | `core_pool_load` |
| `setting_add(setting)`, `menu_open()` (0.5) | a settings row in the Mod Menu (`{label, value, min, max, names, changed}`), kept in `/dirtyloader/settings.txt`; open the Mod Menu | `core_additem`, the Mod Menu |
| `mod_dest_add(engine, name)` (0.5) | a modulation destination added to an engine's list (up to 16; 0.6 headless: 31, [MOD-DESTS.md](MOD-DESTS.md)): pickable in DEST, applied in `ev_mod_apply` | LFO destination names |
| `mod_dest_of(modulator, dest)` (0.6) | the real destination in `ev_mod_apply` (core keeps added destinations by name with the song and the instrument) | — |
| `ram_claim(bytes)` (0.6) | a buffer from RAM the firmware never uses (40 KB of DTCM between `.bss` and the stack), for as long as the unit runs | — |
| `song` | the path of the song last loaded or saved | |

`examples/core-api-demo` uses all of it: a page (song value, MIDI counts), a value kept with the song, a note
sent out on EDIT, a note played in on RIGHT.

For SD writes on all four targets, call
`dl_core_file_write(path, buf, n)` from the UI thread. It returns the
byte count on success or `-1` on failure. On Model:01 this accessor
works with the limited SD API, and `dl_core()` returns null.

## MIDI (core 0.4)

Headless has USB MIDI only. Each packet is a word: byte 0 is cable << 4 | CIN, bytes 1-3 the MIDI bytes
(`DL_MIDI_PACKET`, `DL_MIDI_STATUS` in `dl/core.h`).

- **`ev_midi`** `int f(uint32_t packet)`: every packet `usbMIDI.read()` takes. `usb_midi_read`'s call of
  `usb_midi_read_message` (`0x600A376A`) goes to core's C (`core_midi_read`), which reads the packet (or one a
  mod played in), raises the event, and hands it on; a handler that returns nonzero drops it.
- **`ev_midi_out`** `int f(uint32_t packet)`: every packet `usb_midi_write_packed` (`0x600A3238`) sends; take
  it to drop it.

Checked on hardware: three messages sent from the computer arrived as three `ev_midi` calls, the last one
`0x6307B00B` (CC 7 = 99); the demo's note out arrived at `ev_midi_out` as `0x033C9009`; and its played-in note
reached the M8's read path.

## The .m8x format

A mod is C built with the SDK (`dirtyloader/sdk/include/dl/mod.h`) and linked at address 0:

    #include "dl/mod.h"
    #include "m8-headless/fw.h"
    static void on_tick(void) { ... }
    DL_MOD("my-mod", "0.1.0", "My mod", DL_ON(EV_TICK, 50, on_tick));

`dirtyloader.cc.build_m8x([sources])` compiles it and writes:

| offset | size | |
|---|---|---|
| 0 | 4 | `DLX1` |
| 4 | 4 | image bytes |
| 8 | 4 | .bss bytes |
| 12 | 4 | relocation count |
| 16 | 4 | crc32 of image, then relocations |
| 20 | image | code and data; it starts with the `dl_mod` header |
| | 4 × n | offsets of the words to add the load address to |

Relocations are found without parsing ELF: the mod is linked at 0 and at `0x01000000`, and every word that
differs by exactly that amount is an address into the image. Anything else that differs is refused.

**DLX2** (core 0.7, what the SDK builds now) adds a sixth header word: the offset where the image's writable part
(`.data`) starts. Everything before it is code and constants. Core 0.7 reads both versions.

### MOD LOADER and OPTIONS (core 0.7): all four directions (Model:02), LEFT+RIGHT (Headless)

**Model:02:** all four directions at once (input action `0xE4`) open the MOD LOADER from any screen; the chord again,
or OPT, goes back. The chord used to toggle the firmware's visualizer: core takes it, and the visualizer opens from
the MOD LOADER instead (EDIT on CHAOS CONTROL, whose screen it is). Inside the visualizer the chord still closes it.
LEFT+RIGHT (`0x84`) does nothing on the Model:02 now.

**Headless:** LEFT+RIGHT (`0x84`, unused by the firmware) opens the machine pages; again, or OPT, goes back. All four
directions stay CHAOS CONTROL's (its own screen: headless firmware has no visualizer).

**Model:02: the MOD LOADER.** A full-screen page drawn with `dl/pixui.h` (5×7 font, pixel icons, cut-corner boxes):
- **Header:** MOD LOADER on a striped purple tab, and how many mods are on of how many on the card.
- **Tabs:** ALL, INST, FX, SEQ, UTIL (SHIFT+UP/DOWN). Each known mod has a name, category and icon in core's table
  (`lm_known`); others show their file name under UTIL.
- **Cards:** 2 × 4 a page. Each has the icon (20 × 20 pixel art at 2×, `pui_hicon`), the name (dimmed when off), a
  category pill and a small picture. The selected card has a purple frame and corner brackets, and its icon plays a
  short loop (a hop and a glint, 16 frames of 70 ms; each frame repaints the icon's box, background included, so
  nothing flickers). Arrows move.
- **Keys** (the footer: E OPEN, SE ON/OFF, S} OPTS, S{ INFO, OPT BACK):
  - **EDIT** opens the mod, if it runs: its screen, else its settings, else its INFO page. Its screen is the
    visualizer for a mod `lm_known` marks so (CHAOS CONTROL: the MOD LOADER closes and `core_tick` calls the
    firmware's input router with `0xE4`, straight to the router, so `ev_key` doesn't see it), or the page it added
    (`page_add`; OPT there comes back to the MOD LOADER).
  - **SHIFT+EDIT** switches the mod on or off from the next restart, as on the MODS page.
  - **SHIFT+RIGHT** opens its OPTIONS (below); a mod without settings shows its INFO page.
  - **SHIFT+LEFT** opens its **INFO** page: the icon, name and version, who made it and what it does (the mod's
    `DL_INFO`; without one, its `DL_MOD` title), whether it runs now and from the next restart (or why it was
    refused), its file and size, and its hooks, settings and page. Core reads all this from the `.m8x` on the
    card, so a mod that is off has one too. OPT or SHIFT+LEFT goes back to the cards; EDIT, SHIFT+EDIT and
    SHIFT+RIGHT work there as on its card.
  - **OPT** goes back. If the mods switched on differ from the ones this boot loaded, it asks first: **RESTART TO
    APPLY?** lists them (+ on, - off); EDIT restarts (lifting the crash guard, as the MODS page's RESTART), OPT
    leaves without restarting. The chord does the same as OPT.
- **OPTIONS:** the mod's settings (`setting_add`) as up to eight knobs (a 270° arc of 21 dots, label, value), in two
  rows of four; the selected knob is in brackets. Arrows pick, EDIT+LEFT/RIGHT turns by 1, EDIT+UP/DOWN by 4,
  SHIFT+LEFT/RIGHT flips between mods. Values are saved in `settings.txt` as from the Mod Menu.
- **Drawing:** one piece a frame (header, tabs, a card, the footer), each again only when it changes.
- **The whole screen:** on these two pages core's view says no to the firmware's right-hand column and top
  oscilloscope (its view methods `+0x18` / `+0x1C`, which the frame refresh `0x6004B514` and the view-draw dispatcher
  `0x6004CB7C` ask). It also holds off the column's other routines through `calls` sites (`core_side0`–`7`).
  Leaving gives the firmware its screen back whole.

**Headless: the machine pages.** The same knobs as text pages, a page per mod, in the manner of Elektron's machine
pages and Elekloader's machine-pages mod.

**Ownership:** a setting belongs to the mod whose code holds its `dl_setting`, so declare it `const`. Core records
each mod's code range at load. Core keeps up to 24 settings (8 before 0.7).

Known gaps:
- Core's own screens (MODS page, Mod Menu, loader, OPTIONS) take every key, so PLAY doesn't start or stop the song
  while they're open.
- The search box and the concept's SORT key are drawn but do nothing yet.

| MOD LOADER | OPTIONS |
|---|---|
| ![MOD LOADER on a Model:02](../shots/core07/loader.png) | ![OPTIONS on a Model:02](../shots/core07/loader-options.png) |

### Flash placement (core 0.7)

Mods run from ITCM, and ITCM is small: about 12.5 KB for mods on a Model:02, 20 KB on headless. So a mod that
doesn't fit there runs its code from flash:
- **Where:** core installs it from the SD card into a 1 MB flash area (`FW_MOD_FLASH`, `0x60600000`), which is
  erased on a stock unit and outside the firmware, core, and EEPROM emulation.
- **What goes where:** code and constants (DLX2's image up to the `.data` offset) go to flash, each mod on a 4 KB
  boundary. `.data` and `.bss` go to ITCM.
- **Relocation:** the code is relocated in the spare DTCM (`FW_SPARE_RAM`; no mod has claimed it yet), then
  compared with flash. Flash is erased and programmed only if it differs, so an ordinary boot writes nothing.
  `status.txt` says "(written)" when it did.
- **Writing:** core uses Teensyduino's own routines (`eepromemu_flash_erase_sector` / `eepromemu_flash_write`, in
  ITCM, interrupts off), then drops the instruction cache.
- **Order:** mods load in `active.txt` order, and each goes to ITCM if it fits, otherwise to flash. Put a mod whose
  `.bss` is large early: its `.bss` needs ITCM either way.
- **Cost** (PERF MON on headless, CHAOS CONTROL): from flash it takes 5–16 µs more per audio block on average, and
  its worst blocks about twice as long (instruction-cache misses). Fine for most mods; keep the heaviest DSP in
  ITCM if you can.

Tested on both units:
- examples/flash-probe has a 24 KB table and only fits in flash. Its table sum, function-pointer table, `.data`
  pointer into flash and `.bss` counter all read back right. A second boot didn't write.
- On a Model:02, CHAOS CONTROL (10 KB of code) runs from flash beside lfo-sends and perf-mon in ITCM.

Core reads the relocations in just past the image, where the `.bss` goes. So loading needs image + the larger of
the two. Once they are applied, the mod keeps only image + `.bss`, and the next mod loads right after (since
0.6; before, the relocations' space stayed reserved). A mod's literal pools make many relocations: CHAOS CONTROL
has 342, 1.4 KB, against 212 B of `.bss`.
`dl_mod.fw` names the firmware the mod was built for (the device header sets `DL_FW`); core refuses others.

## Transfer (USB)

Headless has no USB drive mode (no `USB_DRIVE` entry and no mass-storage interface in its USB descriptors),
and the shop needs to put mods on a unit anyway, so core takes files over the USB serial port m8c uses.

A new event, `ev_serial`, sits at the entry of the m8c command parser (`0x6007846C`), which drains every
waiting byte. Core's handler runs first, peeks the receive queue, and takes the call when the next byte is
`0xD1`, which never starts an m8c command (those are ASCII). m8c works as before.

**Request** (host to unit): `D1` cmd seq len(u16 LE) payload sum8, where sum8 is the low byte of the sum of
cmd through payload. **Reply**: a SLIP frame (`C0` ends it, `DB DC` / `DB DD` escape) `F1` cmd seq status
data, alongside any display frames.

| cmd | payload | reply data |
|---|---|---|
| 1 INFO | | text: core, firmware, card, mods area, loaded ids |
| 2 LIST | directory | names, one per line |
| 3 PUT | offset u32, name len u8, name, data (≤ 1 KB) | bytes written u32; offset 0 creates or truncates |
| 4 CHECK | name | size u32, crc32 u32 |
| 5 GET | offset u32, max u16 (≤ 512), name len u8, name | data |
| 6 MKDIRS | | creates `/dirtyloader/mods` |
| 7 RESTART | | (reply, then reset) |
| 8 PEEK | address u32, count u16 (≤ 512) | memory, for development. Nothing guards the read: stay in RAM, and away from address 0 and the stack guard at `0x200415E0`, which fault |
| 9 DELETE | name (under `/dirtyloader` only) | (core 0.4; SdFat's `FatFile::remove` `0x6009FF4C` / `ExFatFile::remove` `0x6009DADC`) |
| 10 POKE | address u32, bytes | (core 0.5) write RAM, `0x20000000..0x20280000` only: development and tests (the hardware tests write OFF notes into a phrase with it) |
| 11 THEME | none, or 39 colour bytes [+ flags u8 [+ name ≤ 12]] | (core 0.7) none: the 13 colours (39 bytes, the `.m8t` order) and the 13-byte name. Colours: set them in the system settings, the palette and a full redraw, at once. Flags bit 0: also save the settings to EEPROM (kept across power-off). A name (A–Z 0–9 space - _) shows on THEME SETTINGS. See "The theme" in [FIRMWARE-MAP.md](FIRMWARE-MAP.md) |
| 12 THEMEX | none, or flags u8 + 15 colours (45 bytes) [+ save u8] | (core 0.7) the extended theme: none reads the flags (1 track colours, 2 screen titles, 4 titles of CHAIN/PHRASE/TABLE/INSTRUMENT follow the current track) and the 15 colours (tracks 1–8; the titles of SONG, CHAIN, PHRASE, INSTRUMENT, TABLE, MIXER, the rest). Setting them repaints every view; save bit 0 also writes `/dirtyloader/theme.ext` (`DLXT`, flags, 3 zero bytes, the colours: 53 bytes), which the next boot loads. See [The extended theme](#the-extended-theme-core-07) |

Status: 0 ok, 1 bad request, 2 checksum, 3 bad path or no file, 4 SD error, 5 no card, 6 timeout, 7 unknown.
Names are `[A-Za-z0-9._-/]`, no `..`, under 64 bytes. PUT writes only under `/dirtyloader/` (names are
relative to it); LIST, CHECK and GET also take an absolute path (`/Songs/x.m8s.mods`).

The host side is `dirtyloader.unit` (Python, no dependencies; `dirtyloader.serialport` opens the port):

    python -m dirtyloader.unit info
    python -m dirtyloader.unit add sd-heartbeat.m8x     upload, verify by crc, switch on (--off: leave it off)
    python -m dirtyloader.unit remove sd-heartbeat.m8x  delete it from the card (clear: all of them)
    python -m dirtyloader.unit ls
    python -m dirtyloader.unit off sd-heartbeat.m8x
    python -m dirtyloader.unit restart
    python -m dirtyloader.unit status

Close m8c first; the port can only be open once. The [mod shop](SHOP.md) (`site/shop`) speaks the same
protocol over Web Serial: install, switch on and off, remove, restart, and the theme.

## The extended theme (core 0.7)

The M8's THEME page has 13 colours. Core adds 15: a colour per track, and a title colour per screen. It gives the
firmware's draw loop a 28-entry palette (the 13, copied before each refresh, then core's 15; FIRMWARE-MAP.md, "How a
cell gets its colour") and, at `ev_draw` after every mod's handler and only on passes that will refresh, rewrites the
attribute bytes of the plain text it recolours:
- **Track colours** (flag 1): SONG's header `1`…`8` and the chains under each (VALUE text), and the track list: `1`…`8`
  down one column in the right half, each alone in its cell, not a `0` above or a `9` below (so CHAIN's and PHRASE's
  row numbers stay as they are).
- **Screen titles** (flag 2): TEXT:TITLES text takes its screen's colour: SONG, CHAIN, PHRASE, INSTRUMENT (+ MODS,
  POOL), TABLE, MIXER (+ EFFECT SETTINGS, EQ, LIMITER), everything else.
- **Follow the track** (flag 4): CHAIN, PHRASE, TABLE and INSTRUMENT titles take the current track's colour: the one
  the cursor colour marks in the track list.

The cursor, selection blocks and play markers are never changed, nor are core's own pages. Turning it off repaints
every view from scratch (`redraw(obj, 1)`). If the literal doesn't hold the firmware's palette at boot (another
firmware), core leaves it alone and THEMEX answers "unknown command".

On the Headless Teensy (2026-11-18): rainbow tracks on SONG, the track list on SONG, CHAIN and PHRASE; CHAIN's and
PHRASE's titles in their screen colours, then in track 1's and track 4's as the cursor moved; kept across a restart
from `theme.ext`; off restores the stock screen. perf-mon: UI loop 260–268 k passes a second with it on, 266 k off.
The Model:02's addresses are checked against its image (tests/test_model02.py), not yet on the unit.

## Bring-up notes

- **Write every byte a mod places, 0xFF included.** The HEX writer used to leave out 0xFF bytes past the stock
  image, trusting the bootloader to have erased them. It had not: a `bl` encoded `FF F7 ..` kept a byte from
  an earlier build and the M8 crashed, at different places from build to build. `ihex.write(..., cover=)` now
  takes every site's range. Most of a day of "SD access breaks the firmware" was this.
- Check the firmware's mounted flag (`fw_storage + 4`) before any SdFat call, as the firmware does.
- The M8 screen can be read without m8c: send `E`, then decode the SLIP frames; `0xFD` frames are characters
  (char, x u16, y u16, colours). Keys go in as `C` plus a bitmask (LEFT 80, UP 40, DOWN 20, SHIFT 10, PLAY 08,
  RIGHT 04, OPT 02, EDIT 01). `python -m dirtyloader.m8screen` does both and writes a PNG.
- Reading address 0 (the NULL trap) or the stack guard (`0x200415E0`) faults the unit; the first such PEEK
  tested the crash guard by accident.
- Teensyduino's crash record in this build has a length of 11 words, not 44; core accepts both.
- The settings screen's enter code reaches its templates through a literal that points at the colour template
  (`0x60074018`), not the text one; core patches all three literals.
- PEEK and POKE have no fault guard: address 0 and its neighbourhood (`0x9308` faulted), the stack guard
  (`0x200415E0`) and parts of OCRAM fault. The crash guard catches the reset that follows.
- Terminate every path core builds: core 0.3 built `<song>.mods` without its NUL, so the name ran on into the
  stack; opens failed until the stack happened to hold a 0, and one write left a stray `1.m8s.mods#`.

## Testing on hardware

`examples/core-selftest` subscribes to every implemented event and counts each call into `selftest_counts`;
PEEK reads the counters (the load address is in `status.txt`, the offset in the build's symbols). On the spare
Teensy: boot 1; tick and draw about 380k a second; render in and out about 740 a second each; one note per
played note; play and stop once per PLAY press; one midi per packet in, one midi_out per packet out; key,
view, tempo, song load and save, and serial as used. Core 0.5's, on a phrase of four C-4s with two OFFs:
`ev_row` 26 and `ev_fx_row` 78 in 3 s (three columns a row), `ev_note_off` 4, `ev_mod_apply` thousands a second
on each of FM, HYPER, MACRO, SAMPLER, WAV and EXTERNAL once MOD1 had a destination, `ev_mute` 8 per toggle,
`ev_step_edit` 1 per note entered, `ev_param_set` 1 per field edit, `ev_inst_save` / `ev_inst_load` 1 each,
`ev_sysex` 1 per SysEx, `ev_card_ready` and `ev_usb` 1 at boot, `ev_audio_out` 691 a second.
`examples/audio-tone` (exclusive audio) came out of the M8's USB audio as a 440.0 Hz sine at 0.2 of full scale,
recorded on the PC (winmm). `examples/track-silence` (`ev_track_audio`) took a WAVSYNTH track's recorded output
from 6,530 RMS to silence. `ev_fx_audio` counted all three send effects and `ev_render_params` one per block.
The demo's added WAVSYNTH destination showed as `0F DEMO` in DEST and was applied 28,352 times in 1.5 s, and its
LEFT copied instrument 00 into 01 through `/Instruments/api.m8i`; the pool's delete fired `ev_inst_clear`.

Mods are compiled freestanding; `sdk/lib/dl_libc.c` (memcpy, memmove, memset, memcmp, strlen) and libgcc are
linked into each, so a struct copy links. A mod's own definitions win.

## Next

- The Model:02 port (its own bindings; needs a Model:02 unit to test).
- The Model:02 build: its own bindings, and the `.bin` container instead of HEX.
