#!/usr/bin/env python3
"""Offline contract for display.cpp edidRepair (-rmbnoedidfix) and the curdiag cursor counters (-rmbnocurdiag).

Checks the 25G229 AMDRadeonX6000Framebuffer bytes (KDK, external input) behind the offsets edidRepair patches:
AmdLink::detectLink copies the DAL status at link+0x50 (0x440 bytes) to the caller's DetectedLinkStatus,
compares the cached EDID link+0x4a0 with link+0x60 (1024 bytes), cacheLinkStatus copies the caller status to
link+0x490, connectionChange commits getConnectedLinkStatus (link+0x50) into link+0x8d0 with
commitLinkStatus, getDDCBlock reads getActiveLinkStatus (link+0x8d0) with size at +0xc and data at +0x10,
and the eDP signal value is 0x80. Also the cursor commit path the curdiag counters describe.
CPU only, no hardware access.
"""
from pathlib import Path
import glob, os, re, subprocess, unittest

KDK = Path(os.environ.get('KDK_EXTENSIONS', '/Library/Developer/KDKs/KDK_26.7_25G229.kdk/System/Library/Extensions'))
FB = KDK / 'AMDRadeonX6000Framebuffer.kext/Contents/MacOS/AMDRadeonX6000Framebuffer'
FB_UUID = 'C3FA8B38-0702-3FCD-9878-C8FD05A7A724'
SRC = Path(__file__).resolve().parents[1] / 'src/Rembrandt660M/display.cpp'
SYMS = Path(__file__).resolve().parents[1] / 'src/Rembrandt660M/tahoe_symbols_25G229.hpp'


def objdump():
    tools = sorted(glob.glob('/usr/lib/llvm-*/bin/llvm-objdump')) + ['llvm-objdump']
    for t in tools:
        if os.path.exists(t) or t == 'llvm-objdump':
            return t


def disasm(start, stop):
    out = subprocess.run([objdump(), '-d', '--no-show-raw-insn', '--no-print-imm-hex', '--start-address=' + hex(start),
                          '--stop-address=' + hex(stop), str(FB)], capture_output=True, text=True, check=True).stdout
    rows = {}
    for line in out.splitlines():
        m = re.match(r'\s*([0-9a-f]+):\s+(.*)', line)
        if m:
            rows[int(m.group(1), 16)] = re.sub(r'\s+', ' ', re.sub(r'<[^>]*>', '', m.group(2).split('##')[0])).strip()
    return rows


def nm():
    out = subprocess.run(['llvm-nm', str(FB)], capture_output=True, text=True, check=True).stdout
    return {p[2]: int(p[0], 16) for p in (l.split() for l in out.splitlines()) if len(p) == 3}


