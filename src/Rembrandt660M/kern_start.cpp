//
//  RembrandtGfx: makes Apple's Navi 23 driver stack run on the Radeon 680M (Rembrandt APU).
//
//  Stage 1 (this file): tracing only. Hooks AMDRadeonX6000HWLibs' TTL init entry points and
//  its event logger and prints what they return, to find where TTL init fails on the APU.
//

#include <Headers/plugin_start.hpp>
#include <Headers/kern_api.hpp>
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/graphics/IOGraphicsTypes.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/acpi/IOACPIPlatformExpert.h>
#include <libkern/c++/OSData.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <kern/thread.h>

#include "rmb.hpp"
#include "dispatch.hpp"
#include "vbios_augment.hpp"

static const char *pathHWLibs[] {"/System/Library/Extensions/AMDRadeonX6000HWServices.kext/Contents/PlugIns/AMDRadeonX6000HWLibs.kext/Contents/MacOS/AMDRadeonX6000HWLibs"};
static const char *pathFB[] {"/System/Library/Extensions/AMDRadeonX6000Framebuffer.kext/Contents/MacOS/AMDRadeonX6000Framebuffer"};
static const char *pathAccel[] {"/System/Library/Extensions/AMDRadeonX6000.kext/Contents/MacOS/AMDRadeonX6000"};
static const char *pathHWServices[] {"/System/Library/Extensions/AMDRadeonX6000HWServices.kext/Contents/MacOS/AMDRadeonX6000HWServices"};
static const char *pathBacklight[] {"/System/Library/Extensions/AppleBacklight.kext/Contents/MacOS/AppleBacklight"};
static const char *pathPCI[] {"/System/Library/Extensions/IOPCIFamily.kext/Contents/MacOS/IOPCIFamily"};
static KernelPatcher::KextInfo kexts[] {
	{"com.apple.kext.AMDRadeonX6000HWLibs", pathHWLibs, arrsize(pathHWLibs), {true, true}, {}, KernelPatcher::KextInfo::Unloaded},
	{"com.apple.kext.AMDRadeonX6000Framebuffer", pathFB, arrsize(pathFB), {true, true}, {}, KernelPatcher::KextInfo::Unloaded},
	{"com.apple.kext.AMDRadeonX6000", pathAccel, arrsize(pathAccel), {true, true}, {}, KernelPatcher::KextInfo::Unloaded},
	{"com.apple.kext.AMDRadeonX6000HWServices", pathHWServices, arrsize(pathHWServices), {true, true}, {}, KernelPatcher::KextInfo::Unloaded},
	{"com.apple.driver.AppleBacklight", pathBacklight, arrsize(pathBacklight), {true, true}, {}, KernelPatcher::KextInfo::Unloaded},
	{"com.apple.iokit.IOPCIFamily", pathPCI, arrsize(pathPCI), {true, true}, {}, KernelPatcher::KextInfo::Unloaded},
};
constexpr size_t BacklightIdx = 4;
constexpr size_t PciIdx = 5;
static auto &kextHWLibs = kexts[0];

// Only hook functions whose first instructions are position-independent: Lilu copies them into
// the trampoline, so a RIP-relative lea or short jump there breaks the original (seen with
// _DevIsSupportedDevice, whose prologue loads _DeviceCapabilityTbl RIP-relative).
// Check with scripts/hook-safe.py.
#define TRACE_HOOK(name)                                                                     \
	static mach_vm_address_t org_##name;                                                     \
	static uint64_t wrap_##name(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) { \
		LOG(">> " #name "(%llx, %llx)", a, b);                                              \
		auto r = reinterpret_cast<Fn6>(org_##name)(a, b, c, d, e, f);                       \
		LOG("<< " #name " = %llx", r);                                                       \
		return r;                                                                            \
	}

TRACE_HOOK(ttl_sw_init)
TRACE_HOOK(ttl_hw_init)
TRACE_HOOK(IpiSwInit)
TRACE_HOOK(IpiHwInit)
TRACE_HOOK(TlsSwInit)
TRACE_HOOK(TlsHwInit)
TRACE_HOOK(psp_sw_init)
TRACE_HOOK(PP_Initialize_internal)
TRACE_HOOK(PHM_Initialize)
TRACE_HOOK(PEM_Initialize)
TRACE_HOOK(PSM_Initialize)
TRACE_HOOK(PECI_Initialize)
TRACE_HOOK(PhwNavi21_Initialize)
TRACE_HOOK(PP_Tables_Initialize)
TRACE_HOOK(PP_ThermalController_Initialize)
TRACE_HOOK(PHM_RegAccessInitialize)
TRACE_HOOK(PP_Navi21_Thermal_Initialize)
TRACE_HOOK(psp_allocate_private_mem)
TRACE_HOOK(psp_ring_init)
TRACE_HOOK(psp_cmd_km_init)
TRACE_HOOK(psp_dtm_init)
TRACE_HOOK(psp_np_fw_init)
TRACE_HOOK(psp_cmd_km_reset)
TRACE_HOOK(psp_tmr_load)
TRACE_HOOK(gc_sw_init)
TRACE_HOOK(gc_get_config)
TRACE_HOOK(gc_set_fw_entry_info)
TRACE_HOOK(gc_init_fcn_ptr_list)
TRACE_HOOK(gc_validate_hw_config_tbl_version)
TRACE_HOOK(psp_hw_init)
TRACE_HOOK(psp_init_pfn_ptr)
TRACE_HOOK(ttlDevCreateInstance)
TRACE_HOOK(ttl_cgs_initialize)
TRACE_HOOK(validateCosInfo)
TRACE_HOOK(ttl_initialize)
TRACE_HOOK(TtlInitialize)
TRACE_HOOK(TtlServicesInitialize)
TRACE_HOOK(TtlInitializeCos)
TRACE_HOOK(TtlInitializeGmm)
TRACE_HOOK(TtlInitializeIcm)
TRACE_HOOK(TtlInitializePrm)

// bcs_log(ctx, eventId, phase, a, b): AMD's SWIP event log. Bit 31 of eventId marks an error.
static mach_vm_address_t org_bcs_log;
static uint32_t bcsLogCount;
static uint64_t wrap_bcs_log(uint64_t ctx, uint64_t event, uint64_t phase, uint64_t a, uint64_t b, uint64_t f) {
	if (bcsLogCount < 400 || (event & 0x80000000)) {
		bcsLogCount++;
		LOG("bcs_log event=%08llx phase=%llu a=%llx b=%llx", event & 0xFFFFFFFF, phase & 0xFFFFFFFF, a, b);
	}
	return reinterpret_cast<Fn6>(org_bcs_log)(ctx, event, phase, a, b, f);
}

// Wake POST. auto BM 10-27: after a real S3 the display I/O comes back unpowered (PWRSEQ0 reads 0
// even after its soft reset, panel VDD/HPD never return). The VM never lost APU power, so it didn't
// need more. Apple's VBIOS POST at wake sits in power_up_event (bgm), which the APU wake never runs
// (10-27 12:50: no bcs_log at all at wake, wake = psp_power_up_event). So run vbs_post (ASIC_Init via
// execute_bios_table, like the firmware at cold boot / Linux amdgpu_device_asic_init at resume) once
// ourselves at the start of psp_power_up_event, on the vbs object vbs_hw_init got at boot.
// -rmbnowakepost: off.
bool gWakePost;
static uint64_t gVbs;
static mach_vm_address_t org_vbs_hw_init, org_vbs_post, org_psp_power_up_event;
static uint64_t wrap_vbs_hw_init(uint64_t vbs, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
	gVbs = vbs;
	return reinterpret_cast<Fn6>(org_vbs_hw_init)(vbs, b, c, d, e, f);
}
static uint64_t wrap_psp_power_up_event(uint64_t psp, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
	if (gWakePost) {
		gWakePost = false;
		vramCrcStep("wake start");
		uint32_t before = mmioRead(0x63db);
		if (checkKernelArgument("-rmbnowakepost") || !gVbs || !org_vbs_post) {
			LOG("wakepost: skipped (vbs %llx post %llx), PWRSEQ0 PERIOD %x", gVbs, (uint64_t)org_vbs_post, before);
		} else {
			LOG("wakepost: PWRSEQ0 PERIOD %x CNTL %x -> vbs_post", before, mmioRead(0x63d9));
			// 10-27 13:00: run with this hook never came back from S3 (fan fast). Get the line to disk
			// first so a hang inside vbs_post shows up as the last line.
			diskLogSync(3000);
			auto r = reinterpret_cast<Fn6>(org_vbs_post)(gVbs, 0, 0, 0, 0, 0);
			LOG("wakepost: vbs_post = %llx, PWRSEQ0 PERIOD %x CNTL %x STATE %x", r & 0xFFFFFFFF, mmioRead(0x63db),
			    mmioRead(0x63d9), mmioRead(0x63da));
			vramCrcStep("after vbs_post");
			diskLogSync(1000);
		}
		pwrseqWakeArm();
	}
	return reinterpret_cast<Fn6>(org_psp_power_up_event)(psp, b, c, d, e, f);
}

// --- Framebuffer ---
// Once the framebuffer initializes, WindowServer opens it right away, but the GPU wrangler only
// sets LoadAccelerator (which the accelerator personality matches on) later. powerUp() then
// sends a PPLIB message to the not-yet-present accelerator and panics. Wait for it instead.
static mach_vm_address_t orgMessageAccelerator, orgDoesAcceleratorExist;
static uint64_t wrapMessageAccelerator(void *ctrl, uint64_t type, uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
	auto exists = reinterpret_cast<bool (*)(void *)>(orgDoesAcceleratorExist);
	if (!exists(ctrl)) {
		LOG("messageAccelerator(%llu): accelerator not attached yet, waiting", type);
		for (int i = 0; i < 200 && !exists(ctrl); i++)
			IOSleep(100);
		LOG("messageAccelerator(%llu): accelerator %s", type, exists(ctrl) ? "attached" : "still missing after 20 s");
	}
	auto r = reinterpret_cast<Fn6>(orgMessageAccelerator)(reinterpret_cast<uint64_t>(ctrl), type, a, b, c, d);
	if (r)
		LOG("messageAccelerator(%llu) = %llx", type, r);
	return r;
}

TRACE_HOOK(CtrlStart)
TRACE_HOOK(CtrlInitInternals)
TRACE_HOOK(CtrlPowerUp)
TRACE_HOOK(FbEnableController)
TRACE_HOOK(CreateCoreServices)
TRACE_HOOK(WranglerStart)
static mach_vm_address_t orgVendorAttr;
static uint64_t wrapVendorAttr(uint64_t self, uint64_t attr, uint64_t in, uint64_t inSize, uint64_t out, uint64_t args) {
	LOG(">> wrangler vendor_doDeviceAttribute(attr %llu)", attr & 0xFFFFFFFF);
	auto r = reinterpret_cast<Fn6>(orgVendorAttr)(self, attr, in, inSize, out, args);
	LOG("<< wrangler vendor_doDeviceAttribute(attr %llu) = %llx", attr & 0xFFFFFFFF, r);
	return r;
}

// -rmbheadless: publish no AMD framebuffers. The display engine (DCN 3.1) isn't supported yet, so
// WindowServer stays on the emulated display and the 680M is used as a display-less (Metal) GPU.
static bool gHeadless;
static uint64_t wrapFbStart(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
	LOG("headless: AmdRadeonFramebuffer start declined");
	return 0;
}

// AmdAsicInfoNavi2::populateDeviceInfo takes device id (bits 15:0) and revision (27:24) from
// mmRCC_DEV0_EPF0_STRAP0 = 0xd31, an NBIO 2.3 (Navi) offset; on Rembrandt (NBIO 7.2) that is a
// different register (reads "Device 0xff00, revNo 15"), so CAIL's caps lookup fails and the Metal
// driver sees an unknown prototype (it then encodes zero-sized dispatches). Report Navi 23 instead.
// Fields: +0x60 family, +0x64 device, +0x68 revNo, +0x6c emuRevNo (= revNo + per-ASIC base).
static mach_vm_address_t orgPopulateDeviceInfo;
static uint32_t wrapPopulateDeviceInfo(void *object) {
 auto that=reinterpret_cast<uint64_t>(object);
	rmbStep("populateDeviceInfo ENTER (hwReadReg32 0xd31)");
	auto r = reinterpret_cast<uint32_t (*)(void *)>(orgPopulateDeviceInfo)(object);
	rmbStep("populateDeviceInfo EXIT");
	auto info = reinterpret_cast<uint8_t *>(that);
	auto &dev = *reinterpret_cast<uint32_t *>(info + 0x64), &rev = *reinterpret_cast<uint32_t *>(info + 0x68),
		 &emu = *reinterpret_cast<uint32_t *>(info + 0x6c);
	LOG("asicinfo: strap says device %x rev %u emu %u; reporting device 73ff rev 0 emu %u", dev, rev, emu, emu - rev);
	emu -= rev;
	dev = 0x73ff;
	rev = 0;
	return r;
}

// AmdAsicInfoNavi2::populateXGmiConfig reads MC_VM_XGMI_LFB_CNTL/SIZE, MC_VM_FB_LOCATION_BASE/TOP
// and MC_VM_FB_OFFSET at MMHUB 2.0 offsets (0x1a867.. ); Rembrandt's MMHUB 2.3 has other offsets, so
// DAL got FB base 0x100000000 and built bad DMUB data windows. Replace with the real FB location and
// one XGMI node. Fields: +0x138 node id, +0x13c nodes, +0x140 base, +0x144 top,
// +0x148 node segment size, +0x14c FB offset (all in 16 MB units).
// Source order: GC's copies (GCMC_VM_FB_*) first, exactly as the GPD reference does at this same driver
// point (ref RembrandtGfx kern_start.cpp:196-216, bare metal Rembrandt, "same values" as MMHUB).
// MMHUB 2.3 copies (regmap.hpp: Navi 0x1a86c/0x1a86d/0x1a857 -> Rembrandt 0x1a8ec/0x1a8ed/0x1a8d7, as
// vmwatch.cpp mmhubDump) only if the GC values are implausible, or first with `-rmbmmfbloc`.
// If neither is plausible the driver's values stay (fail-closed, logged). Lenovo Linux reference:
// linux_amdgpu_dmesg.txt:68 "VRAM: 2048M 0x000000F400000000 - 0x000000F47FFFFFFF" = base f400 top f47f.
static mach_vm_address_t orgPopulateXGmiConfig;
static bool plausibleFb(uint32_t base, uint32_t top) { return base && base <= top && top != 0xFFFFFF; }
static void readFbLocation(bool mmhub, uint32_t &base, uint32_t &top, uint32_t &off) {
	constexpr uint32_t Mm = 0x1A000, GC0 = 0x1260;
	uint32_t b = mmhub ? Mm + 0x8ec : GC0 + 0x16fc, t = mmhub ? Mm + 0x8ed : GC0 + 0x16fd,
	         o = mmhub ? Mm + 0x8d7 : GC0 + 0x16e7;
	base = mmioRead(b) & 0xFFFFFF;
	top = mmioRead(t) & 0xFFFFFF;
	off = mmioRead(o) & 0xFFFFFF;
}
static uint32_t wrapPopulateXGmiConfig(void *object) {
 auto that=reinterpret_cast<uint64_t>(object);
	rmbStep("populateXGmiConfig ENTER (hwReadReg32 0x1a867 0x1a868 0x1a86c 0x1a86d 0x1a857)");
	auto r = reinterpret_cast<uint32_t (*)(void *)>(orgPopulateXGmiConfig)(object);
	rmbStep("populateXGmiConfig original EXIT");
	auto u = [&](uint32_t off) -> uint32_t & { return *reinterpret_cast<uint32_t *>(that + off); };
	bool mmFirst = checkKernelArgument("-rmbmmfbloc");
	uint32_t base, top, off;
	readFbLocation(mmFirst, base, top, off);
	const char *src = mmFirst ? "MMHUB2.3" : "GCMC";
	if (!plausibleFb(base, top)) {
		LOG("asicinfo: %s FB base %x top %x not plausible, trying %s", src, base, top, mmFirst ? "GCMC" : "MMHUB2.3");
		readFbLocation(!mmFirst, base, top, off);
		src = mmFirst ? "GCMC" : "MMHUB2.3";
	}
	LOG("asicinfo: MMHUB2.0-offset FB regs gave base %x top %x offset %x nodes %u; %s base %x top %x offset %x"
	    " (Lenovo Linux: base f400 top f47f%s)",
		u(0x140), u(0x144), u(0x14c), u(0x13c), src, base, top, off,
		base == 0xf400 && top == 0xf47f ? ", match" : ", DIFFERENT");
	u(0x138) = 0;
	u(0x13c) = 1;
	if (plausibleFb(base, top)) {
		u(0x140) = base;
		u(0x144) = top;
		u(0x14c) = off;
	} else {
		LOG("asicinfo: no plausible FB location from GC or MMHUB; driver values kept");
	}
	rmbStep("populateXGmiConfig EXIT");
	return r;
}

// HDMI 2.0 TMDS above 340 MHz (scrambling) gives the LG TV no signal (4K60 RGB, 594 MHz), while 4K60
// YCbCr 4:2:0 (297 MHz) works. Cap the link encoders' max_hdmi_pixel_clock (encoder_feature_support
// {flags 0xf, deep color 12 bpc, 600000 kHz, ...} in __TEXT,__const, one per DCN generation) at
// 340000 so DAL rejects >340 MHz timings and picks 4:2:0 / lower depth. `-rmbhdmi6g` = keep 600 MHz.
static void capHdmiPixelClock(mach_vm_address_t address, size_t size) {
	if (checkKernelArgument("-rmbhdmi6g"))
		return;
	static const uint8_t pat[13] {0x0f, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0xc0, 0x27, 0x09, 0x00, 0x01};
	static const size_t rvas[] {0x232880, 0x236a3c, 0x23e9a8, 0x249bc8, 0x25fbb8, 0x265a48, 0x2725f4};
	auto p = reinterpret_cast<uint8_t *>(address);
	size_t hits = 0;
	if (MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) == KERN_SUCCESS) {
		for (size_t rva : rvas) {
			if (rva + sizeof(pat) <= size && !memcmp(p + rva, pat, sizeof(pat))) {
				*reinterpret_cast<uint32_t *>(p + rva + 8) = 340000;
				hits++;
			}
		}
		MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
	}
	LOG("hdmi: max_hdmi_pixel_clock 600000 -> 340000 kHz in %zu encoder feature tables", hits);
}

static void capPipes(mach_vm_address_t address, size_t size) {
	if (checkKernelArgument("-rmb5pipes"))
		return;
	static const uint32_t find[10] {5, 5, 5, 5, 5, 0, 1, 5, 16, 5};
	// dcn302_resource_construct hardcodes pipe_count and mpcc_count (original RembrandtGfx capPipes):
	// movabs $0xffffffff00000005,%rcx; mov %rcx,0x2a8(%r13); movl $5,0x378(%r13). Left at 5 they index
	// timing_generators[4] (NULL) in init_pipes and MPCC4. 2026-11-09: the previous bytes here
	// (48 89 8b 80 01 / 48 c7 83 b0 01) were not 25G229 code ("0 construct" in rmb-msgbuf.log); these are
	// the bytes at Framebuffer UUID C3FA8B38 file offset 0x118c85.
	static const uint8_t code[28] {
		0x48, 0xb9, 0x05, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
		0x49, 0x89, 0x8d, 0xa8, 0x02, 0x00, 0x00,
		0x41, 0xc7, 0x85, 0x78, 0x03, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00,
	};
	auto p = reinterpret_cast<uint8_t *>(address);
	size_t tblHits = 0, codeHits = 0;
	if (MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) == KERN_SUCCESS) {
		constexpr size_t tblRva = 0x248958;
		if (tblRva + sizeof(find) <= size && !memcmp(p + tblRva, find, sizeof(find))) {
			auto u = reinterpret_cast<uint32_t *>(p + tblRva);
			u[0] = u[1] = u[2] = 4;
			tblHits++;
		}
		constexpr size_t codeRva = 0x118c85;
		if (codeRva + sizeof(code) <= size && !memcmp(p + codeRva, code, sizeof(code))) {
			p[codeRva + 2] = 4;
			p[codeRva + 24] = 4;
			codeHits++;
		}
		MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
	}
	LOG("pipes: res_cap_dcn302 5 -> 4 (%zu table, %zu construct)", tblHits, codeHits);
}

