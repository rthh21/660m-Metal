//
//  Direct access to the 680M's register aperture, for the parts we drive ourselves.
//

#include "rmb.hpp"
#include <IOKit/IOService.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSData.h>
#include <IOKit/pci/IOPCIDevice.h>

static volatile uint32_t *gMmio;
static IOMemoryMap *gMmioMap;
static constexpr uint64_t RegApertureSize = 0x80000;  // "Reg Aper: 512 KB"

static uint32_t dataU32(IORegistryEntry *e, const char *key) {
	auto d = OSDynamicCast(OSData, e->getProperty(key));
	return d && d->getLength() == 4 ? *static_cast<const uint32_t *>(d->getBytesNoCopy()) : 0;
}

static bool lenovoGpu(IORegistryEntry *svc) {
	auto devId = dataU32(svc, "device-id");
	return dataU32(svc, "vendor-id") == 0x1002 && (dataU32(svc, "class-code") >> 16) == 0x03 &&
	       (devId == 0x1681 || devId == 0x73ff) && dataU32(svc, "revision-id") == 3 &&
	       dataU32(svc, "subsystem-vendor-id") == 0x17aa && dataU32(svc, "subsystem-id") == 0x3b1e;
}

static bool mapFrom(IOService *svc, const char *how) {
	if (gMmio)
		return true;
	if (!svc || !lenovoGpu(svc))
		return false;
	for (unsigned i = 0; i < svc->getDeviceMemoryCount(); i++) {
		auto mem = svc->getDeviceMemoryWithIndex(i);
		if (!mem || mem->getLength() != RegApertureSize)
			continue;
		auto map = mem->map(kIOMapInhibitCache);
		if (map && !OSCompareAndSwapPtr(nullptr, map, reinterpret_cast<void * volatile *>(&gMmioMap))) {
			map->release();  // another thread mapped it first
		} else if (map) {
			gMmio = reinterpret_cast<volatile uint32_t *>(map->getVirtualAddress());
			LOG("mapped register aperture (device memory %u, %s)", i, how);
		}
		break;
	}
	return gMmio != nullptr;
}

// During its own matching/start the IOPCIDevice is not returned by getMatchingServices (boot of
// 2026-11-08: coreServices at 26.0 s read 0xffffffff, the aperture only mapped at 27.16 s).
// The controller's start() hands us the provider directly: map from it.
bool mmioAdoptProvider(IOService *provider) {
	if (gMmio)
		return true;
	bool ok = mapFrom(provider, "controller provider");
	if (!ok)
		LOG("mmio: provider %p not usable for the register aperture", provider);
	return ok;
}

static bool mapMmio() {
	if (gMmio)
		return true;
	auto matching = IOService::serviceMatching("IOPCIDevice");
	if (!matching)
		return false;
	auto iter = IOService::getMatchingServices(matching);
	matching->release();
	if (!iter)
		return false;
	while (auto svc = OSDynamicCast(IOService, iter->getNextObject())) {
		if (!lenovoGpu(svc))
			continue;
		mapFrom(svc, "registry");
		break;
	}
	iter->release();
	return gMmio != nullptr;
}

bool mmioAvailable() { return mapMmio(); }

// Core Services window (kern_start.cpp): our own accesses are logged and flushed first as well.
uint32_t mmioRead(uint32_t reg) {
	bool tr = coreTraceActive();
	if (tr)
		coreTraceAccess("mmiord", reg, 0, nullptr);
	uint32_t v = mapMmio() && reg < RegApertureSize / 4 ? gMmio[reg] : 0xFFFFFFFF;
	if (tr)
		coreTraceResult("mmiord", reg, v);
	return v;
}

void mmioWrite(uint32_t reg, uint32_t value) {
	if (coreTraceActive())
		coreTraceAccess("mmiowr", reg, value, nullptr);
	if (mapMmio() && reg < RegApertureSize / 4)
		gMmio[reg] = value;
}

// CPU-visible VRAM (BAR0, 256 MB window onto the carve-out), mapped one page at a time.
static IODeviceMemory *gVram;
static IOMemoryMap *gVramPage;
static uint64_t gVramPageOff = ~0ULL;
// gVramPage is shared by every thread (DMUB ring code, dmub poll, vmwatch, submitscan): without this
// lock one thread released the page another was still reading through (ptscan page fault, 10-25).
// Also keeps MM_INDEX/MM_DATA pairs atomic.
static IOLock *gVramLock;

