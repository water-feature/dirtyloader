import os
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest

from dirtyloader import ihex
from dirtyloader.core import builder
from dirtyloader.core.build_model01_probe import STOCK_LOOP, TICK_CALL, build
from dirtyloader.m8fw import M8Firmware


class Model01ProbeTest(unittest.TestCase):
    def test_stock_round_trip(self):
        stock_dir = os.environ.get("DIRTYLOADER_MODEL01_664_DIR")
        if not stock_dir:
            self.skipTest("set DIRTYLOADER_MODEL01_664_DIR to the stock Model:01 6.6.4A firmware directory")
        stock_bin = Path(stock_dir) / "M8_V6_6_4A_MODEL01.bin"
        stock_hex = Path(stock_dir) / "M8_V6_6_4A_MODEL01 (TyUploader).hex"
        stock = M8Firmware.from_file(stock_bin)

        with TemporaryDirectory() as out:
            bin_path, hex_path = build(stock_bin, stock_hex, Path(out))
            patched = M8Firmware.from_file(bin_path)
            image = ihex.read(hex_path.read_text(encoding="ascii"))

        self.assertEqual(patched.version, stock.version)
        self.assertEqual(patched.load_addr, stock.load_addr)
        self.assertEqual(image.base, patched.load_addr)
        self.assertEqual(image.data, patched.payload)

        region = builder.build_region(STOCK_LOOP)
        self.assertEqual(builder._decode_bl(patched, TICK_CALL), region.tramp_tick)
        cave = stock.addr_to_offset(builder.REGION)
        call = stock.addr_to_offset(TICK_CALL)
        expected = stock.payload.copy()
        expected[cave:cave + len(region.blob)] = region.blob
        expected[call:call + 4] = patched.payload[call:call + 4]
        self.assertEqual(patched.payload, expected)
