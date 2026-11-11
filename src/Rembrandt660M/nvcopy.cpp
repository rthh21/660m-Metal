//
// nvcopy.cpp - faster frame copy for NullMoth's NVIDIA (RTX 5070 Ti) display path.
//
// With our 680M accelerator running, WindowServer composites on the 680M and NullMoth 2.6.3
// (com.nullmoth.NVAccel) shows a monitor on the 5070 Ti through its "copy path":
// NVDisplayPipe::performTransaction copies every composited frame from the IOSurface into the
// NVIDIA scanout (BAR1) with a 4-byte-at-a-time CPU loop (33 MB per 4K frame). Its zero-copy
// flip cannot engage here (debug.nvaccel_iop_flip_unwritten: the NVIDIA plugin never rendered
// the surface, the 680M did). We replace that loop (file/vm 0x5148..0x522b in 2.6.3) with a call
// to rmbNvCopy(), which times every frame and copies by the mode in sysctl debug.rmb_nvcopy
// (-1 default = auto: cycle 0..4, 12 s each, twice, then keep the fastest):
//   0 = same 4-byte loop as NullMoth (baseline)
//   1 = rep movsb per row (fast-string copy)
//   2 = 8-byte loads + movnti non-temporal 8-byte stores, sfence per frame (no SSE in kernel)
//   3 = diagnostic: row -> cached bounce buffer (read time) -> scanout (write time), rep movsb
//   4 = movnti into our own write-combining mapping of the scanout physical range
// First frame logs dst/src physical range + PAT memory type from a page-table walk.
// Log every 5 s while frames come: "nvcopy: mode M: N frames, avg/max us, MB/s" (+ read/write
// split in mode 3). -rmbnonvcopy = leave NullMoth untouched.
//

#include <Headers/kern_api.hpp>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <sys/sysctl.h>
#include "rmb.hpp"