@unittest.skipUnless(FB.exists(), 'KDK framebuffer not in workspace: ' + str(FB))
class EdidRepairContract(unittest.TestCase):
    def test_uuid(self):
        out = subprocess.run([objdump(), '--macho', '--private-headers', str(FB)], capture_output=True, text=True,
                             check=True).stdout
        self.assertIn('uuid ' + FB_UUID, out)

    def test_symbols(self):
        s = nm()
        self.assertEqual(s['__ZN22AMDRadeonX6000_AmdLink10detectLinkE17AmdFbDetectReasonP18DetectedLinkStatus'], 0x45830)
        self.assertEqual(s['__ZN22AMDRadeonX6000_AmdLink15cacheLinkStatusEP18DetectedLinkStatus'], 0x45b1a)
        self.assertEqual(s['__ZN22AMDRadeonX6000_AmdLink16commitLinkStatusEP18DetectedLinkStatus'], 0x46284)
        self.assertEqual(s['__ZN22AMDRadeonX6000_AmdLink22getConnectedLinkStatusEv'], 0x46498)
        self.assertEqual(s['__ZN22AMDRadeonX6000_AmdLink19getActiveLinkStatusEv'], 0x464b0)

    def test_detect_link_copies(self):
        r = disasm(0x458c7, 0x45990)
        self.assertEqual(r[0x458c7], 'leaq 80(%rbx), %r12')  # DAL status = link+0x50
        self.assertEqual(r[0x4592f], 'cmpl $32, %eax')  # DP / MST / eDP signal values
        self.assertEqual(r[0x45934], 'cmpl $64, %eax')
        self.assertEqual(r[0x45939], 'cmpl $128, %eax')  # eDP = 0x80
        self.assertEqual(r[0x4594a], 'movl $1088, %edx')  # memcpy(caller status, link+0x50, 0x440)
        self.assertEqual(r[0x4594f], 'movq %r14, %rdi')
        self.assertEqual(r[0x45952], 'movq %r12, %rsi')
        self.assertEqual(r[0x45975], 'leaq 1184(%rbx), %rdi')  # cached EDID link+0x4a0
        self.assertEqual(r[0x4597c], 'leaq 96(%rbx), %rsi')  # new EDID link+0x60
        self.assertEqual(r[0x45980], 'movl $1024, %edx')

    def test_cache_and_commit(self):
        c = disasm(0x45b1a, 0x45b65)
        self.assertEqual(c[0x45b32], 'movl (%rsi), %eax')  # status +0 = link index
        self.assertEqual(c[0x45b34], 'movl 24(%rdi), %r8d')
        self.assertEqual(c[0x45b3d], 'addq $1168, %rdi')  # cache link+0x490
        self.assertEqual(c[0x45b44], 'movl $1088, %edx')
        m = disasm(0x46284, 0x462c0)
        self.assertEqual(m[0x462a1], 'movl (%rsi), %eax')
        self.assertEqual(m[0x462ac], 'leaq 2256(%rbx), %rdi')  # active link+0x8d0
        self.assertEqual(m[0x462b3], 'movl $1088, %edx')
        self.assertEqual(disasm(0x46498, 0x464a2)[0x4649c], 'leaq 80(%rdi), %rax')  # connected = link+0x50
        self.assertEqual(disasm(0x464b0, 0x464bd)[0x464b4], 'leaq 2256(%rdi), %rax')  # active = link+0x8d0
        cc = disasm(0x163bc, 0x163d7)
        self.assertEqual(cc[0x163c2], 'callq *400(%rax)')  # getConnectedLinkStatus
        self.assertEqual(cc[0x163d1], 'callq *312(%rcx)')  # commitLinkStatus

    def test_get_ddc_block_reads_active_status(self):
        r = disasm(0x5979, 0x59a1)
        self.assertEqual(r[0x598a], 'callq *424(%rcx)')  # getActiveLinkStatus
        self.assertEqual(r[0x5990], 'movl 12(%rax), %r10d')  # EDID bytes at +0xc
        self.assertEqual(r[0x5994], 'addq $12, %rax')
        d = disasm(0x59a1, 0x59ac)
        self.assertEqual(d[0x59a4], 'leaq (%rax,%rcx), %rsi')
        self.assertEqual(d[0x59a8], 'addq $4, %rsi')  # data at +0xc + 4 = +0x10

    def test_source_offsets(self):
        s = SRC.read_text()
        self.assertIn('EdidStatusOff = 0x50, EdidCacheOff = 0x490, EdidActiveOff = 0x8d0, EdidStatusSize = 0xc,', s)
        self.assertIn('EdidStatusData = 0x10, EdidSignalEdp = 0x80, EdidMaxBytes = 1024;', s)
        self.assertIn('gEdidFix = !checkKernelArgument("-rmbnoedidfix");', s)
        self.assertIn('edidRepair(link, status);', s)
        self.assertIn('if (edidSum(edid + b * 128))', s)


@unittest.skipUnless(FB.exists(), 'KDK framebuffer not in workspace: ' + str(FB))
class CursorPathContract(unittest.TestCase):
    def test_set_cursor_state_defers_commit_to_vupdate(self):
        r = disasm(0x16715, 0x16757)
        self.assertEqual(r[0x16715], 'movq 35328(%rbx), %rdi')  # AmdCursor
        self.assertEqual(r[0x16729], 'callq *344(%rax)')  # saveCursorPosition
        self.assertTrue(r[0x1673e].startswith('callq 0x2ad56'))  # AmdFbPresenter::isEnabled
        self.assertTrue(r[0x16745].startswith('jne 0x16757'))  # enabled -> no commit here
        self.assertEqual(r[0x16751], 'callq *352(%rax)')  # commitCursorPosition otherwise
        v = disasm(0xb38a, 0xb39a)
        self.assertEqual(v[0xb38a], 'movq 35328(%rbx), %rdi')
        self.assertEqual(v[0xb394], 'callq *352(%rax)')  # HandleVerticalUpdateInterrupt commits every vupdate

    def test_curdiag_symbols(self):
        s = nm()
        want = {'__ZN35AMDRadeonX6000_AmdRadeonFramebuffer14setCursorImageEPv': 0x164b8,
                '__ZN35AMDRadeonX6000_AmdRadeonFramebuffer14setCursorStateEiib': 0x165f8,
                '__ZN24AMDRadeonX6000_AmdCursor19commitCursorUpdatesEv': 0x3247a,
                '__ZN29AMDRadeonX6000_AmdFbPresenter18dequeueTransationsEy': 0x2c930}
        src = SYMS.read_text()
        for name, rva in want.items():
            self.assertEqual(s[name], rva, name)
            self.assertIn('{"%s", 1, %#xu}' % (name, rva), src)


if __name__ == '__main__':
    unittest.main(verbosity=2)
