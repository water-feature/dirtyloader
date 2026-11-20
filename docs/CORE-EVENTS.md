[dirtyloader docs](../README.md) › Core events

# The hook bus (core's events)

The M8 core follows Elekloader's hook bus: **the core owns the shared sites, and mods subscribe to its events**
instead of patching them. Event names, the "when / C prototype / notes" table and the conventions match
Elekloader's (its `docs/ADAPTING.md`, "The hook bus"), so a mod author who knows one knows the other. Where the
M8 differs (no encoders, a touch screen, an audio-thread sequencer), it gets its own events, as the Digitone and
the Octatrack do on the Elektron side.

The original Model:02 mappings below are for 6.6.2A. The
[Model:02 6.6.4A port](MODEL02-664-PORT.md) records the later bindings.
The Headless section records its core 0.3–0.5 event work.

## Model:01 6.6.4A

The Model:01 menu build supports these mod subscriptions.

| Event | C handler | Verification |
| --- | --- | --- |
| `EV_BOOT` | `void f(void)` | SD heartbeat passed ARM emulation |
| `EV_TICK` | `void f(void)` | Tick probe verified on hardware |
| `EV_KEY` | `int f(void *ui, int action)` | MOD LOADER key handling verified on hardware |
| `EV_TOUCH` | `int f(void *touch)` | Touch Probe passed ARM emulation |
| `EV_TRACK_AUDIO` | `void f(float *left, float *right, int track)` | Track Silence verified on hardware |
| `EV_AUDIO_OUT` | `int f(float *left, float *right, int frames)` | Audio Tone passed ARM emulation |

Audio hooks receive 32 frames per channel. Track numbers are zero-based.
`EV_TOUCH` sees the state after the stock touch poll. A nonzero
return consumes a key or touch event. See the
[Model:01 port notes](MODEL01-664-PORT.md) for binding and test details.
The loader dispatches `EV_CARD_READY` internally, but the Model:01
builder does not currently accept it as a mod subscription.

## Implemented: M8 Headless 6.6.4A (core 0.3 to 0.5)

Mods subscribe with `DL_ON(EV_..., order, handler)` (`dl/mod.h`); firmware locations are in
`m8-headless/fw.h`.

