#include <mach/kmod.h>

extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);

__private_extern__ kmod_start_func_t *_realmain = 0;
__private_extern__ kmod_stop_func_t *_antimain = 0;

KMOD_EXPLICIT_DECL(com.rush.driver.AIC8800MSCBlocker, "0.1.0", _start, _stop)

