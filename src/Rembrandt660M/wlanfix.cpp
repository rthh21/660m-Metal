//
//  wlanfix: MT7922 (14c3:0616 at 02:00.0, root port GPP7 00:02.3) brought back before IOPCIFamily reads it.
//
//  11-09 boots (9 of 9): the configurator read and programmed the function at 1.16 s (class, link caps, BARs),
//  but from the first wlandiag raw read (~2.9 s) every config read of 02:00.0 is a ~459 ms completion
//  timeout returning ffffffff while GPP7 keeps the link up (lnksta 7012: DLLLA, x1 5 GT/s, ASPM L1 + all four
//  L1 PM substates enabled on GPP7). probeBusGated then builds an empty capability dict, the publish shadow
//  never matches IOPCIMatch 0x061614c3 and AirportMtkWlan never probes. A config-dead function with the link
//  up is the MT7922 failure Linux works around with quirk_no_flr (commit 81f64e925c29: after FLR the device
//  stops answering config, a secondary bus reset recovers it). AirportMtkWlan itself has an FLR path
//  (mwx_pci_flr, devcap bit 28 via IOPCIDevice::extendedConfigRead32) and IOPCIFamily's resetFunction /
//  getResources use the same devcap bit (probeBusGated +0x7c2 -> reserved+0x10 -> supportsFLR).
//
//  11-09 07:51 boot (this module, SBR variant): at initializeNub the id read was already a 229 ms timeout; after
//  ASPM / L1SS off and a secondary bus reset (GPP7 lnksta 7012 -> f012: the link retrained) four more reads
//  timed out. The function was fine at 1.2156 s (configurator probe) and the failure is on the GPP7 side:
//  GPP7 Device Control 2 = 0x1006 (bit 12, 10-Bit Tag Requester Enable, set) and AER correctable status
//  0x2000 (Advisory Non-Fatal). probeBusGated +0xa93 sets devctl2 bit 12 on every nub whose devcap2 has bit 17
//  (10-Bit Tag Requester Supported) when its upstream config entry has devcap2 bit 16 (Completer Supported);
//  for a root port that is the host side, not the function below it. GPP7 / GPP8 / 00:01.1 (devcap2 0x7319ff)
//  qualify. NVMe 03:00.0 below GPP8 has devcap2 0x1081f (bit 16: 10-bit completer) and works. The MT7922 is a
//  5 GT/s function; a completer without bit 16 cannot return Tag[9:8], so the completion does not match and
//  every config / MMIO read from GPP7 becomes a completion timeout. An SBR resets the function, not the
//  port, so the failure stayed. The configurator's reads before probeBusGated used 8-bit tags and worked.
//
//  What this module does, once, for the first nub whose property table says 14c3:0616:
//    1. IOPCIBridge::initializeNub (vtable +0x888, called by probeBusGated +0x1b5 before
//       constructCapabilitiesDict and every other IOPCIFamily read of the nub) is routed; the original runs
//       first (space, parent), then the fix runs synchronously in the same gated context.
//    1a. Before the first read of the function: 10-Bit Tag Requester Enable cleared on the upstream port
//       (Device Control 2 bit 12; 8-bit tags work with every completer). wlanFixReport (publish, +2/+10/+60 s)
//       clears it again if anything set it back, and logs that.
//    2. Device answers (fast read of 0x061614c3): ASPM L0s/L1 off in link control (endpoint, then GPP7) and
//       the four L1 PM substate enables off (endpoint, then GPP7), the order Linux uses to disable them.
//       IOPCIFamily records the hardware ASPM state right after (expressASPMDefault), so it stays off.
//    3. Device does not answer: GPP7 ASPM / L1SS off, then secondary bus reset on GPP7 (bridge control
//       bit 6, 2 ms), wait for DLLLA (<= 1 s) + 100 ms, poll the id (<= 4 reads). If it answers again, the
//       BARs are put back from the nub's own "assigned-addresses" (what deviceApplyConfiguration wrote:
//       low dword, high dword for a 64-bit BAR), endpoint MPS = GPP7 MPS; command stays as reset left it
//       (the configurator found and restored cmd 0000). Nothing else is written when it stays dead.
//    4. Devcap FLR bit hidden for this one nub (quirk_no_flr analog): kern_start.cpp's routed
//       extendedConfigRead32 / configRead32 / 16-bit reads call wlanDevcapFilter*, which compares the device
//       pointer and offset only (no registry access, no logging in the config read path).
//  Gate: in-memory IOPCIFamily LC_UUID = 25G229 one and the bytes of initializeNub's head, the probeBusGated
//  call site, the devcap store and supportsFLR match. Raw accesses go through IOPCIDevice::configRead/Write
//  (space, offset) virtuals (vtable +0x850..+0x878), which forward to the parent bridge without configAccess.
//  -rmbnowlanfix = no route (nothing changes), -rmbwlanaspm = ASPM / L1SS left as firmware set them,
//  -rmbwlannosbr = no secondary bus reset (dead device only logged), -rmbwlantag10 = port 10-bit tags left
//  as IOPCIFamily set them (A/B check of the cause).
//