| event | site | C prototype | notes |
|---|---|---|---|
| `ev_boot` | after `setup()` (`0x600A28FA`) | `void f(void)` | mods' `.bss` is zeroed already |
| `ev_tick` | before `loop()` (`0x600A28FE`) | `void f(void)` | about 380k a second |
| `ev_draw` | before the frame is pushed (`0x6007FAB8`) | `void f(void *ui)` | about 380k a second |
| `ev_key` | before the input router (`0x6007FBA0`) | `int f(void *ui, int action)` | `action` is the key chord (`FW_KEY_*`); take it to hide it from the screen |
| `ev_view` | `set_view` entry (`0x6004A96C`) | `int f(void *ui, int view)` | take it to stay on the current screen |
| `ev_serial` | m8c parser entry (`0x6007846C`) | `int f(void *m8c)` | core takes it for transfer requests (`0xD1`) |
| `ev_tempo` | tempo setter entry (`0x60090978`) | `int f(float bpm)` | |
| `ev_play` | sequencer start (`0x60090618`) | `int f(void *seq, int mode)` | `seq` is `0x2000BE80`, its play flag at `+0x3B8`; take it to refuse |
| `ev_stop` | sequencer stop (`0x600907A4`) | `int f(void)` | take it to refuse |
| `ev_midi` | `usb_midi_read`'s read (`0x600A376A`, core's C) | `int f(uint32_t packet)` | each USB MIDI packet in (and those mods play in); take it to drop it |
| `ev_midi_out` | `usb_midi_write_packed` entry (`0x600A3238`) | `int f(uint32_t packet)` | each USB MIDI packet out; take it to drop it |
| `ev_song_load` | song load entry (`0x60062154`) | `void f(void *songs, const char *path)` | before the file is read |
| `ev_song_save` | song save entry (`0x6007AFC0`) | `void f(void *songs, const char *path)` | before the file is written |
| `ev_note` | ITCM note-on (run `0x22BDC`) | `int f(void *player, int instrument)` | audio thread; once per note; players are `0x2001ADF0 + 0x248 × track`; take it to drop the note |
| `ev_render_in` | audio interrupt entry (IRQ 70 vector) | `void f(void)` | audio thread, about 740 a second |
| `ev_render_out` | audio interrupt exit | `void f(void)` | audio thread |
| `ev_crash` | core's handler in front of Teensyduino's fault handler (RAM vectors 3-6) (0.5) | `void f(const uint32_t *frame)` | in the fault: r0-r3, r12, lr, pc, xpsr; no SD, no USB; Teensyduino then records and resets |
| `ev_card_ready` | core_load, after the mods load (0.5) | `void f(int mounted)` | before the firmware first uses the card; before `ev_boot` |
| `ev_usb` | `ev_tick`, when `usb_configuration` (`0x2004159F`) changes (0.5) | `void f(int configured)` | |
| `ev_sysex` | usbMIDI's SysEx handler pointer (`0x20040654`), chained at boot (0.5) | `int f(const uint8_t *data, int len, int last)` | take it to keep the firmware's handler from seeing the chunk |
| `ev_audio_out` | the master soft clip's `update()` (vtable slot in RAM, engine `0x2000BE80` + `0x9750`; Model:02 `0x2001DA34`), on its input (0.5; moved in 0.6) | `int f(float *left, float *right, int frames)` | audio thread, ~690 a second, 64 float frames (Model:02: 32). Process in place (insert) or write and take (exclusive); a silent output gets zeroed blocks. The soft clip feeds both USB (`+0x9308`) and the I2S branch (headphones, speaker); wrapping `+0x9308`, as 0.5 did, reached USB only |
| `ev_seq_tick` | the tick processor, ITCM run `0x21A64` (0.5) | `int f(void *engine)` | audio thread, 192 a beat |
| `ev_row` | the sequencer's row fire, inside run `0x24650` at `0x24B7C` (0.5, `itcm_mid`) | `void f(void *player)` | audio thread; the 9-byte row at `player + 0x4C`, before its note and FX |
| `ev_note_off` | note off, ITCM run `0x22AC0` (0.5) | `int f(void *player, int kill)` | audio thread; take it to keep the note sounding |
| `ev_fx_row` | the phrase FX applier, ITCM run `0x22DE4` (0.5) | `int f(void *player, int column, int cmd, int value)` | audio thread, once per FX column per row; take it to replace the stock effect |
| `ev_fx_command` | the table FX executor, ITCM run `0x258EC` (0.5) | `int f(void *runner, int column, int cmd, int value)` | audio thread |
| `ev_mod_apply` | each engine's modulation-destination switch (7 `itcm_mid` sites) (0.5) | `int f(int dest, void *modulator, void *target, int engine)` | audio thread; `engine` is `FW_ENGINE_*`; take it to skip the stock destination |
| `ev_mute` | the routine every mute and solo applies through, `0x60038940` (0.5) | `int f(void *mixer, int track)` | once per track; masks `fw_mute_mask` / `fw_solo_mask` |
| `ev_inst_load` / `ev_inst_save` | `0x6007BDD0` / `0x6007C47C` (0.5) | `int f(void *songs, const char *path, int slot)` | take it to refuse |
| `ev_param_set` | the instrument view's field-changed method, `0x60053B30` (0.5) | `void f(void *instview, int field, int old)` | |
| `ev_step_edit` | the phrase view's cell-edited method, `0x6006025C`, and its bulk redraw (RAM slot `0x2000173C`) (0.5) | `void f(void *view, uint8_t *cell, int n, int column)` | a cell (`column` 0-8), or a bulk edit: the first changed step, how many steps to the last changed, `column` -1 |
| `ev_inst_copy` / `ev_inst_paste` | clone `0x6005524C`, paste `0x60054894` (0.5) | `int f(void *instview, int src, int dst)` / `int f(void *instview, int action)` | take it to refuse |
| `ev_live_note` | the live / preview trigger, ITCM run `0x23374` (0.5) | `int f(void *player, int instrument, int note, int velocity)` | notes from the keys, not the sequencer; take it to drop the note |
| `ev_inst_clear` | the slot clear, `0x6007FF34` (0.5) | `int f(void *song, int slot, int full)` | the pool's delete; take it to keep the slot |
| `ev_track_audio` | the track voice class's `update()` (RAM slot `0x20002348`) (0.5) | `void f(float *left, float *right, int track)` | audio thread; after the track renders, before its strip and sends: change the blocks in place |
| `ev_fx_audio` | MFX, delay and reverb `update()` (RAM slots) (0.5) | `int f(int stage, float *left, float *right)` | audio thread; the effect's input before it runs (`FW_FX_*`; the delay is mono, `right` 0). Take MFX or reverb to replace it: your blocks go to its outputs |
| `ev_render_params` | the sequencer clock's `update()` (RAM slot `0x20001E94`) (0.5) | `void f(void *engine)` | audio thread; first in each block |

