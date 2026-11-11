#include "AIC8800WiFi.hpp"

#include <pexpert/pexpert.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/network/IOOutputQueue.h>
#include <IOKit/usb/StandardUSB.h>
#include <kern/clock.h>
#include <libkern/OSAtomic.h>
#include <libkern/OSByteOrder.h>
#include <libkern/libkern.h>
#include <sys/kpi_mbuf.h>

OSDefineMetaClassAndStructors(AIC8800WiFi, IOEthernetController)

bool AIC8800WiFi::isDisabledByBootArg() {
    uint32_t flag = 0;
    if (PE_parse_boot_argn("aic_off", &flag, sizeof(flag)) && flag != 0) {
        return true;
    }
    if (PE_parse_boot_argn("-aicoff", &flag, sizeof(flag))) {
        return true;
    }
    return false;
}

bool AIC8800WiFi::init(OSDictionary *properties) {
    if (!super::init(properties)) {
        return false;
    }
    if (aic_slot_pool_init(&txPool_, kTxRingSlots) != 0) {
        return false;
    }
    aic_tx_flow_ctrl_init(&txFlowCtrl_, kTxRingSlots, kTxLowWater, kTxHighWater);
    aic_lmac_cmd_matcher_init(&cmdMatcher_);
    return true;
}

void AIC8800WiFi::free() {
    releasePools();
    releaseLocks();
    releasePipes();
    OSSafeReleaseNULL(mediumDict_);
    OSSafeReleaseNULL(netIf_);
    OSSafeReleaseNULL(workLoop_);
    super::free();
}

IOService *AIC8800WiFi::probe(IOService *provider, SInt32 *score) {
    if (isDisabledByBootArg()) {
        IOLog("AIC8800WiFi: disabled by boot-arg (aic_off / -aicoff)\n");
        return nullptr;
    }

    IOUSBHostInterface *iface = OSDynamicCast(IOUSBHostInterface, provider);
    if (!iface) {
        return nullptr;
    }

    const StandardUSB::InterfaceDescriptor *desc = iface->getInterfaceDescriptor();
    if (!desc || desc->bInterfaceNumber != kWifiInterfaceNumber) {
        return nullptr;
    }

    IOService *res = super::probe(provider, score);
    if (res && score) {
        *score += 1000;
    }
    return res;
}

bool AIC8800WiFi::discoverBulkPipes() {
    if (!usbIf_) {
        return false;
    }
    const StandardUSB::ConfigurationDescriptor *cfg = usbIf_->getConfigurationDescriptor();
    const StandardUSB::InterfaceDescriptor *ifd = usbIf_->getInterfaceDescriptor();
    if (!cfg || !ifd) {
        return false;
    }

    const StandardUSB::EndpointDescriptor *ep = nullptr;
    while ((ep = StandardUSB::getNextEndpointDescriptor(cfg, ifd, ep)) != nullptr) {
        const uint8_t epType = StandardUSB::getEndpointType(ep);
        if (epType != kEndpointTypeBulk) {
            continue;
        }
        const uint8_t epAddr = StandardUSB::getEndpointAddress(ep);
        const uint8_t dir = StandardUSB::getEndpointDirection(ep);
        const uint16_t mps = static_cast<uint16_t>(OSSwapLittleToHostInt16(ep->wMaxPacketSize) & 0x07FFU);

        if (dir == kEndpointDirectionIn) {
            if (!bulkInPipe_) {
                bulkInPipe_ = usbIf_->copyPipe(epAddr);
                if (bulkInPipe_) {
                    epInAddr_ = epAddr;
                }
            } else if (!bulkInMsgPipe_) {
                bulkInMsgPipe_ = usbIf_->copyPipe(epAddr);
                if (bulkInMsgPipe_) {
                    epInMsgAddr_ = epAddr;
                }
            }
        } else if (dir == kEndpointDirectionOut) {
            if (!bulkOutPipe_) {
                bulkOutPipe_ = usbIf_->copyPipe(epAddr);
                if (bulkOutPipe_) {
                    epOutDataAddr_ = epAddr;
                    maxPacketSizeOut_ = (mps > 0U) ? mps : AIC_USB_BULK_MPS_HS;
                }
            } else if (!bulkOutMsgPipe_) {
                bulkOutMsgPipe_ = usbIf_->copyPipe(epAddr);
                if (bulkOutMsgPipe_) {
                    epOutMsgAddr_ = epAddr;
                    maxPacketSizeOutMsg_ = (mps > 0U) ? mps : AIC_USB_BULK_MPS_HS;
                }
            }
        }
    }

    return (bulkInPipe_ != nullptr) &&
           (bulkInMsgPipe_ != nullptr) &&
           (bulkOutPipe_ != nullptr) &&
           (bulkOutMsgPipe_ != nullptr);
}

void AIC8800WiFi::releasePipes() {
    OSSafeReleaseNULL(bulkInPipe_);
    OSSafeReleaseNULL(bulkInMsgPipe_);
    OSSafeReleaseNULL(bulkOutPipe_);
    OSSafeReleaseNULL(bulkOutMsgPipe_);
    epInAddr_ = 0U;
    epInMsgAddr_ = 0U;
    epOutDataAddr_ = 0U;
    epOutMsgAddr_ = 0U;
}

