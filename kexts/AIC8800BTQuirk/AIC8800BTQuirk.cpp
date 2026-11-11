#include "AIC8800BTQuirk.hpp"

#include <Headers/kern_api.hpp>
#include <Headers/kern_util.hpp>
#include <pexpert/pexpert.h>

#include "aic8800_abi.h"

namespace {
static const char *kIOUSBHostFamilyPath[] = {
    "/System/Library/Extensions/IOUSBHostFamily.kext/Contents/MacOS/IOUSBHostFamily",
};

static KernelPatcher::KextInfo kWatchedKexts[] = {
    {
        "com.apple.iokit.IOUSBHostFamily",
        kIOUSBHostFamilyPath,
        arrsize(kIOUSBHostFamilyPath),
        {},
        {},
        KernelPatcher::KextInfo::Unloaded
    },
};
}  // namespace

bool AIC8800BTQuirkModule::init() {
    uint32_t offFlag = 0;
    if (PE_parse_boot_argn("aic_off", &offFlag, sizeof(offFlag)) && offFlag != 0) {
        SYSLOG("aicbt", "disabled by aic_off=1 boot-arg");
        return false;
    }

    /* Phase 0 skeleton: validate Stage 1 constants and register onKextLoad seam cleanly. */
    lilu.onKextLoadForce(
        kWatchedKexts,
        arrsize(kWatchedKexts),
        [](void *user, KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
            auto *self = static_cast<AIC8800BTQuirkModule *>(user);
            if (self) {
                self->processKext(patcher, index, address, size);
            }
        },
        this);

    SYSLOG("aicbt", "Phase 0 skeleton initialized (target PID 0x%04x:%04x)",
           AIC_USB_VID_AICS_WIFI, AIC_USB_PID_WIFI_BT_RUNTIME);
    return true;
}

void AIC8800BTQuirkModule::deinit() {
}

void AIC8800BTQuirkModule::processKext(KernelPatcher &patcher,
                                       size_t index,
                                       mach_vm_address_t address,
                                       size_t size) {
    (void)patcher;
    (void)address;
    (void)size;
    if (index == kWatchedKexts[0].loadIndex) {
        DBGLOG("aicbt", "observed IOUSBHostFamily load in Phase 0 skeleton");
    }
}
