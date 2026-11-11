//
//  wlandiag: read-only state of the MT7922 Wi-Fi nub (14c3:0616 at 02:00.0) and its root port GPP7
//  (1022:14ba at 00:02.3). 11-09 boot: IOPCIFamily published WLAN with "Capability Offsets" = {}, no
//  MSI/PM properties and the driverkit bridge entitlement, i.e. its own config reads returned 0xFF
//  (IOPCIBridge::publishNub +0x1e5 fills the 16-dword match shadow from extendedConfigRead32, which
//  returns ~0 when IOPCIDevice::configAccess refuses). The shadow then never matches IOPCIMatch
//  0x061614c3, so AirportMtkWlan's probe never runs. The configurator itself read the device at 1.14 s.
//  This module tells apart:
//    - device not answering: the raw config reads (space path, no configAccess) give ffffffff;
//      the read time separates an immediate unsupported request (link down) from a completion timeout;
//    - access gate closed: raw reads valid, nub-path reads (configAccess) ~0, and the gate fields
//      (inactive, reserved+0x62 protect mask, parent bridge reserved+0x40 bit 0) say which check.
//  Watch: from the host bridge nub (00:00.0, published ~2 s) until the WLAN nub is published (~12 s),
//  every 250 ms one raw read of the WLAN id plus GPP7 link / device control+status, logged on change,
//  so the log shows when the device stops answering. Snapshots 2 / 10 / 60 s after publish.
//  A WLAN read slower than SlowReadUs stops all further WLAN reads (11-09 12:19 panic: CPU 0 spun in a
//  VoodooI2C interrupt handler on an MMIO read; a stalled config read must not be repeated).
//  Everything runs in our own thread (the publish callback only records nubs). No config writes, no MMIO,
//  no properties changed. Private IOPCIFamily fields are read only when the in-memory LC_UUID is the 25G229
//  one and the configAccess / publishNub / probeBusGated bytes that use them match. -rmbnowlandiag = off.
//

#include "rmb.hpp"
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <mach-o/loader.h>
#include <mach/kmod.h>
#include <stdarg.h>
#include <string.h>

