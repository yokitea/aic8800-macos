#ifndef AIC8800_WIFI_HPP
#define AIC8800_WIFI_HPP

#include <IOKit/IOLib.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/network/IOEthernetController.h>
#include <IOKit/network/IOEthernetInterface.h>
#include <IOKit/network/IONetworkMedium.h>
#include <IOKit/network/IONetworkStats.h>
#include <IOKit/usb/IOUSBHostInterface.h>
#include <IOKit/usb/IOUSBHostPipe.h>

#include "aic8800_abi.h"
#include "aic8800_framing.h"
#include "aic8800_lmac.h"

#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLocks.h>
#include <IOKit/network/IOGatedOutputQueue.h>

class AIC8800WiFi : public IOEthernetController {
    OSDeclareDefaultStructors(AIC8800WiFi)

    using super = IOEthernetController;

public:
    using super::disable;
    using super::enable;
    using super::getHardwareAddress;

    virtual IOOutputQueue *createOutputQueue() APPLE_KEXT_OVERRIDE;

    bool init(OSDictionary *properties = nullptr) override;
    void free() override;
    IOService *probe(IOService *provider, SInt32 *score) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    bool willTerminate(IOService *provider, IOOptionBits options) override;
    bool didTerminate(IOService *provider, IOOptionBits options, bool *defer) override;

    bool createWorkLoop() override;
    IOWorkLoop *getWorkLoop() const override;

    bool configureInterface(IONetworkInterface *interface) override;
    IOReturn enable(IONetworkInterface *netif) override;
    IOReturn disable(IONetworkInterface *netif) override;
    IOReturn getHardwareAddress(IOEthernetAddress *addrP) override;
    UInt32 outputPacket(mbuf_t m, void *param) override;
    IOReturn setPromiscuousMode(bool active) override;
    IOReturn setMulticastMode(bool active) override;

    const OSString *newVendorString() const override;
    const OSString *newModelString() const override;

private:
    static constexpr uint8_t  kWifiInterfaceNumber = 2U;
    static constexpr uint16_t kTxRingSlots         = AIC_USB_TX_URBS;
    static constexpr uint16_t kTxLowWater          = AIC_USB_TX_LOW_WATER;
    static constexpr uint16_t kTxHighWater         = AIC_USB_TX_HIGH_WATER;
    static constexpr uint16_t kRxUrbCount          = AIC_USB_RX_URBS;
    static constexpr uint32_t kRxBufBytes          = AIC_USB_RX_AGGR_MAX_PKT_SIZE;
    static constexpr uint16_t kMsgRxUrbCount       = AIC_USB_MSG_RX_URBS;
    static constexpr uint32_t kMsgRxBufBytes       = AIC_USB_MSG_MAX_PKT_SIZE;
    static constexpr size_t   kTxSlotBufBytes      = AIC_DATA_BUF_MAX;

    struct RxUrbSlot {
        AIC8800WiFi              *owner;
        IOBufferMemoryDescriptor *desc;
        uint8_t                  *buf;
        IOUSBHostCompletion       comp;
        uint16_t                  index;
        bool                      isMsg;
    };

    struct TxUrbSlot {
        AIC8800WiFi              *owner;
        IOBufferMemoryDescriptor *desc;
        uint8_t                  *buf;
        IOUSBHostCompletion       dataComp;
        IOUSBHostCompletion       zlpComp;
        uint16_t                  index;
        uint8_t                   pendingUrbs;
        uint32_t                  bytes;
    };

    static bool isDisabledByBootArg();
    bool discoverBulkPipes();
    void releasePipes();

    bool allocatePools();
    void releasePools();
    void releaseLocks();
    bool startAsyncRx();
    bool submitRxSlot(RxUrbSlot *slot);
    void stopAndDrainIo();
    void wakeCmdWaiters(int32_t statusCode = -3);
    void closeUsbInterface();

    IOReturn sendLmacCmd(uint16_t msgId, uint16_t destId, uint16_t srcId,
                         const void *param, uint16_t paramLen,
                         uint16_t expectedCfmId,
                         void *cfmParamOut, uint16_t cfmMaxLen, uint16_t *cfmActualLenOut,
                         uint32_t timeoutMs);
    bool bringUpLmac();

    size_t encodeEthMbufToSlot(mbuf_t m, uint16_t slotIdx);
    void handleDataRxBuffer(const uint8_t *buf, uint32_t bytesTransferred);
    void handleMsgRxBuffer(const uint8_t *buf, uint32_t bytesTransferred);

    static void onRxComplete(void *owner, void *parameter, IOReturn status, uint32_t bytesTransferred);
    static void onMsgRxComplete(void *owner, void *parameter, IOReturn status, uint32_t bytesTransferred);
    static void onTxComplete(void *owner, void *parameter, IOReturn status, uint32_t bytesTransferred);

    IOUSBHostInterface *usbIf_          = nullptr;
    IOUSBHostPipe      *bulkInPipe_     = nullptr;
    IOUSBHostPipe      *bulkInMsgPipe_  = nullptr;
    IOUSBHostPipe      *bulkOutPipe_    = nullptr;
    IOUSBHostPipe      *bulkOutMsgPipe_ = nullptr;

    IOWorkLoop          *workLoop_   = nullptr;
    IOEthernetInterface *netIf_      = nullptr;
    IONetworkStats      *netStats_   = nullptr;
    OSDictionary        *mediumDict_ = nullptr;

    IOLock       *cmdSerializeLock_ = nullptr;
    IOLock       *cmdWaitLock_      = nullptr;
    IOSimpleLock *txLock_           = nullptr;
    IOLock       *rxDeliverLock_    = nullptr;

    volatile bool isEnabled_  = false;
    volatile bool stopping_   = false;
    uint8_t epInAddr_         = 0;
    uint8_t epInMsgAddr_      = 0;
    uint8_t epOutDataAddr_    = 0;
    uint8_t epOutMsgAddr_     = 0;
    uint16_t maxPacketSizeOut_    = AIC_USB_BULK_MPS_HS;
    uint16_t maxPacketSizeOutMsg_ = AIC_USB_BULK_MPS_HS;

    IOEthernetAddress hwAddr_ = {{0x02, 0xA8, 0x80, 0x00, 0x00, 0x01}};

    uint32_t chipIdReg_     = 0U;
    uint32_t lmacVer_       = 0U;
    uint32_t fwFeatures_    = 0U;
    uint8_t  fwVersion_[64] = {};
    uint8_t  vendorInfo_    = 0U;
    uint8_t  is5gSupported_ = 0U;
    uint8_t  vifIdx_        = 0U;
    bool     lmacReady_     = false;

    aic_fixed_slot_pool_t     txPool_                           = {};
    aic_tx_flow_ctrl_t        txFlowCtrl_                       = {};
    TxUrbSlot                 txSlots_[kTxRingSlots]            = {};
    RxUrbSlot                 rxSlots_[kRxUrbCount]             = {};
    RxUrbSlot                 msgRxSlots_[kMsgRxUrbCount]       = {};
    IOBufferMemoryDescriptor *zlpDesc_                          = nullptr;
    IOBufferMemoryDescriptor *msgOutDesc_                       = nullptr;
    uint8_t                  *msgOutBuf_                        = nullptr;
    aic_lmac_cmd_matcher_t    cmdMatcher_                       = {};
    volatile SInt32           inFlightIoCount_                  = 0;
    uint8_t                   txScratchFrame_[kTxSlotBufBytes]  = {};
    uint8_t                   rxScratchFrame_[kTxSlotBufBytes]  = {};
};

#endif /* AIC8800_WIFI_HPP */
