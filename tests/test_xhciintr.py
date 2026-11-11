#!/usr/bin/env python3
"""Offline contract for kern_start.cpp xhciintr (-rmbxhciintr1): the 25G229 AppleUSBXHCI bytes behind the
TRB interrupter target 1 -> 0 patch. CPU only; the Boot KC is an external input (BOOT_KC)."""
from pathlib import Path
import os, re, struct, unittest

KC = Path(os.environ.get('BOOT_KC', '/System/Library/KernelCollections/BootKernelExtensions.kc'))
SRC = Path(__file__).resolve().parents[1] / 'src/Rembrandt660M/kern_start.cpp'
ENTRY = 'com.apple.driver.usb.AppleUSBXHCI'
UUID = '4F5E24C5-4FEA-32DA-A8AD-D2FEFEF3ABF2'


def load_commands(b, off):
    magic, _, _, _, ncmds, _, _, _ = struct.unpack_from('<IiiIIIII', b, off)
    assert magic == 0xfeedfacf
    p = off + 32
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', b, p)
        yield cmd, p
        p += size


def xhci_image():
    b = KC.read_bytes()
    for cmd, p in load_commands(b, 0):
        if cmd != 0x80000035:  # LC_FILESET_ENTRY
            continue
        vm, fo, nameoff = struct.unpack_from('<QQI', b, p + 8)
        if b[p + nameoff:b.index(b'\0', p + nameoff)].decode() != ENTRY:
            continue
        uuid, text = None, None
        for c, q in load_commands(b, fo):
            if c == 0x1b:
                u = b[q + 8:q + 24].hex().upper()
                uuid = '-'.join((u[:8], u[8:12], u[12:16], u[16:20], u[20:]))
            elif c == 0x19 and b[q + 8:q + 24].rstrip(b'\0') == b'__TEXT':
                text = struct.unpack_from('<QQQQ', b, q + 24)
        return vm, uuid, text, b[text[2]:text[2] + text[3]]
    raise AssertionError(ENTRY + ' not in ' + str(KC))


def source_sites():
    s = SRC.read_text()
    tab = s[s.index('} xhciSites[] {'):]
    tab = tab[:tab.index('\n};')]
    rows = []
    for m in re.finditer(r'\{"(\w+)", (0x[0-9a-f]+), (\d+), (\d+), (0x[0-9a-f]+), (0x[0-9a-f]+), \{([^}]*)\}\}', tab):
        rows.append((m.group(1), int(m.group(2), 16), int(m.group(3)), int(m.group(4)), int(m.group(5), 16),
                     int(m.group(6), 16), bytes(int(x, 16) for x in m.group(7).split(','))))
    return rows


class XhciIntrSource(unittest.TestCase):
    def test_registered_with_kill_switch(self):
        s = SRC.read_text()
        self.assertIn('lilu.onKextLoadForce(kextXhci, arrsize(kextXhci), processXhciKext);', s)
        self.assertIn('checkKernelArgument("-rmbxhciintr1")', s)
        self.assertIn('{"com.apple.driver.usb.AppleUSBXHCI", pathXhci', s)
        self.assertIn('0x4f, 0x5e, 0x24, 0xc5, 0x4f, 0xea, 0x32, 0xda', s)
        self.assertIn('XhciTextSize25G229 = 0x3b000', s)

    def test_all_or_nothing(self):
        s = SRC.read_text()
        body = s[s.index('static bool xhciPrimaryInterrupter('):s.index('static const char *pathXhci[]')]
        verify = body.index('bytes differ')
        write = body.index('p[s.rva + s.off] = s.to;')
        self.assertLess(verify, write)  # every site is checked before the first write
        self.assertIn('mixed image, left unchanged', body)
        self.assertLess(body.index('setKernelWriting(true'), write)
        self.assertLess(write, body.index('setKernelWriting(false'))

    def test_table_shape(self):
        rows = source_sites()
        self.assertEqual([r[0] for r in rows], ['setup', 'zlp', 'data', 'tdsize', 'status', 'link', 'sparse'])
        for name, rva, ln, off, old, new, pat in rows:
            self.assertEqual(len(pat), ln, name)
            self.assertEqual(pat[off], old, name)
            self.assertEqual(new, 0, name)


