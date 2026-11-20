"""Core 0.3 on a synthetic headless image, and loadable mods (.m8x).

Needs arm-none-eabi-gcc (WSL on Windows) as well as keystone/capstone; skipped without them. The image is the
hand-written one from test_core02, as model 0 (headless), so nothing from Dirtywave is used.
"""
import hashlib
import struct
import unittest

from dirtyloader import cc, ihex
from dirtyloader.core import abi
from dirtyloader.m8fw import M8Firmware

try:
    import keystone  # noqa: F401
    import capstone  # noqa: F401
    HAVE_ASM = True
except ImportError:  # pragma: no cover
    HAVE_ASM = False
HAVE_CC = HAVE_ASM and cc.available()

from tests.test_core02 import BINDINGS as B02, MAIN, SETUP, LOAD, _dis, _image  # noqa: E402

C_AT, RAM, RAM_END = 0x60010000, 0x00029B40, 0x00030000
BINDINGS = dict(B02, loader={"c_at": hex(C_AT), "ram": hex(RAM), "ram_end": hex(RAM_END),
                             "fw_header": "m8-headless/fw.h", "load_at": hex(SETUP + 2)})
INNER = SETUP + 0x40               # setup() calls it; core 0.3's loader runs just before that call
HEARTBEAT = "examples/sd-heartbeat/heartbeat.c"


def _headless():
    from dirtyloader.core.builder import _asm
    fw = _image()
    for addr, asm in ((SETUP, f"push {{r3, lr}}; bl #{INNER}; pop {{r3, pc}}"), (INNER, "bx lr")):
        b = _asm(asm, addr)
        fw.payload[addr - LOAD:addr - LOAD + len(b)] = b
    return M8Firmware(version=ihex.HEADLESS_MODEL << 16, load_addr=fw.load_addr, payload=fw.payload)