bool AIC8800WiFi::allocatePools() {
    if (!usbIf_) {
        return false;
    }

    if (!cmdSerializeLock_) {
        cmdSerializeLock_ = IOLockAlloc();
    }
    if (!cmdWaitLock_) {
        cmdWaitLock_ = IOLockAlloc();
    }
    if (!rxDeliverLock_) {
        rxDeliverLock_ = IOLockAlloc();
    }
    if (!txLock_) {
        txLock_ = IOSimpleLockAlloc();
    }
    if (!cmdSerializeLock_ || !cmdWaitLock_ || !rxDeliverLock_ || !txLock_) {
        releasePools();
        return false;
    }

    zlpDesc_ = usbIf_->createIOBuffer(kIODirectionOut, 64U);
    msgOutDesc_ = usbIf_->createIOBuffer(kIODirectionOut, kMsgRxBufBytes);
    if (!zlpDesc_ || !msgOutDesc_) {
        releasePools();
        return false;
    }
    if (void *zlpBuf = zlpDesc_->getBytesNoCopy()) {
        memset(zlpBuf, 0, 64U);
    }
    msgOutBuf_ = static_cast<uint8_t *>(msgOutDesc_->getBytesNoCopy());
    if (!msgOutBuf_) {
        releasePools();
        return false;
    }

    for (uint16_t i = 0U; i < kRxUrbCount; ++i) {
        rxSlots_[i].owner = this;
        rxSlots_[i].index = i;
        rxSlots_[i].isMsg = false;
        rxSlots_[i].desc  = usbIf_->createIOBuffer(kIODirectionIn, kRxBufBytes);
        rxSlots_[i].buf   = rxSlots_[i].desc ? static_cast<uint8_t *>(rxSlots_[i].desc->getBytesNoCopy()) : nullptr;
        rxSlots_[i].comp.owner     = this;
        rxSlots_[i].comp.action    = &AIC8800WiFi::onRxComplete;
        rxSlots_[i].comp.parameter = &rxSlots_[i];
        if (!rxSlots_[i].desc || !rxSlots_[i].buf) {
            releasePools();
            return false;
        }
    }

    for (uint16_t i = 0U; i < kMsgRxUrbCount; ++i) {
        msgRxSlots_[i].owner = this;
        msgRxSlots_[i].index = i;
        msgRxSlots_[i].isMsg = true;
        msgRxSlots_[i].desc  = usbIf_->createIOBuffer(kIODirectionIn, kMsgRxBufBytes);
        msgRxSlots_[i].buf   = msgRxSlots_[i].desc ? static_cast<uint8_t *>(msgRxSlots_[i].desc->getBytesNoCopy()) : nullptr;
        msgRxSlots_[i].comp.owner     = this;
        msgRxSlots_[i].comp.action    = &AIC8800WiFi::onMsgRxComplete;
        msgRxSlots_[i].comp.parameter = &msgRxSlots_[i];
        if (!msgRxSlots_[i].desc || !msgRxSlots_[i].buf) {
            releasePools();
            return false;
        }
    }

    for (uint16_t i = 0U; i < kTxRingSlots; ++i) {
        txSlots_[i].owner       = this;
        txSlots_[i].index       = i;
        txSlots_[i].pendingUrbs = 0U;
        txSlots_[i].bytes       = 0U;
        txSlots_[i].desc        = usbIf_->createIOBuffer(kIODirectionOut, kTxSlotBufBytes);
        txSlots_[i].buf         = txSlots_[i].desc ? static_cast<uint8_t *>(txSlots_[i].desc->getBytesNoCopy()) : nullptr;
        txSlots_[i].dataComp.owner     = this;
        txSlots_[i].dataComp.action    = &AIC8800WiFi::onTxComplete;
        txSlots_[i].dataComp.parameter = &txSlots_[i];
        txSlots_[i].zlpComp.owner      = this;
        txSlots_[i].zlpComp.action     = &AIC8800WiFi::onTxComplete;
        txSlots_[i].zlpComp.parameter  = &txSlots_[i];
        if (!txSlots_[i].desc || !txSlots_[i].buf) {
            releasePools();
            return false;
        }
    }

    return true;
}

void AIC8800WiFi::releasePools() {
    for (uint16_t i = 0U; i < kTxRingSlots; ++i) {
        OSSafeReleaseNULL(txSlots_[i].desc);
        txSlots_[i].buf         = nullptr;
        txSlots_[i].pendingUrbs = 0U;
        txSlots_[i].bytes       = 0U;
    }
    for (uint16_t i = 0U; i < kRxUrbCount; ++i) {
        OSSafeReleaseNULL(rxSlots_[i].desc);
        rxSlots_[i].buf = nullptr;
    }
    for (uint16_t i = 0U; i < kMsgRxUrbCount; ++i) {
        OSSafeReleaseNULL(msgRxSlots_[i].desc);
        msgRxSlots_[i].buf = nullptr;
    }
    OSSafeReleaseNULL(zlpDesc_);
    OSSafeReleaseNULL(msgOutDesc_);
    msgOutBuf_ = nullptr;
}

void AIC8800WiFi::releaseLocks() {
    if (txLock_) {
        IOSimpleLockFree(txLock_);
        txLock_ = nullptr;
    }
    if (rxDeliverLock_) {
        IOLockFree(rxDeliverLock_);
        rxDeliverLock_ = nullptr;
    }
    if (cmdWaitLock_) {
        IOLockFree(cmdWaitLock_);
        cmdWaitLock_ = nullptr;
    }
    if (cmdSerializeLock_) {
        IOLockFree(cmdSerializeLock_);
        cmdSerializeLock_ = nullptr;
    }
}

bool AIC8800WiFi::submitRxSlot(RxUrbSlot *slot) {
    if (!slot || !slot->desc || stopping_ || isInactive()) {
        return false;
    }
    IOUSBHostPipe *pipe = slot->isMsg ? bulkInMsgPipe_ : bulkInPipe_;
    const uint32_t cap  = slot->isMsg ? kMsgRxBufBytes : kRxBufBytes;
    if (!pipe) {
        return false;
    }

    OSIncrementAtomic(&inFlightIoCount_);
    if (stopping_ || isInactive()) {
        OSDecrementAtomic(&inFlightIoCount_);
        return false;
    }

    const IOReturn ret = pipe->io(slot->desc, cap, &slot->comp, 0U);
    if (ret != kIOReturnSuccess) {
        OSDecrementAtomic(&inFlightIoCount_);
        return false;
    }
    return true;
}

bool AIC8800WiFi::startAsyncRx() {
    for (uint16_t i = 0U; i < kMsgRxUrbCount; ++i) {
        if (!submitRxSlot(&msgRxSlots_[i])) {
            IOLog("AIC8800WiFi: failed to arm msg RX URB %u\n", i);
            return false;
        }
    }
    for (uint16_t i = 0U; i < kRxUrbCount; ++i) {
        if (!submitRxSlot(&rxSlots_[i])) {
            IOLog("AIC8800WiFi: failed to arm data RX URB %u\n", i);
            return false;
        }
    }
    return true;
}

void AIC8800WiFi::stopAndDrainIo() {
    stopping_ = true;
    __sync_synchronize();

    auto abortPipes = [this]() {
        IOUSBHostPipe *const pipes[4] = {
            bulkInPipe_,
            bulkInMsgPipe_,
            bulkOutPipe_,
            bulkOutMsgPipe_,
        };
        for (size_t i = 0U; i < 4U; ++i) {
            if (pipes[i]) {
                (void)pipes[i]->abort(IOUSBHostIOSource::kAbortSynchronous,
                                      kIOReturnAborted,
                                      nullptr);
            }
        }
    };

    for (int iter = 0; iter < 500; ++iter) {
        if (inFlightIoCount_ <= 0) {
            break;
        }
        if (iter == 0 || (iter % 25) == 0) {
            abortPipes();
        }
        IOSleep(2U);
    }

    if (inFlightIoCount_ > 0) {
        IOLog("AIC8800WiFi: warning inFlightIoCount=%d after drain\n",
              static_cast<int>(inFlightIoCount_));
    }
}

