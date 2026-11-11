#include "AIC8800BTQuirk.hpp"

#include <Headers/kern_api.hpp>
#include <Headers/kern_util.hpp>
#include <IOKit/IOLib.h>
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
        {true}, /* sys[KernelPatcher::KextInfo::Loaded] = true: hook prelinked KC kext */
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

void AIC8800BTQuirkModule::spoofExistingDevices() {
    const char *targetClasses[] = {"IOUSBHostDevice", "AppleUSBDevice"};
    for (size_t c = 0; c < 2; c++) {
        OSDictionary *dict = IOService::serviceMatching(targetClasses[c]);
        if (!dict) continue;

        OSIterator *iter = IOService::getMatchingServices(dict);
        dict->release();
        if (!iter) continue;

        while (auto *entry = OSDynamicCast(IORegistryEntry, iter->getNextObject())) {
            OSNumber *nP = OSDynamicCast(OSNumber, entry->getProperty("idProduct"));
            if (nP && nP->unsigned16BitValue() == AIC_USB_PID_WIFI_BT_RUNTIME) {
                OSNumber *nV = OSDynamicCast(OSNumber, entry->getProperty("idVendor"));
                uint16_t oldVid = nV ? nV->unsigned16BitValue() : 0;
                entry->setProperty("idVendor", static_cast<unsigned long long>(AIC_USB_VID_CSR_SPOOF), 32);
                entry->setProperty("USB Vendor Name", "CSR");
                entry->setProperty("AIC-BT-Quirk-Active", kOSBooleanTrue);
                IOLog("AIC8800BTQuirk: Retrospectively spoofed %s (0x%04x:0x%04x) -> idVendor=0x%04x (CSR)\n",
                      targetClasses[c], oldVid, AIC_USB_PID_WIFI_BT_RUNTIME, AIC_USB_VID_CSR_SPOOF);

                OSIterator *childIter = entry->getChildIterator(gIOServicePlane);
                if (childIter) {
                    while (auto *child = OSDynamicCast(IORegistryEntry, childIter->getNextObject())) {
                        child->setProperty("idVendor", static_cast<unsigned long long>(AIC_USB_VID_CSR_SPOOF), 32);
                        child->setProperty("USB Vendor Name", "CSR");
                        child->setProperty("AIC-BT-Quirk-Active", kOSBooleanTrue);
                        IOLog("AIC8800BTQuirk: Spoofed child interface %s -> idVendor=0x%04x (CSR)\n",
                              child->getName(), AIC_USB_VID_CSR_SPOOF);
                    }
                    childIter->release();
                }
            }
        }
        iter->release();
    }
}

bool AIC8800BTQuirkModule::init() {
    uint32_t offFlag = 0;
    if (PE_parse_boot_argn("aic_off", &offFlag, sizeof(offFlag)) && offFlag != 0) {
        IOLog("AIC8800BTQuirk: disabled by aic_off=1 boot-arg\n");
        return false;
    }
    if (PE_parse_boot_argn("aic_btoff", &offFlag, sizeof(offFlag)) && offFlag != 0) {
        IOLog("AIC8800BTQuirk: disabled by aic_btoff=1 boot-arg\n");
        return false;
    }

    if (!gZlpLock) {
        gZlpLock = IOSimpleLockAlloc();
    }

    /* Force Lilu to hook already-loaded IOUSBHostFamily in Prelinked Kernel Collection */
    kWatchedKexts[0].sys[KernelPatcher::KextInfo::Loaded] = true;

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

    /* Also immediately scan and spoof if device already attached */
    spoofExistingDevices();

    IOLog("AIC8800BTQuirk: initialized (CSR spoof 0x%04x, ZLP threshold %uB)\n",
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
        IOLog("AIC8800BTQuirk: hookedStart for (0x%04x:0x%04x) -> set idVendor=0x%04x (CSR) for bluetoothd\n",
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
             * Rewrite to 0x20 (Class, Device) if targeting control endpoint / interface 0. */
            if (request.bmRequestType == 0x21 && (request.wIndex & 0xFF) == 0) {
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
        IOLog("AIC8800BTQuirk: successfully patched IOUSBHostFamily (start + internalDeviceRequest + sync/async io)\n");
    } else {
        IOLog("AIC8800BTQuirk: warning: routeMultiple on IOUSBHostFamily encountered errors (code=%d)\n",
              patcher.getError());
        patcher.clearError();
    }

    /* Retrospectively spoof any existing devices */
    spoofExistingDevices();
}
