import os
import shutil
from hashlib import sha256
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest

from dirtyloader import cc
from dirtyloader.core import builder
from dirtyloader.core.build_model01 import BINDINGS, FLASH_BLOCK, build
from dirtyloader.core.build_model01_sd_probe import SETUP_CALL
from dirtyloader.m8fw import M8Firmware
from dirtyloader.re.boot import parse_boot


class Model01CoreTest(unittest.TestCase):
    def test_build_packages_core_and_active_mod(self):
        stock_dir = os.environ.get("DIRTYLOADER_MODEL01_664_DIR")
        if not stock_dir or not shutil.which(cc.GCC):
            self.skipTest("requires the stock Model:01 6.6.4A firmware and ARM toolchain")
        stock_path = Path(stock_dir) / "M8_V6_6_4A_MODEL01.bin"
        stock = M8Firmware.from_file(stock_path)
        with TemporaryDirectory() as temp:
            out = Path(temp)
            patched = M8Firmware.from_file(build(stock_path, out))
            sd = out / "sd" / "dirtyloader"
            self.assertIn("model01-tick.m8x", (sd / "active.txt").read_text().splitlines())
            self.assertEqual((sd / "guard").read_bytes(), b"GOOD")
            mod = cc.parse_m8x((sd / "mods" / "model01-tick.m8x").read_bytes())
            menu_path = build(stock_path, out, menu=True)
            menu = M8Firmware.from_file(menu_path)
            revision = sha256(menu_path.read_bytes()).hexdigest()[:12]

        self.assertEqual(mod["id"], "model01-tick")
        self.assertEqual(mod["fw"], 0x01060604)
        self.assertEqual(patched.model, 1)
        self.assertGreater(len(patched.payload), len(stock.payload))
        self.assertEqual(len(patched.payload) % FLASH_BLOCK, 0)
        self.assertEqual(parse_boot(patched.payload).image_length, len(patched.payload))
        for call in [SETUP_CALL, *(int(site["patch_at"], 16) for site in BINDINGS["core"].values())]:
            self.assertNotEqual(builder._decode_bl(patched, call), builder._decode_bl(stock, call))

        self.assertEqual(menu_path.name, f"M01-664-MENU-{revision}.bin")
        self.assertEqual(parse_boot(menu.payload).image_length, len(menu.payload))
        self.assertEqual(builder._decode_bl(stock, 0x60085696), 0x6004E920)
        self.assertNotEqual(builder._decode_bl(menu, 0x60085696), 0x6004E920)
        self.assertEqual(builder._decode_bl(patched, 0x60085696), 0x6004E920)
        self.assertEqual(builder._decode_bl(stock, 0x6004D6C6), 0x6004B6C0)
        self.assertNotEqual(builder._decode_bl(menu, 0x6004D6C6), 0x6004B6C0)
        self.assertEqual(builder._decode_bl(stock, 0x6004D6B0), 0x6004A0B0)
        self.assertEqual(builder._decode_bl(patched, 0x6004D6B0), 0x6004A0B0)
        self.assertNotEqual(builder._decode_bl(menu, 0x6004D6B0), 0x6004A0B0)
        for call in (0x6004CF36, 0x6004D676):
            self.assertEqual(builder._decode_bl(stock, call), 0x600494BC)
            self.assertEqual(builder._decode_bl(patched, call), 0x600494BC)
            self.assertNotEqual(builder._decode_bl(menu, call), 0x600494BC)
