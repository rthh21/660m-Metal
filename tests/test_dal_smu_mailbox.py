#!/usr/bin/env python3
"""Offline contract for display.cpp's Navi DAL SMU mailbox shim (-rmbnavismu restores pass-through).

Checks the 25G229 AMDRadeonX6000Framebuffer bytes (KDK, external input) that the shim relies on:
dcn30_smu_wait_for_response returns the first nonzero read of C2PMSG_52 (0x16274), a 0xFE reply makes
dcn30_smu_send_msg_with_param return false without reading the arg register, dm_helpers_smu_timeout is
empty, dcn3_init_clocks gates smu_present on GetSmuVersion, and no other FB code touches the mailbox.
CPU only, no hardware access.
"""
from pathlib import Path
import glob, os, re, subprocess, unittest

KDK = Path(os.environ.get('KDK_EXTENSIONS', '/Library/Developer/KDKs/KDK_26.7_25G229.kdk/System/Library/Extensions'))
FB = KDK / 'AMDRadeonX6000Framebuffer.kext/Contents/MacOS/AMDRadeonX6000Framebuffer'
FB_UUID = 'C3FA8B38-0702-3FCD-9878-C8FD05A7A724'
SRC = Path(__file__).resolve().parents[1] / 'src/Rembrandt660M/display.cpp'


def objdump():
    tools = sorted(glob.glob('/usr/lib/llvm-*/bin/llvm-objdump')) + ['llvm-objdump']
    for t in tools:
        if os.path.exists(t) or t == 'llvm-objdump':
            return t


def disasm(start, stop):
    out = subprocess.run([objdump(), '-d', '--no-show-raw-insn', '--start-address=' + hex(start),
                          '--stop-address=' + hex(stop), str(FB)], capture_output=True, text=True, check=True).stdout
    rows = []
    for line in out.splitlines():
        m = re.match(r'\s*([0-9a-f]+):\s+(.*)', line)
        if m:
            rows.append((int(m.group(1), 16), re.sub(r'\s+', ' ', m.group(2).split('##')[0]).strip()))
    return rows


@unittest.skipUnless(FB.exists(), 'KDK framebuffer not in workspace: ' + str(FB))
class DalSmuMailbox(unittest.TestCase):
    def test_uuid(self):
        out = subprocess.run([objdump(), '--macho', '--private-headers', str(FB)], capture_output=True, text=True,
                             check=True).stdout
        self.assertIn('uuid ' + FB_UUID, out)

    def test_wait_returns_first_nonzero_resp(self):
        rows = dict(disasm(0x906f0, 0x9073d))
        self.assertEqual(rows[0x906fe], 'movl $4294767295, %r12d')  # -200001: 200001 polls
        self.assertEqual(rows[0x9070e], 'movl $90740, %esi')  # 0x16274 = C2PMSG_52
        self.assertEqual(rows[0x9071e], 'testl %eax, %eax')
        self.assertTrue(rows[0x90720].startswith('jne 0x90731'))  # nonzero -> return it at once
        self.assertEqual(rows[0x90722], 'movl $10, %edi')  # udelay(10) between polls
        self.assertEqual(rows[0x90731], 'movl %r15d, %eax')

    def test_unknown_cmd_is_failure_without_timeout_or_arg_read(self):
        rows = dict(disasm(0x90082, 0x90170))
        writes = [rows[a] for a in (0x900a8, 0x900ca, 0x900ee)]
        self.assertEqual(writes, ['movl $90740, %esi', 'movl $90739, %esi', 'movl $90762, %esi'])  # resp, arg, msg
        self.assertEqual(rows[0x90116], 'cmpl $1, %eax')
        self.assertTrue(rows[0x90119].startswith('je 0x90124'))
        self.assertEqual(rows[0x9011b], 'cmpl $254, %r13d')  # 0xFE skips the timeout helper
        self.assertTrue(rows[0x90122].startswith('jne 0x90147'))
        self.assertEqual(rows[0x90129], 'cmpl $1, %r13d')  # arg (0x16273) read only for result 1
        self.assertTrue(rows[0x9012d].startswith('jne 0x9015a'))
        self.assertEqual(rows[0x9015a], 'cmpl $1, %r13d')
        self.assertEqual(rows[0x9015e], 'sete %al')  # return value: result == 1

    def test_timeout_helper_is_empty(self):
        rows = [r[1] for r in disasm(0x7fbaf, 0x7fbb5)]
        self.assertEqual(rows, ['pushq %rbp', 'movq %rsp, %rbp', 'popq %rbp', 'retq'])

    def test_init_clocks_gates_smu_present_on_get_smu_version(self):
        rows = dict(disasm(0xf9553, 0xf9585))
        self.assertTrue(rows[0xf9563].startswith('callq 0x90170'))  # GetSmuVersion wrapper (msg 2)
        self.assertEqual(rows[0xf9568], 'testb %al, %al')
        self.assertEqual(rows[0xf956c], 'movb $1, 452(%rbx)')  # smu_present = true only on success
        self.assertTrue(rows[0xf957c].startswith('je 0xf98fd'))  # else return
        msg2 = dict(disasm(0x90170, 0x901dd))
        self.assertIn('movl $2, %esi', msg2.values())

    def test_no_other_fb_mailbox_users(self):
        out = subprocess.run([objdump(), '-d', '--no-show-raw-insn', str(FB)], capture_output=True, text=True,
                             check=True).stdout
        hits = sorted(int(m.group(1), 16) for m in re.finditer(r'^\s*([0-9a-f]+):\s+\S+\s+\$(?:90739|90740|90762),',
                                                                 out, re.M))
        self.assertEqual(hits, [0x900a8, 0x900b7, 0x900ca, 0x900da, 0x900ee, 0x900fe, 0x90139, 0x9070e])

    def test_source_shim_matches(self):
        s = SRC.read_text()
        self.assertIn('NaviDalSmuArg = 0x16273, NaviDalSmuResp = 0x16274, NaviDalSmuMsg = 0x1628a', s)
        self.assertIn('DalsmcResultUnknownCmd = 0xFE', s)
        self.assertIn('gNaviSmu = checkKernelArgument("-rmbnavismu");', s)
        self.assertIn('return r == NaviDalSmuResp ? DalsmcResultUnknownCmd : gNaviSmuArg;', s)


if __name__ == '__main__':
    unittest.main(verbosity=2)
