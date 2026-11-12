//
//  DCN 3.1.2 under DAL's DCN 3.0.2 code: DAL reaches registers only through AmdDalHelper's
//  dalAtomRead/WriteRegister32 callbacks. Remap every 3.0.2 address whose register moved in 3.1.2
//  (dcnremap.hpp, generated from the Linux headers by scripts/dcn-remap.py).
//

#include "rmb.hpp"
#include "dcnremap.hpp"
#include "phyvalid.hpp"
#include <string.h>
#include <stdarg.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <kern/thread.h>
#include <libkern/OSAtomic.h>
#include <pexpert/pexpert.h>

namespace {

const DcnRemap *find(uint32_t reg) {
	size_t lo = 0, hi = sizeof(kDcnRemap) / sizeof(kDcnRemap[0]);
	while (lo < hi) {
		size_t mid = (lo + hi) / 2;
		if (kDcnRemap[mid].from == reg)
			return &kDcnRemap[mid];
		if (kDcnRemap[mid].from < reg)
			lo = mid + 1;
		else
			hi = mid;
	}
	return nullptr;
}

// Backlight PWM: DCN 3.0.2 BL_PWM_CNTL/CNTL2/PERIOD_CNTL/GRP1_REG_LOCK (seg2 0x2888-0x288b) are holes on
// 3.1.2, where they live in PWRSEQ0 (seg2 0x2f19-0x2f1c). Not in dcnremap.hpp: the names differ, so
// scripts/dcn-remap.py can't pair them. LVTMA_PWRSEQ_CNTL/STATE stay unmapped on purpose (eDP power
// sequencing works as is). -rmbabm: no remap.
bool gKeepAbm;

uint32_t remap(uint32_t reg, bool write) {
	if (!gKeepAbm && reg >= 0x34C0 + 0x2888 && reg <= 0x34C0 + 0x288b) {
		static uint32_t loggedBl;
		if (loggedBl++ < 20)
			LOG("bl: DAL %s BL_PWM reg %x -> %x", write ? "write" : "read", reg, reg + 0x2f19 - 0x2888);
		return reg + 0x2f19 - 0x2888;
	}
	auto m = find(reg);
	if (!m)
		return reg;
	static uint32_t logged;
	if (logged < 60) {
		logged++;
		LOG("dcn: %s %s %x -> %x", write ? "write" : "read", m->name, reg, m->to);
	}
	return m->to;
}

// Registers at the same address whose fields moved (scripts/dcn-fielddiff.py). ODMn
// OPTC_DATA_SOURCE_SELECT: 3.0.2 NUM_OF_OUTPUT_SEGMENT 3:2, SEG0-3_SRC_SEL 23:8; 3.1.2 9:8, 31:16 (left
// as reset 0xf, the OTG gets no OPP and scans out black). HUBPRETn_CONTROL: PACK_3TO2_ELEMENT_DISABLE
// bit 12 -> 15; DET_BUF_PLANE1_BASE_ADDRESS has other units on 3.1.2 (Linux never sets it), kept as is.
enum class Fields { None, OdmSrc, HubpRet };

Fields fieldKind(uint32_t hw) {
	if (hw >= 0x4f8b && hw <= 0x4fbb && (hw & 0xf) == 0xb)
		return Fields::OdmSrc;
	if (hw >= 0x3b2c && hw <= 0x3b2c + 3 * 0xdc && (hw - 0x3b2c) % 0xdc == 0)
		return Fields::HubpRet;
	return Fields::None;
}

uint32_t fieldsToDal(Fields k, uint32_t h) {
	if (k == Fields::OdmSrc)
		return (h & 3) | ((h >> 8 & 3) << 2) | ((h >> 16) << 8);
	return (h & 0xffff0000) | ((h >> 15 & 1) << 12);
}

uint32_t fieldsToHw(Fields k, uint32_t hw, uint32_t v, uint32_t old) {
	uint32_t h = k == Fields::OdmSrc ? (v & 3) | ((v >> 2 & 3) << 8) | ((v >> 8 & 0xffff) << 16)
									 : (v & 0xffff0000) | (old & 0x1ff0) | ((v >> 12 & 1) << 15);
	static uint32_t logged;
	if (logged++ < 40)
		LOG("dcn: field remap write %x: DAL %x -> hw %x", hw, v, h);
	return h;
}

// Power gating. DCN 3.0.2: HUBP pipe n = DOMAIN(2n), DPP pipe n = DOMAIN(2n+1) (0..9), DSC =
// DOMAIN16..20; DOMAINx_PG_STATUS follows its PG_CONFIG. DCN 3.1.2 has one domain per pipe
// (DOMAIN0..3) and DSC DOMAIN16..18 at other offsets, so DAL's gating lands on the wrong domains
// and its status waits time out. DAL gets a shadow PG block (status mirrors the requested state)
// and the real domains are held on with DOMAIN_POWER_FORCEON.
constexpr uint32_t Seg2 = 0x34C0, PgFirst = Seg2 + 0x80, PgLast = Seg2 + 0xaa;
uint32_t gPgShadow[PgLast - PgFirst + 1];
bool gPgForced;

bool isPgStatus(uint32_t off) {  // seg2 offset, DCN 3.0.2 numbering
	return (off >= 0x81 && off <= 0x93 && (off & 1)) || (off >= 0xa2 && off <= 0xaa && !(off & 1));
}

void forceDomainsOn() {
	if (gPgForced)
		return;
	gPgForced = true;
	static const uint32_t domains[] {0x80, 0x82, 0x84, 0x86, 0x89, 0x8b, 0x8d};  // DCN 3.1.2 DOMAIN0-3, 16-18 PG_CONFIG
	for (uint32_t d : domains) {
		uint32_t v = mmioRead(Seg2 + d);
		mmioWrite(Seg2 + d, (v | 1u) & ~(1u << 8));  // FORCEON, not POWER_GATE
		LOG("dcn: PG domain cfg %x: %x -> %x, status %x", Seg2 + d, v, mmioRead(Seg2 + d), mmioRead(Seg2 + d + 1));
	}
}

constexpr uint32_t CcDcPipeDis = Seg2 + 0xca;

constexpr uint32_t TraceLen = 256;
struct TraceEnt {
	uint64_t t;
	uint32_t reg, hw, val;
	bool rd;
};
TraceEnt gTrace[TraceLen];
uint32_t gTraceN;

uint64_t nowUs() {
	uint64_t ns;
	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / 1000;
}

void traceAccess(bool rd, uint32_t reg, uint32_t hw, uint32_t val) {
	auto &e = gTrace[gTraceN++ % TraceLen];
	e = {nowUs(), reg, hw, val, rd};
}

// USB-C (DP-alt) DAL side of link training (the DMUB TX cmds, logTxDiag below, already match Linux):
// raw bytes of DAL's native AUX transactions on AUX1-3 (request from AUX_SW_DATA writes, reply from
// AUX_SW_DATA reads, logged at the next AUX_SW_GO; I2C-over-AUX/EDID skipped) and every DAL write to
// DIG2/DP2, to diff against Linux logs/linux-usbc-{dpcd,replug}.txt. Part of -rmbnousbclog.
bool gUsbcLog = true;
constexpr uint32_t AuxTrFirst = Seg2 + 0x1f6c, AuxTrLast = Seg2 + 0x1fbf, AuxTrStride = 0x1c,
				   Dig2Dp2First = Seg2 + 0x2280, Dig2Dp2Last = Seg2 + 0x237f;
struct AuxTr {
	uint8_t cur[20], req[20], rep[20];
	uint32_t pos, nCur, nReq, nRep;
	uint64_t goUs;
	bool live;
};
AuxTr gAuxTr[3];
uint32_t gAuxTrLines, gDigTrLines;

void hexBytes(char *out, size_t size, const uint8_t *b, uint32_t n) {
	size_t o = 0;
	out[0] = 0;
	for (uint32_t i = 0; i < n && o + 4 < size; i++)
		o += snprintf(out + o, size - o, " %02x", b[i]);
}

void auxTrFlush(uint32_t ch) {
	auto &t = gAuxTr[ch];
	if (!t.live)
		return;
	t.live = false;
	if (!t.nReq || ((t.req[0] >> 4) < 8 && (t.nReq < 2 || (t.req[1] >> 4) < 8)) || gAuxTrLines >= 1500)
		return;
	gAuxTrLines++;
	char q[64], p[64];
	hexBytes(q, sizeof(q), t.req, t.nReq);
	hexBytes(p, sizeof(p), t.rep, t.nRep);
	LOG("usbc: aux%u @%llu us req%s -> rep%s", ch + 1, t.goUs, q, p);
}

void retimerGo(uint32_t ch, const uint8_t *req, uint32_t n);
void retimerPattern(uint32_t ch, uint32_t v);
void usbcTrainingEnd(uint32_t ch);

// The AUX request capture also feeds retimerGo (fixed VS/PE retimer, below), so it runs without the log.
void usbcTraceWrite(uint32_t hw, uint32_t v) {
	if ((hw == Seg2 + 0x2318 || hw == Seg2 + 0x2418) && v)	// DP2/DP3_DP_DPHY_TRAINING_PATTERN_SEL: TPS2+
		retimerPattern((hw - Seg2 - 0x2118) / 0x100 - 1, v);
	if ((hw == Seg2 + 0x2308 || hw == Seg2 + 0x2408) && (v & 0x10))  // DP2/DP3_DP_LINK_CNTL TRAINING_COMPLETE
		usbcTrainingEnd((hw - Seg2 - 0x2108) / 0x100 - 1);
	if (gUsbcLog && hw >= Dig2Dp2First && hw <= Dig2Dp2Last && gDigTrLines < 600) {
		gDigTrLines++;
		LOG("usbc: dig2 wr %x = %x", hw - Seg2, v);
		return;
	}
	if (hw < AuxTrFirst || hw > AuxTrLast)
		return;
	uint32_t ch = (hw - AuxTrFirst) / AuxTrStride, off = (hw - AuxTrFirst) % AuxTrStride;
	auto &t = gAuxTr[ch];
	if (off == 6 && !(v & 1)) {	 // AUX_SW_DATA, write mode
		if (v & 0x80000000u)  // AUX_SW_AUTOINCREMENT_DISABLE: index given
			t.pos = (v >> 16) & 0x1f;
		if (t.pos < sizeof(t.cur)) {
			t.cur[t.pos++] = v >> 8;
			t.nCur = t.pos > t.nCur ? t.pos : t.nCur;
		}
		if (!t.pos || (v & 0x80000000u && !((v >> 16) & 0x1f)))
			t.nCur = t.pos;
	} else if (off == 1 && (v & 1)) {  // AUX_SW_GO
		retimerGo(ch, t.cur, t.nCur);
		auxTrFlush(ch);
		memcpy(t.req, t.cur, sizeof(t.req));
		t.nReq = t.nCur;
		t.nRep = 0;
		t.nCur = t.pos = 0;
		t.goUs = nowUs();
		t.live = gUsbcLog;
	}
}

// USB-C 4-lane links train (CR+EQ+align at HBR2 x4) but lose EQ on lanes 0/3 as soon as video starts:
// sink IRQ -> AGDC retrains -> loop every ~150 ms, both screens blank (usbc run 10-27 16:30). Linux on the
// same port never gets EQ on lane 3 at x4 and settles at HBR2 x2. Report MAX_LANE_COUNT (DPCD 0x002 and
// the extended 0x2202) as 2 on the USB-C AUX channels so DAL trains x2 too. -rmbusbc4lane: off.
bool gUsbc4Lane;

// HBR2 x2 trains (CR + EQ + interlane align) but lane 0 drops lock ~1 ms after TPS off, retrain loop =
// flashy picture with one ~100 s stable window (usbc run 10-27 17:15). Test for signal integrity: also
// report MAX_LINK_RATE (DPCD 0x001 / 0x2201) as HBR (0x0a) on the USB-C AUX channels. Result: same lane 0
// loss at HBR x2 (usbc run 10-27 17:30) = not signal integrity, so now opt-in: -rmbusbchbr.
bool gUsbcHbr;

// At HBR2 x1 the USB-C link trains (202 = 07, 204 = 81) and still drops within ~1 ms of DP_VID_STREAM_ENABLE
// (sink IRQ, 0x200C = 01: CR only), every retrain (usbc run 10-27 16:40). The DP2 stream writes differ from
// Linux (logs/linux-usbc-replug.txt) only in DP_SEC_CNTL GSP3_ENABLE (bit 23: Navi DAL's HDR metadata SDP,
// Linux never sends it) and AFMT_MEM_PWR = 0x100 (Linux 0x101/0x1: MEM_PWR_DIS, audio SRAM kept powered).
// Do it like Linux on DIG2/DIG3 (the USB-C ones). -rmbusbcsdp: off.
bool gUsbcSdp;
uint64_t usbcSdpFix(uint32_t hw, uint64_t val) {
	if (gUsbcSdp)
		return val;
	uint64_t v = val;
	if (hw == Seg2 + 0x232b || hw == Seg2 + 0x242b)  // DP2/DP3_DP_SEC_CNTL
		v &= ~0x00800000ull;
	else if (hw == Seg2 + 0x2287 || hw == Seg2 + 0x2387)  // AFMT2/AFMT3_AFMT_MEM_PWR
		v |= 1;
	static uint32_t logged;
	if (v != val && logged++ < 20)
		LOG("usbc: %s %x: %llx -> %llx (like Linux)", (hw & 0xff) == 0x2b ? "DP_SEC_CNTL GSP3 off" : "AFMT_MEM_PWR_DIS",
			hw - Seg2, val & 0xFFFFFFFF, v & 0xFFFFFFFF);
	return v;
}

// DAL's AUX_SW_DATA reads after a native read: [0] stale, [1] reply cmd, [2 + i] = DPCD addr + i.
uint32_t usbcLaneCap(uint32_t ch, const AuxTr &t, uint32_t i, uint32_t v) {
	if (ch == 0 || t.nReq < 4 || (t.req[0] >> 4) != 9 || i < 2)
		return v;
	uint32_t addr = (t.req[0] & 0xFu) << 16 | t.req[1] << 8 | t.req[2], len = t.req[3] + 1u;
	static const uint32_t rates[] {0x00001, 0x02201};
	for (uint32_t cap : rates) {
		if (!gUsbcHbr || cap < addr || cap - addr >= len || i - 2 != cap - addr)
			continue;
		uint32_t b = (v >> 8) & 0xFF;
		if (b <= 0x0a)
			return v;
		static uint32_t logged;
		if (logged++ < 20)
			LOG("usbc: aux%u DPCD %x MAX_LINK_RATE %x -> a (HBR cap)", ch + 1, cap, b);
		return (v & ~0xFF00u) | 0x0a << 8;
	}
	static const uint32_t caps[] {0x00002, 0x02202};
	for (uint32_t cap : caps) {
		if (gUsbc4Lane || cap < addr || cap - addr >= len || i - 2 != cap - addr)
			continue;
		uint32_t b = (v >> 8) & 0xFF;
		if ((b & 0x1F) <= 2)
			return v;
		static uint32_t logged;
		if (logged++ < 20)
			LOG("usbc: aux%u DPCD %x MAX_LANE_COUNT %x -> %x (x2 like Linux)", ch + 1, cap, b, (b & 0xE0) | 2);
		return (v & ~0xFF00u) | ((b & 0xE0) | 2) << 8;
	}
	return v;
}

uint32_t usbcTraceRead(uint32_t hw, uint32_t v) {
	if (hw < AuxTrFirst || hw > AuxTrLast || (hw - AuxTrFirst) % AuxTrStride != 6)
		return v;
	uint32_t ch = (hw - AuxTrFirst) / AuxTrStride;
	auto &t = gAuxTr[ch];
	v = usbcLaneCap(ch, t, t.nRep, v);
	if (t.nRep < sizeof(t.rep))
		t.rep[t.nRep++] = v >> 8;
	return v;
}

// RDPCSTX0 liveness around every DAL register access: shows whether PHY A dies on a DAL access
// or between two of them (another agent: DMUB firmware, PMFW).
bool gPhyAlive;
uint64_t gLastAccessUs;
uint32_t gLastReg;
bool gLastRd;

void phyEnter(bool rd, uint32_t r) {
	if (!phyPollOn())
		return;
	bool alive = mmioRead(PhyA) != 0;
	static uint32_t logged;
	if (gPhyAlive && !alive && logged++ < 5) {
		LOG("phy: RDPCSTX0 died between DAL accesses: after %s %x (%llu us ago), before %s %x", gLastRd ? "read" : "write",
			gLastReg, nowUs() - gLastAccessUs, rd ? "read" : "write", r);
		traceDump("died between DAL accesses");
	}
	gPhyAlive = alive;
}

void phyExit(bool rd, uint32_t r, uint32_t hw, uint32_t val) {
	if (!phyPollOn())
		return;
	bool alive = mmioRead(PhyA) != 0;
	static uint32_t logged;
	if (gPhyAlive && !alive && logged++ < 5) {
		LOG("phy: RDPCSTX0 died right after DAL %s reg %x (hw %x) val %x", rd ? "read" : "write", r, hw, val);
		traceDump("died on a DAL access");
	}
	gPhyAlive = alive;
	gLastAccessUs = nowUs();
	gLastReg = r;
	gLastRd = rd;
}

extern bool gKeepPhyWrites;

// HUBPREQn_DCSURF_SURFACE_CONTROL (pipe stride 0xdc): with DCC decode on, the scanout is scrambled
// (Sequoia wallpaper recognizable only by color); with it cleared the image is clean, so the
// framebuffer content is not actually DCC-compressed the way HUBP expects. Clear all DCC enables /
// independent-block bits. -rmbdcc keeps them.
bool gKeepDcc;
constexpr uint32_t SurfCtl0 = 0x3ada, SurfCtlDcc = 0x2626;
bool isSurfCtl(uint32_t hw) { return hw >= SurfCtl0 && hw <= SurfCtl0 + 3 * 0xdc && (hw - SurfCtl0) % 0xdc == 0; }
bool isPhyHole(uint32_t hw);

// HUBPREQn_DCN_VM_SYSTEM_APERTURE_LOW/HIGH_ADDR: on the APU the framebuffer (MC 0xf4xxxxxxxx) is only
// reachable through the system aperture. The GOP sets it for HUBP0 only and Navi23 DAL (dGPU, no
// vm_pa_config) never writes it, so any other pipe fetches nothing: HUBP underflow, the screen shows
// the MPCC background (0,0,0 = pure green on a YCbCr HDMI stream). Program it like Linux
// hubp21_set_vm_system_aperture_settings from DCN_VM_FB_LOCATION before each surface address write.
// -rmbnoaperture leaves it.
bool gNoAperture;
constexpr uint32_t SurfAddr0 = 0x3aca, AptLow0 = 0x3af5, AptHigh0 = 0x3af6, FbLocBase = 0x3935, FbLocTop = 0x3936;
bool isSurfAddr(uint32_t hw) { return hw >= SurfAddr0 && hw <= SurfAddr0 + 3 * 0xdc && (hw - SurfAddr0) % 0xdc == 0; }
void fixAperture(uint32_t pipe) {
	uint32_t base = mmioRead(FbLocBase) & 0xFFFFFF, top = mmioRead(FbLocTop) & 0xFFFFFF;
	if (!base || top < base)
		return;
	uint32_t lo = base << 6, hi = ((top + 1) << 6) - 1;  // 16 MB units -> 256 KB units
	uint32_t off = pipe * 0xdc;
	if (mmioRead(AptLow0 + off) == lo && mmioRead(AptHigh0 + off) == hi)
		return;
	static uint32_t logged;
	if (logged++ < 8)
		LOG("dcn: HUBP%u system aperture %x-%x -> %x-%x", pipe, mmioRead(AptLow0 + off), mmioRead(AptHigh0 + off), lo, hi);
	mmioWrite(AptLow0 + off, lo);
	mmioWrite(AptHigh0 + off, hi);
}

// DCHUBBUB_DETn_CTRL: DCN 3.1 splits the 1792 KB return buffer (28 x 64 KB segments) between per-pipe
// detile buffers and the compressed buffer; Navi23 (DCN 3.0, fixed DET) DAL never sizes it. The GOP
// gives DET0 3 segments and every other pipe 0 -> that HUBP can't fetch at all (HUBP_UNDERFLOW, same
// green screen). Give an unsized pipe Linux's default 384 KB (6 segments) if the buffer has room.
// Verified live on HDMI pipe 1 (underflow gone). -rmbnodet leaves it.
bool gNoDet;
constexpr uint32_t CompbufCtl = 0x398b, Det0Ctl = 0x398c, CrbSegs = 28, DefaultDetSegs = 6;
void fixDet(uint32_t pipe) {
	if (mmioRead(Det0Ctl + pipe) & 7)
		return;
	uint32_t used = mmioRead(CompbufCtl) & 0x1f;
	for (uint32_t i = 0; i < 4; i++)
		used += mmioRead(Det0Ctl + i) & 7;
	uint32_t segs = used >= CrbSegs ? 0 : CrbSegs - used < DefaultDetSegs ? CrbSegs - used : DefaultDetSegs;
	LOG("dcn: HUBP%u DET size 0 -> %u segments (%u in use)", pipe, segs, used);
	if (segs)
		mmioWrite(Det0Ctl + pipe, segs);
}

// DP_DTOn_MODULO (DCCG, seg1 0xC0 + 0x82 + 4n): pixel clock = DTO ref * PHASE / MODULO, and DAL
// writes MODULO = its idea of the DTO ref (Navi23: 730 MHz). Rembrandt's DP DTO ref is dprefclk =
// 600 MHz (Linux dcn31_clk_mgr), so the panel ran at 49.3 Hz instead of 60 and flashed white on
// static screens. Rescale MODULO to the real ref. -rmbdto keeps DAL's value.
bool gKeepDto;
constexpr uint32_t DpDto0Modulo = 0xC0 + 0x82, NaviDtoRefKhz = 730000, RmbDtoRefKhz = 600000;
bool isDtoModulo(uint32_t hw) { return hw >= DpDto0Modulo && hw <= DpDto0Modulo + 3 * 4 && (hw - DpDto0Modulo) % 4 == 0; }
// DCHUBBUB_ARB_DRAM_STATE_CNTL: with self-refresh (stutter) allowed, DCN runs on Navi23's SR
// watermarks and the panel shows random-height grey/white/yellow/blue bands below the menu bar,
// much more often with 16 vCPUs (DRAM contention). Force SR disallowed (FORCE_ENABLE=1, VALUE=0),
// as Linux does with disable_stutter. -rmbstutter keeps DAL's value.
bool gKeepStutter;
constexpr uint32_t DramStateCntl = 0x39bc, SrForceEnable = 2, SrForceValue = 1;
constexpr uint32_t DmcubInbox1Wptr = Seg2 + 0x1d6, DmcubInbox1Rptr = Seg2 + 0x1d7, DmcubCntl = Seg2 + 0x1f6,
				   DmcubCntl2 = Seg2 + 0x200, DmcubScratch0 = Seg2 + 0x1e3, DmcubCw4OffLo = Seg2 + 0x1bd,
				   DmcubCw4OffHi = Seg2 + 0x1be;
bool inboxHeld();
extern uint32_t gDalWptr;

mach_vm_address_t orgRead, orgWrite;
thread_t gDalThread;

// Navi23 DAL clk_mgr SMU mailbox (FB 25G229 UUID C3FA8B38): dcn30_smu_send_msg_with_param (text 0x90082)
// writes resp C2PMSG_52 (0x16274) = 0, arg C2PMSG_51 (0x16273), msg C2PMSG_74 (0x1628a), and
// dcn30_smu_wait_for_response (0x906f0) polls 0x16274 200001 x with udelay(10) before and after.
// Rembrandt PMFW has no DAL mailbox there (its display mailbox is VBIOSSMC 67/83/91, dmub.cpp), so
// dcn3_init_clocks (0xf94f9) -> GetSmuVersion (msg 2, 0x90170) times out: every boot log shows the DAL
// thread silent 6.28-6.31 s between the DMCUB release and bios_golden_init, with the PM work queue (USB
// power-on) held behind it. Keep DAL's mailbox writes off the hardware and answer 0x16274 with
// DALSMC_Result_UnknownCmd (0xFE): 0x90116-0x9016f treats 0xFE like a timeout (returns false, no arg
// read) minus the waits, so smu_present stays false exactly as today. Not a success reply: DAL sends
// no SMU clock requests either way; the display floors come from the VBIOSSMC votes in dmub.cpp.
// -rmbnavismu: pass DAL's mailbox accesses through as before.
constexpr uint32_t NaviDalSmuArg = 0x16273, NaviDalSmuResp = 0x16274, NaviDalSmuMsg = 0x1628a;
constexpr uint32_t DalsmcResultUnknownCmd = 0xFE;
bool gNaviSmu;
uint32_t gNaviSmuArg, gNaviSmuMsgs;

uint64_t wrapRead(uint64_t helper, uint64_t reg, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
	uint32_t r = reg & 0xFFFFFFFF;
	if (r >= PgFirst && r <= PgLast) {
		uint32_t off = r - Seg2;
		if (isPgStatus(off))
			return (gPgShadow[r - 1 - PgFirst] & (1u << 8)) ? (2u << 30 | 2u << 28) : 0;
		return gPgShadow[r - PgFirst];
	}
	if (!gNaviSmu && (r == NaviDalSmuResp || r == NaviDalSmuArg))
		return r == NaviDalSmuResp ? DalsmcResultUnknownCmd : gNaviSmuArg;
	if ((r == DmcubInbox1Wptr || r == DmcubInbox1Rptr) && inboxHeld()) {
		uint32_t hwW = mmioRead(DmcubInbox1Wptr);
		if (r == DmcubInbox1Wptr || mmioRead(DmcubInbox1Rptr) == hwW)
			return gDalWptr;
	}
	uint32_t hw = remap(r, false);
	auxDalTouch(hw);
	if (!gKeepPhyWrites && isPhyHole(hw)) {
		static uint32_t logged;
		if (logged++ < 60 && rmbVerbose())
			LOG("phy: skipped DAL read of undefined reg %x (hw %x)", r, hw);
		return 0;
	}
	phyEnter(true, r);
	gDalThread = current_thread();
	auto v = reinterpret_cast<Fn6>(orgRead)(helper, hw, c, d, e, f);
	gDalThread = nullptr;
	if (auto k = fieldKind(hw); k != Fields::None)
		v = fieldsToDal(k, v & 0xFFFFFFFF);
	traceAccess(true, r, hw, v & 0xFFFFFFFF);
	v = (v & ~0xFFFFFFFFull) | usbcTraceRead(hw, v & 0xFFFFFFFF);
	phyExit(true, r, hw, v & 0xFFFFFFFF);
	if (r == CcDcPipeDis) {
		static bool logged;
		if (!logged) {
			logged = true;
			LOG("dcn: CC_DC_PIPE_DIS %llx -> %llx (pipe 4 absent on DCN 3.1.2)", v & 0xFFFFFFFF, (v | 0x10) & 0xFFFFFFFF);
		}
		v |= 0x10;
	}
	return v;
}

// DMCUB inbox 1: log every command DAL submits (slots between the old and new WPTR) with the
// DMCUB run state, to see what reaches the firmware and when. The ring lives at code window 4
// (VRAM offset = CW4 offset - GCMC FB base).

// Linux DCN3.x hooks enable_display_power_gating to a dummy and never sends
// DMUB_CMD__VBIOS_ENABLE_DISP_POWER_GATING; Navi23 DAL does, and RDPCSTX0 dies right after.
// Turn those commands into DMUB_CMD__NULL before the firmware sees them (-rmbpwrgate: keep them).
bool gKeepPwrGate;

// Linux dcn31_init_hw does send them via bios_golden_init (global PIPE_INIT + per-pipe DISABLE)
// when the VBIOS isn't in accelerated mode, i.e. a cold DCN after S3. GOP-trained boots don't need
// them, but after an S3 sleep the PHY died ~10 ms into wake training with them NULLed (s3wake6).
// Pass them through after a power loss until the first transmitter cmd (-rmbnocoldpg: never).
bool gDcnCold, gNoColdPg;

// Navi23 DAL's dce110_edp_power_control decides on/off from PWRSEQ_STATE, which on DCN3.1 stays "on"
// after the DMUB firmware cuts panel VDD: every link-training attempt sends LVTMA POWER_OFF (VBIOS
// sub 0xf, action 0x0d) + transmitter POWER_OFF (sub 1, action 0x0d), then skips POWER_ON, so the
// panel trains unpowered (CR 0 on all lanes). Turn the power-offs into NULL cmds (-rmbedpoff: keep).
bool gKeepEdpOff;

// With the power-offs gone the panel keeps VDD, so every later POWER_ON (sub 0xf / sub 1, action
// 0x0c) is redundant, and the firmware sometimes kills RDPCSTX0 processing one (s3wake5: display went
// offline right after the wake modeset, DISABLE then POWER_ON 530 ms later, PHY 0, DMUB wedged).
// Let one through after boot and after each S3 sleep (VDD really off then), NULL the rest
// (-rmbedpon: keep them all).
bool gKeepEdpOn;
bool gEdpPowered;
// A POWER_OFF really reached DMUB since the last passed POWER_ON. gEdpPowered alone can't say "panel
// is off": a GOP-lit boot never sends a POWER_ON (auto-1024-0745: replay skipped, shutdown lines).
bool gEdpOffDone;

// With VDD kept on, shutdown and sleep left the panel lit with no stream: vertical lines crept in
// instead of black (auto-1017-1943/2000). Between a system shutdown/sleep notice and the next eDP
// power-up, let DAL's POWER_OFF through (-rmbedpoffkeep: keep NULLing them); the following POWER_ON
// then goes through too, since the panel really is off.
bool gPanelOffOk, gKeepEdpOffAlways;

// But kIOMessageSystemWillPowerOff only arrives after "CPU halted", long after DAL's last POWER_OFF
// (sent at display sleep / WindowServer exit) was NULLed, so nothing turned the panel off and the
// lines stayed (auto-1017-2023). Keep a copy of the last dropped pair and replay it at shutdown.
uint64_t gEdpOffCmd[2][8];  // [0] = sub 0xf, [1] = sub 1
uint32_t gEdpOffSaved;
// With no display sleep before shutdown nothing was dropped, so there's no pair to replay and
// DAL's own POWER_OFF only comes after our MODE2 (DMUB stuck, lines again: auto-1024-0631).
// The passed POWER_ON pair is the same command with action 0x0c, so keep it and flip it to 0x0d.
uint64_t gEdpOnCmd[2][8];
uint32_t gEdpOnSaved;

// RDPCSTX1 (PHY B, HDMI) CNTL / PHY_CNTL0 / PHY_CNTL6 and RDPCSTX0_CNTL, for the HDMI logs.
// PHY B is never read from the CPU (see isPhyHole): logs 0xdead for it unless -rmbphybcpu.
uint32_t phyRead(uint32_t hw);
#define PHYB_REGS phyRead(Seg2 + 0x2a08), phyRead(Seg2 + 0x2a18), phyRead(Seg2 + 0x2a1e), mmioRead(Seg2 + 0x2930)

// When the stream goes away Navi23 DAL powers down every COMBO-PHY PLL (VBIOS SET_PIXEL_CLOCK, sub 2,
// pixclk 0, pll_id ATOM_COMBOPHY_PLL0-5 = 20-25). Linux DCN3.1 drives DP from the DP DTO and never
// does that; afterwards RDPCSTX0 reads 0 and the firmware hangs in the next transmitter ENABLE.
// NULL those power-downs (-rmbpllpd: keep).
// Only PLL 20 (PHY A, eDP) though: Linux does power the other PHY PLLs down (cs_power_down, FORCE
// program), and with PHY B's dropped the first HDMI SET_PIXEL_CLOCK (PLL 21) hung DMUB for good with
// the whole RDPCSTX1 block reading 0 (auto-1019-1248). -rmbpllpdall: NULL all of 20-25 again.
bool gKeepPllPd, gDropAllPllPd, gUsbcPllPd;
// Bare-metal Linux (logs/linux-hdmi-dmub.txt) powers PLL 20-24 down 1.5 ms before the HDMI SET_PIXEL_CLOCK
// on PLL 21; ours never reach DMUB (sent at init with no OTG on). Keep the PLL 21-25 ones and send them
// later in a slot we NULL anyway (dropped eDP POWER_ON etc.) once an OTG runs. -rmbnopllpdlate: don't.
bool gNoPllPdLate;
uint64_t gPllPdCmd[5][8];
uint32_t gPllPdPend;
struct StreamInfo {
	uint32_t pixKhz, enc, depth;
};
StreamInfo gPendStream, gExtStream;
uint32_t gExtLink;
// AmdRadeonController, set by wrap_establishBootDisplay on every boot (gCtrl is set only when it fails).
uint64_t gDalCtrl;

// Signal of link i from the controller's detect status (ctrl+24376 + i*0x440, the DetectedLinkStatus that
// detectDisplayOnLink (FB 0x4e51e) passes to detectLink; dword 2 = DAL signal type). disklog 2026-11-11:
// link 0 (eDP) 0x80, USB-C hub on link 1 0x20; DAL enum: DVI 1/2, HDMI 4, DP 0x20, DP MST 0x40, eDP 0x80.
// 0 = controller or link unknown.
uint32_t linkSignal(uint32_t i) {
	if (!gDalCtrl || i >= *reinterpret_cast<volatile uint8_t *>(gDalCtrl + 288))  // +288 link count
		return 0;
	return *reinterpret_cast<volatile uint32_t *>(gDalCtrl + 24376 + i * 0x440ULL + 8);
}

// The external stream's link is DP (USB-C DP alt), not HDMI/DVI. The Win Max 2 had HDMI on link 1 and USB-C
// on links 2/3, so this used to be gExtLink >= 2; the Lenovo 82TL has no HDMI connector and its first USB-C
// DP (PHY C / AUX2) is link 1. Unknown counts as DP: Linux never sends DP PLL power-downs.
bool extLinkIsDp() {
	uint32_t s = linkSignal(gExtLink);
	return s != 1 && s != 2 && s != 4;
}

// Navi23 DAL sends a type 0x4c cmd after the watermarks in every commit (payload 0, pixclk 100 Hz, 1).
// In the yellow_carp firmware 0x4c is DMUB_CMD__FW_ASSISTED_MCLK_SWITCH, which Linux DCN3.1 never sends.
// DMUB hung on one in auto-1017-1852, and the HDMI SET_PIXEL_CLOCK right after one hangs every time.
// NULL them (-rmbfams: keep).
bool gKeepFams;

// DMUB ABM SET_BACKLIGHT (type 0x42 sub 2): Navi23 DAL's ABM was never set up for this panel in the
// Rembrandt firmware (no ABM SET_PIPE), so program the PWM like Linux dce_driver_set_backlight and
// NULL the cmd (-rmbabm: pass it). level = u16.16 duty, 0x10000 = full; kept >= 2% so the panel
// never goes dark.
constexpr uint32_t BlPwmCntl = 0x34C0 + 0x2f19, BlPwmPeriod = BlPwmCntl + 2, BlPwmLock = BlPwmCntl + 3;

// After S3 the DMUB firmware owns PWRSEQ0 (auto-1022-1102: the whole block reads 0 and ignores CPU and
// host writes; PWRSEQ1 stays writable), the DCN3.1 way: Linux always drives the panel PWM through DMUB
// PANEL_CNTL HW_INIT (hands the saved PWM regs to the fw, which restores them) + ABM SET_BACKLIGHT.
// The regs are saved while the CPU still owned the PWM. -rmbnofwbl: just skip like before.
struct BlRegs {
	uint32_t cntl, period, refDiv1, refDiv2;
};
BlRegs gBlSaved;
bool gNoFwBl, gPanelCntlInit;
bool cpuOwnsPwm() { return mmioRead(BlPwmPeriod) & 0xffff; }
void dmubBacklight(uint32_t level);

// Correction (auto-1022-1212): PWRSEQ0 is not fw-owned, it is DEAD. It died between display sleep
// (+139 s) and the next modeset (+142 s), long before S3, with no ABM SET_PIPE passed. Then the whole
// block times out on the register bus (RBBMIF_INT_STATUS timeout addr 0x63e1), reads 0 and drops
// writes, the fw too (PANEL_CNTL replies current_backlight 0), and the PWM keeps its last duty (the 2%
// display-sleep dim -> "super dark"). Host test, VM off: one DCIO_SOFT_RESET.PWRSEQ0_SOFT_RESET pulse
// brings it back at reset defaults, then the CPU lock/write/unlock works; hole reads/writes at the Navi
// LVTMA_PWRSEQ addresses (0x5d43-0x5d47), zero REF_DIVs, CNTL2/LOCK writes do NOT kill it. So revive it
// and restore everything saved while it lived, REF_DIV1 + PANEL_PWRSEQ_CNTL (DIGON override = panel
// VDD) first. -rmbnorevive: the DMUB PANEL_CNTL path above.
constexpr uint32_t DcioSoftReset = Seg2 + 0x289e, Pwrseq0SoftReset = 1u << 16, PsCntl = Seg2 + 0x2f14,
				   PsDelay1 = PsCntl + 2, PsDelay2 = PsCntl + 3, PsRefDiv1 = PsCntl + 4, BlPwmCntl2 = BlPwmCntl + 1,
				   PsRefDiv2 = PsCntl + 9;
struct PwrseqRegs {
	uint32_t cntl, delay1, delay2, refDiv1, blCntl, blCntl2, period, lock, refDiv2;
};
PwrseqRegs gPs;
bool gNoRevive;
uint32_t gRevives;

// Alive but at reset defaults (PERIOD 1, BL_PWM_EN off) is not a state to save or to write a duty into.
bool pwrseqProgrammed(uint32_t period, uint32_t blCntl) { return (period & 0xffff) > 1 && (blCntl & 0x80000000); }

// Who kills PWRSEQ0? Every death so far came at a display off whose DMUB cmds were all NULLed, so check
// it around every CPU write (DAL, raw AmdRegisterAccess, VBIOS tables) while it lives. Only while alive:
// a dead block costs a 33 us bus timeout per read.
bool gPsAlive;
void pwrseqWatch(bool before, const char *what, uint32_t reg, uint32_t val) {
	if (!gPsAlive || (mmioRead(BlPwmPeriod) & 0xffff))
		return;
	gPsAlive = false;
	static uint32_t logged;
	if (logged++ < 6) {
		LOG("bl: PWRSEQ0 died %s %s %x = %x", before ? "BEFORE (async, not this write)" : "right after", what, reg, val);
		// 256 trace lines hold the display-off path ~6 s at the serial cap (slows every sleep); -rmbpsdump.
		static int dump = -1;
		if (dump < 0)
			dump = checkKernelArgument("-rmbpsdump");
		if (dump && logged <= 2)
			traceDump("PWRSEQ0 died");
	}
}

void psDiagBoot();

void savePwrseq0() {
	PwrseqRegs p = {mmioRead(PsCntl),	 mmioRead(PsDelay1),   mmioRead(PsDelay2),
					mmioRead(PsRefDiv1), mmioRead(BlPwmCntl),  mmioRead(BlPwmCntl2),
					mmioRead(BlPwmPeriod), mmioRead(BlPwmLock), mmioRead(PsRefDiv2)};
	// auto-1022-1743: after S3 the block came back by itself at reset defaults (PERIOD 1, BL_PWM_CNTL 0);
	// saving that made the next revive "restore" duty 0 -> panel lit but backlight off after wake.
	if (!pwrseqProgrammed(p.period, p.blCntl))
		return;
	gPsAlive = true;
	if (gPs.period && !(p.cntl & 0x10))  // PANEL_PWRSEQ_TARGET_STATE off: a revive must not power the panel down
		p.cntl = gPs.cntl;
	if (!gPs.period)
		psDiagBoot();
	if (!gPs.period)
		LOG("bl: PWRSEQ0 saved: CNTL %x DELAY1 %x DELAY2 %x REF_DIV1 %x BL_PWM_CNTL %x CNTL2 %x PERIOD %x LOCK %x REF_DIV2 %x",
			p.cntl, p.delay1, p.delay2, p.refDiv1, p.blCntl, p.blCntl2, p.period, p.lock, p.refDiv2);
	gPs = p;
}

// auto-1022-1243: revive #10 came back at reset defaults (CNTL 0 STATE 14 PERIOD 1) with every restore
// write lost (written 2 us after the reset) -> PANEL_PWRSEQ off -> eDP retrain failed after wake, panel
// dark for good. So hold the reset longer, wait until the block answers, restore, read back and retry.
void restorePwrseq0() {
	mmioWrite(PsRefDiv1, gPs.refDiv1);
	mmioWrite(PsCntl, gPs.cntl);
	mmioWrite(PsDelay1, gPs.delay1);
	mmioWrite(PsDelay2, gPs.delay2);
	mmioWrite(PsRefDiv2, gPs.refDiv2);
	mmioWrite(BlPwmPeriod, gPs.period);
	mmioWrite(BlPwmCntl2, gPs.blCntl2);
	mmioWrite(BlPwmCntl, gPs.blCntl);
	mmioWrite(BlPwmLock, gPs.lock & ~0x101u);
}

bool revivePwrseq0(uint32_t level) {
	if (gNoRevive || !gPs.period)
		return false;
	uint32_t sr = mmioRead(DcioSoftReset);
	mmioWrite(DcioSoftReset, sr | Pwrseq0SoftReset);
	IODelay(50);
	mmioWrite(DcioSoftReset, sr & ~Pwrseq0SoftReset);
	uint32_t waitUs = 0;
	while (!(mmioRead(BlPwmPeriod) & 0xffff) && waitUs < 1000) {
		IODelay(10);
		waitUs += 10;
	}
	uint32_t tries = 0, cntl = 0, per = 0;
	if (mmioRead(BlPwmPeriod) & 0xffff) {
		for (; tries < 5; tries++) {
			if (tries)
				IODelay(100);
			restorePwrseq0();
			cntl = mmioRead(PsCntl);
			per = mmioRead(BlPwmPeriod);
			if (cntl == gPs.cntl && (per & 0xffff) == (gPs.period & 0xffff))
				break;
		}
	}
	bool ok = cntl == gPs.cntl && pwrseqProgrammed(per, mmioRead(BlPwmCntl));
	// auto-1022-1755: after revive #8 every reg was right (STATE 41f, PWM toggling on the pin, OTG + PHY
	// lit) yet the panel was black with no glow: the death dropped DIGON (panel VDD), the panel reset and
	// lost the eDP link trained before. Retrain it.
	if (ok) {
		gPsAlive = true;
		panelRetrain();
	}
	if (gRevives++ < 40)
		LOG("bl: PWRSEQ0 revived #%u for level %x (DCIO_SOFT_RESET %x, alive after %u us, %u restores): CNTL %x STATE %x "
			"BL_PWM_CNTL %x PERIOD %x LOCK %x%s",
			gRevives, level, sr, waitUs, tries + 1, mmioRead(PsCntl), mmioRead(PsCntl + 1), mmioRead(BlPwmCntl),
			mmioRead(BlPwmPeriod), mmioRead(BlPwmLock),
			ok ? "" : (mmioRead(BlPwmPeriod) & 0xffff) ? " - RESTORE FAILED" : " - STILL DEAD");
	return ok;
}

// Long-S3 dark panel (BM 10-28..30): right after the wake vbs_post PWRSEQ0 is alive at reset defaults
// (PERIOD 1), ~8 s later (DMUB reload + VBIOSSMC votes in between) it reads all 0 and the DCIO soft reset
// no longer brings it back (revive STILL DEAD for minutes; panel VDD/HPD never return). Which clock or
// power gate takes it is unknown, so: snapshot the DCCG/DMU/PG/RBBMIF/DCIO regs while it works at boot,
// poll it every 10 ms after wake (dmub poller) logging the death moment + changes of a few gate/status
// regs, diff everything at death, then try one gate at a time (each followed by the normal reset+restore)
// and log which one revives it. -rmbnopsdiag: off.
struct PsDiagReg {
	uint32_t hw;
	const char *name;
};
constexpr uint32_t DcSeg1 = 0xC0;
constexpr PsDiagReg PsDiagRegs[] = {
	{DcSeg1 + 0x48, "DPREFCLK_CGTT_BLK_CTRL"}, {DcSeg1 + 0x49, "DCCG_GATE_DISABLE_CNTL4"},
	{DcSeg1 + 0x4b, "REFCLK_CGTT_BLK_CTRL"}, {DcSeg1 + 0x55, "DCCG_DS_CNTL"}, {DcSeg1 + 0x58, "DPREFCLK_CNTL"},
	{DcSeg1 + 0x71, "DISPCLK_FREQ_CHANGE_CNTL"}, {DcSeg1 + 0x72, "DC_MEM_GLOBAL_PWR_REQ_CNTL"},
	{DcSeg1 + 0x74, "DCCG_GATE_DISABLE_CNTL"}, {DcSeg1 + 0x75, "DISPCLK_CGTT_BLK_CTRL"},
	{DcSeg1 + 0x76, "SOCCLK_CGTT_BLK_CTRL"}, {DcSeg1 + 0x7c, "DCCG_GATE_DISABLE_CNTL2"},
	{DcSeg1 + 0x7d, "SYMCLK_CGTT_BLK_CTRL"}, {DcSeg1 + 0x7f, "DCCG_DISP_CNTL"}, {DcSeg1 + 0xa6, "DCCG_SOFT_RESET"},
	{Seg2 + 0x5a, "DCCG_GATE_DISABLE_CNTL3"}, {Seg2 + 0x80, "DOMAIN0_PG_CONFIG"}, {Seg2 + 0x81, "DOMAIN0_PG_STATUS"},
	{Seg2 + 0x83, "DOMAIN1_PG_STATUS"}, {Seg2 + 0x85, "DOMAIN2_PG_STATUS"}, {Seg2 + 0x87, "DOMAIN3_PG_STATUS"},
	{Seg2 + 0x8a, "DOMAIN16_PG_STATUS"}, {Seg2 + 0x8c, "DOMAIN17_PG_STATUS"}, {Seg2 + 0x8e, "DOMAIN18_PG_STATUS"},
	{Seg2 + 0x93, "DC_IP_REQUEST_CNTL"}, {Seg2 + 0xca, "CC_DC_PIPE_DIS"}, {Seg2 + 0xcb, "DMU_CLK_CNTL"},
	{Seg2 + 0xcc, "DMU_MEM_PWR_CNTL"}, {Seg2 + 0xcf, "ZSC_CNTL"}, {Seg2 + 0xd0, "ZSC_CNTL2"},
	{Seg2 + 0xd6, "DMU_MISC_ALLOW_DS_FORCE"}, {Seg2 + 0xd7, "ZSC_STATUS"}, {Seg2 + 0x17a, "DMCUB_RBBMIF_SEC_CNTL"},
	{Seg2 + 0x17f, "RBBMIF_TIMEOUT"}, {Seg2 + 0x180, "RBBMIF_STATUS"}, {Seg2 + 0x181, "RBBMIF_STATUS_2"},
	{Seg2 + 0x182, "RBBMIF_INT_STATUS"}, {Seg2 + 0x185, "RBBMIF_STATUS_FLAG"}, {Seg2 + 0x2880, "DC_PINSTRAPS"},
	{Seg2 + 0x2884, "INTERCEPT_STATE"}, {Seg2 + 0x289e, "DCIO_SOFT_RESET"}, {Seg2 + 0x2f10, "PWRSEQ0_GPIO_EN"},
	{Seg2 + 0x2f11, "PWRSEQ0_GPIO_CTRL"}, {Seg2 + 0x2f13, "PWRSEQ0_GPIO_A_Y"}, {Seg2 + 0x2f14, "PWRSEQ0_CNTL"},
	{Seg2 + 0x2f15, "PWRSEQ0_STATE"}, {Seg2 + 0x2f18, "PWRSEQ0_REF_DIV1"}, {Seg2 + 0x2f19, "PWRSEQ0_BL_PWM_CNTL"},
	{Seg2 + 0x2f1b, "PWRSEQ0_BL_PWM_PERIOD"}, {Seg2 + 0x2f1c, "PWRSEQ0_BL_PWM_LOCK"}};
constexpr uint32_t PsDiagN = sizeof(PsDiagRegs) / sizeof(PsDiagRegs[0]);
// Cheap set polled every 10 ms after wake (none in PWRSEQ0: a dead block costs a 33 us timeout per read).
constexpr uint32_t PsWatchIdx[] = {7, 25, 27, 30, 35, 20, 21, 22};  // GATE_DISABLE_CNTL, DMU_CLK, ZSC_CNTL/STATUS, RBBMIF_INT, PG16-18
uint32_t gPsBoot[PsDiagN];
bool gPsBootSnap, gPsDiagOff;
uint64_t gPsWakeUs;  // nonzero: armed by the wake vbs_post
bool gPsWakeAlive;
uint32_t gPsWatch[sizeof(PsWatchIdx) / sizeof(PsWatchIdx[0])];

void psSnap(uint32_t *v) {
	for (uint32_t i = 0; i < PsDiagN; i++)
		v[i] = mmioRead(PsDiagRegs[i].hw);
}

void psDiff(const char *tag) {
	uint32_t now[PsDiagN];
	psSnap(now);
	char buf[200];
	uint32_t len = 0, lines = 0;
	for (uint32_t i = 0; i < PsDiagN; i++) {
		if (gPsBootSnap && now[i] == gPsBoot[i])
			continue;
		int n = snprintf(buf + len, sizeof(buf) - len, " %s %x->%x", PsDiagRegs[i].name, gPsBootSnap ? gPsBoot[i] : 0, now[i]);
		if (n < 0 || len + n >= sizeof(buf) - 1) {
			buf[len] = 0;
			LOG("psdiag: %s vs boot:%s", tag, buf);
			lines++;
			len = 0;
			n = snprintf(buf, sizeof(buf), " %s %x->%x", PsDiagRegs[i].name, gPsBootSnap ? gPsBoot[i] : 0, now[i]);
		}
		len += n;
	}
	buf[len] = 0;
	LOG("psdiag: %s vs boot:%s%s", tag, buf, len || lines ? "" : " (no difference)");
}

bool psAlive() { return mmioRead(BlPwmPeriod) & 0xffff; }

// One gate at a time, each followed by revivePwrseq0 (DCIO soft reset + restore + retrain).
void psTryRevive() {
	static const struct { uint32_t hw, set; const char *what; } Steps[] = {
		{DcSeg1 + 0x74, 0x7c4e1f5f, "DCCG_GATE_DISABLE_CNTL all gates off"},
		{DcSeg1 + 0x49, 0x2001f, "DCCG_GATE_DISABLE_CNTL4 refclk root gates off"},
		{Seg2 + 0x5a, 0xf0ffff, "DCCG_GATE_DISABLE_CNTL3 all gates off"},
		{Seg2 + 0xcb, 0x70, "DMU_CLK_CNTL DMU/DMCU/RBBMIF gates off"},
		{DcSeg1 + 0x72, 0x1, "DC_MEM_GLOBAL_PWR_REQ_DIS"},
		{0, 0, "VBIOSSMC DisallowZstatesEntry + IdleOpt(0) again"}};
	for (auto &s : Steps) {
		if (s.hw) {
			uint32_t b = mmioRead(s.hw);
			mmioWrite(s.hw, b | s.set);
			LOG("psdiag: try %s: %x -> %x", s.what, b, mmioRead(s.hw));
		} else {
			uint32_t r1 = vbiosSmcSend(0x16, 0), r2 = vbiosSmcSend(0x12, 0);
			LOG("psdiag: try %s: resp %x %x", s.what, r1, r2);
		}
		IODelay(1000);
		bool before = psAlive();
		bool ok = revivePwrseq0(0);
		LOG("psdiag: after \"%s\": PWRSEQ0 %s before revive, revive %s", s.what, before ? "ALIVE" : "dead", ok ? "OK" : "failed");
		if (ok) {
			psDiff("revived");
			return;
		}
	}
}

void psDiagBoot() {
	if (gPsBootSnap)
		return;
	gPsDiagOff = !checkKernelArgument("-rmbpsdiag");
	if (gPsDiagOff)
		return;
	psSnap(gPsBoot);
	gPsBootSnap = true;
	char buf[200];
	uint32_t len = 0;
	for (uint32_t i = 0; i < PsDiagN; i++) {
		int n = snprintf(buf + len, sizeof(buf) - len, " %s %x", PsDiagRegs[i].name, gPsBoot[i]);
		if (n < 0 || len + n >= sizeof(buf) - 1) {
			buf[len] = 0;
			LOG("psdiag: boot:%s", buf);
			len = 0;
			n = snprintf(buf, sizeof(buf), " %s %x", PsDiagRegs[i].name, gPsBoot[i]);
		}
		len += n;
	}
	buf[len] = 0;
	LOG("psdiag: boot:%s", buf);
}

void psWakeArm() {
	if (gPsDiagOff || !checkKernelArgument("-rmbpsdiag"))
		return;
	gPsWakeAlive = psAlive();
	for (uint32_t i = 0; i < sizeof(PsWatchIdx) / sizeof(PsWatchIdx[0]); i++)
		gPsWatch[i] = mmioRead(PsDiagRegs[PsWatchIdx[i]].hw);
	gPsWakeUs = nowUs();
	LOG("psdiag: wake armed, PWRSEQ0 %s", gPsWakeAlive ? "alive" : "dead");
	psDiff("wake start");
}

// dmub poller, every 1-10 ms.
void psWakeTick() {
	if (!gPsWakeUs)
		return;
	uint64_t ms = (nowUs() - gPsWakeUs) / 1000;
	if (ms > 300000) {
		LOG("psdiag: wake watch done, PWRSEQ0 %s", psAlive() ? "alive" : "dead");
		gPsWakeUs = 0;
		return;
	}
	static uint32_t lines;
	for (uint32_t i = 0; i < sizeof(PsWatchIdx) / sizeof(PsWatchIdx[0]); i++) {
		auto &r = PsDiagRegs[PsWatchIdx[i]];
		uint32_t v = mmioRead(r.hw);
		if (v != gPsWatch[i] && lines < 120) {
			lines++;
			LOG("psdiag: +%llu ms %s %x -> %x (PWRSEQ0 %s)", ms, r.name, gPsWatch[i], v, gPsWakeAlive ? "alive" : "dead");
		}
		gPsWatch[i] = v;
	}
	// Alive: a cheap read. Dead: check every 100 ms only (each dead read = 33 us bus timeout).
	if (!gPsWakeAlive && ms % 100 >= 10)
		return;
	bool a = psAlive();
	if (a == gPsWakeAlive)
		return;
	gPsWakeAlive = a;
	static uint32_t flips;
	if (flips++ >= 12)
		return;
	LOG("psdiag: +%llu ms PWRSEQ0 %s", ms, a ? "CAME BACK" : "DIED");
	psDiff(a ? "came back" : "died");
	if (!a && flips <= 2)
		psTryRevive();
}

void setBacklight(uint32_t level) {
	if (level > 0x10000)
		level = 0x10000;
	if (level < 0x10000 / 50)
		level = 0x10000 / 50;
	uint32_t per = mmioRead(BlPwmPeriod), bits = per >> 16 & 0x1f, cntl = mmioRead(BlPwmCntl);
	if (!(per & 0xffff) && revivePwrseq0(level)) {
		per = mmioRead(BlPwmPeriod);
		bits = per >> 16 & 0x1f;
		cntl = mmioRead(BlPwmCntl);
	}
	if ((per & 0xffff) && !pwrseqProgrammed(per, cntl) && gPs.period && !gNoRevive) {
		restorePwrseq0();
		static uint32_t restores;
		if (restores++ < 20)
			LOG("bl: PWRSEQ0 at reset defaults (BL_PWM_CNTL %x PERIOD %x) -> restored saved regs: CNTL %x BL_PWM_CNTL %x PERIOD %x",
				cntl, per, mmioRead(PsCntl), mmioRead(BlPwmCntl), mmioRead(BlPwmPeriod));
		per = mmioRead(BlPwmPeriod);
		bits = per >> 16 & 0x1f;
		cntl = mmioRead(BlPwmCntl);
	}
	// auto-1021-1528: during a modeset both read 0 (block not readable); writing would set duty 0.
	if (!(per & 0xffff) || !(cntl & 0x80000000)) {
		// only when PERIOD is unreadable: the ring handler (inbox lock held) calls us only when it reads
		if (!gNoFwBl && gBlSaved.period && !(per & 0xffff)) {
			dmubBacklight(level);
			return;
		}
		LOG("bl: level %x skipped: BL_PWM_CNTL %x PERIOD %x unreadable (PWRSEQ0 CNTL %x REF_DIV1 %x, PWRSEQ1 BL %x, "
			"RBBMIF_SEC %x)", level, cntl, per, mmioRead(Seg2 + 0x2f14), mmioRead(Seg2 + 0x2f18), mmioRead(0x6445),
			mmioRead(Seg2 + 0x17a));
		return;
	}
	if (!bits || bits > 16)
		bits = 16;
	uint32_t period = per & ((1u << bits) - 1);
	uint32_t active = ((uint64_t)period * level >> 16) << (16 - bits);
	if (active > 0xffff)
		active = 0xffff;
	mmioWrite(BlPwmLock, mmioRead(BlPwmLock) | 1);
	mmioWrite(BlPwmCntl, (cntl & ~0xffffu) | active);
	mmioWrite(BlPwmLock, mmioRead(BlPwmLock) & ~1u);
	uint32_t n = 0;
	while ((mmioRead(BlPwmLock) & 0x100) && n++ < 1000)  // BL_PWM_GRP1_REG_UPDATE_PENDING
		IODelay(10);
	gBlSaved = {mmioRead(BlPwmCntl), per, mmioRead(Seg2 + 0x2f18), mmioRead(Seg2 + 0x2f1d)};
	savePwrseq0();
	static uint32_t logged;
	if (logged++ < 40)
		LOG("bl: level %x: BL_PWM_CNTL %x -> %x (period %x bits %u, pending polls %u)", level, cntl, mmioRead(BlPwmCntl), period,
			bits, n);
}

// Backlight smoother (defined next to the panel curve, after setAttrImpl).
bool blRampSet(uint32_t bklt);
void blRampSync(uint32_t level);

static bool anyOtgOn() {
	bool on = false;
	for (uint32_t i = 0; i < 4; i++)
		on |= mmioRead(Seg2 + 0x1b41 + i * 0x80) & 1;  // OTGn_OTG_CONTROL.OTG_MASTER_EN
	return on;
}

// Linux sends DMUB_CMD__CLK_MGR NOTIFY_CLOCKS (type 0x49: dispclk, dppclk kHz) before every HDMI
// SET_PIXEL_CLOCK, in both its working and its hanging enable; Navi23 DAL never sends one. auto-1019-1557:
// with the PLL 21 power-down sent in the 0-stream commit before, DMUB still hung on the pixclk. So wait
// for the commit that carries the ext stream, then use its NULLed slots for CLK_MGR first and the PLL
// power-downs after, as close to the pixclk as we have slots. -rmbnoclkmgr: no CLK_MGR.
bool gNoClkMgr;
bool gClkMgrSent;

static void enableFwTrace();

// Put CLK_MGR or one saved PLL power-down into slot s (being NULLed). True if it did.
static bool sendLatePllPd(uint64_t ring, uint32_t s) {
	if (!gExtStream.pixKhz || !anyOtgOn())
		return false;
	enableFwTrace();  // before the PLL 21 power-down: does it run the PHY B reset path (trace d0/e2) like Linux?
	if (!gNoClkMgr && !gClkMgrSent) {
		gClkMgrSent = true;
		const uint32_t cmd[4] = {0x49, 600000, 100000, 0};  // header type 0x49 sub 0, dispclk, dppclk (Linux: 640000/100000)
		for (uint32_t k = 15; k >= 1; k--)
			vramWrite32(ring + s + k * 4, k < 4 ? cmd[k] : 0);
		vramWrite32(ring + s, cmd[0]);
		LOG("dmub-cmd: CLK_MGR notify clocks (dispclk %u dppclk %u) at [%x]", cmd[1], cmd[2], s);
		return true;
	}
	if (!gPllPdPend)
		return false;
	// Linux DP never sends a PLL power-down. On the USB-C links the PLL 21/24 ones landed ~1 ms after
	// video on and the sink lost CR on its x1 lane every time (usbc run 10-27 16:50). HDMI only.
	if (!gUsbcPllPd && extLinkIsDp()) {
		static bool logged;
		if (!logged) {
			logged = true;
			LOG("usbc: late PLL power-downs %x not sent (DP on link %u, signal %x, like Linux)", gPllPdPend, gExtLink,
				linkSignal(gExtLink));
		}
		return false;
	}
	uint32_t i = __builtin_ctz(gPllPdPend);
	gPllPdPend &= ~(1u << i);
	for (uint32_t k = 1; k < 16; k++)
		vramWrite32(ring + s + k * 4, (uint32_t)(gPllPdCmd[i][k / 2] >> (k % 2 * 32)));
	vramWrite32(ring + s, (uint32_t)gPllPdCmd[i][0]);  // header last
	LOG("dmub-cmd: late PHY PLL %u power-down at [%x] (%08llx %08llx); PHY B CNTL %x PHY_CNTL0 %x PHY_CNTL6 %x, PHY A CNTL %x",
		21 + i, s, gPllPdCmd[i][1] & 0xFFFFFFFF, gPllPdCmd[i][1] >> 32, PHYB_REGS);
	return true;
}


// DPCS/RDPCS PHY + DCIO UNIPHY macro registers (dpcs_4_2_0, seg2 0x2928-0x2d8e). On DCN3.1 the
// DMUB firmware owns them; Linux only reads them. Navi23 DAL writes RDPCSTX_CNTL = 0x400000 and
// DPCSTX_DEBUG_CONFIG during link-encoder init, which resets PHY A. Drop those (-rmbphywr: keep).
constexpr uint32_t PhyFirst = Seg2 + 0x2928, PhyLast = Seg2 + 0x2d8e;
bool gKeepPhyWrites;

// Navi23 DAL also READS dpcs_3_0_0 offsets that don't exist on DCN 3.1.2 (e.g. DPCSTX_DEBUG_CONFIG
// 0x292e); RDPCSTX0 dies on that read and its block then reads 0 / drops writes. Never touch them.
// UNIPHY1-4 (PHY B-E, seg2 0x2a00-0x2d8e) don't answer the CPU at all, even on bare-metal Linux while
// HDMI is lit: every read takes a 33 us bus timeout and returns 0. Our CPU reads there (DAL init,
// "now" of dropped writes, incl. UNIPHY1 RESERVED6 = PHY B's DEBUG_CONFIG hole) left PHY B dead for
// DMUB too: its PHY B steps ran 10-40x slower than Linux and SRAM_INIT_DONE never came
// (auto-1019-1647). Treat the whole block as a hole (-rmbphybcpu: old behaviour).
constexpr uint32_t PhyBFirst = Seg2 + 0x2a00;
bool gPhyBCpu;
bool isPhyHole(uint32_t hw) {
	if (hw < PhyFirst || hw > PhyLast)
		return false;
	if (hw >= PhyBFirst && !gPhyBCpu)
		return true;
	uint32_t i = hw - PhyFirst;
	return !(gPhyValid[i / 32] & (1u << (i % 32)));
}

uint32_t phyRead(uint32_t hw) { return isPhyHole(hw) ? 0xdeadu : mmioRead(hw); }

// HDMI SET_PIXEL_CLOCK (VBIOS sub 2, encoder_mode 3): Navi23 DAL (dce112_program_pix_clk) sends the
// deep-color-adjusted clock with deep_color_ratio 0 and no YUV420 misc bit; Linux dcn31_program_pix_clk
// sends the ratio and YUV420 flag too ("Make sure we send the correct color depth to DMUB for HDMI").
// DMUB hung for good on the first HDMI one (auto-1019-1231: 371.25 MHz, PLL 21, 4K60 4:2:0 10 bpc).
// Fill both in from the stream DAL committed for the non-eDP link (-rmbpixclk: leave as DAL sent it).
bool gKeepPixClk;

uint32_t hdmiClk100Hz(const StreamInfo &st) {
	uint64_t c = (uint64_t)st.pixKhz * 10;
	if (st.enc == 4)  // PIXEL_ENCODING_YCBCR420
		c /= 2;
	switch (st.depth) {  // COLOR_DEPTH_101010 / 121212 / 161616
	case 3: return (uint32_t)(c * 5 / 4);
	case 4: return (uint32_t)(c * 6 / 4);
	case 6: return (uint32_t)(c * 2);
	default: return (uint32_t)c;
	}
}

// DMUB also hangs on an HDMI SET_PIXEL_CLOCK when the target OTG's clock isn't running. Bare-metal Linux
// (2026-10-19): first HDMI enable on OTG0 still clocked from the GOP's eDP DP DTO worked (OTG_CLOCK_ON
// set, DMUB done in 20 ms); a re-enable right after KWin stopped OTG0's clock hung DMUB the same way
// as ours (OTG1 never clocked, OTG1_PIXEL_RATE_CNTL 0). So start the OTG on its DP DTO first and let
// the firmware switch it to the PHY PLL (-rmbotgclk: don't).
bool gNoOtgClk;

void startOtgClock(uint32_t crtc, uint32_t clk100Hz) {
	constexpr uint32_t Seg1 = 0xC0;
	if (gNoOtgClk || crtc > 3)
		return;
	uint32_t otgClk = Seg2 + 0x1b86 + crtc * 0x80, odmClk = Seg2 + 0x1acf + crtc * 0x10;
	uint32_t rate = Seg1 + 0x80 + crtc * 4;  // OTGn_PIXEL_RATE_CNTL, DP_DTOn_PHASE +1, _MODULO +2
	uint32_t before = mmioRead(otgClk), rateBefore = mmioRead(rate);
	// auto-1019-1415: OTG1 clock "on" (103) but PIXEL_RATE_CNTL 8000c000 = no DP/DTB DTO, i.e. fed by the
	// unstarted PHY PLL B, and DMUB still hung. Only a DTO-fed OTG clock counts as running.
	if ((before & (1u << 8)) && (rateBefore & 0x18)) {  // OTG_CLOCK_ON, DP_DTO/DTBCLK_DTO_ENABLE
		LOG("dmub-cmd: OTG%u clock already on (%x), PIXEL_RATE_CNTL %x", crtc, before, rateBefore);
		return;
	}
	uint32_t i = 0;
	if (before & 1) {  // gate the dead-sourced clock off before switching the source
		mmioWrite(otgClk, before & ~1u);
		while ((mmioRead(otgClk) & (1u << 8)) && i++ < 1000)
			IODelay(10);
	}
	uint32_t hz = clk100Hz * 100 < 600000000u ? clk100Hz * 100 : 600000000u;  // dprefclk 600 MHz
	mmioWrite(rate + 1, hz);
	mmioWrite(rate + 2, 600000000u);
	mmioWrite(rate, mmioRead(rate) | 0x10);  // DP_DTOn_ENABLE
	uint32_t j = 0;
	while (!(mmioRead(rate) & 0x80) && j++ < 1000)  // DPDTOn_ENABLE_STATUS
		IODelay(10);
	mmioWrite(odmClk, mmioRead(odmClk) | 3);  // OPTC_INPUT_CLK_GATE_DIS | _EN
	mmioWrite(otgClk, mmioRead(otgClk) | 3);  // OTG_CLOCK_EN | _GATE_DIS
	uint32_t k = 0;
	while (!(mmioRead(otgClk) & (1u << 8)) && k++ < 1000)
		IODelay(10);
	LOG("dmub-cmd: OTG%u clock started on DP DTO %u Hz: OTG_CLOCK_CONTROL %x -> %x, ODM %x, PIXEL_RATE_CNTL %x -> %x (polls off %u dto %u on %u)",
		crtc, hz, before, mmioRead(otgClk), mmioRead(odmClk), rateBefore, mmioRead(rate), i, j, k);
}

// DMUB firmware trace buffer (CW5, read by checkDmubStall): turn every trace class on before the first
// HDMI pixclk so the stall dump shows what the firmware did last. GPINT = DMCUB_GPINT_DATAIN1
// (status 1 << 28 | code << 16 | param), acked when the status bits clear. -rmbnofwtrace: don't.
bool gNoFwTrace;
constexpr uint32_t DmcubGpintDatain1 = Seg2 + 0x1f8;

static bool dmubGpint(uint32_t code, uint32_t param) {
	uint32_t v = code << 16 | param;
	mmioWrite(DmcubGpintDatain1, 1u << 28 | v);
	for (uint32_t i = 0; i < 1000; i++) {
		if (mmioRead(DmcubGpintDatain1) == v)
			return true;
		IODelay(10);
	}
	return false;
}

// yellow_carp fw 0x400004c keeps one 0x3c-byte state record per PHY at CW6 + 0x2130 (fw pointer 0x60008024 ->
// 0x66000300, + 0x1e30). The SET_PIXEL_CLOCK handler skips the RDPCS SRAM_INIT_DONE wait (0x6000d244, spins
// forever on timeout) when bit 16 of the dword at +8 is set; dump them all to compare with Linux fw_state.
static void dumpFwPhyState(const char *why) {
	uint64_t fbBase = (uint64_t)(mmioRead(0x1260 + 0x16fc) & 0xFFFFFF) << 24;
	uint64_t cw6 = ((uint64_t)mmioRead(Seg2 + 0x1c2) << 32 | mmioRead(Seg2 + 0x1c1)) - fbBase;
	LOG("fwstate: %s: CW6 off %llx base %x", why, cw6, mmioRead(Seg2 + 0x1ab));
	for (uint32_t phy = 0; phy < 5; phy++) {
		uint64_t r = cw6 + 0x2130 + phy * 0x3c;
		uint32_t w[15];
		for (uint32_t i = 0; i < 15; i++)
			w[i] = vramRead64(r + i * 4) & 0xFFFFFFFF;
		LOG("fwstate:   phy%u %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x", phy, w[0],
			w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9], w[10], w[11], w[12], w[13], w[14]);
	}
}

