"""Build the Model:01 6.6.4A SD bring-up image and its companion mod."""

import argparse
from hashlib import sha256
from pathlib import Path
from zlib import crc32

from dirtyloader import cc
from dirtyloader.core import abi, builder
from dirtyloader.core.build_model01_probe import STOCK_LOOP, STOCK_SHA256, TICK_CALL
from dirtyloader.linker import link
from dirtyloader.m8fw import M8Firmware
from dirtyloader.m8mod import Mod, Site, make_target

SETUP_CALL = 0x60084CF2
SETUP_TARGET = 0x6009ADE8
MOD_FW = 0x01060604
CORE_BASE = builder.REGION + 0x40


def build(stock_bin: Path, out: Path) -> tuple[Path, Path]:
    raw = stock_bin.read_bytes()
    digest = sha256(raw).hexdigest()
    if digest != STOCK_SHA256:
        raise ValueError("expected the stock Model:01 6.6.4A .bin")
    fw = M8Firmware.parse(raw)
    if (builder._decode_bl(fw, TICK_CALL), builder._decode_bl(fw, SETUP_CALL)) != (STOCK_LOOP, SETUP_TARGET):
        raise ValueError("the stock setup or main-loop call does not match")

    repo = Path(__file__).resolve().parents[2]
    mod, _ = cc.build_m8x([repo / "examples/model01-sd-probe/tick.c"])
    info = cc.parse_m8x(mod)
    subscriptions = info["subscribe"]
    if info["fw"] != MOD_FW:
        raise ValueError("the companion mod targets a different firmware")
    if [sub["event"] for sub in subscriptions] != [abi.Event.TICK]:
        raise ValueError("expected exactly one TICK subscription")
    handler = subscriptions[0]["handler"]
    image, bss, rel = info["image"], info["bss"], info["relocations"]
    if (len(mod) >= 1024 or image + bss > 1024 or len(mod) != info["header"] + image + 4 * len(rel)
            or not (handler & 1 and (handler & ~1) < image)):
        raise ValueError("the companion mod does not fit the SD probe")
    defines = {f"PROBE_{name}": str(value) for name, value in {
        "SIZE": len(mod), "CRC": crc32(mod), "IMAGE": image, "BSS": bss,
        "RELOCS": len(rel), "HEADER": info["header"], "HANDLER": handler,
    }.items()}
    code, syms = cc.build_fixed([Path(__file__).with_name("c") / "model01_sd_probe.c"],
                                CORE_BASE, ["probe_load", "probe_tick"], defines)

    # Preserve the original call arguments and LR. Tail-call the stock target
    # so it returns straight to the original caller.
    cave = builder.REGION
    trampolines = {}
    stubs = b""
    for call, target, fn in ((SETUP_CALL, SETUP_TARGET, "probe_load"), (TICK_CALL, STOCK_LOOP, "probe_tick")):
        at = cave + len(stubs)
        stubs += builder._asm(f"push.w {{r0-r3, r4, lr}}; bl #{syms[fn] & ~1}; "
                              f"pop.w {{r0-r3, r4, lr}}; b.w #{target}", at)
        trampolines[call] = builder._asm(f"bl #{at}", call)

    # Keep the entire patch within the original image. A previous build
    # appended code beyond the stock IVT boot-data length and failed on hardware.
    stub_space = CORE_BASE - cave
    if len(stubs) > stub_space or stub_space + len(code) > abi.CORE_REGION_SIZE:
        raise ValueError("the SD probe exceeds the reserved flash region")
    cave_offset = fw.addr_to_offset(cave)
    end = cave_offset + stub_space + len(code)
    stock_cave = bytes(fw.payload[cave_offset:end])
    if stock_cave != b"\xff" * (end - cave_offset):
        raise ValueError("the SD probe region is in use")
    probe = stubs.ljust(stub_space, b"\xff") + code
    sites = [Site.from_bytes(cave, stock_cave, probe, "code", "SD probe loader and trampolines")]
    for call, patch in trampolines.items():
        off = fw.addr_to_offset(call)
        sites.append(Site.from_bytes(call, bytes(fw.payload[off:off + 4]), patch, "code", "SD probe call"))
    core = Mod(id="model01-sd-probe", target=make_target(fw, digest), sites=sites)
    patched = link(fw, digest, [core])

    out.mkdir(parents=True, exist_ok=True)
    bin_path, mod_path = out / "M01-664-SD-MPU.bin", out / "M01-TICK.m8x"
    patched.to_file(bin_path)
    mod_path.write_bytes(mod)
    return bin_path, mod_path


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stock_bin", type=Path)
    parser.add_argument("out", type=Path)
    args = parser.parse_args()
    for path in build(args.stock_bin, args.out):
        print(path)
