# Model:01 6.6.4A

Dirtyloader runs on a Model:01 with the matching stock 6.6.4A firmware. It loads mods from the SD card and exposes BOOT, TICK, KEY, TOUCH, TRACK_AUDIO, and AUDIO_OUT. See the [hook reference](CORE-EVENTS.md#model01-664a) for details. Model:01 uses manual SD installation and a text MOD LOADER. The SDK's shared SD write accessor is available to mods. The full mod API is available on Model:02 and Headless.

## Current build and installation

With the ARM toolchain installed, build from your copy of the stock Model:01 6.6.4A BIN:

```sh
python -m dirtyloader.core.build_model01 M8_V6_6_4A_MODEL01.bin build/model01 --menu
python -m dirtyloader.cc build examples/track-silence --target m8-model01
```

Copy the generated firmware BIN to the SD card root and install it through **SYSTEM SETTINGS > FIRMWARE UPDATE**. Keep the original stock files for recovery. Copy `build/mods/m8-model01/track-silence.m8x` to `dirtyloader/mods/` on the card. Create `dirtyloader/active.txt` if it does not exist. The build's sample `sd/` directory includes an example active list and tick mod.

Press all four directions to open **MOD LOADER**. UP/DOWN selects a mod, EDIT changes its saved enable setting, and OPT exits. Changes take effect on the next boot. Use USB DRIVE to read `dirtyloader/status.txt` and see which mods loaded.

If the guard is at `SKIP`, MOD LOADER shows **SAFE MODE - MODS NOT LOADED** and **PRESS PLAY TO EXIT SAFE MODE**. Press PLAY to clear the guard, then reboot manually when the footer says **REBOOT TO RE-ENABLE MODS**. You can also write exactly four bytes `GOOD` to `dirtyloader/guard` in USB DRIVE and eject to reboot. See [the crash guard](LOADER.md#crash-guard).

## Verification

| Feature | Result |
| --- | --- |
| SD loading, TICK, MOD LOADER toggles, and guard recovery | Verified on Model:01 hardware |
| Safe-mode recovery | Verified on Model:01 hardware. PLAY clears the guard and the device stays on until manually rebooted. |
| Track Silence | Track 1 muted on hardware. Longer playback and restoration after disabling remain to be verified. |
| Final panel layout and cursor handling | Passed ARM emulation. The safe-mode footer layout was verified on Model:01 hardware. |
| SD heartbeat | Passed ARM emulation. A fresh hardware run remains to be recorded. |
| TOUCH and AUDIO_OUT | Passed ARM emulation. Device verification is pending. |

To test Heartbeat, build `examples/sd-heartbeat` for Model:01, copy the mod to the card, remove any previous `dirtyloader/sd-heartbeat.txt`, and reboot with the mod enabled. The fresh file should read `SD heartbeat: BOOT ran`. Disable the mod and reboot after removing the marker to check that it stays absent.

To test master audio, build `examples/audio-tone` for Model:01 and lower the volume before rebooting with it enabled. Listen for the test tone, then disable the mod and reboot to confirm normal playback returns.

## Implementation notes

The builder verifies the exact stock image, appends the core on a 1 KiB boundary, and updates the ROM boot-data length and the M8FW container. The early hook sets the MPU permissions for the verified ITCM region before the SD loader runs. Each patched call site is checked against the stock firmware when building.

The probe builders in `dirtyloader.core.build_model01_probe` and `dirtyloader.core.build_model01_sd_probe` isolate the TICK and SD-loading paths. For build options, run either module with `--help`. The standard builder also accepts `--guard-diagnostics` to record why the guard skipped a boot.
