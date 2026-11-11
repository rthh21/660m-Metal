//
//  VM fault watchdog: polls GCVM_L2_PROTECTION_FAULT_STATUS and, on a new fault, walks the faulting
//  VMID's page tables through BAR0 and logs every level, to see which entry has a bad address.
//

#include "rmb.hpp"
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/thread.h>
#include "regsnap.hpp"

namespace {

constexpr uint32_t GC0 = 0x1260;
uint32_t gc(uint32_t off) { return mmioRead(GC0 + off); }

// Registers (gc_10_3_0): per-context CNTL at 0x15fc + vmid; PT base/start/end LO32/HI32 pairs.
constexpr uint32_t FaultStatus = 0x15c8, FaultLo = 0x15c9, FaultHi = 0x15ca, FaultCntl = 0x15c4, FbOffset = 0x16e7;
uint32_t ctxCntl(uint32_t v) { return 0x15fc + v; }
uint32_t ptBaseLo(uint32_t v) { return 0x1667 + 2 * v; }
uint32_t ptStartLo(uint32_t v) { return 0x1687 + 2 * v; }

uint64_t reg64(uint32_t lo) { return (uint64_t)gc(lo + 1) << 32 | gc(lo); }

// Translation of one GPU VA: where its 4K page lives.
struct Xlate {
	bool ok, sys;
	uint64_t addr;  // system: bus (= guest physical) address; local: offset into the carve-out
	uint64_t pte;
};

// Walks a VMID's page tables for faultPage, logging each level when verbose. Linux gfx10 layout:
// levels from PDB(depth) down to PTB, 9 bits per level, the PTB spanning block_size extra bits.
// A PDE's bits 63:59 (Block Fragment Size) say each PTE of the table below covers 2^BFS pages;
// macOS uses BFS 4 (64K PTEs), so the PTB index is the page offset >> BFS; a PTB entry with
// Translate Further (bit 56) points to a 16-entry sub-table of 4K PTEs.
Xlate walk(uint32_t status, uint64_t faultPage, bool verbose = true) {
	Xlate x {};
	uint32_t vmid = (status >> 20) & 0xF;
	uint64_t fbPhys = (uint64_t)(gc(FbOffset) & 0xFFFFFF) << 24;
	uint32_t cntl = gc(ctxCntl(vmid));
	uint32_t depth = (cntl >> 1) & 3, block = (cntl >> 3) & 0xF;
	uint64_t base = reg64(ptBaseLo(vmid)), start = reg64(ptStartLo(vmid));
	if (verbose)
		LOG("vmwatch: fault status %x (walker %u map %u cid %u rw %u) vmid %u page %llx; ctx cntl %x depth %u block %u base %llx start %llx fbPhys %llx",
			status, (status >> 1) & 7, (status >> 8) & 1, (status >> 9) & 0x7F, (status >> 18) & 1, vmid, faultPage,
			cntl, depth, block, base, start, fbPhys);
	uint64_t page = faultPage - start;
	uint32_t ptbBits = 9 + block, bfs = 0;
	uint64_t entry = base;
	uint64_t fbBase = (uint64_t)(gc(0x16fc) & 0xFFFFFF) << 24, fbTop = ((uint64_t)(gc(0x16fd) & 0xFFFFFF) << 24) | 0xFFFFFF;
	auto local = [&](uint64_t addr) { return addr >= fbBase && addr <= fbTop ? addr - fbBase : addr >= fbPhys && fbPhys ? addr - fbPhys : addr; };
	for (int level = depth; level >= 0; level--) {
		uint32_t shift = ptbBits + 9 * level;  // in pages
		uint64_t idx = level == 0 ? (page & ((1ULL << ptbBits) - 1)) >> bfs : (page >> shift) & 0x1FF;
		uint64_t addr = entry & 0x0000FFFFFFFFF000ULL;
		uint64_t next = vramRead64(local(addr) + idx * 8);
		if (verbose)
			LOG("vmwatch:  level %d table %llx idx %llx -> entry %llx%s", level, addr, idx, next,
				(next & (1ULL << 54)) ? " [PDE-as-PTE]" : "");
		if (next == ~0ULL || !(next & 1))
			return x;
		if (level != 0 && !(next & (1ULL << 54))) {
			bfs = next >> 59;
			entry = next;
			continue;
		}
		// Translate Further (bit 56): the entry points to a sub-table of 2^bfs 4K PTEs (64-byte aligned).
		if (level == 0 && (next & (1ULL << 56)) && bfs) {
			uint64_t sub = next & 0x0000FFFFFFFFFFC0ULL;
			uint64_t pte = vramRead64(local(sub) + (page & ((1ULL << bfs) - 1)) * 8);
			if (verbose)
				LOG("vmwatch:  TF sub-table %llx idx %llx -> PTE %llx", sub, page & ((1ULL << bfs) - 1), pte);
			if (pte == ~0ULL || !(pte & 1))
				return x;
			next = pte;
			bfs = 0;
		}
		// PTE (or PDE acting as one): covers 2^bfs pages at level 0, or the whole lower range.
		uint64_t span = level == 0 ? (1ULL << bfs) : (1ULL << shift);
		uint64_t pa = (next & 0x0000FFFFFFFFF000ULL) + (page & (span - 1)) * 0x1000;
		x.ok = true;
		x.sys = next & 2;
		x.addr = x.sys ? pa : local(pa);
		x.pte = next;
		return x;
	}
	return x;
}

// Reads n dwords of GPU VA va in vmid (within one page).
bool readGpuVa(uint32_t vmid, uint64_t va, uint32_t *out, uint32_t n) {
	uint64_t start = reg64(ptStartLo(vmid));
	auto x = walk(vmid << 20, va >> 12, false);
	if (!x.ok)
		return false;
	uint64_t a = x.addr + (va & 0xFFF);
	if (!x.sys) {
		for (uint32_t i = 0; i < n; i += 2) {
			uint64_t v = vramRead64(a + i * 4);
			out[i] = (uint32_t)v;
			if (i + 1 < n)
				out[i + 1] = (uint32_t)(v >> 32);
		}
		return true;
	}
	auto md = IOMemoryDescriptor::withAddressRange(a, n * 4, kIODirectionIn, nullptr);
	if (!md)
		return false;
	bool ok = md->prepare() == kIOReturnSuccess && md->readBytes(0, out, n * 4) == n * 4;
	md->complete();
	md->release();
	(void)start;
	return ok;
}

void dumpGpuVa(const char *what, uint32_t vmid, uint64_t va, uint32_t n) {
	uint32_t buf[64];
	n = n > 64 ? 64 : n;
	if ((va & 0xFFF) + n * 4 > 0x1000)
		n = (0x1000 - (va & 0xFFF)) / 4;
	if (!readGpuVa(vmid, va, buf, n)) {
		LOG("%s vmid %u va %llx: not mapped", what, vmid, va);
		return;
	}
	for (uint32_t i = 0; i < n; i += 8) {
		char line[160];
		int len = 0;
		for (uint32_t j = i; j < n && j < i + 8; j++)
			len += snprintf(line + len, sizeof(line) - len, " %08x", buf[j]);
		LOG("%s vmid %u va %llx+%x:%s", what, vmid, va, i * 4, line);
	}
}

constexpr uint32_t GC1 = 0xA000;
uint32_t gc1(uint32_t off) { return mmioRead(GC1 + off); }

void dumpTopology() {
	auto oldIdx = gc1(0x2200);
	for (uint32_t se = 0; se < 2; se++)
		for (uint32_t sh = 0; sh < 2; sh++) {
			mmioWrite(GC1 + 0x2200, (se << 16) | (sh << 8) | (1u << 31));  // INSTANCE_BROADCAST_WRITES
			LOG("topo se%u sh%u: CC_GC_SHADER_ARRAY_CONFIG %x GC_USER_SHADER_ARRAY_CONFIG %x CC_RB_BACKEND_DISABLE %x GC_USER_RB %x",
				se, sh, gc(0x100f), gc(0x1010), gc(0x13dd), gc(0x147f));
		}
	mmioWrite(GC1 + 0x2200, oldIdx);
	LOG("topo: GB_ADDR_CONFIG %x SPI_CONFIG_CNTL %x SQ_CONFIG %x RLC_PG_CNTL %x PG_ALWAYS_ON_WGP %x RLC_CGCG %x GRBM_PWR %x SH_MEM_CONFIG %x BASES %x",
		gc(0x13de), gc(0x11ec), gc(0x10a0), gc1(0x4c43), gc1(0x4c53), gc1(0x4c49), gc(0x0da3), gc(0x10ad), gc(0x10aa));
}

// COMPUTE_PGM_LO/HI persist after a dispatch: when they change on a pipe (ME0 = gfx, ME1/ME2 = MEC),
// a new compute shader was dispatched there. Dump its state and walk its code page in its VMID.
// GRBM_GFX_CNTL: PIPEID [1:0], MEID [3:2], VMID [7:4], QUEUEID [10:8].
void dumpDispatch(uint32_t me, uint32_t pipe, uint64_t pgm) {
	mmioWrite(GC0 + 0x0dc2, me << 2 | pipe);
	uint32_t init = gc(0x1ba0), dx = gc(0x1ba1), dy = gc(0x1ba2), dz = gc(0x1ba3);
	uint32_t rsrc1 = gc(0x1bb2), rsrc2 = gc(0x1bb3), rsrc3 = gc(0x1bc8), nt = gc(0x1ba7), lim = gc(0x1bb5), tmp = gc(0x1bb8);
	uint32_t se0 = gc(0x1bb6), se1 = gc(0x1bb7), vmidReg = gc(0x1bb4);
	uint32_t ud[8];
	for (uint32_t i = 0; i < 8; i++)
		ud[i] = gc(0x1be0 + i);
	LOG("dispatch me%u pipe%u: PGM %llx INIT %x DIM %u,%u,%u NUM_THREAD %x RSRC1 %x RSRC2 %x RSRC3 %x LIMITS %x TMPRING %x TMGMT %x,%x COMPUTE_VMID %x",
		me, pipe, pgm, init, dx, dy, dz, nt, rsrc1, rsrc2, rsrc3, lim, tmp, se0, se1, vmidReg);
	LOG("dispatch me%u pipe%u: USER_DATA %x %x %x %x %x %x %x %x", me, pipe, ud[0], ud[1], ud[2], ud[3], ud[4], ud[5], ud[6], ud[7]);
	for (uint32_t q = 0; q < 8; q++) {
		mmioWrite(GC0 + 0x0dc2, q << 8 | me << 2 | pipe);
		if (gc(0x1fab) & 1)
			LOG("dispatch me%u pipe%u queue%u: HQD active VMID %x PQ_BASE %x RPTR %x WPTR %x", me, pipe, q, gc(0x1fac), gc(0x1fb1),
				gc(0x1fb3), gc(0x1fdf));
	}
	mmioWrite(GC0 + 0x0dc2, 0);
	for (uint32_t v = 1; v < 16; v++) {
		if (!(gc(ctxCntl(v)) & 1) || !reg64(ptBaseLo(v)))
			continue;
		mmioWrite(GC0 + 0x0dc2, v << 4);
		LOG("dispatch vmid %u: SH_MEM_CONFIG %x SH_MEM_BASES %x", v, gc(0x10ad), gc(0x10aa));
		mmioWrite(GC0 + 0x0dc2, 0);
		walk(v << 20, pgm >> 12);
	}
}

// Shader heaps sit at the same VA in every process, so key on PGM + grid + threadgroup size, and
// report each distinct key once (the compositor alternates buffers every frame).
void sampleBusy() {
	static uint64_t seen[64];
	static uint32_t nSeen;
	auto old = gc(0x0dc2);
	for (uint32_t me = 0; me < 3; me++)
		for (uint32_t pipe = 0; pipe < (me ? 4u : 1u); pipe++) {
			mmioWrite(GC0 + 0x0dc2, me << 2 | pipe);
			uint64_t pgm = ((uint64_t)gc(0x1bad) << 32 | gc(0x1bac)) << 8;
			if (pgm >> 48 || !pgm)  // never-programmed pipe (reset garbage)
				continue;
			uint64_t key = pgm ^ (uint64_t)(me << 2 | pipe) << 56;
			static const uint32_t keyRegs[] {0x1ba1, 0x1ba2, 0x1ba3, 0x1ba7};
			for (uint32_t r : keyRegs)
				key = key * 1000003 ^ gc(r);
			bool known = false;
			for (uint32_t i = 0; i < nSeen; i++)
				known |= seen[i] == key;
			if (known || nSeen >= 64)
				continue;
			seen[nSeen++] = key;
			dumpDispatch(me, pipe, pgm);
		}
	mmioWrite(GC0 + 0x0dc2, old);
}

// Every active HQD on the MECs, logged when its read/write pointers move (at most 40 lines).
void sampleQueues() {
	static uint64_t last[2][4][8];
	static uint32_t lines;
	if (lines >= 40)
		return;
	auto old = gc(0x0dc2);
	for (uint32_t me = 1; me < 3; me++)
		for (uint32_t pipe = 0; pipe < 4; pipe++)
			for (uint32_t q = 0; q < 8; q++) {
				mmioWrite(GC0 + 0x0dc2, q << 8 | me << 2 | pipe);
				if (!(gc(0x1fab) & 1))
					continue;
				uint32_t rptr = gc(0x1fb3), wptr = gc(0x1fdf);
				uint64_t key = (uint64_t)rptr << 32 | wptr;
				if (key == last[me - 1][pipe][q])
					continue;
				last[me - 1][pipe][q] = key;
				lines++;
				LOG("hqd me%u pipe%u queue%u: VMID %x PQ_BASE %x_%x RPTR %x WPTR %x IB_BASE %x_%x IB_RPTR %x IB_CONTROL %x CP_STAT %x PQ_CONTROL %x DEQUEUE %x",
					me, pipe, q, gc(0x1fac), gc(0x1fb2), gc(0x1fb1), rptr, wptr, gc(0x1fbc), gc(0x1fbb), gc(0x1fbd), gc(0x1fbe),
					gc(0x0f40 - 0x0000), gc(0x1fba), gc(0x1fc1));
				uint32_t vmid = gc(0x1fac) & 0xF;
				uint64_t pq = ((uint64_t)gc(0x1fb2) << 32 | gc(0x1fb1)) << 8;
				uint64_t ib = (uint64_t)(gc(0x1fbc) & 0xFFFF) << 32 | gc(0x1fbb);
				static uint32_t walks;
				if (vmid && q == 0 && walks++ < 4) {
					LOG("hqd vmid %u: cntl %x PT base %llx start %llx; walking PQ %llx and IB %llx", vmid, gc(ctxCntl(vmid)),
						reg64(ptBaseLo(vmid)), reg64(ptStartLo(vmid)), pq, ib);
					walk(vmid << 20, pq >> 12);
					walk(vmid << 20, ib >> 12);
					uint32_t rp = gc(0x1fb3);
					dumpGpuVa("pq", vmid, pq + ((rp - 0x20) & ((2u << (gc(0x1fba) & 0x3F)) - 1)) * 4, 32);
					for (uint32_t off = 0; off < 0x400; off += 0x100)
						dumpGpuVa("ib", vmid, ib + off, 64);
				}
			}
	mmioWrite(GC0 + 0x0dc2, old);
}

// DMCUB (DCN segment 2 = 0x34C0; dcn_3_1_2 offsets, same as 3.0.2): run state, boot status
// (SCRATCH0), inbox pointers, faults, and the code windows DAL programmed.
void dumpDmub(const char *when) {
	constexpr uint32_t D = 0x34C0;
	auto r = [](uint32_t off) { return mmioRead(0x34C0 + off); };
	LOG("dmub %s: CNTL %x CNTL2 %x SEC_CNTL %x MEM_CNTL %x TIMER %x SCRATCH0-7 %x %x %x %x %x %x %x %x", when, r(0x1f6), r(0x200),
		r(0x1ce), r(0x1cf), r(0x1fd), r(0x1e3), r(0x1e4), r(0x1e5), r(0x1e6), r(0x1e7), r(0x1e8), r(0x1e9), r(0x1ea));
	LOG("dmub %s: INBOX1 base %x size %x wptr %x rptr %x; faults ifetch %x dwrite %x undef %x; SCRATCH10-15 %x %x %x %x %x %x",
		when, r(0x1d4), r(0x1d5), r(0x1d6), r(0x1d7), r(0x1cc), r(0x1cd), r(0x1fa), r(0x1ed), r(0x1ee), r(0x1ef), r(0x1f0), r(0x1f1), r(0x1f2));
	for (uint32_t w = 0; w < 8; w++)
		LOG("dmub %s: CW%u base %x top %x offset %x_%x", when, w, r(0x1a5 + w), r(0x1ad + w), r(0x1b6 + 2 * w), r(0x1b5 + 2 * w));
	(void)D;
}

// Same register list as scripts/linux-gc-snapshot.sh, for diffing with scripts/gc-snapdiff.py.
void dumpSnapshot() {
	constexpr size_t n = sizeof(kGcSnapRegs) / sizeof(kGcSnapRegs[0]);
	for (size_t i = 0; i < n; i += 6) {
		char line[200];
		int len = 0;
		for (size_t j = i; j < n && j < i + 6; j++)
			len += snprintf(line + len, sizeof(line) - len, " %x=%x", kGcSnapRegs[j], mmioRead(kGcSnapRegs[j]));
		LOG("snap%s", line);
	}
	LOG("snap done (%zu registers)", n);
}

void watch(void *, wait_result_t) {
	uint32_t reported = 0;
	IOSleep(2000);
	if (rmbVerbose()) {
		dumpTopology();
		dumpDmub("+2s");
		IOSleep(1000);
		dumpDmub("+3s");
		IOSleep(18000);
		dumpSnapshot();
	}
	for (;;) {
		IOSleep(100);
		if (!mmioAvailable())
			continue;
		sampleBusy();
		sampleQueues();
		uint32_t st = gc(FaultStatus);
		if (!st)
			continue;
		uint64_t page = (uint64_t)(gc(FaultHi) & 0xF) << 32 | gc(FaultLo);
		if (reported < 6) {
			reported++;
			walk(st, page);
		}
		// Clear so the next fault latches.
		mmioWrite(GC0 + FaultCntl, gc(FaultCntl) | 1);
		IODelay(10);
		mmioWrite(GC0 + FaultCntl, gc(FaultCntl) & ~1u);
	}
}

}  // namespace

