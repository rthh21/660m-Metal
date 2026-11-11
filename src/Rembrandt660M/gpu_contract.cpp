#include "gpu_contract.hpp"
namespace rem660 { namespace gpu {
KiqInput makeKiqInput(uint32_t flags,uint64_t ring,uint64_t mqd,uint64_t writeback,uint64_t a1,uint64_t a2) {
    return {flags&~3u,0x80,ring,mqd,writeback,a1,a2,0x200}; // exact native constants
}
bool translateUma(uint64_t mcBase,uint64_t physicalBase,uint64_t segmentSize,uint32_t segments,
                  uint64_t address,uint64_t &output) {
    if(!segmentSize||!segments||segmentSize>UINT64_MAX/segments) return false;
    uint64_t span=segmentSize*segments;
    if(span>UINT64_MAX-mcBase) return false;
    if(address<mcBase||address-mcBase>=span) {output=address;return true;}
    uint64_t offset=address-mcBase;
    if(offset>UINT64_MAX-physicalBase) return false;
    output=physicalBase+offset;return true;
}
bool indexedAddress(uint64_t base,uint64_t span,uint64_t index,uint64_t stride,uint64_t width,uint64_t &output) {
    if(!span||!stride||!width||span>UINT64_MAX-base||index>UINT64_MAX/stride) return false;
    uint64_t offset=index*stride;
    if(offset>=span||width>span-offset) return false;
    output=base+offset;return true;
}
bool queueParameters(uint32_t me,uint32_t pipe,uint32_t queue,uint32_t vmid,uint32_t doorbell,
                     uint32_t meCount,uint32_t pipeCount,uint32_t queueCount,uint32_t vmidCount,
                     uint64_t doorbellDwords) {
    // Counts must come from the owner/hardware profile, never a 680M fuse mask.
    return meCount&&pipeCount&&queueCount&&vmidCount&&doorbellDwords&&
           me<meCount&&pipe<pipeCount&&queue<queueCount&&vmid<vmidCount&&doorbell<doorbellDwords;
}
} }
