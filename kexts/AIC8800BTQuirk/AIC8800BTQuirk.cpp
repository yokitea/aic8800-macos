#include "AIC8800BTQuirk.hpp"

#include <Headers/kern_api.hpp>
#include <Headers/kern_util.hpp>
#include <IOKit/IOLocks.h>
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

struct ZlpSlot {
    IOUSBHostPipe *pipe;
    IOUSBHostCompletion origCompletion;
    bool inUse;
};

static constexpr size_t kZlpPoolSize = 64;
static ZlpSlot gZlpPool[kZlpPoolSize] = {};
static IOSimpleLock *gZlpLock = nullptr;

static void zlpAsyncCallback(void *owner, void *parameter, IOReturn status, uint32_t bytesTransferred) {
    (void)owner;
    auto *slot = static_cast<ZlpSlot *>(parameter);
    if (!slot) {
        return;
    }

    IOUSBHostPipe *pipe = slot->pipe;
    IOUSBHostCompletion orig = slot->origCompletion;

    if (gZlpLock) {
        IOSimpleLockLock(gZlpLock);
        slot->inUse = false;
        slot->pipe = nullptr;
        IOSimpleLockUnlock(gZlpLock);
    }

    if (status == kIOReturnSuccess && pipe && AIC8800BTQuirkModule::orgAsyncIO != 0) {
        /* Chained ZLP transfer with original client completion action */
        using AsyncIoFunc = IOReturn (*)(IOUSBHostPipe *, IOMemoryDescriptor *, uint32_t, IOUSBHostCompletion *, uint32_t);
        auto fn = reinterpret_cast<AsyncIoFunc>(AIC8800BTQuirkModule::orgAsyncIO);
        if (fn) {
            (void)fn(pipe, nullptr, 0, (orig.action ? &orig : nullptr), 0);
            return;
        }
    }

    if (orig.action) {
        orig.action(orig.owner, orig.parameter, status, bytesTransferred);
    }
}

}  // namespace

mach_vm_address_t AIC8800BTQuirkModule::orgStart = 0;
mach_vm_address_t AIC8800BTQuirkModule::orgInternalDeviceRequest = 0;
mach_vm_address_t AIC8800BTQuirkModule::orgSyncIO = 0;
mach_vm_address_t AIC8800BTQuirkModule::orgAsyncIO = 0;

bool AIC8800BTQuirkModule::init() {
    uint32_t offFlag = 0;
    if (PE_parse_boot_argn("aic_off", &offFlag, sizeof(offFlag)) && offFlag != 0) {
        SYSLOG("aicbt", "disabled by aic_off=1 boot-arg");
        return false;
    }
    if (PE_parse_boot_argn("aic_btoff", &offFlag, sizeof(offFlag)) && offFlag != 0) {
        SYSLOG("aicbt", "disabled by aic_btoff=1 boot-arg");
        return false;
    }

    if (!gZlpLock) {
        gZlpLock = IOSimpleLockAlloc();
    }

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

    SYSLOG("aicbt", "AIC8800BTQuirk initialized (CSR spoof 0x%04x, ZLP threshold %uB)",
           AIC_USB_VID_CSR_SPOOF, AIC_USB_BT_ACL_ZLP_CHUNK);
    return true;
}

void AIC8800BTQuirkModule::deinit() {
    if (gZlpLock) {
        IOSimpleLockFree(gZlpLock);
        gZlpLock = nullptr;
    }
}

bool AIC8800BTQuirkModule::hookedStart(IOUSBHostDevice *that, IOService *provider) {
    using StartFunc = bool (*)(IOUSBHostDevice *, IOService *);
    auto fn = reinterpret_cast<StartFunc>(orgStart);
    if (!fn) {
        return false;
    }

    bool ok = fn(that, provider);
    if (!ok || !that) {
        return ok;
    }

    const StandardUSB::DeviceDescriptor *desc = that->getDeviceDescriptor();
    uint16_t pid = 0;
    uint16_t vid = 0;
    if (desc) {
        pid = USBToHost16(desc->idProduct);
        vid = USBToHost16(desc->idVendor);
    } else {
        OSNumber *nP = OSDynamicCast(OSNumber, that->getProperty("idProduct"));
        OSNumber *nV = OSDynamicCast(OSNumber, that->getProperty("idVendor"));
        if (nP) pid = nP->unsigned16BitValue();
        if (nV) vid = nV->unsigned16BitValue();
    }

    if (pid == AIC_USB_PID_WIFI_BT_RUNTIME) {
        that->setProperty("idVendor", static_cast<unsigned long long>(AIC_USB_VID_CSR_SPOOF), 32);
        that->setProperty("USB Vendor Name", "CSR");
        that->setProperty("AIC-BT-Quirk-Active", kOSBooleanTrue);
        SYSLOG("aicbt", "AIC8800 Stage 2 USB device (0x%04x:0x%04x) attached; set idVendor=0x%04x (CSR) for bluetoothd",
               vid, pid, AIC_USB_VID_CSR_SPOOF);
    }

    return ok;
}