@unittest.skipUnless(HAVE_CC, "arm-none-eabi-gcc, keystone or capstone not available")
class TestCore03(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from dirtyloader.core import builder
        cls.builder = builder
        cls.fw = _headless()
        cls.sha = hashlib.sha256(bytes(cls.fw.payload)).hexdigest()
        cls.core = builder.build_core03_mod(cls.fw, cls.sha, BINDINGS)

    def test_core_links_past_the_image(self):
        from dirtyloader.linker import link
        self.assertEqual(self.core.version, "0.7.0")
        self.assertEqual(self.core.target["payload_len"], len(self.fw.payload))   # targets the stock image
        out = link(self.fw, self.sha, [self.core])
        loader = [s for s in self.core.sites if s.addr == C_AT][0]
        self.assertEqual(len(out.payload), C_AT - LOAD + loader.len)              # padded to fit the loader
        self.assertEqual(struct.unpack_from("<HH", out.payload, 0x208), (3, abi.MAX_HOOKS))  # layout 3
        ev, fl, fn = struct.unpack_from(abi.HOOK_ENTRY_FMT, out.payload, 0x210)   # core_tick, order 0
        self.assertEqual((ev, fl), (int(abi.Event.TICK), abi.FLAG_ENABLED))
        self.assertTrue(C_AT < fn < C_AT + loader.len and fn & 1)

    def test_boot_stub_runs_the_loader_around_setup(self):
        out_img = self.builder.build_core02(self.fw, BINDINGS, ram=RAM, early=C_AT + 0x100, boot=C_AT + 0x200,
                                            layout=3)
        code = out_img.blob[0x210:]
        stub = [f"{m} {o}" for _, m, o in _dis(code[out_img.stubs["BOOT"] - self.builder.CODE:][:24],
                                                out_img.stubs["BOOT"])]
        self.assertEqual(stub[1:4], [f"bl #0x{C_AT + 0x100:x}", f"bl #0x{SETUP:x}", f"bl #0x{C_AT + 0x200:x}"])
        disp = [f"{m} {o}" for _, m, o in _dis(code[:0x40], self.builder.CODE)]
        self.assertIn(f"movw sb, #0x{RAM & 0xFFFF:x}", disp)                    # reads the ITCM table's magic
        self.assertIn(f"movt r0, #0x{self.builder.RAM_MAGIC >> 16:x}", disp)

    def test_loader_runs_before_the_call_inside_setup(self):
        site = [s for s in self.core.sites if s.addr == SETUP + 2][0]
        stub = int(_dis(bytes.fromhex(site.new[0][1]), SETUP + 2)[0][2].lstrip("#"), 16)
        o = stub - LOAD
        text = [f"{m} {o_}" for _, m, o_ in _dis(bytes(self.core.sites[0].data(self.fw))[stub - abi.CORE_REGION_ADDR:][:16], stub)]
        self.assertTrue(text[1].startswith("bl #0x6001"))                       # core_load, in the C loader
        self.assertEqual(text[3], f"b.w #0x{INNER:x}")                          # then the original call

    def test_call_site_goes_straight_to_core_c(self):
        from dirtyloader.core.builder import _asm
        fw = _headless()                                    # INNER calls a leaf: that bl becomes core's C
        leaf, at = INNER + 0x20, INNER + 2
        for addr, asm in ((INNER, f"push {{r3, lr}}; bl #{leaf}; pop {{r3, pc}}"), (leaf, "bx lr")):
            code = _asm(asm, addr)
            fw.payload[addr - LOAD:addr - LOAD + len(code)] = code
        b = dict(BINDINGS, loader=dict(BINDINGS["loader"], calls={hex(at): {"fn": "core_midi_read", "event": "MIDI"}}))
        core = self.builder.build_core03_mod(fw, hashlib.sha256(bytes(fw.payload)).hexdigest(), b)
        self.assertIn("MIDI", core.events)
        site = [s for s in core.sites if s.addr == at][0]
        target = int(_dis(bytes.fromhex(site.new[0][1]), at)[0][2].lstrip("#"), 16)
        self.assertTrue(C_AT <= target < C_AT + 0x4000)                         # into core's C, no stub

    def _mid_fw(self):
        from dirtyloader.core.builder import _asm
        fw = _headless()
        at = INNER + 0x10                                   # a mask the C replaces, then code that must stay
        code = _asm("and r3, r3, #0xf; movs r2, #1; bx lr", at)
        fw.payload[at - LOAD:at - LOAD + len(code)] = code
        return fw, at

    def test_flash_mid_calls_core_c_and_returns_into_a_register(self):
        fw, at = self._mid_fw()
        mids = {hex(at): {"fn": "core_dest_cell", "args": ["r5", "r4", "r3"], "ret": "r3",
                          "expect": "and r3, r3, #0xf"}}
        b = dict(BINDINGS, loader=dict(BINDINGS["loader"], mids=mids))
        core = self.builder.build_core03_mod(fw, hashlib.sha256(bytes(fw.payload)).hexdigest(), b)
        site = [s for s in core.sites if s.addr == at][0]
        stub = int(_dis(bytes.fromhex(site.new[0][1]), at)[0][2].lstrip("#"), 16)
        text = [f"{m} {o}" for _, m, o in _dis(bytes(core.sites[0].data(fw))[stub - abi.CORE_REGION_ADDR:][:72], stub)]
        self.assertEqual(text[0], "push.w {r0, r1, r2, r3, ip, lr}")
        self.assertIn("ldr r2, [sp, #0x54]", text)                              # r3 as it was at the site
        call = [t for t in text if t.startswith("bl #")][0]
        self.assertTrue(C_AT <= int(call.split("#")[1], 16) < C_AT + 0x8000)   # core's C
        self.assertIn("str r0, [sp, #0xc]", text)                              # the result lands in r3
        self.assertEqual([t for t in text if t.startswith("b.w")][0], f"b.w #0x{at + 4:x}")

    def test_flash_mid_refuses_other_bytes(self):
        fw, at = self._mid_fw()
        mids = {hex(at): {"fn": "core_dest_cell", "args": ["r5"], "ret": "r3", "expect": "and r7, r7, #0xf"}}
        b = dict(BINDINGS, loader=dict(BINDINGS["loader"], mids=mids))
        with self.assertRaises(RuntimeError):
            self.builder.build_core03_mod(fw, hashlib.sha256(bytes(fw.payload)).hexdigest(), b)

    def test_hex_carries_bytes_past_the_stock_end(self):
        from dirtyloader.linker import link
        out = link(self.fw, self.sha, [self.core])
        img = ihex.HexImage(base=LOAD, data=bytearray(self.fw.payload), covered=bytearray(b"\x01" * 0x4000),
                            start=None)
        back = ihex.read(ihex.write(img, bytes(out.payload)))
        self.assertEqual(bytes(back.data[C_AT - LOAD:]), bytes(out.payload[C_AT - LOAD:]).rstrip(b"\xff"))


@unittest.skipUnless(HAVE_CC, "arm-none-eabi-gcc not available")
class TestM8x(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.blob, cls.info = cc.build_m8x([HEARTBEAT])

    def test_header_and_subscriptions(self):
        m = cc.parse_m8x(self.blob)
        self.assertEqual((m["id"], m["version"], m["abi"]), ("sd-heartbeat", "0.1.3", 1))
        self.assertEqual(m["fw"], 0x00060604)
        self.assertEqual([(s["event"], s["order"]) for s in m["subscribe"]],
                         [(int(abi.Event.BOOT), 50), (int(abi.Event.TICK), 50), (int(abi.Event.TEMPO), 50)])
        self.assertTrue(all(s["handler"] & 1 and s["handler"] < m["image"] for s in m["subscribe"]))
        self.assertEqual(m["bss"], 4)

    def test_dlx2_marks_the_writable_part(self):
        m = cc.parse_m8x(self.blob)
        self.assertEqual(self.blob[:4], b"DLX2")
        self.assertEqual(m["data"], self.info["data"])
        self.assertTrue(0 < m["data"] <= m["image"] and m["data"] % 4 == 0)
        # every relocated word points at code / constants (flash) or at .data / .bss (RAM): never past them
        img = self.blob[m["header"]:m["header"] + m["image"]]
        for r in m["relocations"]:
            self.assertLessEqual(struct.unpack_from("<I", img, r)[0] & ~1, m["image"] + m["bss"])

    def test_relocations_cover_every_internal_pointer(self):
        m = cc.parse_m8x(self.blob)
        img = self.blob[m["header"]:m["header"] + m["image"]]
        # dl_mod's id, version, title and subs pointers and each handler pointer must be relocated
        self.assertTrue({8, 12, 16, 20} <= set(m["relocations"]))
        subs = struct.unpack_from("<I", img, 20)[0]
        self.assertTrue({subs + 4 + 8 * i for i in range(3)} <= set(m["relocations"]))

    def test_struct_copies_link_against_the_sdk_libc(self):
        import os, tempfile
        src = "\n".join(['#include "dl/mod.h"', "struct big { unsigned w[40]; };", "struct big a, b;",
                         "static void on_tick(void) { a = b; }",
                         'DL_MOD("copy", "0.1.0", "t", DL_ON(EV_TICK, 50, on_tick));', ""])
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "copy.c")
            open(path, "w").write(src)
            blob, info = cc.build_m8x([path])                                    # gcc calls memcpy for a = b
        self.assertIn("memcpy", info["symbols"])
        self.assertEqual(cc.parse_m8x(blob)["id"], "copy")

    def test_lfo_sends_builds_and_subscribes(self):
        blob, info = cc.build_m8x(["examples/lfo-sends/lfo_sends.c"])
        m = cc.parse_m8x(blob)
        self.assertEqual(m["id"], "lfo-sends")
        self.assertEqual([s["event"] for s in m["subscribe"]],
                         [int(abi.Event.BOOT), int(abi.Event.MOD_APPLY), int(abi.Event.TRACK_AUDIO)])
        # no soft-float helpers: the audio-thread maths is FPU code
        self.assertFalse([s for s in info["symbols"] if s.startswith("__aeabi_f")])

    def test_chaos_control_builds_for_both_firmwares(self):
        from dirtyloader.core.build_model02 import _alias_dir
        blob, info = cc.build_m8x(["examples/chaos-control/chaos.c"])
        m = cc.parse_m8x(blob)
        self.assertEqual(m["id"], "chaos-control")
        events = [s["event"] for s in m["subscribe"]]
        self.assertIn(int(abi.Event.AUDIO_OUT), events)
        self.assertNotIn(int(abi.Event.TOUCH), events)                 # headless has no touch screen
        self.assertIn(int(abi.Event.KEY), events)                      # nor a visualizer: all four directions
        self.assertFalse([s for s in info["symbols"]
                          if s.startswith("__aeabi_f") or s in ("tanf", "expf", "exp2f", "sinf", "sqrtf", "floorf")])
        blob2, _ = cc.build_m8x(["examples/chaos-control/chaos.c"], defines={"M8_MODEL02": "1"}, includes=[_alias_dir()])
        m2 = cc.parse_m8x(blob2)
        self.assertEqual(m2["fw"], 0x02060602)
        self.assertIn(int(abi.Event.TOUCH), [s["event"] for s in m2["subscribe"]])
        ev2 = [s["event"] for s in m2["subscribe"]]
        self.assertIn(int(abi.Event.KEY), ev2)          # the firmware's visualizer: a key closes it, CHAOS erases first
        self.assertIn(int(abi.Event.VIS_SCOPE), ev2)    # METERS skip the stock scopes
        self.assertNotIn(int(abi.Event.VIS_SCOPE), events)
        self.assertIn(int(abi.Event.VIS_DRAW), [s["event"] for s in m2["subscribe"]])  # drawn over it
        self.assertNotIn(int(abi.Event.VIS_DRAW), events)

    def test_perf_mon_builds_small_for_both_firmwares(self):
        from dirtyloader.core.build_model02 import _alias_dir
        blob, info = cc.build_m8x(["examples/perf-mon/perf.c"])
        m = cc.parse_m8x(blob)
        events = [s["event"] for s in m["subscribe"]]
        self.assertEqual(events.count(int(abi.Event.AUDIO_OUT)), 2)     # brackets the other mods' handlers
        self.assertNotIn(int(abi.Event.VIS_DRAW), events)
        blob2, info2 = cc.build_m8x(["examples/perf-mon/perf.c"], defines={"M8_MODEL02": "1"},
                                    includes=[_alias_dir()])
        m2 = cc.parse_m8x(blob2)
        self.assertEqual([s["event"] for s in m2["subscribe"]].count(int(abi.Event.VIS_DRAW)), 2)
        chaos, _c = cc.build_m8x(["examples/chaos-control/chaos.c"], defines={"M8_MODEL02": "1"},
                                 includes=[_alias_dir()])
        # core 0.7 runs CHAOS CONTROL's code from flash when ITCM is short: what it needs in RAM is small
        self.assertLess(_c["image"] - _c["data"] + _c["bss"], 1024)
        self.assertLess(info2["image"] + info2["bss"], 2048)

    def test_flash_probe_only_fits_in_flash(self):
        blob, info = cc.build_m8x(["examples/flash-probe/flash_probe.c"])
        m = cc.parse_m8x(blob)
        self.assertGreater(m["data"], 24 * 1024)                       # its table is code / constants: flash
        self.assertLess(m["image"] - m["data"] + m["bss"], 64)         # what stays in RAM
        img = blob[m["header"]:m["header"] + m["image"]]
        kinds = {(r < m["data"], struct.unpack_from("<I", img, r)[0] < m["data"]) for r in m["relocations"]}
        self.assertTrue({(True, True), (True, False), (False, True)} <= kinds)   # flash->flash, flash->RAM, RAM->flash

    def test_load_gen_builds(self):
        from dirtyloader.core.build_model02 import _alias_dir
        for kw in ({}, {"defines": {"M8_MODEL02": "1"}, "includes": [_alias_dir()]}):
            blob, info = cc.build_m8x(["examples/load-gen/load_gen.c"], **kw)
            self.assertLess(info["image"] + info["bss"], 640)   # 512 and its DL_INFO text

    def test_corruption_is_refused(self):
        bad = bytearray(self.blob)
        bad[cc.M8X_HEADER2.size + 40] ^= 1
        with self.assertRaises(cc.CCError):
            cc.parse_m8x(bytes(bad))


if __name__ == "__main__":
    unittest.main()
