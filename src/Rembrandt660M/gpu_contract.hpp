// Native25G229 packet prefixes and arithmetic, independent of physical addresses.
#pragma once
#include <stdint.h>
#include <stddef.h>
namespace rem660 { namespace gpu {
struct KiqInput { uint32_t flags,kind;uint64_t ring,mqd,writeback,address1,address2,bytes; };
struct KiqOutput { uint32_t spec[3],reserved;uint64_t published; };
static_assert(sizeof(KiqInput)==0x38&&offsetof(KiqInput,address1)==0x20&&sizeof(KiqOutput)==0x18,
              "25G229 startKIQ service8 packet");
KiqInput makeKiqInput(uint32_t flags,uint64_t ring,uint64_t mqd,uint64_t writeback,uint64_t a1,uint64_t a2);
bool translateUma(uint64_t mcBase,uint64_t physicalBase,uint64_t segmentSize,uint32_t segments,
                  uint64_t address,uint64_t &output);
bool indexedAddress(uint64_t base,uint64_t span,uint64_t index,uint64_t stride,uint64_t width,uint64_t &output);
bool queueParameters(uint32_t me,uint32_t pipe,uint32_t queue,uint32_t vmid,uint32_t doorbell,
                     uint32_t meCount,uint32_t pipeCount,uint32_t queueCount,uint32_t vmidCount,
                     uint64_t doorbellDwords);
} }
