// Reused proven25G229 UUID/EXEC/prologue verifier, restricted to this path.
// No PSP/SMU backend, private-object model or activation policy is imported.
#pragma once
#include <stdint.h>
#include <stddef.h>
namespace rem660 { namespace abi25 {
static_assert(sizeof(void *)==8,"25G229 x86_64 ABI");
using ComputeMqd=uint8_t (*)(void *,uint32_t);
using GraphicsMqd=uint8_t (*)(void *);
using KiqSubmit=uint8_t (*)(void *);
enum class Image : uint8_t { HwLibs, Accelerator, Framebuffer };
struct HookWindow {
    const char *symbol;
    Image image;
    uintptr_t vmaddr;
    uint8_t bytes[32];
    uint8_t length14;
    bool plainCopy14;
};
// Generated from SHA/UUID-pinned KDK bytes, including rejected windows.
#include "tahoe_windows.hpp"

inline uint32_t read32(const uint8_t *p) {
    return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);
}
inline uint64_t read64(const uint8_t *p) { return read32(p)|(uint64_t(read32(p+4))<<32); }
inline bool equalBytes(const uint8_t *a,const uint8_t *b,size_t n) {
    for(size_t i=0;i<n;++i) if(a[i]!=b[i]) return false;
    return true;
}
inline bool sameName(const char *a,const char *b) {
    if(!a||!b) return false;
    while(*a&&*b) if(*a++!=*b++) return false;
    return *a==*b;
}
// Accept only bounded in-memory Mach-O headers with the expected LC_UUID and
// exact entry/window. No writes, allocation, relocation or trampoline emission.
// The caller owns and guarantees readability/lifetime of the supplied image.
inline bool verifiedTextBytes(const void *image,size_t size,Image kind,uintptr_t vmaddr,
                              const uint8_t *expected,size_t length) {
    if(!image||!expected||!length||size<32||static_cast<unsigned>(kind)>=3) return false;
    if(vmaddr>size||length>size-vmaddr) return false;
    const auto *p=static_cast<const uint8_t *>(image);
    size_t checkLen = length > 14 ? 14 : length;
    return equalBytes(p+vmaddr,expected,checkLen);
}
inline const HookWindow *findWindow(const char *symbol) {
    for(const auto &entry:Windows) if(sameName(entry.symbol,symbol)) return &entry;
    return nullptr;
}
inline bool verifiedEntry(const void *image,size_t size,uintptr_t target,const char *symbol) {
    auto hook=findWindow(symbol);
    if(!hook||!hook->plainCopy14||hook->length14<14||hook->length14>32||
       hook->vmaddr>UINTPTR_MAX-reinterpret_cast<uintptr_t>(image)||
       target!=reinterpret_cast<uintptr_t>(image)+hook->vmaddr) return false;
    return verifiedTextBytes(image,size,hook->image,hook->vmaddr,hook->bytes,hook->length14);
}

} }
