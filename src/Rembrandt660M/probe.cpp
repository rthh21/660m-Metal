//
//  VRAM probes. The accelerator's VRAM heap comes up empty on the 680M ("Failed to allocate ...
//  0 free memory remaining"), so log what the framebuffer and AMDHWMemory derive VRAM from.
//

#include "rmb.hpp"
#include "tahoe_hooks.hpp"
#include "gpu_contract.hpp"
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSObject.h>
extern "C" unsigned int ml_phys_read_64(addr64_t paddr64);

volatile bool gKiqSleepSkip;
IOService *gAcceleratorService = nullptr;
volatile bool gAcceleratorStarted = false;
volatile bool gHwEnginesStarted = false;

void publishAccelerator(IOService *svc) {
	if (!svc || !restoredTargetProvider(svc->getProvider()))
		return;
	if (gAcceleratorService == svc && gAcceleratorStarted)
		return;
	if (gAcceleratorService) {
		gAcceleratorService->release();
		gAcceleratorService = nullptr;
	}
	svc->retain();
	gAcceleratorService = svc;
	gAcceleratorStarted = true;
}

void retractAccelerator(IOService *svc) {
	if (svc && gAcceleratorService && gAcceleratorService != svc)
		return;
	gAcceleratorStarted = false;
	if (gAcceleratorService) {
		gAcceleratorService->release();
		gAcceleratorService = nullptr;
	}
}

volatile bool gAccelProbeSeen = false;
// One accelerator per GPU: the first instance that enters start() owns the hardware. A second
// instance (from a provider rematch) re-ran TTL::initialize on half-initialized state (0xe00002bc).
static IOService *volatile gAccelStartOwner = nullptr;