void AIC8800WiFi::handleMsgRxBuffer(const uint8_t *buf, uint32_t bytesTransferred) {
    if (!buf || bytesTransferred < 4U || !cmdWaitLock_) {
        return;
    }

    aic_msg_rx_dispatch_t summary;
    memset(&summary, 0, sizeof(summary));

    IOLockLock(cmdWaitLock_);
    (void)aic_usb_dispatch_msg_urb(&cmdMatcher_, buf, bytesTransferred, &summary);
    if (summary.matched_cmd_cfm || cmdMatcher_.completed) {
        IOLockWakeup(cmdWaitLock_, &cmdMatcher_, false);
    }
    IOLockUnlock(cmdWaitLock_);

    if (summary.print_ptr && summary.print_len > 0U) {
        char line[256];
        const size_t n = (summary.print_len < (sizeof(line) - 1U))
                             ? static_cast<size_t>(summary.print_len)
                             : (sizeof(line) - 1U);
        memcpy(line, summary.print_ptr, n);
        size_t outLen = 0U;
        for (size_t i = 0U; i < n; ++i) {
            if (line[i] == '\0') {
                break;
            }
            if (line[i] == '\r' || line[i] == '\n') {
                line[i] = ' ';
            }
            outLen = i + 1U;
        }
        while (outLen > 0U && line[outLen - 1U] == ' ') {
            outLen--;
        }
        line[outLen] = '\0';
        if (outLen > 0U) {
            IOLog("AIC8800-FW: %s\n", line);
        }
    }
}

void AIC8800WiFi::handleDataRxBuffer(const uint8_t *buf, uint32_t bytesTransferred) {
    if (!buf || bytesTransferred < 4U) {
        return;
    }

    struct DataRxCtx {
        AIC8800WiFi *self;
        bool         queuedPackets;
    };

    struct DataRxHelper {
        static void onEthFrame(const uint8_t *eth_frame,
                               size_t eth_len,
                               const aic_rx_frame_meta_t *meta,
                               void *ctx) {
            (void)meta;
            DataRxCtx *rxCtx = static_cast<DataRxCtx *>(ctx);
            AIC8800WiFi *self = rxCtx ? rxCtx->self : nullptr;
            if (!self || !self->netIf_ || !self->isEnabled_ || self->stopping_ ||
                eth_len < 14U || eth_len > kTxSlotBufBytes) {
                return;
            }

            mbuf_t pkt = self->allocatePacket(static_cast<UInt32>(eth_len));
            if (!pkt) {
                if (self->netStats_) {
                    OSIncrementAtomic(&self->netStats_->inputErrors);
                }
                return;
            }

            if (mbuf_copyback(pkt, 0, eth_len, eth_frame, MBUF_DONTWAIT) != 0) {
                self->freePacket(pkt);
                if (self->netStats_) {
                    OSIncrementAtomic(&self->netStats_->inputErrors);
                }
                return;
            }

            (void)self->netIf_->inputPacket(pkt,
                                            static_cast<UInt32>(eth_len),
                                            IONetworkInterface::kInputOptionQueuePacket,
                                            nullptr);
            rxCtx->queuedPackets = true;
            if (self->netStats_) {
                OSIncrementAtomic(&self->netStats_->inputPackets);
            }
        }

        static void onDataChunk(const uint8_t *chunk, size_t chunk_len, void *ctx) {
            DataRxCtx *rxCtx = static_cast<DataRxCtx *>(ctx);
            AIC8800WiFi *self = rxCtx ? rxCtx->self : nullptr;
            if (!self || !self->netIf_ || !self->isEnabled_ || self->stopping_ || !self->rxDeliverLock_) {
                return;
            }

            IOLockLock(self->rxDeliverLock_);
            const int rc = aic_rx_chunk_to_eth_frames(chunk,
                                                      chunk_len,
                                                      self->rxScratchFrame_,
                                                      sizeof(self->rxScratchFrame_),
                                                      &DataRxHelper::onEthFrame,
                                                      rxCtx);
            IOLockUnlock(self->rxDeliverLock_);
            if (rc < 0 && self->netStats_) {
                OSIncrementAtomic(&self->netStats_->inputErrors);
            }
        }

        static void onCfgPkt(uint8_t cfg_type, const uint8_t *payload, size_t payload_len, void *ctx) {
            DataRxCtx *rxCtx = static_cast<DataRxCtx *>(ctx);
            AIC8800WiFi *self = rxCtx ? rxCtx->self : nullptr;
            if (!self || !self->cmdWaitLock_ || !payload || payload_len > UINT16_MAX) {
                return;
            }

            aic_msg_rx_dispatch_t summary;
            memset(&summary, 0, sizeof(summary));
            summary.matcher = &self->cmdMatcher_;

            IOLockLock(self->cmdWaitLock_);
            (void)aic_usb_handle_ctrl_pkt(&summary,
                                          cfg_type,
                                          payload,
                                          static_cast<uint16_t>(payload_len));
            if (summary.matched_cmd_cfm || self->cmdMatcher_.completed) {
                IOLockWakeup(self->cmdWaitLock_, &self->cmdMatcher_, false);
            }
            IOLockUnlock(self->cmdWaitLock_);
        }
    };

    DataRxCtx rxCtx = { this, false };
    (void)aic_rx_demux_usb_aggr(buf,
                                bytesTransferred,
                                &DataRxHelper::onDataChunk,
                                &DataRxHelper::onCfgPkt,
                                &rxCtx);

    if (rxCtx.queuedPackets && netIf_) {
        (void)netIf_->flushInputQueue();
    }
}

void AIC8800WiFi::onRxComplete(void *owner, void *parameter, IOReturn status, uint32_t bytesTransferred) {
    AIC8800WiFi *self = static_cast<AIC8800WiFi *>(owner);
    RxUrbSlot *slot = static_cast<RxUrbSlot *>(parameter);
    if (!self || !slot) {
        return;
    }

    if (status == kIOReturnSuccess && bytesTransferred > 0U && !self->stopping_ && !self->isInactive()) {
        self->handleDataRxBuffer(slot->buf, bytesTransferred);
    }

    const bool fatalStatus = (status == kIOReturnAborted ||
                              status == kIOReturnNotResponding ||
                              status == kIOReturnOffline);
    if (!fatalStatus && !self->stopping_ && !self->isInactive()) {
        (void)self->submitRxSlot(slot);
    }
    OSDecrementAtomic(&self->inFlightIoCount_);
}

void AIC8800WiFi::onMsgRxComplete(void *owner, void *parameter, IOReturn status, uint32_t bytesTransferred) {
    AIC8800WiFi *self = static_cast<AIC8800WiFi *>(owner);
    RxUrbSlot *slot = static_cast<RxUrbSlot *>(parameter);
    if (!self || !slot) {
        return;
    }

    if (status == kIOReturnSuccess && bytesTransferred > 0U && !self->stopping_ && !self->isInactive()) {
        self->handleMsgRxBuffer(slot->buf, bytesTransferred);
    }

    const bool fatalStatus = (status == kIOReturnAborted ||
                              status == kIOReturnNotResponding ||
                              status == kIOReturnOffline);
    if (!fatalStatus && !self->stopping_ && !self->isInactive()) {
        (void)self->submitRxSlot(slot);
    }
    OSDecrementAtomic(&self->inFlightIoCount_);
}

