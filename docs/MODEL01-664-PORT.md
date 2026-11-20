# Model:01 6.6.4A bring-up

For the next test session, see the [firmware PR handoff](MODEL01-HANDOFF.md).

## Current scope and upstream readiness

The Model:01 branch has hardware-verified SD loading, tick execution,
guard clearing, and a text MOD LOADER that saves both enable and disable
changes. The build workflow covers Model:01, two Model:02 firmware
versions, and Headless. A mod that uses only common hooks and no stock
firmware addresses can already use the same binary across those targets.
The Model:01 menu build exposes BOOT, TICK, KEY, TOUCH, TRACK_AUDIO, and
AUDIO_OUT, with manual SD installation. Track Silence was confirmed
on hardware after clearing a pre-existing SKIP guard.

The work is local on `model01-664`. Track Silence met the first
cross-target milestone. Model:01 catalog coverage, remaining hooks and
the full mod API are still open. No upstream PR has been submitted.
Changes to the crash guard are deferred to a separate PR.

| Feature | Verification |
| --- | --- |
| SD loader, TICK, menu enable/disable, guard clearing | Verified on Model:01 |
| Track Silence (`TRACK_AUDIO`) | Owner confirmed track 1 muted after clearing a pre-existing `SKIP` and rebooting |
| SD heartbeat boot marker | ARM emulation passed. Fresh marker on hardware remains unconfirmed |
| Menu panel layout and cursor fix | ARM emulation passed. Physical display check remains open |
| Touch and master output (`TOUCH`, `AUDIO_OUT`) | ARM emulation passed with Touch Probe and Audio Tone. Hardware check remains open |

## Current build and installation

```sh
python -m dirtyloader.core.build_model01 M8_V6_6_4A_MODEL01.bin build/model01-touch-master \
    --menu --name M01-Touch-Master-Test.bin
python -m dirtyloader.cc build examples/track-silence
```

Copy the BIN to the SD card root and install it with **SYSTEM SETTINGS >
FIRMWARE UPDATE**. Copy the separate Model:01 file from
`build/mods/m8-model01/` into `dirtyloader/mods/` on the card.
On the M8, all four directions open **MOD LOADER**. UP/DOWN selects a
file, EDIT changes its saved enable setting, and OPT exits. The change
takes effect at the next boot. Read `dirtyloader/status.txt` over USB
DRIVE to see which mods actually loaded. The builder's generated
`sd/` folder contains the older tick probe and its sample active list.