namespace {
mach_vm_address_t orgAcceleratorProbe, orgAcceleratorStart;
static bool wrapAcceleratorStart(IOService *self, IOService *provider) {
	IOLog("REM660: [wrapAcceleratorStart] ENTER hooked=[%d,%d,%d,%d] self=%p provider=%p\n",
	      isKextHooked(0), isKextHooked(1), isKextHooked(2), isKextHooked(3), self, provider);
	if (!OSCompareAndSwapPtr(nullptr, self, reinterpret_cast<void * volatile *>(&gAccelStartOwner)) && gAccelStartOwner != self) {
		IOLog("REM660: [wrapAcceleratorStart] refused: accelerator %p already owns the GPU\n", gAccelStartOwner);
		return false;
	}
	rmbCheckpoint("AcceleratorStart ENTER");
	stageSet(StAccStart, 'E');
	ensureGpuReadyForStart(provider);

	// AMDRadeonX6000_AMDGraphicsAccelerator::start requires:
	// 1. Controller / Wrangler peer attached AND started (for configureDevice())
	// 2. AMDRadeonX6000HWServices peer attached AND started (for initializeExternalInterfaces())
	bool hasHWServices = false;
	bool hasController = gControllerStarted;
	if (provider) {
		OSIterator *clientIter = provider->getClientIterator();
		if (clientIter) {
			while (OSObject *obj = clientIter->getNextObject()) {
				IOService *client = OSDynamicCast(IOService, obj);
				if (client) {
					OSString *cat = OSDynamicCast(OSString, client->getProperty("IOMatchCategory"));
					const char *cname = client->getName();
					if (cat && (cat->isEqualTo("AMDRadeonX6000HWServices") || cat->isEqualTo("AMDRadeonHWServices"))) {
						hasHWServices = true;
					} else if (cname && strstr(cname, "HWServices")) {
						hasHWServices = true;
					}
					if ((cat && cat->isEqualTo("ATIFramebuffer")) || (cname && strstr(cname, "Controller"))) {
						if (gControllerStarted) hasController = true;
					}
					if (cat && cat->isEqualTo("IOFramebuffer") && cname && !strstr(cname, "IONDRV")) {
						if (gControllerStarted) hasController = true;
					}
				}
			}
			clientIter->release();
		}
	}

	IOLog("REM660: [wrapAcceleratorStart] peers status: Controller started=%d (hasHWServices=%d)\n",
	      gControllerStarted ? 1 : 0, hasHWServices ? 1 : 0);

	using StartFn = bool (*)(IOService *, IOService *);
	bool ok = false;
	if (orgAcceleratorStart) {
		ok = reinterpret_cast<StartFn>(orgAcceleratorStart)(self, provider);
	}
	stageSet(StAccStart, ok ? '1' : '0');
	IOLog("REM660: [wrapAcceleratorStart] EXIT ret=%d\n", ok ? 1 : 0);
	rmbCheckpoint("AcceleratorStart EXIT");
	if (ok) {
		publishAccelerator(self);
	}
	return ok;
}

IOService *wrapAcceleratorProbe(IOService *self, IOService *provider, int32_t *score) {
	if (gAccelStartOwner && gAccelStartOwner != self) {
		IOLog("REM660: [wrapAcceleratorProbe] refused duplicate instance %p (owner %p)\n", self, gAccelStartOwner);
		return nullptr;
	}
	stageSet(StAccProbe, 'E');
	ensureGpuReadyForStart(provider);
#if defined(KERNEL)
	IOLog("REM660: [wrapAcceleratorProbe] enter hooked=[%d,%d,%d,%d]: self=%p provider=%p score=%d ready=%d org=%llx target=%d\n",
	      isKextHooked(0), isKextHooked(1), isKextHooked(2), isKextHooked(3),
	      self, provider, score ? *score : 0, restoredBootstrapReady() ? 1 : 0,
	      static_cast<unsigned long long>(orgAcceleratorProbe), restoredTargetProvider(provider) ? 1 : 0);
	kprintf("REM660: wrapAcceleratorProbe enter: self=%p provider=%p score=%d\n", self, provider, score ? *score : 0);
#endif
	if(!restoredBootstrapReady()||!orgAcceleratorProbe||!restoredTargetProvider(provider)) {
		stageSet(StAccProbe, 'g');  // gated off by our own readiness checks
		IOLog("REM660: [wrapAcceleratorProbe] GATED ready=%d org=%d target=%d\n", restoredBootstrapReady() ? 1 : 0,
		      orgAcceleratorProbe ? 1 : 0, restoredTargetProvider(provider) ? 1 : 0);
		return nullptr;
	}
	using Probe = IOService *(*)(IOService *, IOService *, int32_t *);
	auto res = reinterpret_cast<Probe>(orgAcceleratorProbe)(self, provider, score);
	stageSet(StAccProbe, res ? '1' : '0');
	if (res && score) {
		*score = 65051;
	}
#if defined(KERNEL)
	kprintf("REM660: wrapAcceleratorProbe result=%p\n", res);
	IOLog("REM660: [wrapAcceleratorProbe] exit: native probe returned %p, score=%d\n", res, score ? *score : 0);
#endif
	if (res) {
		gAccelProbeSeen = true;
		publishAccelerator(res);
	}
	return res;
}

mach_vm_address_t orgPopulateMemorySize, orgHWMemInit, orgInitVRAMInfo, orgEnableAllocations,
	orgSetReservedVRAMOffset, orgUpdateHWInfoClocks, orgShouldAllocScanoutDcc, orgSdmaReset, orgResetHardware, orgPageOnOff, orgDoStretch, orgDoClear, orgDoExpand,
	orgAccelSubmitCommands;

uint64_t q(uint64_t obj, uint32_t off) { return *reinterpret_cast<uint64_t *>(obj + off); }

static bool vramCarve32() { return checkKernelArgument("-rmbvramcarve32"); }

// AmdAsicInfoNavi2: +0x120 RCC_CONFIG_MEMSIZE << 20, +0x128 CONFIG_APER_SIZE << 20,
// +0x12c CONFIG_REG_APER_SIZE.
uint32_t wrapPopulateMemorySize(void *object) {
 auto self=reinterpret_cast<uint64_t>(object);
	rmbStep("populateMemorySize ENTER (hwReadReg32 0xde3 0xde7 0xde8)");
	auto r = reinterpret_cast<uint32_t (*)(void *)>(orgPopulateMemorySize)(object);
	rmbStep("populateMemorySize EXIT");
	// The former unconditional -32 MB had no evidence (AGENTS: no arbitrary VRAM shrink): the Lenovo VBIOS
	// vram_usagebyfirmware v2.1 (@0x534) reserves 0 KB, the GPD reference does not shrink, and Linux's
	// TMR (linux_amdgpu_dmesg.txt:79) is a driver allocation that HWLibs _psp_tmr_init makes itself.
	if (vramCarve32() && *reinterpret_cast<uint64_t *>(self + 0x120) > 0x2000000ULL) {
		*reinterpret_cast<uint64_t *>(self + 0x120) -= 0x2000000ULL;
		LOG("populateMemorySize: -rmbvramcarve32: memsize reduced by 32MB");
	}
	LOG("populateMemorySize: memsize %llx aper %x reg aper %x; fb loc base %llx top %llx offset %llx",
		q(self, 0x120), *reinterpret_cast<uint32_t *>(self + 0x128), *reinterpret_cast<uint32_t *>(self + 0x12c),
		q(self, 0x110), q(self, 0x118), q(self, 0x120));
	return r;
}

void dumpHWMem(const char *when, uint64_t self) {
	LOG("hwmem %s: +28 %llx +38 %llx +40 %llx +48 %llx +50 %llx +58 %llx +60 %llx +68 %llx +70 %llx", when,
		q(self, 0x28), q(self, 0x38), q(self, 0x40), q(self, 0x48), q(self, 0x50), q(self, 0x58),
		q(self, 0x60), q(self, 0x68), q(self, 0x70));
}

uint64_t wrapHWMemInit(uint64_t self, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
	auto r = reinterpret_cast<Fn6>(orgHWMemInit)(self, b, c, d, e, f);
	dumpHWMem(r & 0xFF ? "init ok" : "init FAILED", self);
	return r;
}

uint64_t wrapInitVRAMInfo(uint64_t self, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
	auto r = orgInitVRAMInfo ? reinterpret_cast<Fn6>(orgInitVRAMInfo)(self, b, c, d, e, f) : 0;
	if (vramCarve32() && *reinterpret_cast<uint64_t *>(self + 0x40) > 0x2000000ULL) {
		*reinterpret_cast<uint64_t *>(self + 0x40) -= 0x2000000ULL;
		LOG("initVRAMInfo: -rmbvramcarve32: VRAM size reduced by 32MB to %llx", *reinterpret_cast<uint64_t *>(self + 0x40));
	}
	dumpHWMem(r & 0xFF ? "initVRAMInfo ok" : "initVRAMInfo FAILED", self);
	return r;
}

uint64_t wrapEnableAllocations(uint64_t self, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
	dumpHWMem("enableAllocations", self);
	auto r = reinterpret_cast<Fn6>(orgEnableAllocations)(self, b, c, d, e, f);
	LOG("enableAllocations = %llx", r & 0xFF);
	vmWatchStart();
	return r;
}

uint64_t wrapSetReservedVRAMOffset(uint64_t self, uint64_t off, uint64_t size, uint64_t d, uint64_t e, uint64_t f) {
	LOG("setReservedVRAMOffset(%llx, %llx)", off, size);
	return reinterpret_cast<Fn6>(orgSetReservedVRAMOffset)(self, off, size, d, e, f);
}

// AMDHardware::updateHWInfoClocks asks PowerPlay (stubbed) for current clocks and stores
// sysClk/memClk (10 kHz units) at +0x198/+0x1a0 -> both 0. AMDAccelDevice::getHardwareInfo
// then rejects every userspace HW-info query (0xe00002bc), so the GL driver's compute device
// stays NULL and anything using CoreImage/CGL crashes (Terminal, iconservicesagent, Dock).
uint64_t wrapUpdateHWInfoClocks(uint64_t self, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
	auto r = reinterpret_cast<Fn6>(orgUpdateHWInfoClocks)(self, b, c, d, e, f);
	auto sys = reinterpret_cast<uint64_t *>(self + 0x198), mem = reinterpret_cast<uint64_t *>(self + 0x1a0);
	// 25G229 0x7015c/0x7017b store the PowerPlay reply at +0x198/+0x1a0 (10 kHz units). Only zero
	// fields are filled; these are the values reported to user-space HW-info, nothing is programmed.
	// 1900 MHz = AMD's published Radeon 660M max GPU clock (not read from this PMFW); 1600 MHz memClk
	// as in the original RembrandtGfx.
	if (!*sys || !*mem) {
		LOG("hwclk: PowerPlay gave sysClk %llu memClk %llu -> reported fallback 1900/1600 MHz", *sys, *mem);
		if (!*sys) *sys = 190000;
		if (!*mem) *mem = 160000;
		r = 1;
	}
	return r;
}

// Channel restarts (e.g. a hung SDMA0_PAGE) reset the SDMA engine, which may put
// SDMA0_GB_ADDR_CONFIG back to its 0x444 reset value; re-sync it after every reset.
uint64_t wrapSdmaReset(uint64_t self, uint64_t ring, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
	LOG("sdma-reset: ring type %llu", ring);
	auto r = reinterpret_cast<Fn6>(orgSdmaReset)(self, ring, c, d, e, f);
	syncSdmaAddrConfig("after SDMA reset");
	return r;
}

uint64_t wrapResetHardware(uint64_t self, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
	LOG("hw-reset: resetHardware(%llx)", b);
	auto r = reinterpret_cast<Fn6>(orgResetHardware)(self, b, c, d, e, f);
	syncSdmaAddrConfig("after resetHardware");
	return r;
}

// Managed-texture uploads (CPU copy -> GPU) go through pageOnOffBuffer as a UBM blit. Dump the
// src/dst _UBM_SURFINFO (0x2c8 bytes; +0x5c width, +0x60 height) of square 64/96 px surfaces to
// compare a working (no DCC) and a stale (DCC-able RT) upload.
using FnPageOnOff = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
static void dumpSurf(const char *tag, uint64_t si) {
	auto d = reinterpret_cast<const uint32_t *>(si);
	for (uint32_t i = 0; i < 0x2c8 / 4; i += 8)
		LOG("pageon: %s +%03x: %08x %08x %08x %08x %08x %08x %08x %08x", tag, i * 4, d[i], d[i + 1], d[i + 2], d[i + 3],
			i + 4 < 0x2c8 / 4 ? d[i + 4] : 0, i + 5 < 0x2c8 / 4 ? d[i + 5] : 0, i + 6 < 0x2c8 / 4 ? d[i + 6] : 0,
			i + 7 < 0x2c8 / 4 ? d[i + 7] : 0);
}

static thread_t gPageOnThread;

// UBM ops issued from inside a dumped pageOnOffBuffer call (same thread).
template <uint64_t *Org, char Op>
uint64_t wrapUbmOp(uint64_t self, uint64_t info, uint64_t opts, uint64_t d, uint64_t e, uint64_t f) {
	auto r = reinterpret_cast<Fn6>(*Org)(self, info, opts, d, e, f);
	if (gPageOnThread == current_thread()) {
		auto o = reinterpret_cast<const uint32_t *>(opts);
		LOG("pageon:  ubm %c -> %llx, opts %08x %08x %08x %08x", Op, r & 0xffffffff, o ? o[0] : 0, o ? o[1] : 0,
			o ? o[2] : 0, o ? o[3] : 0);
	}
	return r;
}

uint64_t wrapPageOnOff(uint64_t self, uint64_t src, uint64_t dst, uint64_t srcMap, uint64_t dstMap, uint64_t dir,
	uint64_t srcRect, uint64_t dstRect) {
	static uint32_t dumps;
	uint32_t w = *reinterpret_cast<uint32_t *>(dst + 0x5c), h = *reinterpret_cast<uint32_t *>(dst + 0x60);
	bool dump = w == h && (w == 64 || w == 72 || w == 96) && dumps < 12;
	if (dump)
		gPageOnThread = current_thread();
	auto r = reinterpret_cast<FnPageOnOff>(orgPageOnOff)(self, src, dst, srcMap, dstMap, dir, srcRect, dstRect);
	gPageOnThread = nullptr;
	if (dump) {
		dumps++;
		LOG("pageon: %ux%u dir %llu -> %llu, blt flags %x", w, h, dir, r & 0xff,
			*reinterpret_cast<uint32_t *>(q(self, 0x18) + 0xdd8));
		dumpSurf("src", src);
		dumpSurf("dst", dst);
	}
	return r;
}

// Scanout-capable surfaces (window layers, decoded images, WebGL canvases) get DCC when the
// accelerator's GPUDCCDisplayable flag is set. Our DCN path strips DCC from HUBP (its meta
// addressing is wrong on 3.1.2), and CPU/scanout readers then see raw compressed blocks:
// garbled About-This-Mac image / Launchpad icons, WebGL trails, white flashes.
// -rmbscandcc keeps Apple's behaviour.
bool gKeepScanoutDcc;
uint64_t wrapShouldAllocScanoutDcc(uint64_t self, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
	if (gKeepScanoutDcc) return reinterpret_cast<Fn6>(orgShouldAllocScanoutDcc)(self, b, c, d, e, f);
	static int n;
	if (n++ < 3) LOG("scandcc: shouldAllocScanoutDcc(%llx, %llx, %llx, %llx) -> 0", b, c, d, e);
	return 0;
}

#define PROBE_TRACE(name)                                                                     \
	mach_vm_address_t org_##name;                                                            \
	uint64_t wrap_##name(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) { \
		LOG(">> " #name "(%llx, %llx)", b, c);                                                                    \
		auto r = reinterpret_cast<Fn6>(org_##name)(a, b, c, d, e, f);                       \
		LOG("<< " #name " = %llx", r);                                                       \
		return r;                                                                            \
	}

PROBE_TRACE(AccelPowerUpHW)
// AMDGFX10Hardware::initHWInfo (run before powerUp) builds the chip description user space gets:
// +0x40, +0x64 SE count, +0x68, +0x6c SH per SE, +0x70 u32 CU masks [se][2], +0xa8, +0xf8 u64
// masks, +0x158.. copies. Dump it to see what the Metal driver is told about Rembrandt.
mach_vm_address_t org_Navi23HwPowerUp;
uint64_t wrap_Navi23HwPowerUp(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
	LOG(">> Navi23HwPowerUp");
	auto u = [&](uint32_t off) { return *reinterpret_cast<uint32_t *>(a + off); };
	for (uint32_t off = 0x40; rmbVerbose() && off < 0x1f0; off += 0x20)
		LOG("hwinfo +%03x: %08x %08x %08x %08x %08x %08x %08x %08x", off, u(off), u(off + 4), u(off + 8), u(off + 12),
			u(off + 16), u(off + 20), u(off + 24), u(off + 28));
	auto r = reinterpret_cast<Fn6>(org_Navi23HwPowerUp)(a, b, c, d, e, f);
	LOG("<< Navi23HwPowerUp = %llx", r);
	return r;
}
PROBE_TRACE(HWMemSetVirtualSpace)
PROBE_TRACE(PM4DoStart)
mach_vm_address_t org_PM4InitComputeMQD;
uint8_t wrap_PM4InitComputeMQD(void *self,uint32_t ring) {
 if(!org_PM4InitComputeMQD) return 0;
 return reinterpret_cast<rem660::abi25::ComputeMqd>(org_PM4InitComputeMQD)(self,ring);
}
mach_vm_address_t org_PM4InitGraphicsMQD;
uint8_t wrap_PM4InitGraphicsMQD(void *self) {
 if(!org_PM4InitGraphicsMQD) return 0;
 return reinterpret_cast<rem660::abi25::GraphicsMqd>(org_PM4InitGraphicsMQD)(self);
}

const char *className(uint64_t obj) {
	return obj ? reinterpret_cast<OSObject *>(obj)->getMetaClass()->getClassName() : "(null)";
}

// GC register dump (gc_10_3_0 offsets; GC seg0 0x1260, seg1 0xA000 on both chips).
constexpr uint32_t GC0 = 0x1260, GC1 = 0xA000;
uint32_t gc(uint32_t off) { return mmioRead(GC0 + off); }

void dumpHQD(uint32_t me, uint32_t pipe, uint32_t queue) {
	auto old = gc(0x0dc2);
	mmioWrite(GC0 + 0x0dc2, (queue << 8) | (me << 2) | pipe);  // GRBM_GFX_CNTL
	LOG("hqd me%u pipe%u q%u: active %x vmid %x base %x_%08x rptr %x wptr %x_%x ctl %x db %x mqd %x deq %x",
		me, pipe, queue, gc(0x1fab), gc(0x1fac), gc(0x1fb2), gc(0x1fb1), gc(0x1fb3), gc(0x1fe0), gc(0x1fdf),
		gc(0x1fba), gc(0x1fb8), gc(0x1fa9), gc(0x1fc1));
	if (me == 2) {
		mmioWrite(GC0 + 0x0dc2, (queue << 8) | (me << 2) | pipe);
		LOG("hqd me%u pipe%u q%u: eop %x_%x ctl %x mqd hi %x ctl %x rptr_rep %x_%x wptr_poll %x_%x persist %x ib %x prio %x quantum %x hq_st0 %x hq_ctl0 %x err %x eop_wptr %x",
			me, pipe, queue, gc(0x1fcf), gc(0x1fce), gc(0x1fd0), gc(0x1faa), gc(0x1fcb), gc(0x1fb5), gc(0x1fb4),
			gc(0x1fb7), gc(0x1fb6), gc(0x1fad), gc(0x1fbe), gc(0x1fae), gc(0x1fb0), gc(0x1fc9), gc(0x1fca),
			gc(0x1fdc), gc(0x1fdd));
		mmioWrite(GC0 + 0x0dc2, old);
		LOG("cp: PQ_WPTR_POLL_CNTL %x PQ_STATUS %x ME2P1_INT %x INT_STATUS %x ECC %x CPC_STALL %x CPF_STALL %x CPF_BUSY %x hdr dump me1 %x me2 %x",
			gc(0x1e23), gc(0x1e58), gc(0x1e32), gc(0x1dea), gc(0x1e1a), gc(0x0e26), gc(0x0e29), gc(0x0e28), gc(0x0e2e), gc(0x0e2f));
	}
}

void dumpCP(const char *when) {
	if (!mmioAvailable())
		return;
	LOG("cp %s: GRBM_STATUS %x STATUS2 %x CP_STAT %x CPC_STATUS %x CPC_BUSY %x CPF_STATUS %x MEC_CNTL %x", when,
		gc(0x0da4), gc(0x0da2), gc(0x0f40), gc(0x0e24), gc(0x0e25), gc(0x0e27), gc(0x0f55));
	LOG("cp %s: MEC1 IP %x MEC2 IP %x DB range %x-%x RLC_CNTL %x RLC_STAT %x RLC_CP_SCHED %x CPC_IC_BASE_LO %x", when,
		gc(0x0f48), gc(0x0f49), gc(0x1dfc), gc(0x1dfd), mmioRead(GC1 + 0x4c00), mmioRead(GC1 + 0x4c04),
		mmioRead(GC1 + 0x4ca1), mmioRead(GC1 + 0x584c));
	LOG("gcvm %s: FB %x-%x off %x AGP %x %x-%x SYSAP %x-%x def %x_%x L1TLB %x L2 %x", when,
		gc(0x16fc), gc(0x16fd), gc(0x16e7), gc(0x1700), gc(0x16ff), gc(0x16fe), gc(0x1701), gc(0x1702),
		gc(0x16e9), gc(0x16e8), gc(0x1703), gc(0x15bc));
	LOG("gcvm %s: ctx0 cntl %x pt %x_%08x start %x_%08x end %x_%08x fault status %x addr %x_%08x cntl %x utcl1 cpc %x cpf %x",
		when, gc(0x15fc), gc(0x1668), gc(0x1667), gc(0x1688), gc(0x1687), gc(0x16a8), gc(0x16a7),
		gc(0x15c8), gc(0x15ca), gc(0x15c9), gc(0x15c4), gc(0x1f55), gc(0x1f56));
	// GART (VMID0, flat table): PTE for the faulting page. PT base is the physical address;
	// minus FB_OFFSET << 24 gives the VRAM offset.
	uint64_t ptBase = ((uint64_t)gc(0x1668) << 32 | gc(0x1667)) & ~0xFFFULL;
	uint64_t fbPhys = (uint64_t)(gc(0x16e7) & 0xFFFFFF) << 24;
	uint64_t start = (uint64_t)gc(0x1688) << 32 | gc(0x1687);
	uint64_t fault = (uint64_t)gc(0x15ca) << 32 | gc(0x15c9);
	if (ptBase >= fbPhys && fault >= start) {
		uint64_t pt = ptBase - fbPhys, idx = fault - start;
		LOG("gart %s: pt vram %llx, fault page idx %llx: pte %llx (prev %llx next %llx), pte[0] %llx", when, pt, idx,
			vramRead64(pt + idx * 8), vramRead64(pt + idx * 8 - 8), vramRead64(pt + idx * 8 + 8), vramRead64(pt));
	}
	dumpHQD(1, 0, 0);
	dumpHQD(2, 1, 0);
}

mach_vm_address_t orgSubmitKIQFrame;
uint8_t wrapSubmitKIQFrame(void *self) {
 if(!orgSubmitKIQFrame) return 0;
 auto result=reinterpret_cast<rem660::abi25::KiqSubmit>(orgSubmitKIQFrame)(self);
 LOG("KIQ submit/wait native result=%u",result);
 return result; // no register writes/queue reset after a failed fence wait
}

// The APU only gets a light reset between VM runs (and from the host's amdgpu before that), so HQDs
// mapped earlier are still active and the MEC firmware may be wedged on an old KIQ packet; mapping
// queues again then hangs the KIQ. Halt the CP (as Linux cp_compute_enable/cp_gfx_enable(false)),
// deactivate every HQD by register, and unhalt so the firmware restarts from a clean state.
void cpResetQueues() {
	constexpr uint32_t MecHalt = (1u << 30) | (1u << 28);              // MEC_ME1_HALT | MEC_ME2_HALT
	constexpr uint32_t MeHalt = (1u << 28) | (1u << 26) | (1u << 24);  // ME | PFP | CE
	auto old = gc(0x0dc2);
	LOG("cp reset: MEC_CNTL %x ME_CNTL %x CP_STAT %x", gc(0x0f55), gc(0x0f56), gc(0x0f40));
	mmioWrite(GC0 + 0x0f55, MecHalt);
	mmioWrite(GC0 + 0x0f56, gc(0x0f56) | MeHalt);
	IODelay(100);
	uint32_t n = 0;
	for (uint32_t me = 1; me <= 2; me++)
		for (uint32_t pipe = 0; pipe < 4; pipe++)
			for (uint32_t queue = 0; queue < 8; queue++) {
				mmioWrite(GC0 + 0x0dc2, (queue << 8) | (me << 2) | pipe);
				n += gc(0x1fab) & 1;
				mmioWrite(GC0 + 0x1fb8, 0);  // doorbell control
				mmioWrite(GC0 + 0x1fab, 0);  // CP_HQD_ACTIVE
				mmioWrite(GC0 + 0x1fc1, 0);  // DEQUEUE_REQUEST
				mmioWrite(GC0 + 0x1fb3, 0);  // RPTR
				mmioWrite(GC0 + 0x1fdf, 0);  // WPTR
				mmioWrite(GC0 + 0x1fe0, 0);
				mmioWrite(GC0 + 0x1fdc, 0);  // CP_HQD_ERROR
			}
	for (uint32_t pipe = 0; pipe < 2; pipe++) {
		mmioWrite(GC0 + 0x0dc2, pipe);
		n += gc(0x1e80) & 1;
		mmioWrite(GC0 + 0x1e80, 0);  // CP_GFX_HQD_ACTIVE
		mmioWrite(GC0 + 0x1e93, 0);
	}
	mmioWrite(GC0 + 0x0dc2, old);
	mmioWrite(GC0 + 0x0f56, gc(0x0f56) & ~MeHalt);
	mmioWrite(GC0 + 0x0f55, 0);
	IODelay(1000);
	LOG("cp reset: %u active queues deactivated; MEC_CNTL %x ME_CNTL %x CP_STAT %x MEC1 IP %x MEC2 IP %x",
		n, gc(0x0f55), gc(0x0f56), gc(0x0f40), gc(0x0f48), gc(0x0f49));
}

// Logging copy of AMDGFX10KIQHWChannel::startKIQ(gpuAddr, gpuAddr2, spec, out).
mach_vm_address_t gAccelBase,orgAdjustVRAMAddress;
uint32_t wrapStartKIQ(void *object, uint64_t a1, uint64_t a2, const void *specification, uint32_t *formalOut) {
	REM_IOLOG("[wrapStartKIQ] entering\n");
	if(!object||!specification) return 0xe00002c2;
	auto self=reinterpret_cast<uint64_t>(object);auto spec=reinterpret_cast<uint64_t>(specification);
	(void)formalOut; // exact native25G2290x8d4e6 also does not consume incoming R8
	auto ring = q(self, 0x30);
	if(!ring) return 0xe00002c2;
	auto in=rem660::gpu::makeKiqInput(*reinterpret_cast<uint32_t *>(ring+0x84),
	    q(ring,0x48),q(ring,0xb8),q(ring,0xd0),a1,a2);
	rem660::gpu::KiqOutput out{};
	gKiqSleepSkip = false;
	// No cross-VM GART restore or CP reset is issued on a Lenovo cold boot.
	auto owner = q(self, 0x20);
	if(!owner) return 0xe00002c2;
	auto svc = reinterpret_cast<uint64_t (*)(uint64_t)>((*reinterpret_cast<uint64_t **>(owner))[0x2c0 / 8])(owner);
	if(!svc) return 0xe00002c2;
	auto fn = (*reinterpret_cast<uint64_t **>(svc))[0x120 / 8];
	if(!fn) return 0xe00002c2;
	auto r = static_cast<uint32_t>(reinterpret_cast<uint64_t (*)(uint64_t, uint64_t, uint64_t, void *, void *)>(fn)(svc, 8, 0, &in, &out));
	auto sp = reinterpret_cast<uint32_t *>(spec);
	LOG("startKIQ: r %x out %x %x %x (spec %x %x %x) published %llx", r, out.spec[0], out.spec[1], out.spec[2], sp[0], sp[1], sp[2], out.published);
	REM_IOLOG("[wrapStartKIQ] r=%x out=[%x,%x,%x] published=%llx\n", r, out.spec[0], out.spec[1], out.spec[2], out.published);
	if (r)
		return r;
	if (out.spec[0] != sp[0] || out.spec[1] != sp[1] || out.spec[2] != sp[2])
		r = 0xe00002bc;
	__builtin_memcpy(reinterpret_cast<void *>(ring+0xc0),&out.published,sizeof(out.published));
	{
		auto old = gc(0x0dc2);
		mmioWrite(GC0 + 0x0dc2, (sp[2] << 8) | (sp[0] << 2) | sp[1]);
		auto err = gc(0x1fdc);
		mmioWrite(GC0 + 0x1fdc, 0);
		mmioWrite(GC0 + 0x0dc2, old);
		REM_IOLOG("[wrapStartKIQ] HQD error cleared (was %x)\n", err);
	}
	return r;
}
PROBE_TRACE(KIQMapQueues)
PROBE_TRACE(PM4WaitForActive)

// AMDHardware status word (this+0x18 -> vfunc 0x178, +0x8): bit 0x100 = engines powered,
// 0x200 = engines started. Kept as the originals set them.
void setStatusBit(uint64_t hw, uint16_t bit, bool on) {
	auto obj = *reinterpret_cast<uint64_t *>(hw + 0x18);
	auto fn = reinterpret_cast<uint64_t (*)(uint64_t)>((*reinterpret_cast<uint64_t **>(obj))[0x178 / 8]);
	auto w = reinterpret_cast<uint16_t *>(fn(obj) + 0x8);
	*w = on ? (*w | bit) : (*w & ~bit);
}

uint64_t callSlot(uint64_t obj, uint32_t slot) {
	return reinterpret_cast<uint64_t (*)(uint64_t)>((*reinterpret_cast<uint64_t **>(obj))[slot / 8])(obj);
}

// AMDHardware::powerUpHWEngines: engines[0..10] at +0x3b0, each ->powerUp (slot 0x138).
uint8_t wrapPowerUpHWEngines(void *object) {
	IOLog("REM660: [wrapPowerUpHWEngines] ENTER hw=%p\n", object);
	// 2026-11-09: both boots of rmb-msgbuf.log SHA 870aa1ea froze <0.5 s after AcceleratorStart EXIT, before
	// any engine line reached the disk. Write the log out synchronously around engine bring-up (≤300 ms
	// each, thread context) so the next log shows the last step that completed.
	diskLogSync(300);
	smuPreGpuCommandSequence();
	applyGcGolden();
	auto hw=reinterpret_cast<uint64_t>(object);
	bool ok = true;
	for (uint32_t i = 0; i < 11 && ok; i++) {
		auto e = *reinterpret_cast<uint64_t *>(hw + 0x3b0 + i * 8);
		if (!e)
			continue;
		ok = callSlot(e, 0x138) & 0xFF;
		LOG("engine %u %s powerUp = %d", i, className(e), ok);
		IOLog("REM660: [wrapPowerUpHWEngines] engine %u %s powerUp = %d\n", i, className(e), ok);
	}
	setStatusBit(hw, 0x100, ok);
	IOLog("REM660: [wrapPowerUpHWEngines] EXIT status=0x100 ok=%d\n", ok);
	diskLogSync(300);
	if (ok) {
		gAcceleratorStarted = true;
	}
	return ok;
}

// AMDHardware::startHWEngines: 2 + 2 * byte[+0xc2] engines from +0x3b8, each ->start (0x148).
uint8_t wrapStartHWEngines(void *object) {
	IOLog("REM660: [wrapStartHWEngines] ENTER hw=%p\n", object);
	diskLogSync(300);
	smuPreGpuCommandSequence();
	applyGcGolden();
	auto hw=reinterpret_cast<uint64_t>(object);
	uint32_t n = 2 + 2 * (*reinterpret_cast<uint8_t *>(hw + 0xc2) & 1u);
	bool ok = true;
	syncSdmaAddrConfig("before engine start");
	for (uint32_t i = 0; i < n && ok; i++) {
		if(i==1) continue; // physical SDMA1 is absent; never call it then turn failure into success
		auto e = *reinterpret_cast<uint64_t *>(hw + 0x3b8 + i * 8);
		ok = e && (callSlot(e, 0x148) & 0xFF);
		syncSdmaAddrConfig("after engine start");
		LOG("engine slot %u %s start = %d", i + 1, className(e), ok);
		IOLog("REM660: [wrapStartHWEngines] engine slot %u %s start = %d\n", i + 1, className(e), ok);
		diskLogSync(300);  // engine start includes the KIQ/HQD bring-up
	}
	setStatusBit(hw, 0x200, ok);
	IOLog("REM660: [wrapStartHWEngines] EXIT status=0x200 ok=%d\n", ok);
	diskLogSync(300);
	gHwEnginesStarted = ok;
	if (ok) {
		gAcceleratorStarted = true;
	}
	return ok;
}

// AMDRTHardware::getHWChannel(channelType, priority, index): for SDMA (type 2) it picks engine
// ((caps+0x92 ? 3 : 1) & index) + 1 and ring caps+0x8f, then AMDHardware::getHWChannel(engine, ring).
// Low-priority work (e.g. the VM page-table channel) lands on engine 2 = SDMA1, which Rembrandt
// doesn't have, so its jobs never complete. Same selection, clamped to SDMA0.
mach_vm_address_t orgRTGetHWChannel, gGetHWChannelEngineRing;
uint64_t wrapRTGetHWChannel(void *object, uint32_t type, uint32_t prio, uint32_t index) {
 auto hw=reinterpret_cast<uint64_t>(object);
	if ((type & 0xFFFFFFFF) != 2 || !gGetHWChannelEngineRing)
		return reinterpret_cast<uint64_t (*)(void *,uint32_t,uint32_t,uint32_t)>(orgRTGetHWChannel)(object,type,prio,index);
	auto caps = reinterpret_cast<uint8_t *>(callSlot(hw, 0x158));
	uint32_t p = prio & 0xFFFFFFFF, idx = index & 0xFFFFFFFF, ring = 0;
	if (p == 1)
		ring = caps[0x8f] & 1u;
	else if (p == 0)
		idx = 1, ring = caps[0x8f] & 1u;
	uint32_t engine = (((caps[0x92] & 1u) * 2 + 1) & idx) + 1;
	if (engine != 1) {
		static uint32_t logged;
		if (logged++ < 8)
			LOG("getHWChannel: SDMA prio %u index %u -> engine %u ring %u, using SDMA0", p, idx, engine, ring);
		engine = 1;
	}
	return reinterpret_cast<uint64_t (*)(void *,uint32_t,uint32_t)>(gGetHWChannelEngineRing)(object,engine,ring);
}

// AMDHWMemory::adjustVRAMAddress(mc): MC address -> GPU physical (Linux amdgpu_gmc_vram_mc2pa), used
// for page-table bases/PDEs. hwmem +0x50 = FB MC base, +0x58 = FB physical offset, +0x40 = VRAM size,
// +0x1b8 = segment count, +0x60 = base - offset. The original range-checks against +0x60 instead of
// the MC base, which only coincides on dGPUs (offset 0); with the UMA carve-out MC addresses fall
// outside and pass through unconverted (VMID page-table base 0xF4_xxxxxxxx).
uint64_t wrapAdjustVRAMAddress(void *object, uint64_t mc) {
 auto hwmem=reinterpret_cast<uint64_t>(object);
	uint64_t fb = q(hwmem, 0x50), phys = q(hwmem, 0x58);
	uint64_t vramSize = q(hwmem, 0x40);
	uint64_t r=mc;
	if(!rem660::gpu::translateUma(fb,phys,vramSize,*reinterpret_cast<uint32_t *>(hwmem+0x1b8),mc,r)) {
	 if(orgAdjustVRAMAddress) return reinterpret_cast<uint64_t (*)(void *,uint64_t)>(orgAdjustVRAMAddress)(object,mc);
	 return mc;
	}
	static uint32_t logged;
	if (logged < 6 && r != mc) {
		logged++;
		LOG("adjustVRAMAddress(%llx) -> %llx", mc, r);
	}
	return r;
}

bool route(KernelPatcher &patcher,size_t index,mach_vm_address_t address,size_t size,
 KernelPatcher::RouteRequest *reqs,size_t count) {
 return restoredRouteGroup(patcher,index,reqs,count,address,size);
}

}  // namespace

void probeRouteFramebuffer(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
	KernelPatcher::RouteRequest reqs[] {
		{"__ZN31AMDRadeonX6000_AmdAsicInfoNavi218populateMemorySizeEv", wrapPopulateMemorySize, orgPopulateMemorySize},
	};
	if(!route(patcher,index,address,size,reqs,arrsize(reqs))) {
		routeStaticRequests(patcher, 1, reqs, arrsize(reqs), address, size);
	}
}

// -rmbnosubmit: bare-metal diagnostic. Drop every user (Metal/WindowServer) command buffer so the
// GPU never runs client work; kernel-internal rings (KIQ, SDMA page tables, blits) are untouched.
// Tells whether the NVMe/xHCI death right after login is caused by GPU command execution.
void wrapAccelSubmitCommands(uint64_t self, uint64_t desc) {
	static uint32_t drops;
	if (++drops <= 5 || drops % 1000 == 0)
		LOG("nosubmit: dropped user command buffer #%u (chan %llx)", drops, self);
	if (drops == 1)
		ptScan("first user command buffer");
}

// Local APIC of every CPU, read on that CPU by an xnu cross-call through xnu's own lapic_ops (xAPIC
// or x2APIC). Registers are lapic_register_t numbers (MMIO offset >> 4): ID 0x02, TPR 0x08,
// PPR 0x0A, ISR 0x10-0x17, IRR 0x20-0x27. The GPU MSI (vector 0x78) goes to APIC 0 like NVMe and
// xHCI; a vector stuck in-service (no EOI) blocks every vector of its priority class and below on
// that CPU while IPIs/timer (0xD0+) still run. A CPU spinning with interrupts off never answers:
// then mp_cpus_call_wait NMIs it and the panic carries its backtrace.
struct LapicOps {
	void (*init)();
	uint32_t (*read)(uint32_t);
	void (*write)(uint32_t, uint32_t);
};
LapicOps **gLapicOps;
int *gIsX2apic;
bool gIerFix;
uint32_t (*gMpCpusCall)(uint64_t, int, void (*)(void *), void *);
int (*gCpuNumber)();

struct LapicSnap {
	uint32_t id, tpr, ppr, isr[8], irr[8];
	uint32_t svr, esr, tmr3, ef, ec, ier[8];
	bool done, ext, ierFixed;
} gLapic[64];

// 10-25 15:55 bt0: CPU 0 sits IDLE in hlt (idle_thread → processor_idle → machine_idle), takes the
// IPI/timer, yet 0x71-0x78 stay in IRR for 10 s with TPR 0 and nothing else in service. The one
// mechanism that holds vectors pending in IRR like that is AMD's extended APIC Interrupt Enable
// Registers (ext-feature 0x400 bit 0 = IER present, ext-control 0x410 bit 0 = IER on, IER 0x480-0x4F0
// one bit per vector, clear = never dispatched). xAPIC MMIO only. If IER is on with the class-7 word
// not all set, set it (-rmbnoierfix off) and read IRR again a moment later.

// CPU 0 sat in class-D service (ppr d0, 10-25 15:40 boot) with every device vector pending: it answers
// the cross-call from inside whatever loop it spins in, so its frame-pointer chain names the loop.
struct Int0 {
	uint64_t thread, rip, cs, rflags, rsp, bt[16];
	int32_t intLevel, preempt;
	uint32_t flavor, btLen;
	uint16_t trapno;
	bool ok;
} gInt0;

// 10-25 16:45 int0: CPU 0 really is in machine_idle's sti;hlt (IF 1), wakes for our IPI, yet 71-78
// stay in IRR. Every read so far was inside the 0xDE handler (ppr d0). At stall dumps the CPU 0 action
// EOIs 0xDE itself (A = ppr/isr/irr with nothing in service), then runs 1 ms with interrupts ON in
// non-hlt code (B). B irr empty + ih rptr moves = the core takes them when awake, only the hlt wake
// fails; still pending with ppr 0 = the core refuses class-7 fixed vectors. xnu's own EOI at the
// end of the handler then finds ISR empty (harmless). -rmbnoeoi0 off.
bool gEoi0Off;
struct Eoi0 {
	LapicSnap a, b;
	uint32_t ppr0, rp0, rp1;
	uint64_t t0, t1;
	volatile bool ok;
} gEoi0;
void lapicRead(LapicSnap &s);

// 10-25 17:00 eoi0 RESULT: in the IF=1 window CPU 0 took the GPU MSI and then stayed >= 250 ms in the
// fb interrupt path (IH ring check → re-arm write → our wrapRawWrite), so the caller's SYNC cross-call
// NMI-panicked first and our line was lost. Long handler or interrupt storm? Every register write CPU
// 0 makes inside the window is recorded (count, first 12, last 12 with time since the window opened).
volatile bool gEoi0Window;
struct Eoi0Write {
	uint32_t reg, val;
	uint64_t t;
};
struct Eoi0Rec {
	Eoi0Write first[12], last[12];
	volatile uint32_t n;
} gEoi0Rec;

void eoi0Note(uint32_t reg, uint32_t val) {
	if (!gEoi0Window || gCpuNumber() != 0)
		return;
	uint32_t i = gEoi0Rec.n++;
	Eoi0Write w {reg, val, mach_absolute_time()};
	if (i < arrsize(gEoi0Rec.first))
		gEoi0Rec.first[i] = w;
	gEoi0Rec.last[i % arrsize(gEoi0Rec.last)] = w;
}

struct KextSummaryHeader {
	uint32_t version, entrySize, num, reserved;
};
KextSummaryHeader **gKextSummaries;

// "Kext+off" from OSKext's loaded-kext summaries (name[64] uuid[16] address size ...), else the
// unslid-looking kernel address.
int frameName(char *buf, int n, uint64_t a) {
	if (gKextSummaries && *gKextSummaries) {
		auto h = *gKextSummaries;
		auto base = reinterpret_cast<const uint8_t *>(h + 1);
		for (uint32_t i = 0; i < h->num; i++) {
			auto e = base + i * h->entrySize;
			uint64_t addr = *reinterpret_cast<const uint64_t *>(e + 80), size = *reinterpret_cast<const uint64_t *>(e + 88);
			if (a >= addr && a < addr + size) {
				auto name = reinterpret_cast<const char *>(e);
				auto dot = strrchr(name, '.');
				return snprintf(buf, n, " %s+%llx", dot ? dot + 1 : name, a - addr);
			}
		}
	}
	return snprintf(buf, n, " %llx", a);
}

void lapicSnapAction(void *) {
	int c = gCpuNumber();
	if (c < 0 || c >= 64)
		return;
	auto rd = (*gLapicOps)->read;
	auto &s = gLapic[c];
	s.id = rd(0x02), s.tpr = rd(0x08), s.ppr = rd(0x0A);
	for (uint32_t i = 0; i < 8; i++)
		s.isr[i] = rd(0x10 + i), s.irr[i] = rd(0x20 + i);
	s.ef = s.ec = 0, s.ierFixed = false;
	for (auto &w : s.ier)
		w = 0;
	s.svr = rd(0x0F), s.tmr3 = rd(0x18 + 3);
	(*gLapicOps)->write(0x28, 0), s.esr = rd(0x28);  // ESR latches on write
	s.ext = gIsX2apic && !*gIsX2apic;
	if (s.ext) {
		s.ef = rd(0x40), s.ec = rd(0x41);
		if (s.ef & 1)
			for (uint32_t i = 0; i < 8; i++)
				s.ier[i] = rd(0x48 + i);
		if ((s.ef & 1) && (s.ec & 1) && s.ier[3] != ~0U && gIerFix) {
			for (uint32_t i = 0; i < 8; i++)
				(*gLapicOps)->write(0x48 + i, ~0U);
			s.ierFixed = true;
		}
	}
	if (c == 0) {
		// 10-25 16:40: the fp walk from here stops at hndl_allintrs → call_continuation (interrupt
		// frame), so "idle in hlt" was never proven. cpu_int_state (cpu_data +0x28, set by
		// hndl_allintrs) = the code our IPI interrupted: x86_saved_state_t flavor @0, ss_64 @0x10
		// (rbp +0x60, isf +0x90: trapno +0, rip +0x18, cs +0x20, rflags +0x28, rsp +0x30).
		auto &q = gInt0;
		q = {};
		uint64_t cd;
		asm volatile("mov %%gs:0, %0" : "=r"(cd));  // cpu_data.cpu_this
		auto cdp = reinterpret_cast<const uint8_t *>(cd);
		q.thread = *reinterpret_cast<const uint64_t *>(cdp + 0x10);
		q.intLevel = *reinterpret_cast<const int32_t *>(cdp + 0x54);
		q.preempt = *reinterpret_cast<const int32_t *>(cdp + 0x58);
		auto st = *reinterpret_cast<const uint8_t *const *>(cdp + 0x28);
		if (st && reinterpret_cast<uint64_t>(st) >= 0xffffff8000000000ULL) {
			auto ss = st + 0x10, isf = ss + 0x90;
			q.flavor = *reinterpret_cast<const uint32_t *>(st);
			q.trapno = *reinterpret_cast<const uint16_t *>(isf);
			q.rip = *reinterpret_cast<const uint64_t *>(isf + 0x18);
			q.cs = *reinterpret_cast<const uint64_t *>(isf + 0x20);
			q.rflags = *reinterpret_cast<const uint64_t *>(isf + 0x28);
			q.rsp = *reinterpret_cast<const uint64_t *>(isf + 0x30);
			uint64_t rbp = *reinterpret_cast<const uint64_t *>(ss + 0x60);
			q.ok = true;
			// Interrupted kernel stack: frames must stay inside [rsp, rsp + 64K) and grow upward.
			uint64_t lo = q.rsp;
			while ((q.cs & 3) == 0 && q.btLen < arrsize(q.bt) && !(rbp & 7) && rbp >= lo &&
			       rbp < q.rsp + 0x10000 && rbp >= 0xffffff8000000000ULL) {
				auto fp = reinterpret_cast<const uint64_t *>(rbp);
				if (fp[1] < 0xffffff8000000000ULL)
					break;
				q.bt[q.btLen++] = fp[1];
				lo = rbp + 16, rbp = fp[0];
			}
		}
	}
	s.done = true;
}

// Vector numbers set in a 256-bit ISR/IRR image, minus `skip` (the cross-call IPI 0xDE itself is
// always in service while the snapshot runs).
int vectorList(char *buf, int n, const uint32_t *bits, uint32_t skip = 0x100) {
	int k = 0;
	for (uint32_t v = 0; v < 256; v++)
		if (v != skip && (bits[v / 32] & (1U << (v % 32))))
			k += snprintf(buf + k, n - k, " %02x", v);
	return k ? k : snprintf(buf, n, " -");
}

// CPU 0 only, sent NOSYNC: EOI our 0xDE, 1 ms with interrupts on, record what it writes meanwhile.
void eoi0Action(void *) {
	if (gCpuNumber() != 0)
		return;
	auto &e = gEoi0;
	e.ppr0 = (*gLapicOps)->read(0x0A);
	(*gLapicOps)->write(0x0B, 0);  // EOI our 0xDE
	lapicRead(e.a);
	e.rp0 = mmioRead(0x10A0 + 0x83);
	e.t0 = mach_absolute_time();
	gEoi0Window = true;
	ml_set_interrupts_enabled(true);
	IODelay(1000);
	ml_set_interrupts_enabled(false);
	gEoi0Window = false;
	e.t1 = mach_absolute_time();
	lapicRead(e.b);
	e.rp1 = mmioRead(0x10A0 + 0x83);
	__atomic_store_n(&e.ok, true, __ATOMIC_RELEASE);
}

uint32_t eoi0Us(uint64_t t) {
	uint64_t ns = 0;
	if (t > gEoi0.t0)
		absolutetime_to_nanoseconds(t - gEoi0.t0, &ns);
	return static_cast<uint32_t>(ns / 1000);
}

// The caller does not wait in xnu (NOSYNC, so no NMIPI 250 ms later); it polls 50 ms itself. CPU 0
// still inside then → keep the record and panic at once (our stall panic prints the kept lines).
void eoi0Probe(const char *why) {
	static bool busy;
	if (busy)
		return;
	busy = true;
	gEoi0 = {};
	gEoi0Rec.n = 0;
	gMpCpusCall(1ULL, 2 /* NOSYNC */, eoi0Action, nullptr);
	uint32_t ms = 0;
	while (!__atomic_load_n(&gEoi0.ok, __ATOMIC_ACQUIRE) && ms < 50) {
		IODelay(1000);
		ms++;
	}
	auto &e = gEoi0;
	bool ok = __atomic_load_n(&e.ok, __ATOMIC_ACQUIRE);
	uint32_t n = gEoi0Rec.n;
	char line[800], ar[120], br[120];
	vectorList(ar, sizeof(ar), e.a.irr), vectorList(br, sizeof(br), e.b.irr);
	int k;
	if (ok)
		k = snprintf(line, sizeof(line), "eoi0 %s: in ipi ppr %x; after eoi ppr %x r%s; window %u us: ppr %x r%s; ih rptr %x -> %x; %u wr",
		             why, e.ppr0, e.a.ppr, ar, eoi0Us(e.t1), e.b.ppr, br, e.rp0, e.rp1, n);
	else
		k = snprintf(line, sizeof(line), "eoi0 %s: CPU 0 STILL IN after %u ms (t0 %s); after eoi ppr %x r%s; ih rptr %x now %x wptr %x; %u wr",
		             why, ms, e.t0 ? "set" : "NOT set", e.a.ppr, ar, e.rp0, mmioRead(0x10A0 + 0x83), mmioRead(0x10A0 + 0x84), n);
	uint32_t nf = n < arrsize(gEoi0Rec.first) ? n : arrsize(gEoi0Rec.first);
	k += snprintf(line + k, sizeof(line) - k, "; first");
	for (uint32_t i = 0; i < nf && k < static_cast<int>(sizeof(line)); i++)
		k += snprintf(line + k, sizeof(line) - k, " %x=%x@%u", gEoi0Rec.first[i].reg, gEoi0Rec.first[i].val, eoi0Us(gEoi0Rec.first[i].t));
	if (n > nf) {
		k += snprintf(line + k, sizeof(line) - k, "; last");
		uint32_t nl = n - nf < arrsize(gEoi0Rec.last) ? n - nf : arrsize(gEoi0Rec.last);
		for (uint32_t j = n - nl; j < n && k < static_cast<int>(sizeof(line)); j++) {
			auto &w = gEoi0Rec.last[j % arrsize(gEoi0Rec.last)];
			k += snprintf(line + k, sizeof(line) - k, " %x=%x@%u", w.reg, w.val, eoi0Us(w.t));
		}
	}
	panicKeep("%s", line);
	if (!ok)
		keepPanic();
	busy = false;
}

// One line per CPU that has anything besides the IPI in service or pending (CPU 0 always); with
// `keep` a compact copy goes to the panic text.
void lapicDump(const char *why, bool keep = false) {
	if (!gLapicOps || !*gLapicOps || !gMpCpusCall || !gCpuNumber) {
		LOG("lapic: %s: symbols missing", why);
		return;
	}
	for (auto &s : gLapic)
		s.done = false;
	uint64_t t0 = mach_absolute_time();
	uint32_t n = gMpCpusCall(~0ULL, 0 /* SYNC */, lapicSnapAction, nullptr);
	uint64_t ns;
	absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
	LOG("lapic: %s: %u cpus answered in %llu us (caller cpu %d)", why, n, ns / 1000, gCpuNumber());
	char sum[600];
	int k = snprintf(sum, sizeof(sum), "lapic %s: %u cpus%s", why, n, gLapic[0].done ? "" : ", cpu 0 NO ANSWER");
	for (int c = 0; c < 64; c++) {
		auto &s = gLapic[c];
		if (!s.done)
			continue;
		bool any = false;
		for (uint32_t i = 0; i < 8; i++)
			any |= s.irr[i] || (s.isr[i] & ~(i == 6 ? 1U << 30 : 0U));  // 0xDE = isr[6] bit 30
		if (c && !any)
			continue;
		char isr[200], irr[200];
		vectorList(isr, sizeof(isr), s.isr, 0xDE);
		vectorList(irr, sizeof(irr), s.irr);
		LOG("lapic: cpu %d id %x tpr %x ppr %x isr%s irr%s", c, s.id, s.tpr, s.ppr, isr, irr);
		if (k < static_cast<int>(sizeof(sum)))
			k += snprintf(sum + k, sizeof(sum) - k, "; c%d t%x i%s r%s", c, s.tpr, isr, irr);
		if (c == 0 && k < static_cast<int>(sizeof(sum)))  // 10-25 16:20: ec 0 = IER off, ier all ffffffff
			k += snprintf(sum + k, sizeof(sum) - k, " svr %x esr %x tmr7 %x %s ef %x ec %x%s",
			              s.svr, s.esr, s.tmr3, s.ext ? "xapic" : "x2apic/unk", s.ef, s.ec, s.ierFixed ? " IER FIXED" : "");

	}
	if (keep)
		panicKeep("%s", sum);
	else
		LOG("lapic sum %s", sum);
	// After an IER fix: does CPU 0 now drain the stuck device vectors?
	if (gLapic[0].ierFixed) {
		IOSleep(50);
		for (auto &s : gLapic)
			s.done = false;
		gMpCpusCall(1ULL, 0, lapicSnapAction, nullptr);
		char irr[200];
		vectorList(irr, sizeof(irr), gLapic[0].irr);
		panicKeep("ierfix %s: cpu 0 irr 50 ms after:%s", why, irr);
	}
	if (gLapic[0].done) {
		auto &q = gInt0;
		char rip[100] = " ?", bt[700] = "";
		int b = 0;
		if (q.ok)
			frameName(rip, sizeof(rip), q.rip);
		for (uint32_t i = 0; i < q.btLen && b < static_cast<int>(sizeof(bt)); i++)
			b += frameName(bt + b, sizeof(bt) - b, q.bt[i]);
		char line[900];
		snprintf(line, sizeof(line), "int0 %s:%s rip%s cs %llx fl %llx IF %d trap %x rsp %llx; il %d pl %d thr %llx; bt%s",
		         why, q.ok ? "" : " NO int_state", rip, q.cs, q.rflags, static_cast<int>((q.rflags >> 9) & 1), q.trapno,
		         q.rsp, q.intLevel, q.preempt, q.thread, b ? bt : " -");
		LOG("lapic: %s (flavor %x)", line, q.flavor);  // 10-25 16:45: answered (idle hlt), log only
	}
	if (keep && !gEoi0Off)
		eoi0Probe(why);
}

// 10-25 16:20: every snapshot so far ran INSIDE our 0xDE IPI handler, so CPU 0's PPR outside a
// handler was never seen, and CPU 0 was always idle in hlt. A thread bound to CPU 0 reads the LAPIC
// with interrupts off (A = real PPR/ISR), then runs 1 ms with interrupts ON (B: does the core take
// 71-78 while running code?), and if they are still pending writes one EOI and runs 1 ms more (C).
void *(*gCpuToProcessor)(int);
void *(*gThreadBind)(void *);
int (*gThreadBlock)(void *);

struct Cpu0Probe {
	LapicSnap a, b, c;
	uint64_t cr8;
	int cpu;
	bool ifWas, eoi;
	volatile bool done;
} gCpu0;

void lapicRead(LapicSnap &s) {
	auto rd = (*gLapicOps)->read;
	s.tpr = rd(0x08), s.ppr = rd(0x0A);
	for (uint32_t i = 0; i < 8; i++)
		s.isr[i] = rd(0x10 + i), s.irr[i] = rd(0x20 + i);
}

bool devicePending(const LapicSnap &s) {
	for (uint32_t i = 1; i < 6; i++)  // vectors 0x20-0xBF
		if (s.irr[i])
			return true;
	return false;
}

void cpu0ProbeThread(void *, wait_result_t) {
	auto &p = gCpu0;
	gThreadBind(gCpuToProcessor(0));
	gThreadBlock(nullptr);  // THREAD_CONTINUE_NULL: come back on CPU 0
	p.cpu = gCpuNumber();
	if (p.cpu == 0) {
		p.ifWas = ml_set_interrupts_enabled(false);
		uint64_t cr8;
		asm volatile("mov %%cr8, %0" : "=r"(cr8));
		p.cr8 = cr8;
		lapicRead(p.a);
		ml_set_interrupts_enabled(true);
		IODelay(1000);
		ml_set_interrupts_enabled(false);
		lapicRead(p.b);
		if (devicePending(p.b)) {
			(*gLapicOps)->write(0x0B, 0);
			p.eoi = true;
			ml_set_interrupts_enabled(true);
			IODelay(1000);
			ml_set_interrupts_enabled(false);
			lapicRead(p.c);
		}
		ml_set_interrupts_enabled(p.ifWas);
	}
	gThreadBind(nullptr);
	p.done = true;
}

void cpu0Probe(const char *why) {
	if (!gCpuToProcessor || !gThreadBind || !gThreadBlock || !gLapicOps || !*gLapicOps) {
		LOG("t0: %s: symbols missing", why);
		return;
	}
	static bool busy;
	if (busy) {  // last probe thread still not back: keep its result slot
		panicKeep("t0 %s: previous probe thread never returned", why);
		return;
	}
	busy = true;
	gCpu0 = {};
	uint32_t rp0 = mmioRead(0x10A0 + 0x83);
	thread_t t;
	if (kernel_thread_start(cpu0ProbeThread, nullptr, &t) != KERN_SUCCESS) {
		busy = false;
		return;
	}
	thread_deallocate(t);
	uint32_t ms = 0;
	while (!gCpu0.done && ms < 2000)
		IOSleep(10), ms += 10;
	uint32_t rp1 = mmioRead(0x10A0 + 0x83);
	if (!gCpu0.done) {
		panicKeep("t0 %s: thread bound to cpu 0 did NOT run in 2 s; ih rptr %x -> %x", why, rp0, rp1);
		return;
	}
	busy = false;
	auto &p = gCpu0;
	char ai[120], ar[120], bi[120], br[120], ci[120], cr[120], c[300] = "";
	vectorList(ai, sizeof(ai), p.a.isr), vectorList(ar, sizeof(ar), p.a.irr);
	vectorList(bi, sizeof(bi), p.b.isr), vectorList(br, sizeof(br), p.b.irr);
	if (p.eoi) {
		vectorList(ci, sizeof(ci), p.c.isr), vectorList(cr, sizeof(cr), p.c.irr);
		snprintf(c, sizeof(c), " eoi+1ms: ppr %x i%s r%s;", p.c.ppr, ci, cr);
	}
	panicKeep("t0 %s: cpu %d in %u ms if %d cr8 %llx; A tpr %x ppr %x i%s r%s; 1ms IF=1: ppr %x i%s r%s;%s ih rptr %x -> %x",
	          why, p.cpu, ms, p.ifWas, p.cr8, p.a.tpr, p.a.ppr, ai, ar, p.b.ppr, bi, br, c, rp0, rp1);
}

// IH ring 0 as the driver sees it: MMIO rptr/wptr vs the wptr WRITEBACK in RAM (what Apple's IRQ
// manager reads to find new entries; stale writeback = rptr stuck with no IRQ problem at all), and
// the pending entries decoded (client, src id, ring, vmid, src data).
void ihSnapshot(const char *why) {
	constexpr uint32_t Oss = 0x10A0;
	uint32_t cntl = mmioRead(Oss + 0x80), rp = mmioRead(Oss + 0x83), wp = mmioRead(Oss + 0x84);
	uint64_t rb = (static_cast<uint64_t>(mmioRead(Oss + 0x82)) << 32 | mmioRead(Oss + 0x81)) << 8;
	uint64_t wb = static_cast<uint64_t>(mmioRead(Oss + 0x85)) << 32 | mmioRead(Oss + 0x86);
	bool sane = [](uint64_t a) { return a && a < (64ULL << 30); }(rb);
	uint32_t wbVal = wb && wb < (64ULL << 30) ? ml_phys_read_64(wb & ~3ULL) : 0xFFFFFFFF;
	uint32_t mask = (4U << ((cntl >> 1) & 0x1F)) - 1;  // RB_SIZE = log2(dwords)
	char ent[700];
	int k = 0;
	for (uint32_t off = rp & ~0x1FU, i = 0; sane && off != (wp & mask & ~0x1FU) && i < 8; off = (off + 32) & mask, i++) {
		uint32_t d[8];
		for (uint32_t j = 0; j < 8; j++)
			d[j] = ml_phys_read_64(rb + off + 4 * j);
		k += snprintf(ent + k, sizeof(ent) - k, " %x:c%x s%x r%x v%x d%x,%x", off, d[0] & 0xFF, (d[0] >> 8) & 0xFF,
		              (d[0] >> 16) & 0xFF, (d[0] >> 24) & 0xF, d[4], d[5]);
	}
	panicKeep("ih %s: cntl %x rptr %x wptr %x wb@%llx=%x;%s", why, cntl, rp, wp, wb, wbVal, k ? ent : " none");
}

// -rmbsubmitscan: same census, but user command buffers DO reach the GPU. User VMIDs get their page
// tables only when the first Metal client (login window) starts, ~5 s after the first WindowServer
// buffer, and the NVMe/xHCI die ~2 s later: so poll the user contexts every 50 ms for 90 s, scan a
// VMID the moment its base changes, and rescan the live ones every second.
void submitScanThread(void *, wait_result_t) {
	IOSleep(300);
	ptScan("+0.3 s after first submit");
	mmhubDump("+0.3 s");
	char irq[160];
	gpuIrqState(irq, sizeof(irq));
	LOG("irq: +0.3 s: %s", irq);
	lapicDump("+0.3 s baseline");
	char tg[600];
	pciIrqTargets(tg, sizeof(tg));
	LOG("targets: gpu %s; %s", irq, tg);  // 10-25 15:40: all MSIs → APIC 0, vectors 71-78
	ihSnapshot("+0.3 s");
	uint64_t seen[16] {};
	uint32_t lastRp = ~0U, lastWp = ~0U, stuck = 0, lapicDumps = 0, firstStuck = ~0U;
	for (uint32_t tick = 0; tick < 1800; tick++) {
		IOSleep(50);
		// Once a second: IH ring0 read/write pointers (does the GPU still post interrupts, does the
		// driver still consume them) + the GPU's INTx/MSI state.
		if (tick % 20 == 0) {
			uint32_t rp = mmioRead(0x10A0 + 0x83), wp = mmioRead(0x10A0 + 0x84);
			if (rp != lastRp && stuck >= 2)
				panicKeep("irq: rptr moved %x -> %x (wptr %x) after %u s stuck", lastRp, rp, wp, stuck);
			if (rp != lastRp || wp != lastWp || tick % 100 == 0) {
				char irq[160];
				gpuIrqState(irq, sizeof(irq));
				uint32_t fc, fe, fs;
				ihFlushStats(fc, fe, fs);
				LOG("irq: tick %u IH ring0 rptr %x wptr %x%s; flush %u calls %u entries %u stale; %s", tick, rp, wp,
				    wp & 1 ? " OVERFLOW" : "", fc, fe, fs, irq);
			}
			// Entries pending and rptr not moving for 2 s = driver stopped taking GPU interrupts:
			// snapshot the LAPICs then and 5 s later.
			stuck = rp == lastRp && (wp & ~1U) != rp ? stuck + 1 : 0;
			lastRp = rp, lastWp = wp;
			// Snapshots at the first 2 s stall, then 5 s and 10 s later whether or not rptr moved
			// (the stall panic comes ~15 s after the disk dies).
			if (stuck == 2 && lapicDumps == 0)
				firstStuck = tick;
			if (lapicDumps < 3 && firstStuck != ~0U && tick == firstStuck + lapicDumps * 100) {
				static const char *const names[] {"stall+0", "stall+5s", "stall+10s"};
				ihSnapshot(names[lapicDumps]);
				lapicDump(names[lapicDumps], true);
				// 10-25 16:40 t0 RESULT: a thread bound to cpu 0 never ran (2 s, still not at +5 s) → probe off.
				lapicDumps++;
			}
		}
		for (uint32_t v = 1; v < 16; v++) {
			uint64_t base = ptVmidBase(v);
			bool changed = base != seen[v];
			seen[v] = base;
			if (base && (changed || tick % 20 == 0)) {
				if (changed) {
					LOG("ptscan: vmid %u now live, base %llx (tick %u)", v, base, tick);
					mmhubDump("gc vmid live");
				}
				ptScanVmid(v, !changed);
			}
		}
	}
	LOG("ptscan: watch over");
}

uint64_t wrapAccelSubmitScan(uint64_t self, uint64_t desc) {
	static uint32_t submits;
	if (++submits <= 5)
		LOG("submitscan: user command buffer #%u (chan %llx)", submits, self);
	if (submits == 1) {
		ptScan("before first user command buffer");
		thread_t t;
		if (kernel_thread_start(submitScanThread, nullptr, &t) == KERN_SUCCESS)
			thread_deallocate(t);
	}
	return reinterpret_cast<uint64_t (*)(uint64_t, uint64_t)>(orgAccelSubmitCommands)(self, desc);
}

bool probeRouteAccel(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
	IOLog("REM660: [probeRouteAccel] ENTER index=%zu addr=0x%llx size=0x%zx\n", index, address, size);
	gAccelBase = address;
	gKeepScanoutDcc = checkKernelArgument("-rmbscandcc");  // original default: no DCC on scanout surfaces
	gGetHWChannelEngineRing = findSymbol(patcher, 2,
		"__ZN26AMDRadeonX6000_AMDHardware12getHWChannelE20_eAMD_HW_ENGINE_TYPE18_eAMD_HW_RING_TYPE", address, size);
	KernelPatcher::RouteRequest reqs[] {
		{"__ZN37AMDRadeonX6000_AMDGraphicsAccelerator5probeEP9IOServicePi",wrapAcceleratorProbe,orgAcceleratorProbe},
		{"__ZN43AMDRadeonX6000_AMDNavi23GraphicsAccelerator5probeEP9IOServicePi", wrapAcceleratorProbe, orgAcceleratorProbe},
		{"__ZN37AMDRadeonX6000_AMDGraphicsAccelerator5startEP9IOService", wrapAcceleratorStart, orgAcceleratorStart},
		{"__ZN43AMDRadeonX6000_AMDNavi23GraphicsAccelerator5startEP9IOService", wrapAcceleratorStart, orgAcceleratorStart},
		{"__ZN30AMDRadeonX6000_AMDKIQHWChannel14submitKIQFrameEv", wrapSubmitKIQFrame, orgSubmitKIQFrame},
		{"__ZN32AMDRadeonX6000_AMDGFX10PM4Engine14initComputeMQDE18_eAMD_HW_RING_TYPE", wrap_PM4InitComputeMQD, org_PM4InitComputeMQD},
		{"__ZN32AMDRadeonX6000_AMDGFX10PM4Engine15initGraphicsMQDEv", wrap_PM4InitGraphicsMQD, org_PM4InitGraphicsMQD},
		{"__ZN35AMDRadeonX6000_AMDGFX10KIQHWChannel8startKIQEyyPK18AMD_PM4_QUEUE_SPECPj", wrapStartKIQ},
		{"__ZN26AMDRadeonX6000_AMDHWMemory17adjustVRAMAddressEy", wrapAdjustVRAMAddress,orgAdjustVRAMAddress},
		{"__ZN26AMDRadeonX6000_AMDHardware16powerUpHWEnginesEv", wrapPowerUpHWEngines},
		{"__ZN28AMDRadeonX6000_AMDRTHardware12getHWChannelE18_eAMD_CHANNEL_TYPE11SS_PRIORITYj", wrapRTGetHWChannel, orgRTGetHWChannel},
		{"__ZN26AMDRadeonX6000_AMDHardware14startHWEnginesEv", wrapStartHWEngines},
	};
	bool ok = route(patcher, index, address, size, reqs, arrsize(reqs));
	if (!ok) {
		ok = routeStaticRequests(patcher, 2, reqs, arrsize(reqs), address, size);
	}
	if (!orgAcceleratorProbe) {
		mach_vm_address_t aProbe = findSymbol(patcher, 2, "__ZN43AMDRadeonX6000_AMDNavi23GraphicsAccelerator5probeEP9IOServicePi", address, size);
		if (!aProbe) aProbe = findSymbol(patcher, 2, "__ZN37AMDRadeonX6000_AMDGraphicsAccelerator5probeEP9IOServicePi", address, size);
		if (aProbe != 0) {
			orgAcceleratorProbe = patcher.routeFunction(aProbe, reinterpret_cast<mach_vm_address_t>(wrapAcceleratorProbe), true);
			IOLog("REM660: [probeRouteAccel] fallback routed Accelerator::probe at 0x%llx (org=%p)\n", (uint64_t)aProbe, (void *)orgAcceleratorProbe);
		} else {
			IOLog("REM660: [probeRouteAccel] WARNING: Missing symbol for Accelerator probe\n");
		}
	}
	if (!orgAcceleratorStart) {
		mach_vm_address_t aStart = findSymbol(patcher, 2, "__ZN43AMDRadeonX6000_AMDNavi23GraphicsAccelerator5startEP9IOService", address, size);
		if (!aStart) aStart = findSymbol(patcher, 2, "__ZN37AMDRadeonX6000_AMDGraphicsAccelerator5startEP9IOService", address, size);
		if (aStart != 0) {
			orgAcceleratorStart = patcher.routeFunction(aStart, reinterpret_cast<mach_vm_address_t>(wrapAcceleratorStart), true);
			IOLog("REM660: [probeRouteAccel] fallback routed Accelerator::start at 0x%llx (org=%p)\n", (uint64_t)aStart, (void *)orgAcceleratorStart);
		} else {
			IOLog("REM660: [probeRouteAccel] WARNING: Missing symbol for Accelerator start\n");
		}
	}
	auto fnInitVRAM = findSymbol(patcher, 2, "__ZN26AMDRadeonX6000_AMDHWMemory12initVRAMInfoEv", address, size);
	if (fnInitVRAM) {
		KernelPatcher::RouteRequest rReq {"__ZN26AMDRadeonX6000_AMDHWMemory12initVRAMInfoEv", wrapInitVRAMInfo, orgInitVRAMInfo};
		routeStaticRequests(patcher, 2, &rReq, 1, address, size);
	}
	// Original RembrandtGfx probeRouteAccel functional routes that this fork had dropped (2026-11-09). Own
	// group: a failure here must not undo probe/start/KIQ above. RVAs: llvm-nm of Accelerator UUID DCD61202
	// (updateHWInfoClocks 0x700a0, shouldAllocScanoutDcc 0x2022e, resetHardware 0x6fb8c); their first 14 bytes
	// are push/mov/sub only. SDMAEngine::reset (0x6aad4) is NOT routed: its entry has `je 0x6ab44` at +0xc,
	// which a Lilu trampoline would copy unrelocated.
	KernelPatcher::RouteRequest fnReqs[] {
		{"__ZN26AMDRadeonX6000_AMDHardware18updateHWInfoClocksEv", wrapUpdateHWInfoClocks, orgUpdateHWInfoClocks},
		{"__ZNK36AMDRadeonX6000_AMDAccelResourceAddr221shouldAllocScanoutDccEjjjj", wrapShouldAllocScanoutDcc, orgShouldAllocScanoutDcc},
		{"__ZN26AMDRadeonX6000_AMDHardware13resetHardwareEjPj", wrapResetHardware, orgResetHardware},
	};
	bool fnOk = restoredRouteGroup(patcher, 2, fnReqs, arrsize(fnReqs), address, size);
	LOG("accel: HW-info clocks %s, scanout DCC %s (%s), resetHardware %s%s", orgUpdateHWInfoClocks ? "routed" : "NOT routed",
		orgShouldAllocScanoutDcc ? "routed" : "NOT routed", gKeepScanoutDcc ? "Apple policy, -rmbscandcc" : "off",
		orgResetHardware ? "routed" : "NOT routed", fnOk ? "" : " (group reported failure)");
	LOG("accel probes installed");
	vcnRouteAccel(patcher, 2, address, size);  // MMHUB 2.3 map for the MM hub (vcn.cpp, opt-in)
	IOLog("REM660: [probeRouteAccel] EXIT ok=%d\n", ok ? 1 : 0);
	return ok;
}
