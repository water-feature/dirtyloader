[dirtyloader](../README.md) › docs

# dirtyloader docs

## Using it

- **[The mod shop](https://irpina.github.io/dirtyloader/shop/)**: put core on your M8 (Get started), install mods,
  pick the theme, recover. How it works: [SHOP.md](SHOP.md).
- **[Model:01 6.6.4A](MODEL01-664-PORT.md)**: command-line firmware build, SD installation, and hardware results.

## Writing mods

- **[WRITING-MODS.md](WRITING-MODS.md)**: start here. From nothing to a mod running on your M8, with
  [`examples/hello`](../examples/hello/).
- [CORE-EVENTS.md](CORE-EVENTS.md): every event a mod can handle, its prototype, thread and where it hooks the
  firmware.
- [LOADER.md](LOADER.md): how core loads mods from the SD card, the `.m8x` format, the core API, the crash guard,
  the MODS page, MOD LOADER and Mod Menu, USB transfer.
- [MOD-DESTS.md](MOD-DESTS.md): adding modulation destinations (LFO targets).
- Mods with their own write-ups: [CHAOS-CONTROL.md](CHAOS-CONTROL.md) (an X/Y effects pad: audio, touch, drawing
  on the Model:02), [PERF-MON.md](PERF-MON.md) (measuring audio and UI load).

## How it works inside

- [CORE.md](CORE.md): core's design: the hook bus, detours, how it fits into the firmware.
- [FIRMWARE-MAP.md](FIRMWARE-MAP.md): the firmware mapped: memory, threads, drivers, classes.
- [MODEL02-664-PORT.md](MODEL02-664-PORT.md), [MODEL02-PORT.md](MODEL02-PORT.md): moving core to another firmware
  version, and how each address was found again.
- [PARITY-DN1.md](PARITY-DN1.md): core against Elekloader's Digitone core, feature by feature.
- [FORMAT.md](FORMAT.md), [MODULATION.md](MODULATION.md): the older `.m8mod` format (binary patches applied to the
  firmware file) and its modulation patches.