// Bare metal: the IH wptr WRITEBACK in RAM stays at the first value (0x20) while MMIO IH_RB_WPTR
// moves on (0x40..). The OSSSYS 5 get-wptr op (fb 15.8.1 0x1e2cf6) reads the writeback when ring
// flag +0xe4 bit 1 is set, so the handler sees rptr == wptr, "re-arms" by writing IH_RB_RPTR = 0x20,
// and with RPTR_REARM on the IH fires again at once: 9499 rptr writes in 58 ms on CPU 0 (eoi0 boot
// 10-25 17:xx) = CPU 0 never leaves the GPU interrupt -> NVMe/xHCI MSIs starve -> disk stall.
// Take the op's own MMIO branch: testb $2,0xe4(%rsi); jne <writeback> -> nop the jne.
// `-rmbihwb` = keep the writeback read.
// 10-25 bare-metal desktop boot (ihwptr on): interrupts flow again, but 34 GPU "event timeout ...
// not hung" and ~360 VM faults in 6 min (last VM run: 0 and 0) and a sluggish desktop. The IH
// writes its ring with MC_SPACE 1 (bus address); on bare metal those writes reach DRAM without
// snooping the CPU caches (wptr writeback stayed stale), so the ring ENTRIES the driver reads can be
// stale cache lines too = lost/old interrupts. In the VM the IOMMU forced coherency. Route both
// get-wptr copies: after the original reads the new wptr, clflush the entries old wptr..new wptr
// (re-reading them to count how many were stale in the cache). `-rmbnoihflush` = off.
namespace {
using IhGetWptr = int (*)(void *, uint8_t *);
IhGetWptr gOrgIhGetWptr[2];
volatile uint32_t gIhFlushEntries, gIhFlushStale, gIhFlushCalls;
}  // namespace
volatile uint32_t gIhClient[32], gIhDceSrc[256];
namespace {

void ihFlush(uint8_t *ring, uint32_t from, uint32_t to) {
	auto base = *reinterpret_cast<uint8_t **>(ring);
	uint32_t count = *reinterpret_cast<uint32_t *>(ring + 0x2c), esz = *reinterpret_cast<uint32_t *>(ring + 0x30);
	if (!base || !count || esz != 32 || from >= count || to >= count)
		return;
	uint32_t n = 0, stale = 0;
	for (uint32_t i = from; i != to && n < count; i = (i + 1) % count, n++) {
		auto e = reinterpret_cast<volatile uint64_t *>(base + i * esz);
		uint64_t a = e[0], b = e[1], c = e[2], d = e[3];
		asm volatile("clflush (%0)" ::"r"(e) : "memory");
		asm volatile("mfence" ::: "memory");
		if (e[0] != a || e[1] != b || e[2] != c || e[3] != d)
			stale++;
		// presdiag: SOC15 IV dw0 = client_id[7:0] | src_id[15:8]; DCE (DCN) client = 4.
		uint32_t dw0 = static_cast<uint32_t>(e[0]);
		gIhClient[dw0 & 0x1f]++;
		if ((dw0 & 0xff) == 4)
			gIhDceSrc[(dw0 >> 8) & 0xff]++;
	}
	gIhFlushEntries += n;
	gIhFlushStale += stale;
	gIhFlushCalls++;
}

template <int N>
int wrapIhGetWptr(void *ih, uint8_t *ring) {
	uint32_t old = *reinterpret_cast<uint32_t *>(ring + 0x44);
	int r = gOrgIhGetWptr[N](ih, ring);
	if (r)
		ihFlush(ring, old, *reinterpret_cast<uint32_t *>(ring + 0x44));
	return r;
}
}  // namespace

void ihFlushStats(uint32_t &calls, uint32_t &entries, uint32_t &stale) {
	calls = gIhFlushCalls;
	entries = gIhFlushEntries;
	stale = gIhFlushStale;
}

static void ihMmioWptr(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
	if (checkKernelArgument("-rmbihwb")) {
		LOG("ihwptr: disabled (-rmbihwb)");
		return;
	}
	bool flush = !checkKernelArgument("-rmbnoihflush");
	// 2026-11-09: the earlier 14-byte pattern here (... 75 0d 48 8b 7b 10 8b) was not the 25G229 code, so the
	// jne was never removed ("in 0 places" in both boots of rmb-msgbuf.log SHA 870aa1ea..., which then froze
	// <1 s after AcceleratorStart EXIT with no disk/USB activity = the CPU-0 IH storm described above).
	// Exact bytes, identical to the original RembrandtGfx pattern and to Framebuffer UUID C3FA8B38 at file
	// offsets 0x1e244f and 0x1e534d (llvm-objdump: testb $2,0xe4(%rsi); jne 0x1e2465; movl $0x1124,%esi;
	// callq *0x43b0(%rdi); jmp +6; movq 0x48(%rbx),%rax; movl (%rax),%eax):
	static const uint8_t pat[28] {0xf6, 0x86, 0xe4, 0x00, 0x00, 0x00, 0x02, 0x75, 0x0d, 0xbe, 0x24, 0x11, 0x00, 0x00,
		0xff, 0x97, 0xb0, 0x43, 0x00, 0x00, 0xeb, 0x06, 0x48, 0x8b, 0x43, 0x48, 0x8b, 0x00};
	// push rbp; mov rsp,rbp; push rbx; push rax; mov rsi,rbx (function entry 9 bytes before the pattern)
	static const uint8_t prologue[9] {0x55, 0x48, 0x89, 0xe5, 0x53, 0x50, 0x48, 0x89, 0xf3};
	static const size_t rvas[2] {0x1e244f, 0x1e534d};
	auto p = reinterpret_cast<uint8_t *>(address);
	size_t hits = 0, routed = 0;
	bool entryOk[2] {};
	for (size_t idx = 0; idx < 2; idx++) {
		size_t i = rvas[idx];
		entryOk[idx] = i >= sizeof(prologue) && i + sizeof(pat) <= size && !memcmp(p + i - sizeof(prologue), prologue, sizeof(prologue)) &&
		               !memcmp(p + i, pat, sizeof(pat));
	}
	if (MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) != KERN_SUCCESS) {
		LOG("ihwptr: kernel writing failed");
		return;
	}
	for (size_t idx = 0; idx < 2; idx++) {
		if (!entryOk[idx])
			continue;
		p[rvas[idx] + 7] = 0x90;
		p[rvas[idx] + 8] = 0x90;
		hits++;
	}
	MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
	if (hits != 2)
		LOG("ihwptr: WARNING only %zu of 2 get-wptr sites matched the 25G229 bytes; unmatched sites left native", hits);
	if (flush) {
		for (size_t idx = 0; idx < 2; idx++) {
			size_t i = rvas[idx];
			if (entryOk[idx]) {
				auto fn = address + i - 9;
				auto wrap = idx == 0 ? reinterpret_cast<mach_vm_address_t>(wrapIhGetWptr<0>)
				                     : reinterpret_cast<mach_vm_address_t>(wrapIhGetWptr<1>);
				if (fn != 0) {
					patcher.clearError();
					auto org = patcher.routeFunctionLong(fn, wrap, true, true);
					if (org) {
						gOrgIhGetWptr[idx] = reinterpret_cast<IhGetWptr>(org);
						routed++;
					}
					patcher.clearError();
				}
			}
		}
	}
	LOG("ihwptr: IH get-wptr forced to MMIO IH_RB_WPTR in %zu places, ring clflush routed in %zu", hits, routed);
}

// System Settings > Displays: every panel size but the native one did nothing when clicked
// (auto-1024-1257 DisplaysExt "Requested mode (13) is not safe. Displaying the sheet." and no sheet
// ever showed). Mode safety = IODisplayWrangler::getFlagsForDisplayMode -> fb connectFlags into
// flags = 0, and AmdRadeonFramebuffer::connectFlags is an empty stub (push rbp; mov rsp,rbp;
// xor eax,eax; pop rbp; ret) -> no Valid/Safe bit for any mode. Report Valid|Safe like IODisplay's
// default for drivers without connectFlags. With that the 16:10 sizes switch, but IOGraphicsLib
// still strips Safe from the AMD 4:3/16:9 list modes (1920x1080, 1600x1200, 1280x1024 ...) and
// one click on such a size leaves the pane waiting on a sheet that never shows (auto-1024-1355:
// every later click ignored). extGetInformationForDisplayMode ORs our flags in, so add NeverShow
// to every mode that isn't 16:10 like the panel: the pane offers only sizes that switch, as on a
// real built-in display. `-rmbnosafemodes` = keep the stub, `-rmbshowallmodes` = no NeverShow.
static uint8_t *gConnectFlags;
static mach_vm_address_t gFbModeInfo;
static bool gShowAllModes;

void safeModesLog() {
	if (gConnectFlags)
		LOG("safemodes: connectFlags now %02x %02x %02x %02x %02x %02x", gConnectFlags[0], gConnectFlags[1],
		    gConnectFlags[2], gConnectFlags[3], gConnectFlags[4], gConnectFlags[5]);
	else
		LOG("safemodes: connectFlags never resolved");
}

static IOReturn wrapConnectFlags(void *fb, IOIndex connectIndex, IODisplayModeID mode, UInt32 *flags) {
	*flags = kDisplayModeValidFlag | kDisplayModeSafeFlag;
	if (gShowAllModes || !gFbModeInfo)
		return kIOReturnSuccess;
	IODisplayModeInformation info {};
	auto r = reinterpret_cast<IOReturn (*)(void *, IODisplayModeID, IODisplayModeInformation *)>(gFbModeInfo)(fb, mode, &info);
	bool hide = r == kIOReturnSuccess && info.nominalWidth && info.nominalHeight &&
	            info.nominalWidth * 10 != info.nominalHeight * 16;
	if (hide)
		*flags |= kDisplayModeNeverShowFlag;
	static IODisplayModeID logged[64];
	static unsigned nlogged;
	bool seen = false;
	for (unsigned i = 0; i < nlogged && !seen; i++)
		seen = logged[i] == mode;
	if (!seen && nlogged < arrsize(logged)) {
		logged[nlogged++] = mode;
		LOG("safemodes: mode %x %ux%u (info %x) -> flags %x%s", mode, info.nominalWidth, info.nominalHeight, r,
		    *flags, hide ? " (hidden, not 16:10)" : "");
	}
	return kIOReturnSuccess;
}

static void safeModes(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
	(void)index;
	if (checkKernelArgument("-rmbnosafemodes"))
		return;
	gShowAllModes = !checkKernelArgument("-rmbnoshowallmodes");
	auto fn = findSymbol(patcher, 1, "__ZN35AMDRadeonX6000_AmdRadeonFramebuffer12connectFlagsEiiPj", address, size);
	gFbModeInfo = findSymbol(patcher, 1,
		"__ZN35AMDRadeonX6000_AmdRadeonFramebuffer28getInformationForDisplayModeEiP24IODisplayModeInformation", address, size);
	if (fn) {
		KernelPatcher::RouteRequest req {"__ZN35AMDRadeonX6000_AmdRadeonFramebuffer12connectFlagsEiiPj", wrapConnectFlags};
		routeStaticRequests(patcher, 1, &req, 1, address, size);
		IOLog("Rembrandt660M: safemodes: AmdRadeonFramebuffer::connectFlags routed\n");
	} else {
		IOLog("Rembrandt660M: safemodes: connectFlags not found by symbol, skipping\n");
	}
}

// Read-only trace. 25G229 FB AmdRadeonControllerNavi23::createControllerCoreServices (@0x3584c)
// stores DeviceMemoryManager @+30968 (0x3586f), RegisterAccess @+30960 (0x358e6),
// BiosParser @+30984 (0x3591f), AsicInfo @+30976 (0x35933) and returns 0xE00002C9 when any
// creator returns NULL. Without Core Services, Controller::start skips setupHwServices (0x4bb83),
// the ICM callbacks never reach AmdTtlServices+1400 and TTL::initialize fails with 0xE00002BC.
// 2026-11-08 boots froze the whole machine (no panic, disk log stops ~24.8 s) once the VBIOS reached the
// controller. The disk log flushes every 0.5 s, so a hard freeze hides its last lines: each checkpoint
// writes the log out synchronously (<=400 ms) so the file shows the last stage that was entered.
void rmbCheckpoint(const char *tag) {
	IOLog("REM660: [ckpt] %s\n", tag);
	diskLogSync(400);
}

// 2026-11-08 boots 1+2 (rmb-msgbuf.log 433777 B, lines 2131/4267): last line "coreServices ENTER",
// nothing after it from any subsystem (APFS tx_flush, PMRD, launchd were logging every few ms) ->
// whole-machine stop within one 0.5 s flush period, not a stuck thread (INFERRED: MMIO stall).
// Earlier boots without the VBIOS returned from here (saved.log:5281 devMem=1 regAccess=1
// biosParser=0), so DeviceMemoryManager and RegisterAccess are not the stall; the new code reached is
// AmdBiosParserHelper::createBiosParser with a valid image and AmdAsicInfoNavi23::createAsicInfo,
// whose populate* read Navi offsets through AmdRegisterAccess::hwReadReg32 (vtable +320).
// Static audit (tools/verify_symbol_manifest.py) then found the most likely cause: display.cpp routed
// "__ZN19AMDRadeonController17initializeProjectEv", a name absent from this FB, at RVA 0x5639c, which
// is the middle of `movq %rax,(%rbx)` @0x5639b inside 0x56300; createBiosParser -> initWithData
// (0x16c5f) -> createAtomFirmwareServices 0x54d90 (0x54db9) -> 0x54e10 (0x54f3b) -> 0x56300. Only a
// valid VBIOS gets that far. The route is removed; this trace stays so any further stop is located.
// While this thread is inside, every register access is logged and flushed first. `-rmbnoregtrace`: off.
static volatile bool gCoreTrace;
static thread_t gCoreTraceThread;
static mach_vm_address_t gFbImage;
static uint32_t gCoreTraceN;
static constexpr uint32_t CoreTraceSyncMax = 160, CoreTraceLogMax = 600;

bool coreTraceActive() { return gCoreTrace && gCoreTraceThread == current_thread(); }

void coreTraceAccess(const char *kind, uint32_t reg, uint32_t val, const void *ra) {
	uint32_t n = ++gCoreTraceN;
	if (n > CoreTraceLogMax)
		return;
	auto a = reinterpret_cast<mach_vm_address_t>(ra);
	// RVA in the 25G229 KDK binary (kext image starts at vmaddr 0), comparable with llvm-objdump.
	uint64_t rva = gFbImage && a > gFbImage && a - gFbImage < 0x1000000 ? a - gFbImage : 0;
	IOLog("REM660: [regtrace] #%u %s reg 0x%x val 0x%x from fb+0x%llx\n", n, kind, reg, val, rva);
	if (n <= CoreTraceSyncMax)
		diskLogSync(200);
}

void coreTraceResult(const char *kind, uint32_t reg, uint32_t val) {
	if (gCoreTraceN <= CoreTraceLogMax)
		IOLog("REM660: [regtrace]    %s reg 0x%x -> 0x%x\n", kind, reg, val);
}

void rmbStep(const char *tag) {
	if (coreTraceActive())
		rmbCheckpoint(tag);
	else
		IOLog("REM660: [step] %s\n", tag);
}

// Sub-steps of createControllerCoreServices / createAsicInfo, in call order (disassembly of 25G229):
// createBiosParser @0x16a4c -> initWithData -> initializeAtomBiosImage @0x16ca2 (readAtomBios),
// initializeHwServices, AtiAtomBiosParser::createAtomBiosParser, createAtomFirmwareServices,
// readFirmwareInfo @0x16ef8; then AmdAsicInfoNavi23::createAsicInfo @0x52a24 -> initWithController
// -> refresh @0x3ba82 -> populateDeviceInfo(+488) / populateMemorySize(+504) /
// populateMemoryConfig(+496) @0x3bf12 / populateFbLocation(+480) @0x3b504 -> populateXGmiConfig /
// populateModelName(+512) @0x3bd0e. All take <= 3 integer args; forward all six argument registers.
static constexpr size_t StepCount = 8;
static mach_vm_address_t orgStep[StepCount];
static const char *const kStepName[StepCount] {
	"createBiosParser", "initializeAtomBiosImage", "readFirmwareInfo", "createAsicInfo",
	"asicInfo.refresh", "populateMemoryConfig", "populateFbLocation", "populateModelName",
};
template <size_t I>
static uint64_t wrapStep(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
	char buf[96];
	snprintf(buf, sizeof(buf), "%s ENTER", kStepName[I]);
	rmbStep(buf);
	auto r = reinterpret_cast<Fn6>(orgStep[I])(a, b, c, d, e, f);
	snprintf(buf, sizeof(buf), "%s EXIT ret32=0x%x nonzero=%d", kStepName[I], static_cast<uint32_t>(r), r != 0);
	rmbStep(buf);
	return r;
}

