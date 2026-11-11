//
//  USB4 host router sleep (bare metal). macOS has no driver for the Rembrandt USB4 NHI (74:00.6,
//  1022:162f). Linux sets ROUTER_CS_5 SLP on the host router before S3 and waits for ROUTER_CS_6
//  SLPR (usb4_switch_set_sleep); without that the firmware's S3 resume ends in a CF9 reset (10-27:
//  Linux with thunderbolt unbound rebooted exactly like macOS sleep). Do the same at WillSleep
//  through the NHI control channel: ring 0, raw frames with a CRC32C, polled, no interrupts, as
//  drivers/thunderbolt/{nhi,ctl}.c (v6.12) do it. A boot probe 20 s after publish reads ROUTER_CS
//  only, to check the channel. -rmbnousb4sleep: don't touch the NHI at all.
//

#include "rmb.hpp"
#include <IOKit/IOService.h>
#include <IOKit/IOMessage.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <libkern/OSByteOrder.h>

namespace {

// BAR0 registers (nhi_regs.h): per-hop ring descriptors (16 B) and options (32 B), hop 0 = control.
constexpr uint32_t RegTxRing = 0x00000, RegRxRing = 0x08000, RegTxOpt = 0x19800, RegRxOpt = 0x29800;
constexpr uint32_t RegCaps = 0x39640;
constexpr uint32_t RingEnable = 1u << 31, RingRaw = 1u << 30;
// ring_desc: u64 phys; u32 length:12, eof:4, sof:4, flags:12; u32 time.
constexpr uint32_t DescCompleted = 0x2, DescPosted = 0x4, DescInterrupt = 0x8;
constexpr uint32_t PkgRead = 1, PkgWrite = 2, PkgError = 3;
constexpr uint32_t SpaceSwitch = 2;
constexpr uint32_t RouterCs5Slp = 1u << 0, RouterCs6Slpr = 1u << 0;
constexpr unsigned RingSize = 10, FrameSize = 0x100;
// One DMA page: TX descriptors, RX descriptors, TX frame, RX frames (one per RX slot).
constexpr uint32_t OffTxDesc = 0x000, OffRxDesc = 0x100, OffTxBuf = 0x200, OffRxBuf = 0x300;

struct Desc {
	uint64_t phys;
	uint32_t ctl;
	uint32_t time;
};

IOService *gNhiSvc;
IOPCIDevice *gNhiPci;
IOPCIAddressSpace gNhiSp;
IOMemoryMap *gNhiMap;
volatile uint8_t *gNhi;
IOBufferMemoryDescriptor *gDma;
uint8_t *gDmaVa;
uint64_t gDmaPa;
unsigned gTxHead, gRxHead, gRxTail, gSeq;
IOLock *gUsb4Lock;
IONotifier *gUsb4PciNotifier, *gUsb4PowerNotifier;

uint32_t dataU32(IORegistryEntry *e, const char *key) {
	auto d = OSDynamicCast(OSData, e->getProperty(key));
	return d && d->getLength() >= 4 ? *static_cast<const uint32_t *>(d->getBytesNoCopy()) : 0;
}

uint32_t r32(uint32_t off) { return *reinterpret_cast<volatile uint32_t *>(gNhi + off); }
void w32(uint32_t off, uint32_t v) { *reinterpret_cast<volatile uint32_t *>(gNhi + off) = v; }
Desc *txDesc(unsigned i) { return reinterpret_cast<Desc *>(gDmaVa + OffTxDesc) + i; }
Desc *rxDesc(unsigned i) { return reinterpret_cast<Desc *>(gDmaVa + OffRxDesc) + i; }

uint32_t crc32c(const uint8_t *p, size_t n) {
	uint32_t c = ~0u;
	while (n--) {
		c ^= *p++;
		for (int k = 0; k < 8; k++)
			c = (c >> 1) ^ (0x82F63B78u & (0u - (c & 1)));
	}
	return ~c;
}

// Map BAR0, memory decode + bus master on, D0, one DMA page.
bool prepare() {
	if (!gNhiSvc)
		return false;
	if (!gNhiMap) {
		auto mem = gNhiSvc->getDeviceMemoryWithIndex(0);
		if (!mem || mem->getLength() < 0x40000) {
			LOG("usb4: NHI BAR0 %s", mem ? "too small" : "missing");
			return false;
		}
		gNhiMap = mem->map(kIOMapInhibitCache);
		if (!gNhiMap)
			return false;
		gNhi = reinterpret_cast<volatile uint8_t *>(gNhiMap->getVirtualAddress());
	}
	uint16_t cmd = gNhiPci->configRead16(gNhiSp, static_cast<UInt8>(kIOPCIConfigCommand));
	if ((cmd & 0x6) != 0x6)
		gNhiPci->configWrite16(gNhiSp, static_cast<UInt8>(kIOPCIConfigCommand), cmd | 0x6);
	uint16_t pmcsr = 0xFFFF;
	for (uint8_t p = gNhiPci->configRead8(gNhiSp, 0x34) & 0xFC, n = 0; p && n < 48; n++) {
		if (gNhiPci->configRead8(gNhiSp, p) == 0x01) {
			pmcsr = gNhiPci->configRead16(gNhiSp, p + 4);
			if (pmcsr & 3) {
				gNhiPci->configWrite16(gNhiSp, p + 4, pmcsr & ~3);
				IOSleep(10);
			}
			break;
		}
		p = gNhiPci->configRead8(gNhiSp, p + 1) & 0xFC;
	}
	if (!gDma) {
		gDma = IOBufferMemoryDescriptor::withOptions(kIODirectionInOut | kIOMemoryPhysicallyContiguous, PAGE_SIZE,
		                                             PAGE_SIZE);
		if (!gDma || gDma->prepare() != kIOReturnSuccess) {
			LOG("usb4: no DMA page");
			return false;
		}
		gDmaVa = static_cast<uint8_t *>(gDma->getBytesNoCopy());
		gDmaPa = gDma->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
	}
	uint32_t caps = r32(RegCaps);
	LOG("usb4: NHI cmd %04x pmcsr %04x caps %08x dma %llx", cmd, pmcsr, caps,
	    static_cast<unsigned long long>(gDmaPa));
	return caps != 0xFFFFFFFF && gDmaPa;
}

// tb_ring_stop for TX and RX hop 0.
void ringsStop() {
	w32(RegTxOpt, 0);
	w32(RegRxOpt, 0);
	for (uint32_t o = 0; o < 16; o += 4) {
		w32(RegTxRing + o, 0);
		w32(RegRxRing + o, 0);
	}
}

void rxPost(unsigned slot) {
	auto d = rxDesc(slot);
	d->phys = gDmaPa + OffRxBuf + slot * FrameSize;
	d->time = 0;
	d->ctl = (DescPosted | DescInterrupt) << 20;
}

// tb_ring_start (no RING_FLAG_FRAME: ENABLE | RAW, frame size 0x100), then post RingSize-1 RX frames.
void ringsStart() {
	bzero(gDmaVa, OffRxBuf + RingSize * FrameSize);
	gTxHead = gRxTail = 0;
	gRxHead = RingSize - 1;
	for (unsigned i = 0; i < gRxHead; i++)
		rxPost(i);
	uint64_t tx = gDmaPa + OffTxDesc, rx = gDmaPa + OffRxDesc;
	w32(RegTxRing + 0, static_cast<uint32_t>(tx));
	w32(RegTxRing + 4, static_cast<uint32_t>(tx >> 32));
	w32(RegTxRing + 12, RingSize);
	w32(RegTxOpt + 4, 0);
	w32(RegTxOpt, RingEnable | RingRaw);
	w32(RegRxRing + 0, static_cast<uint32_t>(rx));
	w32(RegRxRing + 4, static_cast<uint32_t>(rx >> 32));
	w32(RegRxRing + 12, (FrameSize << 16) | RingSize);
	w32(RegRxOpt + 4, 0xFFFFFFFF);
	w32(RegRxOpt, RingEnable | RingRaw);
	__sync_synchronize();
	w32(RegRxRing + 8, gRxHead);
}

// Send one control frame (host-order dwords; CRC added) and wait ≤100 ms for a reply of type
// `want` or an error packet. Events and other frames are dropped. Returns the reply type, 0 on timeout.
uint32_t xfer(uint32_t type, const uint32_t *w, unsigned nw, uint32_t want, uint32_t *out, unsigned outMax,
              unsigned &outN) {
	auto buf = reinterpret_cast<uint32_t *>(gDmaVa + OffTxBuf);
	for (unsigned i = 0; i < nw; i++)
		buf[i] = OSSwapHostToBigInt32(w[i]);
	buf[nw] = OSSwapHostToBigInt32(crc32c(reinterpret_cast<uint8_t *>(buf), nw * 4));
	auto d = txDesc(gTxHead);
	d->phys = gDmaPa + OffTxBuf;
	d->time = 0;
	d->ctl = (nw * 4 + 4) | (type << 12) | (type << 16) | ((DescPosted | DescInterrupt) << 20);
	__sync_synchronize();
	gTxHead = (gTxHead + 1) % RingSize;
	w32(RegTxRing + 8, gTxHead << 16);
	for (unsigned t = 0; t < 1000; t++) {
		auto r = rxDesc(gRxTail);
		uint32_t ctl = __atomic_load_n(&r->ctl, __ATOMIC_ACQUIRE);
		if (!(ctl & (DescCompleted << 20))) {
			IODelay(100);
			continue;
		}
		unsigned len = ctl & 0xFFF ? ctl & 0xFFF : 4096, eof = (ctl >> 12) & 0xF;
		auto in = reinterpret_cast<uint32_t *>(gDmaVa + OffRxBuf + gRxTail * FrameSize);
		bool ok = len >= 8 && len <= FrameSize && !(len & 3);
		uint32_t crc = ok ? OSSwapBigToHostInt32(in[len / 4 - 1]) : 0;
		ok = ok && crc == crc32c(reinterpret_cast<uint8_t *>(in), len - 4);
		outN = 0;
		if (ok)
			for (; outN < len / 4 - 1 && outN < outMax; outN++)
				out[outN] = OSSwapBigToHostInt32(in[outN]);
		rxPost(gRxHead);
		gRxHead = (gRxHead + 1) % RingSize;
		gRxTail = (gRxTail + 1) % RingSize;
		__sync_synchronize();
		w32(RegRxRing + 8, gRxHead);
		if (!ok) {
			LOG("usb4: rx frame len %u eof %u bad (crc %08x)", len, eof, crc);
			continue;
		}
		if (eof == want || eof == PkgError)
			return eof;
		LOG("usb4: rx dropped eof %u len %u w0 %08x w2 %08x", eof, len, out[0], outN > 2 ? out[2] : 0);
	}
	LOG("usb4: no reply (tx desc %08x, tx ring %08x, rx ring %08x, rx desc %08x)", txDesc((gTxHead + RingSize - 1) % RingSize)->ctl,
	    r32(RegTxRing + 8), r32(RegRxRing + 8), rxDesc(gRxTail)->ctl);
	return 0;
}

// Router (route 0) config space, TB_CFG_SWITCH, port 0. tb_cfg_address: offset:13 length:6
// port:6 space:2 seq:2.
uint32_t cfgAddr(unsigned off, unsigned n) {
	gSeq = (gSeq + 1) & 3;
	return off | (n << 13) | (SpaceSwitch << 25) | (gSeq << 27);
}

// Reply address: offset, length, space must echo the request; port is the sender's upstream
// port (Linux check_config_address skips it).
constexpr uint32_t AddrMatch = 0x0607FFFF;

bool cfgRead(unsigned off, unsigned n, uint32_t *val) {
	uint32_t req[3] {0, 0, cfgAddr(off, n)}, rsp[3 + 16];
	unsigned got = 0;
	uint32_t t = xfer(PkgRead, req, 3, PkgRead, rsp, arrsize(rsp), got);
	if (t != PkgRead || got < 3 + n || (rsp[2] & AddrMatch) != (req[2] & AddrMatch)) {
		LOG("usb4: read cs %u+%u failed (type %u, %u dw, addr %08x, w2 %08x)", off, n, t, got, req[2], got > 2 ? rsp[2] : 0);
		return false;
	}
	for (unsigned i = 0; i < n; i++)
		val[i] = rsp[3 + i];
	return true;
}

bool cfgWrite(unsigned off, uint32_t v) {
	uint32_t req[4] {0, 0, cfgAddr(off, 1), v}, rsp[4];
	unsigned got = 0;
	uint32_t t = xfer(PkgWrite, req, 4, PkgWrite, rsp, arrsize(rsp), got);
	if (t != PkgWrite) {
		LOG("usb4: write cs %u = %08x failed (type %u, w2 %08x)", off, v, t, got > 2 ? rsp[2] : 0);
		return false;
	}
	return true;
}

// Read ROUTER_CS_0-7; with `sleep` set SLP and wait ≤500 ms for SLPR (usb4_switch_set_sleep).
void usb4Run(const char *why, bool sleep) {
	IOLockLock(gUsb4Lock);
	if (prepare()) {
		ringsStop();
		ringsStart();
		uint32_t cs[8];
		if (cfgRead(0, 8, cs)) {
			LOG("usb4: %s: ROUTER_CS_0-7 %08x %08x %08x %08x %08x %08x %08x %08x", why, cs[0], cs[1], cs[2], cs[3],
			    cs[4], cs[5], cs[6], cs[7]);
			if (sleep && cfgWrite(5, cs[5] | RouterCs5Slp)) {
				uint32_t cs6 = 0;
				unsigned ms = 0;
				for (; ms <= 500; ms += 10) {
					if (!cfgRead(6, 1, &cs6) || (cs6 & RouterCs6Slpr))
						break;
					IOSleep(10);
				}
				LOG("usb4: %s: SLP set, ROUTER_CS_6 %08x -> %s after %u ms", why, cs6,
				    cs6 & RouterCs6Slpr ? "SLPR (sleep ready)" : "NO SLPR", ms);
			}
		}
		ringsStop();
	}
	IOLockUnlock(gUsb4Lock);
}

IOReturn usb4Power(void *, void *, UInt32 type, IOService *, void *arg, vm_size_t) {
	if (sleepEntering(type, arg)) {
		usb4Run("sleep", true);
		diskLogSync(1500);
	}
	return kIOReturnSuccess;
}

void probeThread(void *, wait_result_t) {
	IOSleep(20000);
	usb4Run("boot probe", false);
	thread_terminate(current_thread());
}

bool nhiPublished(void *, void *, IOService *svc, IONotifier *) {
	if (gNhiSvc || dataU32(svc, "vendor-id") != 0x1022 || dataU32(svc, "class-code") != 0x0C0340)
		return true;
	gNhiSvc = svc;
	gNhiPci = static_cast<IOPCIDevice *>(svc);  // virtual calls only, as mmio.cpp
	gNhiSp.bits = dataU32(svc, "reg") & 0x00FFFF00;
	gUsb4PowerNotifier = registerPrioritySleepWakeInterest(usb4Power, nullptr, nullptr);
	LOG("usb4: NHI %04x at %02x:%02x.%x, sleep notifier %s", dataU32(svc, "device-id"), gNhiSp.s.busNum,
	    gNhiSp.s.deviceNum, gNhiSp.s.functionNum, gUsb4PowerNotifier ? "ok" : "MISSING");
	thread_t t;
	if (kernel_thread_start(probeThread, nullptr, &t) == KERN_SUCCESS)
		thread_deallocate(t);
	return true;
}

}  // namespace

void usb4SleepStart() {
	if (checkKernelArgument("-rmbnousb4sleep"))
		return;
	gUsb4Lock = IOLockAlloc();
	auto matching = IOService::serviceMatching("IOPCIDevice");
	if (!gUsb4Lock || !matching)
		return;
	gUsb4PciNotifier = IOService::addMatchingNotification(gIOFirstPublishNotification, matching, nhiPublished, nullptr);
	matching->release();
	LOG("usb4: NHI publish notifier %s", gUsb4PciNotifier ? "armed" : "MISSING");
}