static IOLock *vramLock() {
	if (!gVramLock) {
		auto l = IOLockAlloc();
		if (l && !OSCompareAndSwapPtr(nullptr, l, &gVramLock))
			IOLockFree(l);
	}
	return gVramLock;
}

static bool findVram() {
	if (gVram)
		return true;
	auto matching = IOService::serviceMatching("IOPCIDevice");
	auto iter = matching ? IOService::getMatchingServices(matching) : nullptr;
	if (matching)
		matching->release();
	if (!iter)
		return false;
	while (auto svc = OSDynamicCast(IOService, iter->getNextObject())) {
		auto devId = dataU32(svc, "device-id");
		if (dataU32(svc, "vendor-id") != 0x1002 || (dataU32(svc, "class-code") >> 16) != 0x03 || (devId != 0x1681 && devId != 0x73ff) || dataU32(svc,"revision-id") != 3 || dataU32(svc,"subsystem-vendor-id") != 0x17aa || dataU32(svc,"subsystem-id") != 0x3b1e)
			continue;
		for (unsigned i = 0; i < svc->getDeviceMemoryCount(); i++) {
			auto mem = svc->getDeviceMemoryWithIndex(i);
			if (mem && mem->getLength() >= 0x10000000) {
				gVram = mem;
                gVram->retain(); // cached descriptor has its own lifetime reference
				break;
			}
		}
		break;
	}
	iter->release();
	return gVram != nullptr;
}

// Indirect VRAM access through MM_INDEX (0x0) / MM_DATA (0x1) / MM_INDEX_HI (0x6), as Linux
// amdgpu_device_mm_access does, for offsets beyond the CPU-visible BAR window.
static uint32_t vramReadIndirect32(uint64_t off) {
	mmioWrite(0x0, static_cast<uint32_t>(off) | 0x80000000u);
	mmioWrite(0x6, static_cast<uint32_t>(off >> 31));
	return mmioRead(0x1);
}

// Kernel pointer to VRAM offset off (inside the BAR window), via a one-page mapping; null if unavailable.
static volatile uint8_t *vramPtr(uint64_t off) {
	uint64_t page = off & ~0xFFFULL;
	if (page != gVramPageOff) {
		if (gVramPage)
			gVramPage->release();
		gVramPage = gVram->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapInhibitCache, page, 0x1000);
		gVramPageOff = gVramPage ? page : ~0ULL;
		if (!gVramPage)
			return nullptr;
	}
	return reinterpret_cast<volatile uint8_t *>(gVramPage->getVirtualAddress() + (off & 0xFFF));
}

uint64_t vramRead64(uint64_t off) {
	if (!findVram())
		return ~0ULL;
	auto l = vramLock();
	if (!l)
		return ~0ULL;
	IOLockLock(l);
	uint64_t v;
	if (off + 8 > gVram->getLength())
		v = vramReadIndirect32(off) | (uint64_t)vramReadIndirect32(off + 4) << 32;
	else {
		auto p = vramPtr(off);
		v = p ? *reinterpret_cast<volatile uint64_t *>(p) : ~0ULL;
	}
	IOLockUnlock(l);
	return v;
}

bool vramWrite32(uint64_t off, uint32_t value) {
	if (!findVram() || off + 4 > gVram->getLength())
		return false;
	auto l = vramLock();
	if (!l)
		return false;
	IOLockLock(l);
	auto p = vramPtr(off);
	if (p)
		*reinterpret_cast<volatile uint32_t *>(p) = value;
	IOLockUnlock(l);
	return p != nullptr;
}

