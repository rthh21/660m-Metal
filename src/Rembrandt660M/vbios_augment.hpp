// In-kernel twin of tools/make_lenovo_vbios.py (adapted GPL-2.0 from winmax2 vfct-rom.py + augment-rom.py).
// Pure byte code, no kernel/libc dependency: tests/vbios_augment.cpp checks it byte-for-byte against
// evidence/vbios/lenovo_660m-73ff-aug.rom (sha256 0b64dda9...).
//
// Why: 2026-11-08 boot log (saved.log, 2nd boot) shows "[vbios] ATY,bin_image MISSING on provider" although
// D:\EFI\OC\config.plist (sha256 F2FD8711...) carries it under PciRoot(0x0)/Pci(0x8,0x1)/Pci(0x0,0x0).
// AmdBiosParserHelper::readAtomBios (@0x173a2) then falls back to the PCI ROM BAR (fails: "Failed to get
// descriptor for ROM Base") -> 0xE00002C9 -> Core Services biosParser=0.
//
// Fail-closed: only the exact Lenovo VFCT image (1002:1681, 0xAE00 bytes, FNV-1a64 below = sha256
// 9df40403... offline) is accepted, and the result must hash to the offline tool's output.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace rmbvbios {

constexpr size_t kRawLen = 0xAE00;
constexpr size_t kOutLen = 0xB600;
constexpr uint64_t kRawFnv = 0x45263ce37d8098a3ULL;  // lenovo_660m_vfct_vbios.rom
constexpr uint64_t kOutFnv = 0x87fb54269c20b5e1ULL;  // lenovo_660m-73ff-aug.rom
constexpr uint32_t kVramMb = 2048, kBusWidth = 128;  // linux_amdgpu_dmesg.txt: "VRAM: 2048M", "RAM width 128bits"
constexpr uint8_t kGddr6Marker = 0x70;               // compatibility marker only, NOT a hardware claim

inline uint64_t fnv1a64(const uint8_t *p, size_t n) {
	uint64_t h = 0xcbf29ce484222325ULL;
	for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ULL;
	return h;
}
inline uint32_t rd16(const uint8_t *p, size_t o) { return p[o] | (p[o + 1] << 8); }
inline uint32_t rd32(const uint8_t *p, size_t o) { return rd16(p, o) | (rd16(p, o + 2) << 16); }
inline void wr16(uint8_t *p, size_t o, uint32_t v) { p[o] = v & 0xFF; p[o + 1] = (v >> 8) & 0xFF; }
inline void wr32(uint8_t *p, size_t o, uint32_t v) { wr16(p, o, v & 0xFFFF); wr16(p, o + 2, v >> 16); }

// Locates the 1002:1681 image inside the ACPI VFCT table. Returns nullptr + reason on any mismatch.
inline const uint8_t *findVfctImage(const uint8_t *vfct, size_t len, size_t *imgLen, const char **why) {
	if (!vfct || len < 0x4C || vfct[0] != 'V' || vfct[1] != 'F' || vfct[2] != 'C' || vfct[3] != 'T') { *why = "no VFCT signature"; return nullptr; }
	size_t tlen = rd32(vfct, 4);
	if (tlen > len) { *why = "VFCT length > data"; return nullptr; }
	size_t off = rd32(vfct, 52);
	while (off + 28 <= tlen) {
		uint32_t ven = rd16(vfct, off + 12), dev = rd16(vfct, off + 14), il = rd32(vfct, off + 24);
		if (!il || off + 28 + il > tlen) break;
		if (ven == 0x1002 && dev == 0x1681) { *imgLen = il; return vfct + off + 28; }
		off += 28 + il;
	}
	*why = "1002:1681 image not in VFCT";
	return nullptr;
}

// out must hold kOutLen bytes. Returns kOutLen on success, 0 + reason otherwise.
inline size_t augment(const uint8_t *raw, size_t rawLen, uint8_t *out, size_t outCap, const char **why) {
	if (rawLen != kRawLen || outCap < kOutLen) { *why = "unexpected raw length"; return 0; }
	if (fnv1a64(raw, rawLen) != kRawFnv) { *why = "raw image is not the verified Lenovo VBIOS"; return 0; }
	for (size_t i = 0; i < kOutLen; i++) out[i] = i < rawLen ? raw[i] : 0;
	uint32_t pcir = rd16(out, 0x18);
	if (out[0] != 0x55 || out[1] != 0xAA || out[pcir] != 'P' || rd16(out, pcir + 4) != 0x1002 || rd16(out, pcir + 6) != 0x1681) { *why = "PCIR mismatch"; return 0; }
	wr16(out, pcir + 6, 0x73FF);
	uint32_t romhdr = rd16(out, 0x48);
	if (out[romhdr + 4] != 'A' || out[romhdr + 5] != 'T' || out[romhdr + 6] != 'O' || out[romhdr + 7] != 'M') { *why = "no ATOM header"; return 0; }
	uint32_t mdt = rd16(out, romhdr + 0x20);
	if (rd16(out, mdt + 0x16) || rd16(out, mdt + 0x3C)) { *why = "PSP/vram_info slot already present"; return 0; }

	size_t end = rawLen;
	// PSP directory v2.1, 0 entries, 0x414 bytes (python psp_directory()).
	size_t psp = (end + 0xF) & ~size_t(0xF);
	wr16(out, psp, 0x414); out[psp + 2] = 2; out[psp + 3] = 1;
	out[psp + 4] = '$'; out[psp + 5] = 'P'; out[psp + 6] = 'S'; out[psp + 7] = 'P';
	end = psp + 0x414;
	wr16(out, mdt + 0x16, uint32_t(psp));
	// vram_info v2.6 + one atom_vram_module_v9 (python vram_info()).
	size_t vr = (end + 0xF) & ~size_t(0xF);
	wr16(out, vr, 0x358); out[vr + 2] = 2; out[vr + 3] = 6;
	out[vr + 20] = 1;  // vram_module_num
	size_t m = vr + 24;
	const uint32_t chw = 4, chn = kBusWidth >> chw;
	wr32(out, m + 0, kVramMb);
	wr32(out, m + 4, (1u << chn) - 1);
	wr16(out, m + 20, 0x34);  // vram_module_size
	out[m + 23] = kGddr6Marker;
	out[m + 24] = uint8_t(chn);
	out[m + 25] = uint8_t(chw);
	const char tag[] = "RMB-UMA";
	for (size_t i = 0; i < sizeof(tag); i++) out[m + 32 + i] = uint8_t(tag[i]);
	end = vr + 0x358;
	wr16(out, mdt + 0x3C, uint32_t(vr));

	end = (end + 511) & ~size_t(511);
	if (end != kOutLen) { *why = "layout size mismatch"; return 0; }
	uint32_t blocks = uint32_t(end / 512);
	out[2] = uint8_t(blocks);
	wr16(out, pcir + 0x10, blocks);
	out[0x21] = 0;
	uint8_t sum = 0;
	for (size_t i = 0; i < end; i++) sum = uint8_t(sum + out[i]);
	out[0x21] = uint8_t(-sum);
	if (fnv1a64(out, end) != kOutFnv) { *why = "result differs from offline tool output"; return 0; }
	return end;
}

}  // namespace rmbvbios
