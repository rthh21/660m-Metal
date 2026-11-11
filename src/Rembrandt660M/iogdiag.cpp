//
//  iogdiag.cpp: why the hardware cursor moves only every ~2 s (counters only, no behaviour change).
//
//  Boot 20261108_203956 (curdiag, display.cpp): while the mouse moved, AMD setCursorState ran once per
//  2 s window with "max gap" 2034-2048 ms (HUBP0 CURSOR_POSITION followed it, so the hardware applied every
//  call). 25G229 IOGraphicsFamily (SystemKernelExtensions.kc entry 0x14ce4000, LC_UUID 74AA6483-...):
//  IOFramebuffer::extSetCursorPosition 0x14cf8bb8 first waits for the controller work loop gate
//  (IOGraphicsWorkLoop::timedCloseGate 0x14cf8d04 on *(*(fb+0x1c8))+0x120, closeGate without timeout; Apple
//  GTrace-records long waits, token 0x15ee), opens it again (vtable +0x178) and only then calls
//  moveCursorImpl 0x14cf8daa, which with shmem+0xc9 == 1 calls setCursorState synchronously
//  (deferredMoveCursor 0x14cf7cd9, vtable +0xa78). So the 2 s cadence is one of: WindowServer calls
//  extSetCursorPosition rarely, each call waits ~2 s for the gate, IOFB drops moves (early exits of
//  moveCursorImpl), or the USB mouse reports arrive rarely. This file measures each, nothing else:
//
//  iogdiag  - extSetCursorPosition hook (IOGraphicsFamily, routed at framebuffer load, before WindowServer):
//             calls, failures, duration, gate wait, and the moveCursorImpl preconditions seen at entry.
//  gatediag - the controller work loop is an IOGraphicsControllerWorkLoop (IOFBController::init 0x14ced8fd
//             stores vtable 0x14d18d28, +0x60/+0x68 = NULL, +0x70 = controller); its closeGate/sleepGate
//             overrides only call IOGraphicsWorkLoop's (0x14d0710c, 0x14d07118), whose gate callback wl+0x60 runs
//             with state 1 on the outermost acquire / after sleepGate wakes (0x14ced219, 0x14ced3fb) and state 0
//             on the outermost release / before sleepGate sleeps (0x14ced284, 0x14ced3b1). It is set by one
//             aligned pointer store, no code patch. Hold times per acquire; holds that block a waiting cursor
//             call (or >= 250 ms) get a backtrace, printed as module+offset.
//  hiddiag  - IOUSBHostHIDDevice::interruptReadComplete hook (Boot KC, LC_UUID EC3E3338-...): report rate,
//             errors, burst/late arrivals per HID interface.
//
//  Each image: exact in-memory LC_UUID + __TEXT bounds + prologue bytes, else that part stays off.
//  -rmbnoiogdiag: all off. -rmbnogatediag / -rmbnohiddiag: that part off.
//

#include "rmb.hpp"
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <kern/thread.h>
#include <libkern/OSDebug.h>
#include <mach-o/loader.h>
#include <mach/kmod.h>
#include <string.h>

extern "C" void vm_kernel_unslide_or_perm_external(vm_offset_t addr, vm_offset_t *up_addr);