static uint32_t gFwTraceStartW = ~0u;  // OUTBOX0 WPTR when the mask went on (stall dump starts there)

static void enableFwTrace() {
	static bool done;
	if (done || gNoFwTrace)
		return;
	done = true;
	dumpFwPhyState("before late slots / first HDMI pixclk");
	gFwTraceStartW = mmioRead(Seg2 + 0x1da);
	bool a = dmubGpint(102, 0xFFFF), b = dmubGpint(103, 0xFFFF), c = dmubGpint(101, 0);  // MASK_WORD0/1, UPDATE
	LOG("dmub-cmd: fw trace mask all on: acks %d %d %d, SCRATCH7 %x, OUTBOX0 WPTR %x", a, b, c, mmioRead(Seg2 + 0x1ea),
		mmioRead(Seg2 + 0x1da));
}

// Navi23 DAL's clock requests never reach the Rembrandt PMFW, so dispclk/dppclk stay at the boot vote
// (600 MHz, DPPCLK DTOs 1:1). 4K120 4:2:0 (1188 MHz pixel rate) starved the pipe = green flashes on the LG
// (auto-1020-1138). Vote the ext stream's pixel rate + 1% (PMFW max 1200 MHz), never below 600
// (-rmbnodispclk: keep 600).
bool gNoDispClk;

static void voteDispClk(uint32_t pixKhz) {
	static uint32_t cur = 600;
	uint32_t mhz = (uint32_t)(((uint64_t)pixKhz * 101 + 99999) / 100000);
	mhz = mhz < 600 ? 600 : mhz > 1200 ? 1200 : mhz;
	if (gNoDispClk || mhz == cur)
		return;
	uint32_t r1 = vbiosSmcSend(0x4, mhz), g1 = mmioRead(0x16293);  // SetDispclkFreq
	uint32_t r2 = vbiosSmcSend(0x6, mhz), g2 = mmioRead(0x16293);  // SetDppclkFreq
	LOG("dmub-cmd: ext stream %u kHz: dispclk %u -> %u MHz (resp %x granted %u), dppclk (resp %x granted %u)", pixKhz,
		cur, mhz, r1, g1, r2, g2);
	cur = mhz;
}