namespace {

constexpr int kModes = 5;
// default 4 (movnti into own WC alias, PTEs forced to WC). debug.rmb_nvcopy: -1 = auto (cycle 0..4 for kSlotNs each, twice, then keep the fastest);
// writing 0..4 stops auto and pins that mode.
int gMode = 4;
SYSCTL_INT(_debug, OID_AUTO, rmb_nvcopy, CTLFLAG_RW | CTLFLAG_KERN, &gMode, 0,
	"RembrandtGfx NullMoth copy: -1 auto, 0 dword loop, 1 rep movsb, 2 movnti, 3 read/write split diag, 4 movnti to own WC scanout map");

struct Stats {
	uint64_t frames, ns, maxNs, bytes, readNs, writeNs;
};
Stats gStats[kModes];
struct Total {
	uint64_t frames, ns, bytes;
};
Total gTotal[kModes];  // auto-cycle totals per mode
uint64_t gLastLog;
int gLastMode = -1;
uint8_t gBounce[65536];  // one row (4K BGRA = 15 KB, 8K = 30 KB)

constexpr uint64_t kSlotNs = 12000000000ULL;
uint64_t gAutoStart;   // ns of first frame
int gAutoPicked = -1;  // fastest mode after the cycle
bool gAutoDone;

// Own write-combining mappings of the NVIDIA scanout (dst) by kernel VA.
struct WcMap {
	const uint8_t *va;
	size_t len;
	IOMemoryMap *map;
	uint8_t *wc;
	bool failed;
};
WcMap gWc[8];

extern "C" int rdmsr_carefully(uint32_t msr, uint32_t *lo, uint32_t *hi);
extern "C" unsigned long long ml_phys_read_double_64(addr64_t paddr64);
extern "C" void ml_phys_write_double_64(addr64_t paddr64, unsigned long long data);
extern "C" uint32_t mp_cpus_call(uint64_t cpus, int mode, void (*action)(void *), void *arg);

// Walk the current page tables for a kernel VA: physical address + memory type from PAT.
// Returns false if not mapped. type = PAT entry (0 UC, 1 WC, 4 WT, 5 WP, 6 WB, 7 UC-).
bool vaInfo(const void *va, uint64_t &pa, int &type, uint64_t &pageSize) {
	uint64_t cr3;
	asm volatile("mov %%cr3, %0" : "=r"(cr3));
	uint64_t v = reinterpret_cast<uint64_t>(va), t = cr3 & 0x000ffffffffff000ULL, e = 0;
	const int shifts[] {39, 30, 21, 12};
	int level = 0;
	for (; level < 4; level++) {
		e = ml_phys_read_double_64(t + ((v >> shifts[level]) & 0x1ff) * 8);
		if (!(e & 1))
			return false;
		if (level == 3 || (level > 0 && (e & 0x80)))
			break;
		t = e & 0x000ffffffffff000ULL;
	}
	int s = shifts[level];
	pageSize = 1ULL << s;
	pa = (e & 0x000ffffffffff000ULL & ~(pageSize - 1)) | (v & (pageSize - 1));
	bool pat = level == 3 ? (e & 0x80) : (e & 0x1000);
	int idx = (pat ? 4 : 0) | ((e & 0x10) ? 2 : 0) | ((e & 0x8) ? 1 : 0);
	uint32_t lo = 0, hi = 0;
	if (rdmsr_carefully(0x277, &lo, &hi))
		lo = 0x00070406, hi = 0x00070406;  // power-on default PAT
	uint64_t patMsr = (uint64_t(hi) << 32) | lo;
	type = int((patMsr >> (idx * 8)) & 7);
	return true;
}

const char *typeName(int t) {
	switch (t) {
	case 0: return "UC";
	case 1: return "WC";
	case 4: return "WT";
	case 5: return "WP";
	case 6: return "WB";
	case 7: return "UC-";
	default: return "?";
	}
}

void logBuffer(const char *what, const uint8_t *va, size_t len) {
	uint64_t pa0 = 0, pa1 = 0, ps = 0, ps1 = 0;
	int t0 = -1, t1 = -1;
	bool ok0 = vaInfo(va, pa0, t0, ps), ok1 = vaInfo(va + len - 1, pa1, t1, ps1);
	if (!ok0 || !ok1) {
		LOG("nvcopy: %s %p len %zu: page walk failed (%d %d)", what, va, len, ok0, ok1);
		return;
	}
	LOG("nvcopy: %s %p len %zu: pa %llx..%llx (%s), page %llu KB, type %s / last %s",
		what, va, len, pa0, pa1, pa1 - pa0 == len - 1 ? "contiguous" : "scattered", ps >> 10,
		typeName(t0), typeName(t1));
}

// Leaf page-table entry of a kernel VA: physical address of the entry + page size.
bool leafPte(uint64_t v, uint64_t &entryPa, uint64_t &pageSize) {
	uint64_t cr3;
	asm volatile("mov %%cr3, %0" : "=r"(cr3));
	uint64_t t = cr3 & 0x000ffffffffff000ULL;
	const int shifts[] {39, 30, 21, 12};
	for (int level = 0; level < 4; level++) {
		uint64_t at = t + ((v >> shifts[level]) & 0x1ff) * 8, e = ml_phys_read_double_64(at);
		if (!(e & 1))
			return false;
		if (level == 3 || (level > 0 && (e & 0x80))) {
			entryPa = at;
			pageSize = 1ULL << shifts[level];
			return true;
		}
		t = e & 0x000ffffffffff000ULL;
	}
	return false;
}

struct FlushRange {
	uint64_t va, len;
};
void flushAction(void *arg) {
	auto r = static_cast<FlushRange *>(arg);
	for (uint64_t a = r->va & ~0xfffULL; a < r->va + r->len; a += 4096)
		asm volatile("invlpg (%0)" : : "r"(a) : "memory");
}

// xnu ignores kIOMapWriteCombineCache for this BAR1 range (map came out UC-), so we rewrite
// the PAT bits (PWT/PCD/PAT) of our own alias' PTEs to the PAT index that holds WC, then
// invlpg the range on every CPU. Returns pages changed, -1 if PAT has no WC entry / walk failed.
int forceWc(uint8_t *va, size_t len) {
	uint32_t lo = 0, hi = 0;
	if (rdmsr_carefully(0x277, &lo, &hi))
		return -1;
	uint64_t patMsr = (uint64_t(hi) << 32) | lo;
	int idx = -1;
	for (int i = 0; i < 8; i++)
		if (((patMsr >> (i * 8)) & 7) == 1) {
			idx = i;
			break;
		}
	if (idx < 0) {
		LOG("nvcopy: mode 4: PAT %llx has no WC entry", patMsr);
		return -1;
	}
	int changed = 0;
	uint64_t v = reinterpret_cast<uint64_t>(va), end = v + len;
	while (v < end) {
		uint64_t at = 0, ps = 0;
		if (!leafPte(v, at, ps))
			return -1;
		uint64_t e = ml_phys_read_double_64(at), patBit = ps == 4096 ? 0x80 : 0x1000;
		uint64_t n = e & ~(0x8ULL | 0x10ULL | patBit);
		n |= (idx & 1 ? 0x8 : 0) | (idx & 2 ? 0x10 : 0) | (idx & 4 ? patBit : 0);
		if (n != e) {
			ml_phys_write_double_64(at, n);
			changed++;
		}
		v = (v & ~(ps - 1)) + ps;
	}
	FlushRange r {reinterpret_cast<uint64_t>(va), len};
	mp_cpus_call(~0ULL, 0 /* SYNC */, flushAction, &r);
	return changed;
}

// WC alias of the dst range (contiguous BAR1 memory), or nullptr.
uint8_t *wcAlias(uint8_t *dst, size_t len) {
	for (auto &w : gWc)
		if (w.va == dst && w.len == len)
			return w.failed ? nullptr : w.wc;
	// all slots used (NullMoth swapped buffers / modes): recycle round-robin
	static unsigned next;
	WcMap *slot = &gWc[next++ % (sizeof(gWc) / sizeof(gWc[0]))];
	if (slot->map)
		slot->map->release();
	*slot = {};
	slot->va = dst;
	slot->len = len;
	uint64_t pa0 = 0, pa1 = 0, ps = 0;
	int t = -1;
	if (!vaInfo(dst, pa0, t, ps) || !vaInfo(dst + len - 1, pa1, t, ps) || pa1 - pa0 != len - 1) {
		slot->failed = true;
		LOG("nvcopy: mode 4: dst %p not physically contiguous (pa %llx..%llx) - no WC map, using movnti on it", dst, pa0, pa1);
		return nullptr;
	}
	auto md = IOMemoryDescriptor::withPhysicalAddress(pa0, len, kIODirectionInOut);
	auto map = md ? md->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapWriteCombineCache) : nullptr;
	if (md)
		md->release();
	if (!map) {
		slot->failed = true;
		LOG("nvcopy: mode 4: WC map of pa %llx len %zu failed", pa0, len);
		return nullptr;
	}
	slot->map = map;
	slot->wc = reinterpret_cast<uint8_t *>(map->getVirtualAddress());
	uint64_t pa = 0;
	vaInfo(slot->wc, pa, t, ps);
	int before = t, forced = 0;
	if (t != 1)
		forced = forceWc(slot->wc, len);
	vaInfo(slot->wc, pa, t, ps);
	int t2 = t;
	vaInfo(slot->wc + len - 1, pa, t, ps);
	LOG("nvcopy: mode 4: dst %p (pa %llx len %zu) -> own map %p type %s, PTE rewrite %d pages -> type %s / last %s",
		dst, pa0, len, slot->wc, typeName(before), forced, typeName(t2), typeName(t));
	return slot->wc;
}

