#ifndef AIC8800_BT_QUIRK_HPP
#define AIC8800_BT_QUIRK_HPP

#include <Headers/kern_patcher.hpp>

class AIC8800BTQuirkModule {
public:
    bool init();
    void deinit();

private:
    void processKext(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size);
};

#endif /* AIC8800_BT_QUIRK_HPP */

