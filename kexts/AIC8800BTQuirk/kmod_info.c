#include <mach/kmod.h>

extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);

extern kern_return_t AIC8800BTQuirk_kern_start(kmod_info_t *ki, void *data);
extern kern_return_t AIC8800BTQuirk_kern_stop(kmod_info_t *ki, void *data);

__private_extern__ kmod_start_func_t *_realmain = AIC8800BTQuirk_kern_start;
__private_extern__ kmod_stop_func_t *_antimain = AIC8800BTQuirk_kern_stop;

KMOD_EXPLICIT_DECL(com.rush.driver.AIC8800BTQuirk, "0.1.0", _start, _stop)

