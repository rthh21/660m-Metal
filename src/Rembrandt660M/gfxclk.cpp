//
//  gfxclk: GFX clock policy for the Radeon 660M (Yellow Carp PMFW, SMU 13.0.1 driver interface 4).
//
//  macOS has no PowerPlay for this APU, so nothing ever tells the PMFW its GFX limits. Linux amdgpu
//  (yellow_carp_set_performance_level, level AUTO) sends SetHardMinGfxClk(MinGfxClk) and
//  SetSoftMaxGfxClk(MaxGfxClk) at init and after resume; a Metal FMA probe here ran at ~700 MHz
//  median (360-1140) under full load against a 1900 MHz maximum.
//
//  Measured (2026-11-11, Metal FMA/SGEMM/render probes, tools/gfxcheck): the real clock does reach the
//  1900 MHz soft max under light load, but under heavy shader load the PMFW current limiter trips as soon
//  as the soft max is above a threshold and then holds the clock at ~60% of the soft max (SGEMM 1024:
//  1074 Gflop/s at soft max 1600, 806 at 1900). The threshold falls with CPU load on the shared VDD rail:
//  ~1625 MHz with an idle CPU, ~1575 with one busy thread, ~1500 with two, below 1150 with three or more,
//  where every soft max trips and the highest one (1900) is best. Trip and release follow a soft max or
//  CPU load change within 0.5 s.
//
//  Modes (file /Users/Shared/rmb-gfxclk.ctl, re-read every second, also at boot):
//    auto   default. hard min = floor; soft max chosen every 100 ms from the busy logical CPUs
//           (host_statistics, peak of the last 300 ms): < 0.6 -> cap (1600), < 1.6 -> cap1 (1600),
//           < 2.6 -> cap2 (1425), else cap3 (DPM max); 0.2 CPU hysteresis.
//           FCLK boost: GRBM_STATUS.GUI_ACTIVE sampled every 100 ms; FCLK hard min = fclkboost (1600) once 4
//           of the last 8 samples are busy, released 15 s after fewer than 2 of 8 are. Measured 2026-11-11
//           (LPDDR5-6400, two displays): FCLK stays at 1200 MHz under GPU load if left to the PMFW; GB7
//           Metal 16333 -> 19302 with FCLK held at 1600 (the PMFW's top level, PMFW reads back 1600). The
//           desktop keeps GUI_ACTIVE at 2-5 %, so FCLK is only held while the GPU really works.
//    pmfw   hard min = floor, soft max = cap; the PMFW alone picks the clock (Linux AUTO).
//    boost  as pmfw, plus a load governor: GRBM_STATUS sampled every 4 ms (16 ms when idle); when
//           the busy share of the last 16 samples reaches `up` %, hard min = cap; back to floor after
//           `hold` ms below `down` %.
//    max    hard min = soft max = cap.
//  Keys: floor=MHz cap=MHz cap1= cap2= cap3= (auto) up=% down=% hold=ms mask=GRBM_STATUS bits (default
//  GUI_ACTIVE) trace metrics socmin=MHz socmax=MHz fclkmin=MHz fclkboost=MHz (auto, 0 = off).
//  fclkmin sends SetHardMinFclkByFreq (data fabric / memory clock floor, as Linux PROFILE_PEAK) for good
//  and turns the auto boost off; removing the key sends FclkRelease (below the lowest DPM level, the PMFW
//  picks level 0). trace also logs GetFclkFrequency.
//  `trace` logs once a second: GetGfxclkFrequency min/avg/max (sampled every 100 ms) and the busy share
//  of GUI/CP/SPI/CB/TA. `metrics` (opt-in) reads the PMFW DPM table once and, with trace, the metrics
//  table every second (actual average GFX clock, ThrottlerStatus, InfrastructureGfxMaxFreq, VDD/SOC
//  power/current, temperatures) through the native table (smuGfxClkTableAnyKind, VRAM MC address only).
//  socmin/socmax (need `metrics` for the SOCCLK range) send SetHardMinSocclkByFreq/SetSoftMaxSocclkByFreq.
//  floor/cap are clamped to the PMFW DPM table (or 200..1900 MHz if it is not readable).
//
//  Only GetSmuVersion/GetEnabledSmuFeatures/GetGfxclkFrequency/SetSoftMaxGfxClk/SetHardMinGfxClk and the
//  SOCCLK pair reach the MP1 mailbox (smuGfxClkMsg), plus the table transfer for `metrics`. No GfxOff,
//  reset or power-limit message. auto only moves the soft max inside the DPM range.
//  Crash guard (/private/var/log/rmb-gfxclk.state): "pending" before the first limit is sent or after
//  a policy change, "ok" 60 s later. A boot that finds "pending"/"blocked" only measures until the ctl
//  file is written again. -rmbnogfxclk = no thread at all.
//

