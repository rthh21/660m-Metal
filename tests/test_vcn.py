#!/usr/bin/env python3
"""Offline contract for vcn.cpp (opt-in hardware video) and its smu.cpp/display.cpp services. Source only."""
from pathlib import Path
import re, unittest

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / 'src/Rembrandt660M'
VCN = (SRC / 'vcn.cpp').read_text()
SMU = (SRC / 'smu.cpp').read_text()
DSP = (SRC / 'display.cpp').read_text()
KS = (SRC / 'kern_start.cpp').read_text()
PRB = (SRC / 'probe.cpp').read_text()
SYM = (SRC / 'tahoe_symbols_25G229.hpp').read_text()
FWX = (ROOT / 'scripts/fw-extract.py').read_text()
LINUX_MMHUB = ROOT / 'build/vcn/linux/mmhub_2_3_0_offset.h'  # local reference copy (gitignored)


def body(src, sig):
    a = src.index(sig)
    b = src.index('\n}\n', a)
    return src[a:b]


# MMHUB 2.3 (Linux mmhub_2_3_0_offset.h, BASE_IDX 1) for AMD_GFX10_VM_REGS_PER_HUB, as fillMm23 must build it.
HEAD = ['MMMC_VM_MX_L1_TLB_CNTL', 'MMVM_L2_CNTL', 'MMVM_L2_CNTL2', 'MMVM_L2_CNTL3', 'MMVM_L2_CNTL4',
        'MMVM_L2_PROTECTION_FAULT_CNTL', 'MMVM_L2_PROTECTION_FAULT_CNTL2', 'MMVM_L2_PROTECTION_FAULT_MM_CNTL3',
        'MMVM_L2_PROTECTION_FAULT_MM_CNTL4', 'MMVM_L2_PROTECTION_FAULT_STATUS', 'MMVM_L2_PROTECTION_FAULT_ADDR_LO32',
        'MMVM_L2_PROTECTION_FAULT_ADDR_HI32']
CTX = ['PAGE_TABLE_BASE_ADDR_LO32', 'PAGE_TABLE_BASE_ADDR_HI32', 'PAGE_TABLE_START_ADDR_LO32',
       'PAGE_TABLE_START_ADDR_HI32', 'PAGE_TABLE_END_ADDR_LO32', 'PAGE_TABLE_END_ADDR_HI32']
ENG = ['ADDR_RANGE_LO32', 'ADDR_RANGE_HI32', 'REQ', 'ACK', 'SEM']


def model_fill():
    """Python transcription of vcn.cpp fillMm23 (offsets relative to segment 1)."""
    head = [int(x, 16) for x in re.search(r'Mm23Head\[12\] \{([^}]*)\}', VCN).group(1).split(',')]
    v = {}
    for i, o in enumerate(head):
        v[('head', i)] = o
    for i in range(16):
        for k in range(6):
            v[('ctx', i, k)] = 0x940 + 8 * i + k
        v[('ctx', i, 6)] = 0x740 + i
    for i in range(18):
        for k, o in enumerate((0xa03, 0xa04, 0xa01, 0xa02, 0xa00)):
            v[('eng', i, k)] = o + 8 * i
    return v