// Auto mode for this frame (now = ns uptime).
int autoMode(uint64_t now) {
	if (gAutoDone)
		return gAutoPicked;
	if (!gAutoStart)
		gAutoStart = now;
	uint64_t slot = (now - gAutoStart) / kSlotNs;
	if (slot < 2 * kModes)
		return int(slot % kModes);
	int best = 2;
	uint64_t bestNs = ~0ULL;
	for (int m = 0; m < kModes; m++) {
		auto &t = gTotal[m];
		uint64_t per = t.frames ? t.ns / t.frames : ~0ULL;
		LOG("nvcopy: auto result: mode %d: %llu frames avg %llu us (%llu MB/s)", m, t.frames,
			t.frames ? per / 1000 : 0, t.ns ? t.bytes * 1000 / t.ns : 0);
		if (t.frames && per < bestNs)
			bestNs = per, best = m;
	}
	if (best == 3)  // diag mode: same speed class as 1, keep the plain copy
		best = 1;
	gAutoPicked = best;
	gAutoDone = true;
	LOG("nvcopy: auto: keeping mode %d (sudo sysctl -w debug.rmb_nvcopy=N pins another)", best);
	return best;
}

inline void copyDwords(uint8_t *dst, const uint8_t *src, size_t n) {
	auto d = reinterpret_cast<volatile uint32_t *>(dst);
	auto s = reinterpret_cast<const volatile uint32_t *>(src);
	for (size_t i = 0; i < n / 4; i++)
		d[i] = s[i];
}

inline void copyMovsb(void *dst, const void *src, size_t n) {
	asm volatile("rep movsb" : "+D"(dst), "+S"(src), "+c"(n) : : "memory");
}