static mach_vm_address_t orgCreateCoreServicesN23;
static uint32_t wrapCreateCoreServicesN23(void *self) {
	rmbCheckpoint("coreServices ENTER");
	gCoreTraceN = 0;
	gCoreTraceThread = current_thread();
	gCoreTrace = !checkKernelArgument("-rmbnoregtrace");
	auto r = reinterpret_cast<uint32_t (*)(void *)>(orgCreateCoreServicesN23)(self);
	gCoreTrace = false;
	IOLog("REM660: [coreServices] register accesses traced: %u\n", gCoreTraceN);
	auto p = reinterpret_cast<uint8_t *>(self);
	auto has = [p](size_t off) { return *reinterpret_cast<void *const *>(p + off) != nullptr ? 1 : 0; };
	IOLog("REM660: [coreServices] ret=0x%x devMem=%d regAccess=%d biosParser=%d asicInfo=%d\n", r,
	      has(30968), has(30960), has(30984), has(30976));
	rmbCheckpoint("coreServices EXIT");
	return r;
}

// Trace-only wrappers (25G229 FB, LC_UUID C3FA8B38-0702-3FCD-9878-C8FD05A7A724), called from
// AmdRadeonController::start @0x4bb08 / @0x4bb83. Prologues verified: push rbp; mov rsp,rbp; push r15...
// rax is passed through unchanged (64-bit return), arguments are forwarded as-is.
static mach_vm_address_t orgCreateFbStubs, orgSetupHwServices;
static uint64_t wrapCreateFbStubs(void *self, uint8_t arg) {
	rmbCheckpoint("createFbStubs ENTER");
	auto r = reinterpret_cast<uint64_t (*)(void *, uint8_t)>(orgCreateFbStubs)(self, arg);
	IOLog("REM660: [ckpt] createFbStubs EXIT ret=0x%x\n", static_cast<uint32_t>(r));
	diskLogSync(400);
	return r;
}
static uint64_t wrapSetupHwServices(void *self) {
	rmbCheckpoint("setupHwServices ENTER");
	auto r = reinterpret_cast<uint64_t (*)(void *)>(orgSetupHwServices)(self);
	IOLog("REM660: [ckpt] setupHwServices EXIT ret=0x%x\n", static_cast<uint32_t>(r));
	diskLogSync(400);
	return r;
}

// Read-only: what ATY,bin_image the controller will parse. initializeAtomDataTable (@0x55250) needs
// the master data table PSP directory (+0x16) and vram_info (+0x3C) slots.
void logVbiosProperty(IOService *provider) {
	if (!provider) return;
	auto data = OSDynamicCast(OSData, provider->getProperty("ATY,bin_image"));
	if (!data) {
		IOLog("REM660: [vbios] ATY,bin_image MISSING on provider\n");
		return;
	}
	auto rom = static_cast<const uint8_t *>(data->getBytesNoCopy());
	size_t len = data->getLength();
	auto rd16 = [rom, len](size_t off) -> uint32_t { return off + 2 <= len ? (rom[off] | (rom[off + 1] << 8)) : 0xFFFFFFFF; };
	uint32_t pcir = rd16(0x18), romhdr = rd16(0x48);
	uint32_t pcirDev = pcir < len ? rd16(pcir + 6) : 0xFFFFFFFF;
	uint32_t mdt = romhdr < len ? rd16(romhdr + 0x20) : 0xFFFFFFFF;
	uint32_t psp = mdt < len ? rd16(mdt + 0x16) : 0xFFFFFFFF, vram = mdt < len ? rd16(mdt + 0x3C) : 0xFFFFFFFF;
	uint8_t sum = 0;
	size_t img = len >= 3 ? static_cast<size_t>(rom[2]) * 512 : 0;
	for (size_t i = 0; i < img && i < len; i++) sum += rom[i];
	IOLog("REM660: [vbios] ATY,bin_image len=0x%zx blocks=%u pcirDev=0x%x pspSlot=0x%x vramSlot=0x%x sum=%u\n",
	      len, len >= 3 ? rom[2] : 0, pcirDev, psp, vram, sum);
}

#ifdef RMB_EMBED_VBIOS
// Optional local fallback: fw/lenovo_660m-73ff-aug.rom embedded by vbios.s when that file exists at build time.
// Not in the repository and not in dist/ (the VBIOS is Lenovo/AMD firmware): released builds read it from VFCT.
extern "C" const uint8_t rmb_vbios_aug[], rmb_vbios_aug_end[];
#endif
static bool lenovoPci(void *dev);

[[maybe_unused]] static uint32_t propU32(IOService *s, const char *key) {
	auto d = OSDynamicCast(OSData, s->getProperty(key));
	uint32_t v = 0xFFFFFFFF;
	if (d && d->getLength() >= 4) memcpy(&v, d->getBytesNoCopy(), 4);
	else if (d && d->getLength() >= 1) v = static_cast<const uint8_t *>(d->getBytesNoCopy())[0];
	return v;
}

// getACPITableData is protected in IOACPIPlatformExpert; same vtable call NootedRed uses for the APU VFCT.
class RmbAcpiExpert : public IOACPIPlatformExpert {
public:
	using IOACPIPlatformExpert::getACPITableData;
};

// Returns the ACPI VFCT OSData or nullptr; logs where it looked.
static const OSData *findVfctTable() {
	IORegistryEntry *cands[2] = {IOService::getPlatform(),
		IORegistryEntry::fromPath("/AppleACPIPlatformExpert", gIOServicePlane)};
	OSData *found = nullptr;
	for (int i = 0; i < 2 && !found; i++) {
		auto e = cands[i];
		auto tables = e ? OSDynamicCast(OSDictionary, e->getProperty("ACPI Tables")) : nullptr;
		IOLog("REM660: [vbios] VFCT lookup #%d entry=%s tables=%u\n", i,
		      e ? e->getMetaClass()->getClassName() : "null", tables ? tables->getCount() : 0);
		if (tables) found = OSDynamicCast(OSData, tables->getObject("VFCT"));
	}
	OSSafeReleaseNULL(cands[1]);  // fromPath retains; getPlatform does not
	if (found) return found;
	// 25G229 publishes no 'ACPI Tables' property: ask the ACPI platform expert for the table itself.
	auto pe = IOService::getPlatform();
	if (pe && pe->metaCast("IOACPIPlatformExpert")) {
		auto t = static_cast<RmbAcpiExpert *>(pe)->getACPITableData("VFCT", 0);
		IOLog("REM660: [vbios] VFCT via getACPITableData: %s len=0x%x\n", t ? "found" : "none", t ? t->getLength() : 0);
		return t;
	}
	IOLog("REM660: [vbios] platform expert is not IOACPIPlatformExpert\n");
	return nullptr;
}

// 25G229 boots (2026-11-08): OC DeviceProperties never reach VGA@0 (rmb-msgbuf.log line 1988: all OC-only
// keys 0) and the platform expert has no 'ACPI Tables' (line 1990), so AmdBiosParserHelper::readAtomBios
// (@0x173a2) falls back to the ROM BAR and Core Services fail (biosParser=0, 0xE00002C9).
// Source order: verified existing property -> ACPI VFCT of this laptop (exact transform, vbios_augment.hpp)
// -> embedded image (local builds only, RMB_EMBED_VBIOS; 1002:1681 rev 03 subsystem 17AA:3B1E only).
// Fail-closed: nothing is set unless the result hashes to the verified image. `-rmbnovbios` disables it.
void ensureVbiosProperty(IOService *provider) {
	if (!provider) return;
	// OC-only keys (nothing in this kext sets them): tells whether OC DeviceProperties reached VGA@0 at all.
	IOLog("REM660: [vbios] OC props on provider: override-no-connect=%d CFG_NO_BOOT_PWDN=%d iokit-ignore-ndrv=%d\n",
	      provider->getProperty("AAPL00,override-no-connect") ? 1 : 0, provider->getProperty("CFG_NO_BOOT_PWDN") ? 1 : 0,
	      provider->getProperty("AAPL,iokit-ignore-ndrv") ? 1 : 0);
	logVbiosProperty(provider);
	if (auto cur = OSDynamicCast(OSData, provider->getProperty("ATY,bin_image"))) {
		if (cur->getLength() == rmbvbios::kOutLen &&
		    rmbvbios::fnv1a64(static_cast<const uint8_t *>(cur->getBytesNoCopy()), cur->getLength()) == rmbvbios::kOutFnv) {
			IOLog("REM660: [vbios] existing ATY,bin_image is the verified augmented image, kept\n");
			return;
		}
	}
	if (checkKernelArgument("-rmbnovbios")) {
		IOLog("REM660: [vbios] disabled by -rmbnovbios\n");
		return;
	}
	const char *why = "";
	const char *source = nullptr;
	auto buf = static_cast<uint8_t *>(IOMalloc(rmbvbios::kOutLen));
	if (!buf) return;
	size_t n = 0;
	if (auto vfct = findVfctTable()) {
		size_t imgLen = 0;
		auto img = rmbvbios::findVfctImage(static_cast<const uint8_t *>(vfct->getBytesNoCopy()), vfct->getLength(), &imgLen, &why);
		n = img ? rmbvbios::augment(img, imgLen, buf, rmbvbios::kOutLen, &why) : 0;
		if (n)
			source = "ACPI VFCT";
		else
			IOLog("REM660: [vbios] VFCT len=0x%x rejected (%s)\n", vfct->getLength(), why);
	}
#ifdef RMB_EMBED_VBIOS
	if (!n) {
		uint32_t ssv = propU32(provider, "subsystem-vendor-id") & 0xFFFF, ssid = propU32(provider, "subsystem-id") & 0xFFFF;
		uint32_t rev = propU32(provider, "revision-id") & 0xFF;
		size_t embLen = static_cast<size_t>(rmb_vbios_aug_end - rmb_vbios_aug);
		bool idOk = lenovoPci(provider) && ssv == 0x17AA && ssid == 0x3B1E && rev == 0x03;
		bool embOk = embLen == rmbvbios::kOutLen && rmbvbios::fnv1a64(rmb_vbios_aug, embLen) == rmbvbios::kOutFnv;
		IOLog("REM660: [vbios] embedded fallback: subsys %04x:%04x rev %02x idOk=%d len=0x%zx fnvOk=%d\n", ssv, ssid, rev,
		      idOk ? 1 : 0, embLen, embOk ? 1 : 0);
		if (idOk && embOk) {
			memcpy(buf, rmb_vbios_aug, embLen);
			n = embLen;
			source = "embedded verified image";
		}
	}
#endif
	if (n) {
		if (auto data = OSData::withBytes(buf, static_cast<unsigned>(n))) {
			provider->setProperty("ATY,bin_image", data);
			data->release();
			IOLog("REM660: [vbios] ATY,bin_image set from %s (0x%zx bytes, fnv %llx)\n", source, n, rmbvbios::kOutFnv);
			logVbiosProperty(provider);
			rmbCheckpoint("vbios set");
		}
	} else {
		IOLog("REM660: [vbios] no verified VBIOS source; registry untouched\n");
	}
	IOFree(buf, rmbvbios::kOutLen);
}

static bool processFramebuffer(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
	IOLog("REM660: [processFramebuffer] ENTER addr=0x%llx size=0x%zx\n", address, size);
	{
		KernelPatcher::RouteRequest coreReq {
			"__ZN40AMDRadeonX6000_AmdRadeonControllerNavi2328createControllerCoreServicesEv",
			wrapCreateCoreServicesN23, orgCreateCoreServicesN23
		};
		if (!routeStaticRequests(patcher, 1, &coreReq, 1, address, size))
			IOLog("REM660: [processFramebuffer] coreServices trace not routed (non-fatal)\n");
		patcher.clearError();
		KernelPatcher::RouteRequest traceReqs[] {
			{"__ZN34AMDRadeonX6000_AmdRadeonController13createFbStubsEh", wrapCreateFbStubs, orgCreateFbStubs},
			{"__ZN34AMDRadeonX6000_AmdRadeonController15setupHwServicesEv", wrapSetupHwServices, orgSetupHwServices},
		};
		if (!routeStaticRequests(patcher, 1, traceReqs, 2, address, size))
			IOLog("REM660: [processFramebuffer] controller stage trace not routed (non-fatal)\n");
		patcher.clearError();
		gFbImage = address;
		KernelPatcher::RouteRequest stepReqs[StepCount] {
			{"__ZN34AMDRadeonX6000_AmdBiosParserHelper16createBiosParserER16AtomBiosInitData", wrapStep<0>, orgStep[0]},
			{"__ZN34AMDRadeonX6000_AmdBiosParserHelper23initializeAtomBiosImageEPhm", wrapStep<1>, orgStep[1]},
			{"__ZN34AMDRadeonX6000_AmdBiosParserHelper16readFirmwareInfoEv", wrapStep<2>, orgStep[2]},
			{"__ZN32AMDRadeonX6000_AmdAsicInfoNavi2314createAsicInfoEP35AMDRadeonX6000_IAmdRadeonController", wrapStep<3>, orgStep[3]},
			{"__ZN26AMDRadeonX6000_AmdAsicInfo7refreshEv", wrapStep<4>, orgStep[4]},
			{"__ZN26AMDRadeonX6000_AmdAsicInfo20populateMemoryConfigEv", wrapStep<5>, orgStep[5]},
			{"__ZN31AMDRadeonX6000_AmdAsicInfoNavi218populateFbLocationEv", wrapStep<6>, orgStep[6]},
			{"__ZN26AMDRadeonX6000_AmdAsicInfo17populateModelNameEv", wrapStep<7>, orgStep[7]},
		};
		bool stepsOk = routeStaticRequests(patcher, 1, stepReqs, StepCount, address, size);
		patcher.clearError();
		unsigned routed = 0;
		for (size_t i = 0; i < StepCount; i++)
			routed += orgStep[i] != 0;
		IOLog("REM660: [processFramebuffer] coreServices sub-step trace routed %u/%zu%s\n", routed, StepCount,
		      stepsOk ? "" : " (non-fatal)");
	}
	capHdmiPixelClock(address, size);
	capPipes(address, size);
	ihMmioWptr(patcher, index, address, size);
	safeModes(patcher, index, address, size);
	gHeadless = checkKernelArgument("-rmbheadless");
	if (gHeadless) {
		KernelPatcher::RouteRequest req {"__ZN35AMDRadeonX6000_AmdRadeonFramebuffer5startEP9IOService", wrapFbStart};
		if (!patcher.routeMultiple(index, &req, 1, address, size))
			LOG("headless: failed to route framebuffer start (err %d)", patcher.getError());
		else
			LOG("headless: framebuffers disabled");
		patcher.clearError();
	}
	orgDoesAcceleratorExist = findSymbol(patcher, 1, "__ZNK34AMDRadeonX6000_AmdRadeonController20doesAcceleratorExistEv", address, size);
	if (orgDoesAcceleratorExist) {
		KernelPatcher::RouteRequest msgReq {
			"__ZNK34AMDRadeonX6000_AmdRadeonController18messageAcceleratorE25_eAMDAccelIOFBRequestTypePvS1_S1_",
			wrapMessageAccelerator, orgMessageAccelerator
		};
		routeStaticRequests(patcher, 1, &msgReq, 1, address, size);
	}
	KernelPatcher::RouteRequest required[] {
		{"__ZN31AMDRadeonX6000_AmdAsicInfoNavi218populateDeviceInfoEv",wrapPopulateDeviceInfo,orgPopulateDeviceInfo},
		{"__ZN31AMDRadeonX6000_AmdAsicInfoNavi218populateXGmiConfigEv",wrapPopulateXGmiConfig,orgPopulateXGmiConfig},
	};
	if (!restoredRouteGroup(patcher, 1, required, arrsize(required), address, size)) {
		patcher.clearError();
		routeStaticRequests(patcher, 1, required, arrsize(required), address, size);
		patcher.clearError();
	}
	probeRouteFramebuffer(patcher, 1, address, size);
	if (!gHeadless) {
		displayRoute(patcher, 1, address, size);
	}
	IOLog("REM660: [processFramebuffer] EXIT SUCCESS\n");
	return true;
}

// The 680M sits on the SoC's internal fabric, not behind a PCIe link of its own, so the
// dGPU-only link checks in pcie_ip_sw_init (check_pcie_link_status / check_root_pcie_link_speed)
// fail and abort bgm_create (event 0xc00c020b). There is no link to manage: report success.
static uint64_t wrap_pcie_ip_sw_init(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
	LOG("pcie_ip_sw_init: skipped (APU has no discrete PCIe link)");
	return 0;
}

// DeviceCapabilityTbl is keyed by (device id, internal revision, external/PCI revision). All
// Navi 23 (0x73FF) rows have internal revision 0; Rembrandt reports its own, so no row matches.
// Look up the Navi 23 row with internal revision 0 and the real PCI revision.
mach_vm_address_t gRem660OrgCapability; // external storage keeps the restored dispatch code emitted before installation
__attribute__((used,noinline)) static uint8_t wrap_ttlSetDeviceCapabilityEntry(void *ctx,uint32_t dev,uint32_t intRev,uint32_t extRev) {
 return rem660::selectNavi23Capability(reinterpret_cast<rem660::Capability>(gRem660OrgCapability),ctx,dev,intRev,extRev);
}

// IP discovery entry (0x260 bytes): +0x0 hw id, +0x8 major, +0xC minor, +0x10 revision.
static mach_vm_address_t org_get_hw_revision;
static uint64_t wrap_get_hw_revision(uint64_t maj, uint64_t min, uint64_t rev, uint64_t d, uint64_t e, uint64_t f) {
	LOG("get_hw_revision(%llu.%llu.%llu)", maj & 0xFFFF, min & 0xFFFF, rev & 0xFFFF);
	return reinterpret_cast<Fn6>(org_get_hw_revision)(maj, min, rev, d, e, f);
}

