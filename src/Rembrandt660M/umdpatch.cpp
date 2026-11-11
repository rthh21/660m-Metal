//
//  Finder/zip/exe icon scramble: Apple's AMDRadeonX6000MTLDriver (dyld shared cache) gives every
//  64KB-swizzled surface a pipe/bank XOR (addrlib Gfx10 HwlComputePipeBankXor), and the managed
//  upload path and the GPU disagree about it on Rembrandt → textures ≥72 px land XOR-shifted.
//  textest `x` (in-process patch, 2026-10-24) proved pipeBankXor 0 makes every path correct.
//  Here the same 8 bytes are patched system-wide in every code page the kernel validates
//  (cs_validate_page, like Lilu's slow user patcher; on 15.0 x86 whole 4K pages never go through
//  cs_validate_range, that is only the sub-page chunk path).
//
//  Tahoe 25G229 (2026-11-10, opt-in `-rmbumd`):
//  - running Boot KC 58904DC1-52BD-302C-04C4-998D4364475C, kernel entry LC_UUID
//    1291D44A-C0CD-3AC7-A9E4-CA2749B33959 (= Tahoe-MinDelta/evidence/apple25G229/kernel).
//    cs_validate_page keeps the xnu-11215 ABI: rdi vp, rsi pager, rdx page_offset, rcx data,
//    r8 validated_p, r9 tainted_p, [rbp+0x10] nx_p; *validated_p = 0xf (VMP_CS_ALL_TRUE) when valid.
//    Prologue push rbp / mov rsp,rbp / push r15 / push r14 / push rbx / sub $0x18,rsp (13 bytes, no
//    rip-relative operand). Only vm_fault + vm_page_validate_cs_mapped(_slow) call it; cs_validate_range
//    is called by vm_page_validate_cs_mapped_chunk and load_machfile only, and Lilu's own user patcher
//    (RestrictEvents) takes that one, so only cs_validate_page is routed here.
//  - the hook goes in only when the 0x134 code bytes of cs_validate_page equal the 25G229 Boot KC bytes
//    (sha256 a853a59d2ebe4ed4...), else nothing is routed.
//  - the pattern is in 5 dyld-cache images, each whole inside one 4K page: AMDRadeonX6000MTLDriver
//    text+0x188edc (the one Metal uses here), AMDRadeonX5000MTLDriver, AMDMTLBronzeDriver,
//    AMDRadeonVADriver, AMDRadeonVADriver2 (all addrlib copies, so every AMD user agrees on xor 0).
//  - tools/gfxcheck TexUpload: 122/180 PASS with Apple's code, 180/180 with this patch applied in-process.
//

#include <Headers/kern_api.hpp>
#include <Headers/kern_mach.hpp>
#include "rmb.hpp"

namespace {

// movslq (%rdi,%rax,4),%rax; add %rdi,%rax; mov 4(%rsi),%esi; and $7,%esi;
// [mov (%rax,%rsi,4),%eax; add $2,%cl; shl %cl,%eax] -> xor %eax,%eax + nops; mov %eax,4(%rdx)
const uint8_t xorPat[] {0x48, 0x63, 0x04, 0x87, 0x48, 0x01, 0xf8, 0x8b, 0x76, 0x04, 0x83, 0xe6, 0x07, 0x8b,
                        0x04, 0xb0, 0x80, 0xc1, 0x02, 0xd3, 0xe0, 0x89, 0x42, 0x04};
const uint8_t xorRep[] {0x31, 0xc0, 0x0f, 0x1f, 0x40, 0x00, 0x66, 0x90};
constexpr size_t xorRepOff = 13;

mach_vm_address_t orgCsValidatePage;
uint32_t gXorPatched;

void writeRep(uint8_t *at, const char *how, size_t off) {
	if (MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) != KERN_SUCCESS) {
		LOG("umd: no write access for pipeBankXor patch");
		return;
	}
	memcpy(at, xorRep, sizeof(xorRep));
	MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
	if (++gXorPatched <= 8)
		LOG("umd: HwlComputePipeBankXor patched -> pipeBankXor 0 (%s, page off 0x%lx, #%u)", how,
		    (unsigned long)off, gXorPatched);
}