// GART (VMID 0) page-table census through BAR0: does the table in the carve-out survive guest S3 +
// host S3? (Apple's Navi 23 code expects VRAM to stay in self-refresh like BACO and never rebuilds
// its fixed kernel mappings; after every wake the KIQ faulted on a zero PTE.) Also samples the
// first 16 MB of the carve-out every 64 KB.
void gartCensus(const char *when) {
	if (!mmioAvailable())
		return;
	uint64_t fbPhys = (uint64_t)(gc(FbOffset) & 0xFFFFFF) << 24;
	uint64_t fbBase = (uint64_t)(gc(0x16fc) & 0xFFFFFF) << 24, fbTop = ((uint64_t)(gc(0x16fd) & 0xFFFFFF) << 24) | 0xFFFFFF;
	uint64_t base = reg64(ptBaseLo(0)) & 0x0000FFFFFFFFF000ULL, start = reg64(ptStartLo(0)), end = reg64(0x16a7);
	uint64_t pt = base >= fbBase && base <= fbTop ? base - fbBase : base >= fbPhys && fbPhys ? base - fbPhys : base;
	uint64_t n = end >= start ? end - start + 1 : 0;
	if (n > 0x10000)
		n = 0x10000;
	uint32_t valid = 0, low = 0, ones = 0;
	uint64_t first = ~0ULL, sum = 0;
	for (uint64_t i = 0; i < n; i++) {
		uint64_t e = vramRead64(pt + i * 8);
		if (e == ~0ULL) {
			ones++;
			continue;
		}
		if (!e)
			continue;
		valid++;
		sum += e * (i + 1);
		if (i < 0x200)
			low++;
		if (first == ~0ULL)
			first = i;
	}
	uint32_t vnz = 0;
	for (uint64_t off = 0x100000; off < 0x1000000; off += 0x10000)
		vnz += vramRead64(off) != 0;
	LOG("gart: %s: pt vram+%llx (start %llx end %llx) %llu entries: valid %u (idx<0x200: %u, first %llx, ~0: %u) sum %llx; vram 1-16MB samples nonzero %u/240",
		when, pt, start, end, n, valid, low, first, ones, sum, vnz);
}

