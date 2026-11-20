"""Build a Model:01 6.6.4A main-loop probe for initial hardware validation.

python -m dirtyloader.core.build_model01_probe <stock.bin> <stock.hex> <out dir>
"""

import argparse
from hashlib import sha256
from pathlib import Path

from dirtyloader import ihex
from dirtyloader.core import builder
from dirtyloader.linker import link
from dirtyloader.m8fw import M8Firmware

STOCK_SHA256 = "9c33ab156117f9991bdc2bc15f947b763ec693899625ccdd8fe7ceea8c78ba07"
TICK_CALL = 0x600B0186
STOCK_LOOP = 0x6008460C


def build(stock_bin: Path, stock_hex: Path, out: Path) -> tuple[Path, Path]:
    raw = stock_bin.read_bytes()
    digest = sha256(raw).hexdigest()
    if digest != STOCK_SHA256:
        raise ValueError("expected the stock Model:01 6.6.4A .bin")
    stock = M8Firmware.parse(raw)
    image = ihex.read(stock_hex.read_text(encoding="ascii"))
    if image.base != stock.load_addr or image.data != stock.payload:
        raise ValueError("the stock HEX flash image differs from the BIN payload")
    if builder._decode_bl(stock, TICK_CALL) != STOCK_LOOP:
        raise ValueError("the stock main-loop call target differs from the mapped target")

    core = builder.build_core_mod(stock, digest, TICK_CALL, STOCK_LOOP)
    patched = link(stock, digest, [core])
    coverage = [(site.addr, site.len) for site in core.sites]
    hex_text = ihex.write(image, bytes(patched.payload), cover=coverage)

    out.mkdir(parents=True, exist_ok=True)
    bin_path = out / "M8_V6_6_4A_MODEL01_tick_probe.bin"
    hex_path = out / "M8_V6_6_4A_MODEL01_tick_probe.hex"
    patched.to_file(bin_path)
    hex_path.write_text(hex_text, encoding="ascii", newline="\r\n")
    return bin_path, hex_path


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stock_bin", type=Path)
    parser.add_argument("stock_hex", type=Path)
    parser.add_argument("out", type=Path)
    args = parser.parse_args()
    for path in build(args.stock_bin, args.stock_hex, args.out):
        print(path)