void AIC8800WiFi::onTxComplete(void *owner, void *parameter, IOReturn status, uint32_t bytesTransferred) {
    (void)bytesTransferred;
    AIC8800WiFi *self = static_cast<AIC8800WiFi *>(owner);
    TxUrbSlot *slot = static_cast<TxUrbSlot *>(parameter);
    if (!self || !slot) {
        return;
    }

    bool slotFinished = false;
    bool shouldWake = false;
    if (self->txLock_) {
        IOSimpleLockLock(self->txLock_);
        if (slot->pendingUrbs > 0U) {
            slot->pendingUrbs--;
        }
        if (slot->pendingUrbs == 0U) {
            slotFinished = true;
            (void)aic_slot_pool_release(&self->txPool_, slot->index);
            shouldWake = aic_tx_flow_ctrl_on_release(&self->txFlowCtrl_,
                                                      aic_slot_pool_in_flight(&self->txPool_));
        }
        IOSimpleLockUnlock(self->txLock_);
    }

    if (self->netStats_) {
        if (status == kIOReturnSuccess) {
            if (slotFinished) {
                OSIncrementAtomic(&self->netStats_->outputPackets);
            }
        } else if (status != kIOReturnAborted) {
            OSIncrementAtomic(&self->netStats_->outputErrors);
        }
    }

    if (shouldWake && !self->stopping_ && self->getOutputQueue()) {
        self->getOutputQueue()->service(IOBasicOutputQueue::kServiceAsync);
    }

    OSDecrementAtomic(&self->inFlightIoCount_);
}

IOReturn AIC8800WiFi::sendLmacCmd(uint16_t msgId, uint16_t destId, uint16_t srcId,
                                  const void *param, uint16_t paramLen,
                                  uint16_t expectedCfmId,
                                  void *cfmParamOut, uint16_t cfmMaxLen, uint16_t *cfmActualLenOut,
                                  uint32_t timeoutMs) {
    if (stopping_ || isInactive() || !bulkOutMsgPipe_ || !cmdSerializeLock_ || !cmdWaitLock_ ||
        !msgOutDesc_ || !msgOutBuf_) {
        return kIOReturnNotReady;
    }

    IOLockLock(cmdSerializeLock_);
    if (stopping_ || isInactive()) {
        IOLockUnlock(cmdSerializeLock_);
        return kIOReturnAborted;
    }

    size_t wireLen = 0U;
    if (aic_encode_lmac_msg(msgOutBuf_, kMsgRxBufBytes,
                            msgId, destId, srcId,
                            param, paramLen, &wireLen) != 0) {
        IOLockUnlock(cmdSerializeLock_);
        return kIOReturnBadArgument;
    }

    IOLockLock(cmdWaitLock_);
    aic_lmac_cmd_matcher_arm(&cmdMatcher_, expectedCfmId);
    IOLockUnlock(cmdWaitLock_);

    OSIncrementAtomic(&inFlightIoCount_);
    uint32_t bytesTransferred = 0U;
    IOReturn txRet = bulkOutMsgPipe_->io(msgOutDesc_,
                                         static_cast<uint32_t>(wireLen),
                                         bytesTransferred,
                                         timeoutMs);
    OSDecrementAtomic(&inFlightIoCount_);

    if (txRet == kIOReturnSuccess && bytesTransferred != static_cast<uint32_t>(wireLen)) {
        txRet = kIOReturnUnderrun;
    }

    if (txRet == kIOReturnSuccess && zlpDesc_ != nullptr &&
        aic_usb_needs_zlp(static_cast<uint32_t>(wireLen), maxPacketSizeOutMsg_)) {
        OSIncrementAtomic(&inFlightIoCount_);
        uint32_t zlpBytes = 0U;
        const IOReturn zlpRet = bulkOutMsgPipe_->io(zlpDesc_, 0U, zlpBytes, timeoutMs);
        OSDecrementAtomic(&inFlightIoCount_);
        if (zlpRet != kIOReturnSuccess) {
            txRet = zlpRet;
        }
    }

    if (txRet != kIOReturnSuccess) {
        IOLockLock(cmdWaitLock_);
        aic_lmac_cmd_matcher_disarm(&cmdMatcher_);
        IOLockUnlock(cmdWaitLock_);
        IOLockUnlock(cmdSerializeLock_);
        return txRet;
    }

    AbsoluteTime deadline = 0;
    clock_interval_to_deadline(timeoutMs, kMillisecondScale, &deadline);

    IOLockLock(cmdWaitLock_);
    while (!cmdMatcher_.completed && !stopping_ && !isInactive()) {
        const int waitRes = IOLockSleepDeadline(cmdWaitLock_, &cmdMatcher_, deadline, THREAD_UNINT);
        if (waitRes == THREAD_TIMED_OUT || waitRes == THREAD_INTERRUPTED) {
            break;
        }
    }

    IOReturn result = kIOReturnTimeout;
    if (stopping_ || isInactive()) {
        result = kIOReturnAborted;
    } else if (cmdMatcher_.completed) {
        if (cmdMatcher_.status == 0) {
            if (cfmActualLenOut) {
                *cfmActualLenOut = cmdMatcher_.rx_param_len;
            }
            if (cfmParamOut && cfmMaxLen > 0U && cmdMatcher_.rx_param_len > 0U) {
                const uint16_t copyLen = (cmdMatcher_.rx_param_len < cfmMaxLen)
                                             ? cmdMatcher_.rx_param_len
                                             : cfmMaxLen;
                memcpy(cfmParamOut, cmdMatcher_.rx_param, copyLen);
            }
            result = kIOReturnSuccess;
        } else {
            result = kIOReturnIOError;
        }
    }
    aic_lmac_cmd_matcher_disarm(&cmdMatcher_);
    IOLockUnlock(cmdWaitLock_);
    IOLockUnlock(cmdSerializeLock_);
    return result;
}

