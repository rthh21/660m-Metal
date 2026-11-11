#!/usr/bin/env python3
"""Offline contract for wlandiag.cpp (-rmbnowlandiag): the 25G229 IOPCIFamily bytes (BootKernelExtensions.kc) whose
private fields the diagnostic reads, and the read-only shape of the module. CPU only; the Boot KC is an external input
(evidence/ next to the repository, or WLANDIAG_BOOT_KC)."""
from pathlib import Path
import os, re, unittest

from test_iogdiag import Kc

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / 'src/Rembrandt660M/wlandiag.cpp'
BOOT_KC = Path(os.environ.get('WLANDIAG_BOOT_KC', ROOT.parent / 'evidence' / 'BootKernelExtensions.kc'))
BOOT_VM_TO_FILE = 0xffffff8000100000
IOPCI_VM = 0xffffff80029e5000


def src_bytes(name):
    m = re.search(r'const uint8_t ' + name + r'\[(\d+)\] \{([^}]*)\};', SRC.read_text())
    vals = bytes(int(v, 16) for v in re.findall(r'0x([0-9a-f]{2})', m.group(2)))
    assert len(vals) == int(m.group(1)), name
    return vals


def src_const(name):
    return int(re.search(r'constexpr uint32_t ' + name + r' = (0x[0-9a-f]+);', SRC.read_text()).group(1), 16)