namespace {

uint64_t diagUs() {
	uint64_t ns;
	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / 1000;
}

// ---- image verification -------------------------------------------------------------------------------

struct Image {
	const char *bundle;
	uint8_t uuid[16];
};

// 25G229 SystemKernelExtensions.kc: com.apple.iokit.IOGraphicsFamily, __TEXT 0x14ce4000 size 0x2f000.
constexpr Image IogImage {"com.apple.iokit.IOGraphicsFamily",
	{0x74, 0xaa, 0x64, 0x83, 0x8a, 0xb4, 0x3a, 0x94, 0x81, 0x43, 0x5e, 0x30, 0x6c, 0x16, 0xd7, 0xda}};
// 25G229 BootKernelExtensions.kc (UUID 58904DC1-..., paired with the System KC): IOUSBHostHIDDevice,
// __TEXT 0xffffff8002fb0000 size 0x7000.
constexpr Image HidImage {"com.apple.driver.usb.IOUSBHostHIDDevice",
	{0xec, 0x3e, 0x33, 0x38, 0x6b, 0x2d, 0x3d, 0xf1, 0xa7, 0x53, 0xb3, 0x5a, 0xd4, 0xaf, 0x9a, 0x25}};

// RVAs from the start of the image's __TEXT (= its Mach-O header).
constexpr uint32_t RvaExtSetCursorPosition = 0x14bb8;  // __ZN13IOFramebuffer20extSetCursorPositionEP8OSObjectPvP25IOExternalMethodArguments
constexpr uint32_t RvaCloseGateHook = 0x9219;          // closeGate 0x91da +0x3f: hook(wl, +0x68, +0x70, 1)
constexpr uint32_t RvaOpenGateHook = 0x9284;           // openGate 0x9264 +0x20: hook(wl, +0x68, +0x70, 0)
constexpr uint32_t RvaGraphicsWorkLoopVtbl = 0x34718;  // __ZTV18IOGraphicsWorkLoop 0x14d18708 + 0x10
constexpr uint32_t RvaControllerWorkLoopVtbl = 0x34d28;  // __ZTV28IOGraphicsControllerWorkLoop 0x14d18d18 + 0x10
constexpr uint32_t RvaInterruptReadComplete = 0x4940;  // __ZN18IOUSBHostHIDDevice21interruptReadCompleteEPvij

const uint8_t ExtSetCursorPositionPro[20] {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
	0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x48, 0x48, 0x89, 0xfb};
// mov 0x60(%rbx),%rax; test; je; mov 0x68(%rbx),%rsi; mov 0x70(%rbx),%rdx; mov %rbx,%rdi; mov $1,%ecx
const uint8_t CloseGateHookBytes[25] {0x48, 0x8b, 0x43, 0x60, 0x48, 0x85, 0xc0, 0x74, 0x28, 0x48, 0x8b, 0x73,
	0x68, 0x48, 0x8b, 0x53, 0x70, 0x48, 0x89, 0xdf, 0xb9, 0x01, 0x00, 0x00, 0x00};
// mov 0x60(%rbx),%rax; test; je; mov 0x68(%rbx),%rsi; mov 0x70(%rbx),%rdx; mov %rbx,%rdi; xor %ecx,%ecx; call *%rax
const uint8_t OpenGateHookBytes[24] {0x48, 0x8b, 0x43, 0x60, 0x48, 0x85, 0xc0, 0x74, 0x15, 0x48, 0x8b, 0x73,
	0x68, 0x48, 0x8b, 0x53, 0x70, 0x48, 0x89, 0xdf, 0x31, 0xc9, 0xff, 0xd0};
const uint8_t InterruptReadCompletePro[20] {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54,
	0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x89, 0xfb, 0xc6, 0x87};

// __TEXT base of the named kmod when its in-memory LC_UUID is the expected one; textSize = __TEXT vmsize.
mach_vm_address_t verifiedText(const Image &img, uint64_t &textSize) {
	textSize = 0;
	for (auto k = kmodList(); k; k = k->next) {
		if (strncmp(k->name, img.bundle, KMOD_MAX_NAME))
			continue;
		auto base = static_cast<mach_vm_address_t>(k->address);
		auto hdr = reinterpret_cast<const mach_header_64 *>(base);
		if (!base || hdr->magic != MH_MAGIC_64 || hdr->ncmds > 64 || hdr->sizeofcmds > 0x4000) {
			LOG("iogdiag: %s: no 64-bit Mach-O header at the kmod address, not used", img.bundle);
			return 0;
		}
		bool uuidOk = false;
		auto p = reinterpret_cast<const uint8_t *>(base + sizeof(mach_header_64));
		auto end = p + hdr->sizeofcmds;
		for (uint32_t i = 0; i < hdr->ncmds && p + sizeof(load_command) <= end; i++) {
			auto lc = reinterpret_cast<const load_command *>(p);
			if (lc->cmdsize < sizeof(load_command) || p + lc->cmdsize > end)
				break;
			if (lc->cmd == LC_UUID && lc->cmdsize >= sizeof(uuid_command))
				uuidOk = !memcmp(reinterpret_cast<const uuid_command *>(p)->uuid, img.uuid, sizeof(img.uuid));
			else if (lc->cmd == LC_SEGMENT_64 && lc->cmdsize >= sizeof(segment_command_64) &&
			         !strncmp(reinterpret_cast<const segment_command_64 *>(p)->segname, SEG_TEXT, 16))
				textSize = reinterpret_cast<const segment_command_64 *>(p)->vmsize;
			p += lc->cmdsize;
		}
		if (!uuidOk || !textSize) {
			LOG("iogdiag: %s: LC_UUID %s, __TEXT size %llx: not the 25G229 image, not used", img.bundle,
			    uuidOk ? "match" : "MISMATCH", textSize);
			textSize = 0;
			return 0;
		}
		return base;
	}
	LOG("iogdiag: %s not in the kmod list, not used", img.bundle);
	return 0;
}

bool bytesAt(mach_vm_address_t base, uint64_t textSize, uint32_t rva, const uint8_t *want, size_t n) {
	return base && rva + n <= textSize && !memcmp(reinterpret_cast<const void *>(base + rva), want, n);
}

// ---- iogdiag: extSetCursorPosition --------------------------------------------------------------------

struct CurStat {
	uint32_t calls, fails, lastRet, durMaxUs, n20ms, n200ms, waitMaxUs, gapMaxMs;
	uint64_t durSumUs;
	int32_t x, y;
	// moveCursorImpl 0x14cf8008 preconditions read at entry (racy, diagnostic only)
	uint32_t noShmem, badFrame, noMoveFlag, semaBusy, sw, hw, deferred, hidden;
};
CurStat gC;
uint64_t gCLastUs;
mach_vm_address_t orgExtSetCursorPosition, gIogBase;
volatile thread_t gCurThread;
volatile uint64_t gCurT0;  // nonzero while a cursor call waits for / runs after the gate

void gateWire(uint64_t fb);

void cursorPre(uint64_t fb, uint64_t args) {
	auto rd8 = [](uint64_t a) { return *reinterpret_cast<const volatile uint8_t *>(a); };
	auto rd32 = [](uint64_t a) { return *reinterpret_cast<const volatile uint32_t *>(a); };
	auto rd64 = [](uint64_t a) { return *reinterpret_cast<const volatile uint64_t *>(a); };
	uint64_t shmem = rd64(fb + 0x90), priv = rd64(fb + 0x1c8);
	if (args && rd32(args + 0x28) >= 3) {  // IOExternalMethodArguments: scalarInput +0x20, count +0x28
		auto in = reinterpret_cast<const volatile uint64_t *>(rd64(args + 0x20));
		if (in) {
			gC.x = static_cast<int32_t>(in[0]);
			gC.y = static_cast<int32_t>(in[1]);
			if (priv && in[2] >= rd32(priv + 0x30))  // frame >= priv numCursorFrames
				gC.badFrame++;
		}
	}
	if (!(rd8(fb + 0x170) & 0x10))
		gC.noMoveFlag++;
	if (!shmem) {
		gC.noShmem++;
		return;
	}
	if (rd32(shmem))  // cursorSema (OSSpinLockTry would fail)
		gC.semaBusy++;
	uint8_t act = rd8(shmem + 0xc9);  // hardwareCursorActive: 0 software, 1 synchronous setCursorState, else deferred
	if (!act)
		gC.sw++;
	else if (act == 1)
		gC.hw++;
	else
		gC.deferred++;
	if (rd8(shmem + 8))  // cursorShow != 0: hidden, no setCursorState
		gC.hidden++;
}

uint64_t wrapExtSetCursorPosition(uint64_t fb, uint64_t ref, uint64_t args, uint64_t d, uint64_t e, uint64_t f) {
	if (fb) {
		gateWire(fb);
		cursorPre(fb, args);
	}
	uint64_t t0 = diagUs();
	gCurThread = current_thread();
	gCurT0 = t0;
	auto r = reinterpret_cast<Fn6>(orgExtSetCursorPosition)(fb, ref, args, d, e, f);
	uint64_t t1 = diagUs(), dur = t1 - t0;
	gCurT0 = 0;
	gC.calls++;
	gC.lastRet = static_cast<uint32_t>(r);
	if (static_cast<uint32_t>(r))
		gC.fails++;
	gC.durSumUs += dur;
	if (dur > gC.durMaxUs)
		gC.durMaxUs = static_cast<uint32_t>(dur);
	if (dur >= 20000)
		gC.n20ms++;
	if (dur >= 200000)
		gC.n200ms++;
	if (gCLastUs && (t0 - gCLastUs) / 1000 > gC.gapMaxMs)
		gC.gapMaxMs = static_cast<uint32_t>((t0 - gCLastUs) / 1000);
	gCLastUs = t0;
	return r;
}

// ---- gatediag: IOGraphicsWorkLoop gate callback chain ------------------------------------------------

using GateFn = void (*)(void *wl, void *obj, void *ref, bool closing);
constexpr size_t GateSlots = 4;
struct GateSlot {
	void *wl;
	GateFn org;
	volatile thread_t holder;
	uint64_t since;
	uint32_t holds, holdMaxUs, n20ms, n100ms, n1s, blockedCursor, blockedMaxUs;
};
GateSlot gGate[GateSlots];
uint8_t gGateWiring;
bool gGateOn;
uint32_t gGateWired, gGateRejected;
uint64_t gGateOrgCb[GateSlots];

constexpr size_t BtMax = 24, BtFrames = 12;
struct Bt {
	volatile uint32_t ready;
	uint32_t holdMs, slot, cursor, n;
	uint64_t tid, atMs;
	void *f[BtFrames];
};
Bt gBt[BtMax];
uint32_t gBtN;

void gateHook(void *wl, void *obj, void *ref, bool closing) {
	GateSlot *s = nullptr;
	for (auto &g : gGate)
		if (g.wl == wl) {
			s = &g;
			break;
		}
	if (!s)  // cannot happen: a slot is filled before wl+0x60 points here
		return;
	thread_t me = current_thread();
	uint64_t now = diagUs();
	if (closing) {
		s->holder = me;
		s->since = now;
		if (me == gCurThread && gCurT0 && now - gCurT0 > gC.waitMaxUs)
			gC.waitMaxUs = static_cast<uint32_t>(now - gCurT0);
	} else if (s->holder == me && s->since) {
		uint64_t hold = now - s->since;
		s->holder = nullptr;
		s->since = 0;
		s->holds++;
		if (hold > s->holdMaxUs)
			s->holdMaxUs = static_cast<uint32_t>(hold);
		if (hold >= 20000)
			s->n20ms++;
		if (hold >= 100000)
			s->n100ms++;
		if (hold >= 1000000)
			s->n1s++;
		uint64_t c0 = gCurT0;
		bool cursorWaits = c0 && gCurThread != me && c0 < now && hold >= 20000;
		if (cursorWaits) {
			s->blockedCursor++;
			if (now - c0 > s->blockedMaxUs)
				s->blockedMaxUs = static_cast<uint32_t>(now - c0);
		}
		if ((cursorWaits || hold >= 250000) && __atomic_load_n(&gBtN, __ATOMIC_RELAXED) < BtMax) {
			uint32_t i = __atomic_fetch_add(&gBtN, 1, __ATOMIC_RELAXED);
			if (i < BtMax) {
				Bt &b = gBt[i];
				b.holdMs = static_cast<uint32_t>(hold / 1000);
				b.slot = static_cast<uint32_t>(s - gGate);
				b.cursor = cursorWaits;
				b.tid = thread_tid(me);
				b.atMs = now / 1000;
				b.n = OSBacktrace(b.f, BtFrames);
				__atomic_store_n(&b.ready, 1, __ATOMIC_RELEASE);
			}
		}
	}
	if (s->org)
		s->org(wl, obj, ref, closing);
}

void gateWire(uint64_t fb) {
	if (!gGateOn || gGateWired + gGateRejected >= GateSlots)
		return;
	auto priv = *reinterpret_cast<const uint64_t *>(fb + 0x1c8);
	auto ctrl = priv ? *reinterpret_cast<const uint64_t *>(priv) : 0;
	auto wl = ctrl ? *reinterpret_cast<void *const *>(ctrl + 0x120) : nullptr;
	if (!wl)
		return;
	for (auto &g : gGate)
		if (g.wl == wl)
			return;
	if (__atomic_exchange_n(&gGateWiring, 1, __ATOMIC_ACQUIRE))
		return;  // another thread is wiring: next call
	for (auto &g : gGate)
		if (g.wl == wl) {
			__atomic_store_n(&gGateWiring, 0, __ATOMIC_RELEASE);
			return;
		}
	// Only an IOGraphicsControllerWorkLoop / IOGraphicsWorkLoop of this image (vtable pointer): both use
	// IOGraphicsWorkLoop::openGate (vtable +0x178) and closeGate/sleepGate that reach IOGraphicsWorkLoop's.
	auto vptr = *reinterpret_cast<const uint64_t *>(wl);
	if (vptr != gIogBase + RvaControllerWorkLoopVtbl && vptr != gIogBase + RvaGraphicsWorkLoopVtbl) {
		gGateRejected++;
		__atomic_store_n(&gGateWiring, 0, __ATOMIC_RELEASE);
		return;
	}
	GateSlot *s = nullptr;
	for (auto &g : gGate)
		if (!g.wl) {
			s = &g;
			break;
		}
	if (s) {
		auto cb = reinterpret_cast<GateFn *>(static_cast<uint8_t *>(wl) + 0x60);
		s->org = __atomic_load_n(cb, __ATOMIC_ACQUIRE);
		gGateOrgCb[s - gGate] = reinterpret_cast<uint64_t>(s->org);
		__atomic_store_n(&s->wl, wl, __ATOMIC_RELEASE);
		// One aligned 8-byte store: a gate owner reads either the original callback or gateHook, both with the
		// unchanged +0x68/+0x70 arguments; a release seen without its acquire is ignored above.
		__atomic_store_n(cb, &gateHook, __ATOMIC_RELEASE);
		gGateWired++;
	}
	__atomic_store_n(&gGateWiring, 0, __ATOMIC_RELEASE);
}

// ---- hiddiag: IOUSBHostHIDDevice::interruptReadComplete ----------------------------------------------

constexpr size_t HidSlots = 8;
struct HidSlot {
	void *dev;
	uint32_t n, err, lastErr, bytes, burst, late, gapMaxMs;
	uint64_t lastUs;
};
HidSlot gHid[HidSlots];
mach_vm_address_t orgHidReadComplete;

uint64_t wrapHidReadComplete(uint64_t self, uint64_t param, uint64_t status, uint64_t bytes, uint64_t e, uint64_t f) {
	void *dev = reinterpret_cast<void *>(self);
	HidSlot *s = nullptr;
	for (auto &h : gHid)
		if (h.dev == dev) {
			s = &h;
			break;
		}
	if (!s && dev) {
		for (auto &h : gHid) {
			void *none = nullptr;
			if (__atomic_compare_exchange_n(&h.dev, &none, dev, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
				reinterpret_cast<OSObject *>(dev)->retain();  // kept for the property lookup in the log thread
				s = &h;
				break;
			}
			if (none == dev) {
				s = &h;
				break;
			}
		}
	}
	if (s) {
		uint64_t now = diagUs();
		s->n++;
		s->bytes = static_cast<uint32_t>(bytes);
		if (static_cast<uint32_t>(status)) {
			s->err++;
			s->lastErr = static_cast<uint32_t>(status);
		}
		if (s->lastUs) {
			uint64_t gap = now - s->lastUs;
			if (gap < 2000)
				s->burst++;  // ReportInterval is 8 ms: < 2 ms apart = delivered in a batch
			else if (gap >= 20000 && gap < 1000000)
				s->late++;  // 20 ms .. 1 s: late (>= 1 s counts as idle)
			if (gap < 1000000 && gap / 1000 > s->gapMaxMs)
				s->gapMaxMs = static_cast<uint32_t>(gap / 1000);
		}
		s->lastUs = now;
	}
	return reinterpret_cast<Fn6>(orgHidReadComplete)(self, param, status, bytes, e, f);
}

// ---- log thread ---------------------------------------------------------------------------------------

int symb(char *out, size_t n, uint64_t a) {
	for (auto k = kmodList(); k; k = k->next) {
		if (k->address && k->size && a >= k->address && a - k->address < k->size) {
			const char *nm = k->name;
			if (!strncmp(nm, "com.apple.", 10))
				nm += 10;
			return snprintf(out, n, " %s+%llx", nm, a - static_cast<uint64_t>(k->address));
		}
	}
	vm_offset_t u = 0;
	vm_kernel_unslide_or_perm_external(static_cast<vm_offset_t>(a), &u);
	if (u >= 0xffffff8000000000ULL && u < 0xffffff8010000000ULL)
		return snprintf(out, n, " k+%llx", static_cast<uint64_t>(u) - 0xffffff8000000000ULL);
	return snprintf(out, n, " ?");
}

uint32_t usageOf(void *dev, const char *key) {
	auto num = OSDynamicCast(OSNumber, reinterpret_cast<IOService *>(dev)->getProperty(key));
	return num ? num->unsigned32BitValue() : 0xffffffff;
}

void diagThread(void *, wait_result_t) {
	uint32_t lines = 0, btDone = 0;
	CurStat lastC {};
	GateSlot lastG[GateSlots] {};
	HidSlot lastH[HidSlots] {};
	bool cbLogged[GateSlots] {}, rejLogged = false;
	while (lines < 600) {
		IOSleep(2000);
		CurStat c = gC;
		gC.durMaxUs = gC.waitMaxUs = gC.gapMaxMs = 0;
		uint32_t calls = c.calls - lastC.calls;
		if (calls || c.fails != lastC.fails) {
			LOG("iogdiag: 2s: extSetCursorPosition %u (fail %u, last %x) at %d,%d; duration max %u us avg %llu us, "
			    ">=20ms %u >=200ms %u; gate wait max %u us; call gap max %u ms; at entry: shmem none %u, frame bad %u, "
			    "no move flag %u, sema busy %u, sw cursor %u, hw %u, deferred %u, hidden %u",
			    calls, c.fails - lastC.fails, c.lastRet, c.x, c.y, c.durMaxUs,
			    calls ? (c.durSumUs - lastC.durSumUs) / calls : 0, c.n20ms - lastC.n20ms, c.n200ms - lastC.n200ms,
			    c.waitMaxUs, c.gapMaxMs, c.noShmem - lastC.noShmem, c.badFrame - lastC.badFrame,
			    c.noMoveFlag - lastC.noMoveFlag, c.semaBusy - lastC.semaBusy, c.sw - lastC.sw, c.hw - lastC.hw,
			    c.deferred - lastC.deferred, c.hidden - lastC.hidden);
			lines++;
		}
		lastC = c;
		if (gGateRejected && !rejLogged) {
			LOG("gatediag: %u work loop(s) with an unexpected vtable: gate callback not chained there", gGateRejected);
			rejLogged = true;
			lines++;
		}
		for (size_t i = 0; i < GateSlots; i++) {
			if (!gGate[i].wl)
				continue;
			if (!cbLogged[i]) {
				char cb[96] = " none";
				if (gGateOrgCb[i])
					symb(cb, sizeof(cb), gGateOrgCb[i]);
				LOG("gatediag: wl%zu chained (original callback%s), %u work loops wired, %u rejected (unexpected vtable)",
				    i, cb, gGateWired, gGateRejected);
				cbLogged[i] = true;
				lines++;
			}
			GateSlot g = gGate[i];
			gGate[i].holdMaxUs = gGate[i].blockedMaxUs = 0;
			if (g.n20ms != lastG[i].n20ms || g.blockedCursor != lastG[i].blockedCursor) {
				LOG("gatediag: 2s: wl%zu holds %u, max %u ms, >=20ms %u >=100ms %u >=1s %u; held while a cursor call "
				    "waited %u (longest cursor wait %u ms)",
				    i, g.holds - lastG[i].holds, g.holdMaxUs / 1000, g.n20ms - lastG[i].n20ms,
				    g.n100ms - lastG[i].n100ms, g.n1s - lastG[i].n1s, g.blockedCursor - lastG[i].blockedCursor,
				    g.blockedMaxUs / 1000);
				lines++;
			}
			lastG[i] = g;
		}
		uint32_t btN = __atomic_load_n(&gBtN, __ATOMIC_RELAXED);
		for (; btDone < BtMax && btDone < btN && __atomic_load_n(&gBt[btDone].ready, __ATOMIC_ACQUIRE); btDone++) {
			const Bt &b = gBt[btDone];
			char buf[640];
			int at = 0;
			for (uint32_t j = 0; j < b.n && j < BtFrames && at < static_cast<int>(sizeof(buf)) - 64; j++)
				at += symb(buf + at, sizeof(buf) - at, reinterpret_cast<uint64_t>(b.f[j]));
			buf[at < static_cast<int>(sizeof(buf)) ? at : sizeof(buf) - 1] = 0;
			LOG("gatediag: bt #%u at %llu ms: wl%u held %u ms by tid %llu%s:%s", btDone, b.atMs, b.slot, b.holdMs, b.tid,
			    b.cursor ? " while a cursor call waited" : "", buf);
			lines++;
		}
		for (size_t i = 0; i < HidSlots; i++) {
			HidSlot h = gHid[i];
			gHid[i].gapMaxMs = 0;
			if (!h.dev || h.n == lastH[i].n) {
				lastH[i] = h;
				continue;
			}
			LOG("hiddiag: 2s: HID%zu (usage page %u usage %u, ReportInterval %u us): reports %u (err %u, last %x, %u bytes), "
			    "<2ms %u, 20ms-1s late %u, max gap <1s %u ms",
			    i, usageOf(h.dev, "PrimaryUsagePage"), usageOf(h.dev, "PrimaryUsage"), usageOf(h.dev, "ReportInterval"),
			    h.n - lastH[i].n, h.err - lastH[i].err, h.lastErr, h.bytes, h.burst - lastH[i].burst,
			    h.late - lastH[i].late, h.gapMaxMs);
			lines++;
			lastH[i] = h;
		}
	}
	LOG("iogdiag: log budget used (600 lines), counters keep running silently");
}

}  // namespace

void iogDiagRoute(KernelPatcher &patcher) {
	static bool done;
	if (done || checkKernelArgument("-rmbnoiogdiag"))
		return;
	done = true;
	bool any = false;
	uint64_t textSize = 0;
	auto iog = verifiedText(IogImage, textSize);
	if (iog) {
		bool pro = bytesAt(iog, textSize, RvaExtSetCursorPosition, ExtSetCursorPositionPro, sizeof(ExtSetCursorPositionPro));
		bool gate = bytesAt(iog, textSize, RvaCloseGateHook, CloseGateHookBytes, sizeof(CloseGateHookBytes)) &&
		            bytesAt(iog, textSize, RvaOpenGateHook, OpenGateHookBytes, sizeof(OpenGateHookBytes));
		if (!pro) {
			LOG("iogdiag: IOGraphicsFamily LC_UUID 74AA6483 ok but extSetCursorPosition +%x prologue differs: off", RvaExtSetCursorPosition);
		} else {
			gIogBase = iog;
			patcher.clearError();
			orgExtSetCursorPosition = patcher.routeFunctionLong(iog + RvaExtSetCursorPosition,
				reinterpret_cast<mach_vm_address_t>(wrapExtSetCursorPosition), true, true);
			patcher.clearError();
			if (orgExtSetCursorPosition) {
				any = true;
				gGateOn = gate && !checkKernelArgument("-rmbnogatediag");
				LOG("iogdiag: IOGraphicsFamily LC_UUID 74AA6483-8AB4-3A94-8143-5E306C16D7DA verified, extSetCursorPosition "
				    "+%x routed; gate callback chain %s", RvaExtSetCursorPosition,
				    gGateOn ? "armed (wired at the first cursor call)" : gate ? "off (-rmbnogatediag)" : "off (gate bytes differ)");
			} else {
				LOG("iogdiag: extSetCursorPosition route FAILED");
			}
		}
	}
	if (!checkKernelArgument("-rmbnohiddiag")) {
		auto hid = verifiedText(HidImage, textSize);
		if (hid && bytesAt(hid, textSize, RvaInterruptReadComplete, InterruptReadCompletePro, sizeof(InterruptReadCompletePro))) {
			patcher.clearError();
			orgHidReadComplete = patcher.routeFunctionLong(hid + RvaInterruptReadComplete,
				reinterpret_cast<mach_vm_address_t>(wrapHidReadComplete), true, true);
			patcher.clearError();
			if (orgHidReadComplete) {
				any = true;
				LOG("hiddiag: IOUSBHostHIDDevice LC_UUID EC3E3338-6B2D-3DF1-A753-B35AD4AF9A25 verified, interruptReadComplete "
				    "+%x routed", RvaInterruptReadComplete);
			} else {
				LOG("hiddiag: interruptReadComplete route FAILED");
			}
		} else if (hid) {
			LOG("hiddiag: IOUSBHostHIDDevice interruptReadComplete +%x prologue differs: off", RvaInterruptReadComplete);
		}
	}
	if (any) {
		thread_t t;
		if (kernel_thread_start(diagThread, nullptr, &t) == KERN_SUCCESS)
			thread_deallocate(t);
	}
}