inline void copyMovnti(uint8_t *dst, const uint8_t *src, size_t n) {
	size_t q = n / 8;
	auto s = reinterpret_cast<const uint64_t *>(src);
	auto d = reinterpret_cast<uint64_t *>(dst);
	size_t i = 0;
	for (; i + 4 <= q; i += 4) {
		uint64_t a = s[i], b = s[i + 1], c = s[i + 2], e = s[i + 3];
		asm volatile("movnti %0, (%1)" : : "r"(a), "r"(d + i) : "memory");
		asm volatile("movnti %0, (%1)" : : "r"(b), "r"(d + i + 1) : "memory");
		asm volatile("movnti %0, (%1)" : : "r"(c), "r"(d + i + 2) : "memory");
		asm volatile("movnti %0, (%1)" : : "r"(e), "r"(d + i + 3) : "memory");
	}
	for (; i < q; i++)
		asm volatile("movnti %0, (%1)" : : "r"(s[i]), "r"(d + i) : "memory");
	if (n & 7)
		copyDwords(dst + q * 8, src + q * 8, n & 7);
}

uint64_t nsSince(uint64_t t0) {
	uint64_t ns;
	absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
	return ns;
}

}  // namespace

// Called from the patched NullMoth block. Same work as the original loop: rows x
// (min(pitches) & ~3) bytes, skipped when that is under 16 bytes.
extern "C" void rmbNvCopy(uint8_t *dst, const uint8_t *src, uint32_t dstPitch, uint32_t srcPitch, uint32_t rows) {
	uint32_t row = (dstPitch < srcPitch ? dstPitch : srcPitch) & ~3u;
	if (row < 16 || !dst || !src)
		return;
	uint64_t now;
	absolutetime_to_nanoseconds(mach_absolute_time(), &now);
	if (gLastMode < 0) {
		LOG("nvcopy: first frame %ux%u bytes/row %u (pitch dst %u src %u), mode %d%s",
			row / 4, rows, row, dstPitch, srcPitch, gMode, gMode < 0 ? " = auto cycle 0-4, 12 s each, twice" : "");
		logBuffer("dst scanout", dst, size_t(dstPitch) * (rows - 1) + row);
		logBuffer("src surface", src, size_t(srcPitch) * (rows - 1) + row);
	}
	bool autoRun = gMode < 0;
	int mode = autoRun ? autoMode(now) : gMode;
	if (mode < 0 || mode >= kModes || (mode == 3 && row > sizeof(gBounce)))
		mode = 0;
	uint8_t *out = dst;
	if (mode == 4) {
		out = wcAlias(dst, size_t(dstPitch) * (rows - 1) + row);
		if (!out)
			out = dst;
	}
	uint64_t t0 = mach_absolute_time(), rd = 0, wr = 0;
	for (uint32_t y = 0; y < rows; y++) {
		auto d = out + uint64_t(y) * dstPitch;
		auto s = src + uint64_t(y) * srcPitch;
		switch (mode) {
		case 0: copyDwords(d, s, row); break;
		case 1: copyMovsb(d, s, row); break;
		case 2:
		case 4: copyMovnti(d, s, row); break;
		case 3: {
			uint64_t a = mach_absolute_time();
			copyMovsb(gBounce, s, row);
			uint64_t b = mach_absolute_time();
			copyMovsb(d, gBounce, row);
			rd += b - a;
			wr += mach_absolute_time() - b;
			break;
		}
		}
	}
	if (mode == 2 || mode == 4)
		asm volatile("sfence" ::: "memory");
	uint64_t ns = nsSince(t0);
	if (autoRun && !gAutoDone) {
		auto &t = gTotal[mode];
		t.frames++;
		t.ns += ns;
		t.bytes += uint64_t(row) * rows;
	}
	auto &st = gStats[mode];
	st.frames++;
	st.ns += ns;
	st.bytes += uint64_t(row) * rows;
	if (ns > st.maxNs)
		st.maxNs = ns;
	if (mode == 3) {
		uint64_t r, w;
		absolutetime_to_nanoseconds(rd, &r);
		absolutetime_to_nanoseconds(wr, &w);
		st.readNs += r;
		st.writeNs += w;
	}
	if (mode != gLastMode || now - gLastLog >= 5000000000ULL) {
		gLastMode = mode;
		gLastLog = now;
		uint64_t mbps = st.ns ? st.bytes * 1000 / st.ns : 0;
		if (mode == 3)
			LOG("nvcopy: mode 3: %llu frames avg %llu us max %llu us (%llu MB/s); read src %llu us, write scanout %llu us per frame",
				st.frames, st.ns / st.frames / 1000, st.maxNs / 1000, mbps,
				st.readNs / st.frames / 1000, st.writeNs / st.frames / 1000);
		else
			LOG("nvcopy: mode %d: %llu frames avg %llu us max %llu us (%llu MB/s)",
				mode, st.frames, st.ns / st.frames / 1000, st.maxNs / 1000, mbps);
		st = {};
	}
}

