//
//  GC firmware substitution. HWLibs embeds Navi 23's signed GC microcode and hands it to the PSP,
//  which rejects it on Rembrandt (status 0x8000030a). Point HWLibs' firmware descriptors at the
//  Rembrandt images instead. Descriptor layout (0x38 bytes): +0x0C u32 size, +0x20 data pointer.
//

#include "rmb.hpp"
#include "fw.hpp"
#include "psp_compat.hpp"
#include <IOKit/IOService.h>
#include <IOKit/IOMessage.h>
#include <IOKit/pwr_mgt/RootDomain.h>

// Bare-metal S3 (2026-11-18): the GPD sleep path, wired up for the Lenovo. -rmbnos3: none of it (no
// notifier, no power_down/power_up/vbs routes), i.e. the pre-S3 kext.
bool gS3Enabled;
volatile bool gS3WakePending;  // sleep entry -> first startKIQ after wake (GART restore + CP queue reset)
volatile bool gPspInWake;      // inside psp_power_up_event after a sleep
volatile bool gGpuDownForSleep;  // psp_power_down_event ran for this sleep; cleared by the wake's startKIQ prep
uint32_t gS3Cycles;
volatile bool gDispClkWakePending;

namespace {

struct Substitution {
	const char *symbol;
	const uint8_t *data;
	uint32_t size;
};

// psp_tmr_init sends the TOC (psp+0x3588 data, psp+0x3580 size) with LOAD_TOC; the PSP answers
// with the TMR size it needs. Rembrandt's PSP rejects Navi 23's TOC, so give it yellow_carp_toc.
mach_vm_address_t orgPspTmrInit;
mach_vm_address_t gPspTmrUnload;
mach_vm_address_t gPspRingStop;
mach_vm_address_t orgPspPowerDown;
uint64_t gPsp;
IONotifier *gPowerNotifier;

// A VM restart (or guest S3 resume) can't re-init a PSP that still has the old run's TAs, TMR and
// rings: every KM command then times out. Apple never tears these down at shutdown, and
// psp_hw_exit's TMR destroy fails while TAs are still loaded (pexit1: psp_tmr_destroy failed; the
// next VM boot froze the host). Use Apple's own S3 sequence (psp_power_down_event's full branch,
// same order as Linux psp_suspend): unload the TAs, then the TMR, then stop the UM and KM rings
// (-rmbnopspexit: don't).
const char *const pspTeardown[] {"_psp_hdcp_unload", "_psp_hc_unload", "_psp_ras_unload", "_psp_dtm_unload",
	"_psp_asd_unload", "_psp_tmr_unload"};
mach_vm_address_t gPspTeardown[arrsize(pspTeardown)];
void pspTeardownSeq(uint64_t p, const char *tag) {
	for (size_t i = 0; i < arrsize(pspTeardown); i++) {
		if (!gPspTeardown[i])
			continue;
		auto r = reinterpret_cast<uint64_t (*)(uint64_t)>(gPspTeardown[i])(p);
		LOG("%s: %s = %llx, C2PMSG_64 %08x", tag, pspTeardown[i] + 1, r & 0xFFFFFFFF, mmioRead(0x16080));
	}
	for (uint64_t ring = 1; ring <= 2; ring++) {
		auto r = reinterpret_cast<uint64_t (*)(uint64_t, uint64_t)>(gPspRingStop)(p, ring);
		LOG("%s: psp_ring_stop(%llu) = %llx, C2PMSG_64 %08x", tag, ring, r & 0xFFFFFFFF, mmioRead(0x16080));
	}
}
IOReturn powerHandler(void *, void *, UInt32 type, IOService *, void *arg, vm_size_t) {
	// Lenovo bare metal: shutdown and restart stay exactly as before this notifier existed (no eDP
	// replay, no PSP teardown, no halt): the cold-boot path never needed them and on GPD they were VM
	// restart helpers. -rmbs3shutdownsteps: GPD behaviour.
	if ((type == kIOMessageSystemWillPowerOff || type == kIOMessageSystemWillRestart) &&
	    !checkKernelArgument("-rmbs3shutdownsteps"))
		return kIOReturnSuccess;
	if (type == kIOMessageSystemWillPowerOff || type == kIOMessageSystemWillRestart) {
		LOG("shutdown: %s received", type == kIOMessageSystemWillPowerOff ? "WillPowerOff" : "WillRestart");
		edpAllowPowerOff("shutdown");
		edpPowerOffNow("shutdown");
	}
	else if (sleepEntering(type, arg)) {
		if (type != kIOMessageSystemWillSleep)
			LOG("sleep: from dark wake (capability change drops CPU)");
		gS3Cycles++;
		LOG("s3guard: sleep entry #%u (%s)", gS3Cycles, type == kIOMessageSystemWillSleep ? "WillSleep" : "from dark wake");
		vramCrcStep("sleep");
		diskLogSleep(true);
		gKiqSleepSkip = true;
		edpAllowPowerOff("sleep");
		gWakeWatchArmed = true;
		gWakePost = true;
		gS3WakePending = true;
		gEdpLinkPause = true;
	}
	else if (type == kIOMessageSystemHasPoweredOn) {
		diskLogSleep(false);
		LOG("s3guard: full wake (GPU %s)", gGpuDownForSleep ? "went down, restart pending/in progress" : "not down or already restarted");
		if (!gGpuDownForSleep) {  // sleep aborted before the GPU went down (or wake already done): nothing to redo
			gS3WakePending = false;
			gWakePost = false;
		}
		gKiqSleepSkip = false;
		edpAllowPowerOff(nullptr);  // aborted or finished sleep: wake training OFFs must stay NULLed
		// The PMFW restarted at its default dispclk (11-19: eDP 72.4 Hz after the first S3 wake). The DMCUB
		// re-release re-votes too; this one covers a wake where DAL wrote its DTOs before that release.
		if (gDispClkWakePending) {
			gDispClkWakePending = false;
			dispClkReapply("S3 full wake");
		}
		panelWakeWatch();
		gEdpLinkPause = false;
		gEdpLinkKick = true;
		vramCrcStep("full wake");
	}
	else if (cpuCapChange(type, arg, true)) {
		// Resumed into dark wake: the sleep-entry panic clock must stop here (10-27 false panic
		// 38 s after a WillSleep with two dark wakes and no HasPoweredOn in between).
		LOG("wake: dark wake (capability change brings CPU back)");
		vramCrcStep("dark wake");
		diskLogSleep(false);
	}
	// Power off: skip the PSP teardown + SMU MODE2. They only existed so the next VM start in the same
	// host boot could re-init the PSP (dropped); on bare metal PSP/SMU are platform-wide and a MODE2
	// right before ACPI S5 is the suspect for the 10-31 Shut Down hang. Restart keeps them
	// (-rmbpspexitoff = old behaviour at power off too).
	if (type == kIOMessageSystemWillPowerOff && gPsp && !checkKernelArgument("-rmbpspexitoff")) {
		LOG("psp-exit: power off, PSP teardown + MODE2 skipped (bare metal)");
		gPsp = 0;
	}
	if ((type == kIOMessageSystemWillPowerOff || type == kIOMessageSystemWillRestart) && gPsp && gPspRingStop) {
		auto p = gPsp;
		gPsp = 0;
		LOG("psp-exit: %s, C2PMSG_64 %08x", type == kIOMessageSystemWillPowerOff ? "power off" : "restart",
		    mmioRead(0x16080));
		if (!checkKernelArgument("-rmbnohalt"))
			haltGfxEngines("at shutdown");
		pspTeardownSeq(p, "psp-exit");
		smuExitReset("psp-exit");
		quiesceIh("at shutdown");
	}
	if (type == kIOMessageSystemWillPowerOff || type == kIOMessageSystemWillRestart)
		LOG("shutdown: RembrandtGfx shutdown steps done");
	return kIOReturnSuccess;
}
const char *tmrStageName(rem660::psp::TmrStage stage) {
	switch (stage) {
		case rem660::psp::TmrStage::BadArguments: return "bad arguments";
		case rem660::psp::TmrStage::StagingRejected: return "staging buffer rejected (LOAD_TOC not sent)";
		case rem660::psp::TmrStage::NativeFailed: return "native psp_tmr_init failed";
		case rem660::psp::TmrStage::TmrRejected: return "TMR outside PSP contract";
		case rem660::psp::TmrStage::Ok: return "ok";
	}
	return "?";
}
uint32_t wrapPspTmrInit(void *state) {
	// Cold boot: preserve the first native status (no TMR destroy/retry, MODE2 or cross-VM assumptions).
	// S3 wake only (gPspInWake, set around psp_power_up_event): if LOAD_TOC still finds the old TMR, unload
	// it and retry once, as GPD did (-rmbnotmrfix: don't).
	rem660::psp::TmrReport rep {};
	auto r = rem660::psp::initWithYellowCarpToc(state, fw_yc_toc, FW_SIZE(toc),
		reinterpret_cast<rem660::psp::TmrInit>(orgPspTmrInit), &rep);
	LOG("fw: psp_tmr_init = %#x (%s); TOC %#x -> %#x bytes, native %#x%s", r, tmrStageName(rep.stage),
	    rep.tocBytesBefore, rep.tocBytesSelected, rep.nativeStatus, gPspInWake ? " [S3 wake]" : "");
	LOG("fw: psp staging %#llx bytes gpu %#llx cpu %s handle %s; TMR %#llx bytes gpu %#llx cpu %s handle %s",
	    rep.stagingBytes, rep.stagingGpu, rep.stagingCpu ? "set" : "null", rep.stagingHandle ? "set" : "null",
	    rep.tmrBytes, rep.tmrGpu, rep.tmrCpu ? "set" : "null", rep.tmrHandle ? "set" : "null");
	if (r && gPspInWake && gPspTmrUnload && !checkKernelArgument("-rmbnotmrfix")) {
		auto u = reinterpret_cast<uint64_t (*)(uint64_t)>(gPspTmrUnload)(reinterpret_cast<uint64_t>(state));
		rep = {};
		r = rem660::psp::initWithYellowCarpToc(state, fw_yc_toc, FW_SIZE(toc),
			reinterpret_cast<rem660::psp::TmrInit>(orgPspTmrInit), &rep);
		LOG("fw: S3 wake stale TMR: psp_tmr_unload = %llx, retry psp_tmr_init = %#x (%s)", u & 0xFFFFFFFF, r,
		    tmrStageName(rep.stage));
	}
	// Arm the sleep/wake notifier once the PSP is up (it also needs gPsp for the teardown).
	if (!r && gS3Enabled) {
		gPsp = reinterpret_cast<uint64_t>(state);
		if (!gPowerNotifier)
			gPowerNotifier = registerPrioritySleepWakeInterest(powerHandler, nullptr, nullptr);
		LOG("s3: armed (notifier %s, teardown %s, ring_stop %s)", gPowerNotifier ? "ok" : "MISSING",
		    orgPspPowerDown ? "routed" : "NOT routed", gPspRingStop ? "ok" : "missing");
	}
	return r;
}

// Firmware-loading mode switches, read by gc_get_config / sdma_get_config through
// _sdma_read_config_setting_uint32 (0x91d4) -> SWIP cos+168 = AmdTtlServices+80+168 =
// cosReadConfigurationSetting (initializeCos 0xadcbd) -> IOPCIDevice (+8, ctor 0xae0ee) getProperty.
// All defaults are 0 in 25G229 (ecx=0 at 0x88a1/0x88c6/0x88eb, 0x597b1/0x597fb/0x59819), so a non-zero
// value means a property supplied at runtime; its origin is logged by createPciDeviceCache.
//  GC:   gc+802 EnableswGCFWLoading (0x88ae), +803 EnableswGCFWLoadingBypass (0x88d3),
//        +804 EnableFWBackdoorAutoload (0x88f8). gc_set_fw_entry_info logs _gc_log_event_map[7] =
//        0xc00c0401 and creates no PSP firmware entry if +803 is set (0x9299) or, after the 10.3.4
//        constants, if +802 or +804 is set (0x93a1/0x93ae). This is the GC error seen on the Lenovo.
//  SDMA: sdma+638 EnableswSDMAFWLoading (0x597be), +640 EnableswSDMAFWLoadingBypass (0x59808),
//        +641 EnableFWBackdoorAutoload (0x5982d). sdma_set_fw_entry_info returns silently without an
//        entry if any of them is set (0x59076..0x59093).
// Rembrandt is a production-fused APU: CP/RLC/SDMA microcode only runs after the PSP front-door load
// (the path Apple uses with the defaults). Direct/backdoor/bypass loading would leave GFX and SDMA dead.
// -rmbkeepfwcfg keeps whatever the configuration said.
mach_vm_address_t orgGcGetConfig, orgSdmaGetConfig;
void enforcePspFwLoad(const char *who, volatile uint8_t *base, const uint16_t (&fields)[3], const char *const (&names)[3]) {
	uint8_t v[3] {base[fields[0]], base[fields[1]], base[fields[2]]};
	LOG("%s-config: %s=%u %s=%u %s=%u", who, names[0], v[0], names[1], v[1], names[2], v[2]);
	if (!(v[0] | v[1] | v[2]) || checkKernelArgument("-rmbkeepfwcfg"))
		return;
	for (auto off : fields)
		base[off] = 0;
	LOG("%s-config: non-PSP firmware loading cleared: Rembrandt microcode only runs after the PSP front-door load", who);
}
uint64_t wrapGcGetConfig(uint64_t gc, uint64_t mesVersion, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
	auto r = reinterpret_cast<Fn6>(orgGcGetConfig)(gc, mesVersion, c, d, e, f);
	static const uint16_t fields[3] {802, 803, 804};
	static const char *const names[3] {"EnableswGCFWLoading", "EnableswGCFWLoadingBypass", "EnableFWBackdoorAutoload"};
	if (gc)
		enforcePspFwLoad("gc", reinterpret_cast<volatile uint8_t *>(gc), fields, names);
	return r;
}
uint64_t wrapSdmaGetConfig(uint64_t sdma, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
	auto r = reinterpret_cast<Fn6>(orgSdmaGetConfig)(sdma, b, c, d, e, f);
	static const uint16_t fields[3] {638, 640, 641};
	static const char *const names[3] {"EnableswSDMAFWLoading", "EnableswSDMAFWLoadingBypass", "EnableFWBackdoorAutoload"};
	if (sdma)
		enforcePspFwLoad("sdma", reinterpret_cast<volatile uint8_t *>(sdma), fields, names);
	return r;
}

// psp_power_down_event(psp, flags*): with psp+0x28 bit 5 clear (dGPU BACO style, our case) a real
// power-down (*flags & 1) only saves np fw state and keeps the TAs, TMR and rings, assuming the
// PSP keeps its state. The guest's S3 wake re-inits the PSP from scratch (power_up_event calls
// psp_hardware_initialization), but the old TMR still holds DMUB, so its LOAD_IP_FW fails with
// 0x80000029/0x80000007 (s3wake4). Do the full branch's teardown too (-rmbnos3psp: don't).
// Lenovo bare metal 11-12 (no teardown): the wake's KM ring create timed out -> TTL::powerUp 0xe00002bc.
uint64_t wrapPspPowerDown(uint64_t psp, uint64_t flags, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
	// Only inside a system sleep our notifier saw (gS3WakePending). 11-16..18: 12 display sleeps, 0
	// TTL::powerDown, so this is a guard, not a known case; anything else stays native.
	if (!gS3WakePending || !psp || !flags) {
		LOG("psp-sleep: power_down_event outside system sleep (pending %d), native only", gS3WakePending);
		return reinterpret_cast<Fn6>(orgPspPowerDown)(psp, flags, c, d, e, f);
	}
	bool light = !(*reinterpret_cast<uint64_t *>(psp + 0x28) & 0x20) && (*reinterpret_cast<uint8_t *>(flags) & 1);
	gartCensus("sleep entry");
	auto r = reinterpret_cast<Fn6>(orgPspPowerDown)(psp, flags, c, d, e, f);
	LOG("psp-sleep: power_down_event (%s branch), C2PMSG_64 %08x", light ? "light" : "full/none", mmioRead(0x16080));
	diskLogSync(1000);  // a hang in the teardown below must leave this line on disk
	edpPanelPowerLost();
	if (light && gPspRingStop && !checkKernelArgument("-rmbnos3psp")) {
		pspTeardownSeq(psp, "psp-sleep");
		// Save before MODE2: it resets the GCVM context regs, so a later save sees no table (s3wake13).
		gartCensus("after psp-sleep");
		gartSave();
		// Guest S3 keeps the APU powered: the MEC that hung on Apple's sleep-entry UNMAP_QUEUES
		// (s3wake11: KIQ rptr 86/a0, MEC2 IP 4a1) is still hung on wake and never fetches the KIQ
		// ring again. Reset GFX like at shutdown (-rmbnos3mode2: don't).
		if (!checkKernelArgument("-rmbnos3mode2")) {
			haltGfxEngines("at sleep");
			smuExitReset("psp-sleep");
		}
	} else {
		gartCensus("after psp-sleep");
		gartSave();
	}
	vramCrcStep("psp-sleep");
	gGpuDownForSleep = true;
	LOG("s3guard: GPU down for sleep (teardown %s)", light && gPspRingStop && !checkKernelArgument("-rmbnos3psp") ? "done" : "skipped");
	diskLogSync(1000);
	return r;
}

// psp_np_fw_load walks HWLibs' firmware list and asks this which types to load (HWLibs' own type
// numbering; the PSP failure event tag is this type). Skip Navi 23-only images Rembrandt lacks.
bool skipFwType(uint32_t type) {
	switch (type) {
		case 30:  // global tap delays (Navi 23 RLC v2.4 only)
		case 31:  // SE0 tap delays
		case 32:  // SE1 tap delays
			return true;
		default:
			return false;
	}
}

mach_vm_address_t orgCapCheck;
uint8_t wrapCapCheck(void *psp,uint32_t type) {
 if(!orgCapCheck) return 0;
 if(skipFwType(type)) return 0;
 uint8_t r=reinterpret_cast<uint8_t (*)(void *,uint32_t)>(orgCapCheck)(psp,type);
 if(r && type == 35) dmubResetForLoad();
 return r;
}

// DMCUB: dmcub_set_fw_entry_info sends DCN 3.1.x to dmcub_dcn31_get_fw_constrant, which only
// records the version; HWLibs has no DCN 3.1 image, so the DMUB firmware is never handed to the
// PSP and DAL's DMUB never comes up. Queue Rembrandt's signed image the way the DCN 3.0.2 path
// queues atidmcub_0.dat: dmcub_load_fw(dmcub, name, data, size, fw entry list).
mach_vm_address_t orgDcn31FwConstraint, gDmcubLoadFw;
uint64_t wrapDcn31FwConstraint(uint64_t ver, uint64_t dmcub, uint64_t list, uint64_t d, uint64_t e, uint64_t f) {
	auto r = reinterpret_cast<Fn6>(orgDcn31FwConstraint)(ver, dmcub, list, d, e, f);
	uint64_t ok = 0;
	if (gDmcubLoadFw)
		ok = reinterpret_cast<Fn6>(gDmcubLoadFw)(dmcub, reinterpret_cast<uint64_t>("atidmcub_0.dat"),
			reinterpret_cast<uint64_t>(fw_yc_dmcub), FW_SIZE(dmcub), list, 0) & 0xFF;
	LOG("fw: DCN %llx: queued yellow_carp DMCUB (%#x bytes) -> %llu", ver & 0xFFFFFFFF, FW_SIZE(dmcub), ok);
	return r;
}

}  // namespace


