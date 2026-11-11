//
//  vramcrc.cpp: does the carve-out ("VRAM") survive bare-metal S3?
//
//  BM 10-27 15:10: after a long S3 the hardware cursor showed random coloured lines and the
//  desktop came back grey. AmdCursor::updateCursorImage copies the cursor into its VRAM buffer
//  only when WindowServer sends a new shape, so a garbage cursor means its VRAM buffer changed
//  behind the driver's back. Read-only: hash the first 4 KB of every 1 MB of the carve-out, the
//  cursor buffer and the start of the scanout surface at sleep, and compare at each wake step to
//  see who changes them (firmware S3 resume, our wake POST, Apple's wake path, later).
//  BM 10-27 15:40: the cursor address was never inside the carve-out (logged "@0" = untracked), so
//  when it lies in the GART aperture hash its pages through the PTEs saved at sleep, plus the PTEs.
//  -rmbnovramcrc: off.
//

#include "rmb.hpp"
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/thread.h>

namespace {

constexpr uint32_t GC0 = 0x1260;
constexpr uint32_t FbBase = 0x16fc, FbTop = 0x16fd, FbOffset = 0x16e7;
// DCN 3.1.2 HUBP0 (seg2 0x34C0): HUBPREQ0 primary surface, CURSOR0_0 control/address/size.
constexpr uint32_t PrimLo = 0x3aca, PrimHi = 0x3acb, CurCtl = 0x3b38, CurLo = 0x3b39, CurHi = 0x3b3a, CurSize = 0x3b3b;
constexpr uint64_t Chunk = 1ULL << 20, Sample = 0x1000, Window = 64ULL << 20;
constexpr unsigned MaxChunks = 4096;

struct Snap {
	bool ok;
	uint64_t chunk[MaxChunks];
	uint64_t cur, prim, curPte;
};

// Geometry, taken while the GC registers are alive (MODE2 at sleep resets them).
uint64_t gFbMc, gFbPhys, gFbSize;
uint32_t gCurCtl, gCurSize;
uint64_t gCurOff = ~0ULL, gPrimOff = ~0ULL;
uint64_t gCurLen;
uint64_t gCurAddr;               // raw CURSOR_SURFACE_ADDRESS (MC)
constexpr unsigned CurPages = 0x40000 / 0x1000;
uint64_t gCurIdx = ~0ULL, gCurPt; // GART: PTE index of the cursor's first page, table offset in carve-out
uint64_t gCurPhys[CurPages];      // sleep-time physical pages of the cursor
Snap gSleep, gLate;

uint64_t fnv(const volatile uint64_t *p, size_t n, uint64_t h = 0xcbf29ce484222325ULL) {
	for (size_t i = 0; i < n; i++)
		h = (h ^ p[i]) * 0x100000001b3ULL;
	return h;
}

bool geometry() {
	uint32_t base = mmioRead(GC0 + FbBase), top = mmioRead(GC0 + FbTop), off = mmioRead(GC0 + FbOffset);
	if (base == 0xFFFFFFFF || !off || top < base)
		return gFbSize != 0;
	gFbMc = (uint64_t)(base & 0xFFFFFF) << 24;
	gFbPhys = (uint64_t)(off & 0xFFFFFF) << 24;
	gFbSize = ((uint64_t)((top & 0xFFFFFF) - (base & 0xFFFFFF)) + 1) << 24;
	if (gFbSize > MaxChunks * Chunk)
		gFbSize = MaxChunks * Chunk;
	return true;
}

// Cursor and scanout buffer of pipe 0 as VRAM offsets; kept from the last time they were set.
void dcnBuffers() {
	uint64_t cur = (uint64_t)(mmioRead(CurHi) & 0xFFFF) << 32 | mmioRead(CurLo);
	uint64_t prim = (uint64_t)(mmioRead(PrimHi) & 0xFFFF) << 32 | mmioRead(PrimLo);
	uint32_t ctl = mmioRead(CurCtl), size = mmioRead(CurSize);
	if ((ctl & 1) && cur && size && size != 0xFFFFFFFF) {
		uint32_t w = (size >> 16) & 0x1FF, h = size & 0x1FF, pitch = 64u << ((ctl >> 16) & 3);
		gCurAddr = cur, gCurCtl = ctl, gCurSize = size;
		gCurLen = (uint64_t)(pitch > w ? pitch : w) * (h ? h : 1) * 4;
		if (gCurLen > 0x40000)
			gCurLen = 0x40000;
		gCurOff = cur >= gFbMc && cur < gFbMc + gFbSize ? cur - gFbMc : ~0ULL;
	}
	if (prim >= gFbMc && prim < gFbMc + gFbSize)
		gPrimOff = prim - gFbMc;
}

// Uncached read of [off, off + len) of the carve-out by physical address.
uint64_t hashRange(uint64_t off, uint64_t len) {
	auto md = IOMemoryDescriptor::withPhysicalAddress(gFbPhys + off, len, kIODirectionIn);
	if (!md)
		return 0;
	auto map = md->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapInhibitCache | kIOMapReadOnly);
	md->release();
	if (!map)
		return 0;
	uint64_t h = fnv(reinterpret_cast<const volatile uint64_t *>(map->getVirtualAddress()), len / 8);
	map->release();
	return h;
}