// GART save/restore across S3. Census (2026-10-16, guest-only S3): 1780 valid PTEs at sleep entry,
// 463 on wake before the KM ring create; the KIQ ring page (0xffbfea00) is among the lost ones and
// the wake startKIQ faults on it. Apple's Navi 23 path never re-maps those fixed kernel mappings,
// so put back every PTE that was valid at sleep and is zero now (-rmbnogartrst: don't).
static uint64_t *gGartSaved;
static uint64_t gGartPt, gGartN;

static bool gartGeometry(uint64_t &pt, uint64_t &n) {
	uint64_t fbPhys = (uint64_t)(gc(FbOffset) & 0xFFFFFF) << 24;
	uint64_t fbBase = (uint64_t)(gc(0x16fc) & 0xFFFFFF) << 24, fbTop = ((uint64_t)(gc(0x16fd) & 0xFFFFFF) << 24) | 0xFFFFFF;
	uint64_t base = reg64(ptBaseLo(0)) & 0x0000FFFFFFFFF000ULL, start = reg64(ptStartLo(0)), end = reg64(0x16a7);
	pt = base >= fbBase && base <= fbTop ? base - fbBase : base >= fbPhys && fbPhys ? base - fbPhys : base;
	n = end >= start ? end - start + 1 : 0;
	if (n > 0x10000)
		n = 0x10000;
	return n != 0;
}

