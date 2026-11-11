// Guards around the original Apple PSP11-compatible implementation, not a transport/backend.
#pragma once
#include <stdint.h>
#include <stddef.h>
namespace rem660 { namespace psp {
using TmrInit=uint32_t (*)(void *);
// Exact25G229 native LOAD_TOC command prefix: internal opcode12, address+4,size+12.
// The existing Apple command builder converts internal numbering to PSP wire IDs.
struct CommandPrefix { uint32_t operation,addressLow,addressHigh,bytes; };
static_assert(sizeof(CommandPrefix)==16,"Apple PSP command prefix");
// Where initWithYellowCarpToc returned. Every non-zero return is attributable to exactly one stage,
// so the caller can log it (the native PSP failure path 0x49b4c logs event 0x100216 with type 2,
// which AmdTtlServices::amd_log_psp 0xb3621..0xb363d never prints).
enum class TmrStage : uint32_t {
    BadArguments=0,      // no PSP state / image / original
    StagingRejected=1,   // TOC does not fit the private staging buffer, or buffer not usable
    NativeFailed=2,      // Apple's _psp_tmr_init returned non-zero (its own _psp_cos_log explains why)
    TmrRejected=3,       // native success but TMR size/GPU address outside PSP's 1 MiB contract
    Ok=4,
};
struct TmrReport {
    TmrStage stage;
    uint32_t nativeStatus;
    uint32_t tocBytesBefore,tocBytesSelected;
    uint64_t stagingBytes,stagingGpu;
    uintptr_t stagingCpu,stagingHandle;
    uint64_t tmrBytes,tmrGpu;
    uintptr_t tmrCpu,tmrHandle;
};
bool validDmaRange(uint64_t address,uint64_t bytes,uint64_t alignment);
uint32_t initWithYellowCarpToc(void *nativeState,const uint8_t *toc,size_t bytes,TmrInit,TmrReport *report=nullptr);
} }