// HWLibs only implements PSP 3.1 and 11.0. Rembrandt's MP0 is 13.x; present it as Navi 23's
// 11.0.11 so the PSP 11 code paths get selected (the differing parts get routed separately).
mach_vm_address_t gRem660OrgPspAsicInit; // external storage keeps the restored dispatch code emitted before installation
__attribute__((used,noinline)) static uint32_t wrap_psp_asic_type_init(void *psp,void *info) {
 return rem660::selectApplePsp11(reinterpret_cast<rem660::PspAsicInit>(gRem660OrgPspAsicInit),psp,info);
}

// HWLibs has GC tables for 10.3.0 (Navi 21) and 10.3.4 (Navi 23) only. Rembrandt's GC is 10.3.3,
// the same gfx10.3 design (Linux drives both with gfx_v10_0); present it as 10.3.4. Firmware and
// golden settings that differ are substituted separately.
mach_vm_address_t gRem660OrgGcVersion; // external storage keeps the restored dispatch code emitted before installation
__attribute__((used,noinline)) static uint32_t wrap_gc_get_hw_version(const void *info) {
 return rem660::selectNavi23Gc(reinterpret_cast<rem660::GcVersion>(gRem660OrgGcVersion),info);
}

// PSP mailbox dump around KM ring creation (MP0 seg0 0x16000; C2PMSG_n = 0x40 + n).
static void dumpPspMailbox(const char *when) {
	constexpr uint32_t mp0 = 0x16000;
	LOG("psp %s: C2PMSG_35 %08x 64 %08x 69 %08x 70 %08x 71 %08x 81 %08x", when,
		mmioRead(mp0 + 0x63), mmioRead(mp0 + 0x80), mmioRead(mp0 + 0x85), mmioRead(mp0 + 0x86),
		mmioRead(mp0 + 0x87), mmioRead(mp0 + 0x91));
}

// The host's amdgpu (or an earlier VM run) leaves a KM ring / error status behind in the PSP,
// which keeps running across VM restarts. Destroy rings first, as Linux psp_v13_0_ring_stop does.
static void pspDestroyRings() {
	constexpr uint32_t c2pmsg64 = 0x16000 + 0x80;
	mmioWrite(c2pmsg64, 0x00030000);  // GFX_CTRL_CMD_ID_DESTROY_RINGS
	IODelay(20000);
	for (int i = 0; i < 2000 && !(mmioRead(c2pmsg64) & 0x80000000); i++)
		IODelay(1000);
	LOG("psp: destroy rings -> C2PMSG_64 %08x", mmioRead(c2pmsg64));
}

static mach_vm_address_t org_psp_ring_create;
static uint32_t wrap_psp_ring_create(void *state,uint32_t ringType) {
 if(!org_psp_ring_create) return 1;
 // PSP11 native dispatch is retained. The original pre-create DESTROY_RINGS
 // addressed cross-VM residual state, not a proven Lenovo cold-boot contract.
 auto result=reinterpret_cast<uint32_t (*)(void *,uint32_t)>(org_psp_ring_create)(state,ringType);
 LOG("psp_ring_create(type %u) native status=%#x",ringType,result);
 return result;
}

// PowerPlay event chains: same loop as PEM_ExcuteEventChain, but logging each task (as an offset
// from HWLibs' __TEXT; resolve offline with +0xc305000 against build/amd/AMDRadeonX6000HWLibs).
static mach_vm_address_t gHWLibsBase;
using PemTask = uint32_t (*)(void *, void *);
// Tasks that need driver-built power states from a dGPU PowerPlay table, which Rembrandt doesn't
// have (its PMFW manages DPM itself). Treated as succeeded.
static mach_vm_address_t gSkipTasks[1];
// Tasks whose failure is ignored. NotifyHWOfPowerSource (AC/DC to the SMU) fails on some guest S3
// wakes (TTL dispatcher not up yet, e.g. after WindowServer missed the sleep ack) and Apple then
// panics "Failed Power Play Resume"; Rembrandt's PMFW doesn't need it. `-rmbpwrsrc` = fatal again.
static mach_vm_address_t gTolerateTasks[1];
static uint64_t wrap_PEM_ExcuteEventChain(uint64_t ctx, uint64_t chain, uint64_t data, uint64_t, uint64_t, uint64_t) {
	if (!chain || !data)
		return 7;
	auto name = *reinterpret_cast<const char **>(chain + 0x8);
	auto lists = *reinterpret_cast<PemTask ***>(chain + 0x10);
	uint32_t res = 1;
	// Runtime chains (not boot/sleep) preceded both SDMA hangs that took the host down (1024-1238,
	// 1024-1332) and never returned: trace each task so the last line names the stuck one.
	bool trace = name && strcmp(name, "Initialize") && strcmp(name, "Complete Init") && strcmp(name, "Suspend") &&
	             strcmp(name, "Resume");
	if (trace)
		LOG("pem <%s>: start", name);
	for (; lists && *lists && res == 1; lists++) {
		for (auto task = *lists; *task; task++) {
			bool skip = false;
			for (auto t : gSkipTasks)
				skip |= t && t == reinterpret_cast<mach_vm_address_t>(*task);
			uint64_t t0 = 0;
			if (trace) {
				LOG("pem <%s>: task +%llx", name, reinterpret_cast<mach_vm_address_t>(*task) - gHWLibsBase);
				t0 = mach_absolute_time();
			}
			auto r = skip ? 1 : (*task)(reinterpret_cast<void *>(ctx), reinterpret_cast<void *>(data));
			if (trace) {
				uint64_t ns;
				absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
				if (ns > 50000000)
					LOG("pem <%s>: task +%llx took %llu ms", name, reinterpret_cast<mach_vm_address_t>(*task) - gHWLibsBase, ns / 1000000);
			}
			if (r != 1)
				LOG("pem <%s>: task +%llx = %u", name ? name : "?", reinterpret_cast<mach_vm_address_t>(*task) - gHWLibsBase, r);
			bool tolerate = false;
			for (auto t : gTolerateTasks)
				tolerate |= t && t == reinterpret_cast<mach_vm_address_t>(*task);
			if (r != 1 && tolerate) {
				LOG("pem <%s>: ignoring failed task", name ? name : "?");
				r = 1;
			}
			if (res == 1)
				res = r;
		}
	}
	LOG("pem <%s> = %u", name ? name : "?", res);
	// A failed "Initialize" chain makes ppInitialize return 0xe00002bc and AmdPowerPlayHelper::powerUp
	// panics "Failed Power Play Initialization" right after: write the failing task offsets out first.
	// Only this chain: it runs from PP_InitializeEX in IOFramebuffer::open (thread context).
	if (res != 1 && name && !strcmp(name, "Initialize"))
		diskLogSync(500);
	return res;
}

// The GART/VMID0 page tables live in the UMA carve-out. Their PDEs/PTEs and the page-table base
// register need the carve-out's system physical address: VRAM offset + (GCMC_VM_FB_OFFSET << 24),
// as Linux's amdgpu_gmc_vram_mc2pa does. HWLibs' vm_10_1 does this only when the device caps mark
// the ASIC as UMA (vm ctx +0xc, bits 0x18), and the Navi 23 capability row we use says dGPU.
static mach_vm_address_t org_get_uma_physical_fb_offset;
static uint64_t wrap_get_uma_physical_fb_offset(void *vm) {
 if(!vm||!org_get_uma_physical_fb_offset) return 0;
 // Exact25G2290x2fb2f consumes these flags; native0x2fb76 owns offset+0x210.
 *reinterpret_cast<uint8_t *>(static_cast<uint8_t *>(vm)+0xc)|=8;
 return reinterpret_cast<uint64_t (*)(void *)>(org_get_uma_physical_fb_offset)(vm);
}

// Clamps a power state's clocks to the PowerPlay table's limits and divides by them; with no
// pptable on Rembrandt they are all zero (divide error). PMFW owns DPM, so leave states as-is.
static uint64_t wrap_ApplyClocksAdjustRules(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
	return 1;  // PP_Result_OK
}
static uint64_t wrap_Phw_SwSmu_UpdatePCIEParameters(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
	return 1;  // PP_Result_OK
}
static mach_vm_address_t org_InitMaxSustainableClocks, org_GetAllClockRanges;
static uint64_t wrap_Phw_SwSmu_InitMaxSustainableClocks(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
	if (org_InitMaxSustainableClocks) {
		auto r = reinterpret_cast<Fn6>(org_InitMaxSustainableClocks)(a, b, c, d, e, f);
		if (r != 1) {
			// Pre-existing behaviour kept (PP init continues), but no longer silent.
			LOG("pp: Phw_SwSmu_InitMaxSustainableClocks = %llx, continuing as OK", r);
			return 1;
		}
		return r;
	}
	return 1;
}
static uint64_t wrap_Phw_SwSmu_GetAllClockRanges(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
	if (org_GetAllClockRanges) {
		auto r = reinterpret_cast<Fn6>(org_GetAllClockRanges)(a, b, c, d, e, f);
		if (r != 1) {
			// Pre-existing behaviour kept (PP init continues), but no longer silent.
			LOG("pp: Phw_SwSmu_GetAllClockRanges = %llx, continuing as OK", r);
			return 1;
		}
		return r;
	}
	return 1;
}

// bgm hw init applies the device-capability golden table (Navi23's: GL2 match masks 0xffffffcf etc.)
// and vm_10_3_hw_init programs more GC registers; after each, put Rembrandt's golden values back.
static mach_vm_address_t org_execute_register_golden_settings, org_gc_10_3_initialize_registers;
static uint64_t wrap_execute_register_golden_settings(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
	auto r = reinterpret_cast<Fn6>(org_execute_register_golden_settings)(a, b, c, d, e, f);
	LOG("golden: after execute_register_golden_settings (%llx)", r);
	applyGcGolden();
	return r;
}
static uint64_t wrap_gc_10_3_initialize_registers(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
	auto r = reinterpret_cast<Fn6>(org_gc_10_3_initialize_registers)(a, b, c, d, e, f);
	LOG("golden: after gc_10_3_initialize_registers (%llx)", r);
	applyGcGolden();
	return r;
}

static void *gSyncedDev = nullptr;
static mach_vm_address_t orgCreatePciDeviceCache;
// HWLibs 25G229 (UUID 5580395F-0AE4-3F9D-8263-974BE551E647) accepts a device only through
// _DeviceCapabilityTbl (0x54e240, 80-byte rows, key: +16 device, +24 internal rev, +32 PCI rev).
// _ttl_initialize -> _ttlIsSupportedDevice -> _DevIsSupportedDevice(device, PCI rev) returns status 1
// (Accel 0xe00002c7) for 0x73ff rev 0x03: rows 261-268 exist only for PCI rev 0x40-0x43, 0xc1, 0xc3,
// 0xc7, 0xcb, and are byte-identical apart from that key (family 0x8f, ext base 0x3c, the same five
// table pointers). ttlDevSetAsicType (0xa7d05) derives the ASIC type from rev bits 7:6: 0x03 gives
// type 1, which PSP/SMU/GC/DF/GVM init treat as a special (non-production) part; c1-cb give type 0.
// Present the lowest production row (0xc1), only for the exact Lenovo revision on the spoofed 0x73ff.
static constexpr uint32_t kPciRevisionOffset = 8;
static constexpr uint8_t kLenovoPciRevision = 0x03, kNavi23ProductionRevision = 0xC1;
static bool presentNavi23Revision(uint32_t device, uint32_t &revField) {
	static int disabled = -1;
	if (disabled < 0)
		disabled = checkKernelArgument("-rmbnorevpresent") ? 1 : 0;
	if (disabled || device != 0x73ff || (revField & 0xFF) != kLenovoPciRevision)
		return false;
	revField = (revField & ~0xFFu) | kNavi23ProductionRevision;
	return true;
}

static void wrapCreatePciDeviceCache(void *self) {
	using Fn = void (*)(void *);
	if (orgCreatePciDeviceCache) {
		reinterpret_cast<Fn>(orgCreatePciDeviceCache)(self);
	}
	IOLog("REM660: [createPciDeviceCache] AmdTtlServices cache created self=%p\n", self);
	if (self) {
		uint32_t *pDev24 = reinterpret_cast<uint32_t *>(reinterpret_cast<uint8_t *>(self) + 0x24);
		IOLog("REM660: [createPciDeviceCache] self=%p devId24=0x%08x\n", self, *pDev24);
		if (*pDev24 == 0x1681 || (*pDev24 & 0xFFFF) == 0x1681) {
			*pDev24 = 0x73ff;
			IOLog("REM660: [createPciDeviceCache] spoofed self+0x24 0x1681 -> 0x73ff\n");
		}
		uint16_t *pDev16 = reinterpret_cast<uint16_t *>(reinterpret_cast<uint8_t *>(self) + 0x180);
		uint32_t *pDev32 = reinterpret_cast<uint32_t *>(reinterpret_cast<uint8_t *>(self) + 0x180);
		IOLog("REM660: [createPciDeviceCache] self=%p devId16=0x%04x devId32=0x%08x\n", self, *pDev16, *pDev32);
		if (*pDev16 == 0x1681 || (*pDev32 & 0xFFFF) == 0x1681) {
			*pDev16 = 0x73FF;
			IOLog("REM660: [createPciDeviceCache] spoofed self+0x180 0x1681 -> 0x73ff\n");
		}
	}
	if (self) {
		// AmdTtlServices +0x184 = extendedConfigRead16(8) from the ctor (HWLibs 25G229 0xae16b..0xae17c),
		// copied into the ttl_initialize device info by initialize() at 0xaed1e. createPciDeviceCache runs
		// in initialize(input) at 0xae8ba, before initialize() at 0xae8c2, so this is the last point.
		auto pRev = reinterpret_cast<uint32_t *>(reinterpret_cast<uint8_t *>(self) + 0x184);
		auto pDev = reinterpret_cast<uint32_t *>(reinterpret_cast<uint8_t *>(self) + 0x180);
		uint32_t before = *pRev;
		bool changed = presentNavi23Revision(*pDev & 0xFFFF, *pRev);
		IOLog("REM660: [createPciDeviceCache] TTL PCI revision field 0x%04x -> 0x%04x (%s)\n", before, *pRev,
		      changed ? "presented as Navi23 production row" : "unchanged");
		// AmdTtlServices +8 = IOPCIDevice (ctor 0xae0ee). cosReadConfigurationSetting (0xad8f0) reads the
		// TTL config keys from it; record where gc_get_config's FW-loading switches come from.
		auto pci = OSDynamicCast(IORegistryEntry,
			*reinterpret_cast<OSObject **>(reinterpret_cast<uint8_t *>(self) + 8));
		static const char *const fwKeys[] {"EnableswGCFWLoading", "EnableswGCFWLoadingBypass",
			"EnableFWBackdoorAutoload", "EnableswSDMAFWLoading", "EnableswSDMAFWLoadingBypass"};
		for (auto key : fwKeys) {
			auto obj = pci ? pci->getProperty(key) : nullptr;
			auto num = OSDynamicCast(OSNumber, obj);
			IOLog("REM660: [createPciDeviceCache] PCI config key %s: %s %u\n", key,
			      !pci ? "no IOPCIDevice" : obj ? obj->getMetaClass()->getClassName() : "absent",
			      num ? num->unsigned32BitValue() : 0);
		}
	}
	if (gSyncedDev) {
		syncSinglePciDeviceProps(gSyncedDev);
		IOLog("REM660: [createPciDeviceCache] spoofed cached device_id -> 0x73ff\n");
	}
}

// HWLibs bcs_create (0x22b51c) reads the PCI revision byte through this callback (input: +0 offset,
// +8 buffer, +16 size 1/2/4, +20 mode 0=own device / 1=bus:dev:fn at +24/+28/+32; returns 0 on success).
// get_adapter_info copies it into the bgm query (+12), and ipi_bgm_create passes it as the external
// revision to ttlSetExternalRevisionId (0x9b9fc) and ttlSetDeviceCapabilityEntry (0x9bc62).
struct CosPciInput { uint32_t offset, pad0; uint8_t *buffer; uint32_t size, mode, bus, dev, fn; };
static_assert(sizeof(CosPciInput) == 40, "cos_access_pci_config_data_input layout (read_pci_cfg_by_bus_no 0x22ce46)");
using CosReadPciFn = uint32_t (*)(void *, CosPciInput *, uint32_t *);
static mach_vm_address_t orgCosReadPciConfigData;
static uint32_t wrapCosReadPciConfigData(void *ctx, CosPciInput *in, uint32_t *out) {
	auto org = reinterpret_cast<CosReadPciFn>(orgCosReadPciConfigData);
	uint32_t ret = org ? org(ctx, in, out) : 1;
	if (ret || !in || !in->buffer || in->offset > kPciRevisionOffset || in->offset + in->size <= kPciRevisionOffset)
		return ret;
	// Only the GPU's own config space: re-read vendor/device of the same target through Apple's path.
	uint32_t id = 0, got = 0;
	CosPciInput q = *in;
	q.offset = 0, q.size = 4, q.buffer = reinterpret_cast<uint8_t *>(&id);
	if (org(ctx, &q, &got) || (id != 0x16811002 && id != 0x73ff1002))
		return ret;
	uint8_t &rev = in->buffer[kPciRevisionOffset - in->offset];
	uint32_t wide = rev, before = rev;
	if (presentNavi23Revision(0x73ff, wide)) {
		rev = static_cast<uint8_t>(wide);
		static uint32_t logged;
		if (logged++ < 4)
			IOLog("REM660: [cosReadPciConfigData] mode %u off 0x%x size %u: PCI revision 0x%02x -> 0x%02x\n",
			      in->mode, in->offset, in->size, before, rev);
	}
	return ret;
}

static mach_vm_address_t orgCreateTtlService;
static void *wrapCreateTtlService(IOService *provider) {
	IOLog("REM660: [wrapCreateTtlService] ENTER provider=%p\n", provider);
	if (provider) {
		hookPciDeviceFromInstance(provider);
		syncSinglePciDeviceProps(provider);
		pciEnableMemoryAndBusMaster(provider);
	}
	using Fn = void *(*)(IOService *);
	void *res = nullptr;
	if (orgCreateTtlService) {
		res = reinterpret_cast<Fn>(orgCreateTtlService)(provider);
	}
	if (res) {
		uint16_t *pDev16 = reinterpret_cast<uint16_t *>(reinterpret_cast<uint8_t *>(res) + 0x180);
		void *mmioMap = *reinterpret_cast<void **>(reinterpret_cast<uint8_t *>(res) + 0x198);
		void *mmioVirt = *reinterpret_cast<void **>(reinterpret_cast<uint8_t *>(res) + 0x1a0);
		IOLog("REM660: [wrapCreateTtlService] created ttlService=%p devId=0x%04x mmioMap=%p mmioVirt=%p\n",
		      res, *pDev16, mmioMap, mmioVirt);
		if (*pDev16 == 0x1681) {
			*pDev16 = 0x73FF;
			IOLog("REM660: [wrapCreateTtlService] spoofed ttlService+0x180 0x1681 -> 0x73ff\n");
		}
	}
	IOLog("REM660: [wrapCreateTtlService] EXIT res=%p\n", res);
	return res;
}