void gartSave() {
	if (!mmioAvailable() || checkKernelArgument("-rmbnogartrst"))
		return;
	uint64_t pt, n;
	if (!gartGeometry(pt, n))
		return;
	if (!gGartSaved)
		gGartSaved = static_cast<uint64_t *>(IOMalloc(0x10000 * 8));
	if (!gGartSaved)
		return;
	uint32_t valid = 0;
	for (uint64_t i = 0; i < n; i++) {
		gGartSaved[i] = vramRead64(pt + i * 8);
		valid += gGartSaved[i] && gGartSaved[i] != ~0ULL;
	}
	gGartPt = pt, gGartN = n;
	LOG("gart: saved %u valid PTEs (pt vram+%llx, %llu entries)", valid, pt, n);
}

void gartRestore(const char *when) {
	if (!gGartSaved || !gGartN || !mmioAvailable())
		return;
	uint64_t pt, n;
	if (!gartGeometry(pt, n) || pt != gGartPt || n != gGartN) {
		LOG("gart: %s: geometry changed (pt %llx n %llu vs saved %llx %llu), not restoring", when, pt, n, gGartPt, gGartN);
		gGartN = 0;
		return;
	}
	uint32_t restored = 0, differ = 0, runs = 0;
	uint64_t runStart = ~0ULL;
	for (uint64_t i = 0; i <= n; i++) {
		bool fix = false;
		if (i < n) {
			uint64_t s = gGartSaved[i];
			if (s && s != ~0ULL) {
				uint64_t e = vramRead64(pt + i * 8);
				if (!e) {
					vramWrite32(pt + i * 8, s & 0xFFFFFFFF);
					vramWrite32(pt + i * 8 + 4, s >> 32);
					restored++;
					fix = true;
				} else if (e != s) {
					differ++;
				}
			}
		}
		if (fix && runStart == ~0ULL)
			runStart = i;
		if (!fix && runStart != ~0ULL) {
			if (runs++ < 8 && rmbVerbose())
				LOG("gart:   restored idx %llx-%llx", runStart, i - 1);
			runStart = ~0ULL;
		}
	}
	gGartN = 0;  // one shot per sleep
	// Flush VMID 0 translations (GCVM_INVALIDATE_ENG17, as Linux gmc_v10 does for the GART): L2
	// PTEs/PDEs + L1, legacy flush.
	mmioWrite(GC0 + 0x1630, 1u | 0x00F80000u);
	uint32_t ack = 0;
	for (int i = 0; i < 1000 && !((ack = gc(0x1642)) & 1); i++)
		IODelay(10);
	LOG("gart: %s: restored %u PTEs in %u runs (%u present but different, kept); invalidate ack %x", when, restored, runs, differ, ack);
}

