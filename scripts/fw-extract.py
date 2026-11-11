#!/usr/bin/env python3
"""Split Rembrandt (yellow_carp) amdgpu firmware into the pieces HWLibs' GC firmware slots hold.

  fw-extract.py FW-DIR OUT-DIR

FW-DIR holds decompressed /usr/lib/firmware/amdgpu/yellow_carp_*.bin. Header layouts follow
Linux amdgpu_ucode.h (MIT); payload selection follows amdgpu_ucode_init_single_fw for PSP
(front-door) loading. Writes OUT-DIR/<slot>.bin and prints a table.
"""
import os, struct, sys, hashlib, json
from pathlib import Path

class CheckedBytes(bytes):
    def __getitem__(self, key):
        if isinstance(key, slice):
            start = 0 if key.start is None else key.start
            end = len(self) if key.stop is None else key.stop
            if start < 0 or end < start or end > len(self):
                raise ValueError("firmware slice outside image")
        return super().__getitem__(key)

def read_image(path):
    b = CheckedBytes(Path(path).read_bytes())
    pinned = json.loads((Path(__file__).resolve().parents[1]/'audit/firmware_local_sha256.json').read_text())
    if hashlib.sha256(b).hexdigest() != pinned.get(Path(path).name):
        raise ValueError("unreviewed firmware hash: " + str(path))
    h = common(b)
    if h['size_bytes'] != len(b) or not 32 <= h['header_size_bytes'] <= h['ucode_array_offset_bytes'] <= len(b):
        raise ValueError("invalid firmware header bounds")
    if h['ucode_size_bytes'] > len(b)-h['ucode_array_offset_bytes']:
        raise ValueError("invalid payload size")
    return b


def common(b):
    k = ('size_bytes header_size_bytes hdr_major hdr_minor ip_major ip_minor ucode_version '
         'ucode_size_bytes ucode_array_offset_bytes crc32').split()
    return dict(zip(k, struct.unpack_from('<IIHHHHIIII', b, 0)))

def u32s(b, off, n):
    return struct.unpack_from(f'<{n}I', b, off)

def main():
    src, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    pieces = {}

    def gfx(name, file, with_jt=False):
        b = read_image(os.path.join(src, file))
        h = common(b)
        feat, jt_off, jt_size = u32s(b, 32, 3)
        start, size = h['ucode_array_offset_bytes'], h['ucode_size_bytes']
        if jt_size * 4 > size or (jt_off + jt_size) * 4 > size:
            raise ValueError('invalid jump table')
        if with_jt:
            pieces[name] = b[start:start + size - jt_size * 4]
            pieces[name + '_jt'] = b[start + jt_off * 4:start + jt_off * 4 + jt_size * 4]
        else:
            pieces[name] = b[start:start + size]
        return h

    for n in ('me', 'pfp', 'ce'):
        gfx(n, f'yellow_carp_{n}.bin')
    gfx('mec', 'yellow_carp_mec.bin', with_jt=True)
    gfx('mec2', 'yellow_carp_mec2.bin', with_jt=True)

    b = read_image(os.path.join(src, 'yellow_carp_rlc.bin'))
    h = common(b)
    print(f"rlc header v{h['hdr_major']}.{h['hdr_minor']}")
    start, size = h['ucode_array_offset_bytes'], h['ucode_size_bytes']
    pieces['rlc'] = b[start:start + size]
    v20 = 32 + 4 * 18                                      # end of rlc_firmware_header_v2_0
    if (h['hdr_major'], h['hdr_minor']) >= (2, 1):
        f = u32s(b, v20, 13)                               # v2_1 fields
        for i, n in enumerate(('srlist_cntl', 'srlist_gpm', 'srlist_srm')):
            size, off = f[3 + 4 * i], f[4 + 4 * i]
            pieces[n] = b[off:off + size]
    if (h['hdr_major'], h['hdr_minor']) >= (2, 2):
        f = u32s(b, v20 + 13 * 4, 4)
        pieces['rlc_iram'] = b[f[1]:f[1] + f[0]]
        pieces['rlc_dram'] = b[f[3]:f[3] + f[2]]

    b = read_image(os.path.join(src, 'yellow_carp_sdma.bin'))
    h = common(b)
    pieces['sdma'] = b[h['ucode_array_offset_bytes']:h['ucode_array_offset_bytes'] + h['ucode_size_bytes']]

    # PSP TOC (psp_firmware_header_v1_0): payload as in Linux psp_init_toc_microcode.
    b = read_image(os.path.join(src, 'yellow_carp_toc.bin'))
    h = common(b)
    pieces['toc'] = b[h['ucode_array_offset_bytes']:h['ucode_array_offset_bytes'] + h['ucode_size_bytes']]

    # DMCUB (dmcub_firmware_header_v1_0: inst_const_bytes, bss_data_bytes after the common header):
    # the PSP-signed inst_const image, as Linux hands it to the PSP for AMDGPU_UCODE_ID_DMCUB.
    b = read_image(os.path.join(src, 'yellow_carp_dmcub.bin'))
    h = common(b)
    inst_const, bss = u32s(b, 32, 2)
    pieces['dmcub'] = b[h['ucode_array_offset_bytes']:h['ucode_array_offset_bytes'] + inst_const]
    print(f"dmcub v{h['ucode_version']:#x} inst_const {inst_const:#x} bss {bss:#x}")

    # VCN (common header v1.0): the PSP-signed image Linux hands the PSP for AMDGPU_UCODE_ID_VCN
    # (amdgpu_ucode_init_single_fw default case). Same "$PS1" / "AMD@" (+0xa0) layout as Apple's
    # ativvaxy_vcn3.dat; vcn.cpp serves it to HWLibs as ativvaxy_vcn3_1.dat.
    b = read_image(os.path.join(src, 'yellow_carp_vcn.bin'))
    h = common(b)
    pieces['vcn'] = b[h['ucode_array_offset_bytes']:h['ucode_array_offset_bytes'] + h['ucode_size_bytes']]
    if pieces['vcn'][0x10:0x14] != b'$PS1' or pieces['vcn'][0xa0:0xa4] != b'AMD@':
        raise ValueError('VCN payload is not a PSP-signed image')
    print(f"vcn v{h['ucode_version']:#x} ip {h['ip_major']}.{h['ip_minor']}")

    if any(not data for data in pieces.values()):
        raise ValueError('empty required firmware piece')
    for n, data in pieces.items():
        open(os.path.join(out, n + '.bin'), 'wb').write(data)
        print(f'{n:12s} {len(data):#8x}')


if __name__ == '__main__':
    main()
