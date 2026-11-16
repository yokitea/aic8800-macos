#ifndef AIC8800_WIFI_HPP
#define AIC8800_WIFI_HPP

#include <IOKit/IOLib.h>
#include <IOKit/IOTimerEventSource.h>
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
#include "aic8800_wifi_ctl.h"

#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLocks.h>
#include <IOKit/network/IOGatedOutputQueue.h>

class IOUserClient;

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

    IOReturn newUserClient(task_t owningTask,
                           void *securityID,
                           UInt32 type,
                           OSDictionary *properties,
                           IOUserClient **handler) override;

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

    /* Control-plane APIs invoked by privileged AIC8800WiFiUserClient */
    IOReturn ctlGetStatus(aic_wifi_status_t *outStatus);
    IOReturn ctlStartScan(const aic_wifi_scan_req_t *req);
    IOReturn ctlGetScanResults(aic_wifi_scan_list_t *outList);
    IOReturn ctlConnect(const aic_wifi_connect_req_t *inReq, aic_wifi_connect_req_t *outResp);
    IOReturn ctlDisconnect(const aic_wifi_disconnect_req_t *req);
    IOReturn ctlPollEapol(aic_wifi_eapol_frame_t *outFrame);
    IOReturn ctlSendEapol(const aic_wifi_eapol_frame_t *inFrame);
    IOReturn ctlAddKey(const aic_wifi_key_req_t *inReq, aic_wifi_key_req_t *outResp);
    IOReturn ctlSetPortAndLink(const aic_wifi_port_req_t *req);

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
    IOReturn submitRawEthTx(const uint8_t *ethFrame, size_t ethLen, uint8_t staIdx);
    void handleDataRxBuffer(const uint8_t *buf, uint32_t bytesTransferred);
    void handleMsgRxBuffer(const uint8_t *buf, uint32_t bytesTransferred);
    void handleAsyncLmacEvent(uint16_t msgId, const uint8_t *param, uint16_t paramLen);
    void enqueueEapolRx(const uint8_t *ethFrame, size_t ethLen);
    void applyCurrentLinkStatus();

    static void onAsyncLmacCallback(uint16_t msgId, const uint8_t *param, uint16_t paramLen, void *ctx);
    static void onLinkSyncTimer(OSObject *owner, IOTimerEventSource *sender);
    static IOReturn onWorkLoopSyncLink(OSObject *owner, void *arg0, void *arg1, void *arg2, void *arg3);

    static void onRxComplete(void *owner, void *parameter, IOReturn status, uint32_t bytesTransferred);
    static void onMsgRxComplete(void *owner, void *parameter, IOReturn status, uint32_t bytesTransferred);
    static void onTxComplete(void *owner, void *parameter, IOReturn status, uint32_t bytesTransferred);

    IOUSBHostInterface *usbIf_          = nullptr;
    IOUSBHostPipe      *bulkInPipe_     = nullptr;
    IOUSBHostPipe      *bulkInMsgPipe_  = nullptr;
    IOUSBHostPipe      *bulkOutPipe_    = nullptr;
    IOUSBHostPipe      *bulkOutMsgPipe_ = nullptr;

    IOWorkLoop          *workLoop_      = nullptr;
    IOTimerEventSource  *linkSyncTimer_ = nullptr;
    IOEthernetInterface *netIf_         = nullptr;
    IONetworkStats      *netStats_      = nullptr;
    OSDictionary        *mediumDict_    = nullptr;

    IOLock       *cmdSerializeLock_ = nullptr;
    IOLock       *cmdWaitLock_      = nullptr;
    IOLock       *stateLock_        = nullptr;
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

    /* Preallocated scan / connection / EAPOL state protected by stateLock_ */
    bool     scanning_         = false;
    bool     scanDone_         = false;
    uint16_t scanCount_        = 0U;
    aic_wifi_scan_bss_t scanBss_[AIC_WIFI_MAX_SCAN_BSS] = {};

    bool     associated_       = false;
    bool     linkUp_           = false;
    uint8_t  apStaIdx_         = 0U;
    uint8_t  apQos_            = 1U;
    uint8_t  apBand_           = 0U;
    uint8_t  apWidth_          = 0U;
    uint16_t apCenterFreq_     = 0U;
    uint16_t assocStatus_      = 0U;
    uint16_t disconnectReason_ = 0U;
    uint8_t  apBssid_[AIC_MAC_ADDR_LEN] = {};
    uint8_t  targetSsidLen_    = 0U;
    uint8_t  targetSsid_[AIC_MAC_SSID_LEN] = {};

    uint16_t eapolHead_        = 0U;
    uint16_t eapolTail_        = 0U;
    uint16_t eapolCount_       = 0U;
    uint64_t eapolRxTotal_     = 0U;
    aic_wifi_eapol_frame_t eapolRxRing_[AIC_WIFI_EAPOL_RING_CAP] = {};

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