void fixHdmiPixClk(uint64_t ring, uint32_t s, uint32_t clk) {
	uint32_t w2 = vramRead64(ring + s + 8) & 0xFFFFFFFF, w3 = vramRead64(ring + s + 8) >> 32;
	if (((w2 >> 16) & 0xFF) != 3)  // ATOM_ENCODER_MODE_HDMI
		return;
	enableFwTrace();
	startOtgClock(w3 & 0xFF, clk);
	auto &st = gExtStream;
	if (!st.pixKhz || hdmiClk100Hz(st) != clk) {
		LOG("dmub-cmd: HDMI pixclk %u at [%x] matches no ext stream (last %u kHz enc %u depth %u), left alone", clk, s,
			st.pixKhz, st.enc, st.depth);
		return;
	}
	voteDispClk(st.pixKhz);
	uint32_t ratio = st.depth == 3 ? 1 : st.depth == 4 ? 2 : st.depth == 6 ? 3 : 0;
	uint32_t n2 = w2 | (st.enc == 4 ? 0x04u << 24 : 0), n3 = (w3 & ~0xFF00u) | ratio << 8;
	if (gKeepPixClk) {
		LOG("dmub-cmd: HDMI pixclk %u at [%x] enc %u depth %u (kept: %08x %08x)", clk, s, st.enc, st.depth, w2, w3);
		return;
	}
	vramWrite32(ring + s + 8, n2);
	vramWrite32(ring + s + 12, n3);
	LOG("dmub-cmd: HDMI pixclk %u at [%x] enc %u depth %u: %08x %08x -> %08x %08x", clk, s, st.enc, st.depth, w2, w3, n2, n3);
	LOG("dmub-cmd:   before: PHY B CNTL %x PHY_CNTL0 %x PHY_CNTL6 %x, PHY A CNTL %x", PHYB_REGS);
	if (gPllPdPend)
		LOG("dmub-cmd:   late PLL power-downs never sent (mask %x)", gPllPdPend);
}

// Returns the number of non-NULL commands in DAL's batch [old, newWptr).
uint32_t dropPowerGating(uint32_t old, uint32_t newWptr) {
	if (gKeepPwrGate && gKeepEdpOff && gKeepPllPd && gKeepPixClk && gKeepFams && gKeepAbm)
		return (newWptr - old) % 0x2000 / 64;
	uint64_t fbBase = (uint64_t)(mmioRead(0x1260 + 0x16fc) & 0xFFFFFF) << 24;
	uint64_t ring = ((uint64_t)mmioRead(DmcubCw4OffHi) << 32 | mmioRead(DmcubCw4OffLo)) - fbBase;
	// Who kills PWRSEQ0 (see revivePwrseq0)? Check it before every DAL batch and name the previous one.
	static uint64_t prevCmds[8];
	static uint32_t prevN, prevOld, prevNew, deaths;
	static bool wasAlive;
	bool alive = cpuOwnsPwm();
	if (wasAlive && !alive && deaths++ < 20) {
		LOG("bl: PWRSEQ0 died before DAL batch [%x, %x); previous batch [%x, %x) had %u cmds:", old, newWptr, prevOld,
			prevNew, prevN);
		for (uint32_t i = 0; i < prevN && i < 8; i++)
			LOG("bl:   cmd %u: %016llx", i, prevCmds[i]);
	}
	if (alive)
		savePwrseq0();
	wasAlive = alive;
	prevOld = old;
	prevNew = newWptr;
	prevN = 0;
	uint32_t real = 0;
	for (uint32_t s = old; s != newWptr; s = (s + 64) % 0x2000) {
		uint32_t hdr = vramRead64(ring + s) & 0xFFFFFFFF;
		uint64_t q = vramRead64(ring + s);
		if (prevN < 8)
			prevCmds[prevN] = q;
		prevN++;
		bool edpOff = !gKeepEdpOff && (((hdr & 0xFFFF) == 0x0f80 && ((q >> 32) & 0xFF) == 0x0d) ||
									   ((hdr & 0xFFFF) == 0x0180 && ((q >> 40) & 0xFF) == 0x0d));
		bool edpOn = ((hdr & 0xFFFF) == 0x0f80 && ((q >> 32) & 0xFF) == 0x0c) ||
					 ((hdr & 0xFFFF) == 0x0180 && ((q >> 40) & 0xFF) == 0x0c);
		if (edpOn && !gKeepEdpOn && !gKeepEdpOff) {
			if (gEdpPowered) {
				vramWrite32(ring + s, hdr & ~0xFFFFu);
				static uint32_t loggedOn;
				if (loggedOn++ < 20)
					LOG("dmub-cmd: dropped eDP POWER_ON (sub %x) at [%x] payload %08llx, panel kept powered", (hdr >> 8) & 0xFF, s,
						q >> 32);
				if (sendLatePllPd(ring, s))
					real++;
				continue;
			}
			uint32_t k = (hdr & 0xFFFF) == 0x0f80 ? 0 : 1;
			for (uint32_t i = 0; i < 8; i++)
				gEdpOnCmd[k][i] = vramRead64(ring + s + i * 8);
			gEdpOnSaved |= 1u << k;
			if ((hdr & 0xFFFF) == 0x0180)  // the LVTMA one (sub 0xf) comes first; both make up one power-up
				gEdpPowered = true;
			gEdpOffDone = false;
			gPanelOffOk = false;
		}
		if (!gKeepFams && (hdr & 0xFF) == 0x4c) {
			vramWrite32(ring + s, hdr & ~0xFFFFu);
			static uint32_t loggedFams;
			if (loggedFams++ < 20)
				LOG("dmub-cmd: dropped type 4c (FAMS) at [%x] payload %08llx %08llx", s, q >> 32, vramRead64(ring + s + 8));
			if (sendLatePllPd(ring, s))
				real++;
			continue;
		}
		if ((hdr & 0xFF) == 0x42) {
			static uint32_t loggedAbm;
			if (loggedAbm++ < 40)
				LOG("bl: DMUB ABM sub %x at [%x] payload %08llx %08llx%s", (hdr >> 8) & 0xFF, s, q >> 32, vramRead64(ring + s + 8),
					gKeepAbm ? " (passed)" : "");
			// No revive here: this runs inside DAL's display power-down/up sequence (auto-1022-1243 #10).
			bool fwPwm = !gNoFwBl && !cpuOwnsPwm();
			if (fwPwm && ((hdr & 0xFFFF) == 0x0242 || (hdr & 0xFFFF) == 0x0142)) {
				static uint32_t loggedFw;
				if (loggedFw++ < 10)
					LOG("bl: passed ABM sub %x at [%x] (firmware owns the PWM)", (hdr >> 8) & 0xFF, s);
			} else if (!gKeepAbm && (hdr & 0xFFFF) == 0x0242) {
				vramWrite32(ring + s, hdr & ~0xFFFFu);
				blRampSync(vramRead64(ring + s + 8) & 0xFFFFFFFF);
				setBacklight(vramRead64(ring + s + 8) & 0xFFFFFFFF);
				if (sendLatePllPd(ring, s))
					real++;
				continue;
			}
			// ABM SET_PIPE (sub 1) hands the panel to the firmware: from then on the whole PWRSEQ0 block
			// reads 0 and setBacklight can't work (auto-1021-1540: OK at +100 s, SET_PIPE at +101 s;
			// auto-1021-1621: the 0xff disable alone passed and still 0 ever after). Drop every option.
			uint32_t pipeOpt = (q >> 48) & 0xFF;
			if (!fwPwm && !gKeepAbm && (hdr & 0xFFFF) == 0x0142) {
				vramWrite32(ring + s, hdr & ~0xFFFFu);
				static uint32_t loggedPipe;
				if (loggedPipe++ < 10)
					LOG("bl: dropped ABM SET_PIPE option %x at [%x], PWM stays with the CPU", pipeOpt, s);
				if (sendLatePllPd(ring, s))
					real++;
				continue;
			}
		}
		if ((hdr & 0xFFFF) == 0x0280 && (q >> 32) != 0)
			fixHdmiPixClk(ring, s, q >> 32);
		if (!gKeepPllPd && (hdr & 0xFFFF) == 0x0280 && (q >> 32) == 0) {
			uint32_t pll = vramRead64(ring + s + 8) & 0xFF;
			// Waking DMCUB with every OTG off kills PHY A (see the hold below), so only while one runs.
			bool otgOn = anyOtgOn();
			// PLL 22/23 = COMBOPHY_PLL2/3 = PHY C/D, which this B0 part muxes to F/G = the USB-C ports. The
			// late replay of the PLL 22 power-down landed 23 ms after the USB-C stream went on and the sink
			// lost CR on lane 0 at once (usbc run 10-27 16:40). Drop them for good. -rmbusbcpllpd: old way (also
			// replays the others on USB-C, see sendLatePllPd).
			bool usbcPll = !gUsbcPllPd && (pll == 22 || pll == 23);
			if (pll == 20 || usbcPll || !otgOn || (gDropAllPllPd && pll >= 20 && pll <= 25)) {
				if (!gNoPllPdLate && !gDropAllPllPd && !usbcPll && pll > 20 && pll <= 25) {
					for (uint32_t k = 0; k < 8; k++)
						gPllPdCmd[pll - 21][k] = vramRead64(ring + s + k * 8);
					gPllPdPend |= 1u << (pll - 21);
				}
				vramWrite32(ring + s, hdr & ~0xFFFFu);
				static uint32_t loggedPll;
				if (loggedPll++ < 20)
					LOG("dmub-cmd: dropped PHY PLL %u power-down at [%x]%s", pll, s, otgOn ? "" : " (no OTG on)");
				continue;
			}
			if (pll > 20 && pll <= 25) {
				static uint32_t loggedPass;
				if (loggedPass++ < 20)
					LOG("dmub-cmd: passed PHY PLL %u power-down at [%x] (%08llx %08llx); PHY B CNTL %x PHY_CNTL0 %x PHY_CNTL6 %x, PHY A CNTL %x",
						pll, s, vramRead64(ring + s + 8) & 0xFFFFFFFF, vramRead64(ring + s + 8) >> 32, PHYB_REGS);
			}
		}
		if (edpOff && gPanelOffOk) {
			gEdpPowered = false;
			gEdpOffDone = true;
			LOG("dmub-cmd: passed eDP POWER_OFF (sub %x) at [%x] payload %08llx (shutdown/sleep)", (hdr >> 8) & 0xFF, s, q >> 32);
			real++;
			continue;
		}
		if (edpOff) {
			uint32_t k = (hdr & 0xFFFF) == 0x0f80 ? 0 : 1;
			for (uint32_t i = 0; i < 8; i++)
				gEdpOffCmd[k][i] = vramRead64(ring + s + i * 8);
			gEdpOffSaved |= 1u << k;
			vramWrite32(ring + s, hdr & ~0xFFFFu);
			static uint32_t loggedOff;
			if (loggedOff++ < 20)
				LOG("dmub-cmd: dropped eDP POWER_OFF (sub %x) at [%x] payload %08llx", (hdr >> 8) & 0xFF, s, q >> 32);
			continue;
		}
		if ((hdr & 0xFFFF) == 0x0180 && gDcnCold) {
			gDcnCold = false;
			LOG("dmub-cmd: first transmitter cmd after power loss at [%x], PG filter back on", s);
		}
		if (!gKeepPwrGate && (hdr & 0xFFFF) == 0x0380 && gDcnCold) {
			LOG("dmub-cmd: passed ENABLE_DISP_POWER_GATING at [%x] payload %08llx (cold DCN)", s, q >> 32);
		} else if (!gKeepPwrGate && (hdr & 0xFFFF) == 0x0380) {
			vramWrite32(ring + s, hdr & ~0xFFFFu);
			static uint32_t logged;
			if (logged++ < 20)
				LOG("dmub-cmd: dropped ENABLE_DISP_POWER_GATING at [%x] payload %08llx", s, vramRead64(ring + s) >> 32);
			continue;
		}
		if (hdr & 0xFF)
			real++;
	}
	return real;
}

// The PHY still dies while the firmware is processing one of the NULLed PLL power-downs in
// commit_streams (odmsrc2 at [940], raw1/dcc1 at [980]): with every OTG off, waking DMCUB at all
// lets it power PHY A down. So don't ring the doorbell for NULL-only batches: keep the hardware
// WPTR where it is, remember DAL's, and report DAL's WPTR as RPTR once the firmware has caught up.
// The held NULLs go out with the next real command. -rmbnohold: always submit.
bool gNoHold;
uint32_t gDalWptr = ~0u;

bool inboxHeld() { return gDalWptr != ~0u && gDalWptr != mmioRead(DmcubInbox1Wptr); }

// Our own DMUB commands (dmubInject). DAL's dmub_srv keeps its WPTR in software, so it can't learn
// about them: held (hardware WPTR behind DAL's, the slots between are all NULL) → the cmd takes the
// first held slot; else it goes at the hardware WPTR and the hardware runs gAhead slots ahead of
// DAL. DAL's next batch then starts on top of those consumed slots: realignInbox moves its real cmds
// up behind them (wrapRead keeps reporting DAL's own WPTR). gInboxLock serializes with DAL's WPTR
// writes; DMCUB reload resets the state (inboxReset).
IOLock *gInboxLock;
uint32_t gAhead;
uint64_t gRealign[128][8];

uint64_t inboxRing() {
	uint64_t fbBase = (uint64_t)(mmioRead(0x1260 + 0x16fc) & 0xFFFFFF) << 24;
	return ((uint64_t)mmioRead(DmcubCw4OffHi) << 32 | mmioRead(DmcubCw4OffLo)) - fbBase;
}

void inboxResetState() {
	gAhead = 0;
	gPanelCntlInit = false;
}

bool dmubInject(const uint32_t *cmd, uint32_t *reply) {
	if (!gInboxLock || !mmioAvailable())
		return false;
	IOLockLock(gInboxLock);
	uint64_t ring = inboxRing();
	uint32_t w = mmioRead(DmcubInbox1Wptr), nw = (w + 64) % 0x2000;
	bool held = !gAhead && inboxHeld();
	for (uint32_t i = 0; i < 16; i++)
		vramWrite32(ring + w + i * 4, cmd[i]);
	if (!held) {
		if (gDalWptr == ~0u)
			gDalWptr = w;
		gAhead++;
	}
	mmioWrite(DmcubInbox1Wptr, nw);
	uint32_t ms = 0;
	while (mmioRead(DmcubInbox1Rptr) != nw && ms < 100) {
		IODelay(1000);
		ms++;
	}
	bool ok = mmioRead(DmcubInbox1Rptr) == nw;
	if (reply)
		for (uint32_t i = 0; i < 16; i += 2) {
			uint64_t q = vramRead64(ring + w + i * 4);
			reply[i] = q & 0xFFFFFFFF;
			reply[i + 1] = q >> 32;
		}
	static uint32_t logged;
	if (logged++ < 20 || !ok)
		LOG("dmub-inject: type %x sub %x at [%x]%s, rptr %x after %u ms, ahead %u", cmd[0] & 0xFF, (cmd[0] >> 8) & 0xFF, w,
			held ? " (held slot)" : "", mmioRead(DmcubInbox1Rptr), ms, gAhead);
	IOLockUnlock(gInboxLock);
	return ok;
}

// Panel PWM owned by the fw (see gBlSaved): PANEL_CNTL HW_INIT with the saved PERIOD/REF_DIVs and a
// BL_PWM_CNTL duty computed for this level, on every set (the fw writes the regs it is given). auto-1022-1140:
// one HW_INIT with the saved (display-sleep dimmed, 2%) duty + ABM SET_BACKLIGHT 0x75ae left the panel at 2%
// = SET_BACKLIGHT does nothing without ABM INIT_CONFIG (Linux sends one, Navi DAL doesn't).
// -rmbfwabmset: also send ABM SET_BACKLIGHT (Linux dmub_abm_set_backlight: ramp 0, level u16.16, version 1,
// panel mask 1).
bool gFwAbmSet;
void dmubBacklight(uint32_t level) {
	uint32_t c[16] = {}, rep[16];
	uint32_t bits = gBlSaved.period >> 16 & 0x1f;
	if (!bits || bits > 16)
		bits = 16;
	uint32_t period = gBlSaved.period & ((1u << bits) - 1);
	uint32_t active = ((uint64_t)period * level >> 16) << (16 - bits);
	if (active > 0xffff)
		active = 0xffff;
	uint32_t cntl = (gBlSaved.cntl & ~0xffffu) | active | 0x80000000;
	c[0] = 0x2000004a;  // DMUB_CMD__PANEL_CNTL, HW_INIT, 32 payload bytes
	c[3] = cntl;
	c[4] = gBlSaved.period;
	c[5] = gBlSaved.refDiv1;
	c[6] = 3;  // is_backlight_on | is_powered_on
	c[7] = gBlSaved.refDiv2;
	bool ok = dmubInject(c, rep);
	if (ok)
		gBlSaved.cntl = cntl;
	static uint32_t logged;
	if (logged++ < 40 || !ok || !gPanelCntlInit)
		LOG("bl: level %x via panel-cntl HW_INIT %s: sent cntl %x period %x ref_div1 %x ref_div2 %x; reply cur %x cntl %x period %x "
			"ref_div1 %x flags %x",
			level, ok ? "ok" : "FAILED", c[3], c[4], c[5], c[7], rep[2], rep[3], rep[4], rep[5], rep[6] & 0xFF);
	gPanelCntlInit = true;
	if (!gFwAbmSet)
		return;
	for (uint32_t &x : c)
		x = 0;
	c[0] = 0x20000242;  // DMUB_CMD__ABM, SET_BACKLIGHT
	c[2] = level;
	c[3] = 0x101;
	ok = dmubInject(c, nullptr);
	if (logged < 40 || !ok)
		LOG("bl: level %x via DMUB ABM SET_BACKLIGHT%s", level, ok ? "" : " FAILED");
}

// DAL wrote WPTR = v for its batch [gDalWptr, v) (already filtered in place), while the hardware is
// gAhead slots further. Copy the batch's real cmds to the hardware WPTR; pad with NULLs up to v if
// that ends behind it. Returns the hardware WPTR to write, or ~0u for "nothing to submit".
uint32_t realignInbox(uint32_t v) {
	uint64_t ring = inboxRing();
	uint32_t p = mmioRead(DmcubInbox1Wptr), n = 0, ahead0 = gAhead;
	for (uint32_t s = gDalWptr; s != v && n < 128; s = (s + 64) % 0x2000) {
		if (!(vramRead64(ring + s) & 0xFF))
			continue;
		for (uint32_t k = 0; k < 8; k++)
			gRealign[n][k] = vramRead64(ring + s + k * 8);
		n++;
	}
	uint32_t q = p;
	for (uint32_t i = 0; i < n; i++, q = (q + 64) % 0x2000)
		for (uint32_t k = 0; k < 16; k++)
			vramWrite32(ring + q + k * 4, (gRealign[i][k / 2] >> (k % 2 * 32)) & 0xFFFFFFFF);
	uint32_t ahead = (q - v + 0x2000) % 0x2000;
	if (ahead < 0x1000) {  // still at or past DAL's WPTR
		gAhead = ahead / 64;
	} else if (!n) {  // behind: only NULLs, which [q, v) already are → plain held state
		gAhead = 0;
	} else {
		for (; q != v; q = (q + 64) % 0x2000)
			for (uint32_t k = 0; k < 16; k++)
				vramWrite32(ring + q + k * 4, 0);
		gAhead = 0;
	}
	static uint32_t logged;
	if (logged++ < 20)
		LOG("dmub-inject: realigned DAL batch [%x, %x) (%u real) to hw [%x, %x), ahead %u -> %u", gDalWptr, v, n, p, q, ahead0,
			gAhead);
	return n ? q : ~0u;
}

// After the first DMUB transmitter ENABLE (VBIOS type 0x80 sub 1, action byte 1), log the next DAL
// register writes: shows how Navi23 DAL programs DIG/DP/DCCG around link training.
uint32_t gWriteLogLeft;

void logInboxWrite(uint32_t newWptr) {
	static uint32_t lines;
	static bool armed;
	uint32_t old = mmioRead(DmcubInbox1Wptr);
	uint64_t fbBase = (uint64_t)(mmioRead(0x1260 + 0x16fc) & 0xFFFFFF) << 24;
	uint64_t ring = ((uint64_t)mmioRead(DmcubCw4OffHi) << 32 | mmioRead(DmcubCw4OffLo)) - fbBase;
	if (lines >= 400)
		return;
	LOG("dmub-cmd: wptr %x -> %x (rptr %x) CNTL %x CNTL2 %x SCRATCH0 %x", old, newWptr, mmioRead(DmcubInbox1Rptr),
		mmioRead(DmcubCntl), mmioRead(DmcubCntl2), mmioRead(DmcubScratch0));
	lines++;
	for (uint32_t s = old; s != newWptr && lines < 400; s = (s + 64) % 0x2000, lines++) {
		uint64_t a = vramRead64(ring + s), b = vramRead64(ring + s + 8);
		if (!armed && mmioRead(DmcubCntl2) == 0 && (a & 0xFFFF) == 0x0180 && ((a >> 40) & 0xFF) == 1) {
			armed = true;
			gWriteLogLeft = 800;
			LOG("wlog: transmitter ENABLE at [%x], logging next %u DAL writes", s, gWriteLogLeft);
		}
		LOG("dmub-cmd:   [%x] type %llx sub %llx payload %08llx %08llx %08llx", s, a & 0xFF, (a >> 8) & 0xFF, a >> 32,
			b & 0xFFFFFFFF, b >> 32);
	}
}


// USB-C (DP-alt) link training diag: every DMUB VBIOS command (type 0x80) except eDP (phy 0)
// transmitter control, decoded like Linux logs/linux-usbc-dmub.txt (sub 1 = DIG transmitter
// control v1.7: phyid, action (1 ENABLE, 0xb SET_VOLTAGE_AND_PREEMPASIS), laneset, lanenum,
// symclk 10 kHz, hpdsel/digfe/connobj). Linux on the same port needs VS1 to get clock recovery.
// Always on (300 lines); -rmbnousbclog off (gUsbcLog, defined with usbcTraceWrite above).

void retimerTxEnable(uint32_t newWptr);

void logTxDiag(uint32_t newWptr) {
	static uint32_t lines;
	if (!gUsbcLog || lines >= 300)
		return;
	uint32_t old = mmioRead(DmcubInbox1Wptr);
	uint64_t fbBase = (uint64_t)(mmioRead(0x1260 + 0x16fc) & 0xFFFFFF) << 24;
	uint64_t ring = ((uint64_t)mmioRead(DmcubCw4OffHi) << 32 | mmioRead(DmcubCw4OffLo)) - fbBase;
	for (uint32_t s = old; s != newWptr && lines < 300; s = (s + 64) % 0x2000) {
		uint64_t a = vramRead64(ring + s), b = vramRead64(ring + s + 8), c = vramRead64(ring + s + 16);
		if ((a & 0xFF) != 0x80)
			continue;
		uint32_t c1 = a >> 32;
		if (((a >> 8) & 0xFF) == 1) {
			if ((c1 & 0xFF) == 0)
				continue;
			lines++;
			LOG("usbc: [%x] TX phy %u action %x laneset %02x lanes %u symclk %llu0 kHz c3 %08llx c4 %08llx", s, c1 & 0xFF,
				(c1 >> 8) & 0xFF, (c1 >> 16) & 0xFF, c1 >> 24, b & 0xFFFFFFFF, b >> 32, c & 0xFFFFFFFF);
		} else {
			lines++;
			LOG("usbc: [%x] vbios sub %llx payload %08x %08llx %08llx %08llx", s, (a >> 8) & 0xFF, c1, b & 0xFFFFFFFF,
				b >> 32, c & 0xFFFFFFFF);
		}
	}
}

// DAL waits for DMUB idle after each command, so a new submission while RPTR still trails the
// hardware WPTR means the firmware is stuck on an earlier one. Once per boot: dump the last DAL
// register accesses and the PHY A/B, DCCG, DIG1 and DMCUB state, to see what it is waiting for.
void checkDmubStall() {
	static bool dumped;
	static uint32_t lastRp = ~0u;
	static uint64_t sinceUs;
	uint32_t rp = mmioRead(DmcubInbox1Rptr), wp = mmioRead(DmcubInbox1Wptr);
	if (dumped || rp == wp)
		return;
	if (rp != lastRp) {  // only a RPTR that hasn't moved for 200 ms counts (NO_WAIT cmds can trail)
		lastRp = rp;
		sinceUs = nowUs();
		return;
	}
	if (nowUs() - sinceUs < 200000)
		return;
	dumped = true;
	uint64_t fbBase = (uint64_t)(mmioRead(0x1260 + 0x16fc) & 0xFFFFFF) << 24;
	uint64_t ring = ((uint64_t)mmioRead(DmcubCw4OffHi) << 32 | mmioRead(DmcubCw4OffLo)) - fbBase;
	uint64_t a = vramRead64(ring + rp), b = vramRead64(ring + rp + 8);
	LOG("stall: DMUB stuck at [%x] (wptr %x) type %llx sub %llx payload %08llx %08llx %08llx", rp, wp, a & 0xFF, (a >> 8) & 0xFF,
		a >> 32, b & 0xFFFFFFFF, b >> 32);
	traceDump("DMUB stalled");
	constexpr uint32_t Seg1 = 0xC0;
	static const struct {
		const char *name;
		uint32_t reg;
	} regs[] = {
		{"DMCUB_CNTL", Seg2 + 0x1f6}, {"DMCUB_SCRATCH1", Seg2 + 0x1e4}, {"DMCUB_SCRATCH4", Seg2 + 0x1e7},
		{"DMCUB_TIMER_CURRENT", Seg2 + 0x1fd}, {"DMCUB_INST_FETCH_FAULT_ADDR", Seg2 + 0x1cc},
		{"DMCUB_DATA_WRITE_FAULT_ADDR", Seg2 + 0x1cd}, {"PHYASYMCLK_CLOCK_CNTL", Seg2 + 0x52},
		{"PHYBSYMCLK_CLOCK_CNTL", Seg2 + 0x53}, {"DCCG_GATE_DISABLE_CNTL", Seg1 + 0x74}, {"DCCG_GATE_DISABLE_CNTL2", Seg1 + 0x7c},
		{"OTG1_PIXEL_RATE_CNTL", Seg1 + 0x84}, {"SYMCLKB_CLOCK_ENABLE", Seg1 + 0xa1}, {"DIO_LINKB_CNTL", Seg2 + 0x1f05},
		{"DIG1_DIG_FE_CNTL", Seg2 + 0x218b}, {"DIG1_HDMI_CONTROL", Seg2 + 0x2193}, {"DIG1_DIG_BE_CNTL", Seg2 + 0x21b0},
		{"DOMAIN0_PG_STATUS", Seg2 + 0x81}, {"DOMAIN1_PG_STATUS", Seg2 + 0x83}, {"DOMAIN2_PG_STATUS", Seg2 + 0x85},
		{"DOMAIN3_PG_STATUS", Seg2 + 0x87},
	};
	for (auto &e : regs)
		LOG("stall:   %s (%x) = %x", e.name, e.reg, mmioRead(e.reg));
	for (uint32_t i = 0; i < 16; i += 4)
		LOG("stall:   SCRATCH%u-%u = %x %x %x %x", i, i + 3, mmioRead(Seg2 + 0x1e3 + i), mmioRead(Seg2 + 0x1e4 + i),
			mmioRead(Seg2 + 0x1e5 + i), mmioRead(Seg2 + 0x1e6 + i));
	// Trace buffer: CW5 = 16-byte header (entry_count) + ring of {code, tick, param0, param1}; OUTBOX0 = that ring.
	uint64_t tb = ((uint64_t)mmioRead(Seg2 + 0x1c0) << 32 | mmioRead(Seg2 + 0x1bf)) - fbBase;
	uint32_t cw5Size = (mmioRead(Seg2 + 0x1b2) & 0x1FFFFFFF) - mmioRead(Seg2 + 0x1aa), obSize = mmioRead(Seg2 + 0x1d9),
			 obW = mmioRead(Seg2 + 0x1da), count = vramRead64(tb) & 0xFFFFFFFF;
	LOG("stall:   CW5 off %llx size %x (TOP raw %x) OUTBOX0 base %x size %x wptr %x rptr %x, header %llx %llx", tb,
		cw5Size, mmioRead(Seg2 + 0x1b2), mmioRead(Seg2 + 0x1d8), obSize, obW, mmioRead(Seg2 + 0x1db), vramRead64(tb),
		vramRead64(tb + 8));
	uint32_t maxEnt = obSize >= 16 && obSize < 0x1000000 ? obSize / 16 : 0;
	if (maxEnt) {
		uint32_t last = obW / 16;  // next entry to be written
		uint32_t n = gFwTraceStartW == ~0u ? 48 : (obW + obSize - gFwTraceStartW) % obSize / 16;
		if (n > 256 || n == 0)
			n = n ? 256 : 48;
		for (uint32_t k = n; k >= 1; k--) {
			uint32_t idx = (last + maxEnt - k) % maxEnt;
			uint64_t lo = vramRead64(tb + 16 + idx * 16), hi = vramRead64(tb + 16 + idx * 16 + 8);
			if (lo || hi)
				LOG("stall:   trace[%u] code %08llx tick %08llx p0 %08llx p1 %08llx", idx, lo & 0xFFFFFFFF, lo >> 32,
					hi & 0xFFFFFFFF, hi >> 32);
		}
		LOG("stall:   (header entry_count %u)", count);
	}
	dumpFwPhyState("stall");
	static const uint32_t phyBlocks[] = {Seg2 + 0x2928, Seg2 + 0x2a00};  // RDPCSTX0 / RDPCSTX1
	for (uint32_t base : phyBlocks)
		for (uint32_t r = base; r < base + 0x48; r++)
			if (!isPhyHole(r))
				LOG("stall:   phy %x = %x", r, mmioRead(r));
}

uint64_t wrapWrite(uint64_t helper, uint64_t reg, uint64_t val, uint64_t d, uint64_t e, uint64_t f) {
	uint32_t r = reg & 0xFFFFFFFF;
	eoi0Note(r | 0x40000000, val & 0xFFFFFFFF);  // bit 30 = DAL write
	if (r >= PgFirst && r <= PgLast) {
		gPgShadow[r - PgFirst] = val & 0xFFFFFFFF;
		forceDomainsOn();
		return 0;
	}
	if (!gNaviSmu && (r == NaviDalSmuMsg || r == NaviDalSmuArg || r == NaviDalSmuResp)) {
		if (r == NaviDalSmuArg)
			gNaviSmuArg = val & 0xFFFFFFFF;
		if (r == NaviDalSmuMsg && gNaviSmuMsgs++ < 20)
			LOG("dalsmu: Navi DAL SMU msg %llx arg %x kept off MP1 C2PMSG_74/51 (hw C2PMSG_52 %x), answered UnknownCmd %x",
				val & 0xFFFFFFFF, gNaviSmuArg, mmioRead(NaviDalSmuResp), DalsmcResultUnknownCmd);
		return 0;
	}
	bool inboxLocked = false;
	if (r == DmcubInbox1Wptr) {
		if (gInboxLock) {
			IOLockLock(gInboxLock);
			inboxLocked = true;
		}
		checkDmubStall();
		uint32_t v = val & 0xFFFFFFFF;
		uint32_t real = dropPowerGating(gAhead ? gDalWptr : mmioRead(DmcubInbox1Wptr), v);
		if (gAhead) {
			uint32_t hwW = realignInbox(v);
			gDalWptr = v;
			if (hwW != ~0u)
				mmioWrite(DmcubInbox1Wptr, hwW);
			IOLockUnlock(gInboxLock);
			return 0;
		}
		gDalWptr = v;
		if (!gNoHold && real == 0) {
			static uint32_t logged;
			if (logged++ < 20)
				LOG("dmub-cmd: held NULL-only batch, DAL wptr %x, hw wptr %x", gDalWptr, mmioRead(DmcubInbox1Wptr));
			if (inboxLocked)
				IOLockUnlock(gInboxLock);
			return 0;
		}
		retimerTxEnable(v);
		logTxDiag(v);
		if (rmbVerbose())
			logInboxWrite(v);
	}
	uint32_t hw = remap(r, true);
	auxDalTouch(hw);
	if (hw >= BlPwmCntl && hw <= BlPwmLock) {
		static uint32_t loggedBlWr;
		if (loggedBlWr++ < 60)
			LOG("bl: DAL write %x = %llx (PWRSEQ0 %s)", hw, val & 0xFFFFFFFF, cpuOwnsPwm() ? "alive" : "dead");
	}
	if (!gKeepPhyWrites && hw >= PhyFirst && hw <= PhyLast) {
		static uint32_t logged;
		if (logged++ < 60 && rmbVerbose())
			LOG("phy: dropped DAL write reg %x (hw %x) = %llx, now %x", r, hw, val & 0xFFFFFFFF,
				isPhyHole(hw) ? 0xdeadu : mmioRead(hw));
		else if (gUsbcLog && hw >= PhyBFirst && logged < 300)  // PHY B-E: no CPU read
			LOG("usbc: dropped DAL PHY write reg %x (hw %x) = %llx", r, hw, val & 0xFFFFFFFF);
		return 0;
	}
	if (gWriteLogLeft) {
		gWriteLogLeft--;
		auto m = find(r);
		LOG("wlog: %x (hw %x)%s%s = %llx (was %x)", r, hw, m ? " " : "", m ? m->name : "", val & 0xFFFFFFFF, mmioRead(hw));
	}
	if (auto k = fieldKind(hw); k != Fields::None)
		val = fieldsToHw(k, hw, val & 0xFFFFFFFF, mmioRead(hw));
	if (!gKeepDto && isDtoModulo(hw) && (val & 0xFFFFFFFF)) {
		uint32_t m = static_cast<uint32_t>((val & 0xFFFFFFFF) * RmbDtoRefKhz / NaviDtoRefKhz);
		static uint32_t logged;
		if (logged++ < 20)
			LOG("dcn: DP DTO modulo %x: %llu -> %u", hw, val & 0xFFFFFFFF, m);
		val = m;
	}
	if (!gKeepStutter && hw == DramStateCntl) {
		static uint32_t logged;
		if (logged++ < 5)
			LOG("dcn: self-refresh disallowed (DAL wrote %llx)", val & 0xFFFFFFFF);
		val = (val & ~static_cast<uint64_t>(SrForceValue)) | SrForceEnable;
	}
	if (!gNoAperture && isSurfAddr(hw))
		fixAperture((hw - SurfAddr0) / 0xdc);
	if (!gNoDet && isSurfAddr(hw))
		fixDet((hw - SurfAddr0) / 0xdc);
	if (!gKeepDcc && isSurfCtl(hw) && (val & SurfCtlDcc)) {
		static uint32_t logged;
		if (logged++ < 20)
			LOG("dcn: cleared DCC in surface control %x: %llx", hw, val & 0xFFFFFFFF);
		val &= ~static_cast<uint64_t>(SurfCtlDcc);
	}
	val = usbcSdpFix(hw, val);
	phyEnter(false, r);
	traceAccess(false, r, hw, val & 0xFFFFFFFF);
	usbcTraceWrite(hw, val & 0xFFFFFFFF);
	gDalThread = current_thread();
	pwrseqWatch(true, "DAL write", hw, val & 0xFFFFFFFF);
	auto ret = reinterpret_cast<Fn6>(orgWrite)(helper, hw, val, d, e, f);
	pwrseqWatch(false, "DAL write", hw, val & 0xFFFFFFFF);
	gDalThread = nullptr;
	if (inboxLocked)
		IOLockUnlock(gInboxLock);
	phyExit(false, r, hw, val & 0xFFFFFFFF);
	return ret;
}

// Everything else in the framebuffer (IRQ/HPD handlers, link services, ...) reaches MMIO through
// AmdRegisterAccess::hwReadReg32 / readValidateReg32 / writeReg32 directly, with Navi23 offsets and
// no remap. PHY A still dies asynchronously during commit_streams (odmsrc2, dcc1) with no DAL access
// at fault; guard those paths too: never touch PHY holes, never write the PHY block, and check
// RDPCSTX0 around each access. Log the caller as an offset in the extracted kext (__TEXT 0xbfe6000).
constexpr mach_vm_address_t FbTextBase = 0xbfe6000;
mach_vm_address_t gFbText, orgHwRead, orgValRead, orgRawWrite;

uint32_t callerOff(void *ra) { return static_cast<uint32_t>(reinterpret_cast<mach_vm_address_t>(ra) - gFbText + FbTextBase); }

// AmdRadeonControllerNavi2::setupBootWatermarks copies DCHUBBUB watermark set A to B/C/D with raw
// DCN 3.0.2 offsets (on 3.1.2 they hit other watermarks and DCHUBBUB_TEST_DEBUG_INDEX/DATA): remap.
// Located by symbol (15.0 0xbfea1d2, 15.8.1 at another offset); same 0x36e-byte body in both.
constexpr uint32_t BootWmSize = 0x36e;
uint32_t gBootWmFirst, gBootWmLast;
bool isNaviRawCaller(uint32_t caller) { return gBootWmFirst && caller >= gBootWmFirst && caller <= gBootWmLast; }

bool isDisplayReg(uint32_t r) { return (r >= 0xc0 && r < 0x1260) || (r >= Seg2 && r < 0x8000); }

