# dirtyloader

[![tests](https://github.com/irpina/dirtyloader/actions/workflows/tests.yml/badge.svg)](https://github.com/irpina/dirtyloader/actions/workflows/tests.yml)
[![pages](https://github.com/irpina/dirtyloader/actions/workflows/pages.yml/badge.svg)](https://github.com/irpina/dirtyloader/actions/workflows/pages.yml)

**Mods for the Dirtywave M8**, in the manner of Elekloader. dirtyloader adds a small **core** to Dirtywave's own M8
firmware. Core loads **mods** from the SD card at boot: small C programs that hook into the M8 (keys, screens,
notes, audio, MIDI), get pages and settings of their own, and switch on and off without reflashing.

It never redistributes Dirtywave's firmware. You bring the firmware file Dirtywave publishes, and dirtyloader adds
core to it on your own computer, in the browser or from the command line.

Core 0.7 runs on the **M8 Headless 6.6.4A**, the **M8 Model:02 6.6.4A** (Dirtywave's current download) and the
**Model:02 6.6.2A**. The in-progress [Model:01 6.6.4A port](docs/MODEL01-664-PORT.md) uses a command-line
firmware build, manual SD installation, and a text MOD LOADER. Track Silence works on Model:01 hardware.
The [Model:01 hook list](docs/CORE-EVENTS.md#model01-664a) records the current coverage.
**Read the [warning](#warning) first.**

## Get started: the mod shop

**→ https://irpina.github.io/dirtyloader/shop/** (Chrome, Edge or Opera on a computer: it uses Web Serial and WebHID)

1. **Get started** builds core into Dirtywave's firmware file in your browser (the file never leaves your computer)
   and flashes it to the M8 over USB.
2. **Mods** installs mods onto the M8's card over USB and switches them on and off.
3. **Theme** picks the M8's colours, live on the device.
4. **Recover** restarts the M8 without mods, or puts Dirtywave's firmware back.

For Model:01, follow the [command-line and SD instructions](docs/MODEL01-664-PORT.md#current-build-and-installation).
How the shop works: [docs/SHOP.md](docs/SHOP.md).

On the M8 itself: on a **Model:02**, all four directions at once (UP+DOWN+LEFT+RIGHT) open the **MOD LOADER**, a card
for each mod; on a **Headless**, LEFT+RIGHT opens the mods' pages. SYSTEM SETTINGS has a **MODS** row on both.

## Write a mod

**→ [docs/WRITING-MODS.md](docs/WRITING-MODS.md)** takes you from nothing to a mod running on your M8. The short
version, with Python 3.10+ and `arm-none-eabi-gcc` (in WSL on Windows):

```bash
python -m dirtyloader.cc build examples/hello                      # -> build/mods/<target>/hello.m8x, every firmware
python -m dirtyloader.unit add build/mods/m8-model02-664/hello.m8x # onto the M8's card, over USB
python -m dirtyloader.unit restart
python -m dirtyloader.unit status                                  # hello.m8x: loaded hello 0.1.0 ..., 4 hooks
```

A mod is one or more C files:

```c
#include "dl/mod.h"
#include "m8-headless/fw.h"

static void on_tick(void) { /* every pass of the firmware's main loop */ }

DL_MOD("my-mod", "0.1.0", "My mod", DL_ON(EV_TICK, 50, on_tick));
```

- [`examples/hello`](examples/hello/): the starter: a page, a setting, a few events.
- [`examples/`](examples/): the shop's mods, from small (`sd-heartbeat`, `track-silence`) to large (`chaos-control`:
  audio effects, the touch screen and pixel drawing on the Model:02).
- [docs/CORE-EVENTS.md](docs/CORE-EVENTS.md): every event, with its prototype and thread.
- [`dl/core.h`](dirtyloader/sdk/include/dl/core.h): core's API: pages, settings, song data, files, MIDI, buffers.
- [AGENTS.md](AGENTS.md): notes for coding agents.

All the docs: **[docs/README.md](docs/README.md)**.

## Why this is possible

The M8's `.bin` is a Dirtywave container (`M8FW`) wrapping a flat Teensy 4.1 flash image. It is **unencrypted,
unsigned ARM-GCC code**, protected only by two ordinary CRC-32s. With no cryptographic signature, a patched payload
with recomputed CRCs passes the M8's own check. Core is added to that image as a patch: new code where the image
has none, and short detours at the places it hooks. A mod then needs no patching at all: core copies it from the
card into free RAM at boot and calls it.

### M8FW container format (reverse-engineered from 6.6.2 / Model02)

| Offset | Size | Field | Notes |
|-------:|-----:|-------|-------|
| `0x00` | 4 | magic | `"M8FW"` |
| `0x04` | 4 | version | u32 LE; high word = model (`0x0002` → Model02) |
| `0x08` | 4 | payload_len | u32 LE, == `len(payload)` |
| `0x0C` | 4 | payload_crc32 | `zlib.crc32(payload)` |
| `0x10` | 4 | load_addr | `0x60000000` (Teensy 4.1 FlexSPI flash base) |
| `0x14` | 4 | header_crc32 | `zlib.crc32(header[0x00:0x14])` |
| `0x18`..`0x1FF` | — | zero padding | |
| `0x200` | N | payload | flat IMXRT1062 flash image mapped at load_addr |

Both CRCs are standard zlib CRC-32 (poly `0xEDB88320`). Repacking means patching the payload, recomputing both
CRCs and concatenating. `dirtyloader/m8fw.py` does exactly this, and a no-op rebuild is byte-identical to the stock
file. The Headless firmware is a Teensy `.hex` instead.

## Building core from the command line

The shop builds core in the browser from patch files (`site/shop/firmware/*.json`, no Dirtywave bytes in them). To
build it yourself from Dirtywave's file:

```bash
python -m dirtyloader.core.build_model02  M8_V6_6_4A_MODEL02.bin out   # Model:02 (6.6.2A or 6.6.4A, by the file's sha256)
python -m dirtyloader.core.build_headless M8_V6_6_4A_HEADLESS.hex out  # Headless
```

For Model:01 6.6.4A, use the [Model:01 builder](docs/MODEL01-664-PORT.md#current-build-and-installation).

Each writes the firmware with core in it, and an `sd/` folder with a mod to try. Flash it the usual way: copy the
`.bin` to the M8's SD card and use SYSTEM SETTINGS > FIRMWARE UPDATE, or flash the `.hex` with TyUploader. The shop's
Get started page does both over USB.

## The older `.m8mod` patches

Before core loaded mods from the card, dirtyloader applied **`.m8mod`** patches to the firmware file: Thumb assembly
written into the image, each change checked against a sha256 of the stock bytes it replaces. That path still works
(and its in-browser builder is at **https://irpina.github.io/dirtyloader/**), though new mods are written in C for
core. The format: [docs/FORMAT.md](docs/FORMAT.md).

```bash
python -m dirtyloader info   M8_V6_6_2A_MODEL02.bin                 # inspect / verify a firmware .bin
python -m dirtyloader verify M8_V6_6_2A_MODEL02.bin
python -m dirtyloader mkmod  examples/hello-tick --stock M8_V6_6_2A_MODEL02.bin -o hello-tick.m8mod
python -m dirtyloader lint   hello-tick.m8mod --stock M8_V6_6_2A_MODEL02.bin
python -m dirtyloader build  M8_V6_6_2A_MODEL02.bin -m hello-tick.m8mod -o M8_patched.bin
python -m dirtyloader make-mod stock.bin my-edited.bin -o my-mod.m8mod --id my-mod --name "My mod"
```

## Project layout

```
dirtyloader/
  core/            core: c/core03.c (one C file for every firmware), bindings/*.json (its hook sites per firmware),
                   build_model02.py / build_headless.py
  sdk/include/     the mod SDK: dl/mod.h, dl/core.h, dl/pixui.h, and each firmware's fw.h
  sdk/lib/         the little C library mods link with
  cc.py            the C build: python -m dirtyloader.cc build | info | targets
  unit.py          USB: put mods on the card, switch them, status, restart, safe boot
  m8screen.py      screenshots of the M8 over USB
  m8fw.py          the M8FW container: parse / verify / patch / rebuild
  re/              reverse-engineering tools (memory map, layout, vtables, Ghidra glue, port maps)
  m8mod.py, linker.py, mkmod.py, lint.py, cli.py, web.py    the older .m8mod path
examples/          mods (C), and hello-tick (the older .m8mod format)
site/shop/         the mod shop (GitHub Pages): make_shop.py builds its mods, make_firmware.py its firmware patches
site/              the older in-browser .m8mod builder
tests/             tests that need no firmware (CI): Python and the shop's JavaScript
docs/              everything else: docs/README.md
```

## Tests

```bash
python -m unittest discover -s tests -p "test_*.py"   # no firmware or compiler needed; runs in CI
node tests/test_shop.mjs                              # the shop, against a pretend M8
node tests/test_firmware.mjs                          # the in-browser firmware build and USB flasher
```

Some tests also build from Dirtywave's firmware when they find it on your computer (never in the repository).

## Warning

- **Unofficial, and not affiliated with Dirtywave.** Changed firmware is at your own risk: it may crash the M8, and
  Dirtywave may not support a unit running it.
- dirtyloader **does not distribute firmware.** You supply Dirtywave's file.
- The Teensy's bootloader is never touched, so putting Dirtywave's firmware back always recovers an M8 that still
  powers up: the shop's **Recover** page, the SD card's FIRMWARE UPDATE, or TyUploader.
- A mod that crashes the M8 is caught by core's crash guard: the next boot loads no mods
  ([docs/WRITING-MODS.md](docs/WRITING-MODS.md#10-installing-testing-debugging)).