// Cursor outside the carve-out: GART (VMID 0) page index and table offset, valid while GC is up.
void cursorGart() {
	gCurIdx = ~0ULL;
	if (!gCurAddr || gCurOff != ~0ULL)
		return;
	uint32_t ptLo = mmioRead(GC0 + 0x1667), ptHi = mmioRead(GC0 + 0x1668);
	uint64_t start = (uint64_t)mmioRead(GC0 + 0x1688) << 32 | mmioRead(GC0 + 0x1687);
	uint64_t end = (uint64_t)mmioRead(GC0 + 0x16a8) << 32 | mmioRead(GC0 + 0x16a7);
	uint64_t base = ((uint64_t)ptHi << 32 | ptLo) & 0x0000FFFFFFFFF000ULL, page = gCurAddr >> 12;
	if (ptLo == 0xFFFFFFFF || page < start || page + CurPages - 1 > end)
		return;
	gCurPt = base >= gFbMc && base < gFbMc + gFbSize ? base - gFbMc : base - gFbPhys;
	gCurIdx = page - start;
}

// PTE hash of the cursor pages now; with save, also keep their physical pages.
uint64_t cursorPtes(bool save) {
	if (gCurIdx == ~0ULL)
		return 0;
	uint64_t h = 0xcbf29ce484222325ULL;
	for (unsigned i = 0; i < CurPages; i++) {
		uint64_t e = vramRead64(gCurPt + (gCurIdx + i) * 8);
		if (save)
			gCurPhys[i] = !(e & 1) ? 0 : e & 2 ? e & 0x0000FFFFFFFFF000ULL : gFbPhys + (e & 0x0000FFFFFFFFF000ULL);  // bit 1 = system
		h = (h ^ e) * 0x100000001b3ULL;
	}
	return h;
}

// Content of the sleep-time cursor pages (system RAM), wherever the GART points now.
uint64_t cursorPages() {
	uint64_t h = 0xcbf29ce484222325ULL;
	for (unsigned i = 0; i < CurPages && i * 0x1000ULL < gCurLen; i++) {
		if (!gCurPhys[i])
			continue;
		auto md = IOMemoryDescriptor::withPhysicalAddress(gCurPhys[i], 0x1000, kIODirectionIn);
		if (!md)
			continue;
		auto map = md->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapInhibitCache | kIOMapReadOnly);
		md->release();
		if (!map)
			continue;
		h = fnv(reinterpret_cast<const volatile uint64_t *>(map->getVirtualAddress()), 0x1000 / 8, h);
		map->release();
	}
	return h;
}

bool take(Snap &s) {
	s.ok = false;
	if (!gFbSize)
		return false;
	unsigned n = static_cast<unsigned>(gFbSize / Chunk);
	for (uint64_t w = 0; w < gFbSize; w += Window) {
		uint64_t len = gFbSize - w < Window ? gFbSize - w : Window;
		auto md = IOMemoryDescriptor::withPhysicalAddress(gFbPhys + w, len, kIODirectionIn);
		if (!md)
			return false;
		auto map = md->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapInhibitCache | kIOMapReadOnly);
		md->release();
		if (!map)
			return false;
		auto base = map->getVirtualAddress();
		for (uint64_t c = 0; c < len; c += Chunk) {
			unsigned i = static_cast<unsigned>((w + c) / Chunk);
			if (i < n)
				s.chunk[i] = fnv(reinterpret_cast<const volatile uint64_t *>(base + c), Sample / 8);
		}
		map->release();
	}
	s.cur = gCurOff != ~0ULL ? hashRange(gCurOff, gCurLen) : gCurIdx != ~0ULL ? cursorPages() : 0;
	s.curPte = cursorPtes(false);
	s.prim = gPrimOff != ~0ULL ? hashRange(gPrimOff, 0x10000) : 0;
	s.ok = true;
	return true;
}