// OSSSYS (IH) rings 0-2: {cntl, rptr, wptr} offsets from OSSSYS seg0 base 0x10A0 (osssys 5.0 layout).
// A VM run leaves IH ring 0 enabled with unacknowledged entries: the 680M then holds INTx asserted
// (PCI STATUS bit 3, even with no VM running) and keeps DMA-writing ring entries / the wptr
// writeback into the IOVAs the old guest set up, which the next guest's RAM reuses. In the guest
// INTx pin A shares IRQ 10 with the ICH9 SATA controller, so AppleAHCI's handler spins and the
// whole boot crawls (restart3: AHCI command timeouts, vCPU in AppleAHCI::PrimaryInterruptHandler).
// Disable and drain the rings like Linux navi10_ih_disable_interrupts (-rmbnoihq: don't).
void quiesceIh(const char *why) {
	if (checkKernelArgument("-rmbnoihq"))
		return;
	static constexpr uint32_t Base = 0x10A0;
	static constexpr struct { uint32_t cntl, rptr, wptr; } rings[] {{0x80, 0x83, 0x84}, {0x8c, 0x8f, 0x90}, {0x98, 0x9b, 0x9c}};
	for (size_t i = 0; i < arrsize(rings); i++) {
		auto &r = rings[i];
		uint32_t cntl = mmioRead(Base + r.cntl), rptr = mmioRead(Base + r.rptr), wptr = mmioRead(Base + r.wptr);
		if (cntl == 0xFFFFFFFF) {
			LOG("ihq: %s: register aperture not readable", why);
			return;
		}
		if (i == 0)
			cntl &= ~(1U << 17);  // ENABLE_INTR (ring 0 only)
		mmioWrite(Base + r.cntl, cntl & ~1U);  // RB_ENABLE
		mmioWrite(Base + r.rptr, 0);
		mmioWrite(Base + r.wptr, 0);
		LOG("ihq: %s: ring %zu cntl %08x rptr %x wptr %x -> cntl %08x rptr %x wptr %x", why, i, cntl, rptr, wptr,
		    mmioRead(Base + r.cntl), mmioRead(Base + r.rptr), mmioRead(Base + r.wptr));
	}
}

static IONotifier *gPciNotifier;
static IOPCIDevice *gGpuPci;
static IOPCIAddressSpace gGpuSpace;
// At publish macOS has not enabled memory decode yet: turn it on for the quiesce, then restore
// COMMAND. If INTx is still pending afterwards, set Interrupt Disable (Apple's driver uses MSI).
// Virtual calls only (no IOPCIFamily symbol imports); the nub was identified by its properties.
static struct { IOPCIDevice *pci; IOPCIAddressSpace sp; const char *name; } gIrqDevs[8];
static int gIrqDevCount;

static bool pciPublished(void *, void *, IOService *svc, IONotifier *) {
	uint32_t cls = dataU32(svc, "class-code");
	if ((cls == 0x010802 || cls == 0x0c0330) && gIrqDevCount < static_cast<int>(arrsize(gIrqDevs))) {
		auto &d = gIrqDevs[gIrqDevCount++];
		d.pci = static_cast<IOPCIDevice *>(svc);
		d.sp.bits = dataU32(svc, "reg") & 0x00FFFF00;
		d.name = cls == 0x010802 ? "nvme" : "xhci";
	}
	if (dataU32(svc, "vendor-id") != 0x1002 || (cls >> 16) != 0x03)
		return true;
	auto pci = static_cast<IOPCIDevice *>(svc);
	IOPCIAddressSpace sp;
	sp.bits = dataU32(svc, "reg") & 0x00FFFF00;  // phys.hi: bus/device/function, config space
	gGpuPci = pci, gGpuSpace = sp;
	uint16_t cmd = pci->configRead16(sp, static_cast<UInt8>(kIOPCIConfigCommand)), st = pci->configRead16(sp, static_cast<UInt8>(kIOPCIConfigStatus));
	pci->configWrite16(sp, static_cast<UInt8>(kIOPCIConfigCommand), cmd | kIOPCICommandMemorySpace);
	quiesceIh("at PCI publish");
	uint16_t st2 = pci->configRead16(sp, static_cast<UInt8>(kIOPCIConfigStatus));
	uint16_t newCmd = cmd;
	if ((st2 & 0x8) && !checkKernelArgument("-rmbkeepintx"))
		newCmd |= kIOPCICommandInterruptDisable;
	pci->configWrite16(sp, static_cast<UInt8>(kIOPCIConfigCommand), newCmd);
	LOG("ihq: PCI command %04x status %04x -> after quiesce status %04x, command %04x (status now %04x)", cmd, st, st2,
	    pci->configRead16(sp, static_cast<UInt8>(kIOPCIConfigCommand)), pci->configRead16(sp, static_cast<UInt8>(kIOPCIConfigStatus)));
	return true;
}

