#!/usr/bin/env python3
"""Print the vbios_augment.hpp constants for another laptop's Radeon 660M VBIOS.

  port_vbios.py VFCT.bin --vram-mb 2048 --bus-width 128 [--out my-73ff-aug.rom]

VFCT.bin is the laptop's ACPI VFCT table (Linux: sudo cat /sys/firmware/acpi/tables/VFCT > VFCT.bin).
A bare ROM image (starting with 55 AA) is accepted too.

The kext only augments a VBIOS whose length and FNV-1a64 hash it knows (vbios_augment.hpp), and it checks
the result against a second hash. This tool runs the same augmentation as the kext (and as
make_lenovo_vbios.py, without its Lenovo-only input check) and prints the values to put in
vbios_augment.hpp. On the Lenovo 82TL it prints the constants already in the source.
"""
import argparse
import hashlib
import struct
import sys

from make_lenovo_vbios import PSP_SLOT, VRAM_SLOT, psp_directory, vram_info


def fnv1a64(data):
    h = 0xcbf29ce484222325
    for b in data:
        h = ((h ^ b) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


def from_vfct(t):
    """Same walk as rmbvbios::findVfctImage. Returns (rom, subsystem vendor, subsystem id, revision)."""
    tlen = struct.unpack_from('<I', t, 4)[0]
    off = struct.unpack_from('<I', t, 52)[0]
    found = []
    while off + 28 <= min(tlen, len(t)):
        ven, dev, ssv, ssid, rev, il = struct.unpack_from('<HHHHII', t, off + 12)
        if not il or off + 28 + il > tlen:
            break
        found.append(f'{ven:04x}:{dev:04x}')
        if (ven, dev) == (0x1002, 0x1681):
            return bytes(t[off + 28:off + 28 + il]), ssv, ssid, rev
        off += 28 + il
    sys.exit(f'no 1002:1681 image in the VFCT table (images: {", ".join(found) or "none"})')


def augment(raw, vram_mb, bus_width):
    """Same steps as rmbvbios::augment. Returns the augmented image."""
    rom = bytearray(raw)
    if rom[:2] != b'\x55\xaa':
        sys.exit('image does not start with 55 AA')
    pcir = struct.unpack_from('<H', rom, 0x18)[0]
    ven, dev = struct.unpack_from('<HH', rom, pcir + 4)
    if rom[pcir:pcir + 4] != b'PCIR' or (ven, dev) != (0x1002, 0x1681):
        sys.exit(f'PCIR is {ven:04x}:{dev:04x}, expected 1002:1681')
    struct.pack_into('<H', rom, pcir + 6, 0x73FF)
    romhdr = struct.unpack_from('<H', rom, 0x48)[0]
    if rom[romhdr + 4:romhdr + 8] != b'ATOM':
        sys.exit('no ATOM header')
    mdt = struct.unpack_from('<H', rom, romhdr + 0x20)[0]
    for slot, blob in ((PSP_SLOT, psp_directory()), (VRAM_SLOT, vram_info(vram_mb, bus_width))):
        if struct.unpack_from('<H', rom, mdt + slot)[0]:
            sys.exit(f'master data table slot {slot:#x} is already used: the kext\'s augmentation does not fit this VBIOS')
        off = (len(rom) + 0xF) & ~0xF
        rom += bytes(off - len(rom)) + blob
        if off + len(blob) > 0xFFFF:
            sys.exit('augmented image is over 64 KiB (ATOM offsets are 16-bit)')
        struct.pack_into('<H', rom, mdt + slot, off)
    rom += bytes(-len(rom) % 512)
    rom[2] = len(rom) // 512
    struct.pack_into('<H', rom, pcir + 0x10, len(rom) // 512)
    rom[0x21] = 0
    rom[0x21] = -sum(rom) & 0xFF
    assert sum(rom) & 0xFF == 0 and rom[2] * 512 == len(rom)
    return bytes(rom)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('vfct', help='ACPI VFCT table (or a bare 55 AA ROM image)')
    ap.add_argument('--vram-mb', type=int, required=True, help='Linux dmesg "amdgpu: VRAM: <N>M"')
    ap.add_argument('--bus-width', type=int, required=True, help='Linux dmesg "RAM width <N>bits"')
    ap.add_argument('--out', help='also write the augmented image here (for inspection only)')
    a = ap.parse_args()

    data = open(a.vfct, 'rb').read()
    if data[:4] == b'VFCT':
        raw, ssv, ssid, rev = from_vfct(data)
        # Many BIOSes leave these VFCT fields at 0 (the 82TL does), so lspci stays the source of truth.
        print(f'VFCT image 1002:1681 (VFCT header subsystem {ssv:04x}:{ssid:04x}, revision field {rev:#x})')
    else:
        raw = data
        print('bare ROM image')
    print('PCI subsystem and revision for mmio.cpp / kern_start.cpp: read them with lspci -nnv -d 1002:1681')
    out = augment(raw, a.vram_mb, a.bus_width)
    print(f'raw {len(raw):#x} bytes sha256 {hashlib.sha256(raw).hexdigest()}')
    print(f'augmented {len(out):#x} bytes sha256 {hashlib.sha256(out).hexdigest()}')
    print('\nsrc/Rembrandt660M/vbios_augment.hpp:\n')
    print(f'constexpr size_t kRawLen = {len(raw):#X};'.replace('0X', '0x'))
    print(f'constexpr size_t kOutLen = {len(out):#X};'.replace('0X', '0x'))
    print(f'constexpr uint64_t kRawFnv = {fnv1a64(raw):#018x}ULL;')
    print(f'constexpr uint64_t kOutFnv = {fnv1a64(out):#018x}ULL;')
    print(f'constexpr uint32_t kVramMb = {a.vram_mb}, kBusWidth = {a.bus_width};')
    if a.out:
        open(a.out, 'wb').write(out)
        print(f'\nwrote {a.out}')


if __name__ == '__main__':
    main()