`itcm_mid` sites (core 0.5) sit inside a function, where registers, flags and s0-s15 are live. The stub keeps
them all and returns through a stack slot. It takes the handler's arguments from any register (or a constant),
and a taken event jumps to the function's own epilogue, which the builder checks against the code.

`ev_note` is the first ITCM site: the patched instruction branches through a veneer in ITCM (`ldr.w pc,
[pc]`, 8 bytes at `0x2FF80..`) to its flash stub, which returns to ITCM with `movw`/`movt ip` and `bx ip`. The
render events need no code site: core wraps the audio interrupt's entry in the RAM vector table at boot.

Mods' code runs from ITCM, so audio handlers are fast without a `.itcm` section; the Model:02 notes below
about copying `.itcm` apply to built-in (linked) mods only.

## Conventions

- **Handlers are C functions** (or Thumb assembly) in the ARM procedure-call standard with hard float: integer
  arguments in r0–r3, then the stack; floats in s0–s3; the result in r0 (s0 for a float); r4–r11, s16–s31 and
  `lr` preserved. The stack is the interrupted thread's: keep frames small.
- **`order`** sorts the handlers of one event, lower first; ties go by mod id. Shipped mods use 10–90.
  For the audio events: **10–49 make sound, 50–89 change sound already made** (as `ev_render_voices` on the
  Digitakt).
- **"Take it"**: for events whose prototype returns `int`, a nonzero return stops the event: no later handler
  sees it, and the stock code does not run. A `void` event always reaches every handler.
- **Two threads.** *UI* events run in `loop()` (the main thread). *Audio* events run inside **IRQ 70**, the
  Teensy Audio update interrupt, once per audio block (~1,378 a second at 44.1 kHz with 32-sample blocks):
  keep them short, never block, never call UI code. UI code that touches audio state brackets it with
  `fw_audio_lock()` / `fw_audio_unlock()`.
- **Audio handlers live in ITCM.** The audio thread is ITCM code (run `0x00000000..`), and from there a flash
  routine is out of `bl` range and slow. Put audio-event handlers in the **`.itcm` section**: the core copies it
  into free ITCM (≈18 KB above `0x2B8D0`, shared by all mods) at `ev_boot`, before the first audio block.
  UI-event handlers go in ordinary flash code.

## The events

### Shared with Elekloader (same names and meaning)

| event | when | C prototype | notes | status |
|---|---|---|---|---|
| `ev_boot` | once, after `setup()`, before the first `loop()`; UI thread | `void f(void)` | the core has copied `.itcm` and zeroed mods' `.bss` already | site pinned |
| `ev_tick` | every `loop()` iteration; UI thread | `void f(void)` | free-running, thousands a second (not 30 like the Digitakt's): do almost nothing, or count | **implemented** |
| `ev_draw` | each UI frame (~66 a second), after the views have drawn and **before the frame is pushed** to the display | `void f(void *ui)` | draw over the frame with `fw_text` and friends; what you draw lasts one frame | site pinned |
| `ev_key` | each input action, before the current view handles it; UI thread | `int f(void *ui, int action)` | `action` is the M8's input-action code (each view's `+0x08` handler switches on it). `0xE4`, all four directions, toggles the Model:02's visualizer (`ui + 0`); headless has no visualizer ([CHAOS-CONTROL.md](CHAOS-CONTROL.md)). Return nonzero to take it | site pinned |
| `ev_render_in` | audio block entry, before any node renders; audio thread | `void f(void)` | interrupt level | site pinned |
| `ev_render_out` | audio block exit, after every node; audio thread | `void f(void)` | interrupt level | site pinned |
| `ev_midi` | each MIDI message in | `int f(const unsigned char *msg, int port)` | return nonzero to take it. Headless: `int f(uint32_t packet)`, a USB MIDI packet, with `ev_midi_out` for what goes out | implemented (headless) |
| `ev_settings` | the SYSTEM SETTINGS screen is built | `void f(void *menu)` | the M8's settings screen is a view, not a menu: needs its row model first | design only |
| `ev_serial` | the m8c serial parser runs (headless `0x6007846C` entry) | `int f(void *m8c)` | nonzero takes the call: the parser does not run. Core 0.3 uses it for mod uploads (requests start with `0xD1`, [LOADER.md](LOADER.md)) | implemented (headless) |
| `ev_song_load`, `ev_song_save` | a song file is loaded or saved | `void f(void *songs, const char *path)` | TELOS's project events; core 0.3 keeps each song's mod list with them | implemented (headless) |