bool AIC8800WiFi::bringUpLmac() {
    uint16_t cfmLen = 0U;
    IOReturn ret = kIOReturnSuccess;

    /* 1. Read AIC8800D80 chip ID register */
    aic_dbg_mem_read_req_t chipReq = { AIC_DBG_MEM_ADDR_CHIP_ID };
    aic_dbg_mem_read_cfm_t chipCfm = {};
    ret = sendLmacCmd(AIC_DBG_MEM_READ_REQ, AIC_TASK_DBG, AIC_DRV_TASK_ID,
                      &chipReq, sizeof(chipReq),
                      AIC_DBG_MEM_READ_CFM,
                      &chipCfm, sizeof(chipCfm), &cfmLen, AIC_CMD_TX_TIMEOUT_MS);
    if (ret != kIOReturnSuccess || cfmLen < sizeof(chipCfm)) {
        IOLog("AIC8800WiFi: DBG_MEM_READ_REQ(0x%08x) failed ret=0x%x cfmLen=%u\n",
              AIC_DBG_MEM_ADDR_CHIP_ID, ret, cfmLen);
        return false;
    }
    chipIdReg_ = chipCfm.memdata;

    /* 2. Notify firmware stack start & query 5 GHz / vendor info */
    aic_mm_set_stack_start_req_t stackReq = { 1U, 0U, AIC_STACK_START_VENDOR_BIT, 0U };
    aic_mm_set_stack_start_cfm_t stackCfm = {};
    cfmLen = 0U;
    ret = sendLmacCmd(AIC_MM_SET_STACK_START_REQ, AIC_TASK_MM, AIC_DRV_TASK_ID,
                      &stackReq, sizeof(stackReq),
                      AIC_MM_SET_STACK_START_CFM,
                      &stackCfm, sizeof(stackCfm), &cfmLen, AIC_CMD_TX_TIMEOUT_MS);
    if (ret != kIOReturnSuccess || cfmLen < sizeof(stackCfm)) {
        IOLog("AIC8800WiFi: MM_SET_STACK_START_REQ failed ret=0x%x cfmLen=%u\n", ret, cfmLen);
        return false;
    }
    is5gSupported_ = stackCfm.is_5g_support;
    vendorInfo_    = stackCfm.vendor_info;

    /* 3. Query firmware version string */
    uint8_t fwVerReq = 0U;
    aic_mm_get_fw_version_cfm_t fwVerCfm = {};
    cfmLen = 0U;
    ret = sendLmacCmd(AIC_MM_GET_FW_VERSION_REQ, AIC_TASK_MM, AIC_DRV_TASK_ID,
                      &fwVerReq, sizeof(fwVerReq),
                      AIC_MM_GET_FW_VERSION_CFM,
                      &fwVerCfm, sizeof(fwVerCfm), &cfmLen, AIC_CMD_TX_TIMEOUT_MS);
    if (ret != kIOReturnSuccess || cfmLen == 0U) {
        IOLog("AIC8800WiFi: MM_GET_FW_VERSION_REQ failed ret=0x%x cfmLen=%u\n", ret, cfmLen);
        return false;
    }
    memset(fwVersion_, 0, sizeof(fwVersion_));
    const size_t rawVerLen = (fwVerCfm.fw_version_len < sizeof(fwVerCfm.fw_version))
                                 ? static_cast<size_t>(fwVerCfm.fw_version_len)
                                 : sizeof(fwVerCfm.fw_version);
    const size_t copyVerLen = (rawVerLen < (sizeof(fwVersion_) - 1U)) ? rawVerLen : (sizeof(fwVersion_) - 1U);
    for (size_t i = 0U; i < copyVerLen; ++i) {
        const uint8_t ch = fwVerCfm.fw_version[i];
        if (ch == 0U) {
            break;
        }
        fwVersion_[i] = (ch >= 0x20U && ch <= 0x7EU) ? ch : ' ';
    }

    /* 4. Configure AIC8800D80 v3 TX power levels (95B union) */
    aic_mm_set_txpwr_lvl_req_t pwrReq = {};
    aic_fill_d80_txpwr_lvl_v3(&pwrReq);
    cfmLen = 0U;
    ret = sendLmacCmd(AIC_MM_SET_TXPWR_IDX_LVL_REQ, AIC_TASK_MM, AIC_DRV_TASK_ID,
                      &pwrReq, sizeof(pwrReq),
                      AIC_MM_SET_TXPWR_IDX_LVL_CFM,
                      nullptr, 0U, &cfmLen, AIC_CMD_TX_TIMEOUT_MS);
    if (ret != kIOReturnSuccess) {
        IOLog("AIC8800WiFi: MM_SET_TXPWR_IDX_LVL_REQ failed ret=0x%x\n", ret);
        return false;
    }

    /* 5. Run Stage 2 RF calibration */
    aic_mm_set_rf_calib_req_t calibReq = {};
    aic_mm_set_rf_calib_cfm_t calibCfm = {};
    aic_build_d80_rf_calib_req(&calibReq);
    cfmLen = 0U;
    ret = sendLmacCmd(AIC_MM_SET_RF_CALIB_REQ, AIC_TASK_MM, AIC_DRV_TASK_ID,
                      &calibReq, sizeof(calibReq),
                      AIC_MM_SET_RF_CALIB_CFM,
                      &calibCfm, sizeof(calibCfm), &cfmLen, AIC_CMD_TX_TIMEOUT_MS);
    if (ret != kIOReturnSuccess) {
        IOLog("AIC8800WiFi: MM_SET_RF_CALIB_REQ failed ret=0x%x\n", ret);
        return false;
    }

    /* 6. Read eFuse MAC address BEFORE attaching IOEthernetInterface */
    aic_mm_get_mac_addr_req_t macReq = { 1U };
    aic_mm_get_mac_addr_cfm_t macCfm = {};
    cfmLen = 0U;
    ret = sendLmacCmd(AIC_MM_GET_MAC_ADDR_REQ, AIC_TASK_MM, AIC_DRV_TASK_ID,
                      &macReq, sizeof(macReq),
                      AIC_MM_GET_MAC_ADDR_CFM,
                      &macCfm, sizeof(macCfm), &cfmLen, AIC_CMD_TX_TIMEOUT_MS);
    if (ret != kIOReturnSuccess || cfmLen < sizeof(macCfm)) {
        IOLog("AIC8800WiFi: MM_GET_MAC_ADDR_REQ failed ret=0x%x cfmLen=%u\n", ret, cfmLen);
        return false;
    }
    const bool nonZeroMac = (macCfm.mac_addr[0] | macCfm.mac_addr[1] | macCfm.mac_addr[2] |
                             macCfm.mac_addr[3] | macCfm.mac_addr[4] | macCfm.mac_addr[5]) != 0U;
    const bool unicastMac = (macCfm.mac_addr[0] & 0x01U) == 0U;
    if (nonZeroMac && unicastMac) {
        memcpy(hwAddr_.bytes, macCfm.mac_addr, sizeof(hwAddr_.bytes));
    } else {
        IOLog("AIC8800WiFi: eFuse MAC invalid (%02x:%02x:%02x:%02x:%02x:%02x), keeping fallback\n",
              macCfm.mac_addr[0], macCfm.mac_addr[1], macCfm.mac_addr[2],
              macCfm.mac_addr[3], macCfm.mac_addr[4], macCfm.mac_addr[5]);
    }

    /* 7. Reset LMAC state machine */
    cfmLen = 0U;
    ret = sendLmacCmd(AIC_MM_RESET_REQ, AIC_TASK_MM, AIC_DRV_TASK_ID,
                      nullptr, 0U,
                      AIC_MM_RESET_CFM,
                      nullptr, 0U, &cfmLen, AIC_CMD_TX_TIMEOUT_MS);
    if (ret != kIOReturnSuccess) {
        IOLog("AIC8800WiFi: MM_RESET_REQ failed ret=0x%x\n", ret);
        return false;
    }

    /* 8. Query LMAC version & feature bitmap */
    aic_mm_version_cfm_t verCfm = {};
    cfmLen = 0U;
    ret = sendLmacCmd(AIC_MM_VERSION_REQ, AIC_TASK_MM, AIC_DRV_TASK_ID,
                      nullptr, 0U,
                      AIC_MM_VERSION_CFM,
                      &verCfm, sizeof(verCfm), &cfmLen, AIC_CMD_TX_TIMEOUT_MS);
    if (ret != kIOReturnSuccess || cfmLen < sizeof(verCfm)) {
        IOLog("AIC8800WiFi: MM_VERSION_REQ failed ret=0x%x cfmLen=%u\n", ret, cfmLen);
        return false;
    }
    lmacVer_    = verCfm.version_lmac;
    fwFeatures_ = verCfm.features;

    /* 9. Configure ME capabilities (HT/VHT/HE/PS/DPSM) */
    aic_me_config_req_t meReq = {};
    aic_build_d80_me_config_req(&meReq, fwFeatures_, is5gSupported_);
    cfmLen = 0U;
    ret = sendLmacCmd(AIC_ME_CONFIG_REQ, AIC_TASK_ME, AIC_DRV_TASK_ID,
                      &meReq, sizeof(meReq),
                      AIC_ME_CONFIG_CFM,
                      nullptr, 0U, &cfmLen, AIC_CMD_TX_TIMEOUT_MS);
    if (ret != kIOReturnSuccess) {
        IOLog("AIC8800WiFi: ME_CONFIG_REQ failed ret=0x%x\n", ret);
        return false;
    }

    /* 10. Configure regulatory channels (2.4 GHz + optional 5 GHz) */
    aic_me_chan_config_req_t chanReq = {};
    aic_build_d80_me_chan_config_req(&chanReq, is5gSupported_);
    cfmLen = 0U;
    ret = sendLmacCmd(AIC_ME_CHAN_CONFIG_REQ, AIC_TASK_ME, AIC_DRV_TASK_ID,
                      &chanReq, sizeof(chanReq),
                      AIC_ME_CHAN_CONFIG_CFM,
                      nullptr, 0U, &cfmLen, AIC_CMD_TX_TIMEOUT_MS);
    if (ret != kIOReturnSuccess) {
        IOLog("AIC8800WiFi: ME_CHAN_CONFIG_REQ failed ret=0x%x\n", ret);
        return false;
    }

    /* 11. Start LMAC */
    aic_mm_start_req_t startReq = {};
    aic_build_d80_mm_start_req(&startReq);
    cfmLen = 0U;
    ret = sendLmacCmd(AIC_MM_START_REQ, AIC_TASK_MM, AIC_DRV_TASK_ID,
                      &startReq, sizeof(startReq),
                      AIC_MM_START_CFM,
                      nullptr, 0U, &cfmLen, AIC_CMD_TX_TIMEOUT_MS);
    if (ret != kIOReturnSuccess) {
        IOLog("AIC8800WiFi: MM_START_REQ failed ret=0x%x\n", ret);
        return false;
    }

    /* 12. Enable Wi-Fi/BT coexistence */
    aic_mm_set_coex_req_t coexReq = {};
    aic_build_d80_set_coex_req(&coexReq);
    cfmLen = 0U;
    ret = sendLmacCmd(AIC_MM_SET_COEX_REQ, AIC_TASK_MM, AIC_DRV_TASK_ID,
                      &coexReq, sizeof(coexReq),
                      AIC_MM_SET_COEX_CFM,
                      nullptr, 0U, &cfmLen, AIC_CMD_TX_TIMEOUT_MS);
    if (ret != kIOReturnSuccess) {
        IOLog("AIC8800WiFi: MM_SET_COEX_REQ failed ret=0x%x\n", ret);
        return false;
    }

    /* 13. Create default STA virtual interface with resolved MAC */
    aic_mm_add_if_req_t addIfReq = {};
    aic_mm_add_if_cfm_t addIfCfm = {};
    aic_build_d80_add_if_sta_req(&addIfReq, hwAddr_.bytes);
    cfmLen = 0U;
    ret = sendLmacCmd(AIC_MM_ADD_IF_REQ, AIC_TASK_MM, AIC_DRV_TASK_ID,
                      &addIfReq, sizeof(addIfReq),
                      AIC_MM_ADD_IF_CFM,
                      &addIfCfm, sizeof(addIfCfm), &cfmLen, AIC_CMD_TX_TIMEOUT_MS);
    if (ret != kIOReturnSuccess || cfmLen < sizeof(addIfCfm) || addIfCfm.status != 0U) {
        IOLog("AIC8800WiFi: MM_ADD_IF_REQ failed ret=0x%x cfmLen=%u status=%u\n",
              ret, cfmLen, addIfCfm.status);
        return false;
    }
    vifIdx_ = addIfCfm.inst_nbr;

    /* 14. Disable A-MPDU aggregation initially for Stage 2 baseline */
    aic_mm_set_agg_disable_req_t aggReq = {};
    aic_build_d80_agg_disable_req(&aggReq);
    cfmLen = 0U;
    ret = sendLmacCmd(AIC_MM_SET_AGG_DISABLE_REQ, AIC_TASK_MM, AIC_DRV_TASK_ID,
                      &aggReq, sizeof(aggReq),
                      AIC_MM_SET_AGG_DISABLE_CFM,
                      nullptr, 0U, &cfmLen, AIC_CMD_TX_TIMEOUT_MS);
    if (ret != kIOReturnSuccess) {
        IOLog("AIC8800WiFi: MM_SET_AGG_DISABLE_REQ failed ret=0x%x\n", ret);
        return false;
    }

    char macStr[24];
    snprintf(macStr, sizeof(macStr), "%02x:%02x:%02x:%02x:%02x:%02x",
             hwAddr_.bytes[0], hwAddr_.bytes[1], hwAddr_.bytes[2],
             hwAddr_.bytes[3], hwAddr_.bytes[4], hwAddr_.bytes[5]);

    setProperty("AIC-ChipID", chipIdReg_, 32U);
    setProperty("AIC-LMAC-Version", lmacVer_, 32U);
    setProperty("AIC-FW-Features", fwFeatures_, 32U);
    setProperty("AIC-FW-Version", reinterpret_cast<const char *>(fwVersion_));
    setProperty("AIC-5G-Support", is5gSupported_ != 0U);
    setProperty("AIC-Vendor-Info", vendorInfo_, 8U);
    setProperty("AIC-VIF-Index", vifIdx_, 8U);
    setProperty("AIC-MAC-Address", macStr);
    lmacReady_ = true;

    IOLog("AIC8800WiFi: LMAC ready chip=0x%08x fw=%s lmac=0x%08x feat=0x%08x 5G=%u mac=%s vif=%u\n",
          chipIdReg_, reinterpret_cast<const char *>(fwVersion_),
          lmacVer_, fwFeatures_, is5gSupported_, macStr, vifIdx_);
    return true;
}