IOReturn AIC8800BTQuirkModule::hookedInternalDeviceRequest(IOUSBHostDevice *that,
                                                           IOService *forClient,
                                                           StandardUSB::DeviceRequest &request,
                                                           void *rawBuffer,
                                                           IOMemoryDescriptor *descriptorBuffer,
                                                           uint32_t &bytesTransferred,
                                                           IOUSBHostCompletion *completion,
                                                           uint32_t completionTimeoutMs) {
    if (that) {
        bool isAIC = (that->getProperty("AIC-BT-Quirk-Active") != nullptr);
        if (!isAIC) {
            const StandardUSB::DeviceDescriptor *desc = that->getDeviceDescriptor();
            if (desc && USBToHost16(desc->idProduct) == AIC_USB_PID_WIFI_BT_RUNTIME) {
                isAIC = true;
            }
        }

        if (isAIC) {
            /* Quirk: bmRequestType = 0x21 (Class, Interface) stalls EP0 on AIC8800.
             * Rewrite to 0x20 (Class, Device) if targeting control endpoint (wIndex == 0). */
            if (request.bmRequestType == 0x21 && request.wIndex == 0) {
                request.bmRequestType = 0x20;
            }
        }
    }

    using ReqFunc = IOReturn (*)(IOUSBHostDevice *, IOService *, StandardUSB::DeviceRequest &,
                                void *, IOMemoryDescriptor *, uint32_t &, IOUSBHostCompletion *, uint32_t);
    auto fn = reinterpret_cast<ReqFunc>(orgInternalDeviceRequest);
    if (!fn) {
        return kIOReturnNotReady;
    }

    return fn(that, forClient, request, rawBuffer, descriptorBuffer, bytesTransferred, completion, completionTimeoutMs);
}

IOReturn AIC8800BTQuirkModule::hookedSyncIO(IOUSBHostPipe *that,
                                            IOMemoryDescriptor *dataBuffer,
                                            uint32_t dataBufferLength,
                                            uint32_t &bytesTransferred,
                                            uint32_t completionTimeoutMs) {
    using SyncIoFunc = IOReturn (*)(IOUSBHostPipe *, IOMemoryDescriptor *, uint32_t, uint32_t &, uint32_t);
    auto fn = reinterpret_cast<SyncIoFunc>(orgSyncIO);
    if (!fn) {
        return kIOReturnNotReady;
    }

    IOReturn ret = fn(that, dataBuffer, dataBufferLength, bytesTransferred, completionTimeoutMs);
    if (ret == kIOReturnSuccess && dataBufferLength > 0 && (dataBufferLength % AIC_USB_BT_ACL_ZLP_CHUNK) == 0) {
        const StandardUSB::EndpointDescriptor *ep = (that ? that->getEndpointDescriptor() : nullptr);
        if (ep && ep->bEndpointAddress == AIC_USB_BT_EP_ACL_OUT) {
            uint32_t zlpBytes = 0;
            (void)fn(that, nullptr, 0, zlpBytes, completionTimeoutMs);
        }
    }
    return ret;
}

IOReturn AIC8800BTQuirkModule::hookedAsyncIO(IOUSBHostPipe *that,
                                             IOMemoryDescriptor *dataBuffer,
                                             uint32_t dataBufferLength,
                                             IOUSBHostCompletion *completion,
                                             uint32_t completionTimeoutMs) {
    using AsyncIoFunc = IOReturn (*)(IOUSBHostPipe *, IOMemoryDescriptor *, uint32_t, IOUSBHostCompletion *, uint32_t);
    auto fn = reinterpret_cast<AsyncIoFunc>(orgAsyncIO);
    if (!fn) {
        return kIOReturnNotReady;
    }

    if (dataBufferLength > 0 && (dataBufferLength % AIC_USB_BT_ACL_ZLP_CHUNK) == 0) {
        const StandardUSB::EndpointDescriptor *ep = (that ? that->getEndpointDescriptor() : nullptr);
        if (ep && ep->bEndpointAddress == AIC_USB_BT_EP_ACL_OUT) {
            ZlpSlot *slot = nullptr;
            if (gZlpLock) {
                IOSimpleLockLock(gZlpLock);
                for (size_t i = 0; i < kZlpPoolSize; i++) {
                    if (!gZlpPool[i].inUse) {
                        gZlpPool[i].inUse = true;
                        gZlpPool[i].pipe = that;
                        if (completion) {
                            gZlpPool[i].origCompletion = *completion;
                        } else {
                            gZlpPool[i].origCompletion = {};
                        }
                        slot = &gZlpPool[i];
                        break;
                    }
                }
                IOSimpleLockUnlock(gZlpLock);
            }

            if (slot) {
                IOUSBHostCompletion wrapperComp;
                wrapperComp.owner = nullptr;
                wrapperComp.action = zlpAsyncCallback;
                wrapperComp.parameter = slot;
                return fn(that, dataBuffer, dataBufferLength, &wrapperComp, completionTimeoutMs);
            }
        }
    }

    return fn(that, dataBuffer, dataBufferLength, completion, completionTimeoutMs);
}

void AIC8800BTQuirkModule::processKext(KernelPatcher &patcher,
                                       size_t index,
                                       mach_vm_address_t address,
                                       size_t size) {
    if (index != kWatchedKexts[0].loadIndex) {
        return;
    }

    KernelPatcher::RouteRequest requests[] = {
        {"__ZN15IOUSBHostDevice5startEP9IOService", hookedStart, orgStart},
        {"__ZN15IOUSBHostDevice21internalDeviceRequestEP9IOServiceRN11StandardUSB13DeviceRequestEPvP18IOMemoryDescriptorRjP19IOUSBHostCompletionj", hookedInternalDeviceRequest, orgInternalDeviceRequest},
        {"__ZN13IOUSBHostPipe2ioEP18IOMemoryDescriptorjRjj", hookedSyncIO, orgSyncIO},
        {"__ZN13IOUSBHostPipe2ioEP18IOMemoryDescriptorjP19IOUSBHostCompletionj", hookedAsyncIO, orgAsyncIO},
    };

    if (patcher.routeMultiple(index, requests, address, size)) {
        SYSLOG("aicbt", "successfully patched IOUSBHostFamily (start + internalDeviceRequest + sync/async io)");
    } else {
        SYSLOG("aicbt", "warning: routeMultiple on IOUSBHostFamily encountered errors (code=%d)",
               patcher.getError());
        patcher.clearError();
    }
}