void vmWatchStart() {}  // 100 ms poller removed 10-31 (user: no watchdogs)

// -rmbnosubmit bare-metal diagnostic (2026-10-25): the NVMe + xHCI die seconds after the GPU runs the
// first WindowServer command buffer, and only on bare metal (no IOMMU). Walks the GART and every
// enabled user VMID and reports page-table entries the GPU could write through that point outside
// RAM: system PTEs on pages not in the kernel's pmap_memory_regions (device MMIO, firmware-reserved)
// and local PTEs past the end of the carve-out. Also translates the IH ring + wptr writeback.
namespace {

struct PmapRegion {  // xnu osfmk/i386/pmap.h pmap_memory_region_t (15.8.1 KDK DWARF: 0x28 bytes)
	uint32_t base, allocUp, allocDown, allocFragUp, allocFragDown, end, type;
	uint64_t attribute;
};
static_assert(sizeof(PmapRegion) == 0x28, "pmap_memory_region_t layout");
PmapRegion *gRegions;
uint32_t *gRegionCount;

bool isRam(uint64_t pa) {
	uint64_t pn = pa >> 12;
	for (uint32_t i = 0; i < *gRegionCount; i++)
		if (pn >= gRegions[i].base && pn <= gRegions[i].end)
			return true;
	return false;
}

struct ScanStats {
	uint32_t sys, local, badSys, badLocal, reads;
};

void checkLeaf(ScanStats &s, uint32_t vmid, uint64_t va, uint64_t pte, uint64_t span, uint64_t fbPhys,
               uint64_t fbBase, uint64_t vramSize) {
	uint64_t pa = pte & 0x0000FFFFFFFFF000ULL;
	if (pte & 2) {
		s.sys++;
		bool ok = isRam(pa) && isRam(pa + span * 0x1000 - 1);
		if (!ok && ++s.badSys <= 12)
			LOG("ptscan: vmid %u va %llx (%llu pages) -> SYSTEM pa %llx NOT RAM (pte %llx)", vmid, va << 12, span, pa, pte);
	} else {
		s.local++;
		uint64_t off = pa >= fbBase && pa < fbBase + vramSize ? pa - fbBase : pa >= fbPhys ? pa - fbPhys : ~0ULL;
		if ((off == ~0ULL || off + span * 0x1000 > vramSize) && ++s.badLocal <= 12)
			LOG("ptscan: vmid %u va %llx (%llu pages) -> LOCAL %llx outside carve-out (off %llx size %llx, pte %llx)",
			    vmid, va << 12, span, pa, off, vramSize, pte);
	}
}

// Walks one table level; page = first VA page covered by this table.
void scanTable(ScanStats &s, uint32_t vmid, uint64_t table, int level, uint32_t ptbBits, uint32_t bfs, uint64_t page,
               uint64_t fbPhys, uint64_t fbBase, uint64_t vramSize, uint64_t flatEntries = 0) {
	auto local = [&](uint64_t a) { return a >= fbBase && a < fbBase + vramSize ? a - fbBase : a - fbPhys; };
	uint64_t entries = flatEntries ? flatEntries : level == 0 ? (1U << ptbBits) >> bfs : 512;
	uint64_t perEntry = level == 0 ? (1ULL << bfs) : (1ULL << (ptbBits + 9 * level));
	for (uint64_t i = 0; i < entries && s.reads < 4000000; i++) {
		uint64_t e = vramRead64(local(table) + i * 8);
		s.reads++;
		if (e == ~0ULL || !(e & 1))
			continue;
		uint64_t va = page + i * perEntry;
		if (level != 0 && !(e & (1ULL << 54))) {
			scanTable(s, vmid, e & 0x0000FFFFFFFFF000ULL, level - 1, ptbBits, (uint32_t)(e >> 59), va, fbPhys, fbBase, vramSize);
			continue;
		}
		if (level == 0 && (e & (1ULL << 56)) && bfs) {
			uint64_t sub = e & 0x0000FFFFFFFFFFC0ULL;
			for (uint32_t j = 0; j < (1U << bfs); j++) {
				uint64_t pte = vramRead64(local(sub) + j * 8);
				s.reads++;
				if (pte != ~0ULL && (pte & 1))
					checkLeaf(s, vmid, va + j, pte, 1, fbPhys, fbBase, vramSize);
			}
			continue;
		}
		checkLeaf(s, vmid, va, e, perEntry, fbPhys, fbBase, vramSize);
	}
}

void ihRing(const char *name, uint32_t base, uint32_t baseHi, uint32_t cntl, uint32_t wpHi, uint32_t wpLo) {
	constexpr uint32_t Oss = 0x10A0;
	uint64_t rb = ((uint64_t)mmioRead(Oss + baseHi) << 32 | mmioRead(Oss + base)) << 8;
	LOG("ptscan: IH %s cntl %08x base %llx%s", name, mmioRead(Oss + cntl), rb, rb && !isRam(rb) ? " (as pa: NOT RAM)" : "");
	if (wpHi) {
		uint64_t wp = (uint64_t)mmioRead(Oss + wpHi) << 32 | mmioRead(Oss + wpLo);
		LOG("ptscan: IH %s wptr writeback %llx%s", name, wp, wp && !isRam(wp) ? " (as pa: NOT RAM)" : "");
	}
	uint64_t start = reg64(ptStartLo(0)), end = reg64(0x16a7);
	auto x = rb >> 12 >= start && rb >> 12 <= end ? walk(0, rb >> 12, false) : Xlate {};
	if (x.ok)
		LOG("ptscan: IH %s base via GART -> %s %llx%s", name, x.sys ? "system" : "local", x.addr,
		    x.sys && !isRam(x.addr) ? " NOT RAM" : "");
}

}  // namespace