size_t AIC8800WiFi::encodeEthMbufToSlot(mbuf_t m, uint16_t slotIdx) {
    if (!m || slotIdx >= kTxRingSlots || !txSlots_[slotIdx].buf) {
        return 0U;
    }
    const size_t totalLen = mbuf_pkthdr_len(m);
    if (totalLen == 0U || totalLen > sizeof(txScratchFrame_)) {
        return 0U;
    }
    if (mbuf_copydata(m, 0, totalLen, txScratchFrame_) != 0) {
        return 0U;
    }
    size_t usbLen = 0U;
    if (aic_tx_encode_eth_frame(txScratchFrame_,
                                totalLen,
                                0U,
                                vifIdx_,
                                0U,
                                0U,
                                0U,
                                true,
                                txSlots_[slotIdx].buf,
                                kTxSlotBufBytes,
                                &usbLen) != 0) {
        return 0U;
    }
    return usbLen;
}

bool AIC8800WiFi::start(IOService *provider) {
    if (isDisabledByBootArg()) {
        IOLog("AIC8800WiFi: start aborted due to kill-switch boot-arg\n");
        return false;
    }

    usbIf_ = OSDynamicCast(IOUSBHostInterface, provider);
    if (!usbIf_) {
        return false;
    }

    if (!super::start(provider)) {
        return false;
    }

    if (!usbIf_->open(this)) {
        IOLog("AIC8800WiFi: failed to open IOUSBHostInterface\n");
        super::stop(provider);
        usbIf_ = nullptr;
        return false;
    }

    stopping_        = false;
    isEnabled_       = false;
    lmacReady_       = false;
    inFlightIoCount_ = 0;
    (void)aic_slot_pool_init(&txPool_, kTxRingSlots);
    aic_tx_flow_ctrl_init(&txFlowCtrl_, kTxRingSlots, kTxLowWater, kTxHighWater);
    aic_lmac_cmd_matcher_init(&cmdMatcher_);

    if (!discoverBulkPipes()) {
        IOLog("AIC8800WiFi: 4 bulk IN/OUT endpoints missing on interface %u\n",
              kWifiInterfaceNumber);
        releasePipes();
        closeUsbInterface();
        super::stop(provider);
        usbIf_ = nullptr;
        return false;
    }

    if (!allocatePools()) {
        IOLog("AIC8800WiFi: failed to pre-allocate zero-heap USB DMA pools\n");
        releasePipes();
        closeUsbInterface();
        super::stop(provider);
        usbIf_ = nullptr;
        return false;
    }

    if (!startAsyncRx()) {
        IOLog("AIC8800WiFi: failed to arm async RX/MSG USB URBs\n");
        stopAndDrainIo();
        closeUsbInterface();
        releasePipes();
        releasePools();
        super::stop(provider);
        usbIf_ = nullptr;
        return false;
    }

    /* Execute full Stage 2 LMAC/RF bring-up & eFuse MAC discovery BEFORE attachInterface */
    if (!bringUpLmac()) {
        IOLog("AIC8800WiFi: Stage 2 LMAC bring-up failed\n");
        stopAndDrainIo();
        closeUsbInterface();
        releasePipes();
        releasePools();
        super::stop(provider);
        usbIf_ = nullptr;
        return false;
    }

    if (mediumDict_) {
        OSSafeReleaseNULL(mediumDict_);
    }
    mediumDict_ = OSDictionary::withCapacity(1);
    if (mediumDict_) {
        IONetworkMedium *autoMedium = IONetworkMedium::medium(
            kIOMediumEthernetAuto, 600000000ULL, 0, 0, nullptr);
        if (autoMedium) {
            IONetworkMedium::addMedium(mediumDict_, autoMedium);
            publishMediumDictionary(mediumDict_);
            setSelectedMedium(autoMedium);
            setCurrentMedium(autoMedium);
            autoMedium->release();
        }
    }

    if (!attachInterface(reinterpret_cast<IONetworkInterface **>(&netIf_), false)) {
        IOLog("AIC8800WiFi: attachInterface failed\n");
        stopAndDrainIo();
        closeUsbInterface();
        releasePipes();
        releasePools();
        super::stop(provider);
        usbIf_ = nullptr;
        return false;
    }

    /* Phase 1: keep kIONetworkLinkActive OFF until Phase 3A association */
    setLinkStatus(kIONetworkLinkValid);
    netIf_->registerService();
    registerService();
    IOLog("AIC8800WiFi: started Phase 1 (in=0x%02x msgIn=0x%02x out=0x%02x msgOut=0x%02x mps=%u/%u)\n",
          epInAddr_, epInMsgAddr_, epOutDataAddr_, epOutMsgAddr_,
          maxPacketSizeOut_, maxPacketSizeOutMsg_);
    return true;
}

