// Original 680M dispatch, narrowed to the captured Lenovo revision.
// This selects Apple software tables; it does not change physical IP/fuse data.
#pragma once
#include <stdint.h>
#include <stddef.h>
namespace rem660 {
using PspAsicInit=uint32_t (*)(void *,void *);
using GcVersion=uint32_t (*)(const void *);
using Capability=uint8_t (*)(void *,uint32_t,uint32_t,uint32_t);
inline uint32_t word(const void *p,size_t at) {
    uint32_t value;__builtin_memcpy(&value,static_cast<const uint8_t *>(p)+at,4);return value;
}
__attribute__((always_inline)) inline uint32_t selectApplePsp11(PspAsicInit original,void *psp,void *info) {
    if(!original||!psp||!info) return 1;
    // _psp_asic_type_init25G229 copies exactly0x260 bytes. Only the known
    // 13.0.3 presentation is adapted; no assumed support for other PSP13 IPs.
    if(word(info,8)!=13||word(info,12)!=0||word(info,16)!=3) return original(psp,info);
    alignas(8) uint8_t copy[0x260];__builtin_memcpy(copy,info,sizeof(copy));
    const uint32_t appleVersion[3]{11,0,11};
    __builtin_memcpy(copy+8,appleVersion,sizeof(appleVersion));
    return original(psp,copy); // preserve every native error bit
}
__attribute__((always_inline)) inline uint32_t selectNavi23Gc(GcVersion original,const void *info) {
    if(!original||!info) return 0;
    const auto value=original(info);
    return value==0x0a0303 ? 0x0a0304 : value;
}
__attribute__((always_inline)) inline uint8_t selectNavi23Capability(Capability original,void *context,uint32_t device,
                                     uint32_t internalRevision,uint32_t externalRevision) {
    if(!original) return 0;
    // PCI presentation is scoped separately to the exact Lenovo device and
    // Apple callers. Do not convert arbitrary physical device IDs here.
    if(device==0x73ff) internalRevision=0;
    return original(context,device,internalRevision,externalRevision);
}
}
