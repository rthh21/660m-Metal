#!/usr/bin/env python3
"""Offline contract for iogdiag.cpp (-rmbnoiogdiag): the 25G229 IOGraphicsFamily (SystemKernelExtensions.kc) and
IOUSBHostHIDDevice (BootKernelExtensions.kc) bytes behind the cursor / gate / USB HID counters. CPU only; the
kernel collections are external inputs (evidence/ next to the repository, or IOGDIAG_SYSTEM_KC / IOGDIAG_BOOT_KC)."""
from pathlib import Path
import os, re, struct, unittest, uuid

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / 'src/Rembrandt660M/iogdiag.cpp'
EVID = ROOT.parent / 'evidence'
SYSTEM_KC = Path(os.environ.get('IOGDIAG_SYSTEM_KC', EVID / 'SystemKernelExtensions.kc'))
BOOT_KC = Path(os.environ.get('IOGDIAG_BOOT_KC', EVID / 'BootKernelExtensions.kc'))
IOG = 0x14ce4000  # System KC: vmaddr == fileoff
BOOT_VM_TO_FILE = 0xffffff8000100000
HID_VM = 0xffffff8002fb0000


def src_bytes(name):
    m = re.search(r'const uint8_t ' + name + r'\[(\d+)\] \{([^}]*)\};', SRC.read_text())
    vals = bytes(int(v, 16) for v in re.findall(r'0x([0-9a-f]{2})', m.group(2)))
    assert len(vals) == int(m.group(1)), name
    return vals


def src_const(name):
    return int(re.search(r'constexpr uint32_t ' + name + r' = (0x[0-9a-f]+);', SRC.read_text()).group(1), 16)


class Kc:
    def __init__(self, path):
        self.f = open(path, 'rb')

    def read(self, off, n):
        self.f.seek(off)
        return self.f.read(n)

    def header(self, off):
        """(uuid, {segname: (vmaddr, vmsize)}, [(fileset entry name, vmaddr, fileoff)])"""
        magic, _, _, _, ncmds, size = struct.unpack('<IiiIII', self.read(off, 24))
        assert magic == 0xfeedfacf
        cmds, at, uid, segs, ents = self.read(off + 32, size), 0, None, {}, []
        for _ in range(ncmds):
            cmd, cs = struct.unpack_from('<II', cmds, at)
            if cmd == 0x1b:
                uid = str(uuid.UUID(bytes=cmds[at + 8:at + 24])).upper()
            elif cmd == 0x19:
                segs[cmds[at + 8:at + 24].split(b'\0')[0].decode()] = struct.unpack_from('<QQ', cmds, at + 24)
            elif cmd == 0x80000035:
                va, fo, no = struct.unpack_from('<QQI', cmds, at + 8)
                ents.append((cmds[at + no:at + cs].split(b'\0')[0].decode(), va, fo))
            at += cs
        return uid, segs, ents

    def ptr(self, off):
        return struct.unpack('<Q', self.read(off, 8))[0] & 0x3fffffff  # chained fixup target (KC vmaddr)