void logRawAccess(bool rd, uint32_t r, uint32_t val, uint32_t caller) {
	static uint64_t seen[256];
	static uint32_t n;
	uint64_t key = (uint64_t)r << 32 | caller;
	for (uint32_t i = 0; i < n; i++)
		if (seen[i] == key)
			return;
	if (n >= 256)
		return;
	seen[n++] = key;
	auto m = find(r);
	LOG("raw: %s %x%s%s = %x from fb+%x", rd ? "rd" : "wr", r, m ? " " : "", m ? m->name : "", val, caller);
}

bool rawBlocked(bool rd, uint32_t r, uint32_t caller) {
	if (gKeepPhyWrites || !(rd ? isPhyHole(r) : (r >= PhyFirst && r <= PhyLast)))
		return false;
	static uint32_t logged;
	if (logged++ < 60)
		LOG("phy: skipped non-DAL %s of reg %x from fb+%x", rd ? "read" : "write", r, caller);
	return true;
}

void rawPhyCheck(bool before, bool rd, uint32_t r, uint32_t val, uint32_t caller) {
	if (!phyPollOn())
		return;
	bool alive = mmioRead(PhyA) != 0;
	static uint32_t logged;
	if (gPhyAlive && !alive && logged++ < 5) {
		LOG("phy: RDPCSTX0 died %s non-DAL %s reg %x val %x from fb+%x", before ? "before" : "right after", rd ? "read" : "write", r,
			val, caller);
		traceDump("died around a non-DAL access");
	}
	gPhyAlive = alive;
}

template <mach_vm_address_t &Org>
uint64_t rawReadImpl(uint64_t self, uint64_t reg, void *ra) {
	uint32_t r = reg & 0xFFFFFFFF;
	if (gDalThread == current_thread() || !isDisplayReg(r))
		return reinterpret_cast<uint64_t (*)(uint64_t, uint64_t)>(Org)(self, reg);
	uint32_t caller = callerOff(ra);
	if (isNaviRawCaller(caller))
		reg = r = remap(r, false);
	if (rawBlocked(true, r, caller))
		return 0;
	rawPhyCheck(true, true, r, 0, caller);
	uint64_t v = reinterpret_cast<uint64_t (*)(uint64_t, uint64_t)>(Org)(self, reg);
	traceAccess(true, r | 0x80000000, caller, v & 0xFFFFFFFF);
	rawPhyCheck(false, true, r, v & 0xFFFFFFFF, caller);
	logRawAccess(true, r, v & 0xFFFFFFFF, caller);
	return v;
}

// Core Services window (kern_start.cpp coreTraceActive): log + flush before the access, value after.
template <mach_vm_address_t &Org>
uint64_t wrapRawRead(uint64_t self, uint64_t reg) {
	void *ra = __builtin_return_address(0);
	if (!coreTraceActive())
		return rawReadImpl<Org>(self, reg, ra);
	const char *kind = &Org == &orgValRead ? "valrd" : "hwrd";
	coreTraceAccess(kind, reg & 0xFFFFFFFF, 0, ra);
	uint64_t v = rawReadImpl<Org>(self, reg, ra);
	coreTraceResult(kind, reg & 0xFFFFFFFF, v & 0xFFFFFFFF);
	return v;
}

uint64_t rawWriteImpl(uint64_t self, uint64_t reg, uint64_t val, void *ra) {
	uint32_t r = reg & 0xFFFFFFFF;
	eoi0Note(r, val & 0xFFFFFFFF);
	if (gDalThread == current_thread() || !isDisplayReg(r))
		return reinterpret_cast<uint64_t (*)(uint64_t, uint64_t, uint64_t)>(orgRawWrite)(self, reg, val);
	uint32_t caller = callerOff(ra);
	if (isNaviRawCaller(caller))
		reg = r = remap(r, true);
	if (rawBlocked(false, r, caller))
		return 0;
	rawPhyCheck(true, false, r, val & 0xFFFFFFFF, caller);
	traceAccess(false, r | 0x80000000, caller, val & 0xFFFFFFFF);
	pwrseqWatch(true, "raw write", r, val & 0xFFFFFFFF);
	auto ret = reinterpret_cast<uint64_t (*)(uint64_t, uint64_t, uint64_t)>(orgRawWrite)(self, reg, val);
	pwrseqWatch(false, "raw write", r, val & 0xFFFFFFFF);
	rawPhyCheck(false, false, r, val & 0xFFFFFFFF, caller);
	logRawAccess(false, r, val & 0xFFFFFFFF, caller);
	return ret;
}

uint64_t wrapRawWrite(uint64_t self, uint64_t reg, uint64_t val) {
	void *ra = __builtin_return_address(0);
	if (coreTraceActive())
		coreTraceAccess("wr", reg & 0xFFFFFFFF, val & 0xFFFFFFFF, ra);
	return rawWriteImpl(self, reg, val, ra);
}

// VBIOS command tables DAL runs through the CPU AtomBIOS interpreter (MMIO not seen by wrapWrite).
// Navi23 dcn10_link_encoder_hw_init sends DIG1TransmitterControl INIT; Linux DCN31 never does.
mach_vm_address_t orgExecTable;
bool gKeepTxInit;
constexpr uint32_t Dig1TransmitterControl = 77, TxActionInit = 7;

uint64_t wrapExecTable(uint64_t helper, uint64_t index, uint64_t params, uint64_t d, uint64_t e, uint64_t f) {
	uint32_t idx = index & 0xFFFFFFFF;
	auto p = reinterpret_cast<const uint8_t *>(params);
	uint32_t p0 = p ? *reinterpret_cast<const uint32_t *>(p) : 0, p1 = p ? *reinterpret_cast<const uint32_t *>(p + 4) : 0,
			 p2 = p ? *reinterpret_cast<const uint32_t *>(p + 8) : 0;
	if (!gKeepTxInit && idx == Dig1TransmitterControl && p && p[1] == TxActionInit) {
		LOG("atom: skipped table %u (DIG1TransmitterControl INIT) params %08x %08x %08x, PHY %x", idx, p0, p1, p2, phyPeek());
		return 1;
	}
	uint32_t before = phyPeek();
	pwrseqWatch(true, "atom table", idx, p0);
	auto ret = reinterpret_cast<Fn6>(orgExecTable)(helper, index, params, d, e, f);
	pwrseqWatch(false, "atom table", idx, p0);
	LOG("atom: table %u params %08x %08x %08x = %llx, PHY %x -> %x", idx, p0, p1, p2, ret & 0xFF, before, phyPeek());
	return ret;
}

}  // namespace

void edpAllowPowerOff(const char *why) {
	if (gKeepEdpOffAlways || gKeepEdpOff)
		return;
	gPanelOffOk = why != nullptr;
	LOG("dmub-cmd: eDP POWER_OFF %s (%s)", why ? "allowed" : "blocked again", why ? why : "powered on");
}

void edpPowerOffNow(const char *why) {
	if (gKeepEdpOffAlways || gKeepEdpOff)
		return;
	if (gEdpOffDone) {
		LOG("dmub-cmd: eDP already powered off, no replay at %s", why);
		return;
	}
	bool synth = gEdpOffSaved != 3;
	if (synth) {
		if (gEdpOnSaved != 3) {
			LOG("dmub-cmd: no eDP POWER_OFF/ON pair saved, can't power the panel off at %s", why);
			return;
		}
		for (uint32_t k = 0; k < 2; k++)
			for (uint32_t i = 0; i < 8; i++)
				gEdpOffCmd[k][i] = gEdpOnCmd[k][i];
		gEdpOffCmd[0][0] = (gEdpOffCmd[0][0] & ~(0xFFull << 32)) | 0x0dull << 32;  // LVTMA action
		gEdpOffCmd[1][0] = (gEdpOffCmd[1][0] & ~(0xFFull << 40)) | 0x0dull << 40;  // transmitter action
	}
	uint64_t fbBase = (uint64_t)(mmioRead(0x1260 + 0x16fc) & 0xFFFFFF) << 24;
	uint64_t ring = ((uint64_t)mmioRead(DmcubCw4OffHi) << 32 | mmioRead(DmcubCw4OffLo)) - fbBase;
	uint32_t hwW = mmioRead(DmcubInbox1Wptr), w = gDalWptr != ~0u && !gAhead ? gDalWptr : hwW;
	for (uint32_t k = 0; k < 2; k++, w = (w + 64) % 0x2000)
		for (uint32_t i = 0; i < 16; i++)
			vramWrite32(ring + w + i * 4, (gEdpOffCmd[k][i / 2] >> (i % 2 * 32)) & 0xFFFFFFFF);
	mmioWrite(DmcubInbox1Wptr, w);
	if (gAhead)
		gAhead += 2;
	else
		gDalWptr = w;
	uint32_t ms = 0;
	while (mmioRead(DmcubInbox1Rptr) != w && ms < 500) {
		IODelay(1000);
		ms++;
	}
	gEdpPowered = false;
	gEdpOffDone = true;
	LOG("dmub-cmd: replayed eDP POWER_OFF%s at %s: wptr %x -> %x, rptr %x after %u ms", synth ? " (made from POWER_ON)" : "",
		why, hwW, w, mmioRead(DmcubInbox1Rptr), ms);
}

void inboxReset() { inboxResetState(); }

void edpPanelPowerLost() {
	gEdpPowered = false;
	gDcnCold = !gNoColdPg;
}

void traceWrite(uint32_t reg, uint32_t hw, uint32_t val) {
	traceAccess(false, reg, hw, val);
}

void traceDump(const char *why) {
	uint64_t now = nowUs();
	uint32_t n = gTraceN, first = n > TraceLen ? n - TraceLen : 0;
	LOG("trace: %s; last %u of %u DAL accesses (t = us before now):", why, n - first, n);
	for (uint32_t i = first; i < n; i++) {
		auto &e = gTrace[i % TraceLen];
		if (e.reg & 0x80000000) {
			auto m = find(e.reg & 0x7FFFFFFF);
			LOG("trace:   -%llu us %s %x%s%s = %x NON-DAL from fb+%x", now - e.t, e.rd ? "rd" : "wr", e.reg & 0x7FFFFFFF, m ? " " : "",
				m ? m->name : "", e.val, e.hw);
			continue;
		}
		auto m = find(e.reg);
		LOG("trace:   -%llu us %s %x (hw %x)%s%s = %x", now - e.t, e.rd ? "rd" : "wr", e.reg, e.hw, m ? " " : "", m ? m->name : "",
			e.val);
	}
	LOG("trace: DMUB inbox1 rptr %x wptr %x CNTL %x SCRATCH0 %x; MP1 C2PMSG 66/82/90 %x %x %x 67/83/91 %x %x %x 74/51/52 %x %x %x",
		mmioRead(DmcubInbox1Rptr), mmioRead(DmcubInbox1Wptr), mmioRead(DmcubCntl), mmioRead(DmcubScratch0),
		mmioRead(0x16282), mmioRead(0x16292), mmioRead(0x1629a), mmioRead(0x16283), mmioRead(0x16293), mmioRead(0x1629b),
		mmioRead(0x1628a), mmioRead(0x16273), mmioRead(0x16274));
}

void edidRepair(uint64_t link, uint64_t status);