class Vcn(unittest.TestCase):
    def test_pmfw_messages_reviewed_only(self):
        b = body(SMU, 'uint32_t smuVcnMsg(')
        self.assertIn('if(msg!=0x06&&msg!=0x07&&msg!=0x21&&msg!=0x22) return 0;', b)
        self.assertIn('if(!smuGfxClkLive()) return 0;', b)
        self.assertIn('uint32_t arg=0;', b)
        self.assertIn('case 0x06:case 0x07:case 0x21:case 0x22:break;', body(SMU, 'uint32_t sendMsg('))
        self.assertIn('MsgPowerDownVcn = 0x06, MsgPowerUpVcn = 0x07, MsgPowerDownJpeg = 0x21, MsgPowerUpJpeg = 0x22', VCN)
        self.assertIsNone(re.search(r'sendMsg|mmioWrite|0x1[aA]\b|0x11\b', VCN))
        self.assertEqual(VCN.count('smuVcnMsg('), 1)  # only through pmfw()

    def test_mmhub23_map(self):
        v = model_fill()
        self.assertIn('MmSeg0 = 0x13200, MmSeg1 = 0x1A000', VCN)
        self.assertIn('if (v[0] == MmSeg0 + 0x873 && v[4 + 0] == MmSeg0 + 0x698) {', VCN)
        if not LINUX_MMHUB.exists():
            self.skipTest('no local mmhub_2_3_0_offset.h')
        t = LINUX_MMHUB.read_text()
        reg = {m.group(1): int(m.group(2), 16) for m in re.finditer(r'#define\s+(?:mm|reg)(\w+)\s+0x([0-9a-fA-F]+)', t)}
        idx = {m.group(1): int(m.group(2)) for m in re.finditer(r'#define\s+(?:mm|reg)(\w+)_BASE_IDX\s+(\d+)', t)}
        for i, n in enumerate(HEAD):
            self.assertEqual(v[('head', i)], reg[n], n)
            self.assertEqual(idx[n], 1, n)
        for i in range(16):
            for k, f in enumerate(CTX):
                n = f'MMVM_CONTEXT{i}_{f}'
                self.assertEqual(v[('ctx', i, k)], reg[n], n)
                self.assertEqual(idx[n], 1, n)
            self.assertEqual(v[('ctx', i, 6)], reg[f'MMVM_CONTEXT{i}_CNTL'])
        for i in range(18):
            for k, f in enumerate(ENG):
                n = f'MMVM_INVALIDATE_ENG{i}_{f}'
                self.assertEqual(v[('eng', i, k)], reg[n], n)
                self.assertEqual(idx[n], 1, n)

    def test_guard_before_hardware(self):
        d = body(VCN, 'void decide() {')
        # blocked unless the ctl is newer than a pending/blocked state; pending is on disk before the mode is set
        self.assertIn('if (gGuard && mode != Off && haveSt && pendingOrBlocked(st) && !(ctlT > stT)) {', d)
        # the guard is opt-in: without -rmbvcnguard the state file is neither read for a block nor written
        self.assertIn('gGuard = checkKernelArgument("-rmbvcnguard");', d)
        self.assertIn('if (gGuard && mode != Off) {', d)
        self.assertLess(d.index('rmbFileWrite(StatePath, line)) {'), d.index('gBootMode = mode;'))
        self.assertIn('LOG("vcn: cannot write %s, staying off", StatePath);', d)
        # every hardware-changing hook asks bootMode() first
        for sig in ('uint8_t wrapGetFirmware(', 'uint64_t wrapMm210(', 'uint64_t gate(bool vcn,',
                    'uint64_t wrapVcnHwInit(', 'uint64_t wrapVcnStartQueue(', 'uint64_t wrapFillVM('):
            self.assertIn('bootMode() != Off' if 'gate(' not in sig else 'bootMode() == Off', body(VCN, sig), sig)
        # a VCN start waits for "pending" when the state says ok
        n = body(VCN, 'void noteStart() {')
        self.assertIn('__atomic_load_n(&gStateOk, __ATOMIC_ACQUIRE)', n)
        th = body(VCN, 'void vcnThread(')
        self.assertIn('now >= 90000 && (!last || now - last >= 60000)', th)
        self.assertIn('if (gBootMode != Off && gGuard) {', th)
        self.assertNotIn('rmbFile', body(VCN, 'uint8_t bootMode() {'))

    def test_vcn_instance1_never_gates_instance0(self):
        g = body(VCN, 'uint64_t gate(bool vcn,')
        # PSM_ReArbit sends gate(instance 1) right before the first VCN start; it must not reach the PMFW
        self.assertLess(g.index('if (vcn && (inst & 0xFFFFFFFF)) {'), g.index('return setPower('))
        self.assertIn('return 1;', g[g.index('if (vcn && (inst & 0xFFFFFFFF)) {'):g.index('return setPower(')])

    def test_default_on(self):
        self.assertIn('uint8_t mode = haveCtl ? parseMode(ctl) : On;', body(VCN, 'void decide() {'))
        self.assertIn('parseMode(buf) : On;', body(VCN, 'void vcnThread('))
        self.assertIn('return word("on") ? On : word("probe") ? Probe : Off;', VCN)

    def test_firmware_served_only_by_name_and_size(self):
        g = body(VCN, 'uint8_t wrapGetFirmware(')
        self.assertIn('!strcmp(name, FwName)', g)
        self.assertIn('if (buf && cap >= n) {', g)
        self.assertIn('constexpr const char *FwName = "ativvaxy_vcn3_1.dat";', VCN)
        self.assertIn("pieces['vcn']", FWX)
        self.assertIn("b'$PS1'", FWX)

    def test_routes_and_kill_switch(self):
        for name in ('__ZN14AmdTtlServices11getFirmwareEPKcP10_TtlFwInfo', '_mmhub_2_1_0_initialize_registers',
                     '_mmhub_2_3_0_initialize_registers', '_Phw_SwSmu_PowerGateVcn', '_Phw_SwSmu_PowerGateJpeg',
                     '_vcn_hw_init', '_vcn_start_queue',
                     '__ZN29AMDRadeonX6000_AMDMMHub_2_1_115fillVMRegistersEP25AMD_GFX10_VM_REGS_PER_HUB'):
            self.assertIn(f'{{"{name}", ', SYM, name)
            self.assertIn(f'"{name}"', VCN, name)
        for sig in ('void vcnStart() {', 'void vcnRouteHwLibs(', 'void vcnRouteAccel('):
            self.assertIn('checkKernelArgument("-rmbnovcnfix")', body(VCN, sig))
        self.assertIn('vcnRouteHwLibs(patcher, 0, address, size);', KS)
        self.assertIn('vcnStart();', KS)
        self.assertIn('vcnRouteAccel(patcher, 2, address, size);', PRB)

    def test_novcn_follows_vcn(self):
        n = body(DSP, 'void noVideoEngine(')
        self.assertLess(n.index('publishAccelerator(accel);'), n.index('if (vcnShowVideo()) {'))
        self.assertLess(n.index('if (vcnShowVideo()) {'), n.index('stripVideoProps(accel)'))
        self.assertIn('if (!gVideoShown)', n)
        s = body(DSP, 'unsigned stripVideoProps(')
        self.assertLess(s.index('gSavedVideo->setObject(key, value);'), s.index('accel->removeProperty(key);'))
        p = body(DSP, 'void vcnVideoProps(bool show) {')
        self.assertIn('gVideoAccel->setProperty(key, value);', p)
        self.assertIn('-rmbvcn', DSP)


if __name__ == '__main__':
    unittest.main()