namespace {

// com.apple.iokit.IOPCIFamily in BootKernelExtensions.kc 25G229 (__TEXT vmaddr 0xffffff80029e5000, size 0x54000).
const uint8_t IopciUuid[16] {0x0d, 0x91, 0xf1, 0xaf, 0x74, 0x11, 0x3a, 0x0f, 0x9a, 0x7e, 0x72, 0xd5, 0x9a, 0x2d, 0xa7, 0x5f};
constexpr uint32_t RvaConfigAccessGate = 0x1d2d4;  // IOPCIDevice::configAccess(bool) +0x16
constexpr uint32_t RvaPublishNubShadow = 0x9641;   // IOPCIBridge::publishNub +0x1e5
constexpr uint32_t RvaProbeBusHeader = 0xb125;     // IOPCIBridge::probeBusGated +0x1f5
constexpr uint32_t RvaExtendedConfigRead32 = 0x1b67a;  // IOPCIDevice::extendedConfigRead32(unsigned long long)
// Bytes +0x0f..: call configAccess (rva 0x1d2be); cmp $0xffc,%r14. The first 15 bytes (push rbp .. xor esi,esi)
// are replaced at boot by our own pcidevid routeFunctionLong (kern_start.cpp routePciDevid, spoof32 wrapper that
// is transparent for non-1002:1681 functions), so they are not compared.
constexpr uint32_t ExtendedConfigRead32Skip = 0x0f;
const uint8_t ExtendedConfigRead32Body[8] {0xe8, 0x30, 0x1c, 0x00, 0x00, 0x49, 0x81, 0xfe};
// reserved = this+0xa0 (!= 0), parent = this+0x88 (!= 0), parent+0x98 -> +0x40 bit 0 (children in reset),
// (write + 1) & reserved+0x62 (protect mask).
const uint8_t ConfigAccessGate[49] {0x48, 0x8b, 0x83, 0xa0, 0x00, 0x00, 0x00, 0x48, 0x85, 0xc0, 0x74, 0x27, 0x48, 0x8b,
	0x8b, 0x88, 0x00, 0x00, 0x00, 0x48, 0x85, 0xc9, 0x74, 0x1b, 0x48, 0x8b, 0x89, 0x98, 0x00, 0x00, 0x00, 0xf6, 0x41,
	0x40, 0x01, 0x75, 0x0e, 0x41, 0x0f, 0xb6, 0xce, 0xff, 0xc1, 0x0f, 0xb6, 0x40, 0x62, 0x85, 0xc1};
// reserved+0xa5 = 2; this+0xb0 = shadow; shadow[i] = extendedConfigRead32(i * 4), i < 16.
const uint8_t PublishNubShadow[54] {0x49, 0x8b, 0x86, 0xa0, 0x00, 0x00, 0x00, 0xc6, 0x80, 0xa5, 0x00, 0x00, 0x00, 0x02,
	0x4d, 0x89, 0xbe, 0xb0, 0x00, 0x00, 0x00, 0x45, 0x31, 0xe4, 0x4c, 0x89, 0xf7, 0x4c, 0x89, 0xe6, 0xe8, 0x16, 0x20,
	0x01, 0x00, 0x49, 0x8b, 0x8e, 0xb0, 0x00, 0x00, 0x00, 0x42, 0x89, 0x04, 0x21, 0x49, 0x83, 0xc4, 0x04, 0x49, 0x83,
	0xfc, 0x40};
// reserved+0x60 = extendedConfigRead8(0x0e) & 0x7f (header type).
const uint8_t ProbeBusHeader[16] {0x24, 0x7f, 0x48, 0x8b, 0x8b, 0xa0, 0x00, 0x00, 0x00, 0x88, 0x41, 0x60, 0x48, 0x8b,
	0x41, 0x78};

constexpr uint32_t HostReg = 0x000000;  // bus 0, device 0, function 0 (1022:14b5 root complex)
constexpr uint32_t Gpp7Reg = 0x001300;  // bus 0, device 2, function 3
// A config read answered by the root port itself (link down: unsupported request) takes ~1 us; a completion
// timeout takes milliseconds and holds the CPU (and possibly the I/O path) for that long.
constexpr unsigned SlowReadUs = 2000;
constexpr unsigned WatchMs = 250, WatchLogMax = 48;

IONotifier *gNotifier;
IOPCIDevice *volatile gWlan, *volatile gGpp7, *volatile gHost;
IOPCIAddressSpace gWlanSp, gGpp7Sp;
bool gPrivOk;
bool gWlanReadsStopped;  // a WLAN read exceeded SlowReadUs: no further WLAN reads this boot
// IOPCIFamily is not an allowed import (make check: kernel + Lilu only): called at its verified address.
using ExtRead32 = UInt32 (*)(IOPCIDevice *, IOByteCount);
ExtRead32 gExtRead32;

uint32_t dataU32(IORegistryEntry *e, const char *key) {
	auto d = OSDynamicCast(OSData, e->getProperty(key));
	return d && d->getLength() >= 4 ? *static_cast<const uint32_t *>(d->getBytesNoCopy()) : 0;
}

bool kptr(const void *p) { return reinterpret_cast<uintptr_t>(p) >= 0xffffff8000000000ULL; }

// IOPCIFamily __TEXT when the in-memory image is the 25G229 one and the three field users match.
bool verifyIopci() {
	for (auto k = kmodList(); k; k = k->next) {
		if (strncmp(k->name, "com.apple.iokit.IOPCIFamily", KMOD_MAX_NAME))
			continue;
		auto base = static_cast<mach_vm_address_t>(k->address);
		auto hdr = reinterpret_cast<const mach_header_64 *>(base);
		if (!base || hdr->magic != MH_MAGIC_64 || hdr->ncmds > 64 || hdr->sizeofcmds > 0x4000) {
			LOG("wlandiag: IOPCIFamily: no 64-bit Mach-O header at the kmod address, private fields not read");
			return false;
		}
		bool uuidOk = false;
		uint64_t textSize = 0;
		auto p = reinterpret_cast<const uint8_t *>(base + sizeof(mach_header_64));
		auto end = p + hdr->sizeofcmds;
		for (uint32_t i = 0; i < hdr->ncmds && p + sizeof(load_command) <= end; i++) {
			auto lc = reinterpret_cast<const load_command *>(p);
			if (lc->cmdsize < sizeof(load_command) || p + lc->cmdsize > end)
				break;
			if (lc->cmd == LC_UUID && lc->cmdsize >= sizeof(uuid_command))
				uuidOk = !memcmp(reinterpret_cast<const uuid_command *>(p)->uuid, IopciUuid, sizeof(IopciUuid));
			else if (lc->cmd == LC_SEGMENT_64 && lc->cmdsize >= sizeof(segment_command_64) &&
			         !strncmp(reinterpret_cast<const segment_command_64 *>(p)->segname, SEG_TEXT, 16))
				textSize = reinterpret_cast<const segment_command_64 *>(p)->vmsize;
			p += lc->cmdsize;
		}
		auto at = [&](uint32_t rva, const uint8_t *want, size_t n) {
			return rva + n <= textSize && !memcmp(reinterpret_cast<const void *>(base + rva), want, n);
		};
		bool gate = at(RvaConfigAccessGate, ConfigAccessGate, sizeof(ConfigAccessGate));
		bool shadow = at(RvaPublishNubShadow, PublishNubShadow, sizeof(PublishNubShadow));
		bool header = at(RvaProbeBusHeader, ProbeBusHeader, sizeof(ProbeBusHeader));
		bool ext = at(RvaExtendedConfigRead32 + ExtendedConfigRead32Skip, ExtendedConfigRead32Body,
		              sizeof(ExtendedConfigRead32Body));
		LOG("wlandiag: IOPCIFamily LC_UUID %s, __TEXT %llx, configAccess %s, publishNub %s, probeBusGated %s, "
		    "extendedConfigRead32 %s",
		    uuidOk ? "25G229" : "MISMATCH", textSize, gate ? "ok" : "DIFF", shadow ? "ok" : "DIFF", header ? "ok" : "DIFF",
		    ext ? "ok" : "DIFF");
		if (!(uuidOk && gate && shadow && header && ext))
			return false;
		gExtRead32 = reinterpret_cast<ExtRead32>(base + RvaExtendedConfigRead32);
		return true;
	}
	LOG("wlandiag: IOPCIFamily not in the kmod list, private fields not read");
	return false;
}

// Raw config reads: IOPCIDevice::configRead*(space, offset) forward to the parent bridge without
// configAccess. Extended offsets use IOPCIDevice::extendedConfigRead32's encoding (bits 24-27).
struct Raw {
	IOPCIDevice *pci;
	IOPCIAddressSpace sp;
	uint8_t r8(uint8_t o) const { return pci->configRead8(sp, o); }
	uint16_t r16(uint8_t o) const { return pci->configRead16(sp, o); }
	uint32_t r32(uint8_t o) const { return pci->configRead32(sp, o); }
	uint32_t x32(uint16_t o) const {
		IOPCIAddressSpace e;
		e.bits = (sp.bits & 0xf0ffffffU) | ((static_cast<uint32_t>(o) << 16) & 0x0f000000U);
		return pci->configRead32(e, static_cast<UInt8>(o));
	}
};

IOPCIAddressSpace space(uint32_t bits) {
	IOPCIAddressSpace s;
	s.bits = bits;
	return s;
}

// One timed WLAN read; sets gWlanReadsStopped when it was a completion timeout rather than an answer.
struct Timed {
	uint32_t v, us;
};
Timed wlanRead32(const Raw &d, uint8_t o, const char *tag) {
	uint64_t t0 = mach_absolute_time();
	uint32_t v = d.r32(o);
	uint64_t ns = 0;
	absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
	uint32_t us = ns / 1000 > 0xffffffffULL ? 0xffffffffU : static_cast<uint32_t>(ns / 1000);
	if (us > SlowReadUs && !gWlanReadsStopped) {
		gWlanReadsStopped = true;
		LOG("wlandiag: %s WLAN config read +%02x took %u us (> %u): completion timeout, no further WLAN reads", tag, o,
		    us, SlowReadUs);
	}
	return {v, us};
}

// snprintf at buf + k, never past n: kernel snprintf returns the untruncated length, so n - k could go negative.
__printflike(4, 5) void app(char *buf, int n, int &k, const char *fmt, ...) {
	if (k >= n - 1)
		return;
	va_list va;
	va_start(va, fmt);
	int r = vsnprintf(buf + k, static_cast<size_t>(n - k), fmt, va);
	va_end(va);
	if (r > 0)
		k = k + r < n - 1 ? k + r : n - 1;
}

uint8_t pcieCap(const Raw &d) {
	uint8_t c = d.r8(0x34) & 0xFC;
	for (int i = 0; c && i < 48; c = d.r8(c + 1) & 0xFC, i++)
		if (d.r8(c) == 0x10)
			return c;
	return 0;
}

// Capability list: "id@off" plus the status registers that matter for a link / power / interrupt problem.
int caps(const Raw &d, char *buf, int n) {
	int k = 0;
	buf[0] = 0;
	uint8_t c = d.r8(0x34) & 0xFC;
	for (int i = 0; c && i < 48 && k < n; c = d.r8(c + 1) & 0xFC, i++) {
		uint8_t id = d.r8(c);
		app(buf, n, k, " %02x@%02x", id, c);
		if (id == 0x01)
			app(buf, n, k, " pmc %04x pmcsr %04x", d.r16(c + 2), d.r16(c + 4));
		else if (id == 0x10)
			app(buf, n, k, " pcie %04x dev %08x lnkcap %08x lnk %08x slt %08x dev2 %08x lnk2 %08x", d.r16(c + 2),
			    d.r32(c + 8), d.r32(c + 0x0c), d.r32(c + 0x10), d.r32(c + 0x18), d.r32(c + 0x28), d.r32(c + 0x30));
		else if (id == 0x05)
			app(buf, n, k, " msi %04x", d.r16(c + 2));
		else if (id == 0x11)
			app(buf, n, k, " msix %04x", d.r16(c + 2));
	}
	return k;
}

// Extended capability list; AER (id 1): uncorrectable status / mask / severity, correctable status,
// root error status when the function is a root port; L1 PM substates (id 0x1e): capability, control 1 / 2.
int extCaps(const Raw &d, bool rootPort, char *buf, int n) {
	int k = 0;
	buf[0] = 0;
	uint16_t c = 0x100;
	for (int i = 0; c >= 0x100 && i < 32 && k < n; i++) {
		uint32_t h = d.x32(c);
		if (h == 0 || h == 0xffffffffU) {
			app(buf, n, k, " %08x@%03x", h, c);
			break;
		}
		app(buf, n, k, " %04x@%03x", h & 0xffff, c);
		if ((h & 0xffff) == 0x0001) {
			app(buf, n, k, " unc %08x mask %08x sev %08x cor %08x", d.x32(c + 4), d.x32(c + 8),
			              d.x32(c + 0x0c), d.x32(c + 0x10));
			if (rootPort)
				app(buf, n, k, " rootsta %08x src %08x", d.x32(c + 0x30), d.x32(c + 0x34));
		} else if ((h & 0xffff) == 0x001e) {
			app(buf, n, k, " l1ss cap %08x ctl1 %08x ctl2 %08x", d.x32(c + 4), d.x32(c + 8), d.x32(c + 0x0c));
		}
		c = static_cast<uint16_t>((h >> 20) & 0xffc);
	}
	return k;
}

// Before the WLAN nub exists: everything through the host bridge nub's raw path (AppleACPIPCI).
struct Watch {
	bool started;
	uint8_t secBus, cap;
	uint32_t lastId, lastLnk, lastDev, samples, logged;

