#!/usr/bin/env python3
"""Offline contract for display.cpp tgfix (-rmbnotgfix): the 25G229 AMDRadeonX6000Framebuffer bytes behind
the presenter TG fill. CPU only, KDK framebuffer is an external input."""
from pathlib import Path
import glob, os, re, subprocess, unittest

KDK = Path(os.environ.get('KDK_EXTENSIONS', '/Library/Developer/KDKs/KDK_26.7_25G229.kdk/System/Library/Extensions'))
FB = KDK / 'AMDRadeonX6000Framebuffer.kext/Contents/MacOS/AMDRadeonX6000Framebuffer'
SRC = Path(__file__).resolve().parents[1] / 'src/Rembrandt660M/display.cpp'
SYMS = Path(__file__).resolve().parents[1] / 'src/Rembrandt660M/tahoe_symbols_25G229.hpp'


def objdump():
    t = sorted(glob.glob('/usr/lib/llvm-*/bin/llvm-objdump'))
    return t[-1] if t else 'llvm-objdump'


def disasm(start, stop):
    out = subprocess.run([objdump(), '-d', '--no-show-raw-insn', '--start-address=' + hex(start),
                          '--stop-address=' + hex(stop), str(FB)], capture_output=True, text=True, check=True).stdout
    rows = {}
    for line in out.splitlines():
        m = re.match(r'\s*([0-9a-f]+):\s+(.*)', line)
        if m:
            rows[int(m.group(1), 16)] = re.sub(r'\s+', ' ', re.sub(r'<[^>]*>', '', m.group(2).split('##')[0])).strip()
    return rows


@unittest.skipUnless(FB.exists(), 'KDK framebuffer not in workspace: ' + str(FB))
class TgFixContract(unittest.TestCase):
    def test_uuid(self):
        out = subprocess.run([objdump(), '--macho', '--private-headers', str(FB)], capture_output=True, text=True).stdout
        self.assertIn('uuid C3FA8B38-0702-3FCD-9878-C8FD05A7A724', out)

    def test_set_vupdate_interval_reads_stream_tg(self):
        r = disasm(0x2c148, 0x2c1ee)
        self.assertEqual(r[0x2c151], 'movq 176(%rdi), %rax')  # presenter display path +0xb0
        self.assertEqual(r[0x2c158], 'movl 1880(%rax), %ecx')  # path mode +0x758
        self.assertEqual(r[0x2c169], 'movq $0, 240(%rbx)')  # interval +0xf0
        self.assertEqual(r[0x2c180], 'movl 208(%rax), %ecx')  # stream count +0xd0
        self.assertEqual(r[0x2c18b], 'imulq $400, %rcx, %rcx')  # stream size
        self.assertEqual(r[0x2c19b], 'movl 220(%rax,%rdx), %esi')  # stream TG +0xdc
        self.assertEqual(r[0x2c1b4], 'movl $4294967295, %esi')  # no stream -> -1
        self.assertEqual(r[0x2c1b9], 'movq 5720(%rbx), %rdi')  # controller
        self.assertEqual(r[0x2c1c7], 'callq *2464(%rax)')  # getVUpdateVBlankInterval
        self.assertEqual(r[0x2c1e7], 'movq %rcx, 240(%rbx)')

    def test_navi2_interval_reads_otg_register(self):
        r = disasm(0x4c10, 0x4c48)
        self.assertEqual(r[0x4c19], 'cmpl $6, %esi')
        self.assertEqual(r[0x4c21], 'shll $7, %ebx')  # tg * 0x80: OTG block stride
        self.assertEqual(r[0x4c24], 'orl $20551, %ebx')  # 0x5047 = Seg2 0x34c0 + 0x1b87

    def test_tg_written_by_cache_and_init(self):
        c = disasm(0x4ce42, 0x4ceb3)
        self.assertEqual(c[0x4ce42], 'leaq 440(%rdi), %rdx')  # active display paths
        self.assertEqual(c[0x4ce49], 'leaq 660(%rdi), %r12')  # 440 + 220: stream 0 TG
        self.assertEqual(c[0x4cea9], 'callq *216(%rcx)')  # DAL getTgIndex
        self.assertEqual(c[0x4ceaf], 'movl %eax, (%r12)')
        i = disasm(0x1d917, 0x1d933)
        self.assertEqual(i[0x1d917], 'addq $216, %rbx')
        self.assertEqual(i[0x1d929], 'movl $4294967295, 4(%rbx)')  # stream +4 = TG, initialised -1

    def test_deadline_fallback(self):
        r = disasm(0x2bfaf, 0x2bfe0)
        self.assertEqual(r[0x2bfaf], 'cmpq $0, 240(%r14)')
        self.assertTrue(r[0x2bfbc].startswith('callq 0x2c148'))
        self.assertEqual(r[0x2bfd8], 'movq 80(%r14), %rax')  # no interval: deadline = vblank + frame
        self.assertEqual(r[0x2bfdc], 'movq %rax, 88(%r14)')

    def test_source(self):
        s = SRC.read_text()
        self.assertIn('PresPathOff = 0xb0, PresVuIntervalOff = 0xf0, PathStreamCount = 0xd0, PathStream0Tg = 0xdc,', s)
        self.assertIn('PathStreamSize = 400, PathMaxStreams = 4, PathModeOff = 0x758, PathFlagsOff = 0x718;', s)
        self.assertIn('if (n == 1 && mask && !(mask & (mask - 1))) {', s)
        self.assertIn('mmioRead(Seg2 + 0x1b41 + i * 0x80) & 1', s)
        self.assertIn('gTgFix = !checkKernelArgument("-rmbnotgfix");', s)
        self.assertIn('{"__ZN29AMDRadeonX6000_AmdFbPresenter24setVUpdateVBlankIntervalEv", 1, 0x2c148u}', SYMS.read_text())


if __name__ == '__main__':
    unittest.main(verbosity=2)