void patchPage(const void *data, size_t size) {
	auto p = static_cast<uint8_t *>(const_cast<void *>(data));
	if (!p || size < sizeof(xorPat))
		return;
	// 15.0 had the pattern straddling a 4K page (page offset 0xbfc) and matched an 11..23-byte tail at the
	// start of any page. On 25G229 all 5 copies lie whole inside one page (page offsets 0xdfc, 0xf6c,
	// 0xfac, 0x58c, 0xedc), so only the full 24-byte pattern is accepted: a short tail at some other
	// binary's page start can never be rewritten.
	for (size_t o = 0, end = size - sizeof(xorPat); o <= end; o++) {
		if (p[o] != 0x48 || memcmp(p + o, xorPat, sizeof(xorPat)))
			continue;
		writeRep(p + o + xorRepOff, "whole", o + xorRepOff);
	}
}

// xnu-11215 and 25G229 (xnu-12377.161.15.700.19): void cs_validate_page(vp, pager, page_offset, data,
//                                                   int *validated_p, int *tainted_p, int *nx_p)
void wrapCsValidatePage(void *vp, memory_object_t pager, memory_object_offset_t off, const void *data,
                        int *validated, int *tainted, int *nx) {
	FunctionCast(wrapCsValidatePage, orgCsValidatePage)(vp, pager, off, data, validated, tainted, nx);
	if (validated && *validated && tainted && !*tainted)
		patchPage(data, PAGE_SIZE);
}

// _cs_validate_page in the 25G229 Boot KC, 0xffffff8000907020..+0x134 (up to the padding).
const uint8_t csValidatePage25G229[] {
	0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x48, 0x83, 0xec, 0x18, 0x4c, 0x89, 0xcb,
	0x4d, 0x89, 0xc6, 0x48, 0x85, 0xff, 0x0f, 0x84, 0xa0, 0x00, 0x00, 0x00, 0x80, 0x7f, 0x71, 0x01,
	0x0f, 0x85, 0x96, 0x00, 0x00, 0x00, 0x4c, 0x8b, 0x47, 0x78, 0x4d, 0x85, 0xc0, 0x0f, 0x84, 0x89,
	0x00, 0x00, 0x00, 0x48, 0x89, 0xf8, 0x49, 0x8b, 0x78, 0x50, 0x48, 0x85, 0xff, 0x74, 0x7d, 0x48,
	0x39, 0x3d, 0x3a, 0x44, 0x91, 0xff, 0x0f, 0x87, 0xdb, 0x00, 0x00, 0x00, 0x48, 0x39, 0x3d, 0x35,
	0x44, 0x91, 0xff, 0x0f, 0x86, 0xce, 0x00, 0x00, 0x00, 0x44, 0x8b, 0x05, 0x70, 0x99, 0x91, 0xff,
	0x41, 0x89, 0xf9, 0x41, 0x81, 0xe1, 0xff, 0x0f, 0x00, 0x00, 0x41, 0xba, 0x00, 0x10, 0x00, 0x00,
	0x45, 0x29, 0xca, 0x45, 0x0f, 0xaf, 0xd0, 0x45, 0x39, 0xc2, 0x0f, 0x83, 0xa7, 0x00, 0x00, 0x00,
	0x4c, 0x8b, 0x05, 0x59, 0x44, 0x91, 0xff, 0x49, 0x89, 0xf9, 0x49, 0xc1, 0xe9, 0x08, 0x49, 0xba,
	0xf0, 0xff, 0xff, 0xff, 0x0f, 0x00, 0x00, 0x00, 0x4d, 0x21, 0xca, 0x47, 0x0f, 0xb7, 0x04, 0x02,
	0x41, 0x81, 0xe0, 0xff, 0x03, 0x00, 0x00, 0x66, 0x41, 0x83, 0xf8, 0x08, 0x75, 0x79, 0x48, 0x39,
	0x47, 0x08, 0x74, 0x0a, 0x48, 0x89, 0xc6, 0xe8, 0x4f, 0xa1, 0x2a, 0x00, 0x31, 0xff, 0x4c, 0x8b,
	0x7d, 0x10, 0x41, 0xc7, 0x06, 0x00, 0x00, 0x00, 0x00, 0xc7, 0x03, 0x00, 0x00, 0x00, 0x00, 0x41,
	0xc7, 0x07, 0x00, 0x00, 0x00, 0x00, 0xc7, 0x45, 0xe4, 0x00, 0x00, 0x00, 0x00, 0x48, 0xc7, 0x45,
	0xd8, 0x00, 0x00, 0x00, 0x00, 0x4c, 0x8d, 0x45, 0xd8, 0x4c, 0x8d, 0x4d, 0xe4, 0xe8, 0x3e, 0xf8,
	0xff, 0xff, 0x48, 0x83, 0x7d, 0xd8, 0x00, 0x74, 0x23, 0x85, 0xc0, 0x74, 0x07, 0x41, 0xc7, 0x06,
	0x0f, 0x00, 0x00, 0x00, 0x8b, 0x45, 0xe4, 0xa8, 0x01, 0x74, 0x06, 0xc7, 0x03, 0x0f, 0x00, 0x00,
	0x00, 0xa8, 0x02, 0x74, 0x07, 0x41, 0xc7, 0x07, 0x0f, 0x00, 0x00, 0x00, 0x48, 0x83, 0xc4, 0x18,
	0x5b, 0x41, 0x5e, 0x41, 0x5f, 0x5d, 0xc3, 0x48, 0x89, 0xfe, 0xbf, 0x08, 0x00, 0x00, 0x00, 0xe8,
	0x35, 0x34, 0x2a, 0x00
};
static_assert(sizeof(csValidatePage25G229) == 0x134, "25G229 cs_validate_page body");

}  // namespace