#include "rmb.hpp"
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <kern/clock.h>
#include <mach-o/loader.h>
#include <mach/kmod.h>
#include <string.h>

namespace {

// com.apple.iokit.IOPCIFamily in BootKernelExtensions.kc 25G229 (__TEXT vmaddr 0xffffff80029e5000, size 0x54000).
const uint8_t IopciUuid[16] {0x0d, 0x91, 0xf1, 0xaf, 0x74, 0x11, 0x3a, 0x0f, 0x9a, 0x7e, 0x72, 0xd5, 0x9a, 0x2d, 0xa7, 0x5f};
// IOPCIBridge::initializeNub(IOPCIDevice *, OSDictionary *) -> bool. push rbp; mov rbp,rsp; push r14; push rbx;
// mov rbx,rsi; mov r14,rdi; lea rax,[rsi+0xa8]; mov rcx,[rdi]; mov rsi,rdx; mov rdx,rax;
// call [rcx+0x870] (spaceFromProperties(from, &nub->space)); mov [rbx+0x88],r14 (nub->parent = this).
// Lilu's long route replaces the first 14 bytes (whole instructions end at +0x0d and +0x14, no rip-relative).
constexpr uint32_t RvaInitializeNub = 0x913e;
const uint8_t InitializeNubHead[42] {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x89, 0xf3, 0x49, 0x89, 0xfe, 0x48,
	0x8d, 0x86, 0xa8, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x0f, 0x48, 0x89, 0xd6, 0x48, 0x89, 0xc2, 0xff, 0x91, 0x70, 0x08,
	0x00, 0x00, 0x4c, 0x89, 0xb3, 0x88, 0x00, 0x00, 0x00};
// IOPCIBridge::probeBusGated +0x1a8: mov rdi,[rbp-0x40] (this); mov rax,[rdi]; mov rsi,rbx (nub);
// mov rdx,r14 (getPropertyTable()); call [rax+0x888] (initializeNub). constructCapabilitiesDict follows at +0x1e3.
constexpr uint32_t RvaProbeBusInitNubCall = 0xb0d8;
const uint8_t ProbeBusInitNubCall[19] {0x48, 0x8b, 0x7d, 0xc0, 0x48, 0x8b, 0x07, 0x48, 0x89, 0xde, 0x4c, 0x89, 0xf2, 0xff,
	0x90, 0x88, 0x08, 0x00, 0x00};
// probeBusGated +0x7be: add rsi,4 (PCIe cap + 4); call extendedConfigRead32 (rva 0x1b67a);
// mov rcx,[rbp-0x30]; mov rcx,[rcx+0xa0]; mov [rcx+0x10],eax (reserved->expressDeviceCapabilities = devcap).
constexpr uint32_t RvaProbeBusDevcap = 0xb6ee;
const uint8_t ProbeBusDevcap[23] {0x48, 0x83, 0xc6, 0x04, 0xe8, 0x83, 0xff, 0x00, 0x00, 0x48, 0x8b, 0x4d, 0xd0, 0x48, 0x8b,
	0x89, 0xa0, 0x00, 0x00, 0x00, 0x89, 0x41, 0x10};
// IOPCIDevice::supportsFLR(): return (reserved->expressDeviceCapabilities >> 28) & 1 (byte +0x13, bit 4).
constexpr uint32_t RvaSupportsFlr = 0x1ce62;
const uint8_t SupportsFlr[21] {0x55, 0x48, 0x89, 0xe5, 0x48, 0x8b, 0x87, 0xa0, 0x00, 0x00, 0x00, 0x8a, 0x40, 0x13, 0x24,
	0x10, 0xc0, 0xe8, 0x04, 0x5d, 0xc3};
// probeBusGated +0xa93: bt ebx,0x11 (nub config entry devcap2 bit 17, 10-Bit Tag Requester Supported); jae;
// mov rax,[rcx+0x78]; mov rax,[rax+0x20] (upstream config entry); test byte [rax+0x42],1 (its devcap2 bit 16,
// 10-Bit Tag Completer Supported); je; movzx esi,word [rcx+8]; add rsi,0x28 (PCIe cap + 0x28 = Device Control 2);
// call extendedConfigRead16 (rva 0x193c2); or eax,0x1000 (bit 12) -> extendedConfigWrite16 at +0xad3.
constexpr uint32_t RvaProbeBusTag10 = 0xb9c3;
const uint8_t ProbeBusTag10[38] {0x0f, 0xba, 0xe3, 0x11, 0x73, 0x4a, 0x48, 0x8b, 0x41, 0x78, 0x48, 0x8b, 0x40, 0x20, 0xf6,
	0x40, 0x42, 0x01, 0x74, 0x3c, 0x0f, 0xb7, 0x71, 0x08, 0x48, 0x83, 0xc6, 0x28, 0xe8, 0xde, 0xd9, 0x00, 0x00, 0x0d, 0x00,
	0x10, 0x00, 0x00};

constexpr uint32_t WlanId = 0x061614c3;
constexpr uint32_t DevcapFlr = 0x10000000;  // PCIe Device Capabilities bit 28: Function Level Reset capable
constexpr uint16_t Devctl2Tag10 = 0x1000;   // PCIe Device Control 2 bit 12: 10-Bit Tag Requester Enable
constexpr uint32_t Devcap2Tag10Comp = 0x10000;  // PCIe Device Capabilities 2 bit 16: 10-Bit Tag Completer Supported
constexpr unsigned SlowReadUs = 2000;       // same threshold as wlandiag: an answer takes ~1 us, a timeout ms

using InitNubFn = bool (*)(void *, IOPCIDevice *, OSDictionary *);
mach_vm_address_t gInitNubAddr;          // verified IOPCIFamily address (route source)
volatile mach_vm_address_t gOrgInitNub;  // Lilu trampoline to the original, stored right after the route
// 0 = not run, 1 = running (wlandiag's watch leaves the WLAN alone), 2 = done.
volatile uint32_t gState;
// Devcap filter: set once by the fix (before probeBusGated reads devcap), read in the config read path.
const void *volatile gMaskNub;
volatile uint32_t gMaskOff;  // PCIe cap + 4
volatile uint32_t gMasked;   // how many devcap reads had bit 28 cleared
char gSummary[200];          // one line for wlanFixReport
// Upstream port kept for wlanFixReport's 10-bit tag re-check (nub retained by fix, port identified, cap found).
IOPCIDevice *gPortNub;
uint32_t gPortSpace;
uint8_t gPortCap;
bool gKeepTag10;
uint32_t gTag10Cleared;  // how many times bit 12 was found set and cleared (fix + re-checks)

uint32_t dataU32(OSDictionary *d, const char *key) {
	auto o = OSDynamicCast(OSData, d->getObject(key));
	return o && o->getLength() >= 4 ? *static_cast<const uint32_t *>(o->getBytesNoCopy()) : 0xffffffffU;
}

bool imageVerified() {
	for (auto k = kmodList(); k; k = k->next) {
		if (strncmp(k->name, "com.apple.iokit.IOPCIFamily", KMOD_MAX_NAME))
			continue;
		auto base = static_cast<mach_vm_address_t>(k->address);
		auto hdr = reinterpret_cast<const mach_header_64 *>(base);
		if (!base || hdr->magic != MH_MAGIC_64 || hdr->ncmds > 64 || hdr->sizeofcmds > 0x4000) {
			LOG("wlanfix: IOPCIFamily: no 64-bit Mach-O header at the kmod address, not routed");
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
		bool head = at(RvaInitializeNub, InitializeNubHead, sizeof(InitializeNubHead));
		bool call = at(RvaProbeBusInitNubCall, ProbeBusInitNubCall, sizeof(ProbeBusInitNubCall));
		bool devcap = at(RvaProbeBusDevcap, ProbeBusDevcap, sizeof(ProbeBusDevcap));
		bool flr = at(RvaSupportsFlr, SupportsFlr, sizeof(SupportsFlr));
		bool tag = at(RvaProbeBusTag10, ProbeBusTag10, sizeof(ProbeBusTag10));
		LOG("wlanfix: IOPCIFamily LC_UUID %s, __TEXT %llx, initializeNub %s, probeBusGated call %s, devcap %s, "
		    "supportsFLR %s, 10-bit tag enable %s",
		    uuidOk ? "25G229" : "MISMATCH", textSize, head ? "ok" : "DIFF", call ? "ok" : "DIFF", devcap ? "ok" : "DIFF",
		    flr ? "ok" : "DIFF", tag ? "ok" : "DIFF");
		if (!(uuidOk && head && call && devcap && flr && tag))
			return false;
		gInitNubAddr = base + RvaInitializeNub;
		return true;
	}
	LOG("wlanfix: IOPCIFamily not in the kmod list, not routed");
	return false;
}

// Raw config access through the WLAN nub: IOPCIDevice::configRead/Write*(space, offset) forward to the parent
// bridge (IOPCI2PCIBridge -> GPP7 nub -> host bridge) with no configAccess gate, for any space on this host.
// Extended offsets use extendedConfigRead32 / Write32's encoding (offset bits 8-11 -> space bits 24-27).
struct Cfg {
	IOPCIDevice *pci;
	IOPCIAddressSpace sp;
	uint8_t r8(uint8_t o) const { return pci->configRead8(sp, o); }
	uint16_t r16(uint8_t o) const { return pci->configRead16(sp, o); }
	uint32_t r32(uint8_t o) const { return pci->configRead32(sp, o); }
	void w16(uint8_t o, uint16_t v) const { pci->configWrite16(sp, o, v); }
	void w32(uint8_t o, uint32_t v) const { pci->configWrite32(sp, o, v); }
	IOPCIAddressSpace ext(uint16_t o) const {
		IOPCIAddressSpace e;
		e.bits = (sp.bits & 0xf0ffffffU) | ((static_cast<uint32_t>(o) << 16) & 0x0f000000U);
		return e;
	}
	uint32_t x32(uint16_t o) const { return pci->configRead32(ext(o), static_cast<UInt8>(o)); }
	void xw32(uint16_t o, uint32_t v) const { pci->configWrite32(ext(o), static_cast<UInt8>(o), v); }
};

struct Timed {
	uint32_t v, us;
};
Timed timedId(const Cfg &d) {
	uint64_t t0 = mach_absolute_time();
	uint32_t v = d.r32(0x00);
	uint64_t ns = 0;
	absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
	return {v, ns / 1000 > 0xffffffffULL ? 0xffffffffU : static_cast<uint32_t>(ns / 1000)};
}

uint8_t pcieCap(const Cfg &d) {
	uint8_t c = d.r8(0x34) & 0xFC;
	for (int i = 0; c && i < 48; c = d.r8(c + 1) & 0xFC, i++)
		if (d.r8(c) == 0x10)
			return c;
	return 0;
}

// L1 PM Substates extended capability (id 0x1e), 0 when absent.
uint16_t l1ssCap(const Cfg &d) {
	uint16_t c = 0x100;
	for (int i = 0; c >= 0x100 && i < 32; i++) {
		uint32_t h = d.x32(c);
		if (h == 0 || h == 0xffffffffU)
			return 0;
		if ((h & 0xffff) == 0x001e)
			return c;
		c = static_cast<uint16_t>((h >> 20) & 0xffc);
	}
	return 0;
}

// Link control ASPM L0s / L1 (bits 1:0) off; returns the old link control.
uint16_t aspmOff(const Cfg &d, uint8_t cap) {
	uint16_t v = d.r16(cap + 0x10);
	if (v & 0x3)
		d.w16(cap + 0x10, v & ~0x3);
	return v;
}

// L1 PM Substates control 1: PCI-PM L1.2 / L1.1, ASPM L1.2 / L1.1 enables (bits 3:0) off; returns the old value.
uint32_t l1ssOff(const Cfg &d, uint16_t l1ss) {
	if (!l1ss)
		return 0;
	uint32_t v = d.x32(l1ss + 8);
	if (v != 0xffffffffU && (v & 0xf))
		d.xw32(l1ss + 8, v & ~0xfU);
	return v;
}

// Device Control 2 10-Bit Tag Requester Enable (bit 12) off; returns the old value, reads back so the write has
// completed before the caller sends the next request through this port.
uint16_t tag10Off(const Cfg &d, uint8_t cap) {
	uint16_t v = d.r16(cap + 0x28);
	if (v != 0xffff && (v & Devctl2Tag10)) {
		d.w16(cap + 0x28, v & ~Devctl2Tag10);
		(void)d.r16(cap + 0x28);
		gTag10Cleared++;
	}
	return v;
}

// BARs from the nub's "assigned-addresses" (IOPCIPhysicalAddress: physHi, physMid, physLo, lengthHi, lengthLo;
// physHi bits 7:0 = register). 64-bit is read from the BAR itself (the property always says 32-bit memory).
void restoreBars(const Cfg &w, OSDictionary *from) {
	auto aa = OSDynamicCast(OSData, from->getObject("assigned-addresses"));
	if (!aa || aa->getLength() < 20) {
		LOG("wlanfix: no assigned-addresses in the nub table, BARs left at reset values");
		return;
	}
	auto e = static_cast<const uint32_t *>(aa->getBytesNoCopy());
	unsigned n = aa->getLength() / 20;
	for (unsigned i = 0; i < n && i < 7; i++, e += 5) {
		uint8_t reg = static_cast<uint8_t>(e[0] & 0xff);
		bool bar = reg >= 0x10 && reg <= 0x24 && !(reg & 3);
		if (!bar && reg != 0x30) {
			LOG("wlanfix: assigned-addresses entry %u register %02x skipped", i, reg);
			continue;
		}
		uint32_t before = w.r32(reg);
		bool is64 = bar && (before & 0x7) == 0x4 && reg <= 0x20;  // memory BAR, type 10b = 64-bit
		w.w32(reg, e[2]);
		if (is64)
			w.w32(reg + 4, e[1]);
		LOG("wlanfix: BAR %02x %08x -> %08x%s%08x (want %08x%08x, size %x)", reg, before, w.r32(reg), is64 ? " hi " : " ",
		    is64 ? w.r32(reg + 4) : 0, e[1], e[2], e[4]);
	}
}

void fix(IOPCIDevice *nub, OSDictionary *from, uint32_t space) {
	bool keepAspm = checkKernelArgument("-rmbwlanaspm"), noSbr = checkKernelArgument("-rmbwlannosbr");
	gKeepTag10 = checkKernelArgument("-rmbwlantag10");
	Cfg w {nub, {}};
	w.sp.bits = space;
	uint8_t bus = static_cast<uint8_t>(space >> 16);
	// Upstream port: the nub's device-tree parent (probeBusGated iterates the DT children of the bridge nub, here
	// GPP7, "reg" 00001300); accepted only if it is a type 1 PCIe root / downstream port whose secondary bus is ours.
	auto parent = nub->getParentEntry(gIODTPlane);
	auto port = parent ? OSDynamicCast(OSData, parent->getProperty("reg")) : nullptr;
	Cfg g {nub, {}};
	g.sp.bits = port && port->getLength() >= 4 ? *static_cast<const uint32_t *>(port->getBytesNoCopy()) & 0x00ffff00U : 0;
	uint32_t gid = g.r32(0x00), gbus = g.r32(0x18);
	uint8_t ghdr = g.r8(0x0e) & 0x7f, gcap = pcieCap(g);
	uint16_t gtype = gcap ? (g.r16(gcap + 2) >> 4) & 0xf : 0xff;
	if (!port || ghdr != 1 || static_cast<uint8_t>(gbus >> 8) != bus || !gcap || (gtype != 4 && gtype != 6)) {
		LOG("wlanfix: upstream port not identified (dt parent %s, space %06x id %08x hdr %02x bus %08x cap %02x type %x): "
		    "nothing written",
		    parent ? parent->getName() : "none", g.sp.bits, gid, ghdr, gbus, gcap, gtype);
		snprintf(gSummary, sizeof(gSummary), "no upstream port");
		return;
	}
	uint16_t gl1ss = l1ssCap(g);
	nub->retain();  // kept for wlanFixReport's re-check (one nub, never released)
	gPortNub = nub;
	gPortSpace = g.sp.bits;
	gPortCap = gcap;
	// The port must not use 10-bit tags toward this function (see the header): cleared before the first read.
	uint32_t gdc2cap = g.r32(gcap + 0x24);
	uint16_t gdc2 = gKeepTag10 ? g.r16(gcap + 0x28) : tag10Off(g, gcap);
	LOG("wlanfix: port devcap2 %08x devctl2 %04x -> %04x (10-bit tag requester %s)", gdc2cap, gdc2, g.r16(gcap + 0x28),
	    gKeepTag10 ? "left as set (-rmbwlantag10)" : (gdc2 & Devctl2Tag10) ? "was ON, cleared" : "was off");
	Timed id = timedId(w);
	LOG("wlanfix: WLAN %06x id %08x (%u us); port %06x id %08x cap %02x lnkctl|sta %08x devctl|sta %08x l1ss %03x ctl1 "
	    "%08x bctl %04x",
	    space, id.v, id.us, g.sp.bits, gid, gcap, g.r32(gcap + 0x10), g.r32(gcap + 0x08), gl1ss,
	    gl1ss ? g.x32(gl1ss + 8) : 0, g.r16(0x3e));
	// A valid id is an answer even when slow (a reset is only for a function that does not answer at all).
	bool alive = id.v == WlanId;
	if (alive && id.us > SlowReadUs)
		LOG("wlanfix: WLAN answered but took %u us (> %u)", id.us, SlowReadUs);
	bool reset = false;
	if (!alive) {
		if (!keepAspm) {
			uint16_t lc = aspmOff(g, gcap);
			uint32_t l1 = l1ssOff(g, gl1ss);
			LOG("wlanfix: port ASPM lnkctl %04x -> %04x, L1SS ctl1 %08x -> %08x", lc, g.r16(gcap + 0x10), l1,
			    gl1ss ? g.x32(gl1ss + 8) : 0);
		}
		if (noSbr) {
			LOG("wlanfix: WLAN not answering, secondary bus reset disabled (-rmbwlannosbr)");
			snprintf(gSummary, sizeof(gSummary), "dead %08x/%uus, no SBR (-rmbwlannosbr)", id.v, id.us);
			return;
		}
		uint16_t bctl = g.r16(0x3e);
		g.w16(0x3e, bctl | 0x40);
		IOSleep(2);
		g.w16(0x3e, bctl & ~0x40);
		reset = true;
		unsigned ms = 0;
		uint16_t lsta = 0;
		for (; ms < 1000; ms += 10) {
			lsta = g.r16(gcap + 0x12);
			if (lsta & 0x2000)
				break;
			IOSleep(10);
		}
		IOSleep(100);
		LOG("wlanfix: secondary bus reset done, link %s after %u ms (lnksta %04x), +100 ms", lsta & 0x2000 ? "up" : "DOWN",
		    ms, lsta);
		for (int i = 0; i < 4; i++) {
			id = timedId(w);
			LOG("wlanfix: after reset read %d: id %08x (%u us)", i, id.v, id.us);
			if (id.v == WlanId)
				break;
			IOSleep(100);
		}
		alive = id.v == WlanId;
		if (!alive) {
			LOG("wlanfix: WLAN still not answering after the secondary bus reset: nothing else written");
			snprintf(gSummary, sizeof(gSummary), "dead, SBR did not recover (id %08x), port devctl2 %04x -> %04x", id.v,
			         gdc2, g.r16(gcap + 0x28));
			return;
		}
	}
	uint8_t wcap = pcieCap(w);
	uint16_t wl1ss = l1ssCap(w);
	if (reset) {
		restoreBars(w, from);
		if (wcap) {
			uint16_t dc = w.r16(wcap + 0x08), gdc = g.r16(gcap + 0x08);
			uint16_t want = static_cast<uint16_t>((dc & ~0xe0) | (gdc & 0xe0));
			if (want != dc)
				w.w16(wcap + 0x08, want);
			LOG("wlanfix: WLAN devctl %04x -> %04x (port MPS %u B), cmd %04x", dc, w.r16(wcap + 0x08), 128u << ((gdc >> 5) & 7),
			    w.r16(0x04));
		}
	}
	if (!keepAspm && wcap) {
		uint16_t wlc = aspmOff(w, wcap), glc = aspmOff(g, gcap);
		uint32_t wl1 = l1ssOff(w, wl1ss), gl1 = l1ssOff(g, gl1ss);
		LOG("wlanfix: ASPM off: WLAN lnkctl %04x -> %04x, port %04x -> %04x; L1SS ctl1 WLAN@%03x %08x -> %08x, port %08x -> "
		    "%08x",
		    wlc, w.r16(wcap + 0x10), glc, g.r16(gcap + 0x10), wl1ss, wl1, wl1ss ? w.x32(wl1ss + 8) : 0, gl1,
		    gl1ss ? g.x32(gl1ss + 8) : 0);
	}
	uint32_t wdc2cap = 0;
	if (wcap) {
		uint32_t devcap = w.r32(wcap + 0x04);
		wdc2cap = w.r32(wcap + 0x24);
		gMaskOff = wcap + 4u;
		gMaskNub = nub;
		LOG("wlanfix: WLAN pcie cap %02x devcap %08x (FLR %s): bit 28 hidden from IOPCIFamily / driver reads of +%02x; "
		    "devcap2 %08x (10-bit tag completer %s) devctl2 %04x",
		    wcap, devcap, devcap & DevcapFlr ? "capable" : "not advertised", wcap + 4, wdc2cap,
		    wdc2cap & Devcap2Tag10Comp ? "supported" : "NOT supported", w.r16(wcap + 0x28));
	}
	snprintf(gSummary, sizeof(gSummary), "%s, id %08x, cap %02x, ASPM %s, port 10-bit tags %s, WLAN devcap2 %08x",
	         reset ? "recovered by SBR" : "answered", id.v, wcap, keepAspm ? "kept" : "off",
	         gKeepTag10 ? "kept" : (gdc2 & Devctl2Tag10) ? "cleared" : "were off", wdc2cap);
}

bool wrapInitializeNub(void *bridge, IOPCIDevice *nub, OSDictionary *from) {
	// The jump is live a few instructions before routeFunctionLong returns the trampoline; the original must
	// run (it sets nub->space and nub->parent), so wait for it instead of skipping it.
	mach_vm_address_t org;
	while (!(org = gOrgInitNub))
		__builtin_ia32_pause();
	bool r = reinterpret_cast<InitNubFn>(org)(bridge, nub, from);
	if (gState == 0 && nub && from && dataU32(from, "vendor-id") == 0x14c3 && dataU32(from, "device-id") == 0x0616) {
		gState = 1;
		uint32_t reg = 0xffffffffU;
		auto o = OSDynamicCast(OSData, from->getObject("reg"));
		if (o && o->getLength() >= 4)
			reg = *static_cast<const uint32_t *>(o->getBytesNoCopy()) & 0x00ffff00U;
		uint64_t t0 = mach_absolute_time(), ns = 0;
		LOG("wlanfix: initializeNub WLAN 14c3:0616 space %06x", reg);
		if (reg != 0xffffffffU)
			fix(nub, from, reg);
		else
			snprintf(gSummary, sizeof(gSummary), "no reg property, nothing done");
		absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
		LOG("wlanfix: done in %llu ms: %s", ns / 1000000, gSummary);
		gState = 2;
	}
	return r;
}

}  // namespace

uint32_t wlanFixState() { return gState; }

// Called by wlandiag (publish, +2 s, +10 s, +60 s): state line, then the port's 10-bit tag enable checked again
// (a restore of saved bridge state would bring bit 12 back) and cleared if it is set.
void wlanFixReport(const char *tag) {
	LOG("wlanfix: %s state %u, %s, devcap FLR reads masked %u", tag, gState, gState == 2 ? gSummary : "not run",
	    gMasked);
	if (gState != 2 || !gPortNub || !gPortCap)
		return;
	Cfg g {gPortNub, {}};
	g.sp.bits = gPortSpace;
	uint32_t before = gTag10Cleared;
	uint16_t v = gKeepTag10 ? g.r16(gPortCap + 0x28) : tag10Off(g, gPortCap);
	if (gTag10Cleared != before)
		LOG("wlanfix: %s port devctl2 %04x: 10-bit tag requester was set AGAIN, cleared -> %04x (%u clears)", tag, v,
		    g.r16(gPortCap + 0x28), gTag10Cleared);
	else
		LOG("wlanfix: %s port devctl2 %04x (10-bit tag requester %s)", tag, v, v & Devctl2Tag10 ? "on" : "off");
}

uint32_t wlanDevcapFilter32(const void *dev, uint64_t off, uint32_t v) {
	if (dev && dev == gMaskNub && off == gMaskOff && v != 0xffffffffU && (v & DevcapFlr)) {
		gMasked++;
		return v & ~DevcapFlr;
	}
	return v;
}

uint16_t wlanDevcapFilter16(const void *dev, uint64_t off, uint16_t v) {
	if (dev && dev == gMaskNub && off == gMaskOff + 2 && v != 0xffff && (v & (DevcapFlr >> 16))) {
		gMasked++;
		return static_cast<uint16_t>(v & ~(DevcapFlr >> 16));
	}
	return v;
}

void wlanFixRoute(KernelPatcher &patcher) {
	static bool routed;
	if (routed)
		return;
	routed = true;
	if (checkKernelArgument("-rmbnowlanfix")) {
		LOG("wlanfix: disabled (-rmbnowlanfix)");
		return;
	}
	if (!imageVerified())
		return;
	patcher.clearError();
	auto org = patcher.routeFunctionLong(gInitNubAddr, reinterpret_cast<mach_vm_address_t>(wrapInitializeNub), true, true);
	if (!org) {
		LOG("wlanfix: initializeNub route FAILED (err %d)", patcher.getError());
		patcher.clearError();
		return;
	}
	gOrgInitNub = org;
	LOG("wlanfix: IOPCIBridge::initializeNub routed (MT7922 recovery before probeBusGated reads it)");
}