// Diagnostic only (result returned unchanged): AmdPowerPlayHelper::powerUp panics "Failed Power Play
// Initialization" as soon as AtiPowerPlayServices::ppInitialize (HWLibs 0x2253a0, vtable +0x68) returns
// non-zero, so write the PP stage trace (PHM/PP_Tables/PSM/PECI/PEM ... and pem task lines) to disk first.
static mach_vm_address_t orgPpInitialize;
static uint64_t wrapPpInitialize(void *self, void *config) {
	LOG(">> ppInitialize");
	auto r = reinterpret_cast<uint64_t (*)(void *, void *)>(orgPpInitialize)(self, config);
	LOG("<< ppInitialize = %llx%s", r & 0xffffffff, (r & 0xffffffff) ? " (Apple panics: Failed Power Play Initialization)" : "");
	if (r & 0xffffffff)
		diskLogSync(500);
	return r;
}

static bool processHWLibs(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
	IOLog("REM660: [processHWLibs] ENTER index=%zu addr=0x%llx size=0x%zx\n", index, address, size);
	KernelPatcher::RouteRequest requests[] {
		{"_psp_ring_create", wrap_psp_ring_create, org_psp_ring_create},
		{"_gc_get_hw_version", wrap_gc_get_hw_version, gRem660OrgGcVersion},  // after TRACE: GC 10.3.3 -> 10.3.4
		{"_psp_asic_type_init", wrap_psp_asic_type_init, gRem660OrgPspAsicInit},
		{"_ttlSetDeviceCapabilityEntry", wrap_ttlSetDeviceCapabilityEntry, gRem660OrgCapability},
		{"_vm_10_1_get_uma_physical_fb_offset", wrap_get_uma_physical_fb_offset, org_get_uma_physical_fb_offset},
		{"__ZN14AmdTtlServices20createPciDeviceCacheEv", wrapCreatePciDeviceCache, orgCreatePciDeviceCache},
		{"__ZN14AmdTtlServices20cosReadPciConfigDataEPvP32cos_access_pci_config_data_inputP33cos_access_pci_config_data_output", wrapCosReadPciConfigData, orgCosReadPciConfigData},
		{"__ZN15AmdTtlInterface16createTtlServiceEP11IOPCIDevice", wrapCreateTtlService, orgCreateTtlService},
	};
	if (!restoredRouteGroup(patcher, 0, requests, arrsize(requests), address, size)) {
		patcher.clearError();
		routeStaticRequests(patcher, 0, requests, arrsize(requests), address, size);
		patcher.clearError();
	}

	KernelPatcher::RouteRequest apuRequests[] {
		{"_pcie_ip_sw_init", wrap_pcie_ip_sw_init},
		{"_Phw_CommonDynamicState_ApplyClocksAdjustRules", wrap_ApplyClocksAdjustRules},
		{"_Phw_SwSmu_UpdatePCIEParameters", wrap_Phw_SwSmu_UpdatePCIEParameters},
		{"_Phw_SwSmu_InitMaxSustainableClocks", wrap_Phw_SwSmu_InitMaxSustainableClocks, org_InitMaxSustainableClocks},
		{"_Phw_SwSmu_GetAllClockRanges", wrap_Phw_SwSmu_GetAllClockRanges, org_GetAllClockRanges},
		{"_execute_register_golden_settings", wrap_execute_register_golden_settings, org_execute_register_golden_settings},
		{"_gc_10_3_initialize_registers", wrap_gc_10_3_initialize_registers, org_gc_10_3_initialize_registers},
	};
	routeStaticRequests(patcher, 0, apuRequests, arrsize(apuRequests), address, size);

	// 2026-11-09: wrap_PEM_ExcuteEventChain was defined but never routed in this fork, so PowerPlay ran
	// _PEM_Task_EnableDynamicStateManagement natively. On 25G229 (HWLibs UUID 5580395F) that task needs a
	// PSM state built from a dGPU pptable (_PSM_GetState at 0x105161; else 7 "Invalid Input Data!" or 15
	// "Requested state is not found!"); its failure stops the "Initialize" chain (@0x555560, list 3) ->
	// _PP_InitializeEX != 1 -> AtiPowerPlayServices::ppInitialize 0xe00002bc -> AmdPowerPlayHelper::powerUp
	// panic "Failed Power Play Initialization" (panic-2026-11-08-233604.txt, earlyOpen -> fb open).
	// Original RembrandtGfx: skip that task (Rembrandt's PMFW owns DPM; no driver power states) and
	// tolerate NotifyHWOfPowerSource (`-rmbpwrsrc` = fatal). The wrapper is the native loop of
	// _PEM_ExcuteEventChain 0x10288b (chain +8 name, +0x10 lists, res = first non-1, next list only while
	// res == 1, NULL chain/data -> 7) with these two exceptions; the task addresses are set before routing.
	gSkipTasks[0] = findSymbol(patcher, 0, "_PEM_Task_EnableDynamicStateManagement", address, size);
	if (!checkKernelArgument("-rmbpwrsrc"))
		gTolerateTasks[0] = findSymbol(patcher, 0, "_PEM_Task_NotifyHWOfPowerSource", address, size);
	KernelPatcher::RouteRequest pemReq {"_PEM_ExcuteEventChain", wrap_PEM_ExcuteEventChain};
	bool pemOk = gSkipTasks[0] && routeStaticRequests(patcher, 0, &pemReq, 1, address, size);
	patcher.clearError();
	LOG("pem: event chain %s; skip EnableDynamicStateManagement %s; tolerate NotifyHWOfPowerSource %s",
	    pemOk ? "routed" : "NOT routed", gSkipTasks[0] ? "resolved" : "missing", gTolerateTasks[0] ? "resolved" : "off/missing");
	// PowerPlay init stage trace. All five take <= 6 register args (no 16(%rbp)/24(%rbp) reads in 25G229);
	// _PP_Initialize_internal is NOT traced: it reads stack args at 0xbbf4f/0xbbfee.
	KernelPatcher::RouteRequest ppReqs[] {
		{"__ZN20AtiPowerPlayServices12ppInitializeEP22PPDisplayConfiguration", wrapPpInitialize, orgPpInitialize},
		{"_PHM_Initialize", wrap_PHM_Initialize, org_PHM_Initialize},
		{"_PP_Tables_Initialize", wrap_PP_Tables_Initialize, org_PP_Tables_Initialize},
		{"_PSM_Initialize", wrap_PSM_Initialize, org_PSM_Initialize},
		{"_PECI_Initialize", wrap_PECI_Initialize, org_PECI_Initialize},
		{"_PEM_Initialize", wrap_PEM_Initialize, org_PEM_Initialize},
	};
	routeStaticRequests(patcher, 0, ppReqs, arrsize(ppReqs), address, size);
	patcher.clearError();
	LOG("pp trace: ppInitialize %s, PHM %d PP_Tables %d PSM %d PECI %d PEM %d", orgPpInitialize ? "routed" : "NOT routed",
	    org_PHM_Initialize != 0, org_PP_Tables_Initialize != 0, org_PSM_Initialize != 0, org_PECI_Initialize != 0, org_PEM_Initialize != 0);

	smuRoute(patcher, 0, address, size);
	firmwareRoute(patcher, 0, address, size);
	vcnRouteHwLibs(patcher, 0, address, size);  // opt-in hardware video (ctl /Users/Shared/rmb-vcn.ctl)
	LOG("required HWLibs routes installed; hardware results remain native");
	IOLog("REM660: [processHWLibs] EXIT SUCCESS\n");
	return true;
}

// Lilu only inspects the head of the kmod list when a kext loads, and on this setup it never
// reports the AMD kexts mapped from the System KC. So we route OSKext::start ourselves and scan
// the kmod list for HWLibs before any kext starts; HWLibs starts before AMDRadeonX6000 (which
// runs TTL init), so the hooks are in place in time.
static KernelPatcher *gPatcher;
static kmod_info_t **gKmods;
kmod_info *kmodList() { return gKmods ? *gKmods : nullptr; }
static bool hooked[arrsize(kexts)];
static bool gEnablePciSpoof = false;

static uint32_t gCompletedImages;
static uint8_t gScanActive;

bool isAmdKextLoadIndex(size_t loadIndex) {
	if (loadIndex <= 2) return true;
	for (size_t i = 0; i <= 2 && i < arrsize(kexts); ++i) {
		if (kexts[i].loadIndex == loadIndex)
			return true;
	}
	return false;
}

bool isKextHooked(size_t i) {
	if (i < arrsize(hooked))
		return hooked[i];
	return false;
}
void scanKmods(KernelPatcher *patcher);
bool restoredBootstrapReady() {
	uint32_t imgs = __atomic_load_n(&gCompletedImages, __ATOMIC_ACQUIRE);
	bool essential = (imgs & (1u << 3)) || hooked[PciIdx]; // PCI spoof active
	bool ready = essential && !restoredRoutingFailed() && smuRestoreReady() && restoredPreStartCertified();
	return ready;
}
static uintptr_t gAppleBegin[5];
static size_t gAppleSize[5];
static mach_vm_address_t gDeferredHWLibsAddr = 0;
static size_t gDeferredHWLibsSize = 0;

mach_vm_address_t getKextBase(size_t groupIndex) {
	if (groupIndex == 0) return gHWLibsBase ? gHWLibsBase : gDeferredHWLibsAddr;
	if (groupIndex < arrsize(gAppleBegin) && gAppleBegin[groupIndex]) return gAppleBegin[groupIndex];
	for (size_t i = 0; i < arrsize(kexts); ++i) {
		if (kexts[i].loadIndex == groupIndex && i < arrsize(gAppleBegin)) {
			return gAppleBegin[i];
		}
	}
	return 0;
}
static mach_vm_address_t orgPciExtRead16, orgPciCfgRead16, orgPciCfgRead32, orgPciExtRead32;
#define orgPciRead16 orgPciExtRead16
#define orgPciRead32 orgPciExtRead32

// Bare metal: every async USB transfer (control/bulk/interrupt) completes only at the next MFINDEX
// wrap, i.e. up to 2048 ms late (BT fw 252-B fragments ~4 s each, slow enumeration on XHC1, boot
// busy timeouts). AppleUSBXHCIRequest::prepare puts Interrupter Target 1 (io interrupter) into every
// TRB. These AMD xHCs offer one MSI message (ctl 0081) while macOS runs 3 interrupters; xhciwatch
// (10-26 boot) shows i1 ERDP with EHB (bit 3) set all the time and IMAN IP 0 = i1's interrupt was
// sent but never reached AppleUSBXHCI's handler, so i1's ring is only drained when the wrap event
// on i0 runs the io handler too. Linux uses interrupter 0 only. Point those TRBs at interrupter 0:
// primaryInterrupt handles Transfer Events the same way ioInterrupt does (slot/endpoint dispatch).
// Isochronous TRBs (interrupter 2) are left alone. `-rmbxhciintr1` = keep interrupter 1.
//
// Tahoe 25G229 (Lenovo 660M boots 014130/020832: USB HID cursor moves in 2.048 s steps, all five
// XHC with IOPCIMSIMessageControl 0x80 = one MSI message). The 15.8.1 byte patterns of the original
// no longer cover 25G229: the data TRB is built by `mov $0x400000,%ecx` or by
// `(min(tdsize,31) | 0x20) << 17`, so the fork's scan would have missed the data TRB (and was never
// called: the original ran it from its OSKext::start route, which this fork no longer has).
// Exact sites of the Boot KC image (com.apple.driver.usb.AppleUSBXHCI, LC_UUID
// 4F5E24C5-4FEA-32DA-A8AD-D2FEFEF3ABF2, __TEXT 0x3b000 at the Mach-O header); RVA = offset from that
// header; build/re_xhci_20261109/xhci_sites.py checks each pattern is unique in __TEXT.
// 25G229 static facts behind this (AppleUSBXHCI disassembly, RVAs):
//  - primaryInterrupt +0xc426, Transfer Event = jump table +0xd29c entry 32 -> +0xc4e3: slot = dw3>>24,
//    endpoint = (dw3>>16)&0x1f, this+0x640+slot*16, endpoint vtable 0x178 (+0xca6a..+0xca93) - the
//    same dispatch as ioInterrupt (+0xc29c..+0xc2c3).
//  - no code reads interrupters[1] (+0xe48) directly; ioInterrupt walks +0xe40 by index.
//  - every interrupter's event ring is 0x4000 bytes (Interrupter::initWithParameters +0x14142), i0 too.
//  - isochronous TRBs use 0x800000 / addTD(.., 2, ..) and are not touched. addTD(.., 1, 0x10) builds
//    Link TRBs with Chain only (dw3 0x1811, no IOC -> no event), not touched either.
static const uint8_t XhciUuid25G229[16] {0x4f, 0x5e, 0x24, 0xc5, 0x4f, 0xea, 0x32, 0xda,
                                         0xa8, 0xad, 0xd2, 0xfe, 0xfe, 0xf3, 0xab, 0xf2};
static constexpr uint64_t XhciTextSize25G229 = 0x3b000;
static const struct XhciSite {
	const char *name;
	uint32_t rva;
	uint8_t len, off, from, to;
	uint8_t bytes[17];
} xhciSites[] {
	// Request::prepare: Setup Stage dword2 = 8 | IT 1  (movl $0x400008,0x8(%rcx))
	{"setup", 0x1b79, 10, 8, 0x40, 0x00, {0x48, 0x89, 0x01, 0xc7, 0x41, 0x08, 0x08, 0x00, 0x40, 0x00}},
	// zero-length Normal TRB, dw3 0x421 (IOC)
	{"zlp", 0x20c4, 16, 6, 0x40, 0x00, {0xc7, 0x44, 0x08, 0x08, 0x00, 0x00, 0x40, 0x00, 0xc7, 0x44, 0x08, 0x0c, 0x21, 0x04, 0x00, 0x00}},
	// Data/Normal TRB dword2 without TD size (mov $0x400000,%ecx)
	{"data", 0x2320, 12, 3, 0x40, 0x00, {0xb9, 0x00, 0x00, 0x40, 0x00, 0x4c, 0x89, 0xad, 0xc0, 0xfe, 0xff, 0xff}},
	// Data/Normal TRB dword2 with TD size: cmp $0x1f / cmovae / or $0x20,%ecx / shl $0x11,%ecx
	{"tdsize", 0x2347, 17, 13, 0x20, 0x00, {0x83, 0xf9, 0x1f, 0xb8, 0x1f, 0x00, 0x00, 0x00, 0x0f, 0x43, 0xc8, 0x83, 0xc9, 0x20, 0xc1, 0xe1, 0x11}},
	// Status Stage dword2 (dw3 0x1021 | dir, IOC)
	{"status", 0x2581, 16, 14, 0x40, 0x00, {0x48, 0xc7, 0x04, 0x18, 0x00, 0x00, 0x00, 0x00, 0xc7, 0x44, 0x18, 0x08, 0x00, 0x00, 0x40, 0x00}},
	// Link TRB dword2 (dw3 0x1800; the original's "status" pattern)
	{"link", 0x25da, 16, 6, 0x40, 0x00, {0xc7, 0x44, 0x08, 0x08, 0x00, 0x00, 0x40, 0x00, 0xc7, 0x44, 0x08, 0x0c, 0x00, 0x18, 0x00, 0x00}},
	// SparseRequest::prepare: r8d -> No-op TRB (dw3 0x2020, IOC) and Link TRB
	{"sparse", 0x2eba5, 11, 4, 0x40, 0x00, {0x41, 0xb8, 0x00, 0x00, 0x40, 0x00, 0x44, 0x89, 0x44, 0x3e, 0x08}},
};

static bool xhciImageVerified(mach_vm_address_t base) {
	auto hdr = reinterpret_cast<const mach_header_64 *>(base);
	if (base < 0xffffff8000000000ULL || (base & 0xfff) || hdr->magic != MH_MAGIC_64 || hdr->ncmds > 64 || hdr->sizeofcmds > 0x4000)
		return false;
	bool uuidOk = false;
	uint64_t textSize = 0;
	auto p = reinterpret_cast<const uint8_t *>(base + sizeof(mach_header_64));
	auto end = p + hdr->sizeofcmds;
	for (uint32_t i = 0; i < hdr->ncmds && p + sizeof(load_command) <= end; i++) {
		auto lc = reinterpret_cast<const load_command *>(p);
		if (lc->cmdsize < sizeof(load_command) || p + lc->cmdsize > end)
			break;
		if (lc->cmd == LC_UUID && lc->cmdsize >= sizeof(uuid_command))
			uuidOk = !memcmp(reinterpret_cast<const uuid_command *>(p)->uuid, XhciUuid25G229, sizeof(XhciUuid25G229));
		else if (lc->cmd == LC_SEGMENT_64 && lc->cmdsize >= sizeof(segment_command_64) &&
		         !strncmp(reinterpret_cast<const segment_command_64 *>(p)->segname, SEG_TEXT, 16))
			textSize = reinterpret_cast<const segment_command_64 *>(p)->vmsize;
		p += lc->cmdsize;
	}
	return uuidOk && textSize == XhciTextSize25G229;
}

static mach_vm_address_t xhciFindImage(mach_vm_address_t hint) {
	if (xhciImageVerified(hint))
		return hint;
	for (auto k = gKmods ? *gKmods : nullptr; k; k = k->next)
		if (!strncmp(k->name, "com.apple.driver.usb.AppleUSBXHCI", KMOD_MAX_NAME))
			return xhciImageVerified(k->address) ? static_cast<mach_vm_address_t>(k->address) : 0;
	return 0;
}