void ptScanInit(KernelPatcher &patcher) {
	gRegions = reinterpret_cast<PmapRegion *>(patcher.solveSymbol(KernelPatcher::KernelID, "_pmap_memory_regions"));
	gRegionCount = reinterpret_cast<uint32_t *>(patcher.solveSymbol(KernelPatcher::KernelID, "_pmap_memory_region_count"));
	patcher.clearError();
}

// One VMID's census. User VMID bases are plain carve-out addresses (no valid bit, e.g. 103b000000
// in the VM), the GART's has bit 0 set; walk any nonzero base.
void ptScanVmid(uint32_t vmid, bool quietIfSame) {
	uint32_t cntl = gc(ctxCntl(vmid));
	if (!(cntl & 1))
		return;
	uint64_t fbPhys = (uint64_t)(gc(FbOffset) & 0xFFFFFF) << 24;
	uint64_t fbBase = (uint64_t)(gc(0x16fc) & 0xFFFFFF) << 24;
	uint64_t vramSize = (((uint64_t)(gc(0x16fd) & 0xFFFFFF) << 24) | 0xFFFFFF) + 1 - fbBase;
	uint32_t depth = (cntl >> 1) & 3, block = (cntl >> 3) & 0xF;
	uint64_t base = reg64(ptBaseLo(vmid)), start = reg64(ptStartLo(vmid)), end = reg64(ptStartLo(vmid) + 0x20);
	ScanStats s {};
	// VMID0 (GART) is one flat table over start..end when depth 0; a PDB otherwise.
	if (base & 0x0000FFFFFFFFF000ULL)
		scanTable(s, vmid, base & 0x0000FFFFFFFFF000ULL, depth, 9 + block, 0, start, fbPhys, fbBase, vramSize,
		          depth == 0 ? end - start + 1 : 0);
	static uint64_t last[16];
	uint64_t sig = base ^ ((uint64_t)s.sys << 40) ^ ((uint64_t)s.local << 20) ^ ((uint64_t)s.badSys << 8) ^ s.badLocal;
	if (quietIfSame && last[vmid] == sig)
		return;
	last[vmid] = sig;
	LOG("ptscan: vmid %u cntl %x depth %u block %u base %llx va %llx-%llx: %u system, %u local leaves; BAD system %u, BAD local %u (%u reads)",
	    vmid, cntl, depth, block, base, start << 12, end << 12, s.sys, s.local, s.badSys, s.badLocal, s.reads);
}