void compare(const char *tag, const Snap &now, const Snap &ref, const char *refName) {
	if (!now.ok || !ref.ok) {
		LOG("vramcrc: %s vs %s: no snapshot (%u/%u)", tag, refName, now.ok, ref.ok);
		return;
	}
	unsigned n = static_cast<unsigned>(gFbSize / Chunk), changed = 0, shown = 0;
	char list[200];
	size_t pos = 0;
	list[0] = '\0';
	for (unsigned i = 0; i < n; i++) {
		if (now.chunk[i] == ref.chunk[i])
			continue;
		if (changed++ < 24 && pos < sizeof(list) - 8)
			pos += snprintf(list + pos, sizeof(list) - pos, " %u", i), shown++;
	}
	LOG("vramcrc: %s vs %s: %u of %u MB changed (MB:%s%s); cursor %llx %s %llu B %s, PTEs %s; scanout @%llx %s", tag,
	    refName, changed, n, list, changed > shown ? " ..." : "", gCurAddr,
	    gCurOff != ~0ULL ? "vram" : gCurIdx != ~0ULL ? "gart" : "untracked", gCurLen,
	    now.cur == ref.cur ? "same" : "CHANGED", gCurIdx == ~0ULL ? "-" : now.curPte == ref.curPte ? "same" : "CHANGED",
	    gPrimOff, now.prim == ref.prim ? "same" : "CHANGED");
}

void report(const char *tag) {
	static Snap now;
	if (!take(now)) {
		LOG("vramcrc: %s: snapshot failed (fb phys %llx size %llx)", tag, gFbPhys, gFbSize);
		return;
	}
	compare(tag, now, gSleep, "sleep");
	compare(tag, now, gLate, "psp-sleep");
}

bool enabled() {
	static int on = -1;
	if (on < 0)
		on = checkKernelArgument("-rmbvramcrc");
	return on;
}

void lateThread(void *, wait_result_t) {
	IOSleep(15000);
	vramCrcStep("wake+15s");
	thread_terminate(current_thread());
}

}  // namespace

void vramCrcTrack() {
	if (enabled() && geometry())
		dcnBuffers();
}

// Sleep entry (WillSleep / capability change): geometry + cursor regs + reference snapshot.
// psp-sleep (after Apple's power_down_event, GPU idle): second reference. Wake steps compare.
void vramCrcStep(const char *tag) {
	if (!enabled() || !mmioAvailable())
		return;
	uint64_t t0 = mach_absolute_time(), ns;
	if (!strcmp(tag, "sleep")) {
		if (!geometry())
			return;
		cursorGart();
		cursorPtes(true);
		take(gSleep);
		gLate.ok = false;
		absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
		LOG("vramcrc: sleep: fb mc %llx phys %llx size %llx, cursor ctl %x size %x addr %llx (vram off %llx, gart idx %llx pt +%llx, page0 phys %llx), scanout @%llx, %u MB hashed in %llu ms",
		    gFbMc, gFbPhys, gFbSize, gCurCtl, gCurSize, gCurAddr, gCurOff, gCurIdx, gCurPt, gCurPhys[0], gPrimOff,
		    gSleep.ok ? (unsigned)(gFbSize / Chunk) : 0, ns / 1000000);
		return;
	}
	if (!strcmp(tag, "psp-sleep")) {
		take(gLate);
		compare(tag, gLate, gSleep, "sleep");
		return;
	}
	if (!gSleep.ok)
		return;
	report(tag);
	if (!strcmp(tag, "full wake")) {
		thread_t t;
		if (kernel_thread_start(lateThread, nullptr, &t) == KERN_SUCCESS)
			thread_deallocate(t);
	}
}
