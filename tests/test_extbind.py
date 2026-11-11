#!/usr/bin/env python3
"""Offline contract for display.cpp extbind: FB:1 gets the external DP link (Lenovo 82TL USB-C hub) the way
initializeFixedDisplayBinding (FB 25G229 0x151e6) binds the fixed panel link to FB:0. Source only, no hardware."""
from pathlib import Path
import re, unittest

SRC = Path(__file__).resolve().parents[1] / 'src/Rembrandt660M'
DISPLAY = (SRC / 'display.cpp').read_text()
SYMS = (SRC / 'tahoe_symbols_25G229.hpp').read_text()
SYM = '__ZN35AMDRadeonX6000_AmdRadeonFramebuffer29initializeFixedDisplayBindingEv'


def body(src, sig):
    a = src.index(sig)
    b = src.index('\n}\n', a)
    return src[a:b]


class ExtBind(unittest.TestCase):
    def setUp(self):
        self.bind = body(DISPLAY, 'void extBind(uint64_t fbAddr) {')
        self.guard = body(DISPLAY, 'void extBindGuardThread(void *, wait_result_t) {')
        self.wrap = body(DISPLAY, 'uint64_t wrapInitFixedBinding(')

    def test_manifest_rva(self):
        self.assertIn('{"%s", 1, 0x151e6u},' % SYM, SYMS)

    def test_own_route_with_kill_switch(self):
        i = DISPLAY.index('KernelPatcher::RouteRequest bindReq')
        pre = DISPLAY[i - 200:i]
        self.assertIn('if (!checkKernelArgument("-rmbnoextbind")) {', pre)
        self.assertIn('routeStaticRequests(patcher, 1, &bindReq, 1, address, size)', DISPLAY[i:i + 400])
        a = DISPLAY.index('KernelPatcher::RouteRequest reqs[] {')
        main = DISPLAY[a:DISPLAY.index('\n\t};', a)]
        self.assertNotIn(SYM, main)  # a missing symbol must not fail the DAL register remap group

    def test_wrapper_runs_original_first_fb1_only(self):
        org = self.wrap.index('orgInitFixedBinding')
        ext = self.wrap.index('extBind(fb)')
        self.assertLess(org, ext)
        self.assertIn('[FbIndexOff] == 1', self.wrap)

    def test_offsets_match_fb0_binding(self):
        self.assertIn('FbIndexOff = 480, FbPath1Off = 33232, FbFlagsOff = 35236, FbCtrlOff = 35392', DISPLAY)
        self.assertIn('constexpr size_t FbPathOff = 0x81c8;', DISPLAY)  # 33224
        b = self.bind
        self.assertIn('p[1817] = conn;', b)
        self.assertIn('*reinterpret_cast<uint32_t *>(p + 208) = 1;', b)
        self.assertIn('*reinterpret_cast<uint32_t *>(p + 236) = pick;', b)
        self.assertIn('fb[FbFlagsOff] |= 0x20;', b)
        self.assertIn('link[0x47] |= 1;', b)
        self.assertLess(b.index('vt[440 / 8]'), b.index('vt[392 / 8]'))  # setStreamState(0,0), then isConnected

    def test_link_selection(self):
        self.assertIn('for (uint32_t i = 1; i < count && i < 8; i++)', self.bind)
        self.assertIn("bool dp = sig == 0x20 || (sig == 0 && l[0x1c] == 0x13);", self.bind)
        self.assertIn("bool usable = !(l[0x47] & 1) && (dp || hdmiPort);", self.bind)
        self.assertIn("bool want = usable && (l[0x54] & 1);", self.bind)

    def test_prebind_for_hot_plug(self):
        b = self.bind
        self.assertIn('pre = !checkKernelArgument("-rmbnoextprebind")', b)
        # a connected link wins; the first usable port only when nothing is connected
        self.assertIn('if (want && !link) {', b)
        self.assertIn('if (usable && !port) {', b)
        self.assertLess(b.index('for (uint32_t i = 1;'), b.index('if (!link && pre && port) {'))
        self.assertIn('gBind.pre = true;', b)
        # the guard still runs before any write, pre-bound or not
        self.assertLess(b.index('if (!link && pre && port) {'), b.index('kernel_thread_start(extBindGuardThread'))
        self.assertIn('gBind.linkPtr = link;', b)
        self.assertIn('kernel_thread_start(extBindWatch', b)

    def test_hdmi_only_with_boot_arg(self):
        self.assertIn('const bool hdmi = checkKernelArgument("-rmbextbindhdmi"), pre = ', self.bind)
        self.assertIn("bool hdmiPort = hdmi && (sig == 4 || (sig == 0 && l[0x1c] == 0x0c));", self.bind)

    def test_once_and_only_empty_fb1(self):
        b = self.bind
        self.assertIn('static bool once;', b)
        self.assertIn('checkKernelArgument("-rmbnoextbind")', b)
        self.assertIn('FB:1 already has a display path, nothing done', b)
        self.assertIn('0xffffff8000000000ULL', b)
        self.assertIn('ctrl != gDalCtrl', b)  # fb+35392 must be the controller establishBootDisplay saw

    def test_guard_before_any_write(self):
        b = self.bind
        start = b.index('kernel_thread_start(extBindGuardThread')
        allow = b.index('if (!gBind.allow)')
        first_write = b.index('vt[440 / 8]')
        self.assertLess(start, allow)
        self.assertLess(allow, first_write)
        self.assertIn('i < 300', b)  # waits at most 3 s
        self.assertIn('gBind.abandoned = 1;', b)

    def test_guard_states(self):
        g = self.guard
        self.assertIn('rmbFileRead(ExtBindStatePath', g)
        self.assertIn('armT > stT', g)
        self.assertIn('!strncmp(st, "pending", 7) || !strncmp(st, "blocked", 7)', g)
        self.assertLess(g.index('"blocked: the last extbind'), g.index('"pending: link %u%s%s\\n"'))
        self.assertIn('allow = rmbFileWrite(ExtBindStatePath, line);', g)
        self.assertIn('"/private/var/log/rmb-extbind.state"', DISPLAY)
        v = body(DISPLAY, 'void extBindWatch(void *, wait_result_t) {')
        self.assertIn('"ok: link %u%s, fb1 links %u connected %u\\n"', v)
        self.assertIn('okAt = (gBind.pre ? 30 : 60) * 4', v)
        self.assertIn('IOSleep(250);', v)

    def test_hot_plug_guard(self):
        v = body(DISPLAY, 'void extBindWatch(void *, wait_result_t) {')
        # only after the boot ok, only on a 0 -> 1 link connect; ok again after 30 s or on unplug
        self.assertIn('if (ok && lc && lconn == 0) {', v)
        self.assertIn('"pending: hotplug %u on link %u\\n"', v)
        self.assertIn('if (pendingAt && (!lc || k - pendingAt >= settle)) {', v)
        self.assertIn('settle = 30 * 4', v)
        self.assertIn('logged < 300', v)
        # read-only on FB/link memory
        self.assertNotRegex(v, r'(p|link)\[[^\]]+\] *=[^=]')

    def test_no_symbol_lookups_in_bind(self):
        self.assertIsNone(re.search(r'findSymbol|routeMultiple|solveSymbol', self.bind))


if __name__ == '__main__':
    unittest.main()