There is no `ev_enc`: the M8 has no encoders. Its continuous control is the touch screen (`ev_touch`).

### The M8's own events

| event | when | C prototype | notes | status |
|---|---|---|---|---|
| `ev_view` | before the screen changes (`set_view`); UI thread | `int f(void *ui, int view)` | `view` is the target id (`0x00` SONG … `0x1D`, [FIRMWARE-MAP.md](FIRMWARE-MAP.md) §8); the current one is `*(uint8_t *)(ui + 0x15)`. Return nonzero to stay on the current screen. Ids from `0x1E` are mods' screens (`core_views`, below) | site pinned |
| `ev_touch` | each loop, after the touch engine polled the controller and before it asks for a gesture; UI thread | `int f(void *touch)` | the touch object (`fw_touch`, m8-model02/fw.h): x `+0x40` (from the left), y `+0x44` (from the **bottom**), both 0..1; `+0x58` nonzero while a finger is down, though it reads 0 for a moment now and then (debounce a lift). Return nonzero to take the frame: no view sees it (CHAOS CONTROL) | Model:02 6.6.2A: `call_pre` on the `bl 0x6007BB20` at `0x600A5E88`, run on a unit; headless has no touch screen |
| `ev_vis_scope` | the Model:02's visualizer is about to draw one track's scope (8 a frame); UI thread | `int f(void *scope)` | take it to skip that scope. The stock scopes take 11–21 ms a frame with every track playing; CHAOS CONTROL's FXPAD skips them and draws its own pad (3.8–5.4 ms a frame) | Model:02 6.6.2A: a `calls` site on the `bl 0x6004A64A` at `0x6004B606` (core 0.7) |
| `ev_vis_draw` | each frame of the Model:02's visualizer (all four directions), after its eight scopes are drawn and before the frame is pushed; UI thread | `void f(void *grid)` | the visualizer's frame path never reaches `ev_draw`'s site. Draw with `fw_rect(grid, x, y, w, h, rgb565)` on the 480 × 320 screen. The scopes repaint only the pixels that change and nothing clears the frame, so what you draw stays until you erase it; on leaving, the firmware repaints only text cells that change, so erase in `ev_draw` (CHAOS CONTROL) | Model:02 6.6.2A: `flash_mid` at `0x6004B620` (core 0.6), run on a unit |
| `ev_note` | a row fires on a track, before its note and FX are applied; audio thread | `int f(int track, const unsigned char *step)` | `step` is the phrase step (9 bytes: note, velocity, instrument, 3 × (command, value)); the handler may read it, not write it. Return nonzero to skip the row | site pinned |
| `ev_fx_command` | an FX command runs on a track (the per-tick executor); audio thread | `int f(int track, int cmd, int value)` | `cmd` `0x00..0x4D` are the global commands, `0x80 \| n` an instrument's. Return nonzero to take it: the stock executor does not run it — the way a mod adds a command or changes one | site pinned (argument mapping to confirm) |
| `ev_fx_row` | a row's FX column is applied at row fire; audio thread | `int f(int track, int column, int cmd, int value)` | `column` 0–2; take it to replace that column's stock effect | site pinned |
| `ev_tempo` | the tempo is set; UI thread | `int f(float bpm)` | `bpm` in s0. Return nonzero to refuse the change | site pinned |
| `ev_transport` | play, pause or stop; UI thread | `int f(int state)` | `state` 1 play, 0 pause, −1 stop. Return nonzero to refuse it. Headless has it as two events, `ev_play` and `ev_stop` | site pinned |
| `ev_mod_apply` | a modulator targets a destination past the engine's stock list; audio thread | `void f(int engine, void *voice, void *mod, int dest)` | the default branch of each engine's `switch(dest)` ([hooks/static-m8-params.md](hooks/static-m8-params.md)); with `core_mod_dests` this is how mods add LFO destinations | site pinned (all 6 audio engines; SAMPLER has two copies) |