void ptScan(const char *why) {
	if (!gRegions || !gRegionCount) {
		LOG("ptscan: pmap_memory_regions not found");
		return;
	}
	LOG("ptscan: %s: %u RAM regions (sizeof %zu)", why, *gRegionCount, sizeof(PmapRegion));
	for (uint32_t i = 0; i < *gRegionCount && i < 64; i++)
		LOG("ptscan:   region %u pa %llx-%llx type %u", i, (uint64_t)gRegions[i].base << 12,
		    ((uint64_t)gRegions[i].end << 12) | 0xFFF, gRegions[i].type);
	uint64_t fbPhys = (uint64_t)(gc(FbOffset) & 0xFFFFFF) << 24;
	uint64_t fbBase = (uint64_t)(gc(0x16fc) & 0xFFFFFF) << 24;
	uint64_t vramSize = (((uint64_t)(gc(0x16fd) & 0xFFFFFF) << 24) | 0xFFFFFF) + 1 - fbBase;
	LOG("ptscan: fb mc %llx phys %llx vram %llx; sysap %x-%x default %x_%x; L2 fault default %x_%x", fbBase, fbPhys, vramSize,
	    gc(0x1701), gc(0x1702), gc(0x16e9), gc(0x16e8), gc(0x15cc), gc(0x15cb));
	ihRing("ring0", 0x81, 0x82, 0x80, 0x85, 0x86);
	ihRing("ring1", 0x8d, 0x8e, 0x8c, 0, 0);
	ihRing("ring2", 0x99, 0x9a, 0x98, 0, 0);
	for (uint32_t vmid = 0; vmid < 16; vmid++)
		ptScanVmid(vmid);
	LOG("ptscan: done");
}

