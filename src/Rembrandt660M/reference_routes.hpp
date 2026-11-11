// Original Rembrandt660M code. Alternative routing of exact 25G229 references.
// No stolen instructions or prologue relocation. Native entries remain callable.
#pragma once
#include "tahoe_hooks.hpp"
namespace rem660 { namespace abi25 {
struct ReferenceSite {
    const char *symbol;
    uintptr_t offset;
    uint8_t length,displacement;
    uint8_t bytes[32];
};
#include "tahoe_reference_sites.hpp"
struct ReferencePatch { uintptr_t address; uint8_t before[4],after[4]; };
struct ReferencePlan {
    uintptr_t original=0;
    size_t count=0;
    ReferencePatch patches[2] {};
};
// Checked unsigned arithmetic also handles canonical kernel addresses without
// signed overflow or truncating an out-of-range destination into rel32.
inline bool relative32(uintptr_t next,uintptr_t destination,uint32_t &out) {
    if(destination>=next) {
        uintptr_t delta=destination-next;
        if(delta>0x7fffffffU) return false;
        out=static_cast<uint32_t>(delta);
    } else {
        uintptr_t delta=next-destination;
        if(delta>0x80000000ULL) return false;
        out=0U-static_cast<uint32_t>(delta);
    }
    return true;
}
inline bool prepareReferenceRoute(const void *image,size_t size,const char *symbol,
                                  uintptr_t replacement,ReferencePlan &out) {
    ReferencePlan plan{};
    auto hook=findWindow(symbol);
    uintptr_t base=reinterpret_cast<uintptr_t>(image);
    if(!hook||hook->image!=Image::HwLibs||!replacement||hook->vmaddr>UINTPTR_MAX-base||
       !verifiedTextBytes(image,size,hook->image,hook->vmaddr,hook->bytes,32)) return false;
    plan.original=base+hook->vmaddr;
    if(replacement==plan.original) return false;
    for(const auto &site:ReferenceSites) {
        if(!sameName(site.symbol,symbol)) continue;
        if(plan.count==2||site.offset>UINTPTR_MAX-base||
           site.length>UINTPTR_MAX-(base+site.offset)||
           !verifiedTextBytes(image,size,hook->image,site.offset,site.bytes,32)) return false;
        bool lea=site.length==7&&site.displacement==3&&site.bytes[0]==0x48&&
            site.bytes[1]==0x8d&&site.bytes[2]==0x05;
        bool branch=site.length==5&&site.displacement==1&&
            (site.bytes[0]==0xe8||site.bytes[0]==0xe9);
        if(!lea&&!branch) return false;
        // Verify the pinned displacement resolves to precisely the audited entry.
        int64_t native=int64_t(site.offset)+site.length+
            static_cast<int32_t>(read32(site.bytes+site.displacement));
        if(native<0||uint64_t(native)!=hook->vmaddr) return false;
        uint32_t encoded=0;
        if(!relative32(base+site.offset+site.length,replacement,encoded)) return false;
        auto &patch=plan.patches[plan.count++];
        patch.address=base+site.offset+site.displacement;
        for(unsigned i=0;i<4;++i) {
            patch.before[i]=site.bytes[site.displacement+i];
            patch.after[i]=uint8_t(encoded>>(8*i));
        }
    }
    if(!plan.count) return false;
    out=plan; // failed planning never publishes a partial result
    return true;
}
// Caller must establish that the containing kext cannot execute concurrently.
// kernelWriteLock alone does not provide cross-CPU instruction quiescence.
// No API in this header invents that precondition from a callback notification.
} }

namespace rem660 { namespace abi25 {
// Used only before execution begins. All compares precede publication/writes;
// the caller keeps image and plugin alive and holds the installation lock.
// Failure after a write is terminal; this is not cross-CPU text patching.
template<class Publish,class Write>
bool applyReferencePlan(const ReferencePlan &plan,bool executionQuiescent,Publish publish,Write write) {
    if(!executionQuiescent||!plan.original||!plan.count||plan.count>2) return false;
    for(size_t i=0;i<plan.count;++i)
        if(!plan.patches[i].address||!equalBytes(reinterpret_cast<const uint8_t *>(plan.patches[i].address),
                                               plan.patches[i].before,4)) return false;
    if(!publish(plan.original)) return false;
    for(size_t i=0;i<plan.count;++i) if(!write(plan.patches[i])) return false;
    return true;
}
} }