void AIC8800WiFi::wakeCmdWaiters(int32_t statusCode) {
    if (cmdWaitLock_) {
        IOLockLock(cmdWaitLock_);
        cmdMatcher_.waiting   = false;
        cmdMatcher_.status    = statusCode;
        cmdMatcher_.completed = true;
        IOLockWakeup(cmdWaitLock_, &cmdMatcher_, false);
        IOLockUnlock(cmdWaitLock_);
    }
}

void AIC8800WiFi::closeUsbInterface() {
    if (usbIf_ && usbIf_->isOpen(this)) {
        usbIf_->close(this);
    }
}

bool AIC8800WiFi::willTerminate(IOService *provider, IOOptionBits options) {
    stopping_  = true;
    isEnabled_ = false;
    __sync_synchronize();

    wakeCmdWaiters(-3);

    if (provider == usbIf_) {
        /*
         * IOUSBHostInterface::willTerminate requires closing our open session so
         * IOUSBHostInterface can abort endpoint IO and allow stop()/finalize().
         */
        closeUsbInterface();
    }

    return super::willTerminate(provider, options);
}

bool AIC8800WiFi::didTerminate(IOService *provider, IOOptionBits options, bool *defer) {
    stopping_  = true;
    isEnabled_ = false;
    __sync_synchronize();

    stopAndDrainIo();

    if (provider == usbIf_) {
        closeUsbInterface();
    }

    return super::didTerminate(provider, options, defer);
}

void AIC8800WiFi::stop(IOService *provider) {
    stopping_  = true;
    isEnabled_ = false;
    __sync_synchronize();

    wakeCmdWaiters(-3);

    if (getOutputQueue()) {
        getOutputQueue()->stop();
        (void)getOutputQueue()->flush();
    }

    stopAndDrainIo();

    netStats_ = nullptr;
    if (netIf_) {
        detachInterface(netIf_);
        OSSafeReleaseNULL(netIf_);
    }

    releasePools();
    releasePipes();
    closeUsbInterface();
    usbIf_ = nullptr;
    super::stop(provider);
}