uint64_t ptVmidBase(uint32_t vmid) {
	return gc(ctxCntl(vmid)) & 1 ? reg64(ptBaseLo(vmid)) : 0;
}

// MMHUB 2.3 (Rembrandt) view: Apple's VMPT jobs write Navi MMHUB 2.0 offsets, which are other
// registers here (10-24 VCN hang), so check what the MM side really holds next to a live GC VMID.
void mmhubDump(const char *why) {
	constexpr uint32_t Mm = 0x1A000;
	auto mm = [](uint32_t o) { return mmioRead(Mm + o); };
	LOG("mmhub: %s: fb %x-%x off %x sysap %x-%x default %x_%x agp %x %x-%x; L2 fault status %x addr %x_%x default %x",
	    why, mm(0x8ec), mm(0x8ed), mm(0x8d7), mm(0x8f1), mm(0x8f2), mm(0x8d9), mm(0x8d8), mm(0x8f0), mm(0x8ef), mm(0x8ee),
	    mm(0x70c), mm(0x70e), mm(0x70d), mm(0x70f));
	char buf[480];
	int n = 0;
	for (uint32_t v = 0; v < 16 && n < (int)sizeof(buf) - 60; v++) {
		uint32_t c = mm(0x740 + v);
		if (c & 1)
			n += snprintf(buf + n, sizeof(buf) - n, " %u:%x b%x_%x s%x", v, c, mm(0x941 + 8 * v), mm(0x940 + 8 * v),
			              mm(0x942 + 8 * v));
	}
	buf[n] = 0;
	LOG("mmhub: %s: enabled contexts%s", why, n ? buf : " none");
}
