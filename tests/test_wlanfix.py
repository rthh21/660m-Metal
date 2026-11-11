#!/usr/bin/env python3
"""Offline contract for wlanfix.cpp (-rmbnowlanfix): the 25G229 IOPCIFamily bytes the route and the devcap filter rely
on (BootKernelExtensions.kc, external input: evidence/ next to the repository or WLANFIX_BOOT_KC), and the shape of the
module (order of the config writes, kill switches, no registry access in the config read path). CPU only."""
from pathlib import Path
import os, re, struct, unittest

from test_iogdiag import Kc

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / 'src/Rembrandt660M/wlanfix.cpp'
KERN = ROOT / 'src/Rembrandt660M/kern_start.cpp'
BOOT_KC = Path(os.environ.get('WLANFIX_BOOT_KC', ROOT.parent / 'evidence' / 'BootKernelExtensions.kc'))
IOREG = ROOT / 'evidence/macos_out_20261109_051317/ioreg_full.txt'
EV0751 = ROOT / 'evidence/macos_out_20261109_075102'
BOOT_VM_TO_FILE = 0xffffff8000100000
IOPCI_VM = 0xffffff80029e5000
# llvm-nm of the IOPCIFamily image cut from the same Boot KC (build/wifi_20261109/iopci.syms)
SYM = {
    'initializeNub': 0xffffff80029ee13e,
    'probeBusGated': 0xffffff80029eff30,
    'constructCapabilitiesDict': 0xffffff80029efda4,
    'extendedConfigRead32': 0xffffff8002a0067a,
    'extendedConfigWrite32': 0xffffff8002a006d4,
    'supportsFLR': 0xffffff8002a01e62,
    'extendedConfigRead16': 0xffffff80029fe3c2, 'extendedConfigWrite16': 0xffffff80029fe41c,
    'gIOPCIFlags': 0xffffff8002a40c08,
    'configRead32S': 0xffffff8002a0211c, 'configWrite32S': 0xffffff8002a02144,
    'configRead16S': 0xffffff8002a021a4, 'configWrite16S': 0xffffff8002a021cc,
    'configRead8S': 0xffffff8002a02232, 'configWrite8S': 0xffffff8002a0225a,
}
VTABLES = {'IOPCIHostBridge': 0xffffff8002a39b50, 'IOPCIBridge': 0xffffff8002a3a6e0,
           'IOPCI2PCIBridge': 0xffffff8002a3b2f8, 'IOPCIDevice': 0xffffff8002a3cc60}


def src_bytes(name):
    m = re.search(r'const uint8_t ' + name + r'\[(\d+)\] \{([^}]*)\};', SRC.read_text())
    vals = bytes(int(v, 16) for v in re.findall(r'0x([0-9a-f]{2})', m.group(2)))
    assert len(vals) == int(m.group(1)), name
    return vals


def src_const(name):
    return int(re.search(r'constexpr uint32_t ' + name + r' = (0x[0-9a-f]+);', SRC.read_text()).group(1), 16)


def code(s):
    """source without // comments (the header comment names the IOPCIFamily functions involved)"""
    return re.sub(r'//[^\n]*', '', s)


def body(s, start, end):
    return s[s.index(start):s.index(end, s.index(start))]


