//
//  vcn: hardware video (VCN 3.1.1 / JPEG) on the Radeon 660M. On by default; crash guard opt-in.
//
//  Out of the box VCN hangs the GPU, so display.cpp hides the video properties (novcn). Static analysis of
//  the 25G229 HWLibs/X6000 binaries against Linux (yellow_carp_ppt.c, vcn_v3_0.c, mmhub_v2_3.c,
//  mmhub_2_3_0_offset.h, yellow_carp_offset.h) finds three causes:
//   1. Firmware. HWLibs' engine_3_0_set_fw_entry_info asks AmdTtlServices::getFirmware for
//      ativvaxy_vcn3_1.dat (VCN 3.1 from IP discovery) and the firmware directory has none ("Firmware
//      ativvaxy_vcn3_1.dat not found in directory"), so the PSP never gets a VCN image. Rembrandt's is the
//      PSP-signed payload of yellow_carp_vcn.bin (fw-extract.py `vcn`), the same "$PS1" / "AMD@" layout as
//      Apple's ativvaxy_vcn3.dat. The VCN registers (vcn_3_0_0, base 0x7800/0x7E00/0x02403000) are Navi 23's;
//      Linux drives VCN 3.1.1 with vcn_v3_0 like Navi 2x.
//   2. Power. The Yellow Carp PMFW keeps VCN and JPEG power gated until PowerUpVcn 0x07 / PowerUpJpeg 0x22
//      (argument 0). Linux sends them before VCN hw init / start, PowerDownVcn 0x06 / PowerDownJpeg 0x21 after
//      stop. Apple's Phw_SwSmu_PowerGateVcn/Jpeg return OK without any message (QueryPowerGatingSupport = 0).
//   3. MMHUB. The Navi 23 path describes the MM hub with MMHUB 2.1 offsets on discovery segment 0 (HWLibs
//      _mmhub_2_1_0_initialize_registers, X6000 AMDMMHub_2_1_1::fillVMRegisters). Rembrandt's MMHUB 2.3 has
//      no VM register on segment 0 (0x13200); they all sit on segment 1 (0x1A000) at other offsets, so the
//      invalidate-ack poll of the first MM hub job never ends (SDMA0 hang -> resetHardware -> host death).
//      HWLibs' own Van Gogh/Rembrandt VM init (vm_10_3_1_hw_init) uses _mmhub_2_3_0_initialize_registers with
//      the same slot layout: it runs right after the 2.1 builder and overwrites every slot both fill (the
//      2.1-only XGMI_LFB_CNTL slot stays; only the GC hub's is read). The X6000 table gets the 2.3 map below.
//
//  Control file /Users/Shared/rmb-vcn.ctl, first word: on (default, also when missing) | probe | off.
//    probe  1-3 at boot; the video properties stay hidden, so VideoToolbox keeps its software codecs.
//    on     1-3, and the accelerator keeps its video properties (VideoToolbox uses VCN).
//  1-3 follow the ctl at boot only. The properties follow it live (re-read every 2 s) when 1-3 are active:
//  probe -> on publishes them for processes started afterwards, on -> probe/off hides them again.
//  Crash guard (opt-in since VCN proved stable, boot-arg -rmbvcnguard; without it the state file is neither
//  read nor written): /private/var/log/rmb-vcn.state: "pending" (fsynced) before the first change of a boot and
//  before VCN starts after an "ok"; "ok" once the boot is 90 s old and the last VCN start 60 s old. A boot
//  that finds "pending" or "blocked" and a ctl not newer than the state runs as off and writes "blocked";
//  write the ctl (e.g. `on`) to re-arm. -rmbnovcnfix: no routes, always off. -rmbvcn (display.cpp): keep the
//  properties whatever this file decides (the old unguarded behaviour).
//
//  PMFW messages: only PowerUp/PowerDown Vcn/Jpeg through smuVcnMsg, never GfxOff, reset or table commands.
//

#include "rmb.hpp"
#include "fw.hpp"
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <kern/clock.h>
#include <kern/thread.h>