@unittest.skipUnless(KC.exists(), 'Boot KC not in workspace: ' + str(KC))
class XhciIntrBootKc(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.vm, cls.uuid, cls.text, cls.img = xhci_image()

    def test_uuid_and_text(self):
        self.assertEqual(self.uuid, UUID)
        self.assertEqual(self.vm, 0xffffff8002f12000)
        self.assertEqual(self.text[0], self.vm)  # __TEXT starts at the Mach-O header: RVA = vm - header
        self.assertEqual(self.text[1], 0x3b000)

    def test_sites_match_and_unique(self):
        for name, rva, ln, off, old, new, pat in source_sites():
            self.assertEqual(self.img[rva:rva + ln], pat, name)
            self.assertEqual(self.img.count(pat), 1, name)

    def test_immediates_are_interrupter_target_1(self):
        rows = {r[0]: r for r in source_sites()}
        imm = lambda rva: struct.unpack_from('<I', self.img, rva)[0]
        self.assertEqual(imm(0x1b7c + 3), 0x400008)  # c7 41 08 imm32
        self.assertEqual(imm(0x20c4 + 4), 0x400000)
        self.assertEqual(imm(0x20cc + 4), 0x421)  # Normal, IOC
        self.assertEqual(imm(0x2320 + 1), 0x400000)  # b9 imm32
        self.assertEqual(imm(0x2589 + 4), 0x400000)
        self.assertEqual(self.img[0x2352:0x2358], bytes.fromhex('83c920c1e111'))  # (tdsize|0x20)<<17
        self.assertEqual(imm(0x25da + 4), 0x400000)
        self.assertEqual(imm(0x25e2 + 4), 0x1800)  # Link
        self.assertEqual(imm(0x2eba5 + 2), 0x400000)  # 41 b8 imm32
        self.assertEqual(imm(0x2ebb0 + 4), 0x2020)  # No-op, IOC
        self.assertEqual(len(rows), 7)

    def test_no_other_interrupter1_immediate(self):
        # imm32 0x400000 in all of __TEXT: the five table sites plus PORTSC bit 22 in
        # AppleUSBXHCIPort::interruptOccurred (`or $0x400000,%eax` at +0x22082), which is not a TRB
        hits = [m.start() for m in re.finditer(re.escape(b'\x00\x00\x40\x00'), self.img)]
        self.assertEqual(hits, [0x20c8, 0x2321, 0x258d, 0x25de, 0x22083, 0x2eba7])
        self.assertEqual(self.img[0x22082], 0x0d)  # or imm32,%eax
        # Request::prepare [0x1a7e, 0x2cb4) and SparseRequest::prepare [0x2ea7e, 0x2ec00) hold the table sites
        for _, rva, *_ in source_sites():
            self.assertTrue(0x1a7e <= rva < 0x2cb4 or 0x2ea7e <= rva < 0x2ec00, hex(rva))

    def test_addtd_link_trbs_not_event_generating(self):
        # addTD(td, idx, interrupter, flags): dw2 = interrupter << 22, dw3 = flags | 0x1801 (Link, cycle)
        self.assertEqual(self.img[0x3db2:0x3db6], bytes.fromhex('41c1e416'))
        self.assertEqual(self.img[0x3dbb:0x3dc2], bytes.fromhex('4181cf01180000'))
        call = bytes.fromhex('b90100000041b810000000')  # interrupter 1, flags 0x10 = Chain, no IOC (0x20)
        for rva in (0x24bc, 0x2557, 0x2eb5f):
            self.assertEqual(self.img[rva:rva + len(call)], call, hex(rva))

    def test_isochronous_untouched(self):
        imm = lambda rva: struct.unpack_from('<I', self.img, rva)[0]
        self.assertEqual(imm(0x17d4b + 4), 0x800000)  # IsochronousRequest: interrupter 2
        self.assertEqual(imm(0x181d3 + 4), 0x800000)
        for _, rva, ln, *_ in source_sites():
            self.assertFalse(0x17000 <= rva < 0x1a000, rva)

    def test_primary_dispatches_transfer_events_like_io(self):
        jt = 0xd29c
        self.assertEqual(jt + struct.unpack_from('<i', self.img, jt)[0], 0xc4e3)  # type 32 Transfer Event
        prim = self.img[0xca75:0xca99]
        io = self.img[0xc2a5:0xc2c9]
        # mov slot,%eax; shl $4; mov 0x640(%rbx,%rax),%rdi; test; je; mov (%rdi),%rax; ...; call *0x178(%rax)
        self.assertEqual(prim[3:14], io[3:14])
        self.assertEqual(prim[-6:], bytes.fromhex('ff9078010000'))
        self.assertEqual(io[-6:], bytes.fromhex('ff9078010000'))
        self.assertEqual(self.img[0xc985:0xc989], bytes.fromhex('41c1ef18'))  # slot = dw3 >> 24
        self.assertEqual(self.img[0xc120:0xc124], bytes.fromhex('41c1ed18'))  # io: same decode

    def test_event_rings_same_size(self):
        self.assertEqual(self.img[0x14142:0x14147], bytes.fromhex('ba00400000'))  # 0x4000 bytes per ring
        self.assertEqual(self.img[0x14147:0x1414c], bytes.fromhex('b900400000'))


if __name__ == '__main__':
    unittest.main()