void umdRoute(KernelPatcher &patcher) {
	static bool routed = false;
	if (routed)
		return;
	if (!checkKernelArgument("-rmbumd")) {
		IOLog("REM660: [umdRoute] UMD code-signature hooking quarantined on Tahoe 25G229 (pass -rmbumd to enable)\n");
		return;
	}
	auto cvp = patcher.solveSymbol(KernelPatcher::KernelID, "_cs_validate_page");
	if (!cvp || patcher.getError() != KernelPatcher::Error::NoError) {
		LOG("umd: _cs_validate_page not found (err %d), pipeBankXor patch off", patcher.getError());
		patcher.clearError();
		return;
	}
	const uint8_t *code = reinterpret_cast<const uint8_t *>(cvp);
	if (memcmp(code, csValidatePage25G229, sizeof(csValidatePage25G229))) {
		size_t at = 0;
		while (at < sizeof(csValidatePage25G229) && code[at] == csValidatePage25G229[at]) at++;
		LOG("umd: _cs_validate_page is not the 25G229 code (first difference at +0x%lx), pipeBankXor patch off",
		    (unsigned long)at);
		IOLog("REM660: [umdRoute] cs_validate_page bytes differ from 25G229 at +0x%lx, not routed\n", (unsigned long)at);
		return;
	}
	IOLog("REM660: [umdRoute] installing UMD pipeBankXor=0 hook (-rmbumd, 25G229 cs_validate_page verified)\n");
	KernelPatcher::RouteRequest req {"_cs_validate_page", wrapCsValidatePage, orgCsValidatePage};
	if (!patcher.routeMultipleLong(KernelPatcher::KernelID, &req, 1)) {
		LOG("umd: failed to route cs_validate_page (err %d)", patcher.getError());
		IOLog("REM660: [umdRoute] failed to route cs_validate_page (err %d)\n", patcher.getError());
	} else {
		routed = true;
		LOG("umd: pipeBankXor 0 page patch installed (cs_validate_page, 25G229 bytes verified)");
		IOLog("REM660: [umdRoute] pipeBankXor 0 hook active (cs_validate_page)\n");
	}
	patcher.clearError();
}