If the status says the guard is at `SKIP`, use USB DRIVE to write
exactly the four bytes `GOOD` to `dirtyloader/guard`, then eject once
and let the Model:01 reboot automatically. Mods load immediately at
boot. An additional restart in the first ten seconds can mark the
preceding run as a possible hang and block mods on the next boot.
See [the crash guard](LOADER.md#crash-guard) for the full behavior.

## SD heartbeat hardware candidate

Build the same SD heartbeat source for Model:01, both Model:02 versions,
and Headless with one command.

```sh
python -m dirtyloader.cc build examples/sd-heartbeat
```

SD heartbeat writes `dirtyloader/sd-heartbeat.txt`
through the SDK's `dl_core_file_write()` from BOOT. Headless also keeps
the LED signal. The Model:01 core publishes only the SD write entry
through this accessor. The existing `dl_core()` accessor still requires
the full API table.

For a Model:01 hardware test, use the current firmware and copy
`build/mods/m8-model01/sd-heartbeat.m8x` to `dirtyloader/mods/`. Add
`sd-heartbeat.m8x` to `dirtyloader/active.txt`. If the guard is already
at `SKIP`, reset it to the four bytes `GOOD`. Remove any previous
heartbeat marker before testing. After ejecting USB DRIVE, allow the
automatic reboot at least ten seconds in normal mode before entering
the updater or rebooting again.

Install the candidate with the built-in updater. Confirm that playback,
navigation and the MOD LOADER work. Play a song for ten minutes and
check that saving and loading work. The fresh marker must contain
`SD heartbeat: BOOT ran`, and `dirtyloader/status.txt` must report
`sd-heartbeat 0.1.3` loaded with one hook. Disable it in MOD LOADER,
remove the marker and boot again. Verify that status omits the mod and
the marker stays absent. Re-enable and reboot once more to confirm the
marker returns.

ARM emulation verified the fresh marker with the enabled mod and no
marker when the mod was disabled, the guard skipped loading, or the
card was unmounted. Hardware validation is pending.

## Bring-up history

These notes record builds as they were tested. The filenames and
capability descriptions from older revisions reflect those builds.

The stock Model:01 BIN payload and TyUploader HEX decode to the same 2,016,256
flash bytes starting at `0x60000000`. A single port can emit both formats. Keep
the original HEX available for recovery via Dirtywave's TyUploader procedure.

The first probe detours the call to the stock main loop through the existing
TICK dispatcher with a no-op handler. It is intended to check whether the
dispatcher can run while the device continues to operate normally. The probe
does not include the SD loader or MOD LOADER UI.

```sh
python -m dirtyloader.core.build_model01_probe \
    M8_V6_6_4A_MODEL01.bin 'M8_V6_6_4A_MODEL01 (TyUploader).hex' build/model01
```

The probe builder uses Capstone to check the stock branch and Keystone to
assemble the dispatcher. It requires the exact stock 6.6.4A BIN and verifies
the supplied HEX covers the same flash image.

| Stock site | Model:01 address | Disassembly evidence |
| --- | --- | --- |
| Main | `0x600B0180` | Calls setup, loop and yield in order |
| Setup call | `0x600B0182` | `bl 0x60084C00` |
| Main-loop call | `0x600B0186` | `bl 0x6008460C` |
| Actual yield call | `0x600B018A` | `bl 0x600B7120` |
| Frame refresh call | `0x60085592` | `bl 0x6004D518` |
| Key routing call | `0x60085696` | `bl 0x6004E920` |
| Candidate loader call | `0x60084CF2` | `bl 0x6009ADE8`, verify setup order before using |
| End of stock ITCM copy | `0x0002B2A0` | Startup copy table recovered by `dirtyloader.re.memmap` |

The port mapper's strongest yield candidate was wrong. The main routine's
branch directly identifies the actual target. Apply the same instruction and
caller checks to the remaining candidates. The full port still needs a Model:01
SDK header and bindings, validation of RAM and flash reservations, verification
of screen geometry and firmware object layouts, and hardware tests.

To check the BIN and HEX round-trip and compare the modified ranges against
stock, run the firmware-backed test with both stock files in the same directory.

```sh
DIRTYLOADER_MODEL01_664_DIR=/path/to/Firmware/Model01 python -m unittest tests.test_model01_probe
```

The ten Model:02 C-hook call sites listed in the 6.6.4A binding also map
to Model:01 BL instructions whose decoded targets match their separately
mapped callees. That checks the MIDI read, visualizer scope and eight
side-column calls. It leaves the C calling conventions and object layouts
to verify. The full-screen UI also has hard-coded 320-pixel vertical extents
that need review for the Model:01 display.

## Hardware result (2026-11-20)

The owner installed the probe BIN through the Model:01's built-in updater
and confirmed that the unit booted. The USB display stream then reported
system info `ff0206060401` and returned rectangle and character frames.
The probe exercised the main-loop detour during normal display refresh.
The owner also confirmed normal on-device navigation and song playback.
The TyUploader HEX installation path is unverified.

## SD loading probe

The second build checks SD mod loading in isolation. It uses the Model:01
SdFat entry points and the mounted flag from the stock storage object.
The setup hook at `0x60084CF2` follows the card mount call at
`0x60084CE2` and precedes audio engine initialization. Open and close
use the verified file pointers at offsets `0x38` and `0x3C`. The first
1 KiB after the startup's stock ITCM copy holds the test mod, followed
by one word for its handler address.

```sh
python -m dirtyloader.core.build_model01_sd_probe M8_V6_6_4A_MODEL01.bin build/model01-sd-mpu
```

Copy the two generated files, `M01-664-SD-MPU.bin` and `M01-TICK.m8x`, to the
root of the SD card. Install the BIN from SYSTEM SETTINGS. On first boot
with the companion mod present, the loader checks its size and checksum,
relocates it into ITCM, and calls its TICK handler. When the handler
returns a count of 16, the loader writes `M01-TICK.txt` once. Check for a
fresh file with this content after boot.

```text
Model:01 SD probe: loaded and ran 16 ticks
```

The probe accepts the exact companion built with the firmware. It leaves
the full mod list, crash guard, and menu integration to core. Keep the
stock BIN on the SD card and the TyUploader HEX on the computer.

The first SD probe appended 448 bytes after the stock image. The M8 did
not reach normal operation after the update and was restored through
TyUploader with the official Model:01 HEX. The M8FW container length
included the new code, while the IVT boot-data length still described the
stock image. There was no flash readback to resolve whether the updater
wrote those extra bytes.

The revised build puts both trampolines and the loader in the erased
region used by the first probe. The payload length and boot data match
stock. ARM emulation with mocked exFAT calls exercised a successful
load, tick and status write, and verified that a truncated or changed
companion and an unmounted card produced no status write.

The revised build also failed to reach normal operation on the Model:01.
Keeping the code within the original image was insufficient, and neither
SD build established a successful hardware load. The official Model:01
6.6.4A HEX was restored through TyUploader, and USB display updates
confirmed that the unit was running again.

### Fault found in both SD probes

In the stock Model:01 startup, the routine at `0x60094DE4` configures
MPU region 1 with `RASR = 0x07080025` and enables the MPU before main.
Region 1 covers ITCM and its access-permission bits make it read-only.
Both SD probes first wrote a zero to ITCM at `0x0002B6A0` before checking
the mounted flag. That store violates the stock MPU settings even with
the card removed. The first main-loop probe did not write to ITCM.

Core already makes ITCM writable at startup. This probe now does the
same before initializing its state, leaving the other RASR fields and
the previous region selector intact. An emulator with the stock MPU
permissions modeled rejected the old probe at its first ITCM store and
let the corrected probe load the companion and write the expected
status on mocked exFAT. On the Model:01, the owner confirmed the
corrected image boots and plays normally. The freshly created status
file read back over USB DRIVE contained the expected line,
`Model:01 SD probe: loaded and ran 16 ticks`. This verifies the exFAT
read, companion relocation, TICK execution, and status write on hardware.

### SDK bindings for the standard loader

The Model:01 firmware header under `dirtyloader/sdk/include` now holds the
SdFat interface exercised by the probe. The same names and file layout are
used by the standard loader. The tick mod takes its Model:01 firmware ID
from that header.

The stock boot call at `0x600B0182`, SD load point at `0x60084CF2`, and
main-loop call at `0x600B0186` provide the locations for early setup, SD
loading, and TICK dispatch. The verified ITCM tail begins at
`0x0002B2A0`. The six ITCM banks end at `0x00030000`.

The standard loader also reads the firmware clock, USB state and crash
record, and its post-setup entry currently wraps the audio interrupt and
installs a view and SysEx handler even for a tick-only mod. Those bindings
and side effects need validation before using that entry on Model:01.
At this stage the SDK header covered the SD probe. The later builds
added the standard loader, menu, and SDK and shop target registration.

The stock Model:01 instructions identify the addresses used by the
standard boot guard:

| Value | Model:01 6.6.4A | Stock evidence |
| --- | --- | --- |
| Millisecond counter | `0x2003FF88` | SysTick handler at `0x600B2314` loads, increments and stores it |
| USB configuration byte | `0x200412ED` | Stock code at `0x6007F5E6` loads and reduces it to a connected flag; corresponding Model:02 references match |
| USB serial available count | `0x20033584` | Stock serial read decrements it after copying incoming bytes |
| USB serial read entry | `0x600B1F41` | Thumb routine uses the destination pointer and requested length to consume receive-ring bytes |
| Crash record | `0x2027FF80` | Fault handler at `0x600B0730` records 11 words, with the stacked PC at word 6 |

These are offline findings. The probe does not use the boot guard, and
these checks do not exercise its behavior on hardware.

## Standard loader build

```sh
python -m dirtyloader.core.build_model01 M8_V6_6_4A_MODEL01.bin build/model01-core
```

This builds the shared core with the SD loader and BOOT/TICK dispatch.
It uses the regular `dirtyloader/active.txt`, `dirtyloader/mods/`,
`dirtyloader/status.txt` and `dirtyloader/guard` files. The build produces
`M01-664-CORE.bin` and an `sd/dirtyloader` folder containing the tick mod,
active list and initial guard. The full core API, USB transfer, audio
hooks and MOD LOADER screen are outside this build.

The shared C loader is too large for the erased region used by the
single-mod probe. This build appends it on a 1 KiB boundary and updates
the ROM boot-data length and M8FW container together. The previous SD
probe's missing MPU setup is covered here by the existing core early
hook, before setup or any ITCM store.

For the hardware test, install the BIN with the generated
`dirtyloader` folder present on the SD card. After boot, status should
report `model01-tick.m8x: loaded model01-tick`, the mod should write
`dirtyloader/model01-tick.txt`, and the guard should return to `GOOD`
after ten seconds. Remove the marker and comment out the mod in
`active.txt` for a second boot. The marker should stay absent and the
status should show only the built-in tick hook.

ARM emulation with the Model:01 MPU permissions and mock exFAT calls
verified active mod execution, the tick marker, the guard returning to
`GOOD`, an inactive mod being skipped, and an `ARMD` guard becoming
`SKIP` without running the mod. The build test checks both firmware
lengths and the generated SD files. The Model:02 6.6.4A core payload was
byte-identical in before and after builds with the same local compiler.
On hardware, the Model:01 booted the standard loader and reported core
0.7 with two hooks. The status file listed `model01-tick.m8x` as loaded
from `active.txt`, and its fresh marker reported 16 ticks. The first SD
readback found the guard at `ARMD`. After a second wait in normal mode
without rebooting, the guard still read `ARMD`. The guard transition
passes under emulation but did not occur in this hardware test. The
disabled-mod test remains unverified on hardware.

### Guard diagnostics

Build `M01-664-TRACE.bin` with the optional guard trace.

```sh
python -m dirtyloader.core.build_model01 M8_V6_6_4A_MODEL01.bin build/model01-guard --guard-diagnostics
```

Start the test with the tick mod active and `dirtyloader/guard` set
to the four bytes `GOOD`, so the previous boot's `ARMD` does not
suppress loading. The diagnostic writes `guard-start.txt` at tick 16,
`guard-later.txt` at tick 8192, and `guard-write.txt` on the first attempt
to clear the guard. All three go under `dirtyloader/`. The first two
record the starting and current millisecond values and boot flags. The
last also reports open, write, close and immediate readback. The
diagnostics captured both successful and injected failed writes in
emulation. The hardware cause is still open.

On hardware the first trace builds recorded a later boot with the guard
already at `SKIP`. A normal power cycle starting from a host-verified
`GOOD` reproduced the end state. The later boot's log had replaced the
first failure, so it could not establish why the guard first tripped.

The updated trace keeps the armed tick snapshots in `armed-start.txt`
and `armed-later.txt`. A transition to a skipped boot records its reason,
previous guard value, fault status registers and PC in `guard-last-skip.txt`.
A later boot that reads an already-set `SKIP` does not overwrite that
file. Emulation verified that an interrupted boot records reason 3 and
an injected fault with a previously loaded mod records reason 1 and the
fault address.

### Guard confirmed on hardware

Ejecting the mounted card from macOS reboots the Model:01. An additional
manual reboot or firmware update during the resulting ten-second boot
window can leave `ARMD` and cause a false skipped boot on the next start.
The first preserved skip trace recorded reason 3, a previous guard of
`ARMD`, and no CPU fault.

On the clean run, the owner ejected once, let the M8 restart and waited in
normal mode. The trace from that boot recorded `started_ms 1298`,
`now_ms 11298`, four bytes written, a successful close, and immediate
readback `0x444F4F47` (`GOOD`). On returning to USB DRIVE, the current
status and armed snapshot were from a newer boot (`started_ms 2239`).
The live guard read `ARMD` from that boot, while the preserved write trace
confirmed the earlier boot cleared it as expected. The disabled-mod test
remains unverified on hardware.

## Model:01 MOD LOADER menu

```sh
python -m dirtyloader.core.build_model01 M8_V6_6_4A_MODEL01.bin build/model01-menu --menu
```

Revision 4 of the menu build produced `MENU-R4.bin`. It uses the stock
40-by-24 text grid and installs a view only if slot 7 is free. Stock
`set_view` at `0x6004E1A4` reads the view table at `0x20000BD8`; slot
7 is zero in the stock initialized data. The stock key call at
`0x60085696` targets `0x6004E920`, which handles the four-direction chord
as action `0xE4`. The hook leaves other actions with the stock router
outside the menu and lets the stock chord close an existing visualizer.

Press all four directions together to open MOD LOADER. UP and DOWN
select a mod, EDIT toggles its entry in `dirtyloader/active.txt`, and OPT
returns to the prior view. The change applies on the next boot. The
menu keeps other entries and comments in the active list. The list
must already exist and fit in the core's 512-byte list buffer.

Emulation exercised open, text drawing, toggle off and on, and return
to the prior view with mocked display and exFAT functions. The build
test also checks the patched KEY call. On the Model:01, the owner
confirmed the menu appeared. The four-direction chord and EDIT toggle
removed the tick mod from `active.txt`; the next boot reported only the
two built-in hooks and left the guard at `GOOD`. Re-enabling the mod
through the menu remains to be verified on hardware.

After the disable test, the owner reported an empty list on reopening
the menu and mixed grey and pink letters. The tick file was still on
the card. Model:01's text routine only writes the characters. The
revised menu also initializes every row's attribute bytes at grid
offset `0x3D1`, using the stock settings title and label attributes.
The revised scan reads the directory before the active list, recognizes
upper-case extensions, and shows distinct errors for missing card,
folder or active list. It lists files independently of whether they
are enabled. Emulation verifies that a disabled mod appears as OFF,
can be re-enabled, and draws with consistent attributes. The revised
build has yet to be tested on hardware.

The owner then confirmed that the color fix worked and the selection
controls were visible, but the upper rows were blank. Those controls
are only drawn when the directory scan finds a mod. In the stock frame
path, the notification overlay call at `0x6004D6C6` runs after the view
draws and can clear its old rows on expiry. The original menu drew its
rows only when dirty. Revision 3 redraws while visible and leaves that
overlay off the menu, delegating to stock in other views. Emulation
verified that another draw restores erased rows and the overlay call
is forwarded outside the menu. The package is `MENU-R3.bin`; the layout
fix still needs confirmation on the M8.

The blank list persisted on revision 3. A live capture of the Model:01
display stream showed the MOD LOADER title at y=12 and the footer at
y=240, 264 and 276. It emitted 32 character cells per visible row in
this font mode, with y=42 followed by y=60. There were no character
frames at y=48, where the menu placed its first mod. Revision 4
(`MENU-R4.bin`) starts the list on grid row 5 (y=60 in that capture) and
moves the status line down to make room for all 16 entries. The
firmware-backed build tests and ARM menu emulation pass with the new
layout.

Revision 4 was confirmed on the Model:01. The tick mod was visible and
EDIT added its filename to `active.txt` again. The immediate readback
showed the guard at `SKIP` and only the built-in hooks, so the re-enabled
mod had not yet been observed running on that boot.

### Cursor and footer revision

The heartbeat firmware showed a stationary stock crosshair and clipped
text at the bottom of MOD LOADER. The frame's call at `0x6004D6B0`
draws stock cursor overlays before the view draws. The revised build
runs the stock cursor reset during MOD LOADER and forwards the draw
call in stock views. The menu keeps its `>` selection marker and puts
the controls above the list, leaving the last two grid rows blank.

The revised candidate is `M01-Menu-Cursor-Fix.bin`. ARM emulation
checked cursor reset on the menu, stock cursor dispatch after leaving,
blank bottom rows, and SD heartbeat execution. The owner later observed both artifacts on the unit.

### Panel layout revision

The first cursor fix still showed cursor corners and clipped text.
The USB stream from that build showed corner rectangles repeatedly,
and a photo of the physical panel showed the title and first control
line outside its visible area. The menu now places all text on rows
5–18 and scrolls ten visible mod entries through the full list of
sixteen. It clears the physical display on entry using the stock
full-screen fill and theme background. The selection painter at
`0x600494BC` draws the corner rectangles, so the menu bypasses it both
on normal frames and on a full redraw. Stock views still call it.

Build with `--menu --name M01-Menu-Panel-Fix.bin`. The test build
passed the firmware-backed tests and ARM emulation for the display
clear, selection detours, scrolling, and heartbeat. The physical
panel still needs verification.


## Track Silence

The Model:01 startup copies the track voice's vtable into DTCM.
Its constructor at `0x60097D2C` installs the vtable at `0x20002504`,
whose first entry is the Thumb update routine `0x00002499`. The update
uses the track index at offset `0x3D` and renders samples beginning at
block offset 4. As on Model:02, the main strip is embedded at offset
`0x2E4`, with the input count and queue at strip offsets `0x18` and
`0x20`. The allocation stride is 148 bytes, including 32 float samples.

Model:01 now uses the same `EV_TRACK_AUDIO` wrapper as Model:02 and
Headless. Core wraps the slot at boot when there is a subscriber. The
wrapper calls the stock update first, then gives the mod each complete
stereo pair, including the second pair during a crossfade.

```sh
python -m dirtyloader.cc build examples/track-silence
python -m dirtyloader.core.build_model01 M8_V6_6_4A_MODEL01.bin build/model01-track-silence \
    --menu --name M01-Track-Silence-Test.bin
```

ARM emulation ran the compiled mod through the patched slot and real
dispatcher with a mocked stock update. It checked normal stereo and
crossfade blocks, sample boundaries, another track, and the stock slot
when the mod is disabled or the guard skips loading.

Copy `M01-Track-Silence-Test.bin` to the SD root and the Model:01
`track-silence.m8x` to `dirtyloader/mods/`. With a song that plays on
tracks 1 and 2, confirm both are audible with the mod OFF. Enable
`track-silence` in MOD LOADER and restart. Confirm that track 1 and
its sends are silent and track 2 still plays. After ten minutes of
playback, verify the loaded mod in `dirtyloader/status.txt`. Disable
the mod and restart again to confirm track 1 returns.

On hardware, the first test boot skipped every mod because the guard
was already at `SKIP`. After it was reset to `GOOD` and the unit
rebooted, the owner confirmed Track Silence worked. The longer playback
and disable-and-reboot checks remain unrecorded.


## Touch and master output

The touch engine at `0x600852AC` polls the controller and passes the
updated object at `0x2001C5E0` to the stock activity check. The call at
`0x600852D4` now dispatches TOUCH first, using the same convention as
Model:02. The master soft clip is at engine offset `0x88D8`, giving
`0x20014C58` on Model:01. Core uses the existing AUDIO_OUT wrapper,
with the Model:01 allocator at `0x0001B201` and 32-frame blocks. The
wrapper is installed only when a loaded mod subscribes.

The current firmware command is in [Current build and installation](#current-build-and-installation).
`python -m dirtyloader.cc build examples/audio-tone` builds Audio Tone
from its existing source for all four targets. To
test audio on the M8, lower the volume before booting with Audio Tone
enabled. The local `build/mods/m8-model01/touch-probe.m8x` test mod writes `dirtyloader/touch-probe.txt`
after a touch and leaves the stock touch handling active. Remove any
old marker before testing. ARM emulation exercised both compiled mods
through their Model:01 hooks and verified that disabled or skipped
mods produce no tone or touch marker. Hardware verification is pending.