namespace {

constexpr const char *CtlPath = "/Users/Shared/rmb-vcn.ctl";
constexpr const char *StatePath = "/private/var/log/rmb-vcn.state";
constexpr const char *FwName = "ativvaxy_vcn3_1.dat";
constexpr uint32_t MsgPowerDownVcn = 0x06, MsgPowerUpVcn = 0x07, MsgPowerDownJpeg = 0x21, MsgPowerUpJpeg = 0x22;
constexpr uint32_t MmSeg0 = 0x13200, MmSeg1 = 0x1A000;  // yellow_carp_offset.h MMHUB_BASE segments 0 and 1
constexpr uint32_t GcSeg0 = 0x1260;                      // GC segment 0 (GCMC_VM_FB_LOCATION for comparison)

enum Mode : uint8_t { Off, Probe, On };
const char *const ModeName[] {"off", "probe", "on"};

// Boot decision: 0 = not yet, 1 = requested by a hook, 2 = made.
volatile uint8_t gDecision;
volatile uint8_t gBootMode = Off;  // firmware, MMHUB 2.3 tables, PMFW power: this boot only
volatile uint8_t gLiveMode = Off;  // ctl as last read (video properties)
volatile bool gThreadUp, gStateOk, gForceOff, gGuard;
volatile uint64_t gLastStartMs;
volatile uint32_t gStarts, gFwServed, gMmTables, gMmFills, gMmFillBad, gHwInits, gGateCalls, gPowerFails;
volatile bool gVcnUp, gJpegUp;
IOLock *gPowerLock;

mach_vm_address_t orgGetFirmware, orgMm210, gMm230, orgGateVcn, orgGateJpeg, orgVcnHwInit, orgVcnStartQueue,
	orgFillVM;

uint64_t nowMs() {
	uint64_t ns;
	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / 1000000;
}

uint8_t parseMode(const char *s) {
	while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
		s++;
	auto word = [&](const char *w) {
		size_t n = strlen(w);
		return !strncmp(s, w, n) && (!s[n] || s[n] == ' ' || s[n] == '\t' || s[n] == '\n' || s[n] == '\r');
	};
	return word("on") ? On : word("probe") ? Probe : Off;
}

bool pendingOrBlocked(const char *st) { return !strncmp(st, "pending", 7) || !strncmp(st, "blocked", 7); }

// Runs in the vcn thread (file I/O never in a hook's context).
void decide() {
	char ctl[64], st[160], line[224];
	int64_t ctlT = 0, stT = 0;
	bool haveCtl = rmbFileRead(CtlPath, ctl, sizeof(ctl), &ctlT);
	bool haveSt = rmbFileRead(StatePath, st, sizeof(st), &stT);
	uint8_t mode = haveCtl ? parseMode(ctl) : On;  // hardware video on unless the ctl says otherwise
	if (__atomic_load_n(&gForceOff, __ATOMIC_ACQUIRE)) {  // a hook already ran as off: no mixed boot
		LOG("vcn: a hook timed out before the decision, off for this boot");
		mode = Off;
	}
	gLiveMode = mode;
	gGuard = checkKernelArgument("-rmbvcnguard");
	if (gGuard && mode != Off && haveSt && pendingOrBlocked(st) && !(ctlT > stT)) {
		for (char *p = st; *p; p++)
			if (*p == '\n')
				*p = 0;
		if (strncmp(st, "blocked", 7)) {
			snprintf(line, sizeof(line), "blocked: the last VCN state never reached ok (%.120s)\n", st);
			rmbFileWrite(StatePath, line);
		}
		LOG("vcn: BLOCKED (state '%.120s'), running as off; rewrite %s to re-arm", st, CtlPath);
		mode = Off;
		gLiveMode = Off;
	}
	if (gGuard && mode != Off) {
		snprintf(line, sizeof(line), "pending: %s boot (VCN firmware, MMHUB 2.3 tables, PMFW VCN power)\n", ModeName[mode]);
		if (!rmbFileWrite(StatePath, line)) {
			LOG("vcn: cannot write %s, staying off", StatePath);
			mode = Off;
			gLiveMode = Off;
		}
	}
	gBootMode = mode;
	LOG("vcn: boot mode %s (ctl %s, crash guard %s)", ModeName[mode], haveCtl ? ModeName[parseMode(ctl)] : "missing",
		gGuard ? "on (-rmbvcnguard)" : "off");
	__atomic_store_n(&gDecision, 2, __ATOMIC_RELEASE);
}

// Hooks: ask the thread for the decision and wait for it (<= 5 s; off if the thread never answers).
uint8_t bootMode() {
	if (__atomic_load_n(&gDecision, __ATOMIC_ACQUIRE) == 2)
		return gBootMode;
	uint8_t expected = 0;
	__atomic_compare_exchange_n(&gDecision, &expected, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
	for (int i = 0; i < 500 && gThreadUp && __atomic_load_n(&gDecision, __ATOMIC_ACQUIRE) != 2; i++)
		IOSleep(10);
	if (__atomic_load_n(&gDecision, __ATOMIC_ACQUIRE) != 2) {
		__atomic_store_n(&gForceOff, true, __ATOMIC_RELEASE);
		LOG("vcn: no boot decision (thread %s), off", gThreadUp ? "slow" : "missing");
		return Off;
	}
	return gBootMode;
}

// --- PMFW power ------------------------------------------------------------------------------------------

uint32_t pmfw(uint32_t msg) {
	for (int i = 0; i < 200; i++) {  // the gfxclk thread may hold the mailbox for a few ms
		if (uint32_t r = smuVcnMsg(msg))
			return r;
		if (!smuGfxClkLive())
			return 0;
		IOSleep(1);
	}
	return 0;
}

bool setPower(bool vcn, bool up, const char *why) {
	if (!gPowerLock)
		return false;
	IOLockLock(gPowerLock);
	volatile bool &state = vcn ? gVcnUp : gJpegUp;
	bool ok = true;
	if (state != up) {
		if (up) {  // HWLibs may reach VCN hw init before the SMU session is live (SMU hw init ~0.3 s later)
			uint64_t t0 = nowMs();
			while (!smuGfxClkLive() && nowMs() - t0 < 3000)
				IOSleep(20);
		}
		uint32_t msg = vcn ? (up ? MsgPowerUpVcn : MsgPowerDownVcn) : (up ? MsgPowerUpJpeg : MsgPowerDownJpeg);
		uint32_t r = pmfw(msg);
		ok = r == 1;
		if (ok)
			state = up;
		else
			gPowerFails++;
		LOG("vcn: %s %s (%s): PMFW msg %#x -> %#x%s", vcn ? "VCN" : "JPEG", up ? "power up" : "power down", why, msg, r,
			r ? "" : " (not sent: no live SMU session or mailbox busy)");
	}
	IOLockUnlock(gPowerLock);
	return ok;
}

// A VCN start: "pending" must be on disk before it runs when the state says ok (wait <= 1 s for the thread).
void noteStart() {
	__atomic_add_fetch(&gStarts, 1, __ATOMIC_ACQ_REL);
	__atomic_store_n(&gLastStartMs, nowMs(), __ATOMIC_RELEASE);
	for (int i = 0; i < 100 && gThreadUp && __atomic_load_n(&gStateOk, __ATOMIC_ACQUIRE); i++)
		IOSleep(10);
}

// --- HWLibs hooks ----------------------------------------------------------------------------------------

// AmdTtlServices::getFirmware(name, _TtlFwInfo {u8 *buffer; u32 capacity; u32 size}) -> bool.
uint8_t wrapGetFirmware(void *ttl, const char *name, void *info) {
	if (name && info && !strcmp(name, FwName) && bootMode() != Off) {
		auto i = static_cast<uint8_t *>(info);
		auto buf = *reinterpret_cast<uint8_t **>(i);
		uint32_t cap = *reinterpret_cast<uint32_t *>(i + 8), n = FW_SIZE(vcn);
		if (buf && cap >= n) {
			memcpy(buf, fw_yc_vcn, n);
			*reinterpret_cast<uint32_t *>(i + 0xc) = n;
			gFwServed++;
			LOG("vcn: served yellow_carp VCN firmware as %s (%#x bytes, version %08x)", FwName, n,
				*reinterpret_cast<const uint32_t *>(fw_yc_vcn + 0x60));
			return 1;
		}
		LOG("vcn: %s buffer %p capacity %#x < %#x, not served", FwName, buf, cap, n);
	}
	return reinterpret_cast<uint8_t (*)(void *, const char *, void *)>(orgGetFirmware)(ttl, name, info);
}

// _mmhub_2_1_0_initialize_registers(vm): register table at *(vm + 0x510); slot 0x380 = MM CONTEXT0_CNTL.
uint64_t wrapMm210(uint64_t vm, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
	auto r = reinterpret_cast<Fn6>(orgMm210)(vm, b, c, d, e, f);
	if (vm && gMm230 && bootMode() != Off) {
		auto t = *reinterpret_cast<uint8_t **>(vm + 0x510);
		uint32_t before = t ? *reinterpret_cast<uint32_t *>(t + 0x380) : 0;
		reinterpret_cast<Fn6>(gMm230)(vm, b, c, d, e, f);
		uint32_t after = t ? *reinterpret_cast<uint32_t *>(t + 0x380) : 0;
		gMmTables++;
		LOG("vcn: HWLibs MM hub table -> MMHUB 2.3 (CONTEXT0_CNTL %#x -> %#x, expected %#x)", before, after,
			MmSeg1 + 0x740);
	}
	return r;
}

// Phw_SwSmu_PowerGateVcn(hwmgr, gate, instance) / PowerGateJpeg(hwmgr, gate) -> PP result (1 = OK).
// HWLibs PSM_ReArbit ungates/gates VCN instance 0 by its active flag and, when PHM_IsVcn1Supported (Navi 2x
// table), instance 1 by its own flag: right before the first VCN start it sends ungate(0) then gate(1).
// Rembrandt has one VCN instance (Linux yellow_carp_dpm_set_vcn_enable takes none), so instance 1 is a
// no-op here; acting on it powered the only VCN down. PowerGateJpeg has no instance argument.
uint64_t gate(bool vcn, uint64_t hwmgr, uint64_t gateIt, uint64_t inst, uint64_t d, uint64_t e, uint64_t f) {
	if (bootMode() == Off)
		return reinterpret_cast<Fn6>(vcn ? orgGateVcn : orgGateJpeg)(hwmgr, gateIt, inst, d, e, f);
	uint32_t n = __atomic_add_fetch(&gGateCalls, 1, __ATOMIC_ACQ_REL);
	bool up = !(gateIt & 0xFFFFFFFF);
	if (vcn && (inst & 0xFFFFFFFF)) {
		if (n <= 32)
			LOG("vcn: PowerPlay %s VCN instance %u: Rembrandt has one instance, ignored", up ? "ungate" : "gate",
				static_cast<uint32_t>(inst));
		return 1;
	}
	return setPower(vcn, up, vcn ? "PowerPlay, instance 0" : "PowerPlay") ? 1 : 2;
}
uint64_t wrapGateVcn(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
	return gate(true, a, b, c, d, e, f);
}
uint64_t wrapGateJpeg(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
	return gate(false, a, b, c, d, e, f);
}

// Linux (APU): smu_hw_init powers VCN and JPEG up before any VCN/JPEG register access. Here: before
// HWLibs' VCN hw init (Apple's later PowerGateVcn/Jpeg calls then gate/ungate them).
uint64_t wrapVcnHwInit(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
	bool on = bootMode() != Off;
	if (on) {
		setPower(true, true, "VCN hw init");
		setPower(false, true, "VCN hw init");
		diskLogSync(300);
	}
	auto r = reinterpret_cast<Fn6>(orgVcnHwInit)(a, b, c, d, e, f);
	gHwInits++;
	LOG("vcn: vcn_hw_init -> %#llx (mode %s, VCN %s)", r & 0xFFFFFFFF, ModeName[on ? gBootMode : Off],
		gVcnUp ? "powered" : "gated");
	return r;
}

// vcn_start_queue(vcn, queue) -> 0 = OK. Linux vcn_v3_0_start begins with PowerUpVcn.
uint64_t wrapVcnStartQueue(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
	static uint32_t logged;
	if (bootMode() != Off) {
		if (!gVcnUp)
			setPower(true, true, "VCN start");
		if (!gJpegUp)
			setPower(false, true, "VCN start");
		noteStart();
		if (logged < 8)
			diskLogSync(300);  // the first starts are the risky ones: their log lines on disk first
	}
	auto r = reinterpret_cast<Fn6>(orgVcnStartQueue)(a, b, c, d, e, f);
	if (logged < 32) {
		logged++;
		LOG("vcn: vcn_start_queue #%u -> %#llx (VCN %s)", gStarts, r & 0xFFFFFFFF, gVcnUp ? "powered" : "gated");
	}
	return r;
}

// --- X6000 hook --------------------------------------------------------------------------------------------

// AMD_GFX10_VM_REGS_PER_HUB (0x358 bytes, dword register indices):
//   +0x00 MX_L1_TLB_CNTL, L2_CNTL, L2_CNTL2, L2_CNTL3, L2_CNTL4, PROTECTION_FAULT_CNTL, _CNTL2, _MM_CNTL3,
//         _MM_CNTL4, _STATUS, _ADDR_LO32, _ADDR_HI32
//   +0x30 + 0x1c * ctx (16): PAGE_TABLE_BASE_LO32/HI32, START_LO32/HI32, END_LO32/HI32, CONTEXT_CNTL
//   +0x1f0 + 0x14 * eng (18): INVALIDATE_ENG ADDR_RANGE_LO32/HI32, REQ, ACK, SEM
// MMHUB 2.3 (mmhub_2_3_0_offset.h, all BASE_IDX 1). Field layouts of the registers Apple programs
// (INVALIDATE_ENG REQ/ACK/SEM, MX_L1_TLB_CNTL, PROTECTION_FAULT_CNTL) are the same as 2.1; CONTEXT_CNTL
// only adds the SECURE_PROTECTION_FAULT bits 23/24.
constexpr uint16_t Mm23Head[12] {0x8f3, 0x700, 0x701, 0x702, 0x718, 0x708, 0x709, 0x70a, 0x70b, 0x70c, 0x70d, 0x70e};

void fillMm23(uint32_t *v) {
	for (uint32_t i = 0; i < 12; i++)
		v[i] = MmSeg1 + Mm23Head[i];
	for (uint32_t i = 0; i < 16; i++) {
		auto c = v + (0x30 + 0x1c * i) / 4;
		for (uint32_t k = 0; k < 6; k++)
			c[k] = MmSeg1 + 0x940 + 8 * i + k;  // BASE_LO, BASE_HI, START_LO, START_HI, END_LO, END_HI
		c[6] = MmSeg1 + 0x740 + i;
	}
	for (uint32_t i = 0; i < 18; i++) {
		auto g = v + (0x1f0 + 0x14 * i) / 4;
		g[0] = MmSeg1 + 0xa03 + 8 * i;  // ADDR_RANGE_LO32
		g[1] = MmSeg1 + 0xa04 + 8 * i;  // ADDR_RANGE_HI32
		g[2] = MmSeg1 + 0xa01 + 8 * i;  // REQ
		g[3] = MmSeg1 + 0xa02 + 8 * i;  // ACK
		g[4] = MmSeg1 + 0xa00 + 8 * i;  // SEM
	}
}

// AMDMMHub_2_1_1::fillVMRegisters(regs) -> bool (al); base = discovery MMHUB segment 0.
uint64_t wrapFillVM(uint64_t hub, uint64_t regs, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
	auto r = reinterpret_cast<Fn6>(orgFillVM)(hub, regs, c, d, e, f);
	if ((r & 0xFF) && regs && bootMode() != Off) {
		auto v = reinterpret_cast<uint32_t *>(regs);
		if (v[0] == MmSeg0 + 0x873 && v[4 + 0] == MmSeg0 + 0x698) {
			fillMm23(v);
			gMmFills++;
			LOG("vcn: X6000 MM hub registers -> MMHUB 2.3 (L1_TLB %#x, CONTEXT1 BASE_LO %#x, ENG0 REQ/ACK %#x/%#x)",
				v[0], v[(0x30 + 0x1c) / 4], v[0x1f8 / 4], v[0x1fc / 4]);
		} else {
			gMmFillBad++;
			LOG("vcn: X6000 MM hub base %#x is not Rembrandt's segment 0, left alone", v[0] - 0x873);
		}
	}
	return r;
}

// --- thread -----------------------------------------------------------------------------------------------

void mmhubDump23(const char *why) {
	auto mm = [](uint32_t off) { return mmioRead(MmSeg1 + off); };
	LOG("vcn: MMHUB 2.3 %s: FB %x-%x (GC %x-%x) off %x AGP %x/%x/%x SYS %x-%x L1_TLB %x L2 %x/%x/%x FAULT %x/%x",
		why, mm(0x8ec), mm(0x8ed), mmioRead(GcSeg0 + 0x16fc), mmioRead(GcSeg0 + 0x16fd), mm(0x8d7), mm(0x8ee),
		mm(0x8ef), mm(0x8f0), mm(0x8f1), mm(0x8f2), mm(0x8f3), mm(0x700), mm(0x701), mm(0x702), mm(0x708), mm(0x70c));
	for (uint32_t v = 0; v < 16; v++) {
		uint32_t cntl = mm(0x740 + v);
		if (cntl & 1)
			LOG("vcn:   MM ctx %u CNTL %x base %x%08x start %x%08x end %x%08x", v, cntl, mm(0x941 + 8 * v),
				mm(0x940 + 8 * v), mm(0x943 + 8 * v), mm(0x942 + 8 * v), mm(0x945 + 8 * v), mm(0x944 + 8 * v));
	}
}

void vcnThread(void *, wait_result_t) {
	gThreadUp = true;
	// Decide when a hook asks (HWLibs/novcn reach this ~28 s into the boot, long after the Data volume is
	// mounted) or, with no hook at all, at 240 s.
	while (__atomic_load_n(&gDecision, __ATOMIC_ACQUIRE) == 0 && nowMs() < 240000)
		IOSleep(20);
	decide();
	char buf[64], line[224];
	int64_t ctlT = 0;
	rmbFileRead(CtlPath, nullptr, 0, &ctlT);
	bool dumped = false;
	uint32_t startsSeen = 0;
	uint64_t lastCtl = nowMs();
	for (;;) {
		IOSleep(20);
		uint64_t now = nowMs();
		if (gBootMode != Off && gGuard) {
			uint32_t starts = __atomic_load_n(&gStarts, __ATOMIC_ACQUIRE);
			if (starts != startsSeen && __atomic_load_n(&gStateOk, __ATOMIC_ACQUIRE)) {
				snprintf(line, sizeof(line), "pending: %s, VCN started (%u starts)\n", ModeName[gBootMode], starts);
				rmbFileWrite(StatePath, line);
				__atomic_store_n(&gStateOk, false, __ATOMIC_RELEASE);
			}
			startsSeen = starts;
			uint64_t last = __atomic_load_n(&gLastStartMs, __ATOMIC_ACQUIRE);
			if (!__atomic_load_n(&gStateOk, __ATOMIC_ACQUIRE) && now >= 90000 && (!last || now - last >= 60000)) {
				snprintf(line, sizeof(line),
					"ok: %s, firmware served %u, MM tables %u/%u (bad %u), vcn_hw_init %u, VCN starts %u, power %s/%s, "
					"gate calls %u, power failures %u\n",
					ModeName[gBootMode], gFwServed, gMmTables, gMmFills, gMmFillBad, gHwInits, starts,
					gVcnUp ? "up" : "gated", gJpegUp ? "up" : "gated", gGateCalls, gPowerFails);
				if (rmbFileWrite(StatePath, line)) {
					__atomic_store_n(&gStateOk, true, __ATOMIC_RELEASE);
					LOG("vcn: %s", line);
				}
			}
		}
		if (gBootMode != Off && !dumped && now >= 75000) {
			dumped = true;
			mmhubDump23("75 s");
		}
		if (now - lastCtl < 2000)
			continue;
		lastCtl = now;
		int64_t t = 0;
		bool have = rmbFileRead(CtlPath, nullptr, 0, &t);
		if (have ? t == ctlT : !ctlT)
			continue;
		ctlT = have ? t : 0;
		uint8_t mode = have && rmbFileRead(CtlPath, buf, sizeof(buf), nullptr) ? parseMode(buf) : On;
		uint8_t was = gLiveMode;
		gLiveMode = mode;
		if (gBootMode == Off) {
			LOG("vcn: ctl now %s; firmware/MMHUB/power were off at boot, takes effect at the next boot", ModeName[mode]);
			continue;
		}
		if ((mode == On) != (was == On)) {
			LOG("vcn: ctl %s -> %s, %s the video properties", ModeName[was], ModeName[mode], mode == On ? "publishing" : "hiding");
			vcnVideoProps(mode == On);
		}
	}
}

}  // namespace

bool vcnShowVideo() { return bootMode() != Off && gLiveMode == On; }

void vcnStart() {
	if (checkKernelArgument("-rmbnovcnfix")) {
		LOG("vcn: -rmbnovcnfix, off");
		return;
	}
	gPowerLock = IOLockAlloc();
	thread_t t;
	if (gPowerLock && kernel_thread_start(vcnThread, nullptr, &t) == KERN_SUCCESS)
		thread_deallocate(t);
	else
		LOG("vcn: no thread, off");
}

void vcnRouteHwLibs(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
	if (checkKernelArgument("-rmbnovcnfix"))
		return;
	gMm230 = findSymbol(patcher, index, "_mmhub_2_3_0_initialize_registers", address, size);
	KernelPatcher::RouteRequest reqs[] {
		{"__ZN14AmdTtlServices11getFirmwareEPKcP10_TtlFwInfo", wrapGetFirmware, orgGetFirmware},
		{"_mmhub_2_1_0_initialize_registers", wrapMm210, orgMm210},
		{"_Phw_SwSmu_PowerGateVcn", wrapGateVcn, orgGateVcn},
		{"_Phw_SwSmu_PowerGateJpeg", wrapGateJpeg, orgGateJpeg},
		{"_vcn_hw_init", wrapVcnHwInit, orgVcnHwInit},
		{"_vcn_start_queue", wrapVcnStartQueue, orgVcnStartQueue},
	};
	routeStaticRequests(patcher, index, reqs, arrsize(reqs), address, size);
	patcher.clearError();
	LOG("vcn: HWLibs routes getFirmware %d mmhub %d/%d gate %d/%d hw_init %d start_queue %d", orgGetFirmware != 0,
		orgMm210 != 0, gMm230 != 0, orgGateVcn != 0, orgGateJpeg != 0, orgVcnHwInit != 0, orgVcnStartQueue != 0);
}

void vcnRouteAccel(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
	if (checkKernelArgument("-rmbnovcnfix"))
		return;
	KernelPatcher::RouteRequest req {"__ZN29AMDRadeonX6000_AMDMMHub_2_1_115fillVMRegistersEP25AMD_GFX10_VM_REGS_PER_HUB",
		wrapFillVM, orgFillVM};
	routeStaticRequests(patcher, index, &req, 1, address, size);
	patcher.clearError();
	LOG("vcn: X6000 route AMDMMHub_2_1_1::fillVMRegisters %d", orgFillVM != 0);
}