@unittest.skipUnless(SYSTEM_KC.exists(), 'System KC not in workspace: ' + str(SYSTEM_KC))
class IogDiagIoGraphics(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.kc = Kc(SYSTEM_KC)

    def b(self, addr, n):
        return self.kc.read(addr, n)

    def test_image_identity(self):
        uid, _, ents = self.kc.header(0)
        self.assertEqual(uid, '1CE3B896-9A0D-E9BF-C1B9-2213048203A4')
        self.assertIn(('com.apple.iokit.IOGraphicsFamily', IOG, IOG), ents)
        uid, segs, _ = self.kc.header(IOG)
        self.assertEqual(uid, '74AA6483-8AB4-3A94-8143-5E306C16D7DA')
        self.assertEqual(segs['__TEXT'], (IOG, 0x2f000))
        self.assertIn('0x74, 0xaa, 0x64, 0x83, 0x8a, 0xb4, 0x3a, 0x94, 0x81, 0x43, 0x5e, 0x30, 0x6c, 0x16, 0xd7, 0xda',
                      SRC.read_text())

    def test_runtime_checked_bytes_match_source(self):
        for rva, name in ((src_const('RvaExtSetCursorPosition'), 'ExtSetCursorPositionPro'),
                          (src_const('RvaCloseGateHook'), 'CloseGateHookBytes'),
                          (src_const('RvaOpenGateHook'), 'OpenGateHookBytes')):
            want = src_bytes(name)
            self.assertLessEqual(rva + len(want), 0x2f000)
            self.assertEqual(self.b(IOG + rva, len(want)), want, name)
        self.assertEqual(src_const('RvaExtSetCursorPosition'), 0x14cf8bb8 - IOG)
        self.assertEqual(src_const('RvaCloseGateHook'), 0x14ced1da + 0x3f - IOG)
        self.assertEqual(src_const('RvaOpenGateHook'), 0x14ced264 + 0x20 - IOG)

    def test_ext_set_cursor_position_waits_for_controller_gate_then_moves(self):
        self.assertEqual(self.b(0x14cf8bda, 4).hex(), '4c8b7a20')  # args->scalarInput (+0x20)
        self.assertEqual(self.b(0x14cf8ca2, 4).hex(), '4d8b7710')  # frame = scalarInput[2]
        # wl = *(*(fb+0x1c8)) + 0x120 -> timedCloseGate, then vtable +0x178 (openGate), then moveCursorImpl
        self.assertEqual(self.b(0x14cf8ce6, 17).hex(), '488b83c8010000488b004c8ba020010000')
        self.assertEqual(self.b(0x14cf8d04, 5).hex(), 'e845' + '76ffff')
        self.assertEqual(self.b(0x14cf8d9a, 6).hex(), 'ff9078010000')
        self.assertEqual(self.b(0x14cf8daa, 5).hex(), 'e859f2ffff')  # -> 0x14cf8008 moveCursorImpl
        self.assertEqual(self.b(0x14cf8c76, 6).hex(), '41bfc20200e0')  # out of bounds: e00002c2
        # timedCloseGate: closeGate through vtable +0x180
        self.assertEqual(self.b(0x14cf0392, 6).hex(), 'ff9080010000')

    def test_move_cursor_preconditions_read_by_cursor_pre(self):
        self.assertEqual(self.b(0x14cf802c, 11).hex(), '488b83c8010000443b7030')  # frame >= priv+0x30
        self.assertEqual(self.b(0x14cf803d, 7).hex(), 'f6837001000010')  # fb+0x170 & 0x10
        self.assertEqual(self.b(0x14cf805a, 7).hex(), '4c8bbb90000000')  # shmem fb+0x90
        self.assertEqual(self.b(0x14cf806d, 5)[0], 0xe8)  # OSSpinLockTry(shmem)
        self.assertEqual(self.b(0x14cf7ab9, 8).hex(), '4180bec900000000')  # hardwareCursorActive shmem+0xc9
        self.assertEqual(self.b(0x14cf7c66, 5).hex(), '41807e0800')  # cursorShow shmem+8
        self.assertEqual(self.b(0x14cf7ccf, 8).hex(), '4180bac900000001')  # == 1: synchronous
        self.assertEqual(self.b(0x14cf7cdc, 7).hex(), '488b80780a0000')  # vtable +0xa78 setCursorState

    def test_controller_work_loop_class_and_callback(self):
        # IOFBController::init: new IOGraphicsControllerWorkLoop, vtable 0x14d18d28, +0x60/+0x68 = r14 = 0
        self.assertEqual(self.b(0x14ced8a2, 3).hex(), '4531f6')
        self.assertEqual(self.b(0x14ced8fd, 7).hex(), '488d0524b40200')  # lea 0x14d18d28
        self.assertEqual(0x14ced904 + 0x2b424, 0x14d18d28)
        # mov %r14,0x60(%r15); mov %r14,0x68(%r15); mov %rbx,0x70(%r15): callback NULL, obj NULL, ref = controller
        self.assertEqual(self.b(0x14ced917, 12).hex(), '4d8977604d89776849895f70')
        self.assertEqual(self.b(0x14ced93f, 7).hex(), '4c89bb20010000')  # controller+0x120 = wl
        self.assertEqual(src_const('RvaControllerWorkLoopVtbl'), 0x14d18d28 - IOG)
        self.assertEqual(src_const('RvaGraphicsWorkLoopVtbl'), 0x14d18718 - IOG)
        ctrl, base = 0x14d18d18 + 0x10, 0x14d18708 + 0x10
        self.assertEqual(self.kc.ptr(ctrl + 0x178), 0x14ced264)  # IOGraphicsWorkLoop::openGate
        self.assertEqual(self.kc.ptr(ctrl + 0x180), 0x14d0710c)  # IOGraphicsControllerWorkLoop::closeGate
        self.assertEqual(self.kc.ptr(ctrl + 0x190), 0x14d07118)  # IOGraphicsControllerWorkLoop::sleepGate
        self.assertEqual(self.kc.ptr(base + 0x178), 0x14ced264)
        self.assertEqual(self.kc.ptr(base + 0x180), 0x14ced1da)
        # the overrides only forward to IOGraphicsWorkLoop
        self.assertEqual(self.b(0x14d0710c, 11).hex(), '554889e5e8c560feff5dc3')  # call 0x14ced1da
        self.assertEqual(self.b(0x14d07118, 11).hex(), '554889e5e84362feff5dc3')  # call 0x14ced364

    def test_gate_callback_states(self):
        # closeGate: owner/count set, then hook(wl, +0x68, +0x70, 1) as a tail call
        self.assertEqual(self.b(0x14ced20e, 11).hex(), '4c897350c7435801000000')
        self.assertEqual(self.b(0x14ced23c, 2).hex(), 'ffe0')
        # openGate: only the outermost release (count == 1) calls hook(..., 0) before clearing the owner
        self.assertEqual(self.b(0x14ced27f, 3).hex(), '83f801')
        self.assertEqual(self.b(0x14ced2a8, 8).hex(), '48c7435000000000')
        # sleepGate: hook(..., 0) before sleeping, hook(..., 1) after re-acquiring
        self.assertEqual(self.b(0x14ced39b, 4).hex(), '488b4360')
        self.assertEqual(self.b(0x14ced3af, 4).hex(), '31c9ffd0')
        self.assertEqual(self.b(0x14ced3e2, 4).hex(), '488b4360')
        self.assertEqual(self.b(0x14ced3f6, 7).hex(), 'b901000000ffd0')


@unittest.skipUnless(BOOT_KC.exists(), 'Boot KC not in workspace: ' + str(BOOT_KC))
class IogDiagUsbHid(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.kc = Kc(BOOT_KC)

    def test_image_identity(self):
        uid, _, ents = self.kc.header(0)
        self.assertEqual(uid, '58904DC1-52BD-302C-04C4-998D4364475C')
        self.assertIn(('com.apple.driver.usb.IOUSBHostHIDDevice', HID_VM, HID_VM - BOOT_VM_TO_FILE), ents)
        uid, segs, _ = self.kc.header(HID_VM - BOOT_VM_TO_FILE)
        self.assertEqual(uid, 'EC3E3338-6B2D-3DF1-A753-B35AD4AF9A25')
        self.assertEqual(segs['__TEXT'], (HID_VM, 0x7000))
        self.assertIn('0xec, 0x3e, 0x33, 0x38, 0x6b, 0x2d, 0x3d, 0xf1, 0xa7, 0x53, 0xb3, 0x5a, 0xd4, 0xaf, 0x9a, 0x25',
                      SRC.read_text())

    def test_interrupt_read_complete_prologue_and_args(self):
        rva = src_const('RvaInterruptReadComplete')
        self.assertEqual(rva, 0xffffff8002fb4940 - HID_VM)
        want = src_bytes('InterruptReadCompletePro')
        self.assertEqual(self.kc.read(HID_VM - BOOT_VM_TO_FILE + rva, len(want)), want)
        # (this, void *param, IOReturn status = edx, uint32_t bytes = ecx)
        at = HID_VM - BOOT_VM_TO_FILE + rva
        self.assertEqual(self.kc.read(at + 0x19, 6).hex(), '81faeb0200e0')  # status == kIOReturnAborted
        self.assertEqual(self.kc.read(at + 0x88, 6).hex(), '4189cf4189d6')  # r15d = bytes, r14d = status


class IogDiagSource(unittest.TestCase):
    def test_kill_switches_and_wiring(self):
        s = SRC.read_text()
        for arg in ('-rmbnoiogdiag', '-rmbnogatediag', '-rmbnohiddiag'):
            self.assertIn('"' + arg + '"', s)
        self.assertIn('iogDiagRoute(patcher);', (ROOT / 'src/Rembrandt660M/display.cpp').read_text())
        self.assertIn('iogdiag.o', (ROOT / 'src/Rembrandt660M/Makefile').read_text())

    def test_no_hardware_access_or_behaviour_change(self):
        s = SRC.read_text()
        for word in ('mmioWrite', 'mmioRead', 'vramWrite', 'IODelay', 'setProperty', 'kIOReturn'):
            self.assertNotIn(word, s)
        # every hook forwards to the original with the same arguments and returns its result
        self.assertIn('reinterpret_cast<Fn6>(orgExtSetCursorPosition)(fb, ref, args, d, e, f)', s)
        self.assertIn('reinterpret_cast<Fn6>(orgHidReadComplete)(self, param, status, bytes, e, f)', s)
        self.assertIn('s->org(wl, obj, ref, closing)', s)


if __name__ == '__main__':
    unittest.main()
