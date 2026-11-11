#include "AIC8800MSCBlocker.hpp"

#include <IOKit/IOLib.h>
#include <IOKit/usb/StandardUSB.h>
#include <pexpert/pexpert.h>

OSDefineMetaClassAndStructors(AIC8800MSCBlocker, IOService)

bool AIC8800MSCBlocker::isDisabledByBootArg() {
    uint32_t flag = 0;
    if (PE_parse_boot_argn("aic_off", &flag, sizeof(flag)) && flag != 0) {
        return true;
    }
    if (PE_parse_boot_argn("-aicoff", &flag, sizeof(flag))) {
        return true;
    }
    return false;
}

IOService *AIC8800MSCBlocker::probe(IOService *provider, SInt32 *score) {
    if (isDisabledByBootArg()) {
        return nullptr;
    }

    IOUSBHostInterface *iface = OSDynamicCast(IOUSBHostInterface, provider);
    if (!iface) {
        return nullptr;
    }

    const StandardUSB::InterfaceDescriptor *ifDesc = iface->getInterfaceDescriptor();
    if (!ifDesc || ifDesc->bInterfaceClass != 8U) {
        return nullptr;
    }

    if (score) {
        *score = 100000;
    }
    return this;
}

bool AIC8800MSCBlocker::start(IOService *provider) {
    if (isDisabledByBootArg()) {
        return false;
    }

    IOUSBHostInterface *iface = OSDynamicCast(IOUSBHostInterface, provider);
    if (!iface || !super::start(provider)) {
        return false;
    }

    /* Deliberately do NOT call iface->open(this): keeping IOUSBHostInterface
     * un-opened blocks IOUSBMassStorageInterfaceNub while still allowing
     * user-space aic8800_fwload to open the interface and send the BOT eject.
     */
    setProperty("AIC-MSC-Blocked", true);
    registerService();
    IOLog("AIC8800MSCBlocker: passively claimed Stage 0 MSC interface (class 0x08)\n");
    return true;
}

void AIC8800MSCBlocker::stop(IOService *provider) {
    super::stop(provider);
}

