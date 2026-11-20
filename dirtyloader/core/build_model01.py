"""Build Model:01 6.6.4A with the standard SD loader and BOOT/TICK hooks."""

import argparse
from hashlib import sha256
from pathlib import Path
from struct import pack

from dirtyloader import cc
from dirtyloader.core import abi, builder
from dirtyloader.core.build_headless import write_sd
from dirtyloader.core.build_model01_probe import STOCK_LOOP, STOCK_SHA256, TICK_CALL
from dirtyloader.core.build_model01_sd_probe import SETUP_CALL, SETUP_TARGET
from dirtyloader.linker import link
from dirtyloader.m8fw import M8Firmware
from dirtyloader.m8mod import Mod, Site, make_target
from dirtyloader.re.boot import parse_boot

CORE_RAM = 0x0002B2A0
CORE_RAM_END = 0x00030000
FLASH_BLOCK = 1024
BINDINGS = {
    "core": {
        "BOOT": {"patch_at": "0x600B0182", "kind": "call_post", "target": "0x60084C00"},
        "TICK": {"patch_at": hex(TICK_CALL), "kind": "call_pre", "target": hex(STOCK_LOOP)},
        "TOUCH": {"patch_at": "0x600852D4", "kind": "call_pre", "target": "0x6007E458"},
    },
}


def build(stock_bin: Path, out: Path, *, guard_diagnostics: bool = False, menu: bool = False,
          name: str | None = None) -> Path:
    if name is not None and (Path(name).name != name or Path(name).suffix.lower() != ".bin"):
        raise ValueError("use a plain .bin filename for the build name")
    raw = stock_bin.read_bytes()
    digest = sha256(raw).hexdigest()
    if digest != STOCK_SHA256:
        raise ValueError("expected the stock Model:01 6.6.4A .bin")
    fw = M8Firmware.parse(raw)
    if builder._decode_bl(fw, SETUP_CALL) != SETUP_TARGET:
        raise ValueError("the stock SD setup call does not match")
    defines = {
        "CORE_SD_ONLY": "1",
        "CORE_FW_H": '"m8-model01-6.6.4A/fw.h"',
        "CORE_RAM": hex(CORE_RAM),
        "CORE_RAM_END": hex(CORE_RAM_END),
        "CORE_FLASH_TABLE": hex(builder.TABLE),
        "CORE_FLASH_SLOTS": str(abi.MAX_HOOKS),
        "CORE_DISPATCH": hex(builder.CODE | 1),
        "CORE_VENEERS": hex(builder.VENEER_TABLE),
    }
    if guard_diagnostics:
        defines["CORE_GUARD_DIAGNOSTICS"] = "1"
    code_at = fw.load_addr + len(fw.payload)
    entries = ["core_early", "core_load", "core_boot", "core_tick"]
    bindings = BINDINGS
    if menu:
        defines["CORE_MODEL01_MENU"] = "1"
        entries.extend(("core_key", "model01_menu_toast", "model01_menu_cursor", "model01_menu_selection"))
        bindings = {"core": BINDINGS["core"] | {
            "KEY": {"patch_at": "0x60085696", "kind": "call_pre", "target": "0x6004E920"},
        }}
    code, syms = cc.build_fixed([builder.CORE03_SOURCE], code_at, entries, defines)
    image = builder.build_core02(fw, bindings, ram=CORE_RAM, early=syms["core_early"] & ~1,
                                 boot=syms["core_boot"] & ~1, layout=3,
                                 calls_before={SETUP_CALL: syms["core_load"] & ~1})
    cave = builder.REGION
    start = fw.addr_to_offset(cave)
    stock_cave = bytes(fw.payload[start:start + len(image.blob)])
    if stock_cave != b"\xff" * len(image.blob):
        raise ValueError("the core region is not erased in the stock image")
    sites = [Site.from_bytes(cave, stock_cave, image.blob, "code", "core SD loader hooks")]

    # Keep the ROM's boot-data length and the updater container in agreement.
    boot = parse_boot(fw.payload)
    if boot.image_start != fw.load_addr or boot.image_length != len(fw.payload):
        raise ValueError("stock boot data and container describe different flash ranges")
    padded_code = code.ljust((len(code) + FLASH_BLOCK - 1) // FLASH_BLOCK * FLASH_BLOCK, b"\xff")
    length = boot.boot_data + 4
    off = fw.addr_to_offset(length)
    sites.append(Site.from_bytes(length, bytes(fw.payload[off:off + 4]),
                                 pack("<I", len(fw.payload) + len(padded_code)), "data", "ROM boot image length"))
    sites.append(Site.from_bytes(code_at, b"\xff" * len(padded_code), padded_code, "code", "core SD loader"))
    hooks = image.sites
    if menu:
        for call, target, symbol, overlay in (
            (0x6004CF36, 0x600494BC, "model01_menu_selection", "selection on full redraw"),
            (0x6004D676, 0x600494BC, "model01_menu_selection", "selection"),
            (0x6004D6B0, 0x6004A0B0, "model01_menu_cursor", "cursors"),
            (0x6004D6C6, 0x6004B6C0, "model01_menu_toast", "notifications"),
        ):
            if builder._decode_bl(fw, call) != target:
                raise ValueError(f"the stock {overlay} draw call does not match")
            detour = builder._asm(f"bl #{syms[symbol] & ~1}", call)
            hooks = [*hooks, (call, detour, f"keep stock {overlay} off the mod menu")]
    for addr, patch, note in hooks:
        start = fw.addr_to_offset(addr)
        sites.append(Site.from_bytes(addr, bytes(fw.payload[start:start + len(patch)]), patch, "code", note))
    subscriptions = [{"event": "TICK", "handler": syms["core_tick"] | 1, "order": 0}]
    if menu:
        subscriptions.append({"event": "KEY", "handler": syms["core_key"] | 1, "order": 0})
    core = Mod(id="core", target=make_target(fw, digest), sites=sites, events=list(bindings["core"]),
               resources={"names": ["core:loader"]}, subscribe=subscriptions)
    patched = link(fw, digest, [core])
    repo = Path(__file__).resolve().parents[2]
    tick, _ = cc.build_m8x([repo / "examples/model01-tick/tick.c"])
    out.mkdir(parents=True, exist_ok=True)
    write_sd(out, {"model01-tick.m8x": tick})
    mode = "MENU" if menu else "TRACE" if guard_diagnostics else "CORE"
    firmware = patched.build()
    revision = sha256(firmware).hexdigest()[:12]
    path = out / (name or f"M01-664-{mode}-{revision}.bin")
    if path.exists() and path.read_bytes() != firmware:
        raise FileExistsError(f"{path} holds a different firmware build; choose a new name")
    path.write_bytes(firmware)
    return path


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stock_bin", type=Path)
    parser.add_argument("out", type=Path)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--guard-diagnostics", action="store_true")
    mode.add_argument("--menu", action="store_true")
    parser.add_argument("--name", help="descriptive .bin filename for this revision")
    args = parser.parse_args()
    print(build(args.stock_bin, args.out, guard_diagnostics=args.guard_diagnostics, menu=args.menu, name=args.name))