// NullMoth 2.6.3 NVDisplayPipe::performTransaction, copy block at +0x8d8 (vm 0x5148):
// entry state: [rbp-0x90] dst kva, [rbp-0x30] src kva, [rbp-0x70] dst pitch, [rbp-0x48] src pitch,
// r14d rows. After the block (vm 0x5378) the function reads [rbp-0xe8] = src pitch / 4,
// [rbp-0x50] = rows, [rbp-0xd8] (stored before the block) and reassigns every other register.
void nvCopyPatch(mach_vm_address_t address, size_t size) {
	if (checkKernelArgument("-rmbnonvcopy")) {
		LOG("nvcopy: disabled (-rmbnonvcopy)");
		return;
	}
	static const uint8_t find[] {
		0x8b, 0x45, 0x90, 0x4c, 0x8b, 0x5d, 0xb8, 0x41, 0x39, 0xc3, 0x89, 0xc1, 0x41, 0x0f, 0x42, 0xcb,
		0x89, 0xca, 0xc1, 0xea, 0x02, 0x44, 0x89, 0xde, 0xc1, 0xee, 0x02, 0x48, 0x89, 0xb5, 0x18, 0xff,
		0xff, 0xff, 0x48, 0x8b, 0xb5, 0x70, 0xff, 0xff, 0xff, 0x4c, 0x89, 0x75, 0xb0,
	};
	const size_t blockLen = 0xe4;   // 0x5148..0x522b
	const int32_t toAfter = 0x230;  // block start -> vm 0x5378
	auto p = reinterpret_cast<uint8_t *>(address);
	size_t at = 0, hits = 0;
	for (size_t i = 0; i + blockLen <= size; i++)
		if (!memcmp(p + i, find, sizeof(find)))
			at = i, hits++;
	if (hits != 1) {
		LOG("nvcopy: NVAccel copy block found %zu times (want 1, NullMoth not 2.6.3?) - not patched", hits);
		return;
	}
	static const uint8_t head[] {
		0x4c, 0x8b, 0x5d, 0xb8,                    // mov r11, [rbp-0x48]   src pitch
		0x44, 0x89, 0xde,                          // mov esi, r11d
		0xc1, 0xee, 0x02,                          // shr esi, 2
		0x48, 0x89, 0xb5, 0x18, 0xff, 0xff, 0xff,  // mov [rbp-0xe8], rsi
		0x4c, 0x89, 0x75, 0xb0,                    // mov [rbp-0x50], r14
		0x4c, 0x89, 0xb5, 0x20, 0xff, 0xff, 0xff,  // mov [rbp-0xe0], r14
		0x48, 0x8b, 0xbd, 0x70, 0xff, 0xff, 0xff,  // mov rdi, [rbp-0x90]   dst
		0x48, 0x8b, 0x75, 0xd0,                    // mov rsi, [rbp-0x30]   src
		0x8b, 0x55, 0x90,                          // mov edx, [rbp-0x70]   dst pitch
		0x44, 0x89, 0xd9,                          // mov ecx, r11d         src pitch
		0x45, 0x89, 0xf0,                          // mov r8d, r14d         rows
		0x48, 0xb8,                                // movabs rax, rmbNvCopy (imm64 follows)
	};
	uint8_t stub[blockLen];
	memset(stub, 0xcc, sizeof(stub));
	size_t n = sizeof(head);
	memcpy(stub, head, n);
	auto fn = reinterpret_cast<uint64_t>(&rmbNvCopy);
	memcpy(stub + n, &fn, 8);
	n += 8;
	stub[n++] = 0xff, stub[n++] = 0xd0;               // call rax
	int32_t rel = toAfter - int32_t(n + 5);
	stub[n++] = 0xe9;                                 // jmp vm 0x5378
	memcpy(stub + n, &rel, 4);
	n += 4;
	sysctl_register_oid(&sysctl__debug_rmb_nvcopy);
	if (MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) != KERN_SUCCESS) {
		LOG("nvcopy: kernel writing failed");
		return;
	}
	memcpy(p + at, stub, sizeof(stub));
	MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
	LOG("nvcopy: NullMoth NVAccel copy loop @%llx+%zx replaced (%zu B stub), mode %d (sysctl debug.rmb_nvcopy: -1 auto, 0-4)",
		address, at, n, gMode);
}
