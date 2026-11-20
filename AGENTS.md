# Notes for coding agents

dirtyloader adds **core** to Dirtywave's M8 firmware, built on the user's side from the firmware file they supply,
and core loads **mods** (C compiled to `.m8x`) from the SD card. Read [README.md](README.md) first; the docs start
at [docs/README.md](docs/README.md).

## If you are writing a mod

Follow [docs/WRITING-MODS.md](docs/WRITING-MODS.md): the rules (section 9), the refusals and their fixes (section
11), and what done means (section 13). Start from [`examples/hello`](examples/hello/). Your loop:

```bash
python -m dirtyloader.cc build <moddir>                       # every target, into build/mods/<target>/<id>.m8x
python -m dirtyloader.unit add build/mods/<target>/<id>.m8x   # onto the M8's card, switched on
python -m dirtyloader.unit restart
python -m dirtyloader.unit status                             # loaded? hooks? a crash?
```

On Model:01, use USB DRIVE to copy the mod to `dirtyloader/mods/`.
Enable it in MOD LOADER and read `dirtyloader/status.txt` after reboot.
See [Model:01 setup](docs/MODEL01-664-PORT.md#current-build-and-installation).

`python -m dirtyloader.cc info <file.m8x>` checks a file as core will. `python -m dirtyloader.m8screen shot.png`
screenshots the M8 (`--keys` presses keys first). Events and prototypes: [docs/CORE-EVENTS.md](docs/CORE-EVENTS.md);
the API: `dirtyloader/sdk/include/dl/core.h`.

## If you are changing dirtyloader itself

| path | what |
|---|---|
| `dirtyloader/core/c/core03.c` | core (all firmwares, one C file); `core/bindings/*.json` its hook sites per firmware |
| `dirtyloader/sdk/include/` | the mod SDK: `dl/mod.h`, `dl/core.h`, `dl/pixui.h`, and each firmware's `fw.h` |
| `dirtyloader/cc.py` | the C build (`build_m8x`, the `build` / `info` / `targets` command) |
| `dirtyloader/unit.py`, `serialport.py`, `m8screen.py` | USB: transfer, status, screenshots |
| `site/shop/` | the mod shop (Pages); `make_shop.py` builds the mods' `.m8x` and `catalog.json`, `make_firmware.py` the firmware patches |
| `examples/` | the mods; `mods/core/` the older `.m8mod` core |
| `tests/` | `python -m unittest discover -s tests -p "test_*.py"`, `node tests/test_shop.mjs`, `node tests/test_firmware.mjs` |

**After any change to core** (`core03.c`, a binding, an `fw.h`): run `python site/shop/make_firmware.py` and
commit the patches it writes. CI can't rebuild them: it has no Dirtywave firmware. **After any change to a mod in
the shop:** `python site/shop/make_shop.py` and commit the `.m8x` files and `catalog.json`.

## Never

- Commit or upload Dirtywave's firmware, or anything holding its bytes (`.bin`, `.hex`, the manual). The patches
  in `site/shop/firmware/` hold only put/copy operations and hashes; `make_firmware.py` checks that.
- Touch the Teensy's pins in a Model:02 build without checking the pad is free (it hung a Model:02).
- Flash a Model:02 build onto a bare Teensy 4.1, or the other way round.