namespace {

// DAL's generic_reg_wait (static C, no symbol): log the waits that time out, with the caller's
// function name and line. Located by offset from the kext's __TEXT (0xbfe6000 in the extracted
// binary), checked against its prologue bytes before routing.
using RegWaitFn = void (*)(void *, uint32_t, uint32_t, uint32_t, uint32_t, unsigned, unsigned, const char *, int);
mach_vm_address_t orgRegWait;
void wrapRegWait(void *ctx, uint32_t addr, uint32_t shift, uint32_t mask, uint32_t value, unsigned delay, unsigned tries,
	const char *fn, int line) {
	reinterpret_cast<RegWaitFn>(orgRegWait)(ctx, addr, shift, mask, value, delay, tries, fn, line);
	if (addr >= PgFirst && addr <= PgLast)
		return;  // shadowed: DAL saw the state it asked for
	auto m = find(addr);
	uint32_t real = m ? m->to : addr;
	uint32_t v = mmioRead(real);
	if (((v & mask) >> shift) != value) {
		static uint32_t logged;
		if (logged++ < 40)
			LOG("dcn: REG_WAIT timeout %s:%d reg %x%s%s (hw %x) = %x, want field(mask %x) == %x", fn ? fn : "?", line, addr,
				m ? " " : "", m ? m->name : "", real, v, mask, value);
	}
}

// DAL's dm_logger_write(logger, major, fmt, ...) drops messages whose major isn't enabled in the
// logger mask; print everything (first 6000 lines) to serial instead.
void wrapDalLog(void *, int major, const char *fmt, ...) {
	static uint32_t lines;
	if (!fmt)
		return;
	// dc_commit_streams logs each stream as "pix_clk_khz: .., pixelencoder:.., displaycolorDepth:.."
	// then "link: N"; keep the last non-eDP one for fixHdmiPixClk.
	bool streamLine = strstr(fmt, "pixelencoder") != nullptr, linkLine = !streamLine && strstr(fmt, "link: %d") != nullptr;
	if (!streamLine && !linkLine && (lines >= 6000 || !rmbVerbose()))
		return;
	char buf[400];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (streamLine) {
		auto num = [&](const char *key) -> uint32_t {
			const char *p = strstr(buf, key);
			return p ? (uint32_t)strtoul(p + strlen(key), nullptr, 10) : 0;
		};
		gPendStream = {num("pix_clk_khz: "), num("pixelencoder:"), num("displaycolorDepth:")};
	} else if (linkLine) {
		const char *p = strstr(buf, "link: ");
		uint32_t link = p ? (uint32_t)strtoul(p + 6, nullptr, 10) : 0;
		if (link != 0 && gPendStream.pixKhz) {
			gExtStream = gPendStream;
			gExtLink = link;
		}
	}
	if (lines >= 6000 || !rmbVerbose())
		return;
	size_t n = strlen(buf);
	while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
		buf[--n] = 0;
	lines++;
	LOG("dal[%d] %s", major, buf);
}

// AmdLogger: writeLog checks level (+0x30) >= severity and the type bit in mask (+0x28) inline.
// Open both up after init (framebuffer/link bring-up debugging).
mach_vm_address_t orgLoggerInit;
// -rmbamdlog: force Apple's AMD logger to all levels/categories. Off by default: it prints 2 serial
// lines per cursor move ([CUR:0] saveCursorPosition), and serial printf is synchronous -> laggy mouse.
bool gAmdLogAll;

uint64_t wrapLoggerInit(uint64_t logger, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
	auto r = reinterpret_cast<Fn6>(orgLoggerInit)(logger, b, c, d, e, f);
	if (!gAmdLogAll)
		return r;
	LOG("amdlog: logger %llx level %d mask %llx -> all", logger, *reinterpret_cast<int *>(logger + 0x30),
		*reinterpret_cast<uint64_t *>(logger + 0x28));
	*reinterpret_cast<int *>(logger + 0x30) = 100;
	*reinterpret_cast<uint64_t *>(logger + 0x28) = ~0ULL;
	return r;
}

// AmdLink::detectLink(reason, DetectedLinkStatus *): log result and the status it filled.
mach_vm_address_t orgDetectLink;
uint64_t wrapDetectLink(uint64_t link, uint64_t reason, uint64_t status, uint64_t d, uint64_t e, uint64_t f) {
	auto r = reinterpret_cast<Fn6>(orgDetectLink)(link, reason, status, d, e, f);
	edidRepair(link, status);
	static uint32_t logged;
	if (logged++ < 24) {
		auto q = reinterpret_cast<const uint32_t *>(status);
		LOG("link %llx detect(reason %llu) = %llx status %08x %08x %08x %08x %08x %08x %08x %08x", link, reason & 0xFFFFFFFF,
			r, status ? q[0] : 0, status ? q[1] : 0, status ? q[2] : 0, status ? q[3] : 0, status ? q[4] : 0, status ? q[5] : 0,
			status ? q[6] : 0, status ? q[7] : 0);
	}
	return r;
}

#define DTRACE(name)                                                                              \
	mach_vm_address_t org_##name;                                                                  \
	uint64_t wrap_##name(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) { \
		LOG(">> " #name "(%llx)", b);                                                              \
		auto r = reinterpret_cast<Fn6>(org_##name)(a, b, c, d, e, f);                             \
		LOG("<< " #name "(%llx) = %llx", b, r);                                                    \
		return r;                                                                                  \
	}
DTRACE(detectAllDisplays)
DTRACE(detectDisplayOnLink)

// establishBootDisplay adopts the firmware's boot display (start-up mode from the GOP/NVRAM);
// with no AMD GOP in the VM it returns NotFound and no framebuffer gets the eDP. Treat the eDP
// like a hotplug instead: detectDisplays(mask of link 0), the path used for displays attached
// after boot.
mach_vm_address_t org_establishBootDisplay, gGetLink, gResetConnected;
DTRACE(detectDisplays)

// Boot detection (during power-up, controller not yet enabled) sends link 0's connect event too
// early. Later, forget link 0's cached state (+0x494 connected, +0x4a0 1 KB EDID) so a hotplug-style
// detectDisplays sees a new connection and sends the event again.
// displaypolicyd (the AGDC policy engine that assigns links to framebuffers) polls
// AppleGraphicsDeviceControl::sWindowServerReady for 20 s, then exits and is respawned. WindowServer
// sets it only once it has a working display, which it never gets here, so set it ourselves.
// Offset from the extracted kext: sWindowServerReady 0x13e23914 - __TEXT 0x13e1d000 (15.8.1).
// 25G229 (AGDC LC_UUID 02FECF8A-..., SystemKernelExtensions.kc): 0x13d45914 - 0x13d3f000 = 0x6914 too;
// resolveAgdcBase() (kern_start.cpp) checks that UUID in memory before gAgdcBase is set.
// AGDC 25G229 filtered_doDeviceAttribute: cmd 0x10 read = movslq sWindowServerReady (0x13d410bd);
// cmd 0x10 write from the WindowServer client = OSCompareAndSwap(0,1) / (1,0) (0x13d4199b / 0x13d41b6c),
// so WindowServer can also clear it (e.g. while it starts). Hence: set it once WindowServer has taken
// the framebuffers ('wsrv'), and set it again if it drops to 0 while no fb reported a display.
mach_vm_address_t gAgdcBase;
constexpr mach_vm_address_t AgdcWsReady = 0x6914;
volatile bool gWsAttached;     // wrapFbSetAttr saw setAttribute('wsrv')
volatile bool gFbEnabledSeen;  // wrapGetAttr saw 'enab' != 0 on some fb

void replayConnect(uint64_t ctrl) {
	IOSleep(15000);
	if (!gAgdcBase && !checkKernelArgument("-rmbnowsready"))
		resolveAgdcBase();
	if (gAgdcBase) {
		auto ready = reinterpret_cast<volatile UInt32 *>(gAgdcBase + AgdcWsReady);
		for (int i = 0; i < 45 && !gWsAttached; i++)
			IOSleep(1000);
		IOSleep(2000);
		LOG("replay: AGDC sWindowServerReady %d -> 1 (WindowServer %s)", (int32_t)*ready,
		    gWsAttached ? "attached" : "not seen within 45 s");
		// Boot 2026-11-09 03:17: already 1 here (set by WindowServer itself, 51.5 s). Then displaypolicyd
		// is already polling it and 15 s is enough; 40 s only when we had to set it (respawn delay).
		bool wsSetIt = *ready != 0;
		if (!wsSetIt)
			OSCompareAndSwap(0, 1, ready);
		int32_t last = (int32_t)*ready;
		unsigned zeroFor = 0, reasserts = 0;
		for (int s = 1; s <= (wsSetIt ? 15 : 40) && !gFbEnabledSeen; s++) {
			IOSleep(1000);
			int32_t now = (int32_t)*ready;
			if (now != last) {
				LOG("replay: AGDC sWindowServerReady %d -> %d at +%d s (fb display seen %d)", last, now, s, gFbEnabledSeen);
				last = now;
			}
			zeroFor = now == 0 ? zeroFor + 1 : 0;
			if (zeroFor >= 3 && !gFbEnabledSeen && reasserts < 3) {
				reasserts++;
				zeroFor = 0;
				OSCompareAndSwap(0, 1, ready);
				last = (int32_t)*ready;
				LOG("replay: AGDC sWindowServerReady cleared with no fb display -> set again (%u/3), now %d", reasserts, last);
			}
		}
		LOG("replay: AGDC sWindowServerReady %d, fb display seen %d", (int32_t)*ready, gFbEnabledSeen);
	}
	bool replayAll = checkKernelArgument("-rmbreplayall");
	// Since earlyOpen, link 0 is already connected here. detectDisplays re-detects EVERY link, and
	// re-detecting the HDMI link swapped its sink under WindowServer's validateDetailedTiming:
	// auto-1020-1205 panicked (stream from a sink with link NULL). Replay only if still needed
	// (`-rmbreplayall` = always, old behaviour).
	// Lenovo 25G229 (boots 2026-11-09): link 0's cached connected (+0x494) is 1 after boot detection, yet
	// no framebuffer ever got it (FB:0/FB:1 'enab' 0, "path 0 links []"): that boot-time connect reached
	// AmdAgdcServices before AGDC enabled the controller (log: "vendor_doDeviceAttribute() ??? Controller
	// is NOT enabled so SKIP. cmd:[3]"; FB 25G229 validateCommand 0x20378 needs byte +297, set by
	// setControllerEnabled 0x129c2). So "still needed" = no framebuffer reports a display, not the cache.
	// detectDisplays (FB 0x4e454) needs controller flag +24344 bit 3 and detects every link 0..+288-1, so
	// any other connected link (the HDMI sink swap above) still blocks the replay.
	for (unsigned attempt = 1; attempt <= 2; attempt++) {
		auto link = reinterpret_cast<Fn6>(gGetLink)(ctrl, 0, 0, 0, 0, 0);
		LOG("replay: link0 %llx cached connected %u, new %u, fb display seen %d, ctrl flags %x (attempt %u)", link,
		    link ? *reinterpret_cast<uint8_t *>(link + 0x494) : 0, link ? *reinterpret_cast<uint8_t *>(link + 0x54) : 0,
		    gFbEnabledSeen, *reinterpret_cast<volatile uint8_t *>(ctrl + 24344), attempt);
		if (!link)
			return;
		if (gFbEnabledSeen && !replayAll) {
			LOG("replay: a framebuffer already reports a display, skipped");
			return;
		}
		// CFG_LINK_FIXED_MAP (kern_start.cpp syncPciDeviceDirect): a fixed link (+0x47, getFeature(13)) is
		// bound to FB:0 by initializeFixedDisplayBinding and never sends AGDC link events
		// (detectDisplayOnLink 0x4e5bc), so a replay cannot help it; clearing its EDID cache under FB:0
		// would only disturb it.
		if ((*reinterpret_cast<uint8_t *>(link + 0x47) & 1) && !replayAll) {
			LOG("replay: link0 is a fixed (CFG_LINK_FIXED_MAP) link of FB:0, no AGDC replay");
			return;
		}
		unsigned others = 0;
		for (uint64_t i = 1; i < 16; i++) {  // getLink (FB 0x4fbc0) returns 0 for i >= link count (+288)
			auto l = reinterpret_cast<Fn6>(gGetLink)(ctrl, i, 0, 0, 0, 0);
			if (!l)
				break;
			if (*reinterpret_cast<uint8_t *>(l + 0x494) || *reinterpret_cast<uint8_t *>(l + 0x54)) {
				LOG("replay: link%llu is connected too (cached %u, new %u)", i, *reinterpret_cast<uint8_t *>(l + 0x494),
				    *reinterpret_cast<uint8_t *>(l + 0x54));
				others++;
			}
		}
		if (others && !replayAll) {
			LOG("replay: %u other link(s) connected; detectDisplays re-detects every link (HDMI sink swap panic), "
			    "skipped: boot with only the internal panel", others);
			return;
		}
		*reinterpret_cast<uint8_t *>(link + 0x494) = 0;
		memset(reinterpret_cast<void *>(link + 0x4a0), 0, 0x400);
		auto d2 = reinterpret_cast<Fn6>(org_detectDisplays)(ctrl, 1, 0, 0, 0, 0);
		LOG("replay: detectDisplays(link 0) = %llx", d2);
		for (int s = 0; s < 30 && !gFbEnabledSeen; s++)
			IOSleep(1000);
		LOG("replay: fb display seen %d after replay %u", gFbEnabledSeen, attempt);
		if (gFbEnabledSeen)
			return;
	}
}
uint64_t gCtrl;
void extReplay(void *, wait_result_t);
uint64_t wrap_establishBootDisplay(uint64_t ctrl, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
	auto r = reinterpret_cast<Fn6>(org_establishBootDisplay)(ctrl, b, c, d, e, f);
	LOG("establishBootDisplay = %llx", r & 0xFFFFFFFF);
	gDalCtrl = ctrl;
	// AmdLink 25G229: +0x18 index, +0x1c connector object id (getFeature(14) 0x45fce: 0x14 eDP / 0x0e LVDS),
	// +0x47 CFG_LINK_FIXED_MAP bit, +0x48 CFG_LINK_BUILT_IN_MAP bit (initWithInfo 0x4564c/0x45673),
	// +0x54 connected (isConnected 0x4645c), +0x40 bit 0 initialized.
	for (uint64_t i = 0; gGetLink && i < 8; i++) {
		auto l = reinterpret_cast<uint8_t *>(reinterpret_cast<Fn6>(gGetLink)(ctrl, i, 0, 0, 0, 0));
		if (!l)
			break;
		LOG("fixedmap: link%llu idx %u connector %x init %u fixed %u built-in %u connected %u", i,
		    *reinterpret_cast<uint32_t *>(l + 0x18), l[0x1c], l[0x40] & 1, l[0x47] & 1,
		    l[0x48] & 1, l[0x54] & 1);
	}
	if ((r & 0xFFFFFFFF) == 0xe00002f0 && org_detectDisplays && gGetLink) {
		gCtrl = ctrl;
		thread_t t;
		kernel_thread_start([](void *, wait_result_t) { replayConnect(gCtrl); }, nullptr, &t);
		thread_deallocate(t);
	}
	static bool extStarted;
	if (!extStarted && org_detectDisplayOnLink && gGetLink) {
		extStarted = true;
		if (checkKernelArgument("-rmbnoextreplay")) {
			LOG("extreplay: off (-rmbnoextreplay)");
		} else {
			thread_t t;
			if (kernel_thread_start(extReplay, nullptr, &t) == KERN_SUCCESS)
				thread_deallocate(t);
		}
	}
	return r;
}

// After S3 wake macOS lights the panel, sleeps the display 1-2 s later, then WindowServer reprobes
// and sets the mode again. auto-1022-1243/1313: WS reprobed but never set a mode on the panel (no
// IOFBSetDisplayModeAndDepth, FB:0 stayed 1 x 1) while powerd said display on and HID input arrived
// -> panel dark for good. Good wakes (1212, 1257) differ only in WS taking the main display offline
// first. Watch for that state after a wake: display wrangler on but no OTG running for 8 s -> replay
// a link 0 connect like at boot, so WS sees a hotplug and sets a mode (up to 2 times, 120 s window;
// -rmbnowakewatch: off).
}  // namespace
bool gWakeWatchArmed;
void panelWakeWatch() {
	if (!gWakeWatchArmed || !gCtrl || !gGetLink || !org_detectDisplays || checkKernelArgument("-rmbnowakewatch"))
		return;
	gWakeWatchArmed = false;
	thread_t t;
	kernel_thread_start(
		[](void *, wait_result_t) {
			auto wrangler = IOService::copyMatchingService(IOService::serviceMatching("IODisplayWrangler"));
			LOG("wakewatch: started (wrangler %s)", wrangler ? "found" : "MISSING");
			// auto-1024-0453: replay #1 returned at once without detecting (DAL only powered up), #2 two
			// seconds later gave the hotplug -> dark from 4 s on, and a no-op replay is redone after 1 s.
			constexpr uint32_t DarkLimit = 4;
			uint32_t darkSecs = 0, replays = 0, noops = 0;
			for (uint32_t s = 0; wrangler && s < 120 && replays < 2; s++) {
				IOSleep(1000);
				auto ps = wrangler->getPowerState();
				bool otg = anyOtgOn();
				darkSecs = (ps >= 3 && !otg) ? darkSecs + 1 : 0;
				if (darkSecs < DarkLimit)
					continue;
				auto link = reinterpret_cast<Fn6>(gGetLink)(gCtrl, 0, 0, 0, 0, 0);
				if (!link)
					break;
				replays++;
				LOG("wakewatch: +%u s display on (wrangler %u) but no OTG for %u s -> replay link 0 connect #%u", s + 1,
				    (unsigned)ps, darkSecs, replays);
				*reinterpret_cast<uint8_t *>(link + 0x494) = 0;
				memset(reinterpret_cast<void *>(link + 0x4a0), 0, 0x400);
				uint64_t t0 = nowUs();
				auto d = reinterpret_cast<Fn6>(org_detectDisplays)(gCtrl, 1, 0, 0, 0, 0);
				uint64_t us = nowUs() - t0;
				bool noop = us < 100000 && noops < 5;
				LOG("wakewatch: detectDisplays(link 0) = %llx in %llu ms%s", d & 0xFFFFFFFF, us / 1000,
				    noop ? " (no detect ran?) -> again in 1 s" : "");
				if (noop) {
					noops++;
					replays--;
					darkSecs = DarkLimit - 1;
					continue;
				}
				darkSecs = 0;
			}
			LOG("wakewatch: done, %u replays, OTG %s", replays, anyOtgOn() ? "on" : "off");
			OSSafeReleaseNULL(wrangler);
		},
		nullptr, &t);
	thread_deallocate(t);
}
// auto-1023-1814: after wake the panel was black with no glow while the whole GPU side was lit (OTG0,
// HUBP0 fetching, PHY 28ff0f0, PWRSEQ0 STATE 41f, DP0 4 lanes TRAINING_COMPLETE, video stream on). Host
// AUX read of the panel: DPCD 100/101 = 06 01 (reset defaults), 202 = 00 (no lane lock), 600 = 00 -> the
// sink reset after DAL trained it (at the PWRSEQ0 deaths) and nobody retrained it; the old retrain here
// (link 0 connect replay) got detectDisplays = NotFound and did nothing. A host-side CPU link training
// (Linux dce_aux.c AUX sequence + dcn10 link encoder training pattern regs) lit the panel again. So watch
// the eDP sink's lane status every 2 s and retrain it from the CPU when it is lost, at the link rate and
// lane count the sink had while good (else its max caps). DAL's own AUX0 accesses take gAuxLock too, and
// we start only when DAL left AUX0 alone for 200 ms. Log "edplink:"; -rmbnolinkwatch = off.
constexpr uint32_t Aux0Control = Seg2 + 0x1f50, Aux0SwControl = Aux0Control + 1, Aux0Arb = Aux0Control + 2,
				   Aux0IntCtl = Aux0Control + 3, Aux0SwStatus = Aux0Control + 4, Aux0SwData = Aux0Control + 6,
				   Aux0Last = Aux0Control + 0x1b, Dp0LinkCntl = Seg2 + 0x2108, Dp0Config = Seg2 + 0x210b,
				   Dp0VidStream = Seg2 + 0x210c, Dp0DphyCntl = Seg2 + 0x2117, Dp0TrainPat = Seg2 + 0x2118,
				   Dp0PrbsCntl = Seg2 + 0x211d, Otg0Control = Seg2 + 0x1b41;
constexpr uint32_t AuxIndexWr = 0x80000000u, AuxIndex = 0x1F0000u, AuxDataMask = 0xFF00u, AuxRead = 1u;
IOLock *gAuxLock;
uint64_t gAuxDalUs;
bool gEdpLinkPause;
volatile bool gEdpLinkKick;

void auxDalTouch(uint32_t hw) {
	if (hw < Aux0Control || hw > Aux0Last || !gAuxLock)
		return;
	IOLockLock(gAuxLock);
	gAuxDalUs = nowUs();
	IOLockUnlock(gAuxLock);
}

// One native AUX transaction on the AUX channel whose AUXn_AUX_CONTROL is ctl (cmd 0x80 write / 0x90 read,
// n <= 16). Returns bytes replied, -1 no reply/timeout/HPD low, -2 NACK, -3 DEFER.
int auxXferOn(uint32_t ctl, uint8_t cmd, uint32_t addr, uint8_t *buf, uint32_t n) {
	const uint32_t swControl = ctl + 1, intCtl = ctl + 3, swStatus = ctl + 4, swData = ctl + 6;
	bool write = cmd == 0x80;
	mmioWrite(intCtl, mmioRead(intCtl) | 2);  // AUX_SW_DONE_ACK
	for (int i = 0; i < 100 && (mmioRead(swStatus) & 1); i++)
		IODelay(10);
	mmioWrite(swControl, (mmioRead(swControl) & ~0x1F00F1u) | (4 + (write ? n : 0)) << 16);  // WR_BYTES
	uint32_t base = mmioRead(swData) & ~(AuxIndexWr | AuxIndex | AuxDataMask | AuxRead);
	mmioWrite(swData, base | AuxIndexWr | (uint32_t)(cmd | ((addr >> 16) & 0xf)) << 8);  // index 0
	uint8_t hdr[3] = {uint8_t(addr >> 8), uint8_t(addr), uint8_t(n - 1)};
	for (auto b : hdr)
		mmioWrite(swData, base | uint32_t(b) << 8);
	for (uint32_t i = 0; write && i < n; i++)
		mmioWrite(swData, base | uint32_t(buf[i]) << 8);
	mmioWrite(swControl, mmioRead(swControl) | 1);  // AUX_SW_GO
	uint32_t st = 0;
	for (int i = 0; i < 1000 && !((st = mmioRead(swStatus)) & 1); i++)
		IODelay(10);
	uint32_t cnt = (st >> 24) & 0x1f;
	if (!(st & 1) || (st & 0x200) || !cnt)
		return -1;
	mmioWrite(swData, base);  // the Linux read setup, one field at a time: index 0, autoinc off, read
	mmioWrite(swData, base | AuxIndexWr);
	mmioWrite(swData, base | AuxIndexWr | AuxRead);
	if (uint32_t rep = (mmioRead(swData) >> 8 & 0xff) >> 4)
		return rep == 2 ? -3 : -2;
	cnt--;
	for (uint32_t i = 0; i < cnt; i++) {
		uint8_t b = mmioRead(swData) >> 8;
		if (!write && i < n)
			buf[i] = b;
	}
	return cnt;
}
int auxXfer(uint8_t cmd, uint32_t addr, uint8_t *buf, uint32_t n) {
	int r = auxXferOn(Aux0Control, cmd, addr, buf, n);
	if (r != -3)
		return r;
	return -2;
}

int dpcdRead(uint32_t a, uint8_t *b, uint32_t n) { return auxXfer(0x90, a, b, n); }
template <uint32_t N> int dpcdWrite(uint32_t a, const uint8_t (&d)[N]) {
	uint8_t b[N];
	memcpy(b, d, N);
	return auxXfer(0x80, a, b, N);
}

// edidfix (11-09): boot 190239 lost 120 Hz. EDID block 2 (DisplayID 0x70, Type I timing 3072x1920 at
// 785.28 MHz) reached macOS with bytes 33..47 = ff (one bad 16-byte I2C-over-AUX chunk in the zero padding),
// macOS: "EDID read failure: GPU:0xb684 Port:1.0 Block[2] Bad CheckSum: 90 vs 9f" -> extension dropped, panel
// left at 60 Hz. DAL's read_edid (FB 0x12f655) retries only when its own parse fails, and that parse does not
// check the DisplayID checksum. macOS gets the EDID from the AmdLink status detectLink fills (25G229 FB
// 0x45830: DAL status at link+0x50 copied to the caller's DetectedLinkStatus at 0x4594a and to the cache
// link+0x490 by cacheLinkStatus 0x45b1a; status +0x0 link index, +0x8 DAL signal, +0xc EDID bytes, +0x10 EDID.
// getDDCBlock 0x576c reads link vtable +424 getActiveLinkStatus 0x464b0 = link+0x8d0, which
// connectionChange 0x163d1 fills with commitLinkStatus 0x46284 from getConnectedLinkStatus 0x46498 = link+0x50).
// After detectLink: if an eDP block has a bad checksum, read that block
// again from the panel on AUX0 with DAL's own E-DDC sequence (segment 0x30 for block >= 2, offset 0x50, 16-byte
// I2C reads, stop) and take it only with a valid checksum, and only after block 0 read the same way matches
// the link's block 0 (same sink). Panel bytes only, nothing synthesized. Log "edidfix:"; -rmbnoedidfix = off.
// Contract test: tests/test_edid_repair.py.
constexpr uint32_t EdidStatusOff = 0x50, EdidCacheOff = 0x490, EdidActiveOff = 0x8d0, EdidStatusSize = 0xc,
				   EdidStatusData = 0x10, EdidSignalEdp = 0x80, EdidMaxBytes = 1024;

// One I2C-over-AUX transaction (cmd 0x40 write MOT, 0x50 read MOT, 0x10 read stop) to I2C device dev on the AUX
// channel at ctl; n == 0 = address-only (no length byte). Same register protocol as auxXferOn.
// Returns data bytes replied, -1 timeout/HPD, -2 NACK, -3 DEFER (native or I2C).
int auxI2cOn(uint32_t ctl, uint8_t cmd, uint8_t dev, uint8_t *buf, uint32_t n) {
	const uint32_t swControl = ctl + 1, intCtl = ctl + 3, swStatus = ctl + 4, swData = ctl + 6;
	bool write = !(cmd & 0x10);
	mmioWrite(intCtl, mmioRead(intCtl) | 2);  // AUX_SW_DONE_ACK
	for (int i = 0; i < 100 && (mmioRead(swStatus) & 1); i++)
		IODelay(10);
	uint32_t req = n ? 4 + (write ? n : 0) : 3;
	mmioWrite(swControl, (mmioRead(swControl) & ~0x1F00F1u) | req << 16);  // WR_BYTES
	uint32_t base = mmioRead(swData) & ~(AuxIndexWr | AuxIndex | AuxDataMask | AuxRead);
	mmioWrite(swData, base | AuxIndexWr | uint32_t(cmd) << 8);  // index 0
	mmioWrite(swData, base);                                     // address [15:8] = 0
	mmioWrite(swData, base | uint32_t(dev) << 8);
	if (n)
		mmioWrite(swData, base | uint32_t(n - 1) << 8);
	for (uint32_t i = 0; write && i < n; i++)
		mmioWrite(swData, base | uint32_t(buf[i]) << 8);
	mmioWrite(swControl, mmioRead(swControl) | 1);  // AUX_SW_GO
	uint32_t st = 0;
	for (int i = 0; i < 1000 && !((st = mmioRead(swStatus)) & 1); i++)
		IODelay(10);
	uint32_t cnt = (st >> 24) & 0x1f;
	if (!(st & 1) || (st & 0x200) || !cnt)
		return -1;
	mmioWrite(swData, base);
	mmioWrite(swData, base | AuxIndexWr);
	mmioWrite(swData, base | AuxIndexWr | AuxRead);
	uint32_t rep = (mmioRead(swData) >> 8 & 0xff) >> 4;  // [1:0] native, [3:2] I2C: 0 ACK, 1 NACK, 2 DEFER
	if ((rep & 3) == 2 || (rep >> 2) == 2)
		return -3;
	if (rep)
		return -2;
	cnt--;
	for (uint32_t i = 0; i < cnt; i++) {
		uint8_t b = mmioRead(swData) >> 8;
		if (!write && i < n)
			buf[i] = b;
	}
	return cnt;
}

int edidI2c(uint8_t cmd, uint8_t dev, uint8_t *buf, uint32_t n) {
	int r = -1;
	for (int t = 0; t < 8; t++) {
		r = auxI2cOn(Aux0Control, cmd, dev, buf, n);
		if (r != -3)
			break;
		IODelay(500);
	}
	return r;
}

uint8_t edidSum(const uint8_t *b) {
	uint8_t s = 0;
	for (uint32_t i = 0; i < 128; i++)
		s += b[i];
	return s;
}

// Returns 0 = read with valid checksum, else the failing step (1 segment, 2 offset, 3 data, 4 checksum).
uint32_t edidReadBlock(uint32_t blk, uint8_t *out) {
	uint8_t seg = uint8_t(blk / 2), off = uint8_t((blk & 1) * 128);
	uint32_t fail = 0;
	if (blk >= 2 && edidI2c(0x40, 0x30, &seg, 1) != 0)
		fail = 1;
	else if (edidI2c(0x40, 0x50, &off, 1) != 0)
		fail = 2;
	for (uint32_t i = 0; !fail && i < 128; i += 16)
		if (edidI2c(0x50, 0x50, out + i, 16) != 16)
			fail = 3;
	edidI2c(0x10, 0x50, nullptr, 0);  // address-only read, MOT 0 = stop
	return fail ? fail : edidSum(out) ? 4 : 0;
}

int8_t gEdidFix = -1;
uint32_t gEdidFixLogs;

void edidRepair(uint64_t link, uint64_t status) {
	if (gEdidFix < 0)
		gEdidFix = !checkKernelArgument("-rmbnoedidfix");
	if (!gEdidFix || !link || !mmioAvailable())
		return;
	auto u32 = [](uint64_t a) { return *reinterpret_cast<volatile uint32_t *>(a); };
	uint64_t st = link + EdidStatusOff;
	uint32_t idx = u32(st), signal = u32(st + 8), size = u32(st + EdidStatusSize);
	auto edid = reinterpret_cast<uint8_t *>(st + EdidStatusData);
	if (signal != EdidSignalEdp || size < 128 || size > EdidMaxBytes || size % 128 ||
		u32(st + EdidStatusData) != 0xffffff00u || u32(st + EdidStatusData + 4) != 0x00ffffffu)
		return;
	uint32_t blocks = size / 128, bad = 0;
	if (edid[126] + 1u < blocks)
		blocks = edid[126] + 1u;
	for (uint32_t b = 0; b < blocks; b++)
		if (edidSum(edid + b * 128))
			bad |= 1u << b;
	if (!bad) {
		if (gEdidFixLogs++ < 4)
			LOG("edidfix: link %u eDP EDID %u blocks, checksums OK", idx, blocks);
		return;
	}
	for (uint32_t b = 0; b < blocks; b++)
		if (bad & (1u << b))
			LOG("edidfix: link %u block %u/%u bad checksum (sum %02x, tag %02x) -> re-reading from the panel on AUX0", idx, b,
				blocks, edidSum(edid + b * 128), edid[b * 128]);
	// DAL's own AUX register accesses take gAuxLock (auxDalTouch): wait for 2 ms of DAL AUX quiet, then hold it.
	for (int i = 0; i < 50 && gAuxLock && nowUs() - gAuxDalUs < 2000; i++)
		IOSleep(1);
	if (gAuxLock)
		IOLockLock(gAuxLock);
	uint32_t arb = mmioRead(Aux0Arb), fixed = 0, fails = 0;
	bool requested = false, granted = false;
	if (((arb >> 2) & 3) != 2 && (mmioRead(Aux0Control) & 1)) {
		mmioWrite(Aux0Arb, arb | 0x10000u);  // AUX_SW_USE_AUX_REG_REQ
		requested = true;
		granted = ((mmioRead(Aux0Arb) >> 2) & 3) == 1;
	}
	uint8_t blk0[128], fix[128];
	bool same = false;
	uint32_t why0 = 9;
	for (int t = 0; granted && t < 3 && !same; t++) {
		why0 = edidReadBlock(0, blk0);
		same = !why0 && !memcmp(blk0, edid, (bad & 1) ? 18 : 128);  // header + vendor/product/serial
	}
	for (uint32_t b = 0; same && b < blocks; b++) {
		if (!(bad & (1u << b)))
			continue;
		uint32_t why = 9, t = 0;
		if (b == 0) {
			memcpy(fix, blk0, 128);
			why = 0;
		}
		for (; why && t < 5; t++)
			why = edidReadBlock(b, fix);
		if (why) {
			fails++;
			LOG("edidfix: link %u block %u: %u panel reads failed (last step %u), left as DAL read it", idx, b, t, why);
			continue;
		}
		uint32_t diff = 0;
		for (uint32_t i = 0; i < 128; i++)
			diff += fix[i] != edid[b * 128 + i];
		memcpy(edid + b * 128, fix, 128);
		const uint32_t copies[] = {EdidCacheOff, EdidActiveOff};
		for (uint32_t copy : copies) {
			uint64_t c = link + copy;
			if (u32(c) == idx && u32(c + EdidStatusSize) == size)
				memcpy(reinterpret_cast<uint8_t *>(c + EdidStatusData) + b * 128, fix, 128);
		}
		if (status && u32(status) == idx && u32(status + EdidStatusSize) == size)
			memcpy(reinterpret_cast<uint8_t *>(status + EdidStatusData) + b * 128, fix, 128);
		fixed++;
		LOG("edidfix: link %u block %u repaired from the panel after %u read(s): %u bytes differed from DAL's copy, tag %02x, "
			"checksum valid",
			idx, b, t ? t : 1, diff, fix[0]);
	}
	if (requested)
		mmioWrite(Aux0Arb, (mmioRead(Aux0Arb) & ~0x10000u) | 0x20000u);  // AUX_SW_DONE_USING_AUX_REG
	if (gAuxLock)
		IOLockUnlock(gAuxLock);
	if (!same)
		LOG("edidfix: link %u not repaired: AUX0 arb %x (granted %d), block 0 from AUX0 %s", idx, arb, granted,
			!granted ? "not read" : why0 ? "unreadable" : "differs from the link's EDID");
	else
		LOG("edidfix: link %u done: %u block(s) repaired, %u failed", idx, fixed, fails);
}

// USB-C ports (AUX2/AUX3) sit behind an on-board retimer with FIXED VS/PE (VBIOS chip cap
// EXT_DISPLAY_PATH_CAPS__DP_FIXED_VS_EN; Linux: "LTTPR count is nonzero but invalid lane count"). The
// retimer ignores the sink's VS/PE requests unless the source mirrors them into it with vendor writes to
// DPCD 0xF004F; Navi DAL (2020) predates that, so the sink kept asking VS 0->1->2->3 and never got
// CR/EQ (disklog 10-26 17:30). Linux dp_perform_fixed_vs_pe_training_sequence (logs/linux-usbc-replug.txt
// AUX2 writes) is replayed around DAL's own training AUX transactions, injected at DAL's AUX_SW_GO
// before DAL's request goes out (DAL's request bytes are then put back):
//   before 0x107 write: 0x108 = 1, reset 50 63 ff, VS 0, PE 0, intercept enable 55 63 00
//   before 0x100 write: toggle rate (0x100 = 06, or 0a for RBR); after it: dpmf 06 70 87 (+5 4-lane writes)
//   after the TPS1 burst (0x102 = x1): intercept disable 55 63 6e, NAK -> 55 63 00 and retry (<= 10)
//   before a TPS2+ burst: adicora 55 63 2e, 55 63 01, VS/PE; after it: 55 63 68
//   before every 0x103.. lane set write: VS 51 63 vs, PE 52 63 pe (2 bits per lane)
// Ordering like Linux link_hwss_dio_fixed_vs_pe_retimer.c (disklog 10-26 18:10: with the writes after
// DAL's PHY steps x4 never got CR and HBR2 x2 got CR but never EQ):
//   4-lane output writes also before the DMUB TX ENABLE (4 lanes) for PHY C/D (retimerTxEnable)
//   adicora 55 63 2e / 55 63 01 before DAL switches DP2/DP3 to TPS2+ (retimerPattern), not after
// The retimer NAKs these writes on Linux too (same AUX timing as a NAK; intercept-dis loop runs 10x).
// Log "retimer:"; only with -rmbretimer (the Lenovo 82TL has no LTTPR).
bool gNoRetimer = true;
struct Retimer {
	uint8_t lanes = 4, set[4];
	bool dpmf, intercept, eq3, training, eqPre;
	uint32_t lines;
};
Retimer gRetimer[3];
constexpr uint32_t RetimerAddr = 0xF004F;

int retimerAux(uint32_t ctl, uint8_t cmd, uint32_t addr, uint8_t *b, uint32_t n) {
	int r;
	for (int i = 0; i < 5 && (r = auxXferOn(ctl, cmd, addr, b, n)) == -3; i++)  // DEFER
		IODelay(400);
	return r;
}

bool retimerWrite(uint32_t ch, uint32_t ctl, uint8_t b1, uint8_t b2, uint8_t b3) {
	uint8_t d[4] = {1, b1, b2, b3};
	int r = retimerAux(ctl, 0x80, RetimerAddr, d, 4);
	auto &s = gRetimer[ch];
	if (s.lines < 400) {
		s.lines++;
		LOG("retimer: aux%u %02x %02x %02x -> %s", ch + 1, b1, b2, b3, r >= 0 ? "ack" : r == -2 ? "NAK" : "no reply");
	}
	return r >= 0;
}

void retimerDpcd(uint32_t ch, uint32_t ctl, uint32_t addr, uint8_t v) {
	int r = retimerAux(ctl, 0x80, addr, &v, 1);
	auto &s = gRetimer[ch];
	if (s.lines < 400) {
		s.lines++;
		LOG("retimer: aux%u DPCD %x = %02x -> %d", ch + 1, addr, v, r);
	}
}

void retimerVsPe(uint32_t ch, uint32_t ctl) {
	auto &s = gRetimer[ch];
	uint8_t vs = 0, pe = 0;
	for (uint32_t i = 0; i < s.lanes && i < 4; i++) {
		vs |= (s.set[i] & 3) << (2 * i);
		pe |= ((s.set[i] >> 3) & 3) << (2 * i);
	}
	retimerWrite(ch, ctl, 0x51, 0x63, vs);
	retimerWrite(ch, ctl, 0x52, 0x63, pe);
}

void retimer4Lane(uint32_t ch, uint32_t ctl) {
	retimerWrite(ch, ctl, 0x6e, 0xf2, 0x19);
	retimerWrite(ch, ctl, 0x6b, 0xf2, 0x01);
	retimerWrite(ch, ctl, 0x6d, 0xf2, 0x18);
	retimerWrite(ch, ctl, 0x6c, 0xf2, 0x03);
	retimerWrite(ch, ctl, 0x03, 0xf3, 0x06);
}

namespace {
// DP2/DP3 training pattern register about to get TPS2+ (ch 1 = AUX2/DP2, 2 = AUX3/DP3).
void retimerPattern(uint32_t ch, uint32_t v) {
	if (gNoRetimer || ch < 1 || ch > 2 || !gRetimer[ch].training)
		return;
	uint32_t ctl = AuxTrFirst + ch * AuxTrStride;
	auto &s = gRetimer[ch];
	if (s.lines < 400) {
		s.lines++;
		LOG("retimer: aux%u DP%u pattern %x -> adicora eq before it", ch + 1, ch + 1, v);
	}
	retimerWrite(ch, ctl, 0x55, 0x63, 0x2e);
	retimerWrite(ch, ctl, 0x55, 0x63, 0x01);
	s.eqPre = true;
}

// Training over, DAL is about to switch DP2/DP3 from the training pattern to the idle pattern. Navi DAL
// (old dc) does that first, waits 5 ms and only then writes DPCD 0x102 = 00, so the sink sees scrambled
// idle while still in TPS2 mode; the link was aligned (204 = 81) and lost alignment, then lane 0 CR, ~1 ms
// after DAL's 0x102 write, every retrain (usbc runs 10-27 17:15/17:30). Linux (logs/linux-usbc-dpcd.txt
// vs replug.txt) writes 0x102 = 00 first and sets TRAINING_COMPLETE ~1 ms later; its x2 link holds. Do the
// sink's exit first, like Linux. DAL's own 0x102 = 00 later is a harmless repeat. -rmbusbcltorder: off.
bool gUsbcLtOrder;
void usbcTrainingEnd(uint32_t ch) {
	if (gUsbcLtOrder || ch < 1 || ch > 2 || !gRetimer[ch].training)
		return;
	uint32_t ctl = AuxTrFirst + ch * AuxTrStride;
	uint8_t v = 0;
	int r = retimerAux(ctl, 0x80, 0x102, &v, 1);
	uint8_t st[3] = {};
	int r2 = retimerAux(ctl, 0x90, 0x202, st, 3);
	static uint32_t logged;
	if (logged++ < 40)
		LOG("usbc: aux%u DPCD 102 = 00 before DP%u idle pattern (Linux order) -> %d; 202-204 %02x %02x %02x (%d)",
			ch + 1, ch + 1, r, st[0], st[1], st[2], r2);
	IODelay(1000);	// Linux: ~1 ms between the DPCD write and TRAINING_COMPLETE
}

// New DMUB inbox commands (old hw WPTR .. newWptr, before the firmware sees them): DIG transmitter
// ENABLE with 4 lanes on PHY C/D → retimer 4-lane output writes first.
void retimerTxEnable(uint32_t newWptr) {
	if (gNoRetimer)
		return;
	uint32_t old = mmioRead(DmcubInbox1Wptr);
	uint64_t fbBase = (uint64_t)(mmioRead(0x1260 + 0x16fc) & 0xFFFFFF) << 24;
	uint64_t ring = ((uint64_t)mmioRead(DmcubCw4OffHi) << 32 | mmioRead(DmcubCw4OffLo)) - fbBase;
	for (uint32_t s = old; s != newWptr; s = (s + 64) % 0x2000) {
		uint64_t a = vramRead64(ring + s);
		uint32_t c1 = a >> 32, phy = c1 & 0xFF;
		if ((a & 0xFFFF) != 0x0180 || (phy != 2 && phy != 3) || ((c1 >> 8) & 0xFF) != 1 || (c1 >> 24) != 4)
			continue;
		uint32_t ch = phy - 1;
		if (gRetimer[ch].lines < 400) {
			gRetimer[ch].lines++;
			LOG("retimer: aux%u TX ENABLE phy %u 4 lanes [%x] -> 4-lane output writes first", ch + 1, phy, s);
		}
		retimer4Lane(ch, AuxTrFirst + ch * AuxTrStride);
	}
}

// ch: 0 = AUX1 .. 2 = AUX3; req = DAL's request bytes (cmd|addr19:16, addr15:8, addr7:0, len-1, data).
void retimerGo(uint32_t ch, const uint8_t *req, uint32_t n) {
	if (gNoRetimer || ch < 1 || ch > 2 || n < 4 || (req[0] >> 4) < 8)
		return;
	uint32_t ctl = AuxTrFirst + ch * AuxTrStride;  // AUXn_AUX_CONTROL (AuxTrFirst = AUX1)
	auto &s = gRetimer[ch];
	bool did = false;
	if (s.dpmf) {
		s.dpmf = false;
		did = true;
		retimerWrite(ch, ctl, 0x06, 0x70, 0x87);
		if (s.lanes == 4)
			retimer4Lane(ch, ctl);
	}
	if (s.intercept) {
		s.intercept = false;
		did = true;
		for (int i = 0; i < 10 && !retimerWrite(ch, ctl, 0x55, 0x63, 0x6e); i++)
			retimerWrite(ch, ctl, 0x55, 0x63, 0x00);
	}
	if (s.eq3) {
		s.eq3 = false;
		did = true;
		retimerWrite(ch, ctl, 0x55, 0x63, 0x68);
	}
	if ((req[0] >> 4) == 8 && n > 4) {	// native write
		uint32_t addr = (req[0] & 0xfu) << 16 | req[1] << 8 | req[2], len = n - 4;
		const uint8_t *d = req + 4;
		for (uint32_t i = 0; i < len; i++) {  // lane set bytes 0x103-0x106 (also in 0x102 bursts)
			uint32_t a = addr + i;
			if (a >= 0x103 && a <= 0x106)
				s.set[a - 0x103] = d[i];
			if (a == 0x101)
				s.lanes = d[i] & 0x1f;
		}
		if (addr == 0x107) {
			did = true;
			s.training = true;
			s.eqPre = false;
			retimerDpcd(ch, ctl, 0x108, 1);
			retimerWrite(ch, ctl, 0x50, 0x63, 0xff);
			retimerWrite(ch, ctl, 0x51, 0x63, 0x00);
			retimerWrite(ch, ctl, 0x52, 0x63, 0x00);
			retimerWrite(ch, ctl, 0x55, 0x63, 0x00);
		} else if (addr == 0x100) {
			did = true;
			retimerDpcd(ch, ctl, 0x100, d[0] == 0x06 ? 0x0a : 0x06);
			s.dpmf = true;
		} else if (addr == 0x102 && (d[0] & 0xf) == 1) {
			s.intercept = true;
		} else if (addr == 0x102 && (d[0] & 0xf)) {
			did = true;
			if (!s.eqPre) {	 // retimerPattern didn't see DAL's TPS2 PHY switch
				retimerWrite(ch, ctl, 0x55, 0x63, 0x2e);
				retimerWrite(ch, ctl, 0x55, 0x63, 0x01);
			}
			s.eqPre = false;
			retimerVsPe(ch, ctl);
			s.eq3 = true;
		} else if (addr == 0x102) {	 // training pattern off = training over
			s.training = s.eqPre = false;
		} else if (addr >= 0x103 && addr <= 0x106) {
			did = true;
			retimerVsPe(ch, ctl);
		}
	}
	if (!did)
		return;
	// put DAL's request back (our transfers reused AUX_SW_DATA), DONE acked so DAL waits for its own reply
	const uint32_t intCtl = ctl + 3, swStatus = ctl + 4, swData = ctl + 6;
	mmioWrite(intCtl, mmioRead(intCtl) | 2);
	for (int i = 0; i < 100 && (mmioRead(swStatus) & 1); i++)
		IODelay(10);
	uint32_t base = mmioRead(swData) & ~(AuxIndexWr | AuxIndex | AuxDataMask | AuxRead);
	mmioWrite(swData, base | AuxIndexWr | uint32_t(req[0]) << 8);
	for (uint32_t i = 1; i < n; i++)
		mmioWrite(swData, base | uint32_t(req[i]) << 8);
}

}  // namespace

// All of the first nl lanes CR + EQ + symbol locked and interlane aligned (DPCD 202-204).
bool lanesOk(const uint8_t *st, uint32_t nl, uint8_t bits) {
	for (uint32_t i = 0; i < nl; i++)
		if (((st[i / 2] >> (4 * (i & 1))) & bits) != bits)
			return false;
	return bits != 7 || (st[2] & 1);
}

void dp0Pattern(uint32_t pat) {
	mmioWrite(Dp0TrainPat, pat);
	mmioWrite(Dp0LinkCntl, mmioRead(Dp0LinkCntl) & ~0x10u);	   // DP_LINK_TRAINING_COMPLETE
	mmioWrite(Dp0DphyCntl, mmioRead(Dp0DphyCntl) & ~0x10000u);  // DPHY_BYPASS
	mmioWrite(Dp0PrbsCntl, mmioRead(Dp0PrbsCntl) & ~1u);		   // DPHY_PRBS_EN
}

// Host test (scripts/edp-train.py): CR locked on try 2 with the sink's requested levels written to the
// sink only (the PHY keeps DAL's levels), EQ on try 1.
bool edpTrain(uint8_t bw, uint8_t lanes, uint32_t nl, uint32_t &crTries, uint32_t &eqTries, uint8_t *st) {
	dpcdWrite(0x600, (const uint8_t[]){1});  // SET_POWER D0
	IODelay(2000);
	dpcdWrite(0x100, (const uint8_t[]){bw, lanes});
	dpcdWrite(0x108, (const uint8_t[]){1});  // 8b/10b
	dp0Pattern(0);			// TPS1
	uint8_t set[4] = {};
	bool ok = false;
	for (crTries = 1; crTries <= 5 && !ok; crTries++) {
		dpcdWrite(0x102, (const uint8_t[]){0x21, set[0], set[1], set[2], set[3]});
		IODelay(1000);
		if (dpcdRead(0x202, st, 6) != 6)
			break;
		ok = lanesOk(st, nl, 1);
		uint32_t adj = st[4] | st[5] << 8;
		for (uint32_t i = 0; i < 4; i++)
			set[i] = ((adj >> (4 * i)) & 3) | (((adj >> (4 * i + 2)) & 3) << 3);
	}
	crTries--;
	bool eq = false;
	if (ok) {
		dp0Pattern(1);  // TPS2
		for (eqTries = 1; eqTries <= 5 && !eq; eqTries++) {
			dpcdWrite(0x102, (const uint8_t[]){0x22, set[0], set[1], set[2], set[3]});
			IOSleep(20);
			if (dpcdRead(0x202, st, 6) != 6)
				break;
			eq = lanesOk(st, nl, 7);
			uint32_t adj = st[4] | st[5] << 8;
			for (uint32_t i = 0; i < 4; i++)
				set[i] = ((adj >> (4 * i)) & 3) | (((adj >> (4 * i + 2)) & 3) << 3);
		}
		eqTries--;
	}
	dpcdWrite(0x102, (const uint8_t[]){0});
	mmioWrite(Dp0LinkCntl, mmioRead(Dp0LinkCntl) | 0x10u);  // back to video
	return eq;
}

// One check; with gAuxLock held and AUX0 acquired.
void edpLinkCheck() {
	static uint8_t goodBw, goodLanes;
	static uint32_t fails, retrains, logged;
	static uint64_t nextTryUs;
	uint32_t nl = (mmioRead(Dp0Config) & 3) + 1;
	uint8_t st[6] = {}, cfg[2] = {}, pwr = 0;
	if (int r = dpcdRead(0x202, st, 3); r != 3) {
		static uint32_t auxFails;
		uint32_t sw = mmioRead(Aux0SwStatus);
		bool dead = !(mmioRead(BlPwmPeriod) & 0xffff);
		if (auxFails++ < 10)
			LOG("edplink: DPCD 202 read failed (%d, AUX_SW_STATUS %x), PWRSEQ0 %s", r, sw, dead ? "dead" : "alive");
		// auto BM 10-27: after a real S3 PWRSEQ0 stayed dead past the last backlight set (the only revive
		// caller), so DIGON/panel VDD never came back: HPD low (AUX_SW_RX_HPD_DISCON, bit 9) -> panel dark.
		// The VM revived it ~1 s after wake from a backlight set; here keep trying every check. A good revive
		// restores DIGON and kicks a retrain. -rmbnoauxrevive: off.
		static int noAuxRevive = -1;
		if (noAuxRevive < 0)
			noAuxRevive = checkKernelArgument("-rmbnoauxrevive");
		if (!noAuxRevive && (sw & 0x200) && dead)
			revivePwrseq0(0);
		return;
	}
	if (lanesOk(st, nl, 7)) {
		if (!goodBw && dpcdRead(0x100, cfg, 2) == 2 && cfg[0] && (cfg[1] & 0x1f) == nl) {
			goodBw = cfg[0];
			goodLanes = cfg[1];
			LOG("edplink: watching eDP link, sink bw %x lanes %x status %02x %02x %02x", goodBw, goodLanes, st[0], st[1], st[2]);
		}
		fails = 0;
		return;
	}
	uint64_t now = nowUs();
	if (now < nextTryUs)
		return;
	dpcdRead(0x100, cfg, 2);
	dpcdRead(0x600, &pwr, 1);
	uint8_t bw = goodBw, lanes = goodLanes;
	if (!bw) {
		uint8_t caps[2] = {};
		dpcdRead(0x001, caps, 2);
		bw = caps[0];
		lanes = uint8_t(nl | (caps[1] & 0x80));
	}
	uint8_t before[3] = {st[0], st[1], st[2]};
	uint32_t cr = 0, eq = 0;
	bool ok = bw && edpTrain(bw, lanes, nl, cr, eq, st);
	retrains++;
	fails = ok ? 0 : fails + 1;
	nextTryUs = now + (fails >= 3 ? 30000000 : 0);  // keep failing -> every 30 s
	if (logged++ < 40)
		LOG("edplink: lost (202 %02x %02x %02x, sink 100 %02x %02x, 600 %02x) -> retrain #%u bw %x lanes %x: CR tries %u EQ "
			"tries %u, now %02x %02x %02x %s",
			before[0], before[1], before[2], cfg[0], cfg[1], pwr, retrains, bw, lanes, cr, eq, st[0], st[1], st[2],
			ok ? "OK" : "FAILED");
}

void edpLinkWatch() {
	for (;;) {
		for (int i = 0; i < 20 && !gEdpLinkKick; i++)
			IOSleep(100);
		gEdpLinkKick = false;
		if (gEdpLinkPause || !mmioAvailable())
			continue;
		if (!(mmioRead(Otg0Control) & 1) || !(mmioRead(Dp0VidStream) & 1))
			continue;  // eDP off
		vramCrcTrack();
		// auto-1024-0546: after a wake DAL left DP_LINK_TRAINING_COMPLETE clear with the stream on and the sink
		// reset -> the old gate on it skipped every check (panel dark). DAL training is AUX traffic, so with
		// the bit clear just want a longer DAL AUX quiet time.
		bool trained = mmioRead(Dp0LinkCntl) & 0x10;
		IOLockLock(gAuxLock);
		uint32_t arb = mmioRead(Aux0Arb);
		if (nowUs() - gAuxDalUs >= (trained ? 200000 : 1000000) && ((arb >> 2) & 3) != 2 && (mmioRead(Aux0Control) & 1)) {
			static bool loggedUntrained;
			if (!trained && !loggedUntrained) {
				loggedUntrained = true;
				LOG("edplink: DP0 TRAINING_COMPLETE clear with stream on (LINK_CNTL %x) -> checking sink anyway", mmioRead(Dp0LinkCntl));
			}
			mmioWrite(Aux0Arb, arb | 0x10000u);  // AUX_SW_USE_AUX_REG_REQ
			if (((mmioRead(Aux0Arb) >> 2) & 3) == 1)
				edpLinkCheck();
			mmioWrite(Aux0Arb, (mmioRead(Aux0Arb) & ~0x10000u) | 0x20000u);  // AUX_SW_DONE_USING_AUX_REG
		}
		IOLockUnlock(gAuxLock);
	}
}

void edpLinkWatchStart() {
	if (gAuxLock || checkKernelArgument("-rmbnolinkwatch"))
		return;
	gAuxLock = IOLockAlloc();
	thread_t t;
	if (kernel_thread_start([](void *, wait_result_t) { edpLinkWatch(); }, nullptr, &t) == KERN_SUCCESS)
		thread_deallocate(t);
	LOG("edplink: watchdog started");
}

// After a PWRSEQ0 revive the panel had lost VDD: check the link now.
void panelRetrain() { gEdpLinkKick = true; }

namespace {

// WindowServer waits up to 60 s in GPUWranglerWaitForPublish until the GPU has its AGDC nub
// (AmdAgdcServices). AMD creates it in AmdRadeonController::powerUp (createAgdc), which only runs on
// the first IOFramebuffer::open. With the svga fb ignored (ignore-ndrv) nothing opens the AMD fb
// before WindowServer, which is the one waiting: auto-1017-2146 WS timed out at 78 s, AGDC came at
// 87 s, panel + login only after that. Open framebuffer 0 ourselves once the accelerator is up, as
// WindowServer did at ~26 s while it still had the svga display (`-rmbnoopen` = off).
// AMDRadeonX6000_AmdRadeonFramebuffer::open(), by symbol (moves between macOS builds). Its
// IOFramebuffer::open() vtable slot (303 on 15.0) is found by scanning, in case IOGraphics grew.
mach_vm_address_t gFbOpen;
constexpr size_t FbOpenSlot = 303, FbOpenSlotMin = 200, FbOpenSlotMax = 420;

void earlyOpen(void *, wait_result_t) {
	IOService *accel = nullptr;
	OSDictionary *matching = nullptr;
	for (int i = 0; i < 300 && !accel; i++) {
		if (gAcceleratorStarted && gAcceleratorService) {
			if (restoredTargetProvider(gAcceleratorService->getProvider())) {
				accel = gAcceleratorService;
				accel->retain();
				break;
			}
		}
		matching = IOService::serviceMatching("AMDRadeonX6000_AMDNavi23GraphicsAccelerator");
		if (matching) {
			auto iter = IOService::getMatchingServices(matching);
			OSSafeReleaseNULL(matching);
			while (auto svc = iter ? OSDynamicCast(IOService, iter->getNextObject()) : nullptr) {
				if (restoredTargetProvider(svc->getProvider())) {
					accel = svc;
					accel->retain();
					break;
				}
			}
			OSSafeReleaseNULL(iter);
		}
		if (!accel) {
			IOSleep(200);
		}
	}
	if (accel) {
		if (!gAcceleratorService) {
			publishAccelerator(accel);
		}
		accel->release();
	} else {
		LOG("open: accelerator service not ready yet, proceeding with fb open");
	}
	IOSleep(1000);
	IOService *fb0 = nullptr;
	for (int i = 0; i < 50 && !fb0; i++) {
		matching = IOService::serviceMatching("AMDRadeonX6000_AmdRadeonFramebuffer");
		auto iter = matching ? IOService::getMatchingServices(matching) : nullptr;
		OSSafeReleaseNULL(matching);
		while (auto svc = iter ? OSDynamicCast(IOService, iter->getNextObject()) : nullptr) {
			auto loc = svc->getLocation();
			if (loc && !strcmp(loc, "0")) {
				fb0 = svc;
				fb0->retain();
				break;
			}
		}
		OSSafeReleaseNULL(iter);
		if (!fb0)
			IOSleep(200);
	}
	if (!fb0) {
		LOG("open: framebuffer 0 not found");
		return;
	}
	auto vt = *reinterpret_cast<mach_vm_address_t **>(fb0);
	size_t slot = 0;
	for (size_t i = FbOpenSlotMin; gFbOpen && i <= FbOpenSlotMax && !slot; i++)
		if (vt[i] == gFbOpen)
			slot = i;
	if (!slot) {
		LOG("open: framebuffer open %llx not in vtable slots %zu-%zu (slot %zu = %llx), not opening", gFbOpen,
		    FbOpenSlotMin, FbOpenSlotMax, FbOpenSlot, vt[FbOpenSlot]);
	} else {
		LOG("open: opening framebuffer 0 before WindowServer (vtable slot %zu)", slot);
		safeModesLog();
		auto r = reinterpret_cast<IOReturn (*)(IOService *)>(vt[slot])(fb0);
		LOG("open: framebuffer 0 open = %x", r);
	}
	fb0->release();
}

// Hardware video (VCN) hangs the whole GPU: Rembrandt's VCN is power gated (our SMU shim ignores
// PowerPlay's power-up request) and Navi's MMHUB 2.0 VM regs (MMVM_CONTEXT2_*, INVALIDATE_ENG6) are
// other regs on MMHUB 2.3, so the VMPT job's invalidate-ack poll never ends -> SDMA0_PAGE hang ->
// resetHardware -> host death (App Store videos auto-1024-1711/1332, AirPlay 1238). Hide the codec
// properties VideoToolbox looks for so it decodes/encodes in software (`-rmbvcn` = keep them).
bool isVideoKey(const char *k) {
	return !strncmp(k, "IOGVA", 5) || !strcmp(k, "IOVARendererID") || !strcmp(k, "IOVARendererSubID") ||
	       !strcmp(k, "IODVDBundleName");
}

// vcn.cpp may publish them again (ctl `on` after a `probe` boot): keep what was removed.
OSDictionary *gSavedVideo;
IORegistryEntry *gVideoAccel;
IOLock *gVideoLock;
volatile bool gVideoShown;

unsigned stripVideoProps(IORegistryEntry *accel) {
	unsigned n = 0;
	auto props = accel->dictionaryWithProperties();
	auto iter = props ? OSCollectionIterator::withCollection(props) : nullptr;
	while (auto key = iter ? OSDynamicCast(OSSymbol, iter->getNextObject()) : nullptr) {
		if (isVideoKey(key->getCStringNoCopy())) {
			if (gSavedVideo && !gSavedVideo->getObject(key))
				if (auto value = props->getObject(key))
					gSavedVideo->setObject(key, value);
			accel->removeProperty(key);
			n++;
		}
	}
	OSSafeReleaseNULL(iter);
	OSSafeReleaseNULL(props);
	return n;
}

void noVideoEngine(void *, wait_result_t) {
	IOService *accel = nullptr;
	for (int i = 0; i < 300 && !accel; i++) {
		if (gAcceleratorStarted && gAcceleratorService) {
			if (restoredTargetProvider(gAcceleratorService->getProvider())) {
				accel = gAcceleratorService;
				accel->retain();
				break;
			}
		}
		auto matching = IOService::serviceMatching("AMDRadeonX6000_AMDNavi23GraphicsAccelerator");
		if (matching) {
			auto iter = IOService::getMatchingServices(matching);
			OSSafeReleaseNULL(matching);
			while (auto svc = iter ? OSDynamicCast(IOService, iter->getNextObject()) : nullptr) {
				if (restoredTargetProvider(svc->getProvider())) {
					accel = svc;
					accel->retain();
					break;
				}
			}
			OSSafeReleaseNULL(iter);
		}
		if (!accel) {
			IOSleep(200);
		}
	}
	if (!accel) {
		LOG("novcn: no accelerator");
		return;
	}
	if (!gAcceleratorService) {
		publishAccelerator(accel);
	}
	// vcn.cpp (ctl /Users/Shared/rmb-vcn.ctl `on`, not blocked): VCN firmware, MMHUB 2.3 and PMFW power are
	// in place, keep the properties. Otherwise hide them, and keep the accelerator and the removed values so
	// vcnVideoProps can publish them later. Both reference the accelerator for the rest of the boot.
	gSavedVideo = OSDictionary::withCapacity(8);
	gVideoLock = IOLockAlloc();
	if (!gSavedVideo || !gVideoLock) {
		LOG("novcn: no memory for the saved properties; hiding them for good");
		OSSafeReleaseNULL(gSavedVideo);
	}
	if (vcnShowVideo()) {
		gVideoShown = true;
		if (gVideoLock)
			gVideoAccel = accel;
		else
			accel->release();
		LOG("novcn: VCN on (vcn.cpp), hardware video properties kept");
		return;
	}
	if (gVideoLock)
		IOLockLock(gVideoLock);
	LOG("novcn: hid %u hardware video properties (VideoToolbox uses software codecs)", stripVideoProps(accel));
	if (gVideoLock) {
		gVideoAccel = accel;
		IOLockUnlock(gVideoLock);
	}
	// The driver may publish more at runtime (engine powerUp); keep them off for 3 min.
	for (int i = 0; i < 180; i++) {
		IOSleep(1000);
		if (gVideoLock)
			IOLockLock(gVideoLock);
		if (!gVideoShown)
			if (auto n = stripVideoProps(accel))
				LOG("novcn: hid %u more video properties at +%d s", n, i + 1);
		if (gVideoLock)
			IOLockUnlock(gVideoLock);
	}
	if (!gVideoLock)
		accel->release();
}

// Brightness: AMD's fb has no 'brgt' display parameter ('parm' = 'pscn', an unused 0 slot, then
// IOFramebuffer's list), so AppleBacklightDisplay got no slider and DAL never sent ABM SET_BACKLIGHT
// (auto-1021-1459). For the panel (fb@0) put 'brgt' in the 0 slot and serve it from BL_PWM
// (`-rmbnobrgt` = off).
constexpr uint32_t AttrBrgt = 'brgt', AttrParm = 'parm';
constexpr uint32_t BrgtMax = 1024;
using GetAttrFn = IOReturn (*)(IOService *, int32_t, uint32_t, uintptr_t *);
using SetAttrFn = IOReturn (*)(IOService *, int32_t, uint32_t, uintptr_t);
mach_vm_address_t orgGetAttr, orgSetAttr;
uint32_t gBrgt = ~0u;

bool isPanelFb(IOService *fb) {
	static IOService *panel;
	if (panel)
		return fb == panel;
	auto loc = fb->getLocation();
	if (loc && !strcmp(loc, "0")) {
		panel = fb;
		return true;
	}
	return false;
}

uint32_t currentBrightness() {
	if (gBrgt <= BrgtMax)
		return gBrgt;
	uint32_t per = mmioRead(BlPwmPeriod), bits = per >> 16 & 0x1f;
	if (!bits || bits > 16)
		bits = 16;
	uint32_t period = per & ((1u << bits) - 1);
	if (!period)
		return BrgtMax;
	uint32_t active = (mmioRead(BlPwmCntl) & 0xffff) >> (16 - bits);
	uint32_t v = ((uint64_t)active * BrgtMax + period / 2) / period;
	return v > BrgtMax ? BrgtMax : v;
}

// AppleBacklight's AppleIntelPanelA (under PNLF) only publishes "brightness"/"linear-brightness"
// (what DisplayServices, the Control Center slider and the F14/F15 keys use) if the fb answers
// getAttributeForConnection(0, 'bklt') = {current, min, max}; it then sets the raw level with
// setAttributeForConnection(0, 'bklt', v) (AppleBacklight 180.11 setDisplay/doIntegerSet).
constexpr uint32_t AttrBklt = 'bklt';
constexpr uint32_t BkltMax = 0xffff;

// USB-C DP-alt (link2) is detected with EDID but never becomes a display (no FB>=1 mode validation).
// Trace each fb's display path (fb+0x81c8: link count +0xd0, link index bytes +0xec + i*0x190,
// connected byte +0x719 = the 'enab' answer) and its getMyLink(0) link (+0x18 index, +0x1c connector
// id, +0x54 connected, +0x58 signal), on every 'enab' change and around connectionChange (`fbpath:`).
constexpr uint32_t AttrEnab = 'enab';
constexpr size_t FbPathOff = 0x81c8;
mach_vm_address_t gGetMyLink, orgConnChange;

void logFbPath(IOService *fb, const char *why) {
	static uint32_t logged;
	if (logged++ >= 160)
		return;
	auto loc = fb->getLocation();
	auto path = *reinterpret_cast<uint8_t **>(reinterpret_cast<uint8_t *>(fb) + FbPathOff);
	if (!path) {
		LOG("fbpath: FB:%s %s: no display path", loc ? loc : "?", why);
		return;
	}
	uint32_t n = *reinterpret_cast<uint32_t *>(path + 0xd0);
	char links[32] = "";
	for (uint32_t i = 0, o = 0; i < n && i < 4; i++)
		o += snprintf(links + o, sizeof(links) - o, " %u", path[0xec + i * 0x190]);
	auto link = gGetMyLink ? reinterpret_cast<uint8_t *>(
		reinterpret_cast<Fn6>(gGetMyLink)(reinterpret_cast<uint64_t>(fb), 0, 0, 0, 0, 0)) : nullptr;
	if (!link) {
		LOG("fbpath: FB:%s %s: path %u links [%s] connected %u, no link", loc ? loc : "?", why, n, links, path[0x719]);
		return;
	}
	LOG("fbpath: FB:%s %s: path %u links [%s] connected %u; link idx %u conn-id %x connected %u signal %x",
		loc ? loc : "?", why, n, links, path[0x719], *reinterpret_cast<uint32_t *>(link + 0x18),
		*reinterpret_cast<uint32_t *>(link + 0x1c), link[0x54], *reinterpret_cast<uint32_t *>(link + 0x58));
}

uint64_t wrapConnChange(uint64_t fb, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
	logFbPath(reinterpret_cast<IOService *>(fb), "connectionChange >>");
	auto r = reinterpret_cast<Fn6>(orgConnChange)(fb, b, c, d, e, f);
	logFbPath(reinterpret_cast<IOService *>(fb), "connectionChange <<");
	return r;
}

// USB-C link2: displaypolicyd spins (~2000 wakes/s) and never gives link2 an fb. Trace what it asks the
// AMD AGDC services (`agdc:`): each new (cmd, result, input) combination with in/out bytes, per-cmd
// counts every 5 s while they change, and the inner config-entry validation / DP link commit results.
mach_vm_address_t orgAgdcVendor, orgAgdcValidate, orgAgdcLinkCommit;
constexpr uint32_t AgdcCmdSlots = 64;
struct AgdcCount {
	uint32_t cmd, n, fails, lastRet;
} gAgdcCount[AgdcCmdSlots];
uint32_t gAgdcSeen[256], gAgdcSeenN, gAgdcLines;
IOSimpleLock *gAgdcLock;

void agdcHex(char *out, size_t outSize, const void *p, size_t n) {
	out[0] = 0;
	if (!p)
		return;
	auto b = static_cast<const uint8_t *>(p);
	for (size_t i = 0, o = 0; i < n && o + 3 < outSize; i++)
		o += snprintf(out + o, outSize - o, (i && !(i % 4)) ? " %02x" : "%02x", b[i]);
}

uint32_t agdcHash(uint32_t cmd, uint32_t ret, const void *p, size_t n) {
	uint32_t h = 2166136261u ^ cmd ^ (ret * 16777619u);
	auto b = static_cast<const uint8_t *>(p);
	for (size_t i = 0; b && i < n; i++)
		h = (h ^ b[i]) * 16777619u;
	return h;
}

uint64_t wrapAgdcVendor(uint64_t self, uint64_t cmd, uint64_t in, uint64_t inSize, uint64_t out, uint64_t outSize,
	uint64_t args) {
	using Fn7 = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
	uint8_t inCopy[64] {};
	size_t inN = in ? (inSize < sizeof(inCopy) ? inSize : sizeof(inCopy)) : 0;
	if (inN)
		memcpy(inCopy, reinterpret_cast<void *>(in), inN);
	auto r = reinterpret_cast<Fn7>(orgAgdcVendor)(self, cmd, in, inSize, out, outSize, args);
	uint32_t c = static_cast<uint32_t>(cmd), ret = static_cast<uint32_t>(r);
	auto h = agdcHash(c, ret, inCopy, inN);
	bool fresh = false;
	IOSimpleLockLock(gAgdcLock);
	for (uint32_t i = 0; i < AgdcCmdSlots; i++) {
		auto &e = gAgdcCount[i];
		if (e.n && e.cmd != c)
			continue;
		e.cmd = c;
		e.n++;
		e.fails += ret != 0;
		e.lastRet = ret;
		break;
	}
	uint32_t i = 0;
	while (i < gAgdcSeenN && gAgdcSeen[i] != h)
		i++;
	if (i == gAgdcSeenN && gAgdcSeenN < 256 && gAgdcLines < 300) {
		gAgdcSeen[gAgdcSeenN++] = h;
		gAgdcLines++;
		fresh = true;
	}
	IOSimpleLockUnlock(gAgdcLock);
	if (fresh) {
		size_t outN = 0;
		if (out && outSize)
			outN = *reinterpret_cast<uint64_t *>(outSize) < 64 ? *reinterpret_cast<uint64_t *>(outSize) : 64;
		char hin[200], hout[200];
		agdcHex(hin, sizeof(hin), inCopy, inN);
		agdcHex(hout, sizeof(hout), out ? reinterpret_cast<void *>(out) : nullptr, outN);
		LOG("agdc: cmd %x ret %x in %llu [%s] out %zu [%s]", c, ret, inSize, hin, outN, hout);
	}
	return r;
}

uint64_t wrapAgdcValidate(uint64_t self, uint64_t entry, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
	auto r = reinterpret_cast<Fn6>(orgAgdcValidate)(self, entry, c, d, e, f);
	static uint32_t logged, lastHash;
	if (entry) {
		auto h = agdcHash(0x10000, static_cast<uint32_t>(r), reinterpret_cast<void *>(entry), 0xcc);
		if (h != lastHash && logged++ < 60) {
			lastHash = h;
			char hx[400];
			agdcHex(hx, sizeof(hx), reinterpret_cast<void *>(entry), 0x60);
			LOG("agdc: validateConfigurationEntry ret %x: %s", static_cast<uint32_t>(r), hx);
		}
	}
	return r;
}

uint64_t wrapAgdcLinkCommit(uint64_t self, uint64_t link, uint64_t cfg, uint64_t d, uint64_t e, uint64_t f) {
	auto r = reinterpret_cast<Fn6>(orgAgdcLinkCommit)(self, link, cfg, d, e, f);
	static uint32_t logged;
	if (logged++ < 60) {
		char hx[200];
		agdcHex(hx, sizeof(hx), cfg ? reinterpret_cast<void *>(cfg) : nullptr, 0x30);
		LOG("agdc: commitConfigurationForLink link %u ret %x: %s", static_cast<uint32_t>(link),
			static_cast<uint32_t>(r), hx);
	}
	return r;
}

// presdiag: AmdFbPresenter frame pipeline counters (internal panel at 0.5 fps while a
// monitor runs on the 5070 Ti = FB:0 handleStuckQueue every ~2 s). Normal path:
// enqueueTransactions (+0xd8 outstanding) -> transactionVUpdateHandler applies (+0xe0)
// -> transactionVBlankHandler retires (dequeue when +0xe0 != 0). +0xa0 = FB index,
// +0xc0 = stuck poll counter (121 = forced drop).
mach_vm_address_t orgPresVbl, orgPresVupd, orgPresEnq;
constexpr uint32_t PresMax = 6;
struct PresStat {
	uint64_t pres;
	uint32_t vbl, vblRetire, vupd, vupdApplied, enq;
	uint64_t lastVbl, gapVbl, lastVupd, gapVupd, durVbl, durVupd, durEnq;
};
PresStat gPres[PresMax];
const OSMetaClass *gPresMeta;

PresStat *presStat(uint64_t p) {
	if (!p || !gPresMeta || reinterpret_cast<OSObject *>(p)->getMetaClass() != gPresMeta)
		return nullptr;
	uint8_t i = *reinterpret_cast<volatile uint8_t *>(p + 0xa0);
	if (i >= PresMax)
		return nullptr;
	gPres[i].pres = p;
	return &gPres[i];
}

uint64_t presQ(uint64_t p, uint32_t off) {
	return *reinterpret_cast<volatile uint64_t *>(p + off);
}

void wrapPresVbl(uint64_t owner, uint64_t arg) {
	auto s = presStat(owner);
	uint64_t t0 = nowUs();
	uint64_t pending = s ? presQ(owner, 0xe0) : 0;
	reinterpret_cast<void (*)(uint64_t, uint64_t)>(orgPresVbl)(owner, arg);
	if (!s)
		return;
	uint64_t t1 = nowUs();
	s->vbl++;
	if (pending && !presQ(owner, 0xe0))
		s->vblRetire++;
	if (s->lastVbl && t0 - s->lastVbl > s->gapVbl)
		s->gapVbl = t0 - s->lastVbl;
	s->lastVbl = t0;
	if (t1 - t0 > s->durVbl)
		s->durVbl = t1 - t0;
}

void wrapPresVupd(uint64_t owner, uint64_t arg) {
	auto s = presStat(owner);
	uint64_t t0 = nowUs();
	uint64_t before = s ? presQ(owner, 0xe0) : 0;
	reinterpret_cast<void (*)(uint64_t, uint64_t)>(orgPresVupd)(owner, arg);
	if (!s)
		return;
	uint64_t t1 = nowUs();
	s->vupd++;
	if (!before && presQ(owner, 0xe0))
		s->vupdApplied++;
	if (s->lastVupd && t0 - s->lastVupd > s->gapVupd)
		s->gapVupd = t0 - s->lastVupd;
	s->lastVupd = t0;
	if (t1 - t0 > s->durVupd)
		s->durVupd = t1 - t0;
}

// curdiag (11-09): user reports a VESA-like cursor (jumps, slow) with a working compositor. 25G229 FB bytes:
// AmdRadeonFramebuffer::setCursorState 0x165f8 only saves the position (AmdCursor vtable +344
// saveCursorPosition) and skips commitCursorPosition when the presenter is enabled; the commit comes from
// HandleVerticalUpdateInterrupt 0xb336 (cursor vtable +352 at 0xb394) on every vupdate, which runs
// AmdCursor::commitCursorUpdates 0x3247a on the workloop. Count each step so one boot says whether
// WindowServer uses the hardware cursor at all (setCursorImage success, setCursorState rate) and whether the
// commits keep up. The old "applied/retired" counters read +0xe0 (224) inside the handlers, but 25G229 retires
// through deferredTransactionComplete or dequeueTransations 0x2c930 off +0xe0/+0xe8: count dequeue calls
// instead (frames retired). -rmbnocurdiag = off.
mach_vm_address_t orgCurSetImage, orgCurSetState, orgCurCommit, orgPresDequeue;
struct CurStat {
	uint32_t img, imgFail, imgLast, state, stateFail, stateLast, stateVis, commit, commitFail, commitLast, deq;
	int32_t x, y;
	uint32_t intOn, intOff, gapMaxMs;  // FB setInterruptState(1/0) calls; longest gap between setCursorState calls
	uint64_t lastStateUs;
};
CurStat gCur;
uint32_t gCurLogs, gTgFills, gTgSkips;  // gTg*: tgfix below
constexpr uint32_t CurdiagCurCtl = 0x3b38, CurdiagCurPos = 0x3b3c;

uint64_t wrapCurSetImage(uint64_t fb, uint64_t img, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
	auto r = reinterpret_cast<Fn6>(orgCurSetImage)(fb, img, c, d, e, f);
	gCur.img++;
	gCur.imgLast = static_cast<uint32_t>(r);
	if (static_cast<uint32_t>(r))
		gCur.imgFail++;
	return r;
}

uint64_t wrapCurSetState(uint64_t fb, uint64_t x, uint64_t y, uint64_t vis, uint64_t e, uint64_t f) {
	auto r = reinterpret_cast<Fn6>(orgCurSetState)(fb, x, y, vis, e, f);
	gCur.state++;
	gCur.stateLast = static_cast<uint32_t>(r);
	if (static_cast<uint32_t>(r))
		gCur.stateFail++;
	if (vis & 1)
		gCur.stateVis++;
	gCur.x = static_cast<int32_t>(x);
	gCur.y = static_cast<int32_t>(y);
	uint64_t now = nowUs();
	if (gCur.lastStateUs && (now - gCur.lastStateUs) / 1000 > gCur.gapMaxMs)
		gCur.gapMaxMs = static_cast<uint32_t>((now - gCur.lastStateUs) / 1000);
	gCur.lastStateUs = now;
	return r;
}

uint64_t wrapCurCommit(uint64_t cur, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
	auto r = reinterpret_cast<Fn6>(orgCurCommit)(cur, b, c, d, e, f);
	gCur.commit++;
	gCur.commitLast = static_cast<uint32_t>(r);
	if (static_cast<uint32_t>(r))
		gCur.commitFail++;
	return r;
}

uint64_t wrapPresDequeue(uint64_t self, uint64_t mask, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
	gCur.deq++;
	return reinterpret_cast<Fn6>(orgPresDequeue)(self, mask, c, d, e, f);
}

void curDiagLog() {
	static CurStat last;
	CurStat s = gCur;
	gCur.gapMaxMs = 0;
	s.lastStateUs = last.lastStateUs;
	if (!memcmp(&s, &last, sizeof(s)))
		return;
	// DCN 3.1.2 HUBP0 CURSOR0_0_CURSOR_CONTROL / _POSITION (same address in 3.0.2: not in dcnremap.hpp; vramcrc.cpp
	// CurCtl): bit0 CURSOR_ENABLE, X [29:16], Y [13:0]. Read-only.
	uint32_t ctl = mmioAvailable() ? mmioRead(CurdiagCurCtl) : 0, pos = mmioAvailable() ? mmioRead(CurdiagCurPos) : 0;
	LOG("curdiag: 2s: setCursorImage %u (fail %u, last %x), setCursorState %u (fail %u, last %x, visible %u, at %d,%d), "
		"commitCursorUpdates %u (fail %u, last %x), presenter dequeue %u; HUBP0 CURSOR_CONTROL %x POSITION %u,%u; "
		"setInterruptState on %u off %u, setCursorState max gap %u ms; tgfix fills %u skips %u",
		s.img - last.img, s.imgFail - last.imgFail, s.imgLast, s.state - last.state, s.stateFail - last.stateFail, s.stateLast,
		s.stateVis - last.stateVis, s.x, s.y, s.commit - last.commit, s.commitFail - last.commitFail, s.commitLast,
		s.deq - last.deq, ctl, (pos >> 16) & 0x3fff, pos & 0x3fff, s.intOn - last.intOn, s.intOff - last.intOff, s.gapMaxMs,
		gTgFills, gTgSkips);
	last = s;
	last.gapMaxMs = 0;
	gCurLogs++;
}

// tgfix (11-09): boot 195852 logged "AmdRadeonControllerNavi23::getVUpdateVBlankInterval() !!! Invalid TG (-1)"
// 1610 times, all from FB0 (the only presenter with transactions, presdiag enq 613 vs none on FB1). 25G229 FB:
// AmdFbPresenter::setVUpdateVBlankInterval 0x2c148 takes the TG from the presenter's AmdFbDisplayPath (+0xb0):
// the first of the path's streams (count +0xd0, 400-byte streams from +0xd8, TG int at stream +4 = path+0xdc)
// with TG >= 0, and hands it to controller vtable +2464 = ControllerNavi2::getVUpdateVBlankInterval 0x4c10, which
// reads OTG register Seg2 + 0x1b87 + tg * 0x80 (tg < 6, else the "Invalid TG" log and e0000001). The stream TG
// is written only by AmdRadeonController::cacheTgIndices 0x4ce20 (DAL getTgIndex 0x6bc98 = dc stream status
// primary OTG, from one controller notify event) and reset to -1 by invalidateCachedTgIndices 0x4cf04,
// initializeDisplayPath 0x1d929 and setOffline. With TG -1 the presenter keeps +0xf0 (vupdate->vblank interval)
// at 0, so processTransactions1stPass 0x2bfc1 puts the flip deadline (+0x58) a whole frame after vblank
// (+0x50 = vblank + vtotal * line time) instead of at vupdate, and retries + logs on every transaction.
// Fix: only when the path has exactly one stream and no TG, use the one OTG whose OTG_CONTROL.OTG_MASTER_EN
// (Seg2 + 0x1b41 + i * 0x80, as anyOtgOn) is set; with no or several running OTGs leave -1 (Apple's fallback).
// Register reads only. -rmbnotgfix = off.
mach_vm_address_t orgPresSetVuInt;
int8_t gTgFix = -1;
uint32_t gTgFixLogs;
constexpr uint32_t PresPathOff = 0xb0, PresVuIntervalOff = 0xf0, PathStreamCount = 0xd0, PathStream0Tg = 0xdc,
	PathStreamSize = 400, PathMaxStreams = 4, PathModeOff = 0x758, PathFlagsOff = 0x718;

uint64_t wrapPresSetVuInt(uint64_t pres) {
	if (gTgFix < 0)
		gTgFix = !checkKernelArgument("-rmbnotgfix");
	uint64_t path = gTgFix ? presQ(pres, PresPathOff) : 0;
	int32_t filled = -1;
	uint32_t mask = 0, n = 0;
	if (path && mmioAvailable()) {
		n = *reinterpret_cast<volatile uint32_t *>(path + PathStreamCount);
		bool have = false;
		for (uint32_t i = 0; i < n && i < PathMaxStreams; i++)
			have |= *reinterpret_cast<volatile int32_t *>(path + PathStream0Tg + i * PathStreamSize) >= 0;
		if (n && n <= PathMaxStreams && !have) {
			for (uint32_t i = 0; i < 4; i++)
				if (mmioRead(Seg2 + 0x1b41 + i * 0x80) & 1)  // OTGn_OTG_CONTROL.OTG_MASTER_EN
					mask |= 1u << i;
			if (n == 1 && mask && !(mask & (mask - 1))) {
				filled = __builtin_ctz(mask);
				*reinterpret_cast<volatile int32_t *>(path + PathStream0Tg) = filled;
				gTgFills++;
			} else {
				gTgSkips++;
			}
		}
	}
	auto r = reinterpret_cast<uint64_t (*)(uint64_t)>(orgPresSetVuInt)(pres);
	if ((filled >= 0 || mask) && gTgFixLogs < 12) {
		gTgFixLogs++;
		uint16_t flags = *reinterpret_cast<volatile uint16_t *>(path + PathFlagsOff);
		uint32_t vst = filled >= 0 ? mmioRead(Seg2 + 0x1b87 + filled * 0x80) : 0;
		LOG("tgfix: FB%u path streams %u mode %x flags %x, OTG running mask %x -> %s TG %d; interval %llu (OTG reg 0x1b87 %x), "
		    "ret %x (fills %u, skips %u)",
		    *reinterpret_cast<volatile uint8_t *>(pres + 0xa0), n, *reinterpret_cast<volatile uint32_t *>(path + PathModeOff),
		    flags, mask, filled >= 0 ? "set" : "left", filled, presQ(pres, PresVuIntervalOff), vst,
		    static_cast<uint32_t>(r), gTgFills, gTgSkips);
	}
	return r;
}

uint64_t wrapPresEnq(uint64_t self, uint64_t mask) {
	auto s = presStat(self);
	uint64_t t0 = nowUs();
	auto r = reinterpret_cast<uint64_t (*)(uint64_t, uint64_t)>(orgPresEnq)(self, mask);
	if (s) {
		s->enq++;
		uint64_t d = nowUs() - t0;
		if (d > s->durEnq)
			s->durEnq = d;
	}
	return r;
}

// wsrv diag (10-31): FB0 vbl/vupd handlers never ran once a monitor was on the 5070 Ti. They are
// only registered by enableTransactions, which runs from setAttribute('wsrv') with
// kIOWSAA_Transactional (0x10). Log every wsrv, enableTransactions result + registration refs
// (+0x158 vbl 'vblx', +0x160 vupd 'vbl '/'vsnc'), setInterruptState calls.
mach_vm_address_t orgFbSetAttr, orgPresEnable, orgFbSetIntState;
uint32_t (*gFbSetPipeIntState)(uint64_t, bool);
uint32_t gWsrvLines, gIntStateLines;
bool gPresSelfEnabled[PresMax];

uint32_t wrapFbSetAttr(uint64_t fb, uint32_t attr, uint64_t val) {
	auto r = reinterpret_cast<uint32_t (*)(uint64_t, uint32_t, uint64_t)>(orgFbSetAttr)(fb, attr, val);
	if (attr == 0x77737276)  // 'wsrv': WindowServer has taken the framebuffers (replayConnect waits for it)
		gWsAttached = true;
	if (attr == 0x77737276 && gWsrvLines < 200) {  // 'wsrv'
		gWsrvLines++;
		uint64_t pres = *reinterpret_cast<volatile uint64_t *>(fb + 0x8a28);
		LOG("presdiag: FB ctl %u setAttribute wsrv %llx (state %llu%s%s) -> %x; presenter %llx vblreg %llx vupdreg %llx",
		    *reinterpret_cast<volatile uint8_t *>(fb + 0x1e0), val, val & 0xf, (val & 0x10) ? " Transactional" : "",
		    (val & 0x400) ? " NonConsole" : "", r, pres, pres ? presQ(pres, 0x158) : 0, pres ? presQ(pres, 0x160) : 0);
	}
	return r;
}

uint32_t wrapPresEnable(uint64_t pres) {
	auto r = reinterpret_cast<uint32_t (*)(uint64_t)>(orgPresEnable)(pres);
	LOG("presdiag: FB%u enableTransactions -> %x, vblreg %llx vupdreg %llx flags100 %x",
	    *reinterpret_cast<volatile uint8_t *>(pres + 0xa0), r, presQ(pres, 0x158), presQ(pres, 0x160),
	    *reinterpret_cast<volatile uint32_t *>(pres + 0x100));
	return r;
}

uint32_t wrapFbSetIntState(uint64_t fb, uint64_t ref, uint32_t state) {
	auto r = reinterpret_cast<uint32_t (*)(uint64_t, uint64_t, uint32_t)>(orgFbSetIntState)(fb, ref, state);
	if (state)
		gCur.intOn++;
	else
		gCur.intOff++;
	if (gIntStateLines < 100) {
		gIntStateLines++;
		LOG("presdiag: FB ctl %u setInterruptState ref %llx state %u -> %x",
		    *reinterpret_cast<volatile uint8_t *>(fb + 0x1e0), ref, state, r);
	}
	return r;
}

void presIhLog() {
	static uint32_t lastC[32], lastS[256];
	char buf[400];
	size_t o = 0;
	for (uint32_t i = 0; i < 256 && o < sizeof(buf) - 16; i++) {
		uint32_t v = gIhDceSrc[i];
		if (v != lastS[i])
			o += snprintf(buf + o, sizeof(buf) - o, " %02x:%u", i, v - lastS[i]);
		lastS[i] = v;
	}
	char cbuf[300];
	size_t co = 0;
	for (uint32_t i = 0; i < 32 && co < sizeof(cbuf) - 16; i++) {
		uint32_t v = gIhClient[i];
		if (v != lastC[i])
			co += snprintf(cbuf + co, sizeof(cbuf) - co, " c%u:%u", i, v - lastC[i]);
		lastC[i] = v;
	}
	buf[o] = cbuf[co] = 0;
	LOG("presdiag: IH 5s: clients%s; DCE src%s | st W%c N%c C%c hP%c hS%c aP%c aS%c L%c hk%d%d%d%d ctl%d acc%d",
	    co ? cbuf : " none", o ? buf : " none", gStage[StWrangler], gStage[StNavi2], gStage[StCtrl],
	    gStage[StHwsProbe], gStage[StHwsStart], gStage[StAccProbe], gStage[StAccStart], gStage[StHWLibs],
	    isKextHooked(0), isKextHooked(1), isKextHooked(2), isKextHooked(3), gControllerStarted ? 1 : 0,
	    gAcceleratorStarted ? 1 : 0);
}

// Fix attempt (same problem): FB has queued transactions but WS never armed its handlers ->
// arm them ourselves once (enableTransactions + setPipeInterruptState(1), like setAttribute
// 'wsrv'|Transactional does). `-rmbnopresenable` off.
void presSelfEnable(uint32_t i, const PresStat &s, uint32_t &quiet) {
	if (gPresSelfEnabled[i] || !orgPresEnable || checkKernelArgument("-rmbnopresenable"))
		return;
	if (!presQ(s.pres, 0xd8) || presQ(s.pres, 0x158)) {
		quiet = 0;
		return;
	}
	if (++quiet < 3)  // 15 s with frames queued and no vbl handler registered
		return;
	gPresSelfEnabled[i] = true;
	uint64_t fb = presQ(s.pres, 0x1650);
	LOG("presdiag: FB%u frames queued but vbl handler never registered (WS sent no Transactional wsrv) -> enabling ourselves, fb %llx",
	    i, fb);
	diskLogSync(1000);
	uint32_t r = reinterpret_cast<uint32_t (*)(uint64_t)>(orgPresEnable)(s.pres);
	uint32_t r2 = 0xffffffff;
	if (!r && fb && gFbSetPipeIntState)
		r2 = gFbSetPipeIntState(fb, true);
	LOG("presdiag: FB%u self enableTransactions -> %x, setPipeInterruptState -> %x, vblreg %llx vupdreg %llx", i, r, r2,
	    presQ(s.pres, 0x158), presQ(s.pres, 0x160));
}

void presDiagThread(void *, wait_result_t) {
	uint32_t lines = 0;
	PresStat last[PresMax] {};
	uint32_t quiet[PresMax] {};
	while (lines < 60 || (orgCurSetState && gCurLogs < 150)) {
		IOSleep(lines == 0 ? 500 : 2000);
		if (orgCurSetState || orgPresDequeue)
			curDiagLog();  // keeps going after the 60 presdiag lines: the cursor is used after login
		if (lines >= 60)
			continue;
		if (!isKextHooked(1) || !isKextHooked(2) || !isKextHooked(3)) {
			scanKmods();
		}
		// No periodic rematch: re-registering VGA@0 every 2 s re-ran probe/start of a failed
		// accelerator against live hardware. Only report state.
		if (lines < 3 && isKextHooked(1) && isKextHooked(2) && isKextHooked(3) && !gAcceleratorStarted)
			IOLog("REM660: [presdiag] accelerator not started yet (line=%u)\n", lines);
		presIhLog();
		lines++;
		for (uint32_t i = 0; i < PresMax; i++)
			if (gPres[i].pres)
				presSelfEnable(i, gPres[i], quiet[i]);
		for (uint32_t i = 0; i < PresMax; i++) {
			PresStat s = gPres[i];
			if (!s.pres || (s.vbl == last[i].vbl && s.vupd == last[i].vupd && s.enq == last[i].enq))
				continue;
			uint64_t now = nowUs();
			LOG("presdiag: FB%u 5s: enq %u, vupd %u (applied %u, max gap %llu ms, max %llu us), "
			    "vbl %u (retired %u, max gap %llu ms, max %llu us), enq max %llu us; now out %llx applied %llx stuck %u, "
			    "last vbl %llu ms ago, vupd %llu ms ago; vblreg %llx vupdreg %llx",
			    i, s.enq - last[i].enq, s.vupd - last[i].vupd, s.vupdApplied - last[i].vupdApplied,
			    s.gapVupd / 1000, s.durVupd, s.vbl - last[i].vbl, s.vblRetire - last[i].vblRetire,
			    s.gapVbl / 1000, s.durVbl, s.durEnq, presQ(s.pres, 0xd8), presQ(s.pres, 0xe0),
			    *reinterpret_cast<volatile uint32_t *>(s.pres + 0xc0),
			    s.lastVbl ? (now - s.lastVbl) / 1000 : 0, s.lastVupd ? (now - s.lastVupd) / 1000 : 0,
			    presQ(s.pres, 0x158), presQ(s.pres, 0x160));
			last[i] = s;
			gPres[i].gapVbl = gPres[i].gapVupd = gPres[i].durVbl = gPres[i].durVupd = gPres[i].durEnq = 0;
			lines++;
		}
	}
}

void agdcCountThread(void *, wait_result_t) {
	uint32_t lastTotal = 0;
	for (uint32_t t = 0; t < 600; t += 5) {
		IOSleep(5000);
		AgdcCount snap[AgdcCmdSlots];
		IOSimpleLockLock(gAgdcLock);
		memcpy(snap, gAgdcCount, sizeof(snap));
		IOSimpleLockUnlock(gAgdcLock);
		uint32_t total = 0;
		for (auto &e : snap)
			total += e.n;
		if (total == lastTotal)
			continue;
		lastTotal = total;
		char line[400];
		size_t o = 0;
		for (auto &e : snap)
			if (e.n && o + 40 < sizeof(line))
				o += snprintf(line + o, sizeof(line) - o, " %x:%u/%u(%x)", e.cmd, e.n, e.fails, e.lastRet);
		LOG("agdc: counts cmd:calls/fails(last ret)%s", line);
	}
}

// Backlight handler diag (2026-11-10, Tahoe 25G229, AppleBacklight 180.12; `-rmbnobldiag` = off). macOS can't
// change the panel brightness (DisplayServicesCanChangeBrightness 0, AppleIntelPanelA publishes only "commit")
// and no 'bklt' request ever reached wrapGetAttr, while the 'brgt' path does dim the panel (BrgtSet test).
// Nothing is routed in AppleBacklight:
// (1) every distinct (get/set, attribute, caller) on the AMD fb is logged once, the caller as
//     AppleBacklight+offset when it lies in that image (AppleIntelPanelA::setDisplay = image +0x1290 asks
//     'bklt' at +0x17e, so its return address is +0x1414);
// (2) at +30/+60/+120 s the AppleIntelPanelA fields its setDisplay writes are read (only if the class size is
//     the 180.12 0x148).
bool gBlDiag;
mach_vm_address_t gBlImage;
size_t gBlImageSize;
IOSimpleLock *gBlDiagLock;
struct BlSeen {
	uint32_t attr;
	bool set;
	uintptr_t ra;
};
BlSeen gBlSeen[128];
uint32_t gBlSeenN, gBlLateLogs;

void blDiagAttr(bool set, IOService *fb, int32_t conn, uint32_t attr, uintptr_t ra, IOReturn r, const uintptr_t *value,
                uintptr_t setValue) {
	if (!gBlDiag || !gBlDiagLock)
		return;
	bool inBl = gBlImage && ra >= gBlImage && ra < gBlImage + gBlImageSize;
	bool fresh = false;
	// interrupts off while held: no deadlock even if an fb attribute call ever comes from interrupt context
	IOInterruptState is = IOSimpleLockLockDisableInterrupt(gBlDiagLock);
	uint32_t i = 0;
	while (i < gBlSeenN && !(gBlSeen[i].attr == attr && gBlSeen[i].set == set && gBlSeen[i].ra == ra))
		i++;
	if (i == gBlSeenN) {
		if (gBlSeenN < arrsize(gBlSeen)) {
			gBlSeen[gBlSeenN++] = {attr, set, ra};
			fresh = true;
		} else if (inBl && gBlLateLogs < 32) {  // table full: still show AppleBacklight's calls
			gBlLateLogs++;
			fresh = true;
		}
	}
	IOSimpleLockUnlockEnableInterrupt(gBlDiagLock, is);
	if (!fresh)
		return;
	char cc[5] {static_cast<char>(attr >> 24), static_cast<char>(attr >> 16), static_cast<char>(attr >> 8),
	            static_cast<char>(attr), 0};
	for (size_t k = 0; k < 4; k++)
		if (cc[k] < 0x20 || cc[k] > 0x7e)
			cc[k] = '.';
	char who[40];
	if (inBl)
		snprintf(who, sizeof(who), "AppleBacklight+0x%lx", static_cast<unsigned long>(ra - gBlImage));
	else
		snprintf(who, sizeof(who), "other");
	auto loc = fb ? fb->getLocation() : nullptr;
	if (set)
		LOG("bldiag: set FB:%s conn %d '%s' %08x = %lx -> %x from %s", loc ? loc : "?", conn, cc, attr,
		    static_cast<unsigned long>(setValue), r, who);
	else if (value && r == kIOReturnSuccess && (attr == AttrBklt || attr == AttrBrgt))
		LOG("bldiag: get FB:%s conn %d '%s' -> %x {%lu, %lu, %lu} from %s", loc ? loc : "?", conn, cc, r,
		    static_cast<unsigned long>(value[0]), static_cast<unsigned long>(value[1]),
		    static_cast<unsigned long>(value[2]), who);
	else
		LOG("bldiag: get FB:%s conn %d '%s' %08x -> %x {%lx} from %s", loc ? loc : "?", conn, cc, attr, r,
		    value && r == kIOReturnSuccess ? static_cast<unsigned long>(value[0]) : 0ul, who);
}

void blDiagSnapshot(uint32_t at) {
	auto match = IOService::serviceMatching("AppleIntelPanelA");
	if (!match)
		return;
	auto it = IOService::getMatchingServices(match);
	match->release();
	uint32_t n = 0;
	while (auto s = it ? OSDynamicCast(IOService, it->getNextObject()) : nullptr) {
		n++;
		auto mc = s->getMetaClass();
		if (!mc || strcmp(mc->getClassName(), "AppleIntelPanelA") || mc->getClassSize() != 0x148) {
			LOG("bldiag: +%us panel #%u class %s size %x: not the 180.12 layout, fields not read", at, n,
			    mc ? mc->getClassName() : "?", mc ? mc->getClassSize() : 0);
			continue;
		}
		auto p = reinterpret_cast<const uint8_t *>(s);
		auto u16 = [p](size_t o) { return static_cast<uint32_t>(*reinterpret_cast<const uint16_t *>(p + o)); };
		auto u32 = [p](size_t o) { return *reinterpret_cast<const uint32_t *>(p + o); };
		auto set = [p](size_t o) { return *reinterpret_cast<const void *const *>(p + o) ? "set" : "0"; };
		// 180.12: +0x108 _UID (start); setDisplay: +0x120 IONDRVFramebuffer (no 'bklt'), +0x128 IOFramebuffer
		// answering 'bklt', +0x130 level table of +0xf2 (+1) u16, +0x144 'bklt' max (0x56c default),
		// +0x13c alias, +0x142 alias match, +0x143 cleared on success, +0x138 saved level index.
		LOG("bldiag: +%us panel uid %u: ndrvFb %s bkltFb %s table %s count %u (f0 %x f6 %x) max %x idx %x alias %x "
		    "match %u pending %u flags %x/%x/%x/%x/%x",
		    at, u32(0x108), set(0x120), set(0x128), set(0x130), u16(0xf2), u16(0xf0), u16(0xf6), u32(0x144),
		    u32(0x138), u32(0x13c), p[0x142], p[0x143], p[0x10c], p[0x10e], p[0x110], p[0x112], p[0x115]);
		auto table = *reinterpret_cast<const uint16_t *const *>(p + 0x130);
		uint32_t cnt = u16(0xf2);
		if (table && cnt && cnt <= 0x400)
			LOG("bldiag: +%us panel table[0] %x [1] %x [%u] %x [%u] %x", at, table[0], table[1], cnt / 2, table[cnt / 2],
			    cnt, table[cnt]);
		// copyProperty: a referenced copy, so a concurrent setProperty can't free it under us (table: 180.12
		// allocates 2 * count + 2 bytes at +0x3d3, so table[count] is in bounds; count <= 0x400 at +0x3c2)
		auto paramsObj = s->copyProperty("IODisplayParameters");
		auto params = OSDynamicCast(OSDictionary, paramsObj);
		LOG("bldiag: +%us panel IODisplayParameters %u keys, brightness %s, linear-brightness %s, ApplePanel %s, "
		    "ApplePanels %s", at, params ? params->getCount() : 0,
		    params && params->getObject("brightness") ? "yes" : "no",
		    params && params->getObject("linear-brightness") ? "yes" : "no", s->getProperty("ApplePanel") ? "yes" : "no",
		    s->getProperty("ApplePanels") ? "yes" : "no");
		OSSafeReleaseNULL(paramsObj);
	}
	if (it)
		it->release();
	if (!n)
		LOG("bldiag: +%us no AppleIntelPanelA instance", at);
}

void blDiagThread(void *, wait_result_t) {
	static const uint32_t ats[] {30, 60, 120};
	uint32_t slept = 0;
	for (uint32_t at : ats) {
		IOSleep((at - slept) * 1000);
		slept = at;
		blDiagSnapshot(at);
	}
}

IOReturn getAttrImpl(IOService *fb, int32_t conn, uint32_t attr, uintptr_t *value);
IOReturn setAttrImpl(IOService *fb, int32_t conn, uint32_t attr, uintptr_t value);

IOReturn wrapGetAttr(IOService *fb, int32_t conn, uint32_t attr, uintptr_t *value) {
	auto ra = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
	IOReturn r = getAttrImpl(fb, conn, attr, value);
	blDiagAttr(false, fb, conn, attr, ra, r, value, 0);
	return r;
}

IOReturn wrapSetAttr(IOService *fb, int32_t conn, uint32_t attr, uintptr_t value) {
	auto ra = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
	IOReturn r = setAttrImpl(fb, conn, attr, value);
	blDiagAttr(true, fb, conn, attr, ra, r, nullptr, value);
	return r;
}

IOReturn getAttrImpl(IOService *fb, int32_t conn, uint32_t attr, uintptr_t *value) {
	if (attr == AttrBklt && conn == 0 && value && isPanelFb(fb)) {
		value[0] = currentBrightness() * BkltMax / BrgtMax;
		value[1] = 0;
		value[2] = BkltMax;
		static uint32_t logged;
		if (logged++ < 8)
			LOG("bl: bklt get -> %lu of %u", value[0], BkltMax);
		return kIOReturnSuccess;
	}
	if (attr == AttrBrgt && conn == 0 && value && isPanelFb(fb)) {
		value[0] = currentBrightness();
		value[1] = 0;
		value[2] = BrgtMax;
		return kIOReturnSuccess;
	}
	auto r = reinterpret_cast<GetAttrFn>(orgGetAttr)(fb, conn, attr, value);
	if (attr == AttrEnab && value) {
		if (value[0] && r == kIOReturnSuccess)
			gFbEnabledSeen = true;
		static uintptr_t last[8] = {2, 2, 2, 2, 2, 2, 2, 2};
		auto loc = fb->getLocation();
		uint32_t i = loc ? (loc[0] - '0') & 7 : 7;
		if (value[0] != last[i]) {
			last[i] = value[0];
			LOG("fbpath: FB:%s enab = %lu (r %x)", loc ? loc : "?", value[0], r);
			logFbPath(fb, "enab");
		}
	}
	if (attr == AttrParm && conn == 0 && r == kIOReturnSuccess && value && !value[1] && isPanelFb(fb)) {
		value[1] = AttrBrgt;
		static uint32_t logged;
		if (logged++ < 4)
			LOG("bl: panel fb parameters += brgt (now %u of %u)", currentBrightness(), BrgtMax);
	}
	return r;
}

IOReturn setAttrImpl(IOService *fb, int32_t conn, uint32_t attr, uintptr_t value) {
	if (attr == AttrBklt && conn == 0 && isPanelFb(fb)) {
		uint32_t v = value > BkltMax ? BkltMax : static_cast<uint32_t>(value);
		gBrgt = (v * BrgtMax + BkltMax / 2) / BkltMax;
		static uint32_t logged;
		if (logged++ < 40)
			LOG("bl: bklt set %lu", value);
		if (!blRampSet(v))
			setBacklight(static_cast<uint32_t>((uint64_t)v * 0x10000 / BkltMax));
		return kIOReturnSuccess;
	}
	if (attr == AttrBrgt && conn == 0 && isPanelFb(fb)) {
		gBrgt = value > BrgtMax ? BrgtMax : static_cast<uint32_t>(value);
		static uint32_t logged;
		if (logged++ < 40)
			LOG("bl: brgt set %lu", value);
		blRampSync(static_cast<uint32_t>((uint64_t)gBrgt * 0x10000 / BrgtMax));
		setBacklight(static_cast<uint32_t>((uint64_t)gBrgt * 0x10000 / BrgtMax));
		return kIOReturnSuccess;
	}
	return reinterpret_cast<SetAttrFn>(orgSetAttr)(fb, conn, attr, value);
}

// AppleIntelPanel::setDisplay looks the panel up in its personality's "ApplePanels" by
// F<_UID>{P,I,T..}<DisplayProductID> keys. The GPD panel has none, and the "Default" fallback stores
// a NULL panel entry (setObject fails right after ApplePanels was removed) → setDisplay false →
// AppleIntelPanelA publishes only "commit", no "brightness" (auto-1021-1631). Add an F<uid>T<pid>
// table first (`-rmbnopanel` = off). Values are the raw 'bklt' levels sent to wrapSetAttr.
using PanelSetDisplayFn = bool (*)(IOService *, IOService *);
mach_vm_address_t orgPanelSetDisplay;
constexpr size_t PanelUidOff = 0x108;  // AppleIntelPanel _UID field (AppleBacklight 180.11 start)
// AGDCBacklightControl F10T9c84 curve (max 0x375), scaled to 0..BkltMax below.
const uint16_t kPanelCurve[] {0x0000, 0x002f, 0x0036, 0x0040, 0x004b, 0x0058, 0x0067, 0x0077, 0x008b,
                              0x00af, 0x00dd, 0x0118, 0x0160, 0x01ba, 0x022c, 0x02bf, 0x0375};

void addPanelEntry(IOService *panel, IOService *display, char *key, size_t keySize) {
	auto pid = OSDynamicCast(OSNumber, display->getProperty("DisplayProductID"));
	if (!pid)
		return;
	uint32_t uid = *reinterpret_cast<uint32_t *>(reinterpret_cast<uint8_t *>(panel) + PanelUidOff);
	snprintf(key, keySize, "F%uT%04x", uid, pid->unsigned32BitValue());
	// Registry-owned collections panic on in-place change ("Trying to change a collection in the
	// registry", auto-1021-1643) → build a copy and set it as the new property.
	auto cur = OSDynamicCast(OSDictionary, panel->getProperty("ApplePanels"));
	if (!cur && panel->getProperty("ApplePanel"))
		return;
	if (cur && cur->getObject(key))
		return;
	auto panels = cur ? OSDictionary::withDictionary(cur, cur->getCount() + 1) : OSDictionary::withCapacity(1);
	if (!panels)
		return;
	constexpr size_t n = arrsize(kPanelCurve);
	uint8_t buf[2 + 2 * n] {0, n};
	for (size_t i = 0; i < n; i++) {
		uint32_t v = kPanelCurve[i] * BkltMax / kPanelCurve[n - 1];
		buf[2 + 2 * i] = v >> 8;
		buf[3 + 2 * i] = v & 0xff;
	}
	// Kept retained forever: setDisplay takes the entry unretained, then removeProperty("ApplePanels")
	// frees our copy (only the registry held it) → setObject("ApplePanel", freed data) panicked
	// (auto-1021-1647). Apple's personality dict is kept alive by the catalogue, ours isn't.
	static OSData *data;
	if (!data)
		data = OSData::withBytes(buf, sizeof(buf));
	if (data) {
		panels->setObject(key, data);
		panel->setProperty("ApplePanels", panels);
	}
	panels->release();
	if (!data)
		return;
	LOG("bl: ApplePanels += %s (%zu levels)", key, n);
}

bool wrapPanelSetDisplay(IOService *panel, IOService *display) {
	char key[32] {};
	if (panel && display)
		addPanelEntry(panel, display, key, sizeof(key));
	bool r = reinterpret_cast<PanelSetDisplayFn>(orgPanelSetDisplay)(panel, display);
	LOG("bl: AppleIntelPanel::setDisplay %s = %d", key[0] ? key : "(no DisplayProductID)", r);
	return r;
}

// Backlight smoother (2026-11-12, user: "sliderul de luminozitate ... topaie putin. fa-l fluid").
// corebrightnessd turns each F5/F6 press (1/16 of the range) into 10 'bklt' sub-steps but sends them
// back to back, ~0.5 ms apart (unified log 11:26:51, BezelServices BrightnessDecrease_Down), so BL_PWM
// took every 1/16 step at once. Like WhateverGreen's IGFX backlight smoother: 'bklt' only moves the
// target, and a thread walks BL_PWM there every BlRampTickMs over gBlRampMs, from wherever it is,
// linearly along the panel curve (the F10T0aa8 points above: ~perceptual, roughly geometric in duty).
// A new target mid-ramp restarts from the current position, so held keys and slider drags glide.
// Only while the CPU owns PWRSEQ0 (BL_PWM_PERIOD readable); otherwise, and for the first set after
// boot, the old direct write (with its revive / firmware path). DAL's ABM SET_BACKLIGHT and 'brgt'
// writes cancel the ramp (blRampSync). Lock order: gInboxLock -> gBlRampLock; the thread never calls
// setBacklight with gBlRampLock held. rmbblramp=<ms> (default 50, 0 = off), -rmbnoblramp = off.
// v2 (2026-11-12, user: "merge cu intarziere / sacadat"): the linear 200 ms ramp restarted at every
// 'bklt' set, so held F5/F6 trailed about one step behind and each restart changed speed. Now an
// exponential follower in curve position: each tick moves gBlRampK/1024 of the remaining distance
// (time constant gBlRampMs), so a change starts on the next tick and eases out (~86 % after 2 tau),
// and at least BlRampMinStep per tick so the tail ends (one step: ~25 ticks at tau 50 ms).
constexpr uint32_t BlRampTickMs = 8, BlRampSeg = 4096, BlRampMinStep = 32;
uint32_t gBlRampMs = 50;
uint32_t gBlRampK = 1024;  // set from gBlRampMs at start: 1024 * (1 - exp(-tick / tau))
IOLock *gBlRampLock;
struct BlRamp {
	bool active, thread;
	uint32_t pos, to;      // curve positions (segment i = [i, i + 1] * BlRampSeg)
	uint32_t target, last; // 'bklt' units; last = ~0u until the first set
	uint32_t ticks, logged;
} gRamp {false, false, 0, 0, 0, ~0u, 0, 0};

uint32_t blCurve(size_t i) {
	constexpr size_t n = arrsize(kPanelCurve);
	return kPanelCurve[i] * BkltMax / kPanelCurve[n - 1];
}

uint32_t blToPos(uint32_t v) {
	constexpr size_t n = arrsize(kPanelCurve);
	for (size_t i = 0; i + 1 < n; i++) {
		uint32_t a = blCurve(i), b = blCurve(i + 1);
		if (v <= b)
			return static_cast<uint32_t>(i * BlRampSeg + (v > a && b > a ? (uint64_t)(v - a) * BlRampSeg / (b - a) : 0));
	}
	return static_cast<uint32_t>((n - 1) * BlRampSeg);
}

uint32_t blFromPos(uint32_t p) {
	constexpr size_t n = arrsize(kPanelCurve);
	size_t i = p / BlRampSeg;
	if (i >= n - 1)
		return blCurve(n - 1);
	uint32_t a = blCurve(i), b = blCurve(i + 1);
	return a + static_cast<uint32_t>((uint64_t)(b - a) * (p % BlRampSeg) / BlRampSeg);
}

uint32_t blLevel(uint32_t v) { return static_cast<uint32_t>((uint64_t)v * 0x10000 / BkltMax); }

void blRampThread(void *, wait_result_t) {
	IOLockLock(gBlRampLock);
	for (;;) {
		while (!gRamp.active)
			IOLockSleep(gBlRampLock, &gRamp, THREAD_UNINT);
		int64_t d = (int64_t)gRamp.to - (int64_t)gRamp.pos, ad = d < 0 ? -d : d;
		int64_t step = ad * gBlRampK / 1024;
		if (step < BlRampMinStep)
			step = ad < BlRampMinStep ? ad : BlRampMinStep;
		gRamp.pos = static_cast<uint32_t>((int64_t)gRamp.pos + (d < 0 ? -step : step));
		bool done = gRamp.pos == gRamp.to || !cpuOwnsPwm();
		uint32_t v = done ? gRamp.target : blFromPos(gRamp.pos);
		gRamp.ticks++;
		if (done) {
			gRamp.active = false;
			if (gRamp.logged < 60)
				LOG("bl: ramp done at %u after %u ticks%s", v, gRamp.ticks, cpuOwnsPwm() ? "" : " (PWRSEQ0 not CPU-owned)");
		}
		bool write = v != gRamp.last;
		gRamp.last = v;
		IOLockUnlock(gBlRampLock);
		if (write)
			setBacklight(blLevel(v));
		if (!done)
			IOSleep(BlRampTickMs);
		IOLockLock(gBlRampLock);
	}
}

// 'bklt' set from AppleIntelPanelA. false = not ramped, the caller writes the PWM itself.
bool blRampSet(uint32_t v) {
	if (!gBlRampMs || !gBlRampLock)
		return false;
	bool owned = cpuOwnsPwm();
	IOLockLock(gBlRampLock);
	if (!owned || gRamp.last == ~0u) {
		gRamp.active = false;
		gRamp.last = v;
		IOLockUnlock(gBlRampLock);
		return false;
	}
	if (!gRamp.thread) {
		thread_t t;
		if (kernel_thread_start(blRampThread, nullptr, &t) != KERN_SUCCESS) {
			gRamp.last = v;
			IOLockUnlock(gBlRampLock);
			LOG("bl: backlight smoother thread failed to start, direct writes");
			return false;
		}
		thread_deallocate(t);
		gRamp.thread = true;
	}
	if (!gRamp.active && v == gRamp.last) {
		IOLockUnlock(gBlRampLock);
		return true;
	}
	if (!gRamp.active) {
		// idle: start from the level last written; mid-ramp: keep going from where it is
		gRamp.pos = blToPos(gRamp.last);
		gRamp.ticks = 0;
		if (gRamp.logged < 60) {
			gRamp.logged++;
			LOG("bl: ramp %u -> %u (tau %u ms)", gRamp.last, v, gBlRampMs);
		}
	}
	gRamp.to = blToPos(v);
	gRamp.target = v;
	gRamp.active = true;
	IOLockWakeup(gBlRampLock, &gRamp, true);
	IOLockUnlock(gBlRampLock);
	return true;
}

// Someone else wrote the PWM (level = u16.16 duty): stop the ramp and start the next one from there.
void blRampSync(uint32_t level) {
	if (!gBlRampLock)
		return;
	IOLockLock(gBlRampLock);
	gRamp.active = false;
	gRamp.last = static_cast<uint32_t>((uint64_t)(level > 0x10000 ? 0x10000 : level) * BkltMax / 0x10000);
	IOLockUnlock(gBlRampLock);
}

// extreplay (2026-11-11, Lenovo 82TL + USB-C hub "USB Type-C Digital AV Adapter", DP alt -> HDMI PCON):
// an external DP sink connected at boot is detected before AGDC enables the controller, so its connect
// event is dropped like link 0's (see replayConnect). Every later detectLink then returns -1 = no change
// (cached +0x494 connected and +0x4a0 EDID match) and detectDisplayOnLink (FB 0x4e51e) only notifies AGDC
// (ctrl+31040 vt 2248) when detect != -1, so no framebuffer ever gets the display (disklog 2026-11-11 01:06
// boot: link 1 detect(reason 3) = ffffffff, signal 20, EDID 4c2d/0f90 at 208 s). Once WindowServer has
// the framebuffers and the panel is lit (+10 s), forget the cache of each connected, non-fixed DP SST link
// >= 1 and run detectDisplayOnLink on that link alone; link 0 (panel) is never touched. Only if no
// framebuffer other than FB:0 has a display path: re-detecting a link that is in use swapped its sink under
// WindowServer (auto-1020-1205 panic). One attempt per boot, never repeated.
// Crash guard: ExtStatePath = "pending" (full fsync) before the replay, "ok" 60 s after it. A boot that finds
// "pending" (panic, hang or power-off during the last attempt) writes "blocked" and does nothing; "blocked"
// stays until ExtArmPath is newer than the state file (`touch /Users/Shared/rmb-extdp-arm`, no sudo).
// No state file can be written -> nothing done. -rmbnoextreplay: off. Log "extreplay:".
constexpr const char *ExtStatePath = "/private/var/log/rmb-extdp.state";
constexpr const char *ExtArmPath = "/Users/Shared/rmb-extdp-arm";

// A framebuffer other than the panel's (FB:0) has a display path (fb+FbPathOff: link count +0xd0,
// connected +0x719, see logFbPath). Also true when no framebuffer can be listed.
bool extFbInUse(char *why, size_t n) {
	auto matching = IOService::serviceMatching("AMDRadeonX6000_AmdRadeonFramebuffer");
	auto iter = matching ? IOService::getMatchingServices(matching) : nullptr;
	OSSafeReleaseNULL(matching);
	unsigned fbs = 0;
	bool used = false;
	while (auto fb = iter ? OSDynamicCast(IOService, iter->getNextObject()) : nullptr) {
		fbs++;
		auto loc = fb->getLocation();
		if (!loc || !strcmp(loc, "0"))
			continue;
		auto path = *reinterpret_cast<uint8_t **>(reinterpret_cast<uint8_t *>(fb) + FbPathOff);
		if (path && reinterpret_cast<uintptr_t>(path) < 0xffffff8000000000ULL) {  // not a kernel pointer
			snprintf(why, n, "FB:%s display path pointer %p looks wrong", loc, path);
			used = true;
			break;
		}
		uint32_t links = path ? *reinterpret_cast<uint32_t *>(path + 0xd0) : 0;
		if (path && (links || path[0x719])) {
			snprintf(why, n, "FB:%s has a display path (%u links, connected %u)", loc, links, path[0x719]);
			used = true;
			break;
		}
	}
	OSSafeReleaseNULL(iter);
	if (!fbs && !used) {
		snprintf(why, n, "no AMD framebuffer listed");
		used = true;
	}
	return used;
}

void extReplay(void *, wait_result_t) {
	uint32_t s = 0;
	for (; s < 180 && !(gWsAttached && gFbEnabledSeen); s++)
		IOSleep(1000);
	uint64_t ctrl = gDalCtrl;
	if (!gWsAttached || !gFbEnabledSeen || !ctrl) {
		LOG("extreplay: WindowServer %d / panel display %d not seen within 180 s, nothing done", gWsAttached,
			gFbEnabledSeen);
		return;
	}
	IOSleep(10000);
	uint32_t count = *reinterpret_cast<volatile uint8_t *>(ctrl + 288), mask = 0;
	for (uint32_t i = 1; i < count && i < 8; i++) {
		auto l = reinterpret_cast<uint8_t *>(reinterpret_cast<Fn6>(gGetLink)(ctrl, i, 0, 0, 0, 0));
		if (!l)
			break;
		uint32_t sig = linkSignal(i);
		bool want = !(l[0x47] & 1) && l[0x494] && l[0x54] && sig == 0x20;
		if (l[0x494] || l[0x54])
			LOG("extreplay: link%u signal %x cached %u connected %u fixed %u -> %s", i, sig, l[0x494], l[0x54],
				l[0x47] & 1, want ? "candidate" : "left alone (not a connected DP SST link)");
		if (want)
			mask |= 1u << i;
	}
	if (!mask) {
		LOG("extreplay: +%u s no connected external DP link, nothing to do", s + 10);
		return;
	}
	char why[96] = "";
	if (extFbInUse(why, sizeof(why))) {
		LOG("extreplay: %s, nothing done", why);
		return;
	}
	char st[96];
	int64_t stT = 0, armT = 0;
	bool have = rmbFileRead(ExtStatePath, st, sizeof(st), &stT);
	bool armed = rmbFileRead(ExtArmPath, nullptr, 0, &armT) && armT > stT;
	if (have && !armed && (!strncmp(st, "pending", 7) || !strncmp(st, "blocked", 7))) {
		if (!strncmp(st, "pending", 7))
			rmbFileWrite(ExtStatePath, "blocked: the last extreplay never reached ok (panic, hang or power-off)\n");
		LOG("extreplay: crash guard: last attempt never finished (state \"%.7s\"), nothing done; "
			"`touch %s` to allow one more try", st, ExtArmPath);
		return;
	}
	char line[96];
	snprintf(line, sizeof(line), "pending: links %x%s\n", mask, armed ? " (re-armed)" : "");
	if (!rmbFileWrite(ExtStatePath, line)) {
		LOG("extreplay: cannot write %s (no crash guard), nothing done", ExtStatePath);
		return;
	}
	for (uint32_t i = 1; i < 8; i++) {
		if (!(mask & 1u << i))
			continue;
		auto l = reinterpret_cast<uint8_t *>(reinterpret_cast<Fn6>(gGetLink)(ctrl, i, 0, 0, 0, 0));
		if (!l || !l[0x494] || !l[0x54]) {
			LOG("extreplay: link%u changed before the replay, left alone", i);
			continue;
		}
		l[0x494] = 0;
		memset(l + 0x4a0, 0, 0x400);
		auto d = reinterpret_cast<Fn6>(org_detectDisplayOnLink)(ctrl, i, 0, 0, 0, 0);
		LOG("extreplay: link%u forgot cached connect + EDID, detectDisplayOnLink = %llx (event %d; 0 = AGDC took it)",
			i, d & 0xFFFFFFFF, *reinterpret_cast<volatile int32_t *>(ctrl + 24352 + i * 4ULL));
	}
	bool took = false;
	for (uint32_t t = 1; t <= 60; t++) {
		IOSleep(1000);
		if (!took && extFbInUse(why, sizeof(why))) {
			took = true;
			LOG("extreplay: +%u s %s", t, why);
		}
		if (t == 30 && !took)
			LOG("extreplay: +30 s no framebuffer has the external display yet");
	}
	snprintf(line, sizeof(line), "ok: links %x, framebuffer %s\n", mask, took ? "yes" : "no");
	LOG("extreplay: 60 s stable, state ok (framebuffer %s)", took ? "yes" : "no");
	rmbFileWrite(ExtStatePath, line);
}

// extbind (2026-11-11, Lenovo 82TL + USB-C hub). extreplay proved the replayed Insert reaches AGDP
// ("Port: 2 Event:Insert", 01:31:28) but displaypolicyd then sends no AGDC command at all (counts frozen
// after 30 s), the same dead end as link 0 on 2026-11-09 under iMac20,1: no commitConfiguration (0x1801)
// ever binds a link to a framebuffer. Link 0 works only because it is fixed (CFG_LINK_FIXED_MAP), so
// enableController 0x14f6c -> initializeFixedDisplayBinding 0x151e6 binds it to FB:0 without AGDC; that
// function returns at once for FB index (+480) != 0. Do the same for FB:1 with the connected external DP
// link, exactly as 0x151e6 does for FB:0:
//   link->setStreamState(0, 0) (vt 440 = AmdLink::setStreamState 0x4632a: only stores +3344 for state 0),
//   connected = link->isConnected() (vt 392, 0x4645c), both display paths (fb+33232, fb+33224):
//   +1817 connected, +208 link count 1, +236 link index; fb+35236 |= 0x20 (fixed display).
// Also mark the link fixed (+0x47, getFeature(13)): handleHotPlug 0x1986a then sends fixed links to the
// 'dci ' connection change instead of AGDC, and connectionChange 0x16224 -> handleFixedDisplayChangeEvent
// 0x16434 refreshes +1817 from getMyLink(0) = this framebuffer's own link (so unplug/replug follows).
// (AmdLink vtable from SystemKernelExtensions.kc, FB LC_UUID C3FA8B38: isConnected 0x4645c at kc 0xc1f18d8
// = slot 392; slot 440 = setStreamState 0x4632a, 304 detectLink, 336 getFeature.)
// Only FB:1, once per boot, only a non-fixed link >= 1 with DP SST signal (detect status 0x20, or no status
// yet and DP connector 0x13), only if FB:1 has no display path yet. A connected link wins; with nothing
// connected FB:1 is pre-bound to the first such port (hot plug, 2026-11-11: the hub worked only when present
// at boot): the binding is the same, only connected = isConnected() = 0, and a later plug takes the fixed
// link route handleHotPlug -> detectLink -> 'dci ' -> IOGraphics kConnectionChanged on every FB ->
// connectionChange -> handleFixedDisplayChangeEvent (+1817 from isConnected) -> path copy +
// reportCapabilities, so WindowServer sees FB:1 come online (the 02:04 boot already showed +1817 going
// 0 -> 1 on FB:1 through this route). Unplug goes the same way to 0. -rmbnoextprebind: old behaviour
// (bind only when connected at FB:1 enable).
// -rmbextbindhdmi (off by default, UNTESTED: the 82TL has no HDMI port) also accepts a native HDMI port:
// DAL signal HDMI type A (4), or no status yet and connector HDMI type A (0x0c). Meant for other 660M laptops.
// Crash guard (own state file, same re-arm file as extreplay): "pending" is written with a full fsync by
// a kernel thread (enableController runs in WindowServer's context, which may not write /private/var/log)
// before binding; "ok" 60 s later (30 s when pre-bound with nothing connected). After that every connect
// of the bound link writes "pending: hotplug" again (see extBindWatch) and "ok" 30 s later or on unplug.
// A boot that finds "pending" writes "blocked" and binds nothing until
// `touch /Users/Shared/rmb-extdp-arm`. No state written within 3 s -> nothing bound. -rmbnoextbind: off.
constexpr const char *ExtBindStatePath = "/private/var/log/rmb-extbind.state";
constexpr size_t FbIndexOff = 480, FbPath1Off = 33232, FbFlagsOff = 35236, FbCtrlOff = 35392;
mach_vm_address_t orgInitFixedBinding;
uint64_t gFbBound;
struct ExtBindGuard {
	volatile int done, abandoned;
	volatile bool allow;
	bool pre;  // pre-bound for hot plug: nothing connected at FB:1 enable
	uint32_t link;
	uint8_t *linkPtr;
	char why[112];
	char note[192];  // the link choice, logged again from extBindWatch (WindowServer-context lines get lost)
} gBind;

void extBindGuardThread(void *, wait_result_t) {
	char st[96];
	int64_t stT = 0, armT = 0;
	bool have = rmbFileRead(ExtBindStatePath, st, sizeof(st), &stT);
	bool armed = rmbFileRead(ExtArmPath, nullptr, 0, &armT) && armT > stT;
	bool allow = false;
	if (have && !armed && (!strncmp(st, "pending", 7) || !strncmp(st, "blocked", 7))) {
		if (!strncmp(st, "pending", 7))
			rmbFileWrite(ExtBindStatePath, "blocked: the last extbind never reached ok (panic, hang or power-off)\n");
		snprintf(gBind.why, sizeof(gBind.why), "crash guard: last bind never finished (state \"%.7s\"); `touch %s`",
			st, ExtArmPath);
	} else if (gBind.abandoned) {
		snprintf(gBind.why, sizeof(gBind.why), "guard too late, nothing written");
	} else {
		char line[96];
		snprintf(line, sizeof(line), "pending: link %u%s%s\n", gBind.link, gBind.pre ? " (pre-bound, nothing connected)" : "",
			armed ? " (re-armed)" : "");
		allow = rmbFileWrite(ExtBindStatePath, line);
		if (!allow)
			snprintf(gBind.why, sizeof(gBind.why), "cannot write %s (no crash guard)", ExtBindStatePath);
	}
	gBind.allow = allow;
	__atomic_store_n(&gBind.done, 1, __ATOMIC_RELEASE);
}

// Watches FB:1 for the rest of the boot, every 250 ms, logging changes only (at most 300 lines). State "ok"
// 60 s after the bind (30 s when pre-bound with nothing connected). From then on a connect of the bound
// link (link+0x54, set by detectLink inside handleHotPlug, before the 'dci ' connection change reaches
// WindowServer) writes "pending: hotplug N" and "ok" again 30 s later or on unplug, so a panic or hang on a
// hot plug blocks the next boot's bind like a boot-time one does.
void extBindWatch(void *, wait_result_t) {
	auto fb = reinterpret_cast<uint8_t *>(gFbBound);
	auto link = gBind.linkPtr;
	const uint32_t okAt = (gBind.pre ? 30 : 60) * 4, settle = 30 * 4;  // ticks of 250 ms
	uint8_t conn = 0xff, lconn = 0xff;
	uint32_t links = ~0u, logged = 0, plugs = 0, pendingAt = 0;
	bool ok = false;
	char line[112];
	LOG("extbind: %s", gBind.note);
	for (uint32_t k = 1;; k++) {
		IOSleep(250);
		auto p = *reinterpret_cast<uint8_t *volatile *>(fb + FbPath1Off);
		uint8_t c = p ? reinterpret_cast<volatile uint8_t *>(p)[1817] : 0;
		uint8_t lc = reinterpret_cast<volatile uint8_t *>(link)[0x54] & 1;
		uint32_t n = p ? *reinterpret_cast<volatile uint32_t *>(p + 208) : 0;
		if ((c != conn || lc != lconn || n != links || k == okAt) && logged < 300) {
			logged++;
			LOG("extbind: +%u.%02u s FB:1 path links %u (link %u) connected %u; link%u connected %u signal %x; "
				"display seen %d", k / 4, k % 4 * 25, n, p ? *reinterpret_cast<uint32_t *>(p + 236) : 0, c, gBind.link,
				lc, linkSignal(gBind.link), gFbEnabledSeen);
		}
		if (ok && lc && lconn == 0) {
			plugs++;
			snprintf(line, sizeof(line), "pending: hotplug %u on link %u\n", plugs, gBind.link);
			rmbFileWrite(ExtBindStatePath, line);
			pendingAt = k;
			LOG("extbind: hot plug %u on link%u, crash guard pending", plugs, gBind.link);
		}
		if (pendingAt && (!lc || k - pendingAt >= settle)) {
			snprintf(line, sizeof(line), "ok: link %u, hotplug %u, fb1 links %u connected %u\n", gBind.link, plugs, n, c);
			rmbFileWrite(ExtBindStatePath, line);
			pendingAt = 0;
			LOG("extbind: hot plug %u %s (FB:1 connected %u), state ok", plugs, lc ? "stable 30 s" : "unplugged", c);
		}
		if (!ok && k >= okAt) {
			snprintf(line, sizeof(line), "ok: link %u%s, fb1 links %u connected %u\n", gBind.link,
				gBind.pre ? " (pre-bound)" : "", n, c);
			rmbFileWrite(ExtBindStatePath, line);
			ok = true;
			LOG("extbind: %u s stable, state ok", okAt / 4);
		}
		conn = c;
		lconn = lc;
		links = n;
	}
}

void extBind(uint64_t fbAddr) {
	static bool once;
	if (once)
		return;
	once = true;
	if (checkKernelArgument("-rmbnoextbind")) {
		LOG("extbind: off (-rmbnoextbind)");
		return;
	}
	auto fb = reinterpret_cast<uint8_t *>(fbAddr);
	uint64_t ctrl = *reinterpret_cast<uint64_t *>(fb + FbCtrlOff);
	auto p1 = *reinterpret_cast<uint8_t **>(fb + FbPath1Off), p0 = *reinterpret_cast<uint8_t **>(fb + FbPathOff);
	auto kptr = [](const void *p) { return reinterpret_cast<uintptr_t>(p) >= 0xffffff8000000000ULL; };
	if (!gGetLink || !kptr(reinterpret_cast<void *>(ctrl)) || !kptr(p0) || !kptr(p1) || ctrl != gDalCtrl) {
		LOG("extbind: FB:1 controller/path pointers not usable (ctrl %llx, boot ctrl %llx), nothing done", ctrl, gDalCtrl);
		return;
	}
	if (*reinterpret_cast<uint32_t *>(p0 + 208) || *reinterpret_cast<uint32_t *>(p1 + 208) || (fb[FbFlagsOff] & 0x20)) {
		LOG("extbind: FB:1 already has a display path, nothing done");
		return;
	}
	uint32_t count = *reinterpret_cast<volatile uint8_t *>(ctrl + 288), pick = 0, portPick = 0;
	uint8_t *link = nullptr, *port = nullptr;
	const bool hdmi = checkKernelArgument("-rmbextbindhdmi"), pre = !checkKernelArgument("-rmbnoextprebind");
	for (uint32_t i = 1; i < count && i < 8; i++) {
		auto l = reinterpret_cast<uint8_t *>(reinterpret_cast<Fn6>(gGetLink)(ctrl, i, 0, 0, 0, 0));
		if (!l)
			break;
		uint32_t sig = *reinterpret_cast<volatile uint32_t *>(ctrl + 24376 + i * 0x440ULL + 8);
		bool dp = sig == 0x20 || (sig == 0 && l[0x1c] == 0x13);
		bool hdmiPort = hdmi && (sig == 4 || (sig == 0 && l[0x1c] == 0x0c));
		bool usable = !(l[0x47] & 1) && (dp || hdmiPort);
		bool want = usable && (l[0x54] & 1);
		LOG("extbind: link%u connector %x signal %x connected %u cached %u fixed %u stream %d -> %s", i, l[0x1c], sig,
			l[0x54] & 1, l[0x494], l[0x47] & 1, *reinterpret_cast<int32_t *>(l + 3344),
			want && !link ? "bind to FB:1" : usable && !port ? "hot plug candidate" : "left alone");
		if (want && !link) {
			link = l;
			pick = i;
		}
		if (usable && !port) {
			port = l;
			portPick = i;
		}
	}
	if (!link && pre && port) {
		link = port;
		pick = portPick;
		gBind.pre = true;
	}
	if (!link) {
		LOG("extbind: no %s external %s link at FB:1 enable, nothing done (extreplay may still run)",
			pre ? "usable" : "connected", hdmi ? "DP/HDMI" : "DP");
		return;
	}
	snprintf(gBind.note, sizeof(gBind.note), "FB:1 -> link%u (connector %x signal %x connected %u) %s", pick, link[0x1c],
		linkSignal(pick), link[0x54] & 1, gBind.pre ? "pre-bound for hot plug, nothing connected at FB:1 enable" :
		"connected at FB:1 enable");
	gBind.link = pick;
	thread_t t;
	if (kernel_thread_start(extBindGuardThread, nullptr, &t) != KERN_SUCCESS) {
		LOG("extbind: guard thread not started, nothing done");
		return;
	}
	thread_deallocate(t);
	for (int i = 0; i < 300 && !__atomic_load_n(&gBind.done, __ATOMIC_ACQUIRE); i++)
		IOSleep(10);
	if (!__atomic_load_n(&gBind.done, __ATOMIC_ACQUIRE)) {
		gBind.abandoned = 1;
		LOG("extbind: crash guard not ready within 3 s, nothing done");
		return;
	}
	if (!gBind.allow) {
		LOG("extbind: %s, nothing done", gBind.why);
		return;
	}
	auto vt = *reinterpret_cast<uint64_t **>(link);
	reinterpret_cast<void (*)(void *, uint32_t, uint8_t)>(vt[440 / 8])(link, 0, 0);  // setStreamState(0, 0)
	bool conn = reinterpret_cast<bool (*)(void *)>(vt[392 / 8])(link);           // isConnected()
	uint8_t *paths[] {p1, p0};
	for (auto p : paths) {
		p[1817] = conn;
		*reinterpret_cast<uint32_t *>(p + 208) = 1;
		*reinterpret_cast<uint32_t *>(p + 236) = pick;
	}
	fb[FbFlagsOff] |= 0x20;
	link[0x47] |= 1;
	gBind.linkPtr = link;
	gFbBound = fbAddr;
	LOG("extbind: FB:1 bound to link%u like FB:0's fixed panel (connected %u, stream state %d%s); link%u fixed, "
		"hotplug via connectionChange", pick, conn, *reinterpret_cast<int32_t *>(link + 3344),
		gBind.pre ? ", pre-bound" : "", pick);
	if (kernel_thread_start(extBindWatch, nullptr, &t) == KERN_SUCCESS)
		thread_deallocate(t);
}

uint64_t wrapInitFixedBinding(uint64_t fb, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
	auto r = reinterpret_cast<Fn6>(orgInitFixedBinding)(fb, b, c, d, e, f);
	if (fb && reinterpret_cast<uint8_t *>(fb)[FbIndexOff] == 1)
		extBind(fb);
	return r;
}

}  // namespace

// AppleBacklight's image range, only for the bldiag caller check (kern_start routeGroup; nothing is routed there).
void displayNoteBacklightImage(mach_vm_address_t address, size_t size) {
	gBlImage = address;
	gBlImageSize = size;
	LOG("bldiag: AppleBacklight image noted (size %lx)", static_cast<unsigned long>(size));
}

bool displayRouteBacklight(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
	if (!address || size < 0x1000) {
		IOLog("REM660: [displayRouteBacklight] invalid address/size\n");
		return false;
	}
	if (checkKernelArgument("-rmbnopanel")) {
		IOLog("REM660: [displayRouteBacklight] disabled by -rmbnopanel\n");
		return true;
	}
	if (orgPanelSetDisplay) {
		IOLog("REM660: [displayRouteBacklight] already hooked\n");
		return true;
	}
	IOLog("REM660: [displayRouteBacklight] installing panel backlight hooks (addr=0x%llx, size=0x%zx)\n",
	      static_cast<uint64_t>(address), size);

	KernelPatcher::RouteRequest req {"__ZN15AppleIntelPanel10setDisplayEP9IODisplay", wrapPanelSetDisplay, orgPanelSetDisplay};
	bool ok = patcher.routeMultiple(index, &req, 1, address, size);
	if (!ok) {
		LOG("bl: failed to route AppleIntelPanel::setDisplay (err %d)", patcher.getError());
		IOLog("REM660: [displayRouteBacklight] failed to route AppleIntelPanel::setDisplay (err %d)\n", patcher.getError());
		patcher.clearError();
	} else {
		LOG("bl: AppleIntelPanel::setDisplay hook installed");
		IOLog("REM660: [displayRouteBacklight] AppleIntelPanel::setDisplay hook installed OK\n");
	}
	patcher.clearError();
	IOLog("REM660: [displayRouteBacklight] done\n");
	return true;
}

bool setDisplayBacklight(KernelPatcher &patcher, mach_vm_address_t address, size_t size) {
	return displayRouteBacklight(patcher, 0, address, size);
}

void displaySetAgdcBase(mach_vm_address_t base) {
	gAgdcBase = base;
	LOG("AGDC at %llx", base);
}

bool rmbVerbose() {
	static int on = -1;
	if (on < 0)
		on = checkKernelArgument("-rmbverbose");
	return on;
}

bool gControllerStarted = false;
volatile char gStage[StCount + 1] = {'-', '-', '-', '-', '-', '-', '-', '-', 0};
static mach_vm_address_t orgControllerNavi2Start, orgControllerStart, orgWranglerStart, orgWranglerProbe, orgControllerProbe;

static IOService *wrapWranglerProbe(IOService *self, IOService *provider, int32_t *score) {
	IOLog("REM660: [wrapWranglerProbe] ENTER self=%p provider=%p score=%d\n", self, provider, score ? *score : 0);
	ensureGpuReadyForStart(provider);
	using ProbeFn = IOService *(*)(IOService *, IOService *, int32_t *);
	IOService *res = nullptr;
	if (orgWranglerProbe) {
		res = reinterpret_cast<ProbeFn>(orgWranglerProbe)(self, provider, score);
	}
	IOLog("REM660: [wrapWranglerProbe] EXIT res=%p score=%d\n", res, score ? *score : 0);
	return res;
}

static IOService *wrapControllerProbe(IOService *self, IOService *provider, int32_t *score) {
	IOLog("REM660: [wrapControllerProbe] ENTER self=%p provider=%p score=%d\n", self, provider, score ? *score : 0);
	ensureGpuReadyForStart(provider);
	using ProbeFn = IOService *(*)(IOService *, IOService *, int32_t *);
	IOService *res = nullptr;
	if (orgControllerProbe) {
		res = reinterpret_cast<ProbeFn>(orgControllerProbe)(self, provider, score);
	}
	if (res && score) {
		*score = 65054;
	}
	IOLog("REM660: [wrapControllerProbe] EXIT res=%p score=%d\n", res, score ? *score : 0);
	return res;
}

static bool wrapControllerNavi2Start(IOService *self, IOService *provider) {
	IOLog("REM660: [wrapControllerNavi2Start] ENTER hooked=[%d,%d,%d,%d] self=%p provider=%p\n",
	      isKextHooked(0), isKextHooked(1), isKextHooked(2), isKextHooked(3), self, provider);
	stageSet(StNavi2, 'E');
	ensureGpuReadyForStart(provider);
	using StartFn = bool (*)(IOService *, IOService *);
	bool ok = false;
	if (orgControllerNavi2Start) {
		ok = reinterpret_cast<StartFn>(orgControllerNavi2Start)(self, provider);
	} else if (orgControllerStart) {
		ok = reinterpret_cast<StartFn>(orgControllerStart)(self, provider);
	}
	stageSet(StNavi2, ok ? '1' : '0');
	IOLog("REM660: [wrapControllerNavi2Start] EXIT ret=%d\n", ok ? 1 : 0);
	if (ok) {
		gControllerStarted = true;
		if (provider && !gAccelProbeSeen) {  // see wrapControllerStart
			provider->registerService(kIOServiceAsynchronous);
		}
	}
	return ok;
}

static bool wrapControllerStart(IOService *self, IOService *provider) {
	IOLog("REM660: [wrapControllerStart] ENTER hooked=[%d,%d,%d,%d] self=%p provider=%p\n",
	      isKextHooked(0), isKextHooked(1), isKextHooked(2), isKextHooked(3), self, provider);
	rmbCheckpoint("ControllerStart ENTER");
	// ensureGpuReadyForStart only warns about IONDRVFramebuffer; no synchronous terminate from start().
	stageSet(StCtrl, 'E');
	ensureGpuReadyForStart(provider);
	ensureVbiosProperty(provider);
	// coreServices / populateXGmiConfig run inside this start and read GCMC through mmioRead().
	mmioAdoptProvider(provider);
	using StartFn = bool (*)(IOService *, IOService *);
	bool ok = reinterpret_cast<StartFn>(orgControllerStart)(self, provider);
	stageSet(StCtrl, ok ? '1' : '0');
	IOLog("REM660: [wrapControllerStart] EXIT ret=%d\n", ok ? 1 : 0);
	rmbCheckpoint("ControllerStart EXIT");
	if (ok) {
		gControllerStarted = true;
		// Re-registering after the accelerator already probed made IOKit create a second
		// AMDNavi23GraphicsAccelerator (boot 2026-11-08 log :2451, second TTL::initialize 0xe00002bc).
		if (provider && !gAccelProbeSeen) {
			IOLog("REM660: [wrapControllerStart] no accelerator probe yet: async rematch of provider\n");
			provider->registerService(kIOServiceAsynchronous);
		}
	}
	return ok;
}
static bool wrapWranglerStart(IOService *self, IOService *provider) {
	IOLog("REM660: [wrapWranglerStart] ENTER hooked=[%d,%d,%d,%d] self=%p provider=%p\n",
	      isKextHooked(0), isKextHooked(1), isKextHooked(2), isKextHooked(3), self, provider);
	rmbCheckpoint("WranglerStart ENTER");
	stageSet(StWrangler, 'E');
	ensureGpuReadyForStart(provider);
	using StartFn = bool (*)(IOService *, IOService *);
	bool ok = reinterpret_cast<StartFn>(orgWranglerStart)(self, provider);
	stageSet(StWrangler, ok ? '1' : '0');
	IOLog("REM660: [wrapWranglerStart] EXIT ret=%d\n", ok ? 1 : 0);
	rmbCheckpoint("WranglerStart EXIT");
	if (ok && provider && !gAccelProbeSeen) {  // see wrapControllerStart
		provider->registerService(kIOServiceAsynchronous);
	}
	return ok;
}

static mach_vm_address_t orgPopulateDevInfoPNS;

static uint32_t wrapPopulateDevInfoPNS(void *self, void *info) {
	IOLog("REM660: [wrapPopulateDevInfoPNS] ENTER self=%p info=%p\n", self, info);
	uint32_t r = 0;
	if (orgPopulateDevInfoPNS) {
		r = reinterpret_cast<uint32_t (*)(void *, void *)>(orgPopulateDevInfoPNS)(self, info);
	}
	if (info) {
		uint16_t *devId = reinterpret_cast<uint16_t *>(reinterpret_cast<uint8_t *>(info) + 2);
		if (*devId == 0x1681) *devId = 0x73ff;
	}
	IOLog("REM660: [wrapPopulateDevInfoPNS] EXIT ret=0x%x\n", r);
	return r;
}


void displayRoute(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
	IOLog("REM660: [displayRoute] ENTER addr=0x%llx size=0x%lx\n", (uint64_t)address, size);
	if (!address || size < 0x1000) {
		LOG("dcn: displayRoute invalid address/size");
		return;
	}
	static bool routed = false;
	if (routed)
		return;
	routed = true;
	IOLog("REM660: [displayRoute] installing DCN 3.1 display hooks (index=%zu, addr=0x%llx)\n", index, address);
	gKeepPwrGate = checkKernelArgument("-rmbpwrgate");
	gNoFwBl = checkKernelArgument("-rmbnofwbl");
	gNoRevive = checkKernelArgument("-rmbnorevive");
	gFwAbmSet = checkKernelArgument("-rmbfwabmset");
	if (!gInboxLock)
		gInboxLock = IOLockAlloc();
	{
		uint32_t ms = 0;
		if (PE_parse_boot_argn("rmbblramp", &ms, sizeof(ms)))
			gBlRampMs = ms > 2000 ? 2000 : ms;
		if (checkKernelArgument("-rmbnoblramp"))
			gBlRampMs = 0;
		if (gBlRampMs && !gBlRampLock)
			gBlRampLock = IOLockAlloc();
		// 1 - exp(-x) ~ x / (1 + x / 2), x = tick / tau
		gBlRampK = gBlRampMs ? 1024 * BlRampTickMs / (gBlRampMs + BlRampTickMs / 2) : 1024;
		if (gBlRampK > 1024)
			gBlRampK = 1024;
		LOG("bl: backlight smoother ease-out tau %u ms, tick %u ms, %u/1024 per tick (rmbblramp=<ms>, -rmbnoblramp)", gBlRampMs,
		    BlRampTickMs, gBlRampK);
	}
	gNoColdPg = checkKernelArgument("-rmbnocoldpg");
	gKeepPhyWrites = checkKernelArgument("-rmbphywr");
	gUsbcLog = !checkKernelArgument("-rmbnousbclog");
	gUsbc4Lane = checkKernelArgument("-rmbusbc4lane");
	gUsbcHbr = checkKernelArgument("-rmbusbchbr");
	gUsbcSdp = checkKernelArgument("-rmbusbcsdp");
	gUsbcLtOrder = checkKernelArgument("-rmbusbcltorder");
	gUsbcPllPd = checkKernelArgument("-rmbusbcpllpd");
	// Win Max 2 USB-C retimer emulation. The Lenovo's hub reports no LTTPR (AUX2 DPCD F0000-F0005 all 0,
	// disklog 2026-11-11) and the emulation writes DPCD 0x100/0x108 and vendor 0xF004F on its own: off
	// unless -rmbretimer.
	gNoRetimer = !checkKernelArgument("-rmbretimer") || checkKernelArgument("-rmbnoretimer");
	gPhyBCpu = checkKernelArgument("-rmbphybcpu");
	gKeepDcc = checkKernelArgument("-rmbdcc");
	gNoAperture = checkKernelArgument("-rmbnoaperture");
	gNoDet = checkKernelArgument("-rmbnodet");
	gKeepDto = checkKernelArgument("-rmbdto");
	gKeepStutter = checkKernelArgument("-rmbstutter");
	gKeepEdpOff = checkKernelArgument("-rmbedpoff");
	gKeepEdpOffAlways = checkKernelArgument("-rmbedpoffkeep");
	gKeepPllPd = checkKernelArgument("-rmbpllpd");
	gDropAllPllPd = checkKernelArgument("-rmbpllpdall");
	gNoPllPdLate = checkKernelArgument("-rmbnopllpdlate");
	gKeepEdpOn = checkKernelArgument("-rmbedpon");
	gNoHold = checkKernelArgument("-rmbnohold");
	gAmdLogAll = checkKernelArgument("-rmbamdlog");
	gKeepTxInit = checkKernelArgument("-rmbtxinit");
	gKeepPixClk = checkKernelArgument("-rmbpixclk");
	gNoDispClk = checkKernelArgument("-rmbnodispclk");
	gKeepFams = checkKernelArgument("-rmbfams");
	gKeepAbm = checkKernelArgument("-rmbabm");
	gNaviSmu = checkKernelArgument("-rmbnavismu");
	gNoClkMgr = checkKernelArgument("-rmbnoclkmgr");
	gNoFwTrace = checkKernelArgument("-rmbnofwtrace");
	gNoOtgClk = checkKernelArgument("-rmbotgclk");
	KernelPatcher::RouteRequest reqs[] {
		{"__ZN30AMDRadeonX6000_AmdGpuWrangler5probeEP9IOServicePi", wrapWranglerProbe, orgWranglerProbe},
		{"__ZN29AMDRadeonX6000_AmdGpuWrangler5probeEP9IOServicePi", wrapWranglerProbe, orgWranglerProbe},
		{"__ZN30AMDRadeonX6000_AmdGpuWrangler5startEP9IOService", wrapWranglerStart, orgWranglerStart},
		{"__ZN29AMDRadeonX6000_AmdGpuWrangler5startEP9IOService", wrapWranglerStart, orgWranglerStart},
		{"__ZN40AMDRadeonX6000_AmdRadeonControllerNavi235probeEP9IOServicePi", wrapControllerProbe, orgControllerProbe},
		{"__ZN34AMDRadeonX6000_AmdRadeonController5probeEP9IOServicePi", wrapControllerProbe, orgControllerProbe},
		{"__ZN19AMDRadeonController5probeEP9IOServicePi", wrapControllerProbe, orgControllerProbe},
		{"__ZN40AMDRadeonX6000_AmdRadeonControllerNavi235startEP9IOService", wrapControllerStart, orgControllerStart},
		{"__ZN34AMDRadeonX6000_AmdRadeonController5startEP9IOService", wrapControllerStart, orgControllerStart},
		{"__ZN39AMDRadeonX6000_AmdRadeonControllerNavi25startEP9IOService", wrapControllerNavi2Start, orgControllerNavi2Start},
		{"__ZN27AMDRadeonX6000_AmdDalHelper21dalAtomReadRegister32EPvj", wrapRead, orgRead},
		{"__ZN27AMDRadeonX6000_AmdDalHelper22dalAtomWriteRegister32EPvjj", wrapWrite, orgWrite},
		{"__ZN27AMDRadeonX6000_AmdDalHelper19dalAtomExecCmdTableEPvjS0_", wrapExecTable, orgExecTable},
		{"__ZN24AMDRadeonX6000_AmdLogger15initWithPciInfoEP11IOPCIDevice", wrapLoggerInit, orgLoggerInit},
		{"__ZN22AMDRadeonX6000_AmdLink10detectLinkE17AmdFbDetectReasonP18DetectedLinkStatus", wrapDetectLink, orgDetectLink},
		{"__ZN34AMDRadeonX6000_AmdRadeonController17detectAllDisplaysEv", wrap_detectAllDisplays, org_detectAllDisplays},
		{"__ZN34AMDRadeonX6000_AmdRadeonController14detectDisplaysEj", wrap_detectDisplays, org_detectDisplays},
		{"__ZN34AMDRadeonX6000_AmdRadeonController19detectDisplayOnLinkEh", wrap_detectDisplayOnLink, org_detectDisplayOnLink},
		{"__ZN34AMDRadeonX6000_AmdRadeonController20establishBootDisplayEv", wrap_establishBootDisplay, org_establishBootDisplay},
		{"__ZN32AMDRadeonX6000_AmdRegisterAccess11hwReadReg32Ej", wrapRawRead<orgHwRead>, orgHwRead},
		{"__ZN32AMDRadeonX6000_AmdRegisterAccess17readValidateReg32Ej", wrapRawRead<orgValRead>, orgValRead},
		{"__ZN32AMDRadeonX6000_AmdRegisterAccess10writeReg32Ejj", wrapRawWrite, orgRawWrite},
	};
	gFbText = address;
	gFbOpen = findSymbol(patcher, 1, "__ZN35AMDRadeonX6000_AmdRadeonFramebuffer4openEv", address, size);
	if (auto wm = findSymbol(patcher, 1, "__ZN39AMDRadeonX6000_AmdRadeonControllerNavi219setupBootWatermarksEv", address, size)) {
		gBootWmFirst = static_cast<uint32_t>(wm - address + FbTextBase);
		gBootWmLast = gBootWmFirst + BootWmSize;
	} else {
		LOG("dcn: setupBootWatermarks not found, its raw writes stay unremapped");
	}
	gGetLink = findSymbol(patcher, 1, "__ZNK34AMDRadeonX6000_AmdRadeonController7getLinkEh", address, size);
	gResetConnected = findSymbol(patcher, 1, "__ZN22AMDRadeonX6000_AmdLink20resetConnectedStatusEv", address, size);
	if (!routeStaticRequests(patcher, 1, reqs, arrsize(reqs), address, size))
		LOG("dcn: failed to route DAL register callbacks (err %d)", patcher.getError());
	else
		LOG("dcn: DAL register remap installed (%zu registers)", sizeof(kDcnRemap) / sizeof(kDcnRemap[0]));
	if (!orgWranglerProbe) {
		mach_vm_address_t wProbe = findSymbol(patcher, 1, "__ZN30AMDRadeonX6000_AmdGpuWrangler5probeEP9IOServicePi", address, size);
		if (!wProbe) wProbe = findSymbol(patcher, 1, "__ZN29AMDRadeonX6000_AmdGpuWrangler5probeEP9IOServicePi", address, size);
		if (!wProbe) wProbe = findSymbol(patcher, 1, "__ZN14AmdGpuWrangler5probeEP9IOServicePi", address, size);
		if (wProbe != 0) {
			orgWranglerProbe = patcher.routeFunction(wProbe, reinterpret_cast<mach_vm_address_t>(wrapWranglerProbe), true);
			IOLog("REM660: [displayRoute] fallback routed Wrangler::probe at 0x%llx (org=%p)\n", (uint64_t)wProbe, (void *)orgWranglerProbe);
		} else {
			IOLog("REM660: FATAL - Missing static RVA for AmdGpuWrangler::probe!\n");
		}
	}
	if (!orgControllerProbe) {
		mach_vm_address_t cProbe = findSymbol(patcher, 1, "__ZN40AMDRadeonX6000_AmdRadeonControllerNavi235probeEP9IOServicePi", address, size);
		if (!cProbe) cProbe = findSymbol(patcher, 1, "__ZN34AMDRadeonX6000_AmdRadeonController5probeEP9IOServicePi", address, size);
		if (!cProbe) cProbe = findSymbol(patcher, 1, "__ZN19AMDRadeonController5probeEP9IOServicePi", address, size);
		if (cProbe != 0) {
			orgControllerProbe = patcher.routeFunction(cProbe, reinterpret_cast<mach_vm_address_t>(wrapControllerProbe), true);
			IOLog("REM660: [displayRoute] fallback routed Controller::probe at 0x%llx (org=%p)\n", (uint64_t)cProbe, (void *)orgControllerProbe);
		} else {
			IOLog("REM660: FATAL - Missing static RVA for AMDRadeonController::probe!\n");
		}
	}
	if (!orgControllerNavi2Start) {
		mach_vm_address_t cStart = findSymbol(patcher, 1, "__ZN39AMDRadeonX6000_AmdRadeonControllerNavi25startEP9IOService", address, size);
		if (cStart != 0) {
			orgControllerNavi2Start = patcher.routeFunction(cStart, reinterpret_cast<mach_vm_address_t>(wrapControllerNavi2Start), true);
			IOLog("REM660: [displayRoute] fallback routed ControllerNavi2::start at 0x%llx (org=%p)\n", (uint64_t)cStart, (void *)orgControllerNavi2Start);
		} else {
			IOLog("REM660: FATAL - Missing static RVA for ControllerNavi2::start!\n");
		}
	}
	if (!orgControllerStart) {
		mach_vm_address_t cStart = findSymbol(patcher, 1, "__ZN40AMDRadeonX6000_AmdRadeonControllerNavi235startEP9IOService", address, size);
		if (!cStart) cStart = findSymbol(patcher, 1, "__ZN34AMDRadeonX6000_AmdRadeonController5startEP9IOService", address, size);
		if (!cStart) cStart = findSymbol(patcher, 1, "__ZN19AMDRadeonController5startEP9IOService", address, size);
		if (cStart != 0) {
			orgControllerStart = patcher.routeFunction(cStart, reinterpret_cast<mach_vm_address_t>(wrapControllerStart), true);
			IOLog("REM660: [displayRoute] fallback routed Controller::start at 0x%llx (org=%p)\n", (uint64_t)cStart, (void *)orgControllerStart);
		} else {
			IOLog("REM660: FATAL - Missing static RVA for AMDRadeonController::start!\n");
		}
	}
	if (!orgWranglerStart) {
		mach_vm_address_t wStart = findSymbol(patcher, 1, "__ZN30AMDRadeonX6000_AmdGpuWrangler5startEP9IOService", address, size);
		if (!wStart) wStart = findSymbol(patcher, 1, "__ZN29AMDRadeonX6000_AmdGpuWrangler5startEP9IOService", address, size);
		if (!wStart) wStart = findSymbol(patcher, 1, "__ZN14AmdGpuWrangler5startEP9IOService", address, size);
		if (wStart != 0) {
			orgWranglerStart = patcher.routeFunction(wStart, reinterpret_cast<mach_vm_address_t>(wrapWranglerStart), true);
			IOLog("REM660: [displayRoute] fallback routed Wrangler::start at 0x%llx (org=%p)\n", (uint64_t)wStart, (void *)orgWranglerStart);
		} else {
			IOLog("REM660: FATAL - Missing static RVA for AmdGpuWrangler::start!\n");
		}
	}
	// Only symbols that exist in the 25G229 FB (llvm-nm). The former populateDeviceInfo()/allocateFBMemory/
	// initializeSwComponents/initializeProject entries named no symbol in this binary; their RVAs
	// (0x562ca 0x56540 0x56ae4 0x5639c) were mid-instruction / padding / an unrelated function, and
	// 0x5639c sits inside 0x56300, which createAtomFirmwareServices (0x54d90 -> 0x54e10 -> 0x54f3b) calls.
	KernelPatcher::RouteRequest ctrlReqs[] {
		{"__ZN34AMDRadeonX6000_AmdRadeonController18populateDeviceInfoEPNS_10DeviceInfoE", wrapPopulateDevInfoPNS, orgPopulateDevInfoPNS},
	};
	routeStaticRequests(patcher, 1, ctrlReqs, arrsize(ctrlReqs), address, size);
	if (!checkKernelArgument("-rmbnobrgt")) {
		KernelPatcher::RouteRequest attrReqs[] {
			{"__ZN35AMDRadeonX6000_AmdRadeonFramebuffer25getAttributeForConnectionEijPm", wrapGetAttr, orgGetAttr},
			{"__ZN35AMDRadeonX6000_AmdRadeonFramebuffer25setAttributeForConnectionEijm", wrapSetAttr, orgSetAttr},
		};
		if (!routeStaticRequests(patcher, 1, attrReqs, arrsize(attrReqs), address, size)) {
			LOG("bl: failed to route fb attribute hooks (err %d)", patcher.getError());
		} else {
			LOG("bl: brgt attribute hooks installed");
			if (!checkKernelArgument("-rmbnobldiag") && (gBlDiagLock = IOSimpleLockAlloc()) != nullptr) {
				gBlDiag = true;
				thread_t t;
				if (kernel_thread_start(blDiagThread, nullptr, &t) == KERN_SUCCESS)
					thread_deallocate(t);
				LOG("bldiag: fb attribute callers + AppleIntelPanelA snapshots at +30/+60/+120 s (-rmbnobldiag = off)");
			}
		}
	}
	gGetMyLink = findSymbol(patcher, 1, "__ZN35AMDRadeonX6000_AmdRadeonFramebuffer9getMyLinkEh", address, size);
	{
		KernelPatcher::RouteRequest ccReq {"__ZN35AMDRadeonX6000_AmdRadeonFramebuffer16connectionChangeEv", wrapConnChange, orgConnChange};
		if (!routeStaticRequests(patcher, 1, &ccReq, 1, address, size))
			LOG("fbpath: connectionChange hook FAILED (err %d)", patcher.getError());
	}
	if (!checkKernelArgument("-rmbnopresdiag")) {
		gPresMeta = reinterpret_cast<const OSMetaClass *>(
			findSymbol(patcher, 1, "__ZN29AMDRadeonX6000_AmdFbPresenter10gMetaClassE", address, size));
		KernelPatcher::RouteRequest presReqs[] {
			{"__ZN29AMDRadeonX6000_AmdFbPresenter24transactionVBlankHandlerEP8OSObjectPv", wrapPresVbl, orgPresVbl},
			{"__ZN29AMDRadeonX6000_AmdFbPresenter25transactionVUpdateHandlerEP8OSObjectPv", wrapPresVupd, orgPresVupd},
			{"__ZN29AMDRadeonX6000_AmdFbPresenter19enqueueTransactionsEy", wrapPresEnq, orgPresEnq},
			{"__ZN35AMDRadeonX6000_AmdRadeonFramebuffer12setAttributeEjm", wrapFbSetAttr, orgFbSetAttr},
			{"__ZN29AMDRadeonX6000_AmdFbPresenter18enableTransactionsEv", wrapPresEnable, orgPresEnable},
			{"__ZN35AMDRadeonX6000_AmdRadeonFramebuffer17setInterruptStateEPvj", wrapFbSetIntState, orgFbSetIntState},
		};
		if (!checkKernelArgument("-rmbnocurdiag")) {
			KernelPatcher::RouteRequest curReqs[] {
				{"__ZN35AMDRadeonX6000_AmdRadeonFramebuffer14setCursorImageEPv", wrapCurSetImage, orgCurSetImage},
				{"__ZN35AMDRadeonX6000_AmdRadeonFramebuffer14setCursorStateEiib", wrapCurSetState, orgCurSetState},
				{"__ZN24AMDRadeonX6000_AmdCursor19commitCursorUpdatesEv", wrapCurCommit, orgCurCommit},
				{"__ZN29AMDRadeonX6000_AmdFbPresenter18dequeueTransationsEy", wrapPresDequeue, orgPresDequeue},
			};
			if (!routeStaticRequests(patcher, 1, curReqs, arrsize(curReqs), address, size))
				LOG("curdiag: hooks FAILED (err %d)", patcher.getError());
			else
				LOG("curdiag: cursor/dequeue counters installed");
		}
		gFbSetPipeIntState = reinterpret_cast<uint32_t (*)(uint64_t, bool)>(
			findSymbol(patcher, 1, "__ZN35AMDRadeonX6000_AmdRadeonFramebuffer21setPipeInterruptStateEb", address, size));
		if (!routeStaticRequests(patcher, 1, presReqs, arrsize(presReqs), address, size)) {
			LOG("presdiag: hooks FAILED (err %d)", patcher.getError());
		} else {
			LOG("presdiag: AmdFbPresenter vblank/vupdate/enqueue counters installed");
			thread_t t;
			if (kernel_thread_start(presDiagThread, nullptr, &t) == KERN_SUCCESS)
				thread_deallocate(t);
		}
	}
	// IOGraphicsFamily is a dependency of this kext, so it is loaded; WindowServer is not running yet, so
	// extSetCursorPosition is not executing while it is routed (iogdiag.cpp).
	iogDiagRoute(patcher);
	if (!checkKernelArgument("-rmbnotgfix")) {
		KernelPatcher::RouteRequest tgReq {"__ZN29AMDRadeonX6000_AmdFbPresenter24setVUpdateVBlankIntervalEv", wrapPresSetVuInt,
			orgPresSetVuInt};
		if (!routeStaticRequests(patcher, 1, &tgReq, 1, address, size))
			LOG("tgfix: setVUpdateVBlankInterval hook FAILED (err %d)", patcher.getError());
		else
			LOG("tgfix: presenter TG fill from the running OTG installed");
	}
	// extbind: FB:1 gets the external DP link like FB:0's fixed panel (see extBind). Prologue of 0x151e6 is
	// `cmpb $0, 480(%rdi)` (7 bytes, no RIP/branch), short-jump safe (verify_symbol_manifest.py).
	if (!checkKernelArgument("-rmbnoextbind")) {
		KernelPatcher::RouteRequest bindReq {"__ZN35AMDRadeonX6000_AmdRadeonFramebuffer29initializeFixedDisplayBindingEv",
			wrapInitFixedBinding, orgInitFixedBinding};
		if (!routeStaticRequests(patcher, 1, &bindReq, 1, address, size))
			LOG("extbind: initializeFixedDisplayBinding hook FAILED (err %d)", patcher.getError());
		else
			LOG("extbind: initializeFixedDisplayBinding hook installed");
	}
	// On by default (2026-11-09): what displaypolicyd asks AmdAgdcServices decides whether link 0 ever
	// gets a framebuffer; bounded logging (300 combos, 60+60 entries, counts 600 s). `-rmbnoagdctrace` = off.
	if (!checkKernelArgument("-rmbnoagdctrace")) {
		gAgdcLock = IOSimpleLockAlloc();
		KernelPatcher::RouteRequest agdcReqs[] {
			{"__ZN30AMDRadeonX6000_AmdAgdcServices24vendor_doDeviceAttributeEjPmmS0_S0_P25IOExternalMethodArguments",
				wrapAgdcVendor, orgAgdcVendor},
			{"__ZN30AMDRadeonX6000_AmdAgdcServices26validateConfigurationEntryEPK32AGDCResourceConfigurationEntry_t",
				wrapAgdcValidate, orgAgdcValidate},
			{"__ZN30AMDRadeonX6000_AmdAgdcServices26commitConfigurationForLinkEjPK18AGDCDPLinkConfig_t",
				wrapAgdcLinkCommit, orgAgdcLinkCommit},
		};
		if (!gAgdcLock || !routeStaticRequests(patcher, 1, agdcReqs, arrsize(agdcReqs), address, size)) {
			LOG("agdc: trace hooks FAILED (err %d)", patcher.getError());
		} else {
			LOG("agdc: trace hooks installed");
			thread_t t;
			if (kernel_thread_start(agdcCountThread, nullptr, &t) == KERN_SUCCESS)
				thread_deallocate(t);
		}
	}

	if (!checkKernelArgument("-rmbnoopen")) {
		thread_t t;
		if (kernel_thread_start(earlyOpen, nullptr, &t) == KERN_SUCCESS)
			thread_deallocate(t);
	}
	if (!checkKernelArgument("-rmbvcn")) {
		thread_t t;
		if (kernel_thread_start(noVideoEngine, nullptr, &t) == KERN_SUCCESS)
			thread_deallocate(t);
	}
	edpLinkWatchStart();

	patcher.clearError();
	IOLog("REM660: [displayRoute] EXIT ok\n");
}

// vcn.cpp: publish (VCN on) or hide the accelerator's video properties. Publishing sets back the values novcn
// removed; processes started afterwards see the hardware codecs.
void vcnVideoProps(bool show) {
	if (!gVideoLock)
		return;
	IOLockLock(gVideoLock);
	gVideoShown = show;
	unsigned n = 0;
	if (gVideoAccel && show && gSavedVideo) {
		auto iter = OSCollectionIterator::withCollection(gSavedVideo);
		while (auto key = iter ? OSDynamicCast(OSSymbol, iter->getNextObject()) : nullptr)
			if (auto value = gSavedVideo->getObject(key)) {
				gVideoAccel->setProperty(key, value);
				n++;
			}
		OSSafeReleaseNULL(iter);
		LOG("novcn: published %u hardware video properties again (VCN on)", n);
	} else if (gVideoAccel && !show) {
		LOG("novcn: hid %u hardware video properties (VCN not on)", stripVideoProps(gVideoAccel));
	}
	IOLockUnlock(gVideoLock);
}

// For dmub.cpp's QUERY_DP_ALT diag (dmubInject / gExtLink live in this file's anonymous namespace).
bool dmubInjectCmd(const uint32_t *cmd, uint32_t *reply) { return dmubInject(cmd, reply); }
uint32_t extLink() { return gExtLink; }
bool extLinkDp() { return extLinkIsDp(); }
void pwrseqDiagBoot() { psDiagBoot(); }
void pwrseqWakeArm() { psWakeArm(); }
void pwrseqWakeTick() { psWakeTick(); }
