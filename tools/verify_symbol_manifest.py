#!/usr/bin/env python3
"""Offline audit of src/Rembrandt660M/tahoe_symbols_25G229.hpp against the 25G229 KDK binaries.

Every static RVA the kext routes or calls must be:
  1. in a binary whose LC_UUID equals apple_evidence/TAHOE_KDK_VERIFIED_MANIFEST.tsv,
  2. the llvm-nm address of the exact name, or (alias) the address of another defined symbol with the
     same method part (e.g. derived-class name resolving to the inherited base implementation), with
     the exact name absent from the binary,
  3. safe for Lilu's 5-byte short-jump trampoline: the instructions copied from the first >= 5 bytes
     must not be RIP-relative or relative branches (Lilu copies them unmodified) and must not return,
     unless the symbol has call-site reference plans in tahoe_reference_sites.hpp (restoredRouteGroup
     then patches the rel32 call/lea sites instead of the prologue).

Exit 0 only if every entry of groups 0-3 passes. Group 5 (IOPCIFamily) is reported as not verifiable
offline: its binary is not in the workspace (routing.cpp resolves it from the live symtab first).
"""
import re
import subprocess
import sys

GROUPS = {
    0: ('AMDRadeonX6000HWServices.kext/Contents/PlugIns/AMDRadeonX6000HWLibs.kext/Contents/MacOS/AMDRadeonX6000HWLibs',
        '5580395F-0AE4-3F9D-8263-974BE551E647'),
    1: ('AMDRadeonX6000Framebuffer.kext/Contents/MacOS/AMDRadeonX6000Framebuffer',
        'C3FA8B38-0702-3FCD-9878-C8FD05A7A724'),
    2: ('AMDRadeonX6000.kext/Contents/MacOS/AMDRadeonX6000', 'DCD61202-702C-339A-840E-6E590CA6D74C'),
    3: ('AMDRadeonX6000HWServices.kext/Contents/MacOS/AMDRadeonX6000HWServices',
        'D8C84822-10E1-3EE0-A53A-DEEF952F3A03'),
}
REL = re.compile(r'^(j[a-z]+|call[a-z]*|loop[a-z]*)\s+0x[0-9a-f]+')


def run(*args):
    return subprocess.run(args, capture_output=True, text=True, check=True).stdout


def body(name):
    # mangled name without the class length prefix: tolerates a wrong length digit in the manifest name
    return re.sub(r'^_?_ZN\d+', '', name)


def method_part(name):
    # _ZN<len><class><len><method>E...  -> everything after the first (class) component
    m = re.match(r'_?_ZN(\d+)', name)
    if not m:
        return name
    i = m.end() + int(m.group(1))
    return name[i:]


def prologue(binary, rva):
    out = run('llvm-objdump', '-d', '--no-show-raw-insn', f'--start-address={rva:#x}',
              f'--stop-address={rva + 24:#x}', binary)
    ins = []
    for l in out.splitlines():
        m = re.match(r'\s+([0-9a-f]+):\s*\t(.*)', l)
        if m:
            ins.append((int(m.group(1), 16), m.group(2).strip()))
    covered = []
    for i, (a, t) in enumerate(ins):
        covered.append(t)
        nxt = ins[i + 1][0] if i + 1 < len(ins) else rva + 24
        if nxt - rva >= 5:
            break
    return covered


def main():
    if len(sys.argv) != 3:
        print('usage: verify_symbol_manifest.py <tahoe_symbols_25G229.hpp> <KDK Extensions dir>')
        return 2
    src = open(sys.argv[1]).read()
    refsrc = open(sys.argv[1].replace('tahoe_symbols_25G229.hpp', 'tahoe_reference_sites.hpp')).read()
    refrouted = set(re.findall(r'\{"([^"]+)",0x[0-9a-fA-F]+,', refsrc))
    ents = re.findall(r'\{"([^"]+)",\s*(\d+),\s*(0x[0-9a-fA-F]+)u?\}', src)
    bad = 0
    for g, (rel, uuid) in GROUPS.items():
        binary = f'{sys.argv[2]}/{rel}'
        got = re.search(r'uuid ([0-9A-F-]+)', run('llvm-objdump', '--macho', '--private-headers', binary))
        if not got or got.group(1) != uuid:
            print(f'FAIL group {g}: {binary} LC_UUID {got and got.group(1)} != {uuid}')
            bad += 1
            continue
        by_name, by_addr = {}, {}
        for l in run('llvm-nm', '--defined-only', binary).splitlines():
            p = l.split()
            if len(p) == 3:
                a = int(p[0], 16)
                by_name.setdefault(p[2], []).append(a)
                by_addr.setdefault(a, []).append(p[2])
        n = alias = 0
        for name, grp, rva in ents:
            if int(grp) != g:
                continue
            n += 1
            rva = int(rva, 16)
            have = by_name.get(name, [])
            kind = 'exact'
            if have != [rva]:
                at = by_addr.get(rva, [])
                same = [s for s in at if method_part(s) == method_part(name) or body(s) == body(name)]
                if have or not same:
                    print(f'FAIL group {g}: {name} rva {rva:#x}: nm {[hex(x) for x in have]}, symbols at rva {at or "none"}')
                    bad += 1
                    continue
                kind = f'alias of {same[0]}'
                alias += 1
            pro = prologue(binary, rva)
            hazard = [t for t in pro if '(%rip)' in t or REL.match(t) or t.startswith('ret')]
            if hazard and name in refrouted:
                print(f'note group {g}: {name} rva {rva:#x}: prologue {pro} not short-jump safe; reference-routed')
            elif hazard:
                print(f'FAIL group {g}: {name} rva {rva:#x} ({kind}): first 5 bytes not short-jump safe: {pro}')
                bad += 1
        print(f'group {g} {uuid}: {n} entries checked, {alias} aliases')
    g5 = [e for e in ents if e[1] == '5']
    print(f'group 5 (IOPCIFamily): {len(g5)} entries NOT VERIFIED offline (binary not in workspace)')
    print('RESULT:', 'FAIL' if bad else 'PASS', f'({bad} failures)')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
