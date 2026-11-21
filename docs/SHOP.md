# The mod shop (`site/shop`)

A static page, after Elekloader's shop, that installs mods straight onto an M8 running dirtyloader core, over USB,
and sets the M8's theme. It is published with the rest of `site/` (GitHub Pages, `.github/workflows/pages.yml`) at
`/shop/`.

## What it does

- **Mod Shop** (`#shop`): every mod in `catalog.json` as a card on a shelf per category (Performance, Modulation,
  Effects, Tools; Developer behind "Show developer mods"). The sidebar's categories, the model filter, the search
  and Sort filter them. For Model:01, download the matching mod and copy it to `dirtyloader/mods/` on the card. A card's cover is line art for its motif, in its category's colour.
  **+ Install** puts the build for your M8's firmware on its card and switches it on; **✓ Installed** goes to it on
  the next page; **Update to x.y.z** replaces an older version. **Details** shows the summary, the keys, each build's
  size and sha256, the hooks it uses, and what was checked on a unit.
- **On your M8** (`#device`): the `.m8x` files on the card, each with its state (Running, On at restart, Off at
  restart, Off, Not loaded and why, from `status.txt`), an on/off switch (`active.txt`) and Remove. A `.m8x` dropped
  on the page is checked like core checks it (and against the M8's firmware) and installed. **Restart M8** restarts
  it, and the page reconnects when it comes back. The top bar says when what's on the card differs from what runs.
- **Theme** (`#theme`): the 13 colours of SYSTEM SETTINGS > THEME, with an M8 screen drawn in them (core's 5 × 7
  font; hovering a colour blinks what it paints).
  - Themes, in one dropdown above the screen: **from your M8** (what it shows now, and every `.m8t` in its
    `/Themes`), **here** (the M8's default, and five made for dirtyloader), **kept in this browser**, or a `.m8t` file
    you open. Once you change a colour, it reads "(edited)".
  - **The page wears the theme.** Every colour on the page is one of the theme's (`theme.js` `tokens()`): BACKGROUND
    is the page, TEXT:EMPTY its panels and lines, TEXT:DEFAULT the text, TEXT:VALUE strong text, TEXT:TITLES
    headings, CURSOR the buttons and focus, SELECTION what's picked, PLAY MARKERS "on", SCOPE progress, the meters
    ok / warning / error. Text that would be hard to read is pushed toward black or white until it is.
  - **Live on the M8:** with an M8 connected, each change repaints it as well (at most every 60 ms), without saving.
  - **Apply to M8** saves it there like the THEME page does (EEPROM, kept after a power-off), with the name.
    **Put the M8's back** restores what it had when the page connected, and saves that.
  - **Download .m8t** writes the theme as a file the M8's THEME page can LOAD.
  - When the page connects, it takes the M8's own theme unless you've already picked one here.
  - **More colours** (core 0.7's THEMEX): a colour for each of the 8 tracks and a title colour for each kind of
    screen, past the 13 the M8's THEME page has, with three switches (track colours, screen titles, titles follow the
    track) and Rainbow / Pastel / Neon fills. The preview screen shows them; they go live with the rest, Apply saves
    them on the card (`/dirtyloader/theme.ext`), and "Keep in this browser" keeps them with the theme. A `.m8t` holds
    only the 13. How core does it: [LOADER.md](LOADER.md#the-extended-theme-core-07).

- **Get started** (`#start`): how to use dirtyloader, and putting core on the M8, all in the tab.
  1. Which M8: Model:02 6.6.4A, Model:02 6.6.2A or Headless 6.6.4A; the connected M8 picks it (core's firmware id says
     which Model:02 build, `m8.js describeFw`).
     Model:01 uses the [command-line firmware build and SD setup](MODEL01-664-PORT.md#current-build-and-installation).
  2. Dirtywave's firmware, with a link to it (the Model:02's 6.6.4A is Dirtywave's current download, `M8Firmware.zip`,
     until they release a newer one; 6.6.2A is in their archive). Drop the `.bin`, the `.hex`, or the `.zip` as
     downloaded: it's recognised by its sha256 (`firmware.js identify`, reading the zip with `DecompressionStream`).
  3. Build: core's patch for that file (`firmware/<target>.json`) is applied and the image checked against the sha256
     it was made with; the page offers the `.bin` (SD card FIRMWARE UPDATE) and a `.hex` (TyUploader) that writes every
     byte, 0xFF included.
  4. Flash, after ticking "I know how to get my M8 back to stock": **over USB** (`halfkay.js`: Web Serial opens the M8
     at 134 baud so Teensyduino restarts into its bootloader, then WebHID writes the image in 1 KB blocks, every block,
     and reboots it), from the **SD card** (Model:02, Dirtywave's way; not yet tried with a dirtyloader build), or with
     **TyUploader**. The browser asks for the M8, then for its bootloader; the second time it doesn't ask. After a
     flash the page reconnects to the M8 by itself.
  5. Add mods.
- **Recover** (`#recover`): restart without mods (writes `SKIP` to core's `guard` and restarts; **Load mods again**
  writes `GOOD`), what core's crash and hang guard does, flashing Dirtywave's file as it is (a known one by sha256, or
  any M8 `.bin` whose checksums are good, or a `.hex` for the M8's flash), what to do when the M8 doesn't start, and
  where dirtyloader's files are.

### The firmware patches

`python site/shop/make_firmware.py` (ARM compiler and Dirtywave's stock files needed) builds core against each stock
file as `build_headless` / `build_model02` do and writes `firmware/<target>.json` and `firmware/index.json`. A patch is
not firmware: it lists where the built image differs from the stock one, and every run of 4 or more of those bytes that
also occurs in the stock image is an instruction to copy it from the user's file, not a byte in the patch. The script
checks that the patch rebuilds the image exactly and that no 4-byte window of the bytes it ships occurs in the stock
image. 2026-11-18: Model:02 6.6.2A 20,178 bytes from dirtyloader (89 KB of JSON), Model:02 6.6.4A 19,880 (90 KB),
Headless 15,484 (68 KB). The Model:02's two builds are two targets, `m8-model02` (6.6.2A) and `m8-model02-664`. Commit them after
any change to core: CI can't build them.

## How it talks to the M8

`m8.js` is `python -m dirtyloader.unit` in JavaScript, over [Web Serial](https://developer.mozilla.org/docs/Web/API/Web_Serial_API)
(Chrome, Edge, Opera on a computer; the chooser lists Teensy devices, USB vendor `0x16C0`). The requests are the ones
in [LOADER.md](LOADER.md#transfer-usb): INFO, LIST, GET (by range: a mod's id and version are read from its header
without the whole file), PUT in 1 KB pieces, CHECK (size and crc32 against the file sent), DELETE, MKDIRS, RESTART,
and core 0.7's THEME. Close m8c first: the port opens once.

Before a mod goes to the M8, the page checks the file's sha256 against `catalog.json`, then checks it the way core
does (`parseM8x`: magic, crc32 of image and relocations, the mod header) and that it was built for the connected
firmware (or for any).

**Privacy:** the page loads only its own files (a `<meta>` Content-Security-Policy: `default-src 'self'`), has no
analytics and no server side, and sends nothing anywhere but to your M8. `localStorage` keeps the theme you picked,
your saved themes, the model filter and the developer switch.

## The catalog

`site/shop/meta.json` is edited by hand: each mod's sources, category, cover motif, title, summary, details, keys,
and what was checked on a unit. `python site/shop/make_shop.py` needs the ARM compiler. By default it builds
a catalog entry for both Model:02 firmware versions and Headless. An entry with an explicit `targets` list can
add Model:01 or restrict the builds to selected targets. It writes the outputs to
`site/shop/mods/<target>/<id>.m8x`, checks each
with `cc.parse_m8x`, and writes `catalog.json` (sizes, sha256, versions, the hooks by name) and `font.js` (core's
font, from `dl/pixui.h`). Commit all three: CI has no ARM compiler. A `.m8x` holds only the mod's own code, no
firmware.

## Tests

- `node tests/test_shop.mjs` (CI): the protocol against `site/shop/test/fake_m8.js`, a pretend core with an
  in-memory card (install, verify, on/off, delete, theme read/preview/save, `/Themes`), every catalog file by its
  sha256, header and firmware, the `.m8t` round trip, and that the page's text stays readable in every preset.
- On hardware (2026-11-18, the Headless 6.6.4A Teensy): `m8.js` from Node through a serial-to-TCP relay installed a
  mod switched off, listed the card's 11 mods with their versions (1.6 s), previewed a theme and put it back, and
  removed the mod. The THEME command itself: SOLARIZED applied, saved with its name, and still there after a restart.
- In a browser with the pretend core (`fakePort()`; `window.dlShop.connect(port)` takes it): install, the device
  list, Restart's pending notice, presets from the card, Apply, and the page re-themed in dark and light themes, at
  desktop and phone widths.
- `node tests/test_firmware.mjs` (CI): `.bin` and `.hex` round trips (the `.hex` leaves no byte out), a `.zip` read,
  a patch's puts and copies and its sha256 check, the flasher against a pretend bootloader (every block in order,
  the reboot, retrying while it's busy), and, where Dirtywave's files are on the computer, both firmwares built from
  them and Dirtywave's 6.6.2 `.zip` as downloaded. The Headless build is byte for byte the one `build_headless` makes.
- The flasher on hardware (2026-11-18, the Headless Teensy): `halfkay.js` in Node, its reports passed to the Teensy's
  bootloader by a Windows HID relay. The 134-baud restart put it in the bootloader (HID usage 0xFF9C/0x25, 1088-byte
  reports). Dirtywave's stock 6.6.4A, then the page's build: 37-45 s each; the bootloader took two blocks, then refused
  writes for ~8.5 s while erasing (the first version gave up after 5 s: now up to a minute for the first 8 blocks).
  Stock booted without core; the build booted with core 0.7 and its five mods, and its flash read back (PEEK)
  matched the image's sha256. Restart without mods / Load mods again: nothing loaded, then all five.
- **Not yet:** the page in a real browser over Web Serial and WebHID with a real M8 (the choosers need a person),
  THEME on the Model:02 (its addresses are found and checked against the stock image, not run), and the SD card's
  FIRMWARE UPDATE with a dirtyloader `.bin`.

## Known limits

- **A preview can stick.** The firmware saves its settings from a dozen places, not only the THEME page, so colours
  previewed live can reach the EEPROM before you Apply. Put the M8's back (it saves) if you don't keep them.
- **Themes on the card:** the page reads `/Themes` but can't write there (core's PUT writes only under
  `/dirtyloader`). Apply keeps the theme in the M8's settings; Download gives you a `.m8t` to copy over yourself.
- **No firmware here.** The Mod Shop and Theme need core on the M8 (without it, Connect says there was no reply);
  Get started puts it there from Dirtywave's own file, which the user brings. One firmware version per model: a
  different Dirtywave file is refused, by its sha256.
- **Flashing needs WebHID** (Chrome or Edge on a computer). Elsewhere the page still builds the files, for the SD card
  or TyUploader.