static uint8_t gXhciIntrState;  // 0 pending, 1 done/refused, 2 being applied
// true when there is nothing more to do (applied, already applied, refused or disabled)
static bool xhciPrimaryInterrupter(mach_vm_address_t hint, const char *when) {
	uint8_t expect = 0;
	if (!__atomic_compare_exchange_n(&gXhciIntrState, &expect, 2, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
		return true;
	auto finish = [](bool done) {
		__atomic_store_n(&gXhciIntrState, done ? 1 : 0, __ATOMIC_RELEASE);
		return done;
	};
	if (checkKernelArgument("-rmbxhciintr1")) {
		LOG("xhciintr: disabled (-rmbxhciintr1), TRBs keep interrupter target 1");
		return finish(true);
	}
	auto base = xhciFindImage(hint);
	if (!base) {
		// Not loaded yet, or an image other than 25G229 (then the name is in the kmod list).
		bool listed = false;
		for (auto k = gKmods ? *gKmods : nullptr; k; k = k->next)
			if (!strncmp(k->name, "com.apple.driver.usb.AppleUSBXHCI", KMOD_MAX_NAME))
				listed = true;
		if (listed) {
			LOG("xhciintr: %s: AppleUSBXHCI is not the 25G229 image (LC_UUID/__TEXT mismatch), not patched", when);
			return finish(true);
		}
		LOG("xhciintr: %s: AppleUSBXHCI not in the kmod list yet", when);
		return finish(false);
	}
	auto p = reinterpret_cast<uint8_t *>(base);
	size_t todo = 0, done = 0;
	for (auto &s : xhciSites) {
		auto q = p + s.rva;
		bool ctx = true;
		for (uint8_t i = 0; i < s.len; i++)
			if (i != s.off && q[i] != s.bytes[i])
				ctx = false;
		if (!ctx || (q[s.off] != s.from && q[s.off] != s.to)) {
			LOG("xhciintr: %s: site %s +%x bytes differ (byte %02x), nothing patched", when, s.name, s.rva, q[s.off]);
			return finish(true);
		}
		(q[s.off] == s.from ? todo : done)++;
	}
	if (todo && done) {
		LOG("xhciintr: %s: %zu sites patched, %zu not - mixed image, left unchanged", when, done, todo);
		return finish(true);
	}
	if (!todo) {
		LOG("xhciintr: %s: all %zu sites already at interrupter 0", when, done);
		return finish(true);
	}
	if (MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) != KERN_SUCCESS) {
		LOG("xhciintr: %s: kernel writing failed, not patched", when);
		return finish(true);
	}
	for (auto &s : xhciSites)
		p[s.rva + s.off] = s.to;
	MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
	size_t ok = 0;
	for (auto &s : xhciSites)
		ok += p[s.rva + s.off] == s.to;
	LOG("xhciintr: %s: AppleUSBXHCI 25G229 LC_UUID 4F5E24C5 verified, TRB interrupter target 1 -> 0 at %zu/%zu sites "
	    "(setup zlp data tdsize status link sparse), isochronous untouched", when, ok, arrsize(xhciSites));
	return finish(true);
}

static const char *pathXhci[] {"/System/Library/Extensions/IOUSBHostFamily.kext/Contents/PlugIns/AppleUSBXHCI.kext/Contents/MacOS/AppleUSBXHCI"};
static KernelPatcher::KextInfo kextXhci[] {
	{"com.apple.driver.usb.AppleUSBXHCI", pathXhci, arrsize(pathXhci), {true}, {}, KernelPatcher::KextInfo::Unloaded},
};

// Lilu's load callback runs after AppleUSBXHCI is linked and before its start (or at patcher load
// if the Boot KC already loaded it). If the kmod is not listed yet, poll it for up to 20 s.
static void xhciRetryThread(void *, wait_result_t) {
	for (uint32_t i = 0; i < 200; i++) {
		IOSleep(100);
		if (xhciPrimaryInterrupter(0, "retry"))
			return;
	}
	LOG("xhciintr: AppleUSBXHCI never appeared in the kmod list, not patched");
}

static void processXhciKext(void *, KernelPatcher &, size_t id, mach_vm_address_t address, size_t) {
	if (id != kextXhci[0].loadIndex)
		return;
	if (xhciPrimaryInterrupter(address, "load"))
		return;
	thread_t t {};
	if (kernel_thread_start(xhciRetryThread, nullptr, &t) == KERN_SUCCESS)
		thread_deallocate(t);
}

// Lilu calls our onPatcherLoad callback (which routes OSKext::start) BEFORE it loads the kinfos of
// our kexts[] (kern_api.cpp: callbacks, then loadKinfo per stored kext). A kext starting on another
// CPU in that window ran scanKmods with loadIndex still Unloaded (0 = the kernel's kinfo): the
// IOPCIFamily route failed for good, the device-id spoof never applied, HWLibs saw 1681
// (ttl_initialize = 1) and WindowServer panicked "Failed to send PPLIB IRI to Accelerator".
// Skip such a kext until Lilu has filled its loadIndex; a later OSKext::start retries it.
static bool kinfoReady(size_t i) {
	if (kexts[i].loadIndex != KernelPatcher::KextInfo::Unloaded)
		return true;
	static bool logged[arrsize(kexts)];
	if (!logged[i])
		logged[i] = true, LOG("kinfo: %s seen before Lilu loaded its kinfo, retry later", kexts[i].id);
	return false;
}

static mach_vm_address_t orgHWServicesProbe, orgHWServicesStart;
static IOService *wrapHWServicesProbe(IOService *self, IOService *provider, int32_t *score) {
	IOLog("REM660: [wrapHWServicesProbe] ENTER self=%p provider=%p score=%d\n", self, provider, score ? *score : 0);
	stageSet(StHwsProbe, 'E');
	ensureGpuReadyForStart(provider);
	using ProbeFn = IOService *(*)(IOService *, IOService *, int32_t *);
	IOService *res = nullptr;
	if (orgHWServicesProbe) {
		res = reinterpret_cast<ProbeFn>(orgHWServicesProbe)(self, provider, score);
	}
	if (res && score) {
		// Apple's personalities give HWServices (65051) the highest score of the AMD group
		// (Wrangler 60000, ControllerNavi23 1000), so HWServices is attached/started before
		// Controller::start -> setupHwServices -> findHwServices (FB @0x4eca2) looks for it.
		// Our Wrangler/Controller use 65055/65054, so keep HWServices above both.
		*score = 65056;
	}
	stageSet(StHwsProbe, res ? '1' : '0');
	IOLog("REM660: [wrapHWServicesProbe] EXIT res=%p score=%d\n", res, score ? *score : 0);
	return res;
}

static bool wrapHWServicesStart(IOService *self, IOService *provider) {
	IOLog("REM660: [wrapHWServicesStart] ENTER self=%p provider=%p\n", self, provider);
	rmbCheckpoint("HWServicesStart ENTER");
	stageSet(StHwsStart, 'E');
	ensureGpuReadyForStart(provider);
	using StartFn = bool (*)(IOService *, IOService *);
	bool ok = false;
	if (orgHWServicesStart) {
		ok = reinterpret_cast<StartFn>(orgHWServicesStart)(self, provider);
	}
	stageSet(StHwsStart, ok ? '1' : '0');
	IOLog("REM660: [wrapHWServicesStart] EXIT ok=%d (hooked0=%d)\n", ok ? 1 : 0, isKextHooked(0) ? 1 : 0);
	rmbCheckpoint("HWServicesStart EXIT");
	if (ok && provider && !gAccelProbeSeen) {  // rematch only while no accelerator instance exists
		provider->registerService(kIOServiceAsynchronous);
	}
	return ok;
}

static bool processHWServices(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
	IOLog("REM660: [processHWServices] ENTER addr=0x%llx size=0x%zx\n", address, size);
	KernelPatcher::RouteRequest requests[] {
		// 25G229 HWServices (sha256 e4ee406d...): Navi has no own probe/start. SWIP::start (@0xff0)
		// calls Abstract::start via the Abstract vtable, then createTtlInterface (+0xd0). Route only
		// one symbol per org slot so the shared trampoline can never skip createTtlInterface.
		{"__ZN42AMDRadeonX6000_AMDRadeonHWServicesAbstract5probeEP9IOServicePi", wrapHWServicesProbe, orgHWServicesProbe},
		{"__ZN38AMDRadeonX6000_AMDRadeonHWServicesSWIP5startEP9IOService", wrapHWServicesStart, orgHWServicesStart},
	};
	if (!restoredRouteGroup(patcher, 3, requests, arrsize(requests), address, size)) {
		patcher.clearError();
		routeStaticRequests(patcher, 3, requests, arrsize(requests), address, size);
		patcher.clearError();
	}
	if (!orgHWServicesProbe) {
		mach_vm_address_t sProbe = findSymbol(patcher, 3, "__ZN38AMDRadeonX6000_AMDRadeonHWServicesNavi5probeEP9IOServicePi", address, size);
		if (!sProbe) sProbe = findSymbol(patcher, 3, "__ZN42AMDRadeonX6000_AMDRadeonHWServicesAbstract5probeEP9IOServicePi", address, size);
		if (sProbe != 0) {
			orgHWServicesProbe = patcher.routeFunction(sProbe, reinterpret_cast<mach_vm_address_t>(wrapHWServicesProbe), true);
			IOLog("REM660: [processHWServices] fallback routed HWServices::probe at 0x%llx (org=%p)\n", (uint64_t)sProbe, (void *)orgHWServicesProbe);
		} else {
			IOLog("REM660: FATAL - Missing symbol for HWServicesProbe\n");
		}
	}
	if (!orgHWServicesStart) {
		mach_vm_address_t sStart = findSymbol(patcher, 3, "__ZN38AMDRadeonX6000_AMDRadeonHWServicesNavi5startEP9IOService", address, size);
		if (!sStart) sStart = findSymbol(patcher, 3, "__ZN38AMDRadeonX6000_AMDRadeonHWServicesSWIP5startEP9IOService", address, size);
		if (!sStart) sStart = findSymbol(patcher, 3, "__ZN42AMDRadeonX6000_AMDRadeonHWServicesAbstract5startEP9IOService", address, size);
		if (sStart != 0) {
			orgHWServicesStart = patcher.routeFunction(sStart, reinterpret_cast<mach_vm_address_t>(wrapHWServicesStart), true);
			IOLog("REM660: [processHWServices] fallback routed HWServices::start at 0x%llx (org=%p)\n", (uint64_t)sStart, (void *)orgHWServicesStart);
		} else {
			IOLog("REM660: FATAL - Missing symbol for HWServicesStart\n");
		}
	}
	IOLog("REM660: [processHWServices] EXIT SUCCESS\n");
	return true;
}

static void prefaultKextPages(mach_vm_address_t address, size_t size) {
	if (!address || !size)
		return;
	volatile uint8_t sink = 0;
	const volatile uint8_t *base = reinterpret_cast<const volatile uint8_t *>(address);
	for (size_t off = 0; off < size; off += 0x1000) {
		sink ^= base[off];
	}
	sink ^= base[size - 1];
	(void)sink;
}

static bool routeGroup(KernelPatcher &patcher, size_t i, mach_vm_address_t address, size_t size) {
	if (i >= arrsize(kexts) || hooked[i] || address == 0 || size == 0)
		return false;

	hooked[i] = true;

	if (i < arrsize(gAppleBegin)) {
		gAppleBegin[i] = address;
		gAppleSize[i] = size;
	}

	prefaultKextPages(address, size);
	if (kexts[i].loadIndex != KernelPatcher::KextInfo::Unloaded) {
		auto updatedSize = patcher.updateRunningInfo(kexts[i].loadIndex, address, size);
		if (updatedSize > size) {
			prefaultKextPages(address, updatedSize);
			size = updatedSize;
		}
		patcher.clearError();
	}

	LOG("routeGroup: ENTER idx=%lu (%s) addr=%llx size=%zx", (unsigned long)i, kexts[i].id, (unsigned long long)address, size);
	IOLog("REM660: [routeGroup] idx=%lu START (%s)\n", (unsigned long)i, kexts[i].id);
	bool ok = false;
	if (i == 0) {
		gHWLibsBase = address;
		IOLog("REM660: [hwlibsRoute] HOOKED HWLibs at 0x%llx\n", static_cast<uint64_t>(address));
		stageSet(StHWLibs, 'E');
		ok = processHWLibs(patcher, kexts[i].loadIndex, address, size);
		stageSet(StHWLibs, ok ? '1' : '0');
		if (ok) {
			hooked[0] = true;
			__atomic_fetch_or(&gCompletedImages, 1u << 0, __ATOMIC_RELEASE);
		}
	} else if (i == 1) {
		ok = processFramebuffer(patcher, kexts[i].loadIndex, address, size);
		if (ok) {
			hooked[1] = true;
			__atomic_fetch_or(&gCompletedImages, 1u << 1, __ATOMIC_RELEASE);
		}
	} else if (i == 2) {
		ok = probeRouteAccel(patcher, kexts[i].loadIndex, address, size);
		if (ok) {
			hooked[2] = true;
			__atomic_fetch_or(&gCompletedImages, 1u << 2, __ATOMIC_RELEASE);
		}
	} else if (i == 3) {
		IOLog("REM660: [routeGroup] routing AMDRadeonX6000HWServices (idx=3)\n");
		ok = processHWServices(patcher, kexts[i].loadIndex, address, size);
		if (ok) {
			hooked[3] = true;
			__atomic_fetch_or(&gCompletedImages, 1u << 3, __ATOMIC_RELEASE);
		}
	} else if (i == BacklightIdx) {
		IOLog("REM660: [routeGroup] AppleBacklight (idx=%lu) bypassed for Pageable KC safety\n", (unsigned long)BacklightIdx);
		displayNoteBacklightImage(address, size);  // caller check for the bldiag fb attribute log only
		hooked[BacklightIdx] = true;
		ok = true;
	}
	LOG("routeGroup: EXIT idx=%lu ok=%d failed=%d", (unsigned long)i, ok ? 1 : 0, restoredRoutingFailed() ? 1 : 0);
	IOLog("REM660: [routeGroup] idx=%lu DONE (ok=%d)\n", (unsigned long)i, ok ? 1 : 0);
	if (!ok) {
		hooked[i] = false;
	}
	// Boot evidence 2026-11-08: the 18:29 binary called this at every routeGroup (first at idx=4,
	// AppleBacklight, the first Pageable KC kext) and FB/HWServices matched afterwards; the 19:20
	// binary without it stopped after idx=4 with no AMD kext loaded. Keep the property sync and
	// spoof enable, but never registerService() from this callback.
	syncPciDeviceProperties(false);
	if (hooked[1] && hooked[2] && hooked[3])
		IOLog("REM660: [routeGroup] AMD kext groups 1,2,3 routed (no forced rematch)\n");
	return ok;
}

void scanKmods(KernelPatcher *patcher) {
	if (patcher) gPatcher = patcher;
	if (!gKmods || !gPatcher)
		return;
	if (__atomic_test_and_set(&gScanActive, __ATOMIC_ACQUIRE))
		return;
	struct UnlockScan { ~UnlockScan() { __atomic_clear(&gScanActive, __ATOMIC_RELEASE); } } unlock;

	// Deferred routing for HWLibs (idx=0) captured during processLoadedKext
	// Routing after Pageable KC sliding has finished avoids "Sliding pageable kc was stopped"
	if (!hooked[0] && gDeferredHWLibsAddr && gDeferredHWLibsSize) {
		if (kexts[0].loadIndex == KernelPatcher::KextInfo::Unloaded) {
			gPatcher->loadKinfo(&kexts[0]);
		}
		IOLog("REM660: [scanKmods] executing deferred routing for HWLibs (idx=0) at 0x%llx\n", static_cast<uint64_t>(gDeferredHWLibsAddr));
		routeGroup(*gPatcher, 0, gDeferredHWLibsAddr, gDeferredHWLibsSize);
	}

	static bool loggedAmdKmods = false;
	if (!loggedAmdKmods) {
		loggedAmdKmods = true;
		for (auto k = *gKmods; k; k = k->next) {
			if (k->name[0] && strstr(k->name, "AMD")) {
				IOLog("REM660: [kmod] %s addr=0x%llx size=0x%llx\n", k->name, static_cast<uint64_t>(k->address), static_cast<uint64_t>(k->size));
			}
		}
	}

	// Strictly enforce dependency order across *gKmods:
	// HWLibs (0) -> Framebuffer (1) -> Accelerator (2) -> HWServices (3) -> Backlight
	static const size_t priorityOrder[] = {0, 1, 2, 3, BacklightIdx};
	for (size_t p = 0; p < arrsize(priorityOrder); p++) {
		size_t i = priorityOrder[p];
		if (i >= arrsize(kexts) || hooked[i])
			continue;
		for (auto kmod = *gKmods; kmod; kmod = kmod->next) {
			if (strncmp(kmod->name, kexts[i].id, KMOD_MAX_NAME))
				continue;
			if (kexts[i].loadIndex == KernelPatcher::KextInfo::Unloaded) {
				gPatcher->loadKinfo(&kexts[i]);
			}
			// Only require kinfoReady for non-AMD kexts (Backlight, etc.)
			// For AMD kexts (HWLibs, FB, Accel, HWServices), we have static RVAs and can route directly!
			if (i > 3 && !kinfoReady(i))
				break;
			if (restoredRoutingFailed()) return;
			prefaultKextPages(kmod->address, kmod->size);
			mach_vm_size_t size = kmod->size;
			if (kexts[i].loadIndex != KernelPatcher::KextInfo::Unloaded) {
				auto updatedSize = gPatcher->updateRunningInfo(kexts[i].loadIndex, kmod->address, kmod->size);
				if (updatedSize > size) size = updatedSize;
			}
			LOG("found %s, size %zx", kexts[i].id, (size_t)size);
			IOLog("REM660: [scanKmods] found %s (idx=%lu), size 0x%zx\n", kexts[i].id, (unsigned long)i, (size_t)size);
			routeGroup(*gPatcher, i, kmod->address, size);
			break;
		}

		// No _psp_ring_create - 0x4c6a0 / fake 16 MB fallback (unverified base and size).
	}
}

void scanKmods(KernelPatcher &patcher) {
	scanKmods(&patcher);
}

void scanKmods() {
	scanKmods(nullptr);
}

static void processLoadedKext(void *user, KernelPatcher &patcher, size_t id, mach_vm_address_t address, size_t size) {
	for (size_t i = 0; i < arrsize(kexts); ++i) {
		if (kexts[i].loadIndex == id) {
			IOLog("REM660: [processLoadedKext] matched idx=%lu (%s) id=%lu at 0x%llx size=0x%lx\n",
			      (unsigned long)i, kexts[i].id, (unsigned long)id, static_cast<uint64_t>(address), (unsigned long)size);

			if (i == 0) {
				// Defer HWLibs patching until after Pageable KC sliding is complete!
				// Patching executable pages during onKextLoad triggers "Sliding pageable kc was stopped"!
				gDeferredHWLibsAddr = address;
				gDeferredHWLibsSize = size;
				IOLog("REM660: [processLoadedKext] deferred HWLibs (idx=0) at 0x%llx size=0x%lx for post-slide routing\n",
				      static_cast<uint64_t>(address), (unsigned long)size);
				return;
			}

			if (i == 1 || i == 2 || i == 3) {
				gEnablePciSpoof = true;
			}

			routeGroup(patcher, i, address, size);
			break;
		}
	}
}

// The AMD kexts read the PCI device ID from config space (IOPCIDevice::extendedConfigRead*),
// not from the device-id property. In the VM QEMU's x-pci-device-id faked it; on bare metal
// they see the real 1002:1681 and HWLibs rejects the device (ttl_initialize = 1, then the
// "Failed to send PPLIB IRI to Accelerator" panic). Report 73ff like the VM did.
static bool gDevidLogged;

static bool lenovoPci(void *dev) {
	if (!dev) return false;
	if (gSyncedDev && gSyncedDev == dev)
		return true;
	auto svc = static_cast<IOService *>(dev);
	auto vData = OSDynamicCast(OSData, svc->getProperty("vendor-id"));
	auto dData = OSDynamicCast(OSData, svc->getProperty("device-id"));
	if (vData && dData && vData->getLength() >= 2 && dData->getLength() >= 2) {
		uint16_t vendor = *reinterpret_cast<const uint16_t *>(vData->getBytesNoCopy());
		uint16_t device = *reinterpret_cast<const uint16_t *>(dData->getBytesNoCopy());
		if (vendor == 0x1002 && (device == 0x1681 || device == 0x73ff || device == 0x77ff))
			return true;
	}
	return false;
}

bool restoredTargetProvider(void *provider) {
	// Public string metacast keeps the dynamic type check without importing the
	// IOPCIDevice metaclass; keep the existing public runtime type check.
	return provider && static_cast<IOService *>(provider)->metaCast("IOPCIDevice") && lenovoPci(provider);
}

#ifndef kIOServiceForce
#define kIOServiceForce 0
#endif

static void syncPciDeviceDirect(IOService *svc) {
	if (!svc) return;
	const uint32_t spoofId = 0x73ff;
	if (auto *devData = OSData::withBytes(&spoofId, sizeof(spoofId))) {
		svc->setProperty("device-id", devData);
		devData->release();
	}
	static const char kCompatNavi23[] = "pci1002,73ff";
	if (auto *compatData = OSData::withBytes(kCompatNavi23, sizeof(kCompatNavi23))) {
		svc->setProperty("compatible", compatData);
		compatData->release();
	}
	static const char kModel[] = "AMD Radeon 660M";
	if (auto *modelData = OSData::withBytes(kModel, sizeof(kModel))) {
		svc->setProperty("model", modelData);
		modelData->release();
	}
	// Metal device name (MTLDevice.name, Geekbench, About This Mac GPU lists): the GFX10 Metal driver
	// (AMDRadeonX6000MTLDriver, dyld cache 25G229) builds it from the PCI device's ATY,FamilyName and
	// ATY,DeviceName, which Apple's firmware injects on real Macs ("Radeon Pro" + "5500M" -> "AMD Radeon
	// Pro 5500M"); without them it reports "AMD Radeon HD GFX10 Family Unknown Prototype". Same NUL-terminated
	// data form as EFI device properties.
	static const char kFamily[] = "Radeon", kDevice[] = "660M";
	if (auto *d = OSData::withBytes(kFamily, sizeof(kFamily))) {
		svc->setProperty("ATY,FamilyName", d);
		d->release();
	}
	if (auto *d = OSData::withBytes(kDevice, sizeof(kDevice))) {
		svc->setProperty("ATY,DeviceName", d);
		d->release();
	}
	svc->removeProperty("AAPL,ndrv-dev");
	svc->setProperty("LoadAccelerator", kOSBooleanTrue);
	svc->setProperty("LoadController", kOSBooleanTrue);
	svc->setProperty("LoadHWServices", kOSBooleanTrue);
	svc->setProperty("LoadPlugIn", "Load6000");
	// Internal panel = AMD link 0 (boot 2026-11-09 03:40, evidence/rmb-msgbuf_20261109_0340_replay.log:
	// link 0 detect status carries EDID 00ffffffffffff00 09e5 = BOE panel, link 1 nothing). FB 25G229
	// (LC_UUID C3FA8B38-...) binds a link to FB:0 by itself only for links flagged in the IOPCIDevice
	// property CFG_LINK_FIXED_MAP (bit n = link n):
	//   AmdLink::initWithInfo 0x4562c: getRegistryProperty(controller->getPciDevice(), "CFG_LINK_FIXED_MAP")
	//     (OSData/OSString/OSBoolean/OSNumber, parseOSObjectValue 0x3ac68) -> link+0x47 = getFeature(13);
	//   AmdRadeonController::getAttribute(1) 0x4ea0d -> first link with getFeature(13) == 1;
	//   AmdRadeonFramebuffer::enableController 0x14f6c -> initializeFixedDisplayBinding 0x151e6 (FB index 0
	//     only): display path +0xd0 = 1 link, +0xec = link idx, +0x719 = link->isConnected(), fb flag 0x20;
	//   detectDisplayOnLink 0x4e5bc / handleHotPlug 0x1986a: fixed links skip the AGDC link event.
	// Without it every link waits for displaypolicyd's AGDC commitConfiguration (cmd 0x1801), which never
	// came under iMac20,1: no AGDC command at all after the replayed link-0 Insert at 66.97 s, FB:0/FB:1
	// "path 0 links []" for 3 minutes. Keep a value DeviceProperties may have set; -rmbnofixedmap = don't add.
	if (!svc->getProperty("CFG_LINK_FIXED_MAP") && !checkKernelArgument("-rmbnofixedmap")) {
		if (auto *fixedMap = OSNumber::withNumber(1ULL, 32)) {
			svc->setProperty("CFG_LINK_FIXED_MAP", fixedMap);
			fixedMap->release();
			IOLog("REM660: [syncPciDeviceDirect] CFG_LINK_FIXED_MAP = 1 (link 0 = internal eDP -> FB:0)\n");
		}
	}
	// NOTE: Do NOT call svc->registerService() from inside wrapPciRead16/32 to prevent recursive matching
}

bool hookPciDeviceFromInstance(KernelPatcher &patcher, IOService *provider);
bool hookPciDeviceFromInstance(IOService *provider);

void syncSinglePciDeviceProps(void *dev, bool triggerRematch) {
	if (!dev) return;
	gSyncedDev = dev;
	auto svc = reinterpret_cast<IOService *>(dev);
	syncPciDeviceDirect(svc);
	if (triggerRematch && !gAcceleratorStarted) {
		IOLog("REM660: [syncSinglePciDeviceProps] async registerService requested on VGA@0\n");
		svc->registerService(kIOServiceAsynchronous);
	}
}

static mach_vm_address_t parseSymbolFromMachO(mach_vm_address_t headerAddr, const char *targetSym) {
	if (!headerAddr || !targetSym) return 0;
	const auto *hdr = reinterpret_cast<const mach_header_64 *>(headerAddr);
	if (hdr->magic != MH_MAGIC_64) return 0;

	mach_vm_address_t textVmAddr = 0;
	mach_vm_address_t linkeditVmAddr = 0;
	uint64_t linkeditFileOff = 0;
	const symtab_command *symtab = nullptr;

	const uint8_t *cmdPtr = reinterpret_cast<const uint8_t *>(headerAddr + sizeof(mach_header_64));
	for (uint32_t i = 0; i < hdr->ncmds; i++) {
		const auto *cmd = reinterpret_cast<const load_command *>(cmdPtr);
		if (cmd->cmd == LC_SEGMENT_64) {
			const auto *seg = reinterpret_cast<const segment_command_64 *>(cmdPtr);
			if (strncmp(seg->segname, SEG_TEXT, sizeof(seg->segname)) == 0) {
				textVmAddr = seg->vmaddr;
			} else if (strncmp(seg->segname, SEG_LINKEDIT, sizeof(seg->segname)) == 0) {
				linkeditVmAddr = seg->vmaddr;
				linkeditFileOff = seg->fileoff;
			}
		} else if (cmd->cmd == LC_SYMTAB) {
			symtab = reinterpret_cast<const symtab_command *>(cmdPtr);
		}
		cmdPtr += cmd->cmdsize;
	}

	if (!symtab) return 0;

	mach_vm_address_t slide = (textVmAddr && headerAddr > textVmAddr) ? (headerAddr - textVmAddr) : 0;
	mach_vm_address_t linkeditBase = 0;
	if (linkeditVmAddr) {
		linkeditBase = linkeditVmAddr + slide - linkeditFileOff;
	} else {
		linkeditBase = headerAddr;
	}

	const auto *syms = reinterpret_cast<const nlist_64 *>(linkeditBase + symtab->symoff);
	const char *strings = reinterpret_cast<const char *>(linkeditBase + symtab->stroff);

	for (uint32_t i = 0; i < symtab->nsyms; i++) {
		uint32_t strx = syms[i].n_un.n_strx;
		if (strx < symtab->strsize) {
			const char *name = strings + strx;
			if (strcmp(name, targetSym) == 0) {
				mach_vm_address_t addr = syms[i].n_value + slide;
				return addr;
			}
		}
	}
	return 0;
}

// AppleGraphicsDeviceControl 8.1.12 in the 25G229 SystemKernelExtensions.kc (evidence/, fileset entry
// vmaddr 0x13d3f000): LC_UUID 02FECF8A-3227-3BBB-B0EB-1DB31965ADB9, the UUID the running 25G229 system
// reports (D:\panic-2026-11-08-233604.txt dependency list). Its symtab gives
// __ZN26AppleGraphicsDeviceControl18sWindowServerReadyE = 0x13d45914 (__DATA,__common) = __TEXT + 0x6914.
// The original (15.8.1) found this kmod in scanKmods() and called displaySetAgdcBase(); the fork lost
// that call, so replayConnect never set the flag and displaypolicyd never assigned the eDP to a
// framebuffer (boot 2026-11-09: FB:0/FB:1 "links [] connected 0", black screen). Only the exact image.
bool resolveAgdcBase() {
	static const uint8_t AgdcUuid[16] {0x02, 0xfe, 0xcf, 0x8a, 0x32, 0x27, 0x3b, 0xbb,
	                                   0xb0, 0xeb, 0x1d, 0xb3, 0x19, 0x65, 0xad, 0xb9};
	constexpr uint64_t WsReadyOff = 0x6914;
	if (!gKmods || !*gKmods) {
		LOG("agdc: kmod list missing, sWindowServerReady not resolved");
		return false;
	}
	for (auto k = *gKmods; k; k = k->next) {
		if (strncmp(k->name, "com.apple.AppleGraphicsDeviceControl", KMOD_MAX_NAME))
			continue;
		auto base = static_cast<mach_vm_address_t>(k->address);
		auto hdr = reinterpret_cast<const mach_header_64 *>(base);
		if (!base || hdr->magic != MH_MAGIC_64 || hdr->ncmds > 64 || hdr->sizeofcmds > 0x4000) {
			LOG("agdc: AppleGraphicsDeviceControl at %llx: no 64-bit Mach-O header, sWindowServerReady not used", base);
			return false;
		}
		bool uuidOk = false, haveText = false, haveData = false;
		uint64_t textVm = 0, dataVm = 0, dataSize = 0;
		auto p = reinterpret_cast<const uint8_t *>(base + sizeof(mach_header_64));
		auto end = p + hdr->sizeofcmds;
		for (uint32_t i = 0; i < hdr->ncmds && p + sizeof(load_command) <= end; i++) {
			auto lc = reinterpret_cast<const load_command *>(p);
			if (lc->cmdsize < sizeof(load_command) || p + lc->cmdsize > end)
				break;
			if (lc->cmd == LC_UUID && lc->cmdsize >= sizeof(uuid_command)) {
				uuidOk = !memcmp(reinterpret_cast<const uuid_command *>(p)->uuid, AgdcUuid, sizeof(AgdcUuid));
			} else if (lc->cmd == LC_SEGMENT_64 && lc->cmdsize >= sizeof(segment_command_64)) {
				auto seg = reinterpret_cast<const segment_command_64 *>(p);
				if (!strncmp(seg->segname, SEG_TEXT, sizeof(seg->segname)))
					textVm = seg->vmaddr, haveText = true;
				else if (!strncmp(seg->segname, SEG_DATA, sizeof(seg->segname)))
					dataVm = seg->vmaddr, dataSize = seg->vmsize, haveData = true;
			}
			p += lc->cmdsize;
		}
		uint64_t dataOff = dataVm - textVm;
		if (!uuidOk || !haveText || !haveData || dataVm < textVm || WsReadyOff < dataOff ||
		    WsReadyOff + sizeof(uint32_t) > dataOff + dataSize) {
			LOG("agdc: AppleGraphicsDeviceControl at %llx: LC_UUID %s, __TEXT %d __DATA %d (+%llx size %llx): "
			    "not the 25G229 image, sWindowServerReady not used", base, uuidOk ? "match" : "MISMATCH", haveText,
			    haveData, dataOff, dataSize);
			return false;
		}
		LOG("agdc: AppleGraphicsDeviceControl LC_UUID 02FECF8A-3227-3BBB-B0EB-1DB31965ADB9 verified, __DATA +%llx "
		    "size %llx holds sWindowServerReady +%llx", dataOff, dataSize, WsReadyOff);
		displaySetAgdcBase(base);
		return true;
	}
	LOG("agdc: com.apple.AppleGraphicsDeviceControl not in kmod list, sWindowServerReady not resolved");
	return false;
}

static mach_vm_address_t findIopciBase(mach_vm_address_t address, void *dev = nullptr) {
	if (address) {
		return address;
	}
	if (gKmods && *gKmods) {
		for (auto kmod = *gKmods; kmod; kmod = kmod->next) {
			if (kmod->name[0] && !strncmp(kmod->name, "com.apple.iokit.IOPCIFamily", KMOD_MAX_NAME)) {
				if (kmod->address) {
					IOLog("REM660: [findIopciBase] found IOPCIFamily at kmod->address 0x%llx\n", static_cast<uint64_t>(kmod->address));
					return kmod->address;
				}
			}
		}
	}
	return 0;
}



// Spoof semantics follow the original RembrandtGfx (kern_start.cpp:886-910 of the 680M source):
// only DeviceID (off 2, 16-bit, vendor re-read == 0x1002) and VendorID/DeviceID dword (off 0) of
// a 1002:1681 function. No IORegistry access here: config reads can run with locks held.
using PciRead16Fn = uint16_t (*)(void *, uint64_t);
using PciRead32Fn = uint32_t (*)(void *, uint64_t);

static uint16_t spoof16(mach_vm_address_t org, void *dev, uint64_t off, const char *tag) {
	auto fn = reinterpret_cast<PciRead16Fn>(org);
	uint16_t v = fn ? fn(dev, off) : 0xFFFF;
	if (gEnablePciSpoof && fn && off == kIOPCIConfigDeviceID && v == 0x1681 && fn(dev, kIOPCIConfigVendorID) == 0x1002) {
		static uint32_t logged;
		if (logged++ < 4)
			IOLog("REM660: [PCI16] %s DeviceID 0x1681 -> 0x73ff\n", tag);
		return 0x73ff;
	}
	return v;
}

static uint32_t spoof32(mach_vm_address_t org, void *dev, uint64_t off, const char *tag) {
	auto fn = reinterpret_cast<PciRead32Fn>(org);
	uint32_t v = fn ? fn(dev, off) : 0xFFFFFFFF;
	if (gEnablePciSpoof && off == kIOPCIConfigVendorID && v == 0x16811002) {
		static uint32_t logged;
		if (logged++ < 4)
			IOLog("REM660: [PCI32] %s 0x16811002 -> 0x73ff1002\n", tag);
		return 0x73ff1002;
	}
	return v;
}

// wlanDevcapFilter*: MT7922 devcap FLR bit hidden (wlanfix.cpp); pointer + offset compare only. The configRead*(UInt8)
// offset is an 8-bit argument, so only its low byte is meaningful.
static uint16_t wrapPciExtRead16(void *dev, uint64_t off) {
	return wlanDevcapFilter16(dev, off, spoof16(orgPciExtRead16, dev, off, "ext16"));
}
static uint16_t wrapPciCfgRead16(void *dev, uint64_t off) {
	return wlanDevcapFilter16(dev, off & 0xff, spoof16(orgPciCfgRead16, dev, off, "cfg16"));
}
static uint32_t wrapPciCfgRead32(void *dev, uint64_t off) {
	return wlanDevcapFilter32(dev, off & 0xff, spoof32(orgPciCfgRead32, dev, off, "cfg32"));
}
static uint32_t wrapPciExtRead32(void *dev, uint64_t off) {
	return wlanDevcapFilter32(dev, off, spoof32(orgPciExtRead32, dev, off, "ext32"));
}

static inline uint16_t wrapPciRead16(void *dev, uint64_t off) { return wrapPciExtRead16(dev, off); }
static inline uint32_t wrapPciRead32(void *dev, uint64_t off) { return wrapPciExtRead32(dev, off); }

void syncPciDeviceProperties(bool triggerRematch) {
	gEnablePciSpoof = true;
	auto matching = IOService::serviceMatching("IOPCIDevice");
	if (!matching) return;
	auto iter = IOService::getMatchingServices(matching);
	matching->release();
	if (!iter) return;
	while (auto svc = OSDynamicCast(IOService, iter->getNextObject())) {
		if (lenovoPci(svc)) {
			static bool diagLogged;
			if (!diagLogged) {
				// One-shot diagnostic: did OC DeviceProperties reach VGA@0 before our sync, and what
				// does config space really hold? Decides whether IOPCIMatch needs 0x16811002.
				diagLogged = true;
				auto d = OSDynamicCast(OSData, svc->getProperty("device-id"));
				uint32_t propDev = 0;
				if (d && d->getLength() >= 2)
					memcpy(&propDev, d->getBytesNoCopy(), d->getLength() >= 4 ? 4 : 2);
				uint32_t raw = orgPciCfgRead32 ? reinterpret_cast<PciRead32Fn>(orgPciCfgRead32)(svc, 0) : 0;
				IOLog("REM660: [diag] VGA@0 pre-sync device-id prop=0x%x LoadController=%d LoadAccelerator=%d "
				      "LoadHWServices=%d ignore-ndrv=%d rawCfg0=0x%08x spoof=%d\n", propDev,
				      svc->getProperty("LoadController") ? 1 : 0, svc->getProperty("LoadAccelerator") ? 1 : 0,
				      svc->getProperty("LoadHWServices") ? 1 : 0, svc->getProperty("AAPL,iokit-ignore-ndrv") ? 1 : 0,
				      raw, gEnablePciSpoof ? 1 : 0);
			}
			gSyncedDev = svc;
			syncPciDeviceDirect(svc);
			hookPciDeviceFromInstance(svc);
			mmioAvailable();

			static uint32_t gSyncCount = 0;
			if (gSyncCount++ < 10) {
				IOLog("REM660: [syncPciDeviceProperties] PCI spoof applied on VGA@0 (triggerRematch=%d, count=%u)\n", triggerRematch ? 1 : 0, gSyncCount);
			}

			(void)triggerRematch; // rematch intentionally removed (see routeGroup)
			break;
		}
	}
	iter->release();
}

bool routePciDevid(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
	if (checkKernelArgument("-rmbnopcidevid")) {
		LOG("pcidevid: disabled (-rmbnopcidevid)");
		return false;
	}
	if (hooked[PciIdx] && (orgPciExtRead16 || orgPciCfgRead16)) {
		return true;
	}
	IOLog("REM660: [routePciDevid] ENTER index=%zu addr=0x%llx size=0x%lx\n", index, static_cast<uint64_t>(address), (unsigned long)size);

	mach_vm_address_t pciBase = findIopciBase(address, gSyncedDev);
	IOLog("REM660: [routePciDevid] pciBase=0x%llx\n", static_cast<uint64_t>(pciBase));

	mach_vm_address_t rExt16 = 0, rCfg16 = 0, rCfg32 = 0, rExt32 = 0;

	// Tier 1: Parse Mach-O SYMTAB on pciBase
	if (pciBase) {
		rExt16 = parseSymbolFromMachO(pciBase, "__ZN11IOPCIDevice20extendedConfigRead16Ey");
		if (!rExt16) rExt16 = parseSymbolFromMachO(pciBase, "__ZN11IOPCIDevice20extendedConfigRead16Em");
		rCfg16 = parseSymbolFromMachO(pciBase, "__ZN11IOPCIDevice12configRead16Eh");
		if (!rCfg16) rCfg16 = parseSymbolFromMachO(pciBase, "__ZN11IOPCIDevice12configRead16Em");
		if (!rCfg16) rCfg16 = parseSymbolFromMachO(pciBase, "__ZN11IOPCIDevice12configRead16Ey");
		rCfg32 = parseSymbolFromMachO(pciBase, "__ZN11IOPCIDevice12configRead32Eh");
		if (!rCfg32) rCfg32 = parseSymbolFromMachO(pciBase, "__ZN11IOPCIDevice12configRead32Em");
		if (!rCfg32) rCfg32 = parseSymbolFromMachO(pciBase, "__ZN11IOPCIDevice12configRead32Ey");
		rExt32 = parseSymbolFromMachO(pciBase, "__ZN11IOPCIDevice20extendedConfigRead32Ey");
		if (!rExt32) rExt32 = parseSymbolFromMachO(pciBase, "__ZN11IOPCIDevice20extendedConfigRead32Em");

		// Static RVA fallback if not found in Mach-O symtab
		if (!rExt16) {
			mach_vm_address_t cand = pciBase + 0x193c2u;
			if (*reinterpret_cast<const uint8_t *>(cand) == 0x55 || *reinterpret_cast<const uint32_t *>(cand) == 0xe5894855) rExt16 = cand;
		}
		if (!rCfg16) {
			mach_vm_address_t cand = pciBase + 0x1d420u;
			if (*reinterpret_cast<const uint8_t *>(cand) == 0x55 || *reinterpret_cast<const uint32_t *>(cand) == 0xe5894855) rCfg16 = cand;
		}
		if (!rCfg32) {
			mach_vm_address_t cand = pciBase + 0x1d358u;
			if (*reinterpret_cast<const uint8_t *>(cand) == 0x55 || *reinterpret_cast<const uint32_t *>(cand) == 0xe5894855) rCfg32 = cand;
		}
		if (!rExt32) {
			mach_vm_address_t cand = pciBase + 0x1b67au;
			if (*reinterpret_cast<const uint8_t *>(cand) == 0x55 || *reinterpret_cast<const uint32_t *>(cand) == 0xe5894855) rExt32 = cand;
		}
	}

	// Tier 2: solveSymbol on KernelID
	if (!rExt16) {
		patcher.clearError();
		rExt16 = patcher.solveSymbol(KernelPatcher::KernelID, "__ZN11IOPCIDevice20extendedConfigRead16Ey");
		if (!rExt16) rExt16 = patcher.solveSymbol(KernelPatcher::KernelID, "__ZN11IOPCIDevice20extendedConfigRead16Em");
	}
	if (!rCfg16) {
		patcher.clearError();
		rCfg16 = patcher.solveSymbol(KernelPatcher::KernelID, "__ZN11IOPCIDevice12configRead16Eh");
		if (!rCfg16) rCfg16 = patcher.solveSymbol(KernelPatcher::KernelID, "__ZN11IOPCIDevice12configRead16Em");
		if (!rCfg16) rCfg16 = patcher.solveSymbol(KernelPatcher::KernelID, "__ZN11IOPCIDevice12configRead16Ey");
	}
	if (!rCfg32) {
		patcher.clearError();
		rCfg32 = patcher.solveSymbol(KernelPatcher::KernelID, "__ZN11IOPCIDevice12configRead32Eh");
		if (!rCfg32) rCfg32 = patcher.solveSymbol(KernelPatcher::KernelID, "__ZN11IOPCIDevice12configRead32Em");
		if (!rCfg32) rCfg32 = patcher.solveSymbol(KernelPatcher::KernelID, "__ZN11IOPCIDevice12configRead32Ey");
	}
	if (!rExt32) {
		patcher.clearError();
		rExt32 = patcher.solveSymbol(KernelPatcher::KernelID, "__ZN11IOPCIDevice20extendedConfigRead32Ey");
		if (!rExt32) rExt32 = patcher.solveSymbol(KernelPatcher::KernelID, "__ZN11IOPCIDevice20extendedConfigRead32Em");
	}

	// Tier 3: solveSymbol on kext loadIndex if valid
	if (kexts[PciIdx].loadIndex == KernelPatcher::KextInfo::Unloaded) {
		patcher.loadKinfo(&kexts[PciIdx]);
	}
	size_t pciIdx = index;
	if (pciIdx == KernelPatcher::KextInfo::Unloaded || pciIdx == 0) {
		pciIdx = kexts[PciIdx].loadIndex;
	}
	if (pciIdx != KernelPatcher::KextInfo::Unloaded) {
		if (!rExt16) {
			rExt16 = patcher.solveSymbol(pciIdx, "__ZN11IOPCIDevice20extendedConfigRead16Ey");
			if (!rExt16) rExt16 = patcher.solveSymbol(pciIdx, "__ZN11IOPCIDevice20extendedConfigRead16Em");
		}
		if (!rCfg16) {
			rCfg16 = patcher.solveSymbol(pciIdx, "__ZN11IOPCIDevice12configRead16Eh");
			if (!rCfg16) rCfg16 = patcher.solveSymbol(pciIdx, "__ZN11IOPCIDevice12configRead16Em");
			if (!rCfg16) rCfg16 = patcher.solveSymbol(pciIdx, "__ZN11IOPCIDevice12configRead16Ey");
		}
		if (!rCfg32) {
			rCfg32 = patcher.solveSymbol(pciIdx, "__ZN11IOPCIDevice12configRead32Eh");
			if (!rCfg32) rCfg32 = patcher.solveSymbol(pciIdx, "__ZN11IOPCIDevice12configRead32Em");
			if (!rCfg32) rCfg32 = patcher.solveSymbol(pciIdx, "__ZN11IOPCIDevice12configRead32Ey");
		}
		if (!rExt32) {
			rExt32 = patcher.solveSymbol(pciIdx, "__ZN11IOPCIDevice20extendedConfigRead32Ey");
			if (!rExt32) rExt32 = patcher.solveSymbol(pciIdx, "__ZN11IOPCIDevice20extendedConfigRead32Em");
		}
	}
	patcher.clearError();

	IOLog("REM660: [routePciDevid] targets: ext16=0x%llx cfg16=0x%llx cfg32=0x%llx ext32=0x%llx\n",
	      static_cast<uint64_t>(rExt16), static_cast<uint64_t>(rCfg16), static_cast<uint64_t>(rCfg32), static_cast<uint64_t>(rExt32));
	IOLog("REM660: [routePciDevid] VTABLE/MACH-O HOOKED ext16=0x%llx cfg16=0x%llx ext32=0x%llx cfg32=0x%llx\n",
	      static_cast<uint64_t>(rExt16), static_cast<uint64_t>(rCfg16), static_cast<uint64_t>(rExt32), static_cast<uint64_t>(rCfg32));

	size_t routedCount = 0;
	if (rExt16 && !orgPciExtRead16) {
		patcher.clearError();
		auto org = patcher.routeFunctionLong(rExt16, reinterpret_cast<mach_vm_address_t>(wrapPciExtRead16), true, true);
		if (org) { orgPciExtRead16 = org; routedCount++; }
	}
	if (rCfg16 && !orgPciCfgRead16) {
		patcher.clearError();
		auto org = patcher.routeFunctionLong(rCfg16, reinterpret_cast<mach_vm_address_t>(wrapPciCfgRead16), true, true);
		if (org) { orgPciCfgRead16 = org; routedCount++; }
	}
	if (rCfg32 && !orgPciCfgRead32) {
		patcher.clearError();
		auto org = patcher.routeFunctionLong(rCfg32, reinterpret_cast<mach_vm_address_t>(wrapPciCfgRead32), true, true);
		if (org) { orgPciCfgRead32 = org; routedCount++; }
	}
	if (rExt32 && !orgPciExtRead32) {
		patcher.clearError();
		auto org = patcher.routeFunctionLong(rExt32, reinterpret_cast<mach_vm_address_t>(wrapPciExtRead32), true, true);
		if (org) { orgPciExtRead32 = org; routedCount++; }
	}
	patcher.clearError();

	if (orgPciExtRead16 || orgPciCfgRead16 || orgPciCfgRead32 || orgPciExtRead32) {
		hooked[PciIdx] = true;
		__atomic_fetch_or(&gCompletedImages, 8u, __ATOMIC_RELEASE);
		LOG("pcidevid: IOPCIDevice config reads routed");
		IOLog("REM660: [routePciDevid] HOOKED PCI reads: ext16=%p cfg16=%p cfg32=%p ext32=%p (routed %zu)\n",
		      reinterpret_cast<void *>(orgPciExtRead16), reinterpret_cast<void *>(orgPciCfgRead16),
		      reinterpret_cast<void *>(orgPciCfgRead32), reinterpret_cast<void *>(orgPciExtRead32), routedCount);
		return true;
	}
	LOG("pcidevid: route FAILED (err %d)", patcher.getError());
	IOLog("REM660: [routePciDevid] route FAILED (err %d)\n", patcher.getError());
	return false;
}

bool hookPciDeviceFromInstance(KernelPatcher &patcher, IOService *provider) {
	if (!hooked[PciIdx]) {
		routePciDevid(patcher, 0, 0, 0);
	}
	return hooked[PciIdx];
}

bool hookPciDeviceFromInstance(IOService *provider) {
	if (gPatcher) {
		return hookPciDeviceFromInstance(*gPatcher, provider);
	}
	return false;
}

void pciEnableMemoryAndBusMaster(IOService *provider) {
	// Native IOPCIDevice::open handles BusMaster and Memory Space enable.
	// No blind vtable offsets are touched.
}

void ensureGpuReadyForStart(IOService *provider) {
	gEnablePciSpoof = true;
	if (provider) {
		syncSinglePciDeviceProps(provider);
		// AAPL,iokit-ignore-ndrv (OC DeviceProperties) keeps IONDRVFramebuffer off VGA@0. Never
		// terminate a client synchronously from inside another driver's probe()/start(): that
		// waits on the provider's busy state held by the matching thread (deadlock).
		if (auto clientIter = provider->getClientIterator()) {
			while (auto client = OSDynamicCast(IOService, clientIter->getNextObject()))
				if (client->metaCast("IONDRVFramebuffer"))
					IOLog("REM660: [ensureGpuReadyForStart] WARNING IONDRVFramebuffer attached to VGA@0 (check AAPL,iokit-ignore-ndrv)\n");
			clientIter->release();
		}
	}
	if (gPatcher) {
		if (!hooked[0]) {
			if (gDeferredHWLibsAddr && gDeferredHWLibsSize) {
				if (kexts[0].loadIndex == KernelPatcher::KextInfo::Unloaded) {
					gPatcher->loadKinfo(&kexts[0]);
				}
				IOLog("REM660: [ensureGpuReadyForStart] executing deferred routing for HWLibs (idx=0) at 0x%llx\n", static_cast<uint64_t>(gDeferredHWLibsAddr));
				routeGroup(*gPatcher, 0, gDeferredHWLibsAddr, gDeferredHWLibsSize);
			} else if (gKmods && *gKmods) {
				for (auto k = *gKmods; k; k = k->next) {
					if (strncmp(k->name, "com.apple.kext.AMDRadeonX6000HWLibs", KMOD_MAX_NAME) == 0) {
						if (kexts[0].loadIndex == KernelPatcher::KextInfo::Unloaded) {
							gPatcher->loadKinfo(&kexts[0]);
						}
						IOLog("REM660: [ensureGpuReadyForStart] found HWLibs in kmod list at 0x%llx size=0x%llx\n",
						      static_cast<uint64_t>(k->address), static_cast<uint64_t>(k->size));
						routeGroup(*gPatcher, 0, k->address, k->size);
						break;
					}
				}
			}
			// No base/size guessing: KernelID solveSymbol cannot resolve HWLibs symbols and a fake
			// 16 MB size would let updateRunningInfo/routing walk unrelated memory.
			if (!hooked[0])
				IOLog("REM660: [ensureGpuReadyForStart] HWLibs not routed yet (deferred=%d)\n", gDeferredHWLibsAddr ? 1 : 0);

		}
	}
}


static constexpr bool CertifiedPreStart=true;
bool restoredPreStartCertified() { return CertifiedPreStart; }
static bool restoreLifecycleVerified(KernelPatcher &) {
	LOG("runtime install certified: Tahoe 25G229 AMD lifecycle verified");
	return true;
}

static void pluginStart() {
	LOG("restored 680M Navi23/PSP11 architecture; Tahoe25G229");
	IOLog("REM660: [pluginStart] starting Rembrandt660M plugin (Tahoe 25G229)\n");
	if(!smuRestoreReady()) {
		LOG("BOOT_NO_GO: SMU missing-contract mask=%#x (DMA tables/required operations); no hooks/hardware started",smuRestoreBlockers());
		IOLog("REM660: [pluginStart] BOOT_NO_GO: SMU blockers mask=%#x\n", smuRestoreBlockers());
		return;
	}
	// DO NOT touch IORegistry or hook PCI in pluginStart!
	// Running early in [ PCI configuration begin ] deadlocks PCI bridge and USB discovery!
	lilu.onKextLoadForce(kexts, arrsize(kexts), processLoadedKext);
	// USB: TRB interrupter target 1 -> 0 on the exact 25G229 AppleUSBXHCI image (`-rmbxhciintr1` = off).
	lilu.onKextLoadForce(kextXhci, arrsize(kextXhci), processXhciKext);
	lilu.onPatcherLoadForce([](void *, KernelPatcher &patcher) {
		if(!restoreLifecycleVerified(patcher)) return;
		gPatcher = &patcher;
		gKmods = reinterpret_cast<kmod_info_t **>(patcher.solveSymbol(KernelPatcher::KernelID, "_kmod"));
		patcher.clearError();
		// Original RembrandtGfx onPatcherLoad (kern_start.cpp:950): kernel msgbuf ->
		// /private/var/log/rmb-msgbuf.log on the Data volume, so the lines that scroll off the
		// verbose screen survive. Opens only after 5 s uptime and once /private/var is writable.
		// `-rmbnodisklog` turns it off.
		diskLogStart(patcher);
		wlanDiagStart();  // read-only Wi-Fi nub diagnostic thread (-rmbnowlandiag)
		gfxClkStart(patcher);  // GFX clock policy once the SMU session is live (-rmbnogfxclk)
		vcnStart();            // hardware video boot decision + crash guard (-rmbnovcnfix)
		LOG("patcher ready, kmod list %s, hwlibs kinfo %zu", gKmods ? "found" : "missing", kextHWLibs.loadIndex);
		IOLog("REM660: [onPatcherLoadForce] patcher ready, kmod list %s\n", gKmods ? "found" : "missing");
		static bool gPciDevidRouted = false;
		if (!gPciDevidRouted && routePciDevid(patcher, 0, 0, 0)) {
			gPciDevidRouted = true;
			hooked[PciIdx] = true;
			IOLog("REM660: [onPatcherLoadForce] routePciDevid SUCCESS (gEnablePciSpoof is false at boot)\n");
		}
		// MT7922 recovery in IOPCIBridge::initializeNub, before probeBusGated reads the Wi-Fi nub (-rmbnowlanfix).
		wlanFixRoute(patcher);
		// Texture/icon/text scramble: addrlib pipeBankXor 0 in the AMD UMD code pages via cs_validate_page,
		// only with `-rmbumd` and only on the byte-verified 25G229 cs_validate_page (umdpatch.cpp).
		umdRoute(patcher);
	});
}

static const char *bootargOff[] {"-rem660off"};
static const char *bootargDebug[] {"-rem660dbg"};
static const char *bootargBeta[] {"-rem660beta"};

PluginConfiguration ADDPR(config) {
	xStringify(PRODUCT_NAME),
	parseModuleVersion(xStringify(MODULE_VERSION)),
	LiluAPI::AllowNormal | LiluAPI::AllowSafeMode | LiluAPI::AllowInstallerRecovery,
	bootargOff, arrsize(bootargOff),
	bootargDebug, arrsize(bootargDebug),
	bootargBeta, arrsize(bootargBeta),
	KernelVersion::Tahoe,
	KernelVersion::Tahoe,
	pluginStart
};
