#!/usr/bin/env python3
"""Offline contract for umdpatch.cpp (-rmbumd): addrlib HwlComputePipeBankXor -> 0 in the AMD UMD code pages,
applied from the 25G229 cs_validate_page hook. CPU only; the Boot KC (BOOT_KC) and the dyld shared cache
(DYLD_CACHE_DIR) are external inputs, the cases that need them are skipped when they are absent."""
from pathlib import Path
import os, re, struct, unittest

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / 'src/Rembrandt660M/umdpatch.cpp'
KERN = ROOT / 'src/Rembrandt660M/kern_start.cpp'
KC = Path(os.environ.get('BOOT_KC', '/System/Library/KernelCollections/BootKernelExtensions.kc'))
DYLD = Path(os.environ.get('DYLD_CACHE_DIR', '/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld'))
KERNEL_UUID = '1291D44A-C0CD-3AC7-A9E4-CA2749B33959'
PAGE = 4096
# 25G229 dyld_shared_cache_x86_64h subcache file offsets of the 5 whole-page copies (2026-11-10).
SITES = {('dyld_shared_cache_x86_64h.03', o) for o in (0x1093edfc, 0x10ac7f6c, 0x10b7ffac, 0x10dd158c, 0x1125aedc)}


def arr(src, name):
    m = re.search(r'const uint8_t ' + name + r'\[\] \{([^}]*)\}', src)
    assert m, name
    return bytes(int(x, 16) for x in re.findall(r'0x[0-9a-f]{2}', m.group(1)))


def source():
    s = SRC.read_text()
    rep_off = int(re.search(r'constexpr size_t xorRepOff = (\d+);', s).group(1))
    return s, arr(s, 'xorPat'), arr(s, 'xorRep'), rep_off, arr(s, 'csValidatePage25G229')


def load_commands(b, off):
    magic, _, _, _, ncmds, _, _, _ = struct.unpack_from('<IiiIIIII', b, off)
    assert magic == 0xfeedfacf
    p = off + 32
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', b, p)
        yield cmd, p
        p += size


def fmt_uuid(raw):
    u = raw.hex().upper()
    return '-'.join((u[:8], u[8:12], u[12:16], u[16:20], u[20:]))


class UmdPatch(unittest.TestCase):
    def test_replacement_is_xor0_over_the_shift(self):
        _, pat, rep, off, _ = source()
        self.assertEqual(len(pat), 24)
        self.assertEqual(len(rep), 8)
        # mov (%rax,%rsi,4),%eax ; add $2,%cl ; shl %cl,%eax  ->  xor %eax,%eax ; nopl 0(%rax) ; xchg %ax,%ax
        self.assertEqual(pat[off:off + 8], bytes.fromhex('8b04b080c102d3e0'))
        self.assertEqual(rep, bytes.fromhex('31c00f1f40006690'))
        # the following mov %eax,4(%rdx) stores pipeBankXor = 0
        self.assertEqual(pat[off + 8:], bytes.fromhex('894204'))

    def test_only_cs_validate_page_routed_behind_gate(self):
        s = SRC.read_text()
        route = s[s.index('void umdRoute(KernelPatcher &patcher) {'):]
        self.assertIn('checkKernelArgument("-rmbumd")', route)
        self.assertIn('{"_cs_validate_page", wrapCsValidatePage, orgCsValidatePage}', route)
        self.assertNotIn('"_cs_validate_range"', route)
        # the byte check comes before the route
        self.assertLess(route.index('memcmp(code, csValidatePage25G229'), route.index('routeMultipleLong'))
        self.assertNotIn('"split"', s)

    def test_wired_once_in_patcher_load(self):
        k = KERN.read_text()
        body = k[k.index('lilu.onPatcherLoadForce('):]
        body = body[:body.index('\n\t});')]
        self.assertEqual(body.count('umdRoute(patcher);'), 1)
        self.assertEqual(k.count('umdRoute(patcher);'), 1)

    @unittest.skipUnless(KC.is_file(), 'BOOT_KC not available')
    def test_cs_validate_page_bytes_are_25G229_boot_kc(self):
        _, _, _, _, body = source()
        self.assertEqual(len(body), 0x134)
        b = KC.read_bytes()
        segs, kernel_uuid, symtab = [], None, None
        for cmd, p in load_commands(b, 0):
            if cmd == 0x19:
                segs.append(struct.unpack_from('<QQQQ', b, p + 24))
            elif cmd == 0x80000035:
                vm, fo, nameoff = struct.unpack_from('<QQI', b, p + 8)
                if b[p + nameoff:b.index(b'\0', p + nameoff)] == b'com.apple.kernel':
                    for c, q in load_commands(b, fo):
                        if c == 0x1b:
                            kernel_uuid = fmt_uuid(b[q + 8:q + 24])
                        elif c == 0x2:  # the kernel entry's own LC_SYMTAB (into the KC __LINKEDIT)
                            symtab = struct.unpack_from('<IIII', b, q + 8)
        self.assertEqual(kernel_uuid, KERNEL_UUID)
        symoff, nsyms, stroff, _ = symtab
        addr = None
        for i in range(nsyms):
            strx, ntype, _, _, value = struct.unpack_from('<IBBHQ', b, symoff + 16 * i)
            if (ntype & 0x0e) == 0x0e and b[stroff + strx:b.index(b'\0', stroff + strx)] == b'_cs_validate_page':
                addr = value
                break
        self.assertIsNotNone(addr)
        fo = next(f + addr - v for v, vs, f, fs in segs if v <= addr < v + fs)
        self.assertEqual(b[fo:fo + 0x134], body)

    @unittest.skipUnless((DYLD / 'dyld_shared_cache_x86_64h').is_file(), 'DYLD_CACHE_DIR not available')
    def test_page_wise_patch_hits_exactly_the_5_sites(self):
        _, pat, rep, off, _ = source()
        found, straddling = set(), []
        for f in sorted(DYLD.glob('dyld_shared_cache_x86_64h*')):
            if f.suffix in ('.map', '.atlas') or f.name.endswith('.symbols'):
                continue
            with f.open('rb') as h:
                base, tail = 0, b''
                while True:
                    chunk = h.read(1 << 24)
                    if not chunk:
                        break
                    buf = tail + chunk
                    start = base - len(tail)
                    i = buf.find(pat)
                    while i != -1:
                        o = start + i
                        if (o % PAGE) + len(pat) <= PAGE:
                            found.add((f.name, o))
                        else:
                            straddling.append((f.name, o))
                        i = buf.find(pat, i + 1)
                    tail = buf[-(len(pat) - 1):]
                    base += len(chunk)
        self.assertEqual(straddling, [])
        self.assertEqual(found, SITES)
        for name, o in found:
            with (DYLD / name).open('rb') as h:
                h.seek(o - o % PAGE)
                page = bytearray(h.read(PAGE))
            p = o % PAGE
            page[p + off:p + off + len(rep)] = rep
            self.assertEqual(bytes(page).find(pat), -1)  # one rewrite per page, nothing left to match


if __name__ == '__main__':
    unittest.main()
