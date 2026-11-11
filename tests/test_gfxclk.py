#!/usr/bin/env python3
"""Offline contract for gfxclk.cpp (GFX clock policy) and its smu.cpp service. Source only, no hardware."""
from pathlib import Path
import re, unittest

SRC = Path(__file__).resolve().parents[1] / 'src/Rembrandt660M'
GFX = (SRC / 'gfxclk.cpp').read_text()
SMU = (SRC / 'smu.cpp').read_text()
KS = (SRC / 'kern_start.cpp').read_text()
MK = (SRC / 'Makefile').read_text()


def body(src, sig):
    a = src.index(sig)
    b = src.index('\n}\n', a)
    return src[a:b]


class GfxClk(unittest.TestCase):
    def test_mailbox_whitelist(self):
        b = body(SMU, 'uint32_t smuGfxClkMsg(')
        self.assertIn('if(msg!=0x02&&msg!=0x12&&msg!=0x17&&msg!=0x1b&&msg!=0x1c&&msg!=0x13&&msg!=0x1d&&msg!=0x18&&msg!=0x23) return 0;', b)
        # FCLK: only GetFclkFrequency 0x18 and SetHardMinFclkByFreq 0x23 (Linux PEAK/HIGH), never the soft max 0x1E
        self.assertIn('MsgFclkNow = 0x18, MsgFclkHardMin = 0x23', GFX)
        # auto FCLK boost: only the hard min, only while GUI_ACTIVE is busy, released after a hold; fixed fclkmin wins
        self.assertIn('AutoFclkBoost = 1600, FclkUpOf8 = 4, FclkBusyOf8 = 2, FclkHoldMs = 15000', GFX)
        self.assertIn('uint32_t fclkBoost = AutoFclkBoost;', GFX)
        self.assertIn('cfg.mode == Auto && cfg.fclkBoost && !cfg.fclkMin && allow && gFclkFails < 3', GFX)
        self.assertIn('setFclk(cfg.fclkBoost, "auto, GPU busy");', GFX)
        self.assertIn('setFclk(0, "auto, GPU idle");', GFX)
        self.assertIn('uint32_t want = mhz ? mhz : FclkRelease, a = want, r = msg(MsgFclkHardMin, a)', GFX)
        self.assertNotIn('msg(0x1e', GFX)
        self.assertIn('case 0x23:break;', body(SMU, 'uint32_t sendMsg('))
        self.assertNotIn('0x1e', body(SMU, 'uint32_t sendMsg(').lower())
        self.assertNotIn('0x1e', GFX.lower().replace('0x1e9', ''))
        self.assertIn('if(!smuGfxClkLive()) return 0;', b)
        self.assertIn('return sendMsg(msg,arg,1000);', b)
        # no GfxOff (0x19/0x1a), reset (0x11) or table-address messages through this service
        self.assertIsNone(re.search(r'0x1a|0x19|0x11|0x0d|0x0e|0x0f', b))

    def test_table_never_first_allocator(self):
        b = body(SMU, 'uint32_t smuGfxClkTable(')
        self.assertIn('!gContext.table.owned||!gContext.table.usable) return rem660::smu::NotReady;', b)
        self.assertLess(b.index('operationLock,1'), b.index('readOwnedSmuTable'))
        self.assertIn('if(table!=4&&table!=7)', b)

    def test_metrics_table_guarded(self):
        b = body(SMU, 'uint32_t smuGfxClkTableAnyKind(')
        self.assertIn('if(a<vramLo||a>=vramHi||need>vramHi-a||(a&0xfff)) return rem660::smu::Invalid;', b)
        self.assertLess(b.index('a<vramLo'), b.index('readDmaTable('))
        self.assertLess(b.index('readDmaTable('), b.index('clflush'))
        self.assertLess(b.index('mfence'), b.index('src[i]'))
        self.assertIn('!t.owned||!t.handle||!t.lease.cpu||t.lease.inFlight', b)
        self.assertNotIn('allocateTable', b)
        t = body(GFX, 'void gfxClkThread(')
        self.assertIn('if (cfg.metrics && allow && vramRange(lo, hi) &&', t)
        self.assertIn('if (cfg.metrics)\n\t\t\treadDpmTable();', t)

    def test_soc_only_inside_table_range(self):
        a = body(GFX, 'void applySoc(')
        self.assertIn('if (!gSocLo || !gSocHi || gSocLo > gSocHi) {', a)
        self.assertIn('v < gSocLo ? gSocLo : v > gSocHi ? gSocHi : v', a)

    def test_only_known_messages_in_gfxclk(self):
        self.assertIn('MsgVersion = 0x02, MsgFeatures = 0x12, MsgGfxNow = 0x17, MsgSoftMax = 0x1b, MsgHardMin = 0x1c', GFX)
        self.assertIsNone(re.search(r'sendMsg|mmioWrite', GFX))
        self.assertEqual(GFX.count('smuGfxClkMsg('), 1)  # only through msg()

    def test_kill_switch_and_start(self):
        s = body(GFX, 'void gfxClkStart(KernelPatcher &patcher) {')
        self.assertLess(s.index('checkKernelArgument("-rmbnogfxclk")'), s.index('solveSymbol'))
        self.assertLess(s.index('solveSymbol'), s.index('kernel_thread_start'))
        self.assertIn('gfxClkStart(patcher);', KS)
        self.assertIn(' gfxclk.o ', MK)

    def test_auto_default_and_levels(self):
        self.assertIn('Mode mode = Auto;', GFX)
        self.assertIn('AutoCap0 = 1600, AutoCap1 = 1600, AutoCap2 = 1425', GFX)
        self.assertIn('AutoUp[3] {60, 160, 260}, AutoHyst = 20', GFX)
        a = body(GFX, 'uint32_t autoCap(')
        self.assertEqual(a.count('clampMhz('), 4)  # every auto level inside the DPM range
        self.assertIn('clampMhz(c.cap3, gHi)', a)
        l = body(GFX, 'uint32_t autoLevel(')
        self.assertIn('peak >= AutoUp[level]', l)
        self.assertIn('peak + AutoHyst < AutoUp[level - 1]', l)

    def test_auto_cpu_load_source(self):
        c = body(GFX, 'bool cpuTicks(')
        self.assertIn('if (!gHostStatistics || !gHostSelf)\n\t\treturn false;', c)
        self.assertIn('HOST_CPU_LOAD_INFO', c)
        self.assertIn('n < HOST_CPU_LOAD_INFO_COUNT', c)
        s = body(GFX, 'void gfxClkStart(KernelPatcher &patcher) {')
        for sym in ('"_host_statistics"', '"_host_self"', '"_real_ncpus"'):
            self.assertIn(sym, s)
        t = body(GFX, 'void gfxClkThread(')
        self.assertIn('if (cfg.mode == Auto && haveCpu && now - lastCpu >= AutoTickMs) {', t)
        # level changes reach the PMFW only when the crash guard allows it, only through apply()
        self.assertIn('if (allow)\n\t\t\t\t\t\tapply(clampMhz(cfg.floor, gLo), autoCap(cfg, level));', t)
        self.assertIn('auto keeps cap', t)

    def test_clamped_to_table_range(self):
        c = body(GFX, 'uint32_t clampMhz(')
        self.assertIn('return v < gLo ? gLo : v > gHi ? gHi : v;', c)
        i = body(GFX, 'void info() {')
        self.assertIn('d.minGfx < d.maxGfx && d.maxGfx <= 3000', i)
        self.assertIn('FallbackMin = 200, FallbackMax = 1900', GFX)
        # every limit sent goes through clampMhz
        t = body(GFX, 'void gfxClkThread(')
        for m in re.finditer(r'apply\(([^;]*)\);', t):
            self.assertIn('clampMhz', t[max(0, m.start() - 400):m.end()], m.group(0))

    def test_guard_before_first_set(self):
        t = body(GFX, 'void gfxClkThread(')
        self.assertIn('"/private/var/log/rmb-gfxclk.state"', GFX)
        self.assertIn("!strncmp(st, \"pending\", 7) || !strncmp(st, \"blocked\", 7)) && !(haveCtl && ctlT > stT)", t)
        self.assertIn('allow = rmbFileWrite(StatePath, line);', t)
        self.assertLess(t.index('arm(haveCtl ?'), t.index('applyCfg();'))
        self.assertIn('nowMs() - pendingSince >= 60000', t)
        # pendingSince can be newer than the loop's `now` right after a ctl change (no instant ok)
        self.assertNotIn(' now - pendingSince', t)
        self.assertIn('if (cfg.mode == Boost && allow)', t)

    def test_pause_while_asleep(self):
        t = body(GFX, 'void gfxClkThread(')
        self.assertLess(t.index('diskLogSleeping() || !smuGfxClkLive()'), t.index('mmioRead(GrbmStatus)'))
        self.assertIn('gFloor = gCap = 0;', t)

    def test_grbm_status_register(self):
        self.assertIn('GrbmStatus = 0x1260 + 0x0da4', GFX)  # same register haltGfxEngines reads
        self.assertIn('GrbmStatus = GC0 + 0x0da4', SMU)


if __name__ == '__main__':
    unittest.main()
