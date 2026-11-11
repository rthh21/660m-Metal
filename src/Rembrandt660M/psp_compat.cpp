#include "psp_compat.hpp"
namespace rem660 { namespace psp {
template<class T> static T load(const uint8_t *p,size_t at) {
    T value;__builtin_memcpy(&value,p+at,sizeof(value));return value;
}
template<class T> static void store(uint8_t *p,size_t at,T value) {
    __builtin_memcpy(p+at,&value,sizeof(value));
}
bool validDmaRange(uint64_t address,uint64_t bytes,uint64_t alignment) {
    return address&&bytes&&alignment&&!(alignment&(alignment-1))&&
           !(address&(alignment-1))&&bytes<=UINT64_MAX-address;
}
static uint32_t finish(TmrReport *report,TmrStage stage,uint32_t result) {
    if(report) report->stage=stage;
    return result;
}
uint32_t initWithYellowCarpToc(void *nativeState,const uint8_t *toc,size_t bytes,TmrInit original,
                               TmrReport *report) {
    if(report) *report=TmrReport{};
    if(!nativeState||!toc||!bytes||bytes>UINT32_MAX||!original) return finish(report,TmrStage::BadArguments,1);
    auto state=static_cast<uint8_t *>(nativeState);
    // _psp_allocate_private_mem 0x4887c -> _psp_cgs_alloc_memory 0x4bef4 (type 1, visible):
    // size +0xb28, GPU address +0xb38 (cgs out+0, used as the LOAD_TOC address at 0x4d6e1),
    // CPU mapping +0xb48 (cgs out+8, memcpy destination at 0x4d6b7) and handle +0xb50 (cgs out+32,
    // gmm out+24 = tracking object, set on the visible path at 0xaa743). No host->GPU cast.
    uint64_t stagingBytes=load<uint64_t>(state,0xb28);
    uint64_t stagingGpu=load<uint64_t>(state,0xb38);
    auto stagingCpu=load<uintptr_t>(state,0xb48),stagingHandle=load<uintptr_t>(state,0xb50);
    auto oldBytes=load<uint32_t>(state,0x3580);
    if(report) {
        report->stagingBytes=stagingBytes;report->stagingGpu=stagingGpu;
        report->stagingCpu=stagingCpu;report->stagingHandle=stagingHandle;
        report->tocBytesBefore=oldBytes;report->tocBytesSelected=static_cast<uint32_t>(bytes);
    }
    if(bytes>stagingBytes||!stagingCpu||!stagingHandle||!validDmaRange(stagingGpu,stagingBytes,0x100000))
        return finish(report,TmrStage::StagingRejected,2);
    auto oldImage=load<const uint8_t *>(state,0x3588);
    const auto selectedBytes=static_cast<uint32_t>(bytes);
    store(state,0x3580,selectedBytes);store(state,0x3588,toc);
    const auto result=original(nativeState);
    // _psp_tmr_init 0x4d72b takes the TMR size from the native LOAD_TOC reply; the allocation at
    // 0x4d772 writes GPU address +0xb08 (cgs out+0), CPU mapping +0xb18 (cgs out+8) and handle
    // +0xb20 (cgs out+32).
    uint64_t tmrBytes=load<uint64_t>(state,0xaf8),tmrGpu=load<uint64_t>(state,0xb08);
    if(report) {
        report->nativeStatus=result;report->tmrBytes=tmrBytes;report->tmrGpu=tmrGpu;
        report->tmrCpu=load<uintptr_t>(state,0xb18);report->tmrHandle=load<uintptr_t>(state,0xb20);
    }
    if(result) {
        // Restore only our still-owned binding; do not overwrite a native
        // replacement or release any allocation belonging to Apple.
        if(load<uint32_t>(state,0x3580)==selectedBytes&&load<const uint8_t *>(state,0x3588)==toc) {
            store(state,0x3580,oldBytes);store(state,0x3588,oldImage);
        }
        return finish(report,TmrStage::NativeFailed,result); // no TMR destroy/retry or forced acceptance
    }
    // The TMR is allocated with type 2 first (0x4d68d), i.e. GMM kind 1 = non-CPU-visible VRAM.
    // During TTL init AmdTtlServices::gmmCbAllocateLocalGpuMemoryInit serves it from the reserved
    // non-visible range (0xaa77d..0xaa7eb): it returns only the GPU address (out+16) and leaves the
    // CPU mapping and the tracking handle (out+0/+8/+24) at zero, so +0xb18/+0xb20 are legitimately
    // null here. Only the PSP contract is checked: non-zero 32-bit size, 1 MiB aligned GPU address
    // (PSP_TMR_ALIGNMENT, the alignment Apple passes at 0x4d683).
    if(!tmrBytes||tmrBytes>UINT32_MAX||!validDmaRange(tmrGpu,tmrBytes,0x100000))
        return finish(report,TmrStage::TmrRejected,2);
    return finish(report,TmrStage::Ok,result); // native success only, with validated PSP resource bounds
}
} }
