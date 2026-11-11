#!/usr/bin/env python3
"""Verify pinned kernel/Lilu and external symbol names; not KPI/ABI/load certification."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import uuid

ROOT = Path(__file__).resolve().parents[1]
KERNEL_UUID = '1291D44A-C0CD-3AC7-A9E4-CA2749B33959'
KERNEL_SHA = '7004ebc1020a5c7e3f075c631507f31fdc0ce3fe02e9f01f42dce1d178770a08'
LILU_UUID = 'C2A303B8-6D22-3163-A905-5DB8E340D285'
LILU_SHA = 'ff98508b40a1eb5fb477029db8886412520f8e39965279ea331c99e3d2e3b274'


def metadata(path):
    data = path.read_bytes()
    original_size, original_sha = len(data), hashlib.sha256(data).hexdigest()
    if data[:4] == b'\xca\xfe\xba\xbe':
        count = struct.unpack_from('>I', data, 4)[0]
        if count > 32 or 8+count*20 > len(data):
            raise ValueError('Invalid fat slice table')
        for i in range(count):
            cpu, sub, off, size, align = struct.unpack_from('>5I', data, 8+i*20)
            if cpu == 0x1000007:
                if off+size > len(data):
                    raise ValueError('Truncated fat slice')
                data = data[off:off+size]
                break
    if len(data) < 32 or struct.unpack_from('<II', data) != (0xfeedfacf, 0x1000007):
        raise ValueError(f'Expected x86_64 Mach-O: {path}')
    count, size = struct.unpack_from('<II', data, 16)
    limit, at, uid = 32+size, 32, ''
    if limit > len(data):
        raise ValueError('Truncated load commands')
    for _ in range(count):
        if at+8 > limit:
            raise ValueError('Missing load command')
        cmd, n = struct.unpack_from('<II', data, at)
        if n < 8 or at+n > limit:
            raise ValueError('Invalid load command')
        if cmd == 0x1b:
            if n != 24 or uid:
                raise ValueError('Invalid/duplicate LC_UUID')
            uid = str(uuid.UUID(bytes=data[at+8:at+24])).upper()
        at += n
    if at != limit or not uid:
        raise ValueError('Missing UUID or inconsistent load command size')
    return {'path': path.name, 'bytes': original_size, 'sha256': original_sha,
            'uuid': uid, 'x86_64_slice_bytes': len(data)}


def symbols(path, *flags):
    proc = subprocess.run(['llvm-nm', '--arch=x86_64', '-j', *flags, str(path)],
                          capture_output=True, text=True)
    if proc.returncode:
        raise ValueError(f'llvm-nm exit {proc.returncode}: {proc.stderr}')
    return {line.strip() for line in proc.stdout.splitlines() if line.strip()}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('image', type=Path)
    ap.add_argument('--kernel', type=Path, default=ROOT/'evidence/apple25G229/kernel')
    ap.add_argument('--lilu', type=Path, default=ROOT/'build/runtime-inputs/Lilu')
    ap.add_argument('--output', type=Path, default=ROOT/'audit/import_authority.json')
    args = ap.parse_args()
    result = {'candidate': args.kernel.name, 'required_panic_uuid': KERNEL_UUID,
              'status': 'BLOCKED: verification did not complete',
              'limit': 'Defined external names only. KPI policy, C++ ABI, live KC linkage, hooks, boot and Metal NOT TESTED.'}
    # Clear previous PASS before inspecting inputs: failed checks must not leave stale authority.
    args.output.write_text(json.dumps(result, indent=2)+'\n')
    rc = 2
    try:
        kernel, lilu, plugin = (metadata(p) for p in (args.kernel, args.lilu, args.image))
        result.update(sha256=kernel['sha256'], uuid=kernel['uuid'], images={'kernel': kernel, 'lilu': lilu, 'plugin': plugin})
        if (kernel['uuid'], kernel['sha256']) != (KERNEL_UUID, KERNEL_SHA):
            raise ValueError('Kernel is not the pinned standalone 25G229 image')
        if (lilu['uuid'], lilu['sha256']) != (LILU_UUID, LILU_SHA):
            raise ValueError('Lilu runtime SHA256/UUID mismatch')
        imports = symbols(args.image, '-u')
        providers = {'kernel': symbols(args.kernel, '-g', '--defined-only'),
                     'lilu': symbols(args.lilu, '-g', '--defined-only')}
        rows = [{'symbol': symbol, 'defined_external_in': [name for name, names in providers.items() if symbol in names]}
                for symbol in sorted(imports)]
        missing = [row['symbol'] for row in rows if not row['defined_external_in']]
        result.update(imports=len(imports), matched_names=len(imports)-len(missing), unresolved=missing, rows=rows,
                      lilu_path=args.lilu.name, lilu_sha256=lilu['sha256'])
        result['status'] = 'FAIL: unresolved external names' if missing else 'PASS: external names matched; KPI/ABI/load NOT TESTED'
        (ROOT/'audit/undefined_symbols.txt').write_text(''.join(symbol+'\n' for symbol in sorted(imports)))
        rc = 1 if missing else 0
    except (OSError, ValueError, struct.error) as error:
        result['status'] = 'BLOCKED: '+str(error)
    args.output.write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps({key: value for key, value in result.items() if key != 'rows'}, indent=2))
    return rc


if __name__ == '__main__':
    raise SystemExit(main())
