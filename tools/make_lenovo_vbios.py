#!/usr/bin/env python3
"""Build the ATY,bin_image for the Lenovo Radeon 660M (1002:1681 rev 03, 17AA:3B1E).

  make_lenovo_vbios.py IN.rom OUT.rom --vram-mb 2048 --bus-width 128

Adapted (GPL-2.0) from winmax2-2022-6800u-hackintosh scripts/vfct-rom.py + scripts/augment-rom.py.

Why (25G229 AMDRadeonX6000Framebuffer, KDK sha256 9f157151...):
  AmdRadeonControllerNavi23::createControllerCoreServices (@0x3584c) -> createBiosParser (@0x3591a);
  AmdAtomFwServices::initializeAtomDataTable (@0x55250) returns 0xE00002C9 when m_pspDirectory
  (@0x552d1) or m_vramInfo (@0x552f2) is NULL. The raw Rembrandt VFCT image has neither master
  data table slot (+0x16 PSP directory, +0x3C vram_info), so Core Services fail, setupHwServices
  (@0x4bb83) never registers the ICM callbacks and HWLibs AmdTtlServices::initialize returns
  0xE00002BC to the accelerator.

What is changed (nothing else):
  - PCIR device id 0x1681 -> 0x73FF (same identity as the OC device-id property / config spoof).
  - PSP directory v2.1 with 0 entries, padded to the 0x414 bytes the driver maps.
  - vram_info v2.6 with ONE atom_vram_module_v9: size/width from the Lenovo Linux log
    (linux_amdgpu_dmesg.txt: "VRAM: 2048M", "RAM width 128bits LPDDR5"). memory_type is the
    GDDR6 code 0x70 because the driver only maps 0x50/0x60/0x70 (LPDDR5 has no code there):
    a compatibility marker, NOT a hardware claim.
  - image length (byte 2 + PCIR) and ATOM checksum byte 0x21.
"""
import argparse
import hashlib
import struct
import sys

PSP_SLOT, VRAM_SLOT = 0x16, 0x3C
GDDR6_MARKER = 0x70
EXPECTED_IN_SHA256 = "9df4040315e38e30"  # prefix of the Lenovo VFCT image already in config.plist


def table_header(size, frev, crev):
    return struct.pack('<HBB', size, frev, crev)


def psp_directory():
    body = struct.pack('<4sIII', b'$PSP', 0, 0, 0)  # cookie, checksum, entry count, reserved
    t = table_header(0x414, 2, 1) + body
    return t + bytes(0x414 - len(t))


def vram_info(vram_mb, bus_width):
    channel_width = 4  # 1 << 4 = 16-bit channels
    channel_num = bus_width >> channel_width
    module = struct.pack('<III6sHHBBBBBBBBBB20s',
                         vram_mb, (1 << channel_num) - 1, 0, bytes(6), 0, 0x34,
                         0, GDDR6_MARKER, channel_num, channel_width,
                         0, 0, 0, 0, 0, 0, b'RMB-UMA\0')
    assert len(module) == 0x34
    hdr = table_header(0x358, 2, 6) + bytes(16) + struct.pack('<BBBB', 1, 0, 0, 0)
    t = hdr + module
    return t + bytes(0x358 - len(t))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('inp')
    ap.add_argument('out')
    ap.add_argument('--vram-mb', type=int, required=True)
    ap.add_argument('--bus-width', type=int, required=True)
    a = ap.parse_args()

    rom = bytearray(open(a.inp, 'rb').read())
    sha = hashlib.sha256(rom).hexdigest()
    print(f'input {a.inp}: {len(rom):#x} bytes sha256 {sha}')
    if not sha.startswith(EXPECTED_IN_SHA256):
        sys.exit('input is not the Lenovo VFCT image this tool was verified against')
    assert rom[:2] == b'\x55\xaa'
    pcir = struct.unpack_from('<H', rom, 0x18)[0]
    assert rom[pcir:pcir + 4] == b'PCIR' and rom[pcir + 0x15] & 0x80, 'expects a single-image ROM'
    ven, dev = struct.unpack_from('<HH', rom, pcir + 4)
    assert (ven, dev) == (0x1002, 0x1681), (hex(ven), hex(dev))
    struct.pack_into('<H', rom, pcir + 6, 0x73FF)

    romhdr = struct.unpack_from('<H', rom, 0x48)[0]
    assert rom[romhdr + 4:romhdr + 8] == b'ATOM'
    mdt = struct.unpack_from('<H', rom, romhdr + 0x20)[0]
    for slot, blob in ((PSP_SLOT, psp_directory()), (VRAM_SLOT, vram_info(a.vram_mb, a.bus_width))):
        if struct.unpack_from('<H', rom, mdt + slot)[0]:
            sys.exit(f'slot {slot:#x} unexpectedly present')
        off = (len(rom) + 0xF) & ~0xF
        rom += bytes(off - len(rom)) + blob
        assert off + len(blob) <= 0xFFFF, 'ATOM table offsets are 16-bit'
        struct.pack_into('<H', rom, mdt + slot, off)
        print(f'slot {slot:#x}: {len(blob):#x} bytes @ {off:#x}')

    rom += bytes(-len(rom) % 512)
    blocks = len(rom) // 512
    assert blocks < 256
    rom[2] = blocks
    struct.pack_into('<H', rom, pcir + 0x10, blocks)
    rom[0x21] = 0
    rom[0x21] = -sum(rom) & 0xFF
    # Same checks as AmdBiosParserHelper::validateAtomBiosImage (@0x17350) + 64 KiB copy limit (@0x16cd3).
    assert rom[:2] == b'\x55\xaa' and rom[2] * 512 == len(rom) <= 0x10000
    assert sum(rom[:rom[2] * 512]) & 0xFF == 0
    open(a.out, 'wb').write(rom)
    print(f'wrote {a.out}: {len(rom):#x} bytes, PCIR 1002:73ff, sha256 {hashlib.sha256(rom).hexdigest()}')


if __name__ == '__main__':
    main()
