// Independently implemented bookkeeping. Does not certify a hook ABI/trampoline.
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace rem660 {
enum class RouteState : uint8_t { Empty, Preflight, Installing, Complete, Failed, Quarantined };
struct RouteClaim { uintptr_t target, originalSlot; };
class RouteLedger {
    static constexpr size_t Capacity=256;
    RouteClaim claims_[Capacity] {};
    size_t used_=0;
    uint8_t locked_=0, failed_=0;
    RouteState state_=RouteState::Empty;
public:
    RouteLedger()=default;
    RouteLedger(const RouteLedger &)=delete;
    RouteLedger &operator=(const RouteLedger &)=delete;
    bool failed() const { return __atomic_load_n(&failed_,__ATOMIC_ACQUIRE)!=0; }
    void abortSetup() { __atomic_store_n(&failed_,1,__ATOMIC_RELEASE); }
    // Caller supplies one whole group. Resolve has no text mutation. Install
    // may have changed code even when it reports failure; no rollback is assumed.
    // Original slots are addresses of trampoline storage, never trampoline values.
    template<class Resolve, class Install>
    bool run(size_t count, Resolve resolve, Install install) {
        if (__atomic_exchange_n(&locked_,1,__ATOMIC_ACQUIRE)) {
            // Reentrant/concurrent setup cannot be silently skipped while a
            // caller believes an earlier route family completed.
            __atomic_store_n(&failed_,1,__ATOMIC_RELEASE); return false;
        }
        struct Unlock { uint8_t *p; ~Unlock(){__atomic_store_n(p,0,__ATOMIC_RELEASE);} } unlock{&locked_};
        if (failed()) return false;
        state_=RouteState::Preflight;
        if (!count || count>Capacity-used_) return fail(false);
        for (size_t i=0;i<count;++i) {
            RouteClaim c{};
            if (!resolve(i,c) || !c.target) return fail(false);
            for (size_t j=0;j<used_+i;++j)
                if (claims_[j].target==c.target ||
                    (c.originalSlot && claims_[j].originalSlot==c.originalSlot)) return fail(false);
            claims_[used_+i]=c;
        }
        // Reserve the complete group BEFORE the first write, including targets
        // not reached if a later installation fails. No retry/reset API exists.
        used_+=count; state_=RouteState::Installing;
        for (size_t i=0;i<count;++i)
            if (failed() || !install(i)) return fail(true);
        if (failed()) return fail(true);
        state_=RouteState::Complete; return true;
    }
private:
    bool fail(bool mayHaveMutated) {
        state_=mayHaveMutated ? RouteState::Quarantined : RouteState::Failed;
        __atomic_store_n(&failed_,1,__ATOMIC_RELEASE); return false;
    }
};
}