bool AIC8800WiFi::createWorkLoop() {
    if (!workLoop_) {
        workLoop_ = IOWorkLoop::workLoop();
    }
    return workLoop_ != nullptr;
}

IOWorkLoop *AIC8800WiFi::getWorkLoop() const {
    return workLoop_;
}

IOOutputQueue *AIC8800WiFi::createOutputQueue() {
    return IOGatedOutputQueue::withTarget(this, getWorkLoop(), kTxRingSlots);
}

bool AIC8800WiFi::configureInterface(IONetworkInterface *interface) {
    if (!super::configureInterface(interface)) {
        return false;
    }
    IONetworkData *data = interface->getNetworkData(kIONetworkStatsKey);
    if (data) {
        netStats_ = static_cast<IONetworkStats *>(const_cast<void *>(data->getBuffer()));
    }
    return true;
}

IOReturn AIC8800WiFi::enable(IONetworkInterface *netif) {
    (void)netif;
    if (stopping_ || isInactive()) {
        return kIOReturnOffline;
    }
    isEnabled_ = true;
    if (getOutputQueue()) {
        getOutputQueue()->start();
    }
    setLinkStatus(kIONetworkLinkValid);
    return kIOReturnSuccess;
}

IOReturn AIC8800WiFi::disable(IONetworkInterface *netif) {
    (void)netif;
    isEnabled_ = false;
    if (getOutputQueue()) {
        getOutputQueue()->stop();
        (void)getOutputQueue()->flush();
    }
    setLinkStatus(kIONetworkLinkValid);
    return kIOReturnSuccess;
}

IOReturn AIC8800WiFi::getHardwareAddress(IOEthernetAddress *addrP) {
    if (!addrP) {
        return kIOReturnBadArgument;
    }
    *addrP = hwAddr_;
    return kIOReturnSuccess;
}

UInt32 AIC8800WiFi::outputPacket(mbuf_t m, void *param) {
    (void)param;
    if (!m) {
        return kIOReturnOutputSuccess;
    }

    if (!isEnabled_ || stopping_ || !bulkOutPipe_ || !txLock_) {
        if (netStats_) {
            OSIncrementAtomic(&netStats_->outputErrors);
        }
        freePacket(m);
        return kIOReturnOutputDropped;
    }

    uint16_t slotIdx = 0U;
    TxUrbSlot *slot = nullptr;

    auto releaseTxSlot = [this](uint16_t idx) {
        IOSimpleLockLock(txLock_);
        txSlots_[idx].pendingUrbs = 0U;
        (void)aic_slot_pool_release(&txPool_, idx);
        const bool shouldWake = aic_tx_flow_ctrl_on_release(&txFlowCtrl_,
                                                            aic_slot_pool_in_flight(&txPool_));
        IOSimpleLockUnlock(txLock_);
        if (shouldWake && !stopping_ && getOutputQueue()) {
            getOutputQueue()->service(IOBasicOutputQueue::kServiceAsync);
        }
    };

    IOSimpleLockLock(txLock_);
    if (!aic_tx_flow_ctrl_can_accept(&txFlowCtrl_, aic_slot_pool_in_flight(&txPool_)) ||
        aic_slot_pool_acquire(&txPool_, &slotIdx) != 0) {
        IOSimpleLockUnlock(txLock_);
        return kIOReturnOutputStall;
    }
    (void)aic_tx_flow_ctrl_on_acquire(&txFlowCtrl_, aic_slot_pool_in_flight(&txPool_));
    slot = &txSlots_[slotIdx];
    IOSimpleLockUnlock(txLock_);

    const size_t encodedLen = encodeEthMbufToSlot(m, slotIdx);
    if (encodedLen == 0U || encodedLen > kTxSlotBufBytes || !slot->desc || !slot->buf) {
        releaseTxSlot(slotIdx);
        if (netStats_) {
            OSIncrementAtomic(&netStats_->outputErrors);
        }
        freePacket(m);
        return kIOReturnOutputDropped;
    }

    const bool needsZlp = (zlpDesc_ != nullptr) &&
                          aic_usb_needs_zlp(static_cast<uint32_t>(encodedLen), maxPacketSizeOut_);

    IOSimpleLockLock(txLock_);
    slot->bytes       = static_cast<uint32_t>(encodedLen);
    slot->pendingUrbs = needsZlp ? 2U : 1U;
    IOSimpleLockUnlock(txLock_);

    OSIncrementAtomic(&inFlightIoCount_);
    if (stopping_) {
        OSDecrementAtomic(&inFlightIoCount_);
        releaseTxSlot(slotIdx);
        if (netStats_) {
            OSIncrementAtomic(&netStats_->outputErrors);
        }
        freePacket(m);
        return kIOReturnOutputDropped;
    }

    const IOReturn ret = bulkOutPipe_->io(slot->desc, static_cast<uint32_t>(encodedLen), &slot->dataComp, 0U);
    if (ret != kIOReturnSuccess) {
        OSDecrementAtomic(&inFlightIoCount_);
        releaseTxSlot(slotIdx);
        if (netStats_) {
            OSIncrementAtomic(&netStats_->outputErrors);
        }
        freePacket(m);
        return kIOReturnOutputDropped;
    }

    freePacket(m);

    if (needsZlp) {
        OSIncrementAtomic(&inFlightIoCount_);
        const IOReturn zlpRet = bulkOutPipe_->io(zlpDesc_, 0U, &slot->zlpComp, 0U);
        if (zlpRet != kIOReturnSuccess) {
            OSDecrementAtomic(&inFlightIoCount_);
            bool shouldWake = false;
            IOSimpleLockLock(txLock_);
            if (slot->pendingUrbs > 0U) {
                slot->pendingUrbs--;
            }
            if (slot->pendingUrbs == 0U) {
                (void)aic_slot_pool_release(&txPool_, slotIdx);
                shouldWake = aic_tx_flow_ctrl_on_release(&txFlowCtrl_,
                                                         aic_slot_pool_in_flight(&txPool_));
            }
            IOSimpleLockUnlock(txLock_);
            if (shouldWake && !stopping_ && getOutputQueue()) {
                getOutputQueue()->service(IOBasicOutputQueue::kServiceAsync);
            }
        }
    }

    return kIOReturnOutputSuccess;
}

IOReturn AIC8800WiFi::setPromiscuousMode(bool active) {
    (void)active;
    return kIOReturnSuccess;
}

IOReturn AIC8800WiFi::setMulticastMode(bool active) {
    (void)active;
    return kIOReturnSuccess;
}

const OSString *AIC8800WiFi::newVendorString() const {
    return OSString::withCString("AICSemi");
}

const OSString *AIC8800WiFi::newModelString() const {
    return OSString::withCString("AIC8800D80 USB Wi-Fi 6");
}
