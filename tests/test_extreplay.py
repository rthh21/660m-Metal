#!/usr/bin/env python3
"""Offline contract for display.cpp extreplay (external DP sink connected at boot, Lenovo 82TL USB-C hub) and the
Win Max 2 topology fixes (DP test by signal, retimer emulation off by default). Source only, no hardware."""
from pathlib import Path
import re, unittest

SRC = Path(__file__).resolve().parents[1] / 'src/Rembrandt660M'
DISPLAY = (SRC / 'display.cpp').read_text()
DMUB = (SRC / 'dmub.cpp').read_text()
DISKLOG = (SRC / 'disklog.cpp').read_text()


def body(src, sig):
    a = src.index(sig)
    b = src.index('\n}\n', a)
    return src[a:b]


class ExtReplay(unittest.TestCase):
    def setUp(self):
        self.ext = body(DISPLAY, 'void extReplay(void *, wait_result_t) {')
        self.boot = body(DISPLAY, 'uint64_t wrap_establishBootDisplay(')

    def test_started_once_with_kill_switch(self):
        self.assertIn('static bool extStarted;', self.boot)
        self.assertIn('checkKernelArgument("-rmbnoextreplay")', self.boot)
        self.assertIn('kernel_thread_start(extReplay, nullptr, &t)', self.boot)
        self.assertIn('gDalCtrl = ctrl;', self.boot)

    def test_waits_for_windowserver_and_panel(self):
        wait = self.ext.index('!(gWsAttached && gFbEnabledSeen)')
        settle = self.ext.index('IOSleep(10000);')
        detect = self.ext.index('org_detectDisplayOnLink')
        self.assertLess(wait, settle)
        self.assertLess(settle, detect)

    def test_never_touches_link0_or_fixed_or_non_dp(self):
        self.assertIn('for (uint32_t i = 1; i < count && i < 8; i++)', self.ext)
        self.assertIn('for (uint32_t i = 1; i < 8; i++)', self.ext)
        self.assertIn("bool want = !(l[0x47] & 1) && l[0x494] && l[0x54] && sig == 0x20;", self.ext)
        self.assertNotIn('org_detectDisplays', self.ext)  # detectDisplays re-detects every link

    def test_crash_guard_order(self):
        inuse = self.ext.index('extFbInUse(why, sizeof(why))')
        guard = self.ext.index('!strncmp(st, "pending", 7)')
        pending = self.ext.index('"pending: links %x%s\\n"')
        write = self.ext.index('rmbFileWrite(ExtStatePath, line)')
        detect = self.ext.index('org_detectDisplayOnLink')
        ok = self.ext.index('"ok: links %x, framebuffer %s\\n"')
        self.assertLess(inuse, guard)
        self.assertLess(guard, pending)
        self.assertLess(pending, write)
        self.assertLess(write, detect)
        self.assertLess(detect, ok)
        self.assertIn('cannot write %s (no crash guard), nothing done', self.ext)
        self.assertIn('armT > stT', self.ext)
        self.assertIn('"/private/var/log/rmb-extdp.state"', DISPLAY)
        self.assertIn('"/Users/Shared/rmb-extdp-arm"', DISPLAY)

    def test_single_attempt(self):
        self.assertEqual(self.ext.count('org_detectDisplayOnLink'), 1)
        self.assertIsNone(re.search(r'for \(\w+ attempt', self.ext))

    def test_state_file_is_fully_synced(self):
        w = body(DISKLOG, 'bool rmbFileWrite(const char *path, const char *s) {')
        self.assertIn('VNOP_FSYNC(vp, MNT_WAIT, ctx);', w)
        self.assertIn('VNOP_IOCTL(vp, F_FULLFSYNC, nullptr, 0, ctx);', w)
        self.assertIn('VATTR_SET(&va, va_data_size, 0);', w)

    def test_fb_in_use_is_fail_safe(self):
        f = body(DISPLAY, 'bool extFbInUse(char *why, size_t n) {')
        self.assertIn('0xffffff8000000000ULL', f)
        self.assertIn('no AMD framebuffer listed', f)
        self.assertIn('!strcmp(loc, "0")', f)


class Topology(unittest.TestCase):
    def test_no_link_index_dp_tests_left(self):
        code = lambda src: re.sub(r'//[^\n]*', '', src)
        self.assertNotIn('gExtLink >= 2', code(DISPLAY))
        self.assertNotIn('extLink() >= 2', code(DMUB))
        self.assertIn('if (!gUsbcPllPd && extLinkIsDp()) {', DISPLAY)
        self.assertIn('extLink() >= 1 && extLinkDp()', DMUB)

    def test_dp_test_by_signal(self):
        f = body(DISPLAY, 'bool extLinkIsDp() {')
        self.assertIn('return s != 1 && s != 2 && s != 4;', f)
        g = body(DISPLAY, 'uint32_t linkSignal(uint32_t i) {')
        self.assertIn('gDalCtrl + 24376 + i * 0x440ULL + 8', g)
        self.assertIn('gDalCtrl + 288', g)

    def test_retimer_off_by_default(self):
        self.assertIn('bool gNoRetimer = true;', DISPLAY)
        self.assertIn('gNoRetimer = !checkKernelArgument("-rmbretimer") || checkKernelArgument("-rmbnoretimer");',
                      DISPLAY)

    def test_no_new_routes(self):
        self.assertIsNone(re.search(r'findSymbol|routeMultiple|solveSymbol', body(DISPLAY, 'void extReplay(')))


if __name__ == '__main__':
    unittest.main()