// GPU interrupt delivery as the device sees it: COMMAND/STATUS (INTx disable, INTx pending) and
// the MSI capability (enable, address, data). "-" when the nub was never published.
int gpuIrqState(char *buf, int n) {
	if (!gGpuPci)
		return snprintf(buf, n, "-");
	auto r8 = [](uint8_t o) { return gGpuPci->configRead8(gGpuSpace, o); };
	auto r16 = [](uint8_t o) { return gGpuPci->configRead16(gGpuSpace, o); };
	auto r32 = [](uint8_t o) { return gGpuPci->configRead32(gGpuSpace, o); };
	int k = snprintf(buf, n, "cmd %04x st %04x", r16(kIOPCIConfigCommand), r16(kIOPCIConfigStatus));
	for (uint8_t c = r8(0x34) & 0xFC, i = 0; c && i < 48; c = r8(c + 1) & 0xFC, i++) {
		if (r8(c) == 0x05) {
			uint16_t ctl = r16(c + 2);
			bool b64 = ctl & 0x80;
			k += snprintf(buf + k, n - k, " msi ctl %04x addr %x_%08x data %04x", ctl, b64 ? r32(c + 8) : 0, r32(c + 4),
			              r16(b64 ? c + 12 : c + 8));
		} else if (r8(c) == 0x11) {
			k += snprintf(buf + k, n - k, " msix ctl %04x", r16(c + 2));
		}
	}
	return k;
}

// Where NVMe / xHCI send their interrupts: "<name> bb:dd.f st <status> msi d<dest APIC> v<vector>" or
// "msix <ctl> [i:d<dest> v<vector>(m = masked)]" for the first 4 table entries (table read through an
// uncached map of its BAR). Matched against the CPU 0 LAPIC IRR/ISR at the stall.
int pciIrqTargets(char *buf, int n) {
	int k = 0;
	for (int i = 0; i < gIrqDevCount && k < n; i++) {
		auto pci = gIrqDevs[i].pci;
		auto sp = gIrqDevs[i].sp;
		auto r8 = [&](uint8_t o) { return pci->configRead8(sp, o); };
		auto r16 = [&](uint8_t o) { return pci->configRead16(sp, o); };
		auto r32 = [&](uint8_t o) { return pci->configRead32(sp, o); };
		k += snprintf(buf + k, n - k, "%s%s %02x:%02x.%x st %04x", k ? "; " : "", gIrqDevs[i].name, (sp.bits >> 16) & 0xFF,
		              (sp.bits >> 11) & 0x1F, (sp.bits >> 8) & 7, r16(kIOPCIConfigStatus));
		for (uint8_t c = r8(0x34) & 0xFC, j = 0; c && j < 48 && k < n; c = r8(c + 1) & 0xFC, j++) {
			if (r8(c) == 0x05) {
				uint16_t ctl = r16(c + 2);
				uint32_t addr = r32(c + 4);
				uint16_t data = r16(ctl & 0x80 ? c + 12 : c + 8);
				k += snprintf(buf + k, n - k, " msi %04x d%x v%x", ctl, (addr >> 12) & 0xFF, data & 0xFF);
			} else if (r8(c) == 0x11) {
				uint16_t ctl = r16(c + 2);
				uint32_t tbl = r32(c + 4), bir = tbl & 7;
				k += snprintf(buf + k, n - k, " msix %04x", ctl);
				if (!(ctl & 0x8000) || bir > 5)
					continue;
				uint8_t barReg = static_cast<uint8_t>(0x10 + 4 * bir);
				uint32_t lo = r32(barReg);
				uint64_t pa = lo & ~0xFULL;
				if ((lo & 6) == 4)
					pa |= static_cast<uint64_t>(r32(barReg + 4)) << 32;
				pa += tbl & ~7U;
				uint32_t entries = (ctl & 0x7FF) + 1;
				entries = entries < 4 ? entries : 4;
				auto md = IODeviceMemory::withRange(pa, entries * 16);
				auto map = md ? md->map(kIOMapInhibitCache) : nullptr;
				if (map) {
					auto t = reinterpret_cast<volatile uint32_t *>(map->getVirtualAddress());
					for (uint32_t e = 0; e < entries && k < n; e++)
						k += snprintf(buf + k, n - k, " [%u:d%x v%x%s]", e, (t[e * 4] >> 12) & 0xFF, t[e * 4 + 2] & 0xFF,
						              t[e * 4 + 3] & 1 ? "m" : "");
					map->release();
				}
				if (md)
					md->release();
			}
		}
	}
	return k;
}


void ihQuiesceAtBoot() {
	auto matching = IOService::serviceMatching("IOPCIDevice");
	if (!matching)
		return;
	gPciNotifier = IOService::addMatchingNotification(gIOFirstPublishNotification, matching, pciPublished, nullptr);
	matching->release();
	LOG("ihq: PCI publish notifier %s", gPciNotifier ? "armed" : "MISSING");
}
