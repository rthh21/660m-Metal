// Yellow Carp adaptation of the original 680M mailbox, not a new SMU backend.
// Apple layout authority: exact25G229 _smu_sw_init/_ipi_smu_sw_init,
// _smu_11_0_7_dpm_control and its callees; evidence/pre_kc/.
#pragma once
#include <stdint.h>
#include <stddef.h>
namespace rem660 { namespace smu {
enum : uint32_t { Ok=0, Invalid=1, NotReady=2, TimedOut=3, Unsupported=4 };
using Send=uint32_t (*)(uint32_t message,uint32_t &argument,int milliseconds);
using ReadTable=uint32_t (*)(void *owner,uint32_t table,void *output,size_t bytes);
struct Services {
    Send send; ReadTable readTable; void *tableOwner;
    uint32_t appleClockMask=0; // explicit provider evidence; never infer a mask from6CU
};
// Linux v6.12 smu13_driver_if_yellow_carp.h, interface4. MHz, not 10kHz.
struct DfPstate { uint32_t fabric,memory,voltage; uint8_t ratio,reserved[3]; };
struct DpmClocks {
    uint32_t dcf[8],display[8],dpp[8],soc[8],vclk[8],dclk[8],socVoltage[8];
    DfPstate df[4];
    uint8_t dcfCount,displayCount,socCount,vcnCount,dfCount,reserved[3];
    uint32_t minGfx,maxGfx;
};
static_assert(sizeof(DpmClocks)==304 && offsetof(DpmClocks,df)==224 &&
              offsetof(DpmClocks,minGfx)==296,"Yellow Carp interface4 DPMCLOCKS");
struct Metrics {
    uint16_t gfx,soc,vclk,dclk,memory,spare;
    uint16_t gfxActivity,vcnActivity,voltage[2],current[2],power[2];
    uint16_t coreFrequency[8],corePower[8],coreTemperature[8];
    uint16_t l3Frequency,l3Temperature,gfxTemperature,socTemperature;
    uint16_t throttlerStatus,socketPowerMilliwatts,stapmOperatingWatts,stapmCurrentWatts;
    uint32_t apuPowerMilliwatts,dgpuPowerMilliwatts;
    uint16_t vddTdc,socTdc,vddEdc,socEdc,cpuMaxFrequency,gfxMaxFrequency,skinTemperature,deviceState;
};
static_assert(sizeof(Metrics)==116 && offsetof(Metrics,memory)==8 &&
 offsetof(Metrics,stapmOperatingWatts)==88 && offsetof(Metrics,stapmCurrentWatts)==90 &&
 offsetof(Metrics,apuPowerMilliwatts)==92,"Yellow Carp metrics interface4");
struct PowerBudget { uint32_t operatingWatts=0,currentWatts=0;bool valid=false; };
uint32_t readPowerBudget(const Services &,PowerBudget &);
// Exact Tahoe power-control op2: selector/controller plus two reserved words.
struct PowerLimitInput { uint32_t selector,controller,reserved8,reservedC; };
static_assert(sizeof(PowerLimitInput)==16,"Tahoe power-limit request");
using PowerCallback=uint32_t (*)(void *,uint32_t,const void *,void *);
using ThermalCallback=uint32_t (*)(void *,uint32_t,const void *,void *);
using FanCallback=uint32_t (*)(void *,uint32_t,const void *,void *);
uint32_t compatPowerControl(const Services &,uint32_t,const void *,void *,PowerBudget &);
uint32_t compatThermalControl(uint32_t,const void *,void *);
uint32_t compatFanControl(uint32_t,const void *,void *);
struct SwInput { uint32_t size,opaque4,required8,major,minor,revision; uint8_t opaque[0x2c0-24]; };
struct SwOutput {
    uint32_t size,opaque4;
    void *handle;
    uint8_t firmwareNeeded,reserved11[3];
    uint32_t firmwareVersion;
    const void *firmware;
    uint32_t firmwareBytes,reserved24;
    void *firmwareOwner;
    uint8_t runtimePptable[0x20];
};
static_assert(sizeof(SwInput)==0x2c0 && sizeof(SwOutput)==0x50,"25G229 SMU SW records");
static_assert(offsetof(SwOutput,firmwareNeeded)==0x10 && offsetof(SwOutput,firmwareVersion)==0x14 &&
              offsetof(SwOutput,firmwareOwner)==0x28 && offsetof(SwOutput,runtimePptable)==0x30,"SW0x50 fields");
struct ClockQuery { uint32_t clock,maxKind; };
struct ClockLimits { uint64_t minimum,maximum; };
struct ClockSet { uint32_t clock,kind; uint64_t mhz; };
struct CurrentClocks { uint64_t memory,gfx,soc; };
struct AllClockLimits { ClockLimits clocks[18]; };
static_assert(sizeof(ClockQuery)==8 && sizeof(ClockLimits)==16 && sizeof(ClockSet)==16 &&
              sizeof(CurrentClocks)==24,"25G229 DPM payloads");
static_assert(sizeof(AllClockLimits)==0x120,"25G229 all-queried clock limits");
// These are Apple clock selectors, NOT Linux clock enum values.
enum : uint32_t { Gfx=0,Memory=1,Soc=2 };
// Minimum Apple cold-compute software clock profile, NOT a CU/fuse mask.
// Native20c99e/20c9ac requires Memory/Soc maxima; exposed GFX capability400
// requires Gfx. Other records are outside this minimum and stay untouched.
// Each requested record still needs real, validated Yellow Carp table data.
inline constexpr uint32_t ColdComputeClockMask=(1u<<Gfx)|(1u<<Memory)|(1u<<Soc);
// Exact25G229 _smu_process_fw_response0x6ca52; raw PMFW replies stay in transport.
uint32_t mailboxStatus(uint32_t response);
uint32_t query(Send,uint32_t message,uint32_t &value);
uint32_t prepareSwOutput(const SwInput *,SwOutput *,void *ownedHandle,Send);
uint32_t clockLimits(const DpmClocks &,uint32_t clock,ClockLimits &);
uint32_t dpmControl(const Services &,uint32_t operation,const void *input,void *output);
// HW0xb0: exact21 slots copied by _ipi_smu_hw_init0xa45ac. Opaque slots
// stay null, never universal success callbacks with guessed argument types.
enum class Slot : uint32_t {
 Event,FullReset,EngineReset,PpTable,Overdrive,Fan,Thermal,Fullscreen,
 Dpm,Power,Ips,Azalia,Ulv,GfxOff,SystemFeatures,I2c,FeatureCaps,Log,Ras,Displays,Stb
};
using DpmCallback=uint32_t (*)(void *,uint32_t,const void *,void *);
struct SystemFeatureInput {uint8_t enable,reserved[3];uint32_t opaque4;};
using SystemFeatureCallback=uint32_t (*)(void *,const SystemFeatureInput *);
struct EventInput {uint32_t size,event;const void *payload;};
using EventCallback=uint32_t (*)(void *,const EventInput *,void *);
using FeatureCapsCallback=uint32_t (*)(void *,uint64_t *);
// AMD Yellow Carp PMFW interface4 GFX_DPM_BIT19; Apple native SMU11
// DPM_GFXCLK_BIT1 is translated to capability0x400 at79f50.
inline constexpr uint64_t YellowCarpGfxDpm=1ULL<<19;
struct HwOutput {uint32_t size,reserved4;uintptr_t callbacks[21];};
static_assert(sizeof(EventInput)==16&&offsetof(EventInput,payload)==8&&
 sizeof(SystemFeatureInput)==8&&sizeof(HwOutput)==0xb0&&
 offsetof(HwOutput,callbacks)==8,"25G229 HW output and feature input");
// Software preparation only: this never grants HW-init/publication readiness.
bool buildHwCandidate(HwOutput &,DpmCallback,SystemFeatureCallback,EventCallback=nullptr,FeatureCapsCallback=nullptr,PowerCallback=nullptr,ThermalCallback=nullptr,FanCallback=nullptr);
uint32_t compatEventNotify(const EventInput *,void *);
// Private software preparation, NOT Apple power_up completion. Caller holds
// the existing SMU operation lock; borrowed DMA ownership stays with Services.
struct PowerUpPreparation {
    uint32_t firmwareVersion=0;
    uint64_t enabled=0;
    DpmClocks clocks{};
    bool prepared=false;
    bool coldInitialized=false; // software SMU setup, never a completed GPU fence/power transition
};
uint32_t preparePowerUp(const Services &,PowerUpPreparation &);
uint32_t compatPowerUpEvent(const Services &,const EventInput *,PowerUpPreparation &);
uint32_t compatPowerFeatureCaps(Send,uint64_t *);
uint32_t enabledFeatures(Send,uint64_t &);
uint32_t systemFeaturesControl(Send,const SystemFeatureInput *,uint64_t requiredPmfwMask);
enum MissingPowerPlay : uint32_t {
 MissingFeatureProfile=1,MissingFeatureCaps=2,MissingPowerLimit=4,
 MissingVoltageLimits=8,MissingFanLimits=16,MissingThermalTargets=32,
 MissingClockProfile=64,MissingDcLimits=128,MissingLifecycleEvents=256
};
uint32_t missingPowerPlayContracts();

// No allocator is fabricated. Caller must hold an existing, pinned DMA lease
// and serialize all table-address programming. Host pointers are never GPUVA.
struct DmaTableLease {
    void *cpu; uint64_t mcAddress; size_t bytes; void *owner;
    uint32_t (*invalidateHdpForCpu)(void *owner); // actual HDP completion/status, never a cache-coherence guess
    bool inFlight=false;             // retained after timeout; no automatic free/retry
    uint8_t transactionLock=0;
};
uint32_t readDmaTable(Send,DmaTableLease &,uint32_t table,void *output,size_t bytes);
// Borrowed native CGS prefix from SW input+0x2b0. Exact25G229
// _smu_cgs_alloc_memory0x6b106/_smu_cgs_release_memory0x6b296.
struct AllocateRequest { uint32_t memoryType,reserved;uint64_t bytes;uint32_t alignment,flags; };
struct AllocateResponse { uint64_t gpu;void *cpu;uint64_t opaque10,opaque18;void *handle; };
struct ReleaseRequest { uint32_t memoryType,reserved;uint64_t bytes;void *cpu;uint64_t gpu;void *handle; };
using Allocate=uint32_t (*)(void *,const AllocateRequest *,AllocateResponse *);
using Release=uint32_t (*)(void *,const ReleaseRequest *);
using WriteRegister=void (*)(void *,uint32_t,uint32_t,uint32_t);
using ReadRegister=uint32_t (*)(void *,uint32_t,uint32_t);
struct CgsPrefix {
    uint32_t bytes,reserved4;void *context;Allocate allocate;void *optionalFlush;Release release;
    void *mapGart=nullptr,*unmapGart=nullptr;
    WriteRegister writeRegister=nullptr;ReadRegister readRegister=nullptr;
};
static_assert(sizeof(AllocateRequest)==24&&sizeof(AllocateResponse)==40&&sizeof(ReleaseRequest)==40&&
              sizeof(CgsPrefix)==0x48&&offsetof(CgsPrefix,context)==8&&
              offsetof(CgsPrefix,allocate)==0x10&&offsetof(CgsPrefix,optionalFlush)==0x18&&
              offsetof(CgsPrefix,release)==0x20&&offsetof(CgsPrefix,writeRegister)==0x38&&
              offsetof(CgsPrefix,readRegister)==0x40,"25G229 CGS prefix records");
struct OwnedDmaTable {
    DmaTableLease lease{};
    CgsPrefix *cgs=nullptr;
    void *handle=nullptr;
    uint32_t nativeMemoryType=0;
    bool owned=false;
    bool usable=false; // ownership alone does not establish successful allocation
};
uint32_t allocateTable(CgsPrefix *,OwnedDmaTable &,size_t bytes,uint32_t alignment,uint32_t nativeMemoryType);
uint32_t releaseTable(OwnedDmaTable &);
// Borrow only the CGS provider in SW init; no SMU table transfer/allocation is required there.
uint32_t bindSwDmaProvider(const SwInput *,OwnedDmaTable &);
// Allocate once when a table consumer needs it. Does not certify HDP visibility.
uint32_t bindSwDmaTable(OwnedDmaTable &);
// Borrowed native GVM services. Only pure lookup/arithmetic functions are
// resolved; no second HDP backend or new native allocation is introduced.
struct GvmServices {
    void *(*discovery)(void *,uint32_t)=nullptr;
    uint32_t (*registerOffset)(void *,uint32_t,uint32_t,uint32_t)=nullptr;
};
struct HdpBinding {
    void *gvm=nullptr;CgsPrefix *cgs=nullptr;uint32_t invalidateRegister=0;
    bool live=false; // set only after successful native GVM HW init
};
// Caller serializes these functions, DMA transactions and both GVM exits.
uint32_t prepareGvmHdp(void *,const GvmServices &,HdpBinding &);
uint32_t bindOwnedHdp(OwnedDmaTable &,HdpBinding &);
uint32_t invalidateHdpForCpu(void *);
void retireGvmHdp(OwnedDmaTable &,HdpBinding &);
} }
