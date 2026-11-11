// Reuse only the prior fork's proven UUID/EXEC/prologue guard. No feature policy,
// PSP13 backend, synthetic SWIP or new lifecycle framework is imported.
#include "rmb.hpp"
#include "tahoe_hooks.hpp"
#include "route_group.hpp"
#include "reference_routes.hpp"
#include "tahoe_symbols_25G229.hpp"

namespace { rem660::RouteLedger gRemRouteLedger; }
bool restoredRoutingFailed() { return false; }
void restoredRoutingAbort() { }

mach_vm_address_t findSymbol(KernelPatcher &patcher, size_t groupIndex, const char *symbol, mach_vm_address_t address, size_t size) {
    (void)patcher;
    (void)size;
    if (!symbol) return 0;
    if (!address) {
        address = getKextBase(groupIndex);
    }
    if (address) {
        mach_vm_address_t target = rem660::tahoe25G229::resolveSymbol(groupIndex, symbol, address);
        if (target) return target;
    }
    IOLog("REM660: [findSymbol] MISSING static RVA for group=%lu sym=%s (addr=0x%llx)\n",
          (unsigned long)groupIndex, symbol, static_cast<uint64_t>(address));
    return 0;
}

bool routeStaticRequests(KernelPatcher &patcher, size_t groupIndex, KernelPatcher::RouteRequest *reqs, size_t count, mach_vm_address_t address, size_t size) {
    if (!reqs || !count) return false;
    bool allOk = true;
    for (size_t i = 0; i < count; ++i) {
        if (!reqs[i].symbol && !reqs[i].from) continue; // Already installed or empty
        if (reqs[i].org && *reqs[i].org) continue; // Already hooked!

        mach_vm_address_t target = reqs[i].from ? reqs[i].from : findSymbol(patcher, groupIndex, reqs[i].symbol, address, size);
        if (target != 0 && reqs[i].to != 0) {
            reqs[i].from = target;
            reqs[i].symbol = nullptr;
            patcher.clearError();
            bool ok = patcher.routeMultiple(groupIndex, &reqs[i], 1, address, size, true, false);
            if (!ok) {
                patcher.clearError();
                ok = patcher.routeMultiple(groupIndex, &reqs[i], 1, 0, 0, true, false);
            }
            if (!ok) {
                patcher.clearError();
                mach_vm_address_t org = patcher.routeFunction(target, reqs[i].to, reqs[i].org != nullptr);
                if (org) {
                    if (reqs[i].org) *reqs[i].org = org;
                    ok = true;
                }
                patcher.clearError();
            }
            if (ok) {
                if (!reqs[i].org) reqs[i].from = 0;
            } else {
                allOk = false;
            }
        } else {
            allOk = false;
        }
    }
    patcher.clearError();
    return allOk;
}

bool restoredRouteGroup(KernelPatcher &patcher,size_t index,KernelPatcher::RouteRequest *requests,
                        size_t count,mach_vm_address_t address,size_t size) {
    patcher.clearError();
    if(!requests||!count||count>32) return false;
    bool allOk = true;
    for(size_t i=0;i<count;++i) {
        auto &r=requests[i];
        if(!r.symbol && !r.from) continue;
        if(r.org && *r.org) continue;
        const char *sym = r.symbol;
        auto target = r.from ? r.from : findSymbol(patcher, index, sym, address, size);
        if(!target || !r.to) {
            allOk = false;
            continue;
        }
        rem660::abi25::ReferencePlan alt{};
        if(sym && r.org && !rem660::abi25::verifiedEntry(reinterpret_cast<const void *>(address),size,target,sym) &&
           rem660::abi25::prepareReferenceRoute(reinterpret_cast<const void *>(address),size,sym,r.to,alt) &&
           alt.original==target && alt.count > 0) {
            if(MachInfo::setKernelWriting(true,KernelPatcher::kernelWriteLock)==KERN_SUCCESS) {
                bool applied=rem660::abi25::applyReferencePlan(alt,true,[&](uintptr_t original) {
                    __atomic_store_n(r.org,original,__ATOMIC_RELEASE);return true;
                },[](const rem660::abi25::ReferencePatch &patch) {
                    memcpy(reinterpret_cast<void *>(patch.address),patch.after,4);return true;
                });
                MachInfo::setKernelWriting(false,KernelPatcher::kernelWriteLock);
                if(applied) continue;
            }
        }
        r.from = target;
        r.symbol = nullptr;
        patcher.clearError();
        bool ok = patcher.routeMultiple(index, &r, 1, address, size, true, false);
        if (!ok) {
            patcher.clearError();
            ok = patcher.routeMultiple(index, &r, 1, 0, 0, true, false);
        }
        if (!ok && target != 0 && r.to != 0) {
            patcher.clearError();
            mach_vm_address_t org = patcher.routeFunction(target, r.to, r.org != nullptr);
            if (org) {
                if (r.org) *r.org = org;
                ok = true;
            }
            patcher.clearError();
        }
        if (ok) {
            if (!r.org) r.from = 0;
        } else {
            allOk = false;
        }
    }
    return allOk;
}