@unittest.skipUnless(BOOT_KC.exists(), 'Boot KC not in workspace: ' + str(BOOT_KC))
class WlanFixIopci(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.kc = Kc(BOOT_KC)
        cls.off = IOPCI_VM - BOOT_VM_TO_FILE

    def b(self, rva, n):
        return self.kc.read(self.off + rva, n)

    def vt(self, cls, slot):
        """vtable slot (byte offset after the two header words) -> KC vmaddr"""
        return BOOT_VM_TO_FILE + self.kc.ptr(VTABLES[cls] - BOOT_VM_TO_FILE + 0x10 + slot)

    def test_image_identity(self):
        uid, _, ents = self.kc.header(0)
        self.assertEqual(uid, '58904DC1-52BD-302C-04C4-998D4364475C')
        self.assertIn(('com.apple.iokit.IOPCIFamily', IOPCI_VM, self.off), ents)
        uid, segs, _ = self.kc.header(self.off)
        self.assertEqual(uid, '0D91F1AF-7411-3A0F-9A7E-72D59A2DA75F')
        self.assertEqual(segs['__TEXT'], (IOPCI_VM, 0x54000))
        self.assertEqual(src_bytes('IopciUuid').hex().upper(), '0D91F1AF74113A0F9A7E72D59A2DA75F')

    def test_checked_windows_match_kc_and_symbols(self):
        for rva, name, at in (('RvaInitializeNub', 'InitializeNubHead', SYM['initializeNub']),
                              ('RvaProbeBusInitNubCall', 'ProbeBusInitNubCall', SYM['probeBusGated'] + 0x1a8),
                              ('RvaProbeBusDevcap', 'ProbeBusDevcap', SYM['probeBusGated'] + 0x7be),
                              ('RvaSupportsFlr', 'SupportsFlr', SYM['supportsFLR']),
                              ('RvaProbeBusTag10', 'ProbeBusTag10', SYM['probeBusGated'] + 0xa93)):
            want = src_bytes(name)
            self.assertEqual(src_const(rva), at - IOPCI_VM, rva)
            self.assertLessEqual(src_const(rva) + len(want), 0x54000)
            self.assertEqual(self.b(src_const(rva), len(want)), want, name)

    def test_initialize_nub_head_is_long_route_safe(self):
        # 14-byte Lilu long jump: instructions end at +0x0d (mov r14,rdi) and +0x14 (lea rax,[rsi+0xa8]); no
        # rip-relative operand and no branch in the copied head; space (+0xa8) and parent (+0x88) set by the original.
        head = self.b(src_const('RvaInitializeNub'), 0x14)
        self.assertEqual(head.hex(), '554889e54156534889f34989fe' + '488d86a8000000')
        self.assertNotIn(b'\x8d\x05', head)

    def test_initialize_nub_is_vtable_0x888_everywhere_and_called_before_any_read(self):
        for cls in ('IOPCIHostBridge', 'IOPCIBridge', 'IOPCI2PCIBridge'):
            self.assertEqual(self.vt(cls, 0x888), SYM['initializeNub'], cls)
        pbg = SYM['probeBusGated'] - IOPCI_VM
        # +0x1b5: call [rax+0x888]; +0x1e3: call constructCapabilitiesDict (the first read of the nub)
        self.assertEqual(self.b(pbg + 0x1b5, 6).hex(), 'ff9088080000')
        call = self.b(pbg + 0x1e3, 5)
        self.assertEqual(call[0], 0xe8)
        self.assertEqual(pbg + 0x1e8 + int.from_bytes(call[1:], 'little', signed=True),
                         SYM['constructCapabilitiesDict'] - IOPCI_VM)

    def test_devcap_read_goes_through_the_routed_extended_read(self):
        pbg = SYM['probeBusGated'] - IOPCI_VM
        call = self.b(pbg + 0x7c2, 5)
        self.assertEqual(call[0], 0xe8)
        self.assertEqual(pbg + 0x7c7 + int.from_bytes(call[1:], 'little', signed=True),
                         SYM['extendedConfigRead32'] - IOPCI_VM)
        # kern_start.cpp routes exactly that function (static RVA fallback 0x1b67a) and filters its result
        self.assertIn('pciBase + 0x1b67au', KERN.read_text())

    def rel_call(self, rva):
        c = self.b(rva, 5)
        self.assertEqual(c[0], 0xe8)
        return rva + 5 + int.from_bytes(c[1:], 'little', signed=True)

    def test_probe_bus_sets_10bit_tag_requester_without_looking_below(self):
        """probeBusGated +0xa93..+0xad8: devctl2 |= 0x1000 on the nub being probed when the nub can request 10-bit
        tags (its devcap2 bit 17) and its UPSTREAM entry can complete them (devcap2 bit 16); for a root port the
        completer that matters is the function below it, which is not checked."""
        pbg = SYM['probeBusGated'] - IOPCI_VM
        self.assertEqual(self.rel_call(pbg + 0xaaf), SYM['extendedConfigRead16'] - IOPCI_VM)
        self.assertEqual(self.b(pbg + 0xab4, 5).hex(), '0d00100000')            # or eax,0x1000
        self.assertEqual(self.b(pbg + 0xac8, 4).hex(), '4883c628')              # add rsi,0x28 (devctl2)
        self.assertEqual(self.rel_call(pbg + 0xad3), SYM['extendedConfigWrite16'] - IOPCI_VM)
        # the devctl read / write before it uses the same nub ([rbp-0x30], also the devcap store's nub)
        self.assertEqual(self.b(pbg + 0xacf, 4).hex(), '488b7dd0')              # mov rdi,[rbp-0x30]
        self.assertEqual(self.b(pbg + 0x7c7, 4).hex(), '488b4dd0')              # mov rcx,[rbp-0x30] (devcap store)
        # whole block skipped only when gIOPCIFlags bit 27 is set (pci= boot-arg), not used here
        rva = pbg + 0xa36
        ins = self.b(rva, 7)
        self.assertEqual(ins[:2].hex(), 'f605')
        self.assertEqual(ins[6], 0x08)
        self.assertEqual(IOPCI_VM + rva + 7 + int.from_bytes(ins[2:6], 'little', signed=True), SYM['gIOPCIFlags'] + 3)

    def test_raw_config_virtuals(self):
        # IOPCIDevice::configRead/Write*(IOPCIAddressSpace, UInt8[, data]): vtable +0x850..+0x878 (wlanfix.o uses
        # 2128 / 2136 / 2144 / 2152 / 2160 with the MacKernelSDK header)
        for slot, name in ((0x850, 'configRead32S'), (0x858, 'configWrite32S'), (0x860, 'configRead16S'),
                           (0x868, 'configWrite16S'), (0x870, 'configRead8S'), (0x878, 'configWrite8S')):
            self.assertEqual(self.vt('IOPCIDevice', slot), SYM[name], name)

    def test_extended_write_encoding(self):
        # extendedConfigWrite32 +0x67: mov eax,r15d; shl eax,16; and eax,0x0f000000; mov esi,0xf0ffffff;
        # and esi,[r14+0xa8]; or esi,eax; mov rax,[r14]; mov rax,[rax+0x858]  (= Cfg::ext + configWrite32)
        self.assertEqual(self.b(SYM['extendedConfigWrite32'] - IOPCI_VM + 0x67, 0x23).hex(),
                         '4489f8' + 'c1e010' + '250000000f' + 'beffffff f0'.replace(' ', '') + '4123b6a8000000' + '09c6'
                         + '498b06' + '488b8058080000')
        self.assertIn('(sp.bits & 0xf0ffffffU) | ((static_cast<uint32_t>(o) << 16) & 0x0f000000U)', SRC.read_text())


class WlanFixSource(unittest.TestCase):
    def test_kill_switches_and_wiring(self):
        s = SRC.read_text()
        for arg in ('"-rmbnowlanfix"', '"-rmbwlanaspm"', '"-rmbwlannosbr"'):
            self.assertIn(arg, s)
        k = KERN.read_text()
        cb = body(k, 'lilu.onPatcherLoadForce(', '});')
        self.assertLess(cb.index('routePciDevid(patcher, 0, 0, 0)'), cb.index('wlanFixRoute(patcher);'))
        self.assertIn('wlanfix.o', (ROOT / 'src/Rembrandt660M/Makefile').read_text())
        route = body(s, 'void wlanFixRoute(', '\n}\n')
        self.assertLess(route.index('"-rmbnowlanfix"'), route.index('imageVerified()'))
        self.assertLess(route.index('if (!imageVerified())'), route.index('routeFunctionLong('))

    def test_all_routed_config_reads_are_filtered(self):
        k = KERN.read_text()
        self.assertIn('return wlanDevcapFilter16(dev, off, spoof16(orgPciExtRead16, dev, off, "ext16"));', k)
        self.assertIn('return wlanDevcapFilter16(dev, off & 0xff, spoof16(orgPciCfgRead16, dev, off, "cfg16"));', k)
        self.assertIn('return wlanDevcapFilter32(dev, off & 0xff, spoof32(orgPciCfgRead32, dev, off, "cfg32"));', k)
        self.assertIn('return wlanDevcapFilter32(dev, off, spoof32(orgPciExtRead32, dev, off, "ext32"));', k)

    def test_filter_is_pointer_compare_only(self):
        s = SRC.read_text()
        for name in ('uint32_t wlanDevcapFilter32(', 'uint16_t wlanDevcapFilter16('):
            f = body(s, name, '\n}\n')
            for word in ('LOG', 'printf', 'IOLog', 'getProperty', 'getObject', 'configRead', 'IOSleep'):
                self.assertNotIn(word, f, (name, word))
            self.assertIn('dev == gMaskNub', f)
            self.assertIn("v != 0xffff", f)  # a dead read stays all-ones
        self.assertIn('constexpr uint32_t DevcapFlr = 0x10000000;', s)
        fx = body(s, 'void fix(', '\nbool wrapInitializeNub(')
        self.assertLess(fx.index('gMaskOff = wcap + 4u;'), fx.index('gMaskNub = nub;'))

    def test_original_runs_first_and_fix_runs_once(self):
        s = SRC.read_text()
        w = body(s, 'bool wrapInitializeNub(', '\n}\n')
        self.assertLess(w.index('while (!(org = gOrgInitNub))'), w.index('reinterpret_cast<InitNubFn>(org)(bridge, nub, from)'))
        self.assertLess(w.index('reinterpret_cast<InitNubFn>(org)(bridge, nub, from)'), w.index('fix(nub, from, reg)'))
        self.assertIn('gState == 0 && nub && from && dataU32(from, "vendor-id") == 0x14c3 && '
                      'dataU32(from, "device-id") == 0x0616', w)
        self.assertLess(w.index('gState = 1;'), w.index('fix(nub, from, reg)'))
        self.assertLess(w.index('fix(nub, from, reg)'), w.index('gState = 2;'))
        self.assertTrue(w.rstrip().endswith('return r;'))

    def test_write_order_and_fail_closed(self):
        s = SRC.read_text()
        fx = body(s, 'void fix(', '\nbool wrapInitializeNub(')
        # upstream port identified (type 1 header, secondary bus = WLAN bus, PCIe root / downstream port) before
        # any write; otherwise return with nothing written
        ident = fx.index('if (!port || ghdr != 1 || static_cast<uint8_t>(gbus >> 8) != bus || !gcap || (gtype != 4 && gtype != 6)) {')
        first_write = min(fx.index(x) for x in ('aspmOff(', 'l1ssOff(', '.w16(', '.w32(', 'restoreBars(') if x in fx)
        self.assertLess(ident, first_write)
        self.assertIn('nothing written', fx[ident:first_write])
        # secondary bus reset: set bit 6, 2 ms, clear; DLLLA (lnksta bit 13) <= 1 s; 100 ms; id polled
        sbr = body(fx, 'uint16_t bctl = g.r16(0x3e);', '\t\talive = id.v == WlanId;')
        self.assertLess(sbr.index('g.w16(0x3e, bctl | 0x40);'), sbr.index('IOSleep(2);'))
        self.assertLess(sbr.index('IOSleep(2);'), sbr.index('g.w16(0x3e, bctl & ~0x40);'))
        self.assertIn('for (; ms < 1000; ms += 10) {', sbr)
        self.assertIn('if (lsta & 0x2000)', sbr)
        self.assertLess(sbr.index('g.w16(0x3e, bctl & ~0x40);'), sbr.index('IOSleep(100);'))
        self.assertIn('for (int i = 0; i < 4; i++) {', sbr)
        # still dead: return before BARs / devctl / mask
        dead = fx.index('if (!alive) {\n\t\t\tLOG("wlanfix: WLAN still not answering')
        self.assertLess(dead, fx.index('restoreBars(w, from);'))
        self.assertIn('-rmbwlannosbr', fx[:fx.index('uint16_t bctl = g.r16(0x3e);')])
        # ASPM: endpoint before port; L1SS: endpoint before port (Linux pcie_config_aspm_l1ss / _link order)
        self.assertLess(fx.index('aspmOff(w, wcap)'), fx.index('glc = aspmOff(g, gcap)'))
        self.assertLess(fx.index('glc = aspmOff(g, gcap)'), fx.index('l1ssOff(w, wl1ss)'))
        self.assertLess(fx.index('l1ssOff(w, wl1ss)'), fx.index('gl1 = l1ssOff(g, gl1ss)'))
        self.assertIn('if (!keepAspm && wcap) {', fx)
        self.assertIn('d.w16(cap + 0x10, v & ~0x3);', s)
        self.assertIn('d.xw32(l1ss + 8, v & ~0xfU);', s)

    def test_port_10bit_tags_cleared_before_the_first_function_read(self):
        s = SRC.read_text()
        fx = body(s, 'void fix(', '\nbool wrapInitializeNub(')
        ident = fx.index('if (!port || ghdr != 1 ||')
        clear = fx.index('tag10Off(g, gcap)')
        self.assertLess(ident, clear)
        self.assertLess(clear, fx.index('Timed id = timedId(w);'))
        self.assertLess(fx.index('gKeepTag10 = checkKernelArgument("-rmbwlantag10");'), clear)
        self.assertLess(clear, fx.index('uint16_t bctl = g.r16(0x3e);'))     # before any bus reset
        t = body(s, 'uint16_t tag10Off(', '\n}\n')
        self.assertIn('uint16_t v = d.r16(cap + 0x28);', t)
        self.assertIn('if (v != 0xffff && (v & Devctl2Tag10)) {', t)
        self.assertLess(t.index('d.w16(cap + 0x28, v & ~Devctl2Tag10);'), t.index('(void)d.r16(cap + 0x28);'))
        self.assertIn('constexpr uint16_t Devctl2Tag10 = 0x1000;', s)
        self.assertIn('constexpr uint32_t Devcap2Tag10Comp = 0x10000;', s)
        # only the port's devctl2 is written by tag10Off; the function's devctl2 is only read (logged)
        self.assertEqual(code(s).count('tag10Off('), 3)  # definition, fix, report
        self.assertIn('w.r16(wcap + 0x28)', fx)
        self.assertNotIn('w.w16(wcap + 0x28', s)

    def test_report_rechecks_the_port_only_after_the_fix(self):
        s = SRC.read_text()
        r = body(s, 'void wlanFixReport(', '\n}\n')
        self.assertLess(r.index('if (gState != 2 || !gPortNub || !gPortCap)'), r.index('tag10Off(g, gPortCap)'))
        self.assertIn('gKeepTag10 ? g.r16(gPortCap + 0x28) : tag10Off(g, gPortCap)', r)
        fx = body(s, 'void fix(', '\nbool wrapInitializeNub(')
        self.assertLess(fx.index('if (!port || ghdr != 1 ||'), fx.index('nub->retain();'))
        self.assertLess(fx.index('nub->retain();'), fx.index('gPortNub = nub;'))

    def test_no_other_side_effects(self):
        s = code(SRC.read_text())
        for word in ('setProperty', 'removeProperty', 'mmioWrite', 'IOMap', 'registerService', 'extendedConfigWrite',
                     'extendedConfigRead', 'configWrite32(static_cast<UInt8>', 'configRead32(static_cast<UInt8>',
                     'IODelay', 'PE_enter_debugger', 'kernelRequestProbe'):
            self.assertNotIn(word, s)

    def test_bar_restore_shape(self):
        s = SRC.read_text()
        rb = body(s, 'void restoreBars(', '\n}\n')
        self.assertIn('aa->getLength() / 20', rb)
        self.assertIn('uint8_t reg = static_cast<uint8_t>(e[0] & 0xff);', rb)
        self.assertIn('bool is64 = bar && (before & 0x7) == 0x4 && reg <= 0x20;', rb)
        self.assertLess(rb.index('w.w32(reg, e[2]);'), rb.index('w.w32(reg + 4, e[1]);'))
        self.assertNotIn('0x04, ', rb)  # command register untouched (configurator restored cmd 0000)

    @unittest.skipUnless(IOREG.exists(), 'ioreg evidence not in workspace')
    def test_bars_written_for_the_11_09_layout(self):
        """What restoreBars would write for the WLAN nub of the 11-09 05:13 boot."""
        txt = IOREG.read_text(errors='replace')
        blk = txt[txt.index('o WLAN@0 '):]
        hexs = re.search(r'"assigned-addresses" = <([0-9a-f]+)>', blk).group(1)
        raw = bytes.fromhex(hexs)
        ents = [struct.unpack_from('<5I', raw, i) for i in range(0, len(raw) - 19, 20)]
        regs = [(e[0] & 0xff, e[1], e[2], e[4]) for e in ents]
        self.assertEqual(regs, [(0x10, 0, 0xf0000000, 0x100000), (0x18, 0, 0x80800000, 0x8000)])
        self.assertTrue(all(((e[0] >> 16) & 0xff) == 2 for e in ents))  # bus 2 = GPP7 secondary bus


@unittest.skipUnless((EV0751 / 'rmb-msgbuf.log').exists() and (EV0751 / 'ioreg_full.txt').exists(),
                     '11-09 07:51 evidence not in workspace')
class WlanFixEvidence0751(unittest.TestCase):
    """The boot that rules out the function: SBR retrained the link, the function stayed dead, the port had
    10-bit tags on; every function that works below a 10-bit-capable port can complete 10-bit tags."""

    def test_sbr_did_not_help_and_port_had_10bit_tags_on(self):
        log = (EV0751 / 'rmb-msgbuf.log').read_text(errors='replace')
        self.assertRegex(log, r'WLAN 020000 id ffffffff \(2\d{5} us\); port 001300 .*lnkctl\|sta 70120042')
        self.assertIn('secondary bus reset done, link up after 10 ms (lnksta f012)', log)
        self.assertEqual(len(re.findall(r'after reset read \d: id ffffffff \(2\d{5} us\)', log)), 4)
        self.assertIn('GPP7 caps 01@50 pmc c803 pmcsr 0000 10@58 pcie 0142 dev 00012910 lnkcap 03737814 lnk f0120040 '
                      'slt 01400000 dev2 00001006', log)
        # the configurator read the function (BAR sizes) before probeBusGated
        self.assertRegex(log, r'Probing type 0 device class-code 0x028000 cmd 0x0000 at \[i19\]2:0:0\(0x14c3:0x616\)')
        self.assertIn('[0x10 PFM] 0x7f30200000:0x100000', log)

    def test_devcap2_below_10bit_capable_ports(self):
        txt = (EV0751 / 'ioreg_full.txt').read_text(errors='replace')
        caps, dbg = {}, None
        for line in txt.splitlines():
            m = re.search(r'"pcidebug" = "([^"]+)"', line)
            if m:
                dbg = m.group(1)
            m = re.search(r'"IOPCIExpressDeviceCapabilities2" = (\d+)', line)
            if m and dbg:
                caps.setdefault(dbg, int(m.group(1)))
        for port in ('0:2:3(2:2)', '0:2:4(3:3)'):           # GPP7 (WLAN), GPP8 (NVMe)
            self.assertEqual(caps[port], 0x7319ff, port)
            self.assertTrue(caps[port] & 0x20000)             # 10-bit tag requester supported
        self.assertTrue(caps['3:0:0'] & 0x10000)              # NVMe: 10-bit tag completer -> works
        self.assertNotIn('2:0:0', caps)                       # WLAN: capability dict empty (dead at publish)


if __name__ == '__main__':
    unittest.main()