#include "rmb.hpp"
#include "smu_powerplay.hpp"
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <mach/host_info.h>
#include <mach/machine.h>

namespace {

constexpr const char *CtlPath = "/Users/Shared/rmb-gfxclk.ctl";
constexpr const char *StatePath = "/private/var/log/rmb-gfxclk.state";
constexpr uint32_t GrbmStatus = 0x1260 + 0x0da4;  // GC seg0 GRBM_STATUS, dword index
constexpr uint32_t MsgVersion = 0x02, MsgFeatures = 0x12, MsgGfxNow = 0x17, MsgSoftMax = 0x1b, MsgHardMin = 0x1c;
constexpr uint32_t MsgSocHardMin = 0x13, MsgSocSoftMax = 0x1d;
constexpr uint32_t MsgFclkNow = 0x18, MsgFclkHardMin = 0x23, FclkRelease = 400, FclkMaxMhz = 2400;
constexpr uint32_t GcFbBase = 0x1260 + 0x16fc, GcFbTop = 0x1260 + 0x16fd;  // GCMC_VM_FB_LOCATION_BASE/TOP, 16 MB units
// Used only when the DPM clocks table is not readable: Rembrandt MinGfxClk (Linux pp_dpm_sclk level 0)
// and AMD's published 660M maximum. The PMFW still clamps to its own fused range.
constexpr uint32_t FallbackMin = 200, FallbackMax = 1900;
constexpr uint32_t FastTickMs = 4, SlowTickMs = 16, IdleTickMs = 1000, AutoTickMs = 100;
// auto: soft max per CPU level (0, 1, 2 busy logical CPUs; level 3 = DPM max), level thresholds in
// busy CPUs * 100 (up when the 300 ms peak reaches Up[l], down when it falls below Up[l-1] - 20).
constexpr uint32_t AutoCap0 = 1600, AutoCap1 = 1600, AutoCap2 = 1425;  // cap1 1525 -> 1600: GB7 Metal 16064 -> 16869, SGEMM+1 thread 972 -> 1065
constexpr uint32_t AutoUp[3] {60, 160, 260}, AutoHyst = 20;
// auto FCLK boost: hard min while the GPU is busy (GUI_ACTIVE samples out of the last 8, one per 100 ms).
constexpr uint32_t AutoFclkBoost = 1600, FclkUpOf8 = 4, FclkBusyOf8 = 2, FclkHoldMs = 15000;  // 5 s lost the start of most GB7 workloads (gaps 0.5-10 s): 18898 vs 19302 fixed

enum Mode : uint8_t { Pmfw, Boost, Max, Auto };
const char *const ModeName[] {"pmfw", "boost", "max", "auto"};

struct Config {
	Mode mode = Auto;
	uint32_t floor = 0, cap = 0;  // MHz, 0 = DPM table min / max (auto: AutoCap0)
	uint32_t cap1 = 0, cap2 = 0, cap3 = 0;  // auto, 0 = AutoCap1 / AutoCap2 / DPM max
	uint32_t up = 40, down = 15, holdMs = 300;
	uint32_t mask = 1u << 31;  // GRBM_STATUS.GUI_ACTIVE
	uint32_t socMin = 0, socMax = 0;  // MHz, 0 = leave SOCCLK alone
	uint32_t fclkMin = 0;             // MHz, 0 = leave FCLK to the PMFW (or the auto boost)
	uint32_t fclkBoost = AutoFclkBoost;  // auto: FCLK hard min while the GPU is busy, 0 = off
	bool trace = false, metrics = false;
};

uint32_t gLo = FallbackMin, gHi = FallbackMax;
uint32_t gFloor, gCap;  // last values the PMFW accepted, 0 = not sent yet
bool gTableKnown;
uint32_t gSocLo, gSocHi, gSocMinSent, gSocMaxSent;
uint32_t gFclkMinSent, gFclkLogs, gFclkFails;

// CPU load for auto (resolved in gfxClkStart; auto falls back to `cap` without them).
using HostStatistics = kern_return_t (*)(void *, host_flavor_t, host_info_t, mach_msg_type_number_t *);
using HostSelf = void *(*)();
HostStatistics gHostStatistics;
HostSelf gHostSelf;
const int *gRealNcpus;

uint64_t nowMs() {
	uint64_t t, ns;
	clock_get_uptime(&t);
	absolutetime_to_nanoseconds(t, &ns);
	return ns / 1000000;
}

// smuGfxClkMsg returns 0 when the mailbox lock is held by another caller; retry for ≤ 5 ms.
uint32_t msg(uint32_t m, uint32_t &arg) {
	for (int i = 0; i < 20; i++) {
		uint32_t a = arg, r = smuGfxClkMsg(m, a);
		if (r) {
			arg = a;
			return r;
		}
		if (!smuGfxClkLive())
			return 0;
		IODelay(250);
	}
	return 0;
}

uint32_t clampMhz(uint32_t v, uint32_t dflt) {
	if (!v)
		v = dflt;
	return v < gLo ? gLo : v > gHi ? gHi : v;
}

bool setClk(uint32_t m, uint32_t mhz, uint32_t &cur, const char *what) {
	if (cur == mhz)
		return true;
	uint32_t a = mhz, r = msg(m, a);
	if (r != 1) {
		LOG("gfxclk: %s %u MHz -> PMFW %#x", what, mhz, r);
		cur = 0;
		return false;
	}
	cur = mhz;
	return true;
}

// Aggregate scheduler ticks of all CPUs (user+system+nice, total); false if unavailable.
bool cpuTicks(uint32_t &busy, uint32_t &total) {
	if (!gHostStatistics || !gHostSelf)
		return false;
	host_cpu_load_info_data_t li {};
	mach_msg_type_number_t n = HOST_CPU_LOAD_INFO_COUNT;
	if (gHostStatistics(gHostSelf(), HOST_CPU_LOAD_INFO, reinterpret_cast<host_info_t>(&li), &n) != KERN_SUCCESS ||
		n < HOST_CPU_LOAD_INFO_COUNT)
		return false;
	busy = li.cpu_ticks[CPU_STATE_USER] + li.cpu_ticks[CPU_STATE_SYSTEM] + li.cpu_ticks[CPU_STATE_NICE];
	total = busy + li.cpu_ticks[CPU_STATE_IDLE];
	return true;
}

// auto soft max for a CPU level; every value through clampMhz.
uint32_t autoCap(const Config &c, uint32_t level) {
	return level == 0 ? clampMhz(c.cap, AutoCap0)
		 : level == 1 ? clampMhz(c.cap1, AutoCap1)
		 : level == 2 ? clampMhz(c.cap2, AutoCap2)
					  : clampMhz(c.cap3, gHi);
}

// Next CPU level from the busy-CPU peak (x100), with hysteresis.
uint32_t autoLevel(uint32_t level, uint32_t peak) {
	while (level < 3 && peak >= AutoUp[level])
		level++;
	while (level > 0 && peak + AutoHyst < AutoUp[level - 1])
		level--;
	return level;
}

uint32_t gSets;
void apply(uint32_t floor, uint32_t cap) {
	if (floor > cap)
		floor = cap;
	uint32_t f = gFloor, c = gCap;
	if (cap < gFloor) {  // lowering the cap below the current floor: floor first
		setClk(MsgHardMin, floor, gFloor, "SetHardMinGfxClk");
		setClk(MsgSoftMax, cap, gCap, "SetSoftMaxGfxClk");
	} else {
		setClk(MsgSoftMax, cap, gCap, "SetSoftMaxGfxClk");
		setClk(MsgHardMin, floor, gFloor, "SetHardMinGfxClk");
	}
	gSets += (f != gFloor) + (c != gCap);
}

bool tokIs(const char *t, size_t n, const char *w) {
	size_t k = strlen(w);
	return n == k && !strncmp(t, w, k);
}

uint32_t number(const char *p, size_t n) {
	uint32_t v = 0;
	if (n > 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
		for (size_t i = 2; i < n; i++) {
			char c = p[i];
			int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 16;
			if (d > 15)
				break;
			v = v << 4 | static_cast<uint32_t>(d);
		}
		return v;
	}
	for (size_t i = 0; i < n && p[i] >= '0' && p[i] <= '9'; i++)
		v = v * 10 + static_cast<uint32_t>(p[i] - '0');
	return v;
}

Config parse(const char *s) {
	Config c;
	while (*s) {
		while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r' || *s == ',' || *s == ';')
			s++;
		const char *t = s;
		while (*s && *s != ' ' && *s != '\t' && *s != '\n' && *s != '\r' && *s != ',' && *s != ';')
			s++;
		size_t n = static_cast<size_t>(s - t);
		if (!n)
			continue;
		const char *eq = t;
		while (eq < s && *eq != '=')
			eq++;
		size_t kn = static_cast<size_t>(eq - t);
		uint32_t v = eq < s ? number(eq + 1, static_cast<size_t>(s - eq - 1)) : 0;
		if (tokIs(t, n, "pmfw"))
			c.mode = Pmfw;
		else if (tokIs(t, n, "boost"))
			c.mode = Boost;
		else if (tokIs(t, n, "max"))
			c.mode = Max;
		else if (tokIs(t, n, "auto"))
			c.mode = Auto;
		else if (tokIs(t, n, "trace"))
			c.trace = true;
		else if (tokIs(t, n, "metrics"))
			c.metrics = true;
		else if (eq < s && tokIs(t, kn, "socmin"))
			c.socMin = v;
		else if (eq < s && tokIs(t, kn, "socmax"))
			c.socMax = v;
		else if (eq < s && tokIs(t, kn, "fclkmin") && v <= FclkMaxMhz)
			c.fclkMin = v;
		else if (eq < s && tokIs(t, kn, "fclkboost") && v <= FclkMaxMhz)
			c.fclkBoost = v;
		else if (eq < s && tokIs(t, kn, "floor"))
			c.floor = v;
		else if (eq < s && tokIs(t, kn, "cap"))
			c.cap = v;
		else if (eq < s && tokIs(t, kn, "cap1"))
			c.cap1 = v;
		else if (eq < s && tokIs(t, kn, "cap2"))
			c.cap2 = v;
		else if (eq < s && tokIs(t, kn, "cap3"))
			c.cap3 = v;
		else if (eq < s && tokIs(t, kn, "up") && v && v <= 100)
			c.up = v;
		else if (eq < s && tokIs(t, kn, "down") && v <= 100)
			c.down = v;
		else if (eq < s && tokIs(t, kn, "hold") && v <= 10000)
			c.holdMs = v;
		else if (eq < s && tokIs(t, kn, "mask") && v)
			c.mask = v;
		else
			LOG("gfxclk: ctl token '%.*s' ignored", static_cast<int>(n < 40 ? n : 40), t);
	}
	return c;
}

// PMFW identity, enabled features, current clock and the DPM table range (if the table is usable).
void info() {
	uint32_t ver = 0, lo = 0, hi = 1, now = 0, st = 0;
	uint32_t rv = msg(MsgVersion, ver), rl = msg(MsgFeatures, lo), rh = msg(MsgFeatures, hi), rn = msg(MsgGfxNow, now);
	rem660::smu::DpmClocks d{};
	uint32_t rt = smuGfxClkTable(4, &d, sizeof(d), &st);
	bool tableOk = !rt && d.minGfx && d.maxGfx && d.minGfx < d.maxGfx && d.maxGfx <= 3000;
	if (tableOk) {
		gLo = d.minGfx;
		gHi = d.maxGfx;
	}
	LOG("gfxclk: PMFW %x (%#x), features %08x%08x (%#x/%#x; GFX_DPM %u DS_GFXCLK %u), gfx now %u MHz (%#x)", ver, rv, hi, lo,
		rl, rh, (lo >> 19) & 1, (lo >> 20) & 1, now, rn);
	LOG("gfxclk: DPM table state %#x read %#x: gfx %u..%u MHz%s; range used %u..%u MHz", st, rt, d.minGfx, d.maxGfx,
		tableOk ? "" : " (not usable, fallback)", gLo, gHi);
}

// VRAM MC aperture from the GC FB location registers; false if implausible.
bool vramRange(uint64_t &lo, uint64_t &hi) {
	uint64_t b = mmioRead(GcFbBase), t = mmioRead(GcFbTop);
	if (b == 0xffffffffu || t == 0xffffffffu)
		return false;
	b &= 0xFFFFFF;
	t &= 0xFFFFFF;
	if (!b || t < b)
		return false;
	lo = b << 24;
	hi = (t + 1) << 24;
	return true;
}

// Opt-in: the PMFW DPM clocks table, once; its GFX range replaces the fallback, its SOCCLK levels give socmin/socmax.
void readDpmTable() {
	if (gTableKnown)
		return;
	uint64_t lo = 0, hi = 0, mc = 0;
	uint32_t kind = 0;
	if (!vramRange(lo, hi)) {
		LOG("gfxclk: metrics: FB location not plausible, no table read");
		return;
	}
	rem660::smu::DpmClocks d{};
	uint32_t r = smuGfxClkTableAnyKind(4, &d, sizeof(d), lo, hi, &mc, &kind);
	LOG("gfxclk: metrics: DPM table read %#x (mc %llx kind %u, VRAM %llx..%llx): gfx %u..%u, soc n %u %u %u %u %u %u %u %u %u, "
		"df n %u fclk/mclk %u/%u %u/%u %u/%u %u/%u", r, mc, kind, lo, hi, d.minGfx, d.maxGfx, d.socCount, d.soc[0], d.soc[1],
		d.soc[2], d.soc[3], d.soc[4], d.soc[5], d.soc[6], d.soc[7], d.dfCount, d.df[0].fabric, d.df[0].memory, d.df[1].fabric,
		d.df[1].memory, d.df[2].fabric, d.df[2].memory, d.df[3].fabric, d.df[3].memory);
	if (r || !d.minGfx || d.minGfx >= d.maxGfx || d.maxGfx > 3000)
		return;
	gTableKnown = true;
	gLo = d.minGfx;
	gHi = d.maxGfx;
	if (d.socCount && d.socCount <= 8) {
		uint32_t mn = ~0u, mx = 0;
		for (uint32_t i = 0; i < d.socCount; i++) {
			mn = d.soc[i] && d.soc[i] < mn ? d.soc[i] : mn;
			mx = d.soc[i] > mx ? d.soc[i] : mx;
		}
		if (mx && mx <= 3000) {
			gSocLo = mn;
			gSocHi = mx;
		}
	}
}

// socmin/socmax only inside the table's SOCCLK range; back to that range when the key is removed.
void applySoc(const Config &c) {
	if (!gSocLo || !gSocHi || gSocLo > gSocHi) {
		if (c.socMin || c.socMax)
			LOG("gfxclk: socmin/socmax need `metrics` (SOCCLK range unknown), not sent");
		return;
	}
	auto cl = [](uint32_t v, uint32_t d) { return !v ? d : v < gSocLo ? gSocLo : v > gSocHi ? gSocHi : v; };
	uint32_t mn = c.socMin || gSocMinSent ? cl(c.socMin, gSocLo) : 0, mx = c.socMax || gSocMaxSent ? cl(c.socMax, gSocHi) : 0;
	if (mn && mx && mn > mx)
		mn = mx;
	if (mx && mx != gSocMaxSent)
		setClk(MsgSocSoftMax, mx, gSocMaxSent, "SetSoftMaxSocclkByFreq");
	if (mn && mn != gSocMinSent)
		setClk(MsgSocHardMin, mn, gSocMinSent, "SetHardMinSocclkByFreq");
}

// FCLK hard min (SetHardMinFclkByFreq); mhz 0 = release (FclkRelease, below the lowest DPM level, so the
// PMFW picks again). The soft max stays the PMFW's own. Sends only on change, logs at most 200 times.
void setFclk(uint32_t mhz, const char *why) {
	if (mhz == gFclkMinSent)
		return;
	uint32_t want = mhz ? mhz : FclkRelease, a = want, r = msg(MsgFclkHardMin, a), now = 0;
	msg(MsgFclkNow, now);
	if (gFclkLogs++ < 200)
		LOG("gfxclk: SetHardMinFclkByFreq %u MHz (%s) -> PMFW %#x, FCLK now %u MHz", want, why, r, now);
	if (r != 1)
		gFclkFails++;
	gFclkMinSent = r == 1 ? mhz : 0;
}

// fclkmin: fixed hard min; without it FCLK goes back to the PMFW (the auto boost raises it again while
// the GPU is busy).
void applyFclk(const Config &c) {
	setFclk(c.fclkMin, c.fclkMin ? "fclkmin" : "release");
}

const struct {
	uint32_t bit;
	const char *name;
} TraceBits[] {{31, "GUI"}, {29, "CP"}, {22, "SPI"}, {30, "CB"}, {14, "TA"}};

void gfxClkThread(void *, wait_result_t) {
	uint64_t t0 = nowMs();
	while (!smuGfxClkLive() && nowMs() - t0 < 180000)
		IOSleep(500);
	if (!smuGfxClkLive()) {
		LOG("gfxclk: no live SMU session after 180 s, nothing done");
		return;
	}
	IOSleep(15000);  // past WindowServer start and the first frames
	info();

	char st[112], buf[256], line[160];
	int64_t stT = 0, ctlT = 0;
	bool haveSt = rmbFileRead(StatePath, st, sizeof(st), &stT);
	bool haveCtl = rmbFileRead(CtlPath, buf, sizeof(buf), &ctlT);
	Config cfg = haveCtl ? parse(buf) : Config{};
	bool allow = true;
	if (haveSt && (!strncmp(st, "pending", 7) || !strncmp(st, "blocked", 7)) && !(haveCtl && ctlT > stT)) {
		allow = false;
		if (strncmp(st, "blocked", 7))
			rmbFileWrite(StatePath, "blocked: the last gfxclk policy never reached ok (panic, hang or power-off)\n");
		LOG("gfxclk: BLOCKED (state '%.60s'), measuring only; write %s to re-arm", st, CtlPath);
	}

	uint64_t pendingSince = 0;
	bool okWritten = true;
	auto capText = [&](char *o, size_t n) {
		if (cfg.mode == Auto)
			snprintf(o, n, "%u/%u/%u/%u", autoCap(cfg, 0), autoCap(cfg, 1), autoCap(cfg, 2), autoCap(cfg, 3));
		else
			snprintf(o, n, "%u", clampMhz(cfg.cap, gHi));
	};
	char caps[32];
	auto arm = [&](const char *why) {
		capText(caps, sizeof(caps));
		snprintf(line, sizeof(line), "pending: %s floor %u cap %s (%s)\n", ModeName[cfg.mode], clampMhz(cfg.floor, gLo), caps,
			why);
		allow = rmbFileWrite(StatePath, line);
		pendingSince = nowMs();
		okWritten = false;
		if (!allow)
			LOG("gfxclk: cannot write %s, policy not applied", StatePath);
	};
	bool boosted = false, fclkBoosted = false;
	uint32_t gpuHist = 0, fclkBoosts = 0;
	uint64_t lastGpu = 0, lastGpuBusy = 0;
	uint32_t level = 0, levelChanges = 0, cpuPeak[3] {}, cpuBusy0 = 0, cpuTotal0 = 0;
	uint64_t lastCpu = 0;
	bool haveCpu = cpuTicks(cpuBusy0, cpuTotal0);
	if (!haveCpu)
		LOG("gfxclk: no CPU load source (host_statistics), auto keeps cap %u", autoCap(cfg, 0));
	auto applyCfg = [&]() {
		boosted = false;
		fclkBoosted = false;
		gpuHist = 0;
		if (cfg.metrics)
			readDpmTable();
		applySoc(cfg);
		applyFclk(cfg);
		uint32_t cap = cfg.mode == Auto ? autoCap(cfg, level) : clampMhz(cfg.cap, gHi);
		apply(cfg.mode == Max ? cap : clampMhz(cfg.floor, gLo), cap);
		capText(caps, sizeof(caps));
		LOG("gfxclk: %s floor %u cap %s (PMFW now hard min %u soft max %u) fclk %s %u%s", ModeName[cfg.mode],
			clampMhz(cfg.floor, gLo), caps, gFloor, gCap, cfg.fclkMin ? "min" : cfg.mode == Auto ? "boost" : "pmfw",
			cfg.fclkMin ? cfg.fclkMin : cfg.mode == Auto ? cfg.fclkBoost : 0, cfg.trace ? " trace" : "");
	};
	if (allow) {
		arm(haveCtl ? "boot, ctl file" : "boot, default");
		if (allow)
			applyCfg();
	}

	uint32_t hist = 0, boosts = 0;
	uint64_t lastBusy = 0, lastCtl = nowMs(), lastClk = 0, trT0 = nowMs();
	uint32_t trN = 0, trBits[5] {}, clkN = 0, clkMin = ~0u, clkMax = 0, clkSum = 0;
	bool wasAsleep = false;
	for (;;) {
		uint64_t now = nowMs();
		if (diskLogSleeping() || !smuGfxClkLive()) {
			wasAsleep = true;
			IOSleep(200);
			continue;
		}
		if (wasAsleep) {  // the PMFW may have lost the limits across sleep / a GPU reset
			wasAsleep = false;
			hist = 0;
			gFloor = gCap = 0;
			gFclkMinSent = 0;  // applyCfg below also restarts the FCLK boost
			if (allow)
				applyCfg();
		}
		if (now - lastCtl >= 1000) {
			lastCtl = now;
			int64_t t = 0;
			bool have = rmbFileRead(CtlPath, nullptr, 0, &t);
			if (have != haveCtl || (have && t != ctlT)) {
				haveCtl = have;
				ctlT = t;
				if (have)
					rmbFileRead(CtlPath, buf, sizeof(buf), nullptr);
				cfg = have ? parse(buf) : Config{};
				hist = 0;
				arm(have ? "ctl file changed" : "ctl file removed");
				if (allow)
					applyCfg();
			}
			// nowMs(), not `now`: arm() just above may have set pendingSince after `now` (uint64 underflow -> instant ok)
			if (allow && !okWritten && nowMs() - pendingSince >= 60000) {
				capText(caps, sizeof(caps));
				snprintf(line, sizeof(line), "ok: %s floor %u cap %s\n", ModeName[cfg.mode], clampMhz(cfg.floor, gLo), caps);
				okWritten = rmbFileWrite(StatePath, line);
			}
		}

		uint32_t tick = IdleTickMs;
		if (cfg.mode == Auto && haveCpu && now - lastCpu >= AutoTickMs) {
			uint32_t b = 0, t = 0;
			if (cpuTicks(b, t)) {
				uint32_t db = b - cpuBusy0, dt = t - cpuTotal0;
				cpuBusy0 = b;
				cpuTotal0 = t;
				uint64_t el = lastCpu ? now - lastCpu : AutoTickMs;
				uint32_t ncpu = gRealNcpus && *gRealNcpus > 0 ? static_cast<uint32_t>(*gRealNcpus)
															 : static_cast<uint32_t>(uint64_t(dt) * 10 / (el ? el : 1));
				uint32_t busy = dt ? static_cast<uint32_t>(uint64_t(db) * 100 * ncpu / dt) : 0;
				cpuPeak[2] = cpuPeak[1];
				cpuPeak[1] = cpuPeak[0];
				cpuPeak[0] = busy;
				uint32_t peak = cpuPeak[0] > cpuPeak[1] ? cpuPeak[0] : cpuPeak[1];
				peak = peak > cpuPeak[2] ? peak : cpuPeak[2];
				uint32_t next = autoLevel(level, peak);
				if (next != level) {
					level = next;
					levelChanges++;
					if (allow)
						apply(clampMhz(cfg.floor, gLo), autoCap(cfg, level));
				}
			}
			lastCpu = now;
		}
		// auto FCLK boost (see the header). A fixed fclkmin wins; 3 refused sends turn it off for this boot.
		if (cfg.mode == Auto && cfg.fclkBoost && !cfg.fclkMin && allow && gFclkFails < 3 && now - lastGpu >= AutoTickMs) {
			lastGpu = now;
			uint32_t g = mmioRead(GrbmStatus);
			gpuHist = gpuHist << 1 | (g != 0xffffffffu && (g & cfg.mask) ? 1u : 0u);
			uint32_t busy8 = static_cast<uint32_t>(__builtin_popcount(gpuHist & 0xff));
			if (busy8 >= FclkBusyOf8)
				lastGpuBusy = now;
			if (!fclkBoosted && busy8 >= FclkUpOf8) {
				setFclk(cfg.fclkBoost, "auto, GPU busy");
				fclkBoosted = true;
				fclkBoosts++;
			} else if (fclkBoosted && now - lastGpuBusy >= FclkHoldMs) {
				setFclk(0, "auto, GPU idle");
				fclkBoosted = false;
			}
		}
		if (cfg.mode == Auto && (haveCpu || cfg.fclkBoost))
			tick = AutoTickMs;
		if (cfg.mode == Boost || cfg.trace) {
			uint32_t g = mmioRead(GrbmStatus);
			if (g == 0xffffffffu)
				g = 0;  // aperture not mapped
			hist = hist << 1 | ((g & cfg.mask) ? 1u : 0u);
			if (cfg.trace) {
				trN++;
				for (uint32_t i = 0; i < 5; i++)
					trBits[i] += (g >> TraceBits[i].bit) & 1;
			}
			uint32_t pct = static_cast<uint32_t>(__builtin_popcount(hist & 0xffff)) * 100 / 16;
			if (cfg.mode == Boost && allow) {
				uint32_t cap = clampMhz(cfg.cap, gHi);
				if (!boosted && pct >= cfg.up) {
					apply(cap, cap);
					boosted = true;
					boosts++;
					lastBusy = now;
				} else if (boosted) {
					if (pct >= cfg.down)
						lastBusy = now;
					else if (now - lastBusy >= cfg.holdMs) {
						apply(clampMhz(cfg.floor, gLo), cap);
						boosted = false;
					}
				}
			}
			tick = cfg.trace || boosted || (hist & 0xffff) ? FastTickMs : tick < SlowTickMs ? tick : SlowTickMs;
		}
		if (cfg.trace) {
			if (now - lastClk >= 100) {
				lastClk = now;
				uint32_t a = 0;
				if (msg(MsgGfxNow, a) == 1 && a && a < 5000) {
					clkN++;
					clkSum += a;
					clkMin = a < clkMin ? a : clkMin;
					clkMax = a > clkMax ? a : clkMax;
				}
			}
			if (now - trT0 >= 1000) {
				uint32_t n = trN ? trN : 1, fclk = 0;
				msg(MsgFclkNow, fclk);
				LOG("gfxclk: trace %s hardmin %u softmax %u | clk %u/%u/%u MHz (n %u) | busy %% %s %u %s %u %s %u %s %u %s %u "
					"(n %u) | boosts %u sets %u | cpu %u.%02u level %u changes %u | fclk %u min %u boosts %u",
					ModeName[cfg.mode], gFloor, gCap, clkN ? clkMin : 0, clkN ? clkSum / clkN : 0, clkMax, clkN,
					TraceBits[0].name, trBits[0] * 100 / n, TraceBits[1].name, trBits[1] * 100 / n, TraceBits[2].name,
					trBits[2] * 100 / n, TraceBits[3].name, trBits[3] * 100 / n, TraceBits[4].name, trBits[4] * 100 / n,
					trN, boosts, gSets, cpuPeak[0] / 100, cpuPeak[0] % 100, level, levelChanges, fclk, gFclkMinSent, fclkBoosts);
				rem660::smu::Metrics m{};
				uint64_t lo = 0, hi = 0;
				if (cfg.metrics && allow && vramRange(lo, hi) &&
					!smuGfxClkTableAnyKind(7, &m, sizeof(m), lo, hi, nullptr, nullptr))
					LOG("gfxclk: metrics gfx %u MHz (infra max %u) act %u soc %u mem %u | throttler %#x | VDD %u mV %u mA %u mW "
						"SOC %u mV %u mA %u mW | socket %u mW apu %u mW stapm %u/%u W | tdc %u/%u edc %u/%u mA | temp gfx %u soc %u "
						"skin %u | cpu max %u", m.gfx, m.gfxMaxFrequency, m.gfxActivity, m.soc, m.memory, m.throttlerStatus,
						m.voltage[0], m.current[0], m.power[0], m.voltage[1], m.current[1], m.power[1], m.socketPowerMilliwatts,
						m.apuPowerMilliwatts, m.stapmCurrentWatts, m.stapmOperatingWatts, m.vddTdc, m.socTdc, m.vddEdc, m.socEdc,
						m.gfxTemperature, m.socTemperature, m.skinTemperature, m.cpuMaxFrequency);
				trT0 = now;
				trN = clkN = clkSum = clkMax = 0;
				clkMin = ~0u;
				for (auto &b : trBits)
					b = 0;
			}
		}
		IOSleep(tick);
	}
}

}  // namespace

void gfxClkStart(KernelPatcher &patcher) {
	if (checkKernelArgument("-rmbnogfxclk")) {
		LOG("gfxclk: disabled (-rmbnogfxclk)");
		return;
	}
	gHostStatistics = reinterpret_cast<HostStatistics>(patcher.solveSymbol(KernelPatcher::KernelID, "_host_statistics"));
	patcher.clearError();
	gHostSelf = reinterpret_cast<HostSelf>(patcher.solveSymbol(KernelPatcher::KernelID, "_host_self"));
	patcher.clearError();
	gRealNcpus = reinterpret_cast<const int *>(patcher.solveSymbol(KernelPatcher::KernelID, "_real_ncpus"));
	patcher.clearError();
	thread_t t;
	if (kernel_thread_start(gfxClkThread, nullptr, &t) == KERN_SUCCESS)
		thread_deallocate(t);
	else
		LOG("gfxclk: thread start FAILED");
}