## Core's tables

As on the Elektron side (`core_machines`, the Mod Menu), mods add **entries to core's tables**; the linker
places them and refuses clashes by `resources.names`.

| table | entry | what core does with it | claim |
|---|---|---|---|
| `core_views` | `{ int id; void *view; const char *name; }` — `view` an object whose first word is a `View` vtable (8 slots: enter, …, input `+0x08`, …, leave `+0x14`) | `set_view(id)` for `id >= 0x1E` switches to it like a stock screen: leave the old view, set `ui+4`, enter the new one; touch and keys reach it through its own vtable | `view:<name>` |
| `core_mod_dests` | `{ uint8_t engines; const char *name; }` — `engines` a bit per engine (WAVSYNTH, MACROSYNTH, SAMPLER, FMSYNTH, HYPERSYNTH, EXTERNAL, MIDI OUT) | extends each engine's destination count (`vt+0x104`) and name table (`vt+0x0F8`) with the entries, in link order; their indices then reach `ev_mod_apply` | `moddest:<name>` |

The extended-LFO-destinations mod is [examples/lfo-sends](../examples/lfo-sends/lfo_sends.c). It adds DRY, MFX
SEND, DEL SEND and REV SEND to every sound engine's DEST list with `mod_dest_add`. A modulator keeps its destination
in 4 bits and the stock lists use 14 or 15 of them, so this relies on core 0.6's extended destinations: core keeps
the real index with the song and the instrument, and `mod_dest_of` gives it in `ev_mod_apply`.
`ev_track_audio` then sets the track's strip gains (`fw_gain_set`, m8-headless/fw.h) on the firmware's curve.
[MOD-DESTS.md](MOD-DESTS.md) has the findings, the six `flash_mid` sites and the hardware results.
The KAOSS mod is [examples/chaos-control](../examples/chaos-control/chaos.c), CHAOS CONTROL
([CHAOS-CONTROL.md](CHAOS-CONTROL.md)): a page of its own, `ev_audio_out` for the sound, and on the Model:02 `ev_touch`
for the pad and `ev_vis_draw` for its overlay on the visualizer.

## Firmware locations (6.6.2A)

Named exports, as Elekloader core 3.0's `fw_*`, so a mod that names only these rebuilds for another firmware
unchanged. The C declarations are in `dirtyloader/sdk/include/m8-model02/core.h`.

| name | 6.6.2A | what |
|---|---|---|
| `fw_song` | `0x2025A570` | the song (OCRAM); `FW_PHRASE(p, s)`, `FW_CHAIN(c, s)`, `FW_INSTRUMENT(i)`, `FW_EQ(n)` give its parts ([hooks/static-m8-song.md](hooks/static-m8-song.md)) |
| `fw_clock` | `0x2000D218` | the sequencer clock: `+0x28` running, `+0x29` paused, `+0x2C` ticks, `+0x30` bpm (float) |
| `fw_track_players` | `0x2000D270` | 8 × `0x2B0` per-track sequencer state |
| `fw_ui` | `0x20015458` | the UI context: `+0x04` current view object, `+0x15` current view id, `+0x17` previous |
| `fw_touch` | `0x200177D4` | the touch state: `+0x30` dx, `+0x34` dy, `+0x40` x, `+0x44` y (floats) |
| `fw_instruments` | 8 singletons | SAMPLER `0x2000EAA0`, WAVSYNTH `0x20037DB8`, MACROSYNTH `0x20038DE4`, MIDI OUT `0x2003AA94`, FMSYNTH `0x2003B97C`, HYPERSYNTH `0x2003DAAC`, EXTERNAL `0x2003EC88`, NONE `0x20014064` |
| `fw_send_mixer(t, bus)` | `0x20010F50 + 0x38·(4t + bus)` | the per-track send mixers (`AudioMixer4`-style), 32 nodes |
| `fw_set_view` | `0x6004C13C` | switch screens |
| `fw_set_tempo`, `fw_set_playing`, `fw_stop` | `0x6009A6C0`, `0x6009A528`, `0x6009A55C` | transport |
| `fw_text` | `0x60030ECC` | draw a string (arguments to confirm) |
| `fw_audio_lock` / `fw_audio_unlock` | NVIC `ICER2` / `ISER2`, bit `0x40` | `AudioNoInterrupts()` / `AudioInterrupts()` |