	void sample(unsigned ms) {
		Raw g {gHost, space(Gpp7Reg)};
		if (!started) {
			started = true;
			uint32_t bus = g.r32(0x18);
			secBus = static_cast<uint8_t>(bus >> 8);
			cap = pcieCap(g);
			LOG("wlandiag: watch from %u ms via host bridge nub: GPP7 id %08x bus %08x pcie cap %02x, WLAN at %02x:00.0",
			    ms, g.r32(0x00), bus, cap, secBus);
		}
		uint32_t lnk = cap ? g.r32(cap + 0x10) : 0;  // link control | link status << 16
		uint32_t dev = cap ? g.r32(cap + 0x08) : 0;  // device control | device status << 16
		Timed id {lastId, 0};
		// not while wlanfix is resetting / reprogramming the function (initializeNub, IOPCIFamily workloop)
		bool read = !gWlanReadsStopped && secBus && secBus != 0xff && wlanFixState() != 1;
		if (read)
			id = wlanRead32(Raw {gHost, space(static_cast<uint32_t>(secBus) << 16)}, 0x00, "watch");
		samples++;
		if (samples == 1 || id.v != lastId || lnk != lastLnk || dev != lastDev) {
			if (logged++ < WatchLogMax)
				LOG("wlandiag: watch %u ms: WLAN id %08x (%u us%s), GPP7 lnkctl|sta %08x devctl|sta %08x", ms, id.v,
				    id.us, read ? "" : ", not read", lnk, dev);
			lastId = id.v, lastLnk = lnk, lastDev = dev;
		}
	}
};

void snapWlan(const char *tag) {
	auto pci = gWlan;
	Raw d {pci, gWlanSp};
	char buf[640];
	bool alive = false;
	if (gWlanReadsStopped) {
		LOG("wlandiag: %s WLAN %02x:%02x.%x not read (an earlier read was a completion timeout)", tag,
		    gWlanSp.s.busNum, gWlanSp.s.deviceNum, gWlanSp.s.functionNum);
	} else {
		Timed id = wlanRead32(d, 0x00, tag);
		alive = id.v != 0xffffffffU && !gWlanReadsStopped;
		if (!alive) {
			LOG("wlandiag: %s WLAN %02x:%02x.%x raw id %08x (%u us): not answering or slow, other WLAN registers not read",
			    tag, gWlanSp.s.busNum, gWlanSp.s.deviceNum, gWlanSp.s.functionNum, id.v, id.us);
		} else {
			LOG("wlandiag: %s WLAN %02x:%02x.%x raw id %08x (%u us) cmdst %08x class %08x hdr %08x bar %08x %08x %08x "
			    "%08x %08x %08x ss %08x cap %02x int %08x",
			    tag, gWlanSp.s.busNum, gWlanSp.s.deviceNum, gWlanSp.s.functionNum, id.v, id.us, d.r32(0x04),
			    d.r32(0x08), d.r32(0x0c), d.r32(0x10), d.r32(0x14), d.r32(0x18), d.r32(0x1c), d.r32(0x20), d.r32(0x24),
			    d.r32(0x2c), d.r8(0x34), d.r32(0x3c));
			caps(d, buf, sizeof(buf));
			LOG("wlandiag: %s WLAN caps%s", tag, buf);
			extCaps(d, false, buf, sizeof(buf));
			LOG("wlandiag: %s WLAN ext%s", tag, buf);
		}
	}
	if (!gPrivOk) {
		LOG("wlandiag: %s WLAN inactive %d (IOPCIFamily not verified: nub path and gate not read)", tag, pci->isInactive());
		return;
	}
	// Through configAccess: the call publishNub +0x1e5 used to fill the match shadow. Only when the device
	// answers raw reads (otherwise it would be one more read of a device that does not answer).
	if (alive) {
		uint32_t nid = gExtRead32(pci, 0x00), ncls = gExtRead32(pci, 0x08);
		LOG("wlandiag: %s WLAN nub path id %08x class %08x (%s), inactive %d", tag, nid, ncls,
		    nid != 0xffffffffU ? "answers" : "REFUSED or dead", pci->isInactive());
	}
	auto self = reinterpret_cast<const uint8_t *>(pci);
	auto res = *reinterpret_cast<const uint8_t *const *>(self + 0xa0);
	auto parent = *reinterpret_cast<const uint8_t *const *>(self + 0x88);
	auto shadow = *reinterpret_cast<const uint32_t *const *>(self + 0xb0);
	uint32_t sp = *reinterpret_cast<const uint32_t *>(self + 0xa8);
	int pres = -1;
	if (kptr(parent)) {
		auto pr = *reinterpret_cast<const uint8_t *const *>(parent + 0x98);
		pres = kptr(pr) ? pr[0x40] : -2;
	}
	LOG("wlandiag: %s WLAN gate: inactive %d reserved %s hdr %02x protect %02x attach %02x, parent %s reserved+0x40 %d "
	    "(bit0 = children in reset), space %08x; shadow id %08x cmd %08x class %08x hdr %08x",
	    tag, pci->isInactive(), kptr(res) ? "ok" : "NULL", kptr(res) ? res[0x60] : 0xff, kptr(res) ? res[0x62] : 0xff,
	    kptr(res) ? res[0x63] : 0xff, kptr(parent) ? "ok" : "NULL", pres, sp, kptr(shadow) ? shadow[0] : 0,
	    kptr(shadow) ? shadow[1] : 0, kptr(shadow) ? shadow[2] : 0, kptr(shadow) ? shadow[3] : 0);
}

void snapGpp7(const char *tag) {
	auto pci = gGpp7;
	if (!pci) {
		LOG("wlandiag: %s GPP7 00:02.3 nub not seen", tag);
		return;
	}
	Raw d {pci, gGpp7Sp};
	char buf[640];
	LOG("wlandiag: %s GPP7 raw id %08x cmdst %08x bus %08x io %08x mem %08x pfm %08x %08x %08x bctl %08x", tag,
	    d.r32(0x00), d.r32(0x04), d.r32(0x18), d.r32(0x1c), d.r32(0x20), d.r32(0x24), d.r32(0x28), d.r32(0x2c),
	    d.r32(0x3c));
	caps(d, buf, sizeof(buf));
	LOG("wlandiag: %s GPP7 caps%s", tag, buf);
	extCaps(d, true, buf, sizeof(buf));
	LOG("wlandiag: %s GPP7 ext%s", tag, buf);
}

// Publish callback: only records the three nubs (no config access in the publisher's context).
bool pciPublished(void *, void *, IOService *svc, IONotifier *) {
	uint32_t vid = dataU32(svc, "vendor-id"), did = dataU32(svc, "device-id"), reg = dataU32(svc, "reg") & 0x00FFFF00;
	if (!gHost && vid == 0x1022 && did == 0x14b5 && reg == HostReg) {
		svc->retain();
		gHost = static_cast<IOPCIDevice *>(svc);  // virtual calls only, as mmio.cpp
	} else if (!gGpp7 && vid == 0x1022 && did == 0x14ba && reg == Gpp7Reg) {
		gGpp7Sp.bits = reg;
		svc->retain();
		gGpp7 = static_cast<IOPCIDevice *>(svc);
	} else if (!gWlan && vid == 0x14c3 && did == 0x0616) {
		gWlanSp.bits = reg;
		svc->retain();
		gWlan = static_cast<IOPCIDevice *>(svc);
	}
	return true;
}

void diagThread(void *, wait_result_t) {
	IOSleep(1000);  // started at patcher ready (~1.15 s); configuration ends ~1.7 s, host bridge nub ~2-3 s
	gPrivOk = verifyIopci();
	auto matching = IOService::serviceMatching("IOPCIDevice");
	if (matching) {
		gNotifier = IOService::addMatchingNotification(gIOFirstPublishNotification, matching, pciPublished, nullptr);
		matching->release();
	}
	LOG("wlandiag: PCI publish notifier %s", gNotifier ? "armed" : "MISSING");
	unsigned ms = 0;
	Watch w {};
	for (; !gWlan && ms < 90000; ms += WatchMs) {
		if (gHost)
			w.sample(ms);
		IOSleep(WatchMs);
	}
	LOG("wlandiag: watch ended at %u ms: %u samples, last WLAN id %08x, GPP7 lnkctl|sta %08x devctl|sta %08x, WLAN "
	    "reads %s",
	    ms, w.samples, w.lastId, w.lastLnk, w.lastDev, gWlanReadsStopped ? "STOPPED (slow)" : "fast");
	if (!gWlan) {
		LOG("wlandiag: WLAN 14c3:0616 nub not published 90 s after the notifier");
		snapGpp7("no-wlan");
	} else {
		static const struct { unsigned ms; const char *tag; } at[] {{2000, "+2s"}, {10000, "+10s"}, {60000, "+60s"}};
		LOG("wlandiag: WLAN nub seen %u ms after the notifier", ms);
		wlanFixReport("publish");
		// A watch read that timed out before wlanfix ran says nothing about the function after it: read again.
		if (gWlanReadsStopped && wlanFixState() == 2) {
			gWlanReadsStopped = false;
			LOG("wlandiag: WLAN reads re-enabled after wlanfix");
		}
		unsigned done = 0;
		for (auto &a : at) {
			IOSleep(a.ms - done);
			done = a.ms;
			snapWlan(a.tag);
			snapGpp7(a.tag);
			wlanFixReport(a.tag);
		}
	}
	if (gNotifier)
		gNotifier->remove();
	gNotifier = nullptr;
	thread_terminate(current_thread());
}

}  // namespace

void wlanDiagStart() {
	if (checkKernelArgument("-rmbnowlandiag")) {
		LOG("wlandiag: disabled (-rmbnowlandiag)");
		return;
	}
	thread_t t;
	if (kernel_thread_start(diagThread, nullptr, &t) == KERN_SUCCESS)
		thread_deallocate(t);
	else
		LOG("wlandiag: thread start FAILED");
}
