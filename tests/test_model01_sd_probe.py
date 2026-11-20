import os
import shutil
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest

from dirtyloader import cc
from dirtyloader.core import abi, builder
from dirtyloader.core.build_model01_probe import TICK_CALL
from dirtyloader.core.build_model01_sd_probe import MOD_FW, SETUP_CALL, build
from dirtyloader.m8fw import M8Firmware


class Model01SDProbeTest(unittest.TestCase):
    def test_probe_keeps_stock_image_length(self):
        stock_dir = os.environ.get("DIRTYLOADER_MODEL01_664_DIR")
        if not stock_dir or not shutil.which(cc.GCC):
            self.skipTest("requires the stock Model:01 6.6.4A firmware and ARM toolchain")
        stock_path = Path(stock_dir) / "M8_V6_6_4A_MODEL01.bin"
        stock = M8Firmware.from_file(stock_path)

        with TemporaryDirectory() as out:
            bin_path, mod_path = build(stock_path, Path(out))
            patched = M8Firmware.from_file(bin_path)
            mod = cc.parse_m8x(mod_path.read_bytes())

        self.assertEqual(mod["fw"], MOD_FW)
        self.assertEqual(mod["subscribe"][0]["event"], abi.Event.TICK)
        self.assertEqual(patched.version, stock.version)
        self.assertEqual(patched.load_addr, stock.load_addr)
        self.assertEqual(len(patched.payload), len(stock.payload))
        self.assertEqual(patched.payload[0x1000:0x1030], stock.payload[0x1000:0x1030])

        expected = stock.payload.copy()
        cave = stock.addr_to_offset(builder.REGION)
        end = cave + abi.CORE_REGION_SIZE
        expected[cave:end] = patched.payload[cave:end]
        for call in (SETUP_CALL, TICK_CALL):
            target = builder._decode_bl(patched, call)
            self.assertTrue(builder.REGION <= target < builder.REGION + abi.CORE_REGION_SIZE)
            offset = stock.addr_to_offset(call)
            expected[offset:offset + 4] = patched.payload[offset:offset + 4]
        self.assertEqual(patched.payload, expected)