## The sites core owns (6.6.2A) — do not patch them

| event | site | kind |
|---|---|---|
| `ev_boot` | `0x600AE04E` `bl setup` | flash: `bl` detour |
| `ev_tick` | `0x600AE052` `bl loop` | flash: `bl` detour (core 0.1) |
| `ev_touch` | `0x60081C32` `bl` touch engine | flash: `bl` detour |
| `ev_key` | `0x60081C56` `bl` input router | flash: `bl` detour |
| `ev_draw` | `0x6004B6DA..0x6004B6E1` frame push (`ldr.w r3,[r3,#0xA0]; blx r3`) | flash: replace both |
| `ev_view` | `0x6004C13C` `set_view` entry | flash: entry detour |
| `ev_tempo` | `0x6009A6C0` entry | flash: entry detour |
| `ev_transport` | `0x6009A528`, `0x6009A55C` entries | flash: entry detour |
| `ev_render_in` / `ev_render_out` | run `0x2929C` entry / `0x292EE` exit of `software_isr` | ITCM: long veneer |
| `ev_note` | run `0x24DE0` row fire | ITCM: long veneer |
| `ev_fx_row` | run `0x24854` | ITCM: long veneer |
| `ev_fx_command` | run `0x27124` executor | ITCM: long veneer |
| `ev_mod_apply` | the default branch of the apply switch, run `0x0C9D0` FM, `0x0E552` HYPER, `0x1185A` MACRO, `0x1439E` SAMPLER, `0x171BA` WAV | ITCM: retarget `bhi` |
| core region | `0x60000200..0x60001000` | dispatcher, table, stubs |

*Flash* sites use the Core's existing `bl`/entry detour into the cave at `0x60000200`. *ITCM* sites run at their
run address and cannot reach that cave: the core places their stubs in free ITCM and patches each site with the
linker's long-branch form (`ldr.w pc,[pc,#-0]` + literal) — [CORE.md](CORE.md), "ITCM hook sites".

## How core 0.2 dispatches

Core 0.1 calls each TICK handler with the event id in r0. Core 0.2 keeps the table (`event:u16, flags:u16,
handler:u32`, 64 entries, `order`-sorted by the linker) and gives each site a **stub** that:

1. saves the site's argument registers and builds the event's arguments from them;
2. calls `core_dispatch(event, a0, a1, a2, a3)`, which calls each enabled handler for that event in table
   order with those arguments and, for an `int` event, stops at the first nonzero return;
3. if the event was taken, returns past the stock code with the stock's "nothing done" result; otherwise runs
   the displaced instructions and continues into the stock code.

The audio-thread stubs and their dispatcher copy live in ITCM; the UI ones in the flash cave.

## Mapping to Elekloader

| Elekloader | M8 | |
|---|---|---|
| `ev_tick`, `ev_draw`, `ev_key`, `ev_render_in`, `ev_render_out`, `ev_settings` | same names | shared |
| `ev_enc` | — | no encoders; continuous input is `ev_touch` |
| `ev_voice_on` (Digitone) | `ev_note` | a note starts on a track; the M8 has no per-voice event |
| TELOS project mod lists (Digitone) | `ev_song_load` / `ev_song_save` | the M8's unit of work is the song |
| `ev_midi_cc` (Digitone), `ev_midi` (Octatrack) | `ev_midi`, `ev_midi_out` | headless: USB MIDI packets |
| `ev_hold` (Digitone) | — | the M8 has no Mod Menu yet; `core_views` is its screen hook |
| `core_machines` | `core_mod_dests`, `core_views` | the M8's extension tables |
| `fw_*` (core 3.0) | `fw_*` | same idea |
| `.fast` (fast SRAM) | `.itcm` | the M8's fast code area, for the audio thread |

## Building toward core 0.2

1. Generalise `dirtyloader.core.builder`: one stub per event from a site table (flash `bl`, flash entry, ITCM
   long veneer), the argument-passing dispatcher, and the `.itcm` copy at boot.
2. `mkmod`: `subscribe` by `ev_*` name (the loader accepts `ev_tick` and `TICK` alike) and an `.itcm` section.
3. Validate on the Teensy, event by event, starting with the UI ones; then the ITCM ones.
