// Original 680M SMU mailbox retained for Rembrandt; no new backend.
// Real PMFW response codes are preserved. The original generic success stubs
// and automatic MODE2/DisallowGfxOff are not a validated Lenovo contract.
// Apple init refuses to publish a fabricated handle or an operation table.

#include "rmb.hpp"
#include "smu_powerplay.hpp"
#include "dispatch.hpp"
#include "tahoe_hooks.hpp"
#include <IOKit/IOLib.h>

namespace {
uint8_t gMailboxLock;
bool gMailboxPoisoned;
bool gExitHooksReady;
struct SmuContext {
 uint8_t phase;
 uint8_t operationLock;
 rem660::smu::OwnedDmaTable table;
 rem660::smu::HdpBinding hdp;
 rem660::smu::PowerUpPreparation powerUp;
 rem660::smu::PowerBudget powerBudget;
 uint32_t appleClockMask=rem660::smu::ColdComputeClockMask; // exact native minimum, no CU inference
 uint64_t requiredPmfwFeatures=rem660::smu::YellowCarpGfxDpm; // minimal advertised GFX capability
}; // private to this shim, never passed to native COS free
SmuContext gContext{};
rem660::smu::GvmServices gGvmServices{};
uintptr_t orgGvmHwInit,orgGvmHwExit,orgGvmSwExit;
mach_vm_address_t orgSmuHwExit = 0;
mach_vm_address_t orgSmuSwExit = 0;
using GvmInit=uint32_t (*)(void *,void *,void *);
using GvmExit=uint32_t (*)(void *);

uint32_t wrapGvmHwInit(void *gvm,void *input,void *output) {
 if(!orgGvmHwInit) return rem660::smu::NotReady;
 const auto status=reinterpret_cast<GvmInit>(orgGvmHwInit)(gvm,input,output);
 if(status) return status; // never publish a failed native initialization
 if(__atomic_exchange_n(&gContext.operationLock,1,__ATOMIC_ACQUIRE)) return status;
 struct Unlock { ~Unlock(){__atomic_store_n(&gContext.operationLock,0,__ATOMIC_RELEASE);} } unlock;
 const auto bound=rem660::smu::prepareGvmHdp(gvm,gGvmServices,gContext.hdp);
 LOG("HDP: native GVM init=%#x, DMA binding=%#x",status,bound);
 return status; // native GVM result; SMU table access separately requires binding
}
uint32_t exitGvm(void *gvm,uintptr_t original) {
 if(!original) return rem660::smu::NotReady;
 if(__atomic_exchange_n(&gContext.operationLock,1,__ATOMIC_ACQUIRE)) return rem660::smu::NotReady;
 struct Unlock { ~Unlock(){__atomic_store_n(&gContext.operationLock,0,__ATOMIC_RELEASE);} } unlock;
 if(gvm==gContext.hdp.gvm) {
  gContext.powerUp={}; // observations expire before the borrowed GVM/HDP
  gContext.powerBudget={};
  if(__atomic_load_n(&gContext.phase,__ATOMIC_ACQUIRE)==1||gContext.table.lease.inFlight) {
   // Apple upper-level teardown may ignore an exit error. Never reuse the
   // borrowed CGS/GVM pointers on a later callback after such an attempt.
   rem660::smu::retireGvmHdp(gContext.table,gContext.hdp);
   __atomic_store_n(&gContext.phase,5,__ATOMIC_RELEASE);
   return rem660::smu::NotReady; // retain uncertain allocation; no reset/free
  }
  if(gContext.table.owned) {
   const auto released=rem660::smu::releaseTable(gContext.table);
   if(released) {
    rem660::smu::retireGvmHdp(gContext.table,gContext.hdp);
    __atomic_store_n(&gContext.phase,5,__ATOMIC_RELEASE);return released;
   }
  }
  rem660::smu::retireGvmHdp(gContext.table,gContext.hdp);
 }
 return reinterpret_cast<GvmExit>(original)(gvm);
}
uint32_t wrapGvmHwExit(void *gvm) { return exitGvm(gvm,orgGvmHwExit); }
uint32_t wrapGvmSwExit(void *gvm) { return exitGvm(gvm,orgGvmSwExit); }

// MP1 mailbox (Linux smu_v13_0_set_smu_mailbox_registers): MP1 seg0 0x16000 + C2PMSG_66/82/90.
constexpr uint32_t MP1Base = 0x16000;
constexpr uint32_t RegMsg = MP1Base + 0x282, RegArg = MP1Base + 0x292, RegResp = MP1Base + 0x29a;

enum : uint32_t {
	PPSMC_MSG_TestMessage = 0x01,
	PPSMC_MSG_GetSmuVersion = 0x02,
	PPSMC_MSG_GfxDeviceDriverReset = 0x11,
	PPSMC_MSG_DisallowGfxOff = 0x1A,
};

bool waitResp(uint32_t &resp, int ms = 1000) {
	if(ms<=0||ms>5000) return false;
	for (unsigned i = 0; i < static_cast<unsigned>(ms) * 100; i++) {
		resp = mmioRead(RegResp);
		if (resp == 0xffffffffu) return false; // unmapped/invalid aperture is not a PMFW reply
		if (resp)
			return true;
		IODelay(10);
	}
	return false;
}

// Returns the PMFW response (1 = OK); the result argument is read back into arg.
uint32_t sendMsg(uint32_t msg, uint32_t &arg, int ms = 1000) {
	// Reuse the original transaction, including response/argument/message order.
	// No reset, GfxOff or unreviewed message can reach the aperture through it.
	switch(msg) {
	 case 1:case 2:case 3:case 0x09:case 0x0d:case 0x0e:case 0x0f:case 0x12:
	 case 0x13:case 0x17:case 0x18:case 0x1b:case 0x1c:case 0x1d:case 0x24:break;
	 // VCN/JPEG power (smuVcnMsg only): Linux yellow_carp_dpm_set_vcn_enable / _jpeg_enable.
	 case 0x06:case 0x07:case 0x21:case 0x22:break;
	 // FCLK hard min (smuGfxClkMsg only): Linux yellow_carp_set_soft_freq_limited_range(SMU_FCLK).
	 case 0x23:break;
	 default:return 0;
	}
	if(ms<=0||ms>5000||__atomic_exchange_n(&gMailboxLock,1,__ATOMIC_ACQUIRE)) return 0;
	struct Unlock { ~Unlock(){__atomic_store_n(&gMailboxLock,0,__ATOMIC_RELEASE);} } unlock;
	if(gMailboxPoisoned) return 0;
	uint32_t resp;
	if (!waitResp(resp)) {
		LOG("smu: mailbox busy before msg %#x", msg);
		return 0;
	}
	mmioWrite(RegResp, 0);
	mmioWrite(RegArg, arg);
	mmioWrite(RegMsg, msg);
	if (!waitResp(resp, ms)) {
		gMailboxPoisoned=true; // completion/ownership unknown; no automatic retry
		LOG("smu: msg %#x timed out after %d ms", msg, ms);
		return 0;
	}
	if (resp == 1) arg = mmioRead(RegArg);
	else if(resp!=0xff&&resp!=0xfe&&resp!=0xfd&&resp!=0xfc) gMailboxPoisoned=true;
	return resp; // preserve the actual PMFW response, including rejection
}

// The original21 opStub callbacks returned success for every operation.
// They cannot be published as real Lenovo operations without their contracts.
// Keep the original mailbox transport, not a replacement PSP/SMU backend.
uint32_t smuSwInit(void *in, void *out) {
 // Never expose our owned handle to native smu_sw_exit/COS release. Both exit
 // routes must have completed first; a partial/failed install cannot publish.
 if(!__atomic_load_n(&gExitHooksReady,__ATOMIC_ACQUIRE)||restoredRoutingFailed()) return 2;
 uint8_t expected=0;
 if(!__atomic_compare_exchange_n(&gContext.phase,&expected,1,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) return 2;
 if(!out) {__atomic_store_n(&gContext.phase,0,__ATOMIC_RELEASE);return 1;}
 auto ready=*static_cast<rem660::smu::SwOutput *>(out);
 auto result=rem660::smu::prepareSwOutput(static_cast<const rem660::smu::SwInput *>(in),
     &ready,&gContext,sendMsg);
 if(!result) result=rem660::smu::bindSwDmaProvider(static_cast<const rem660::smu::SwInput *>(in),gContext.table);
 if(result) {
  // Never publish an owned handle on failed allocation. A failed cleanup
  // retains its owner and permanently quarantines this session, without retry.
  if(gContext.table.owned) rem660::smu::releaseTable(gContext.table);
  if(!gContext.table.owned) gContext.table.cgs=nullptr;
  __atomic_store_n(&gContext.phase,gContext.table.owned?5:0,__ATOMIC_RELEASE);
 } else {
  *static_cast<rem660::smu::SwOutput *>(out)=ready;
  __atomic_store_n(&gContext.phase,2,__ATOMIC_RELEASE);
 }
 return result;
}

uint32_t dispatchDpm(void *handle,uint32_t operation,const void *input,void *output) {
 return smuDpmControl(handle,operation,input,output);
}
static uint32_t readOwnedSmuTable(void *owner,uint32_t table,void *output,size_t bytes) {
 auto owned=static_cast<rem660::smu::OwnedDmaTable *>(owner);
 if(!owned) return rem660::smu::NotReady;
 auto status=rem660::smu::bindOwnedHdp(*owned,gContext.hdp);
 if(status) return status;
 status=rem660::smu::bindSwDmaTable(*owned);
 if(status) return status;
 if(table!=4&&table!=7) {
  if(output&&bytes) __builtin_memset(output,0,bytes);
  return rem660::smu::Ok;
 }
 return rem660::smu::readDmaTable(sendMsg,owned->lease,table,output,bytes);
}
uint32_t dispatchEvent(void *handle,const rem660::smu::EventInput *input,void *output) {
 if(handle!=&gContext) return rem660::smu::NotReady;
 if(__atomic_exchange_n(&gContext.operationLock,1,__ATOMIC_ACQUIRE)) return rem660::smu::NotReady;
 struct Unlock { ~Unlock(){__atomic_store_n(&gContext.operationLock,0,__ATOMIC_RELEASE);} } unlock;
 if(__atomic_load_n(&gContext.phase,__ATOMIC_ACQUIRE)!=2) return rem660::smu::NotReady;
 if(input&&input->size==sizeof(*input)&&
    (input->event==0||input->event==4||input->event==6||input->event==10)) {
  const rem660::smu::Services services{sendMsg,readOwnedSmuTable,&gContext.table,gContext.appleClockMask};
  const auto status=rem660::smu::compatPowerUpEvent(services,input,gContext.powerUp);
  LOG("smu: event=%u cold-software-init=%u result=%#x (GPU execution unverified)",
      input->event,gContext.powerUp.coldInitialized,status);
  return status;
 }
 return rem660::smu::compatEventNotify(input,output);
}
uint32_t dispatchPower(void *handle,uint32_t operation,const void *input,void *output) {
 if(handle!=&gContext) return rem660::smu::NotReady;
 if(__atomic_exchange_n(&gContext.operationLock,1,__ATOMIC_ACQUIRE)) return rem660::smu::NotReady;
 struct Unlock { ~Unlock(){__atomic_store_n(&gContext.operationLock,0,__ATOMIC_RELEASE);} } unlock;
 if(__atomic_load_n(&gContext.phase,__ATOMIC_ACQUIRE)!=2) return rem660::smu::NotReady;
 const rem660::smu::Services services{sendMsg,readOwnedSmuTable,&gContext.table,gContext.appleClockMask};
 return rem660::smu::compatPowerControl(services,operation,input,output,gContext.powerBudget);
}
uint32_t dispatchThermal(void *handle,uint32_t operation,const void *input,void *output) {
 if(handle!=&gContext) return rem660::smu::NotReady;
 if(__atomic_exchange_n(&gContext.operationLock,1,__ATOMIC_ACQUIRE)) return rem660::smu::NotReady;
 struct Unlock { ~Unlock(){__atomic_store_n(&gContext.operationLock,0,__ATOMIC_RELEASE);} } unlock;
 if(__atomic_load_n(&gContext.phase,__ATOMIC_ACQUIRE)!=2) return rem660::smu::NotReady;
 return rem660::smu::compatThermalControl(operation,input,output);
}
uint32_t dispatchFan(void *handle,uint32_t operation,const void *input,void *output) {
 if(handle!=&gContext) return rem660::smu::NotReady;
 if(__atomic_exchange_n(&gContext.operationLock,1,__ATOMIC_ACQUIRE)) return rem660::smu::NotReady;
 struct Unlock { ~Unlock(){__atomic_store_n(&gContext.operationLock,0,__ATOMIC_RELEASE);} } unlock;
 if(__atomic_load_n(&gContext.phase,__ATOMIC_ACQUIRE)!=2) return rem660::smu::NotReady;
 return rem660::smu::compatFanControl(operation,input,output);
}
uint32_t dispatchFeatureCaps(void *handle,uint64_t *output) {
 if(handle!=&gContext) return rem660::smu::NotReady;
 if(__atomic_exchange_n(&gContext.operationLock,1,__ATOMIC_ACQUIRE)) return rem660::smu::NotReady;
 struct Unlock { ~Unlock(){__atomic_store_n(&gContext.operationLock,0,__ATOMIC_RELEASE);} } unlock;
 if(__atomic_load_n(&gContext.phase,__ATOMIC_ACQUIRE)!=2) return rem660::smu::NotReady;
 return rem660::smu::compatPowerFeatureCaps(sendMsg,output);
}
uint32_t dispatchSystemFeatures(void *handle,const rem660::smu::SystemFeatureInput *input) {
 if(handle!=&gContext) return rem660::smu::NotReady;
 if(__atomic_exchange_n(&gContext.operationLock,1,__ATOMIC_ACQUIRE)) return rem660::smu::NotReady;
 struct Unlock { ~Unlock(){__atomic_store_n(&gContext.operationLock,0,__ATOMIC_RELEASE);} } unlock;
 if(__atomic_load_n(&gContext.phase,__ATOMIC_ACQUIRE)!=2) return rem660::smu::NotReady;
 return rem660::smu::systemFeaturesControl(sendMsg,input,gContext.requiredPmfwFeatures);
}
uint32_t smuHwInit(void *handle,void *in,void *out) {
	REM_IOLOG("[smuHwInit] initializing SMU hardware interfaces handle=%p in=%p out=%p\n", handle, in, out);
	if(handle!=&gContext||__atomic_load_n(&gContext.phase,__ATOMIC_ACQUIRE)!=2||!in||!out) return 1;
	if(rem660::word(in,0)!=4||rem660::word(out,0)!=0xb0) return 1;
	if(__atomic_exchange_n(&gContext.operationLock,1,__ATOMIC_ACQUIRE)) return 2;
	struct Unlock { ~Unlock(){__atomic_store_n(&gContext.operationLock,0,__ATOMIC_RELEASE);} } unlock;
	if(__atomic_load_n(&gContext.phase,__ATOMIC_ACQUIRE)!=2) return 2;
	auto candidate=*static_cast<rem660::smu::HwOutput *>(out);
	if(!rem660::smu::buildHwCandidate(candidate,dispatchDpm,dispatchSystemFeatures,dispatchEvent,dispatchFeatureCaps,dispatchPower,dispatchThermal,dispatchFan)) return 1;

	if(rem660::smu::missingPowerPlayContracts()) return 4;

	// Publish the complete HW0xb0 table without refusing exit
	*static_cast<rem660::smu::HwOutput *>(out)=candidate;
	applyGcGolden();
	startGoldenGuard();
	dmubPollerStart();
	REM_IOLOG("[smuHwInit] SMU hardware init SUCCESS (status=0)\n");
	return 0;
}

uint32_t smuHwExit(void *) { return 1; // no live handle is owned
}

uint32_t smuSwExit(void *handle) {
 if(handle!=&gContext) return 1;
 if(__atomic_exchange_n(&gContext.operationLock,1,__ATOMIC_ACQUIRE)) return 2;
 struct Unlock { ~Unlock(){__atomic_store_n(&gContext.operationLock,0,__ATOMIC_RELEASE);} } unlock;
 uint8_t expected=2;
 if(!__atomic_compare_exchange_n(&gContext.phase,&expected,3,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) return 1;
 gContext.powerUp={};
 gContext.powerBudget={};
 if(gContext.table.owned) {
  auto result=rem660::smu::releaseTable(gContext.table);
  if(result) {__atomic_store_n(&gContext.phase,2,__ATOMIC_RELEASE);return result;}
 }
 gContext.table.cgs=nullptr; // release the borrowed provider reference too
 __atomic_store_n(&gContext.phase,4,__ATOMIC_RELEASE);
 // Cold-compute scope: do not recycle this pointer for a later generation
 // while an unproven Apple caller might retain a stale handle (ABA).
 return 0; // owned query-only software session released exactly once; no native free
}

}  // namespace

// Stop the command processors and SDMA the way Linux gfx/sdma hw_fini do (cp_enable/sdma_enable
// false), then wait for the GUI to go idle.
void haltGfxEngines(const char *why) {
	constexpr uint32_t GC0 = 0x1260;
	constexpr uint32_t GrbmStatus = GC0 + 0x0da4, MecCntl = GC0 + 0x0f55, MeCntl = GC0 + 0x0f56,
	                   Sdma0F32Cntl = GC0 + 0x002a;
	auto before = mmioRead(GrbmStatus);
	mmioWrite(MecCntl, (1u << 30) | (1u << 28));                              // MEC_ME1/ME2_HALT
	mmioWrite(MeCntl, mmioRead(MeCntl) | (1u << 28) | (1u << 26) | (1u << 24));  // ME/PFP/CE_HALT
	mmioWrite(Sdma0F32Cntl, mmioRead(Sdma0F32Cntl) | 1);                         // HALT
	int ms = 0;
	while ((mmioRead(GrbmStatus) & (1u << 31)) && ms < 200) {
		IODelay(1000);
		ms++;
	}
	LOG("halt %s: GRBM_STATUS %x -> %x after %d ms, MEC_CNTL %x ME_CNTL %x SDMA0_F32_CNTL %x", why, before,
	    mmioRead(GrbmStatus), ms, mmioRead(MecCntl), mmioRead(MeCntl), mmioRead(Sdma0F32Cntl));
}

// Linux resets the APU's GFX (yellow_carp_mode2_reset) only after psp_suspend; doing it at the next
// VM start instead finds the CP running again (QEMU's device reset clears our halt) and PMFW never
// answers (restart2). Called at guest shutdown after the PSP teardown (-rmbnoexitreset: don't).
void smuExitReset(const char *tag) {
 LOG("%s: automatic MODE2 prohibited without Lenovo-specific evidence",tag);
}

void smuRoute(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
	patcher.clearError();
	// Pure native discovery helpers, UUID/text/bytes checked before publication.
	uintptr_t services[2]{};
	const char *names[]{"_gvm_get_hwip_discovery_info","_soc15_reg_offset"};
	for(unsigned i=0;i<2;++i) {
	 auto window=rem660::abi25::findWindow(names[i]);
	 auto target=findSymbol(patcher, 0, names[i], address, size);
	 if(!window||!target||target!=address+window->vmaddr||!rem660::abi25::verifiedTextBytes(
	     reinterpret_cast<const void *>(address),size,window->image,window->vmaddr,window->bytes,32)) {
	  LOG("smu: smuRoute FAILED service %s target=%lx", names[i], target);
	  restoredRoutingAbort();return;
	 }
	 services[i]=target;
	}
	gGvmServices={reinterpret_cast<decltype(gGvmServices.discovery)>(services[0]),
	              reinterpret_cast<decltype(gGvmServices.registerOffset)>(services[1])};
	KernelPatcher::RouteRequest requests[] {
		{"_smu_sw_init", smuSwInit},
		{"_smu_hw_init", smuHwInit},
		{"_smu_hw_exit", smuHwExit, orgSmuHwExit},
		{"_smu_sw_exit", smuSwExit, orgSmuSwExit},
		{"_gvm_hw_init",wrapGvmHwInit,orgGvmHwInit},
		{"_gvm_hw_exit",wrapGvmHwExit,orgGvmHwExit},
		{"_gvm_sw_exit",wrapGvmSwExit,orgGvmSwExit},
	};
	if(!restoredRouteGroup(patcher, 0, requests, arrsize(requests), address, size)) {
	 LOG("smu: smuRoute FAILED restoredRouteGroup");
	 return;
	}
	__atomic_store_n(&gExitHooksReady,true,__ATOMIC_RELEASE);
	LOG("smu: native-status shims installed; complete operation table still required");
}

// Concrete missing prerequisite, not an opt-in feature or a hardware-success gate.
uint32_t smuRestoreBlockers() {
 // CGS + real HDP adapter/lifetime are implemented; actual hardware remains
 // untested. Required PowerPlay init contracts remain open;
 // see evidence/smu_path/_PhwNavi21_EnableDPMTasks.txt, not all21 stubs.
 return rem660::smu::missingPowerPlayContracts() ? 2u : 0u;
}
bool smuRestoreReady() { return smuRestoreBlockers()==0; }

uint32_t smuDpmControl(void *handle,uint32_t operation,const void *input,void *output) {
 if(handle!=&gContext) return rem660::smu::NotReady;
 if(__atomic_exchange_n(&gContext.operationLock,1,__ATOMIC_ACQUIRE)) return rem660::smu::NotReady;
 struct Unlock { ~Unlock(){__atomic_store_n(&gContext.operationLock,0,__ATOMIC_RELEASE);} } unlock;
 if(__atomic_load_n(&gContext.phase,__ATOMIC_ACQUIRE)!=2) return rem660::smu::NotReady;
 // Reuse the native CGS allocator on demand; preserve its MC address.
 // No table command runs without a separately bound real HDP readback service.
 const rem660::smu::Services services{sendMsg,readOwnedSmuTable,&gContext.table,gContext.appleClockMask};
 const auto status=rem660::smu::dpmControl(services,operation,input,output);
 // Which DPM operations Apple really issues (a native op 4 would compete with the gfxclk policy).
 static uint32_t logged;
 if(logged<24) {
  ++logged;
  uint32_t w[4]{};
  if(input&&(operation==3||operation==4)) __builtin_memcpy(w,input,operation==4?16:8);
  LOG("smu: dpm op %u in %x/%x/%x status %#x (table owned %u usable %u)",operation,w[0],w[1],w[2],status,
      gContext.table.owned,gContext.table.usable);
 }
 return status;
}

// gfxclk.cpp service: only the reviewed Yellow Carp GFX DPM messages that dpmControl already sends
// (GetSmuVersion 0x02, GetEnabledSmuFeatures 0x12, GetGfxclkFrequency 0x17, SetSoftMaxGfxClk 0x1B,
// SetHardMinGfxClk 0x1C, and the SOCCLK pair of dpmControl's Soc row: SetHardMinSocclkByFreq 0x13,
// SetSoftMaxSocclkByFreq 0x1D), and the FCLK pair Linux uses for its PEAK/HIGH levels (GetFclkFrequency 0x18,
// SetHardMinFclkByFreq 0x23, yellow_carp_set_soft_freq_limited_range), only while the published SW/HW session is live. Raw PMFW response
// (1 = OK), 0 = not sent (mailbox busy/poisoned or no session). No GfxOff, reset or table command.
bool smuGfxClkLive() { return __atomic_load_n(&gContext.phase,__ATOMIC_ACQUIRE)==2; }
uint32_t smuGfxClkMsg(uint32_t msg,uint32_t &arg) {
 if(msg!=0x02&&msg!=0x12&&msg!=0x17&&msg!=0x1b&&msg!=0x1c&&msg!=0x13&&msg!=0x1d&&msg!=0x18&&msg!=0x23) return 0;
 if(!smuGfxClkLive()) return 0;
 return sendMsg(msg,arg,1000);
}
// vcn.cpp service: VCN/JPEG power exactly as Linux yellow_carp_dpm_set_vcn_enable / _jpeg_enable send it
// (smu_v13_0_1_ppsmc.h: PowerDownVcn 0x06, PowerUpVcn 0x07, PowerDownJpeg 0x21, PowerUpJpeg 0x22, argument 0),
// only while the published SW/HW session is live. Raw PMFW response (1 = OK), 0 = not sent.
uint32_t smuVcnMsg(uint32_t msg) {
 if(msg!=0x06&&msg!=0x07&&msg!=0x21&&msg!=0x22) return 0;
 if(!smuGfxClkLive()) return 0;
 uint32_t arg=0;
 return sendMsg(msg,arg,1000);
}
// Opt-in (gfxclk ctl `metrics`): SMU table 4/7 through the table the native path allocated, also when its
// owner kind is the post-init one bindSwDmaTable refuses (CPU cache contract unknown). Only if the MC address
// lies in [vramLo, vramHi), the VRAM aperture, so the PMFW DMA lands in this allocation. The copy is taken
// after clflush of every line of the buffer, so a write-back mapping cannot return stale data.
uint32_t smuGfxClkTableAnyKind(uint32_t table,void *output,size_t bytes,uint64_t vramLo,uint64_t vramHi,
                               uint64_t *mc,uint32_t *kind) {
 auto &t=gContext.table;
 if(mc) *mc=t.lease.mcAddress;
 if(kind) {*kind=UINT32_MAX;if(t.handle) __builtin_memcpy(kind,t.handle,4);}
 const size_t need=table==4?sizeof(rem660::smu::DpmClocks):table==7?sizeof(rem660::smu::Metrics):0;
 if(!need||bytes!=need||!output) return rem660::smu::Invalid;
 if(__atomic_exchange_n(&gContext.operationLock,1,__ATOMIC_ACQUIRE)) return rem660::smu::NotReady;
 struct Unlock { ~Unlock(){__atomic_store_n(&gContext.operationLock,0,__ATOMIC_RELEASE);} } unlock;
 if(!smuGfxClkLive()||!t.owned||!t.handle||!t.lease.cpu||t.lease.inFlight||t.lease.bytes<need) return rem660::smu::NotReady;
 const uint64_t a=t.lease.mcAddress;
 if(a<vramLo||a>=vramHi||need>vramHi-a||(a&0xfff)) return rem660::smu::Invalid;
 auto status=rem660::smu::bindOwnedHdp(t,gContext.hdp);
 if(status) return status;
 alignas(8) uint8_t scratch[sizeof(rem660::smu::DpmClocks)];
 status=rem660::smu::readDmaTable(sendMsg,t.lease,table,scratch,need);
 if(status) return status;
 const auto base=reinterpret_cast<uintptr_t>(t.lease.cpu);
 for(uintptr_t line=base&~uintptr_t(63);line<base+need;line+=64)
  asm volatile("clflush (%0)"::"r"(line):"memory");
 asm volatile("mfence":::"memory");
 auto src=static_cast<const volatile uint8_t *>(t.lease.cpu);
 for(size_t i=0;i<need;i++) static_cast<uint8_t *>(output)[i]=src[i];
 return rem660::smu::Ok;
}
// DPM clocks (4) / metrics (7) only through a DMA table the native path already allocated and bound;
// a foreign thread is never the first CGS allocator. *state: 1 owned, 2 usable, 4 in flight, 8 HDP live.
uint32_t smuGfxClkTable(uint32_t table,void *output,size_t bytes,uint32_t *state) {
 if(state) *state=(gContext.table.owned?1u:0u)|(gContext.table.usable?2u:0u)|
                  (gContext.table.lease.inFlight?4u:0u)|(gContext.hdp.live?8u:0u);
 if(table!=4&&table!=7) return rem660::smu::Unsupported;
 if(__atomic_exchange_n(&gContext.operationLock,1,__ATOMIC_ACQUIRE)) return rem660::smu::NotReady;
 struct Unlock { ~Unlock(){__atomic_store_n(&gContext.operationLock,0,__ATOMIC_RELEASE);} } unlock;
 if(!smuGfxClkLive()||!gContext.table.owned||!gContext.table.usable) return rem660::smu::NotReady;
 return readOwnedSmuTable(&gContext.table,table,output,bytes);
}

void smuDisallowGfxOff() {
	// DisallowGfxOff (0x1A) is never sent on this APU (unvalidated); no-op.
}

void smuHdpFlush() {
	if (gContext.table.lease.owner && gContext.table.lease.invalidateHdpForCpu) {
		gContext.table.lease.invalidateHdpForCpu(gContext.table.lease.owner);
	}
}

void smuPreGpuCommandSequence() {
	smuDisallowGfxOff();
	smuHdpFlush();
}
