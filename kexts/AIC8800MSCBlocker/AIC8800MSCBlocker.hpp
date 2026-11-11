#ifndef AIC8800_MSC_BLOCKER_HPP
#define AIC8800_MSC_BLOCKER_HPP

#include <IOKit/IOService.h>
#include <IOKit/usb/IOUSBHostInterface.h>

/**
 * Passive high-score Stage 0 USB Mass Storage blocker for AIC8800 (0xA69C:0x5721..0x5732).
 *
 * Outscores IOUSBMassStorageInterfaceNub (IOProbeScore=100000) and attaches without
 * calling provider->open(this) so user-space aic8800_fwload can open the USB
 * interface and issue the BOT SCSI eject CBW cleanly without kIOReturnExclusiveAccess.
 */
class AIC8800MSCBlocker : public IOService {
    OSDeclareDefaultStructors(AIC8800MSCBlocker)

    using super = IOService;

public:
    IOService *probe(IOService *provider, SInt32 *score) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;

private:
    static bool isDisabledByBootArg();
};

#endif /* AIC8800_MSC_BLOCKER_HPP */

