#ifndef AIC8800_BT_QUIRK_HPP
#define AIC8800_BT_QUIRK_HPP

#include <Headers/kern_patcher.hpp>
#include <IOKit/usb/IOUSBHostDevice.h>
#include <IOKit/usb/IOUSBHostPipe.h>
#include <IOKit/usb/StandardUSB.h>

class AIC8800BTQuirkModule {
public:
    bool init();
    void deinit();

    /* Quirk hook handlers */
    static bool hookedStart(IOUSBHostDevice *that, IOService *provider);

    static IOReturn hookedInternalDeviceRequest(IOUSBHostDevice *that,
                                               IOService *forClient,
                                               StandardUSB::DeviceRequest &request,
                                               void *rawBuffer,
                                               IOMemoryDescriptor *descriptorBuffer,
                                               uint32_t &bytesTransferred,
                                               IOUSBHostCompletion *completion,
                                               uint32_t completionTimeoutMs);

    static IOReturn hookedSyncIO(IOUSBHostPipe *that,
                                IOMemoryDescriptor *dataBuffer,
                                uint32_t dataBufferLength,
                                uint32_t &bytesTransferred,
                                uint32_t completionTimeoutMs);

    static IOReturn hookedAsyncIO(IOUSBHostPipe *that,
                                 IOMemoryDescriptor *dataBuffer,
                                 uint32_t dataBufferLength,
                                 IOUSBHostCompletion *completion,
                                 uint32_t completionTimeoutMs);

    /* Retrospectively scan and spoof existing devices */
    static void spoofExistingDevices();

    /* Trampoline storage */
    static mach_vm_address_t orgStart;
    static mach_vm_address_t orgInternalDeviceRequest;
    static mach_vm_address_t orgSyncIO;
    static mach_vm_address_t orgAsyncIO;

private:
    void processKext(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size);
};

#endif /* AIC8800_BT_QUIRK_HPP */
