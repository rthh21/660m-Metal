#include <mach/mach_types.h>
extern kern_return_t Rembrandt660M_kern_start(kmod_info_t *, void *);
extern kern_return_t Rembrandt660M_kern_stop(kmod_info_t *, void *);
__attribute__((visibility("default"))) KMOD_EXPLICIT_DECL(org.rembrandt660m.restore, "0.1.0", Rembrandt660M_kern_start, Rembrandt660M_kern_stop)
__private_extern__ kmod_start_func_t *_realmain = 0;
__private_extern__ kmod_stop_func_t *_antimain = 0;
__private_extern__ int _kext_apple_cc = __APPLE_CC__;
