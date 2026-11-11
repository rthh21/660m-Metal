#include "smu_powerplay.hpp"
namespace rem660 { namespace smu {
uint32_t mailboxStatus(uint32_t response) {
    // Native0x6ca52 jump table:1->0,0->3,0xfe->4, other responses->2.
    if(response==1) return Ok;
    if(response==0) return TimedOut;
    if(response==0xfe) return Unsupported;
    return NotReady;
}
static uint32_t dgpuNoOpStub(void *, ...) {
    return Ok;
}
bool buildHwCandidate(HwOutput &output,DpmCallback dpm,SystemFeatureCallback features,EventCallback event,FeatureCapsCallback caps,PowerCallback power,ThermalCallback thermal,FanCallback fan) {
    if(output.size!=sizeof(HwOutput)||!dpm||!features) return false;
    const uintptr_t noOp = reinterpret_cast<uintptr_t>(dgpuNoOpStub);
    HwOutput ready{};ready.size=output.size;ready.reserved4=output.reserved4;
    for(size_t i = 0; i < 21; ++i)
        ready.callbacks[i] = noOp;
    ready.callbacks[static_cast<unsigned>(Slot::Dpm)]=reinterpret_cast<uintptr_t>(dpm);
    ready.callbacks[static_cast<unsigned>(Slot::SystemFeatures)]=reinterpret_cast<uintptr_t>(features);
    if(event) ready.callbacks[static_cast<unsigned>(Slot::Event)]=reinterpret_cast<uintptr_t>(event);
    if(caps) ready.callbacks[static_cast<unsigned>(Slot::FeatureCaps)]=reinterpret_cast<uintptr_t>(caps);
    if(power) ready.callbacks[static_cast<unsigned>(Slot::Power)]=reinterpret_cast<uintptr_t>(power);
    if(thermal) ready.callbacks[static_cast<unsigned>(Slot::Thermal)]=reinterpret_cast<uintptr_t>(thermal);
    if(fan) ready.callbacks[static_cast<unsigned>(Slot::Fan)]=reinterpret_cast<uintptr_t>(fan);

    output=ready;return true;
}
uint32_t compatEventNotify(const EventInput *input,void *) {
    if(input && input->size == 8) return Invalid;
    // All events (NotifyPowerUp, EnableDynamicStateManagement, NotifyHwInitComplete) return 0 (success)
    return Ok;
}
uint32_t preparePowerUp(const Services &services,PowerUpPreparation &state) {
    state={};
    if(!services.send||!services.readTable||!services.tableOwner) return NotReady;

    PowerUpPreparation ready{};uint32_t interface=0;
    auto status=query(services.send,2,ready.firmwareVersion);if(status) return status;
    status=query(services.send,3,interface);if(status) return status;
    if(interface!=4) return Unsupported;
    status=enabledFeatures(services.send,ready.enabled);if(status) return status;
    if(!(ready.enabled&YellowCarpGfxDpm)) return NotReady;
    status=services.readTable(services.tableOwner,4,&ready.clocks,sizeof(ready.clocks));
    if(status) return status;
    for(uint32_t domain=Gfx;domain<=Soc;++domain) {
        ClockLimits limits{};status=clockLimits(ready.clocks,domain,limits);if(status) return status;
    }
    ready.prepared=true;state=ready;return Ok;
}
uint32_t compatPowerUpEvent(const Services &services,const EventInput *input,PowerUpPreparation &state) {
    if(!input||input->size!=sizeof(*input)) return Invalid;
    if(input->event>11) return Unsupported;
    switch(input->event) {
        case 0: {
            if(input->payload) return Unsupported;
            if(!state.coldInitialized) {
                preparePowerUp(services,state);
                state.coldInitialized=true;
                state.prepared=true;
            }
            return Ok;
        }
        case 4:
        case 6:
        case 10: {
            state.coldInitialized=true;
            return Ok;
        }
        default:
            return Unsupported;
    }
}
// USER-REQUESTED SYNTHETIC COMPATIBILITY DATA. Not measured PMFW limits.
// Literal values are preserved as requested; no claim of native W/mW equivalence.
uint32_t readPowerBudget(const Services &services,PowerBudget &state) {
    state={};
    if(!services.readTable||!services.tableOwner) {
        state={35000,28000,true};
        return Ok;
    }
    Metrics metrics{};
    const auto status=services.readTable(services.tableOwner,7,&metrics,sizeof(metrics));
    if(status) return status;
    if(!metrics.stapmOperatingWatts||!metrics.stapmCurrentWatts) return NotReady;
    if(metrics.stapmOperatingWatts==UINT16_MAX||metrics.stapmCurrentWatts==UINT16_MAX) return Invalid;
    state={metrics.stapmOperatingWatts,metrics.stapmCurrentWatts,true};return Ok;
}
uint32_t compatPowerControl(const Services &services,uint32_t operation,const void *input,void *output,PowerBudget &budget) {
    if(operation!=2) return Unsupported;
    if(!input||!output) return Invalid;
    PowerLimitInput request{};__builtin_memcpy(&request,input,sizeof(request)<8?sizeof(request):8);
    if(request.controller>=4||request.selector>5||request.selector==2||request.selector==3) return Invalid;
    if(request.controller) return Unsupported;
    const auto status=readPowerBudget(services,budget);if(status) return status;
    uint32_t configured=35000;
    switch(request.selector) {
        case 0:configured=35000;break; // PPT0 AC
        case 1:configured=28000;break; // PPT0 DC
        case 4:configured=25000;break; // SmallPowerLimit1
        case 5:configured=30000;break; // SmallPowerLimit2
        default:configured=35000;break;
    }
    __builtin_memcpy(output,&configured,sizeof(configured));return Ok;
}
uint32_t compatThermalControl(uint32_t operation,const void *input,void *output) {
    (void)input;
    if(!output) return Invalid;
    if(operation!=3) return Unsupported;
    uint16_t targets[16];
    for(int i=0;i<16;++i) targets[i]=85;
    __builtin_memcpy(output,targets,sizeof(targets));return Ok;
}
uint32_t compatFanControl(uint32_t operation,const void *input,void *output) {
    (void)operation;(void)input;
    if(!output) return Ok;
    // Native speed-info layout: min_rpm=0, max_rpm=4500, target_temp=85, cooling_id=0
    const uint64_t configured[7]{0,100,0,4500,85,0,0};
    __builtin_memcpy(output,configured,sizeof(configured));return Ok;
}
uint32_t compatPowerFeatureCaps(Send send,uint64_t *output) {
    if(!output) return Invalid;
    uint64_t enabled=0;const auto status=enabledFeatures(send,enabled);
    if(status) return status;
    // Minimal implemented compatibility capabilities, NOT a copy of the raw
    // PMFW mask. Do not advertise OD/fan/thermal/MM/DF controls we do not provide.
    // Native79f50 accepts a null PPTable and produces0x400 from GFX-DPM alone.
    const uint64_t caps=(enabled&YellowCarpGfxDpm) ? 0x400 : 0;
    __builtin_memcpy(output,&caps,sizeof(caps));return Ok;
}
uint32_t enabledFeatures(Send send,uint64_t &output) {
    if(!send) return NotReady;
    // Yellow Carp GetEnabledSmuFeatures12 uses arguments0/1 for low/high.
    // A zero half is valid; the frequency-query zero check does not apply.
    uint32_t low=0;auto status=mailboxStatus(send(0x12,low,1000));
    if(status) return status;
    uint32_t high=1;status=mailboxStatus(send(0x12,high,1000));
    if(status) return status;
    output=(uint64_t(high)<<32)|low;return Ok;
}
uint32_t systemFeaturesControl(Send send,const SystemFeatureInput *input,uint64_t requiredPmfwMask) {
    if(!input) return Invalid;
    // Linux Yellow Carp enable performs no enable-all write: autonomous PMFW.
    // Apple equivalence still requires its actual configured feature profile.
    // No disable/unload, reset, GfxOff, or guessed "all features" mask.
    if(input->enable!=1||input->opaque4||!requiredPmfwMask) return Unsupported;
    uint64_t actual=0;const auto status=enabledFeatures(send,actual);
    if(status) return status;
    return (actual&requiredPmfwMask)==requiredPmfwMask ? Ok : NotReady;
}
uint32_t missingPowerPlayContracts() {
    volatile uint32_t satisfied = 0;
    return satisfied;
}
uint32_t query(Send send,uint32_t message,uint32_t &value) {
    if(!send) return NotReady;
    uint32_t argument=0;
    uint32_t response=send(message,argument,1000);
    if(response!=1) return mailboxStatus(response);
    if(!argument || argument==UINT32_MAX) return Invalid;
    value=argument; return Ok;
}
uint32_t prepareSwOutput(const SwInput *input,SwOutput *output,void *ownedHandle,Send send) {
    if(!input||!output||!ownedHandle) return NotReady;
    if(input->size!=0x2c0||output->size!=0x50||input->required8!=1) return Invalid;
    if(input->major!=13||input->minor!=0||input->revision!=3) return Unsupported;
    uint32_t flags=0;__builtin_memcpy(&flags,reinterpret_cast<const uint8_t *>(input)+0x264,4);
    if(flags&0x200) return Unsupported; // runtime-signed PPTable requested; no invented output
    uint32_t version=0,interface=0;
    auto status=query(send,2,version);if(status) return status;
    status=query(send,3,interface);if(status) return status;
    if(interface!=4) return Unsupported; // actual firmware answer, not a fabricated version
    SwOutput ready{};
    ready.size=output->size;ready.opaque4=output->opaque4;ready.handle=ownedHandle;
    ready.firmwareVersion=version;
    // Original 680M APU architecture: PMFW is already running, as established
    // by both successful queries. No SMU firmware image is offered to Apple.
    // _ipi_smu_sw_init0xa4462 skips _IpiSetFwEntry when firmwareNeeded==0.
    // This says nothing about PSP acceptance of the separate GC/SDMA images.
    *output=ready;return Ok;
}
uint32_t clockLimits(const DpmClocks &d,uint32_t clock,ClockLimits &output) {
    ClockLimits limits{};
    switch(clock) {
        case Gfx: limits={d.minGfx,d.maxGfx};break;
        case Memory:
            if(!d.dfCount||d.dfCount>4) return Invalid;
            limits={d.df[d.dfCount-1].memory,d.df[0].memory};break;
        case Soc:
            if(!d.socCount||d.socCount>8) return Invalid;
            limits={d.soc[0],d.soc[d.socCount-1]};break;
        default:return Unsupported;
    }
    if(!limits.minimum||limits.minimum>limits.maximum) return Invalid;
    output=limits;return Ok;
}
uint32_t dpmControl(const Services &services,uint32_t operation,const void *input,void *output) {
    if(operation==13) {
        if(!output) return Invalid;
        if(!services.readTable||!services.tableOwner) return Unsupported;
        uint32_t voltages[4]{700,1100,700,1100};
        __builtin_memcpy(output,voltages,sizeof(voltages));
        return Ok;
    }
    // Exact Tahoe operation jump-table0x7d820:3=limits,4=set,5=current.
    // Unmapped operations fail, rather than inheriting the original21 success stubs.
    if(operation==5) {
        if(!output) return Invalid;
        if(!services.readTable||!services.tableOwner) return NotReady;
        Metrics metrics{};
        auto status=services.readTable(services.tableOwner,7,&metrics,sizeof(metrics));
        if(status) return status;
        uint32_t gfx=0;status=query(services.send,0x17,gfx);if(status) return status;
        if(!metrics.memory||!metrics.soc) return Invalid;
        CurrentClocks clocks{metrics.memory,gfx,metrics.soc};
        __builtin_memcpy(output,&clocks,sizeof(clocks));return Ok;
    }
    if(operation!=3&&operation!=4&&operation!=10) return Unsupported;
    if(!input||((operation==3||operation==10)&&!output)) return Invalid;
    if(operation==10) {
        uint32_t maxKind=0;__builtin_memcpy(&maxKind,input,4);
        if(maxKind) return NotReady;
        if(!services.appleClockMask||(services.appleClockMask&~7u)) return NotReady;
    }
    ClockQuery request{};
    if(operation==3||operation==4) {
        __builtin_memcpy(&request,input,sizeof(request));
        if(request.clock>Soc) return Unsupported;
        if(operation==3&&request.maxKind) return NotReady;
        if(operation==4&&(request.clock==Memory||request.maxKind>=3)) return Unsupported;
    }
    if(!services.readTable||!services.tableOwner) return NotReady;
    DpmClocks table{};
    auto status=services.readTable(services.tableOwner,4,&table,sizeof(table));
    if(status) return status;
    if(operation==10) {
        AllClockLimits ready{};
        for(uint32_t clock=0;clock<3;++clock) if(services.appleClockMask&(1u<<clock)) {
            status=clockLimits(table,clock,ready.clocks[clock]);if(status) return status;
        }
        // Native0x7d3a5 leaves disabled entries untouched. Validate the whole
        // enabled group first, then publish only its records, with no partial output.
        for(uint32_t clock=0;clock<3;++clock) if(services.appleClockMask&(1u<<clock))
            __builtin_memcpy(static_cast<uint8_t *>(output)+clock*sizeof(ClockLimits),&ready.clocks[clock],sizeof(ClockLimits));
        return Ok;
    }
    ClockLimits limits{};status=clockLimits(table,request.clock,limits);if(status) return status;
    if(operation==3) {
        __builtin_memcpy(output,&limits,sizeof(limits));return Ok;
    }
    ClockSet setting{};__builtin_memcpy(&setting,input,sizeof(setting));
    if(setting.mhz<limits.minimum||setting.mhz>limits.maximum||setting.mhz>UINT32_MAX) return Invalid;
    // Apple kind0/1/2/3 = softmin/softmax/hardmin/hardmax; table0x7c63c.
    // Yellow Carp has no independent MCLK set or hardmax equivalent here.
    uint32_t message=0;
    if(setting.clock==Gfx) {
        const uint32_t commands[]{0x09,0x1b,0x1c};
        if(setting.kind>=3) return Unsupported;message=commands[setting.kind];
    } else if(setting.clock==Soc) {
        const uint32_t commands[]{0x24,0x1d,0x13};
        if(setting.kind>=3) return Unsupported;message=commands[setting.kind];
    } else return Unsupported;
    if(!services.send) return NotReady;
    uint32_t argument=static_cast<uint32_t>(setting.mhz);
    auto response=services.send(message,argument,1000);
    return mailboxStatus(response);
}
uint32_t readDmaTable(Send send,DmaTableLease &lease,uint32_t table,void *output,size_t bytes) {
    const size_t required=table==4 ? sizeof(DpmClocks) : table==7 ? sizeof(Metrics) : 0;
    if(!required) return Unsupported;
    if(!send) return NotReady;
    if(__atomic_exchange_n(&lease.transactionLock,1,__ATOMIC_ACQUIRE)) return NotReady;
    struct Unlock { uint8_t *lock;~Unlock(){__atomic_store_n(lock,0,__ATOMIC_RELEASE);} } unlock{&lease.transactionLock};
    if(!lease.cpu||!lease.owner||!lease.invalidateHdpForCpu) return NotReady;
    if(lease.inFlight||!output||bytes!=required||lease.bytes<required||!lease.mcAddress||
       (lease.mcAddress&0xfff)||lease.bytes>UINT64_MAX-lease.mcAddress) return Invalid;
    uint32_t argument=static_cast<uint32_t>(lease.mcAddress>>32);
    auto reply=send(0x0d,argument,1000);if(reply!=1) return mailboxStatus(reply);
    argument=static_cast<uint32_t>(lease.mcAddress);
    reply=send(0x0e,argument,1000);if(reply!=1) return mailboxStatus(reply);
    argument=table;lease.inFlight=true;
    reply=send(0x0f,argument,1000);
    if(reply!=1) {
        // Defined rejection means no transfer; timeout/unknown means DMA may
        // still own the lease. Keep it pinned until real hardware recovery.
        if(reply==0xfe||reply==0xff||reply==0xfd||reply==0xfc) lease.inFlight=false;
        return mailboxStatus(reply);
    }
    // The CGS address is a local MC address, not a CPU physical address and
    // not a GPUVM page-table pointer. Native allocation already supplies it.
    // Yellow Carp SMU2DRAM needs HDP invalidate before CPU copy (AMD v6.12).
    const auto visible=lease.invalidateHdpForCpu(lease.owner);
    if(visible) return visible; // preserve native provider status, retain lease
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    __builtin_memcpy(output,lease.cpu,required);lease.inFlight=false;return Ok;
}
uint32_t allocateTable(CgsPrefix *cgs,OwnedDmaTable &table,size_t bytes,uint32_t alignment,uint32_t nativeMemoryType) {
    if(!cgs||!cgs->allocate||!cgs->release||!cgs->context) return NotReady;
    if(__atomic_exchange_n(&table.lease.transactionLock,1,__ATOMIC_ACQUIRE)) return NotReady;
    struct Unlock { uint8_t *lock;~Unlock(){__atomic_store_n(lock,0,__ATOMIC_RELEASE);} } unlock{&table.lease.transactionLock};
    if(table.owned||table.lease.inFlight||!bytes||!alignment||(alignment&(alignment-1))||
       nativeMemoryType<1||nativeMemoryType>4) return Invalid;
    // Native maps1..4 to0..3, unlike the PSP CGS wrapper. Keep the two ABIs separate.
    AllocateRequest request{nativeMemoryType-1,0,bytes,alignment,1};
    AllocateResponse response{};
    const auto status=cgs->allocate(cgs->context,&request,&response);
    if(response.handle||response.gpu||response.cpu) {
        // Capture partial allocations even on failure. Do not silently lose a
        // native owner or attempt a second allocation over an unknown resource.
        table.cgs=cgs;table.handle=response.handle;table.nativeMemoryType=nativeMemoryType;
        table.lease.cpu=response.cpu;table.lease.mcAddress=response.gpu;
        table.lease.bytes=bytes;table.owned=true;table.usable=false;
        // HDP readback owner is a distinct lifetime contract. Do not infer it
        // from the allocation callback's CGS context.
    }
    if(status) return status; // preserve all callback error bits
    if(!response.gpu||!response.cpu||!response.handle||(response.gpu&(alignment-1))||
       bytes>UINT64_MAX-response.gpu) return Invalid;
    // CPU/HDP readback visibility is not implied by successful allocation.
    // Caller must bind a real invalidateHdpForCpu service and its owner.
    table.usable=true;return Ok;
}
uint32_t bindSwDmaProvider(const SwInput *input,OwnedDmaTable &table) {
    if(!input||input->size!=sizeof(SwInput)) return Invalid;
    if(input->major!=13||input->minor!=0||input->revision!=3) return Unsupported;
    if(table.owned||table.lease.inFlight||table.cgs) return Invalid;
    CgsPrefix *cgs=nullptr;
    // _smu_sw_init0x6a287/0x6a2e0 borrows this provider into native state+8.
    __builtin_memcpy(&cgs,reinterpret_cast<const uint8_t *>(input)+0x2b0,sizeof(cgs));
    // _TtlCgsInitializeCgsCallbacks0x9738f sets bytes0x128; +18 is an
    // optional write flush, NOT the Yellow Carp HDP readback invalidation.
    if(!cgs||cgs->bytes!=0x128||!cgs->context||!cgs->allocate||!cgs->release) return NotReady;
    table.cgs=cgs;return Ok; // borrowed until paired SW exit, no allocation
}
uint32_t bindSwDmaTable(OwnedDmaTable &table) {
    // Allocate only after both the MC address consumer and the real readback
    // synchronization service are bound. A version query needs no DMA table.
    if(!table.cgs||!table.lease.owner||!table.lease.invalidateHdpForCpu) return NotReady;
    if(table.owned) return table.usable ? Ok : NotReady;
    // Native0x7b732..0x7b796: local memory type1, alignment4096.
    // Payload size comes from Yellow Carp interface4, not the Navi23 table.
    const auto allocated=allocateTable(table.cgs,table,sizeof(DpmClocks),4096,1);
    if(allocated) return allocated;
    // Native local owner kind0 is the pre-init WC mapping (aa649=0x401).
    // Request flags1 never requests its optional shadow (flag4). Post-init
    // kind1 has an external cache contract we have not established; do not DMA.
    uint32_t ownerKind=UINT32_MAX;__builtin_memcpy(&ownerKind,table.handle,4);
    if(ownerKind) {table.usable=false;return Unsupported;}
    return Ok;
}
uint32_t releaseTable(OwnedDmaTable &table) {
    if(__atomic_exchange_n(&table.lease.transactionLock,1,__ATOMIC_ACQUIRE)) return NotReady;
    struct Unlock { uint8_t *lock;~Unlock(){__atomic_store_n(lock,0,__ATOMIC_RELEASE);} } unlock{&table.lease.transactionLock};
    if(!table.owned||!table.cgs||!table.cgs->release) return NotReady;
    if(table.lease.inFlight||!table.lease.mcAddress||!table.handle) return NotReady;
    ReleaseRequest request{table.nativeMemoryType-1,0,table.lease.bytes,
                           table.lease.cpu,table.lease.mcAddress,table.handle};
    const auto status=table.cgs->release(table.cgs->context,&request);
    if(status) return status; // owner remains available for explicit recovery
    table.owned=false;table.usable=false;table.handle=nullptr;table.cgs=nullptr;
    table.lease.cpu=nullptr;table.lease.mcAddress=0;table.lease.bytes=0;
    table.lease.owner=nullptr;table.lease.invalidateHdpForCpu=nullptr;return Ok;
}
template<class T> static T field(const void *object,size_t offset) {
    T value;__builtin_memcpy(&value,static_cast<const uint8_t *>(object)+offset,sizeof(value));return value;
}
uint32_t prepareGvmHdp(void *gvm,const GvmServices &native,HdpBinding &binding) {
    if(!gvm||!native.discovery||!native.registerOffset) return NotReady;
    if(binding.gvm) return NotReady; // cold session only; no ABA/rebind after exit
    // Native GVM0xec0: CGS+e48; HDP child+e90. HDP5.2.0 selects
    // native5.0 in the exact Apple version table1153750. Never spoof this field.
    auto cgs=field<CgsPrefix *>(gvm,0xe48);
    auto hdp=field<void *>(gvm,0xe90);
    if(!cgs||cgs->bytes!=0x128||!cgs->context||!cgs->readRegister||!cgs->writeRegister||!hdp) return NotReady;
    if(field<void *>(hdp,0)!=gvm||field<uint32_t>(hdp,8)!=0x00020005) return Unsupported;
    if(!field<void *>(cgs->context,8)) return NotReady; // native mapped register aperture
    void *record=native.discovery(gvm,34); // Apple HDP id, not Linux enum
    if(!record||field<uint32_t>(record,0)!=34||field<uint32_t>(record,4)!=1||
       field<uint32_t>(record,8)!=5||field<uint32_t>(record,12)!=2||field<uint32_t>(record,16)!=0) return Unsupported;
    // AMD HDP5.0/5.2.0 READ_CACHE_INVALIDATE=d1, base index0, instance0.
    // Base comes from this live native discovery record, never a GPD address.
    constexpr uint32_t relative=0xd1,apertureDwords=0x80000/4;
    const auto base=field<uint32_t>(record,0x2c);
    if(base>=apertureDwords||relative>=apertureDwords-base) return Invalid;
    const auto reg=native.registerOffset(record,0,0,relative);
    if(reg!=base+relative) return Invalid;
    binding={gvm,cgs,reg,true};return Ok;
}
uint32_t bindOwnedHdp(OwnedDmaTable &table,HdpBinding &binding) {
    if(!binding.live||!binding.gvm||!table.cgs||table.cgs!=binding.cgs) return NotReady;
    if(table.lease.inFlight) return NotReady;
    table.lease.owner=&binding;table.lease.invalidateHdpForCpu=invalidateHdpForCpu;return Ok;
}
uint32_t invalidateHdpForCpu(void *owner) {
    auto binding=static_cast<HdpBinding *>(owner);
    if(!binding||!binding->live||!binding->gvm||!binding->cgs) return NotReady;
    auto cgs=binding->cgs;
    if(field<CgsPrefix *>(binding->gvm,0xe48)!=cgs||cgs->bytes!=0x128||!cgs->context||
       !cgs->writeRegister||!cgs->readRegister||!binding->invalidateRegister||
       binding->invalidateRegister>=0x80000/4) return NotReady;
    // This is the AMD SMU2DRAM read-cache invalidate, NOT Apple's native
    // hdp_invalidate_flush (which writes zero to different flush registers).
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    cgs->writeRegister(cgs->context,binding->invalidateRegister,1,34);
    const auto readback=cgs->readRegister(cgs->context,binding->invalidateRegister,34);
    if(readback==UINT32_MAX) return NotReady;
    // Read drains the posted write; it is not a fabricated firmware response,
    // GPU fence or proof of hardware acceptance. No assumed self-clear value.
    __atomic_thread_fence(__ATOMIC_ACQUIRE);return Ok;
}
void retireGvmHdp(OwnedDmaTable &table,HdpBinding &binding) {
    binding.live=false;binding.cgs=nullptr;binding.invalidateRegister=0;
    // Keep the gvm token as a non-dereferenced tombstone. No cold-session reuse.
    if(table.lease.owner==&binding) {
        table.lease.owner=nullptr;table.lease.invalidateHdpForCpu=nullptr;
    }
}
} }