void firmwareRoute(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
	const Substitution subs[] {
		{"_gc_10_3_4_me_ucode", fw_yc_me, FW_SIZE(me)},
		{"_gc_10_3_4_pfp_ucode", fw_yc_pfp, FW_SIZE(pfp)},
		{"_gc_10_3_4_ce_ucode", fw_yc_ce, FW_SIZE(ce)},
		{"_gc_10_3_4_mec_ucode", fw_yc_mec, FW_SIZE(mec)},
		{"_gc_10_3_4_mec_jt_ucode", fw_yc_mec_jt, FW_SIZE(mec_jt)},
		{"_gc_10_3_4_rlc_ucode", fw_yc_rlc, FW_SIZE(rlc)},
		{"_gc_10_3_4_rlc_srlist_cntl", fw_yc_srlist_cntl, FW_SIZE(srlist_cntl)},
		{"_gc_10_3_4_rlc_srlist_gpm_mem", fw_yc_srlist_gpm, FW_SIZE(srlist_gpm)},
		{"_gc_10_3_4_rlc_srlist_srm_mem", fw_yc_srlist_srm, FW_SIZE(srlist_srm)},
		{"_gc_10_3_4_rlc_lx6_iram_ucode", fw_yc_rlc_iram, FW_SIZE(rlc_iram)},
		{"_gc_10_3_4_rlc_lx6_dram_ucode", fw_yc_rlc_dram, FW_SIZE(rlc_dram)},
	};
	gDmcubLoadFw = findSymbol(patcher, 0, "_dmcub_load_fw", address, size);
	KernelPatcher::RouteRequest reqs[] {
		{"_psp_tmr_init", wrapPspTmrInit, orgPspTmrInit},
		{"_psp_np_fw_load_capability_check", wrapCapCheck, orgCapCheck},
		{"_dmcub_dcn31_get_fw_constrant", wrapDcn31FwConstraint, orgDcn31FwConstraint},
	};
 patcher.clearError();
 size_t dataSpan = size > 0x1afa000 ? size : 0x1afa000;
 uint8_t *descriptors[arrsize(subs)]{};
 for(size_t i=0;i<arrsize(subs);++i) {
  auto target = findSymbol(patcher, 0, subs[i].symbol, address, dataSpan);
  auto desc = reinterpret_cast<uint8_t *>(target);
  auto value=reinterpret_cast<uintptr_t>(desc);
  if(!desc||dataSpan<0x38||value<address||value-address>dataSpan-0x38) {
   LOG("fw: firmwareRoute FAILED symbol %s desc=%p", subs[i].symbol, desc);
   restoredRoutingAbort();return;
  }
  descriptors[i]=desc;
 }
 if(!restoredRouteGroup(patcher, 0, reqs, arrsize(reqs), address, size)) {
  LOG("fw: firmwareRoute FAILED restoredRouteGroup");
  return;
 }
 // Fault in private Copy-On-Write __DATA pages while interrupts are ENABLED before setKernelWriting(true) (cli).
 for(size_t i=0;i<arrsize(subs);++i) {
  uint32_t w=*reinterpret_cast<const volatile uint32_t *>(descriptors[i]+0xc);
  *reinterpret_cast<volatile uint32_t *>(descriptors[i]+0xc)=w;
 }
 if(MachInfo::setKernelWriting(true,KernelPatcher::kernelWriteLock)!=KERN_SUCCESS) {restoredRoutingAbort();return;}
 for(size_t i=0;i<arrsize(subs);++i) {
  *reinterpret_cast<uint32_t *>(descriptors[i]+0xc)=subs[i].size;
  *reinterpret_cast<const uint8_t **>(descriptors[i]+0x20)=subs[i].data;
 }
 if(MachInfo::setKernelWriting(false,KernelPatcher::kernelWriteLock)!=KERN_SUCCESS) restoredRoutingAbort();
 LOG("fw: substituted %zu yellow_carp GC firmware descriptors", arrsize(subs));
 // Separate group: a failure here must not undo or skip the TMR/TOC routes and GC substitution above.
 KernelPatcher::RouteRequest cfgReqs[] {
		{"_gc_get_config", wrapGcGetConfig, orgGcGetConfig},
		{"_sdma_get_config", wrapSdmaGetConfig, orgSdmaGetConfig},
	};
 bool cfgOk = restoredRouteGroup(patcher, 0, cfgReqs, arrsize(cfgReqs), address, size);
 LOG("fw: PSP firmware-load config guards: gc %s, sdma %s%s", orgGcGetConfig ? "routed" : "NOT routed",
     orgSdmaGetConfig ? "routed" : "NOT routed", cfgOk ? "" : " (group reported failure)");
 // Bare-metal S3, own group: a failure leaves sleep as before (no teardown) and touches nothing above.
 if (checkKernelArgument("-rmbnos3")) {
  LOG("s3: off (-rmbnos3)");
  return;
 }
 gPspTmrUnload = findSymbol(patcher, 0, "_psp_tmr_unload", address, size);
 gPspRingStop = findSymbol(patcher, 0, "_psp_ring_stop", address, size);
 size_t found = 0;
 for (size_t i = 0; i < arrsize(pspTeardown); i++) {
  gPspTeardown[i] = findSymbol(patcher, 0, pspTeardown[i], address, size);
  found += gPspTeardown[i] != 0;
 }
 patcher.clearError();
 if (!gPspRingStop || found != arrsize(pspTeardown)) {
  LOG("s3: PSP teardown symbols missing (ring_stop %d, %zu/%zu unloads), S3 path NOT armed", gPspRingStop != 0, found,
      arrsize(pspTeardown));
  gPspRingStop = 0;
  return;
 }
 KernelPatcher::RouteRequest s3Reqs[] {
  {"_psp_power_down_event", wrapPspPowerDown, orgPspPowerDown},
 };
 bool s3Ok = restoredRouteGroup(patcher, 0, s3Reqs, arrsize(s3Reqs), address, size) && orgPspPowerDown;
 patcher.clearError();
 gS3Enabled = s3Ok;
 LOG("s3: psp_power_down_event %s, teardown %zu unloads + ring_stop resolved", s3Ok ? "routed" : "NOT routed", found);
}