@unittest.skipUnless(BOOT_KC.exists(), 'Boot KC not in workspace: ' + str(BOOT_KC))
class WlanDiagIopci(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.kc = Kc(BOOT_KC)
        cls.off = IOPCI_VM - BOOT_VM_TO_FILE

    def b(self, rva, n):
        return self.kc.read(self.off + rva, n)

    def test_image_identity(self):
        uid, _, ents = self.kc.header(0)
        self.assertEqual(uid, '58904DC1-52BD-302C-04C4-998D4364475C')
        self.assertIn(('com.apple.iokit.IOPCIFamily', IOPCI_VM, self.off), ents)
        uid, segs, _ = self.kc.header(self.off)
        self.assertEqual(uid, '0D91F1AF-7411-3A0F-9A7E-72D59A2DA75F')
        self.assertEqual(segs['__TEXT'], (IOPCI_VM, 0x54000))
        self.assertEqual(src_bytes('IopciUuid').hex().upper(), '0D91F1AF74113A0F9A7E72D59A2DA75F')

    def test_runtime_checked_bytes_match_kc(self):
        for rva, name in (('RvaConfigAccessGate', 'ConfigAccessGate'), ('RvaPublishNubShadow', 'PublishNubShadow'),
                          ('RvaProbeBusHeader', 'ProbeBusHeader')):
            want = src_bytes(name)
            self.assertLessEqual(src_const(rva) + len(want), 0x54000)
            self.assertEqual(self.b(src_const(rva), len(want)), want, name)
        # extendedConfigRead32: our own routePciDevid long route rewrites the head at boot, so only +0x0f.. is checked
        skip = src_const('ExtendedConfigRead32Skip')
        self.assertEqual(skip, 0x0f)
        want = src_bytes('ExtendedConfigRead32Body')
        self.assertEqual(self.b(src_const('RvaExtendedConfigRead32') + skip, len(want)), want)
        # instructions before the skip end exactly at +0x0f (push rbp; mov rbp,rsp; push r14; push rbx; mov r14,rsi;
        # mov rbx,rdi; xor esi,esi) and cover Lilu's 14-byte long jump
        self.assertEqual(self.b(src_const('RvaExtendedConfigRead32'), skip).hex(), '554889e54156534989f64889fb31f6')
        # symbol + offset of each checked window (iopci.syms from the same image)
        self.assertEqual(src_const('RvaConfigAccessGate'), 0xffffff8002a022be + 0x16 - IOPCI_VM)
        self.assertEqual(src_const('RvaPublishNubShadow'), 0xffffff80029ee45c + 0x1e5 - IOPCI_VM)
        self.assertEqual(src_const('RvaProbeBusHeader'), 0xffffff80029eff30 + 0x1f5 - IOPCI_VM)
        self.assertEqual(src_const('RvaExtendedConfigRead32'), 0xffffff8002a0067a - IOPCI_VM)

    def test_extended_read_goes_through_config_access(self):
        rva = src_const('RvaExtendedConfigRead32')
        call = self.b(rva + 0x0f, 5)
        self.assertEqual(call[0], 0xe8)
        self.assertEqual(rva + 0x14 + int.from_bytes(call[1:], 'little', signed=True), 0xffffff8002a022be - IOPCI_VM)
        # space = (this+0xa8 & 0xf0ffffff) | (offset << 16 & 0x0f000000), then vtable +0x850 configRead32(space, off):
        # the encoding Raw::x32 uses for extended offsets.
        # mov %r14d,%eax; shl $16,%eax; mov $0xf0ffffff,%esi; and 0xa8(%rbx),%esi; and $0x0f000000,%eax; or %eax,%esi;
        # mov (%rbx),%rax; mov 0x850(%rax),%rax (jmp *%rax follows)
        self.assertEqual(self.b(rva + 0x21, 0x22).hex(), '4489f0' + 'c1e010' + 'befffffff0' + '23b3a8000000'
                         + '250000000f' + '09c6' + '488b03' + '488b8050080000')

    def test_extended_read_encoding_in_source(self):
        s = SRC.read_text()
        self.assertIn('(sp.bits & 0xf0ffffffU) | ((static_cast<uint32_t>(o) << 16) & 0x0f000000U)', s)


class WlanDiagSource(unittest.TestCase):
    def test_kill_switch_and_wiring(self):
        s = SRC.read_text()
        self.assertIn('"-rmbnowlandiag"', s)
        self.assertIn('wlanDiagStart();', (ROOT / 'src/Rembrandt660M/kern_start.cpp').read_text())
        self.assertIn('wlandiag.o', (ROOT / 'src/Rembrandt660M/Makefile').read_text())

    def test_read_only(self):
        s = SRC.read_text()
        for word in ('configWrite', 'extendedConfigWrite', 'setProperty', 'removeProperty', 'mmioWrite', 'IOMap',
                     'registerService', 'kernelRequestProbe', 'protectDevice', 'IODelay'):
            self.assertNotIn(word, s)
        # private fields only behind the image check; IOPCIFamily is not imported (make check: kernel + Lilu only)
        self.assertIn('if (!gPrivOk) {', s)
        self.assertIn('gExtRead32 = reinterpret_cast<ExtRead32>(base + RvaExtendedConfigRead32);', s)
        self.assertNotIn('->extendedConfigRead', s)
        self.assertNotIn('configRead32(static_cast<UInt8>', s)  # resolves to the inline extendedConfigRead32 import

    def test_publish_callback_only_records(self):
        s = SRC.read_text()
        cb = s[s.index('bool pciPublished('):s.index('void diagThread(')]
        self.assertNotIn('snap', cb)
        self.assertNotIn('configRead', cb)

    def test_log_buffer_is_bounded(self):
        s = SRC.read_text()
        self.assertNotIn('k += snprintf', s)
        self.assertIn('if (k >= n - 1)', s)
        self.assertIn('k = k + r < n - 1 ? k + r : n - 1;', s)
        self.assertEqual(s.count('\tbuf[0] = 0;'), 2)  # caps / extCaps always leave a C string

    def test_matches_only_the_three_functions(self):
        s = SRC.read_text()
        self.assertIn('vid == 0x14c3 && did == 0x0616', s)
        self.assertIn('vid == 0x1022 && did == 0x14ba && reg == Gpp7Reg', s)
        self.assertIn('vid == 0x1022 && did == 0x14b5 && reg == HostReg', s)
        self.assertIn('constexpr uint32_t Gpp7Reg = 0x001300;', s)
        self.assertIn('constexpr uint32_t HostReg = 0x000000;', s)

    def test_wlan_reads_are_timed_and_stop_when_slow(self):
        s = SRC.read_text()
        self.assertIn('constexpr unsigned SlowReadUs = 2000;', s)
        rd = s[s.index('Timed wlanRead32('):s.index('// snprintf at buf + k')]
        self.assertIn('mach_absolute_time()', rd)
        self.assertIn('if (us > SlowReadUs && !gWlanReadsStopped) {', rd)
        self.assertIn('gWlanReadsStopped = true;', rd)
        # the watch reads the WLAN only through wlanRead32 and never after a slow read
        w = s[s.index('struct Watch {'):s.index('void snapWlan(')]
        self.assertIn('bool read = !gWlanReadsStopped && secBus && secBus != 0xff && wlanFixState() != 1;', w)
        self.assertEqual(w.count('wlanRead32('), 1)
        self.assertNotIn('<< 16)}, 0x04', w)
        # snapshots: first WLAN read timed; the other registers, caps and the nub path only when it answered
        sw = s[s.index('void snapWlan('):s.index('void snapGpp7(')]
        self.assertLess(sw.index('if (gWlanReadsStopped) {'), sw.index('Timed id = wlanRead32(d, 0x00, tag);'))
        self.assertLess(sw.index('if (!alive) {'), sw.index('d.r32(0x04)'))
        self.assertLess(sw.index('if (alive) {'), sw.index('gExtRead32(pci, 0x00)'))
        self.assertEqual(sw.count('caps(d, buf'), 1)
        self.assertEqual(sw.count('extCaps(d, false'), 1)

    def test_watch_runs_in_own_thread_before_publish(self):
        s = SRC.read_text()
        t = s[s.index('void diagThread('):]
        self.assertIn('for (; !gWlan && ms < 90000; ms += WatchMs) {', t)
        self.assertIn('if (gHost)\n\t\t\tw.sample(ms);', t)
        self.assertIn('constexpr unsigned WatchMs = 250, WatchLogMax = 48;', s)
        self.assertIn('if (logged++ < WatchLogMax)', s)


if __name__ == '__main__':
    unittest.main()
