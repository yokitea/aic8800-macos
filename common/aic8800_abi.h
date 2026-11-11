/*
 * aic8800_abi.h - Bit-exact wire ABI definitions for AICSemi AIC8800D80 (USB)
 *
 * Strictly POD C structs matching Linux aic8800d80 (aic_load_fw + aic8800_fdrv)
 * compiled with CONFIG_RWNX_FULLMAC=y, CONFIG_USER_MAX=1, NX_TXQ_CNT=5, USE_5G=y.
 * Safe for user-space C11/C++17 tests and macOS x86_64 Kernel.framework builds.
 */

#ifndef AIC8800_ABI_H
#define AIC8800_ABI_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "AIC8800 wire ABI helpers assume a little-endian host (x86_64/arm64)."
#endif
#endif

#ifdef __cplusplus
#define AIC_STATIC_ASSERT(cond, msg) static_assert((cond), msg)
#else
#define AIC_STATIC_ASSERT(cond, msg) _Static_assert((cond), msg)
#endif

#ifndef AIC_PACKED
#define AIC_PACKED __attribute__((packed))
#endif

/* --------------------------------------------------------------------------
 * USB Identifiers & Endpoints (AIC8800D80 / EZTECH AX928B)
 * -------------------------------------------------------------------------- */
#define AIC_USB_VID_AICS_BOOT       0xA69CU
#define AIC_USB_VID_AICS_WIFI       0x368BU

#define AIC_USB_PID_MSC_CDROM       0x5721U /* EZTECH AX928B Stage 0 MSC CDROM (VID 0xA69C) */
#define AIC_USB_PID_MSC_ALT         0x5722U
#define AIC_USB_PID_BOOTLOADER      0x8D80U /* Stage 1 Bootloader (VID 0xA69C) */
#define AIC_USB_PID_WIFI_BT_RUNTIME 0x8D81U /* Stage 2 Composite Wi-Fi+BT (VID 0x368B) */

/* USB Mass Storage Bulk-Only Transport (BOT) Eject CBW constants */
#define AIC_USB_BOT_CBW_LEN         31U
#define AIC_USB_BOT_CSW_LEN         13U
#define AIC_USB_BOT_CBW_SIGNATURE   0x43425355U /* "USBC" little-endian */
#define AIC_USB_BOT_CSW_SIGNATURE   0x53425355U /* "USBS" little-endian */

/* Safe Bluetooth dongle spoof target (never spoof 0x0489:0xE0D8 / MTK trap) */
#define AIC_BT_SAFE_SPOOF_VID       0x0B05U /* ASUS USB-BT500 */
#define AIC_BT_SAFE_SPOOF_PID       0x190EU

/* USB wire limits & constants (aicwf_txrxif.h, aicwf_usb.h) */
#define AIC_CMD_BUF_MAX             1536U
#define AIC_DATA_BUF_MAX            2048U
#define AIC_TX_ALIGNMENT            4U
#define AIC_RX_ALIGNMENT            4U
#define AIC_RX_HWHDR_LEN            60U /* sizeof(hw_rxhdr)=56 + 2(msdu_offset) + 2(sdio_align) */
#define AIC_CCMP_OR_WEP_INFO_LEN    8U
#define AIC_USB_AGGR_MAX_PKT_SIZE   (2048U * 10U)
#define AIC_USB_MSG_MAX_PKT_SIZE    2048U
#define AIC_USB_BULK_MPS_HS         512U

/* USB packet type tags in header byte 2 (aicwf_usb.h enum usb_type) */
enum aic_usb_pkt_type {
    AIC_USB_TYPE_DATA         = 0x00U,
    AIC_USB_TYPE_TX_DATA_OUT  = 0x01U, /* outbound TX data header type */
    AIC_USB_TYPE_CFG          = 0x10U,
    AIC_USB_TYPE_CFG_CMD_RSP  = 0x11U,
    AIC_USB_TYPE_CFG_DATA_CFM = 0x12U,
    AIC_USB_TYPE_CFG_PRINT    = 0x13U,
};

/* --------------------------------------------------------------------------
 * Bootloader / Stage 1 Firmware Constants (aic_probe.c / aic_compat_8800d80.c)
 * -------------------------------------------------------------------------- */
#define AIC_CHIP_REV_U01            0x01U
#define AIC_CHIP_REV_U02            0x03U
#define AIC_CHIP_REV_U03            0x07U
#define AIC_CHIP_REV_U04            0x0FU
#define AIC_CHIP_ID_H_MASK          0xC0U

#define AIC_SYS_CTRL_CHIP_ID_REG    0x40500000U
#define AIC_SYS_CTRL_MCU_CFG_REG    0x40100020U

#define AIC_RAM_FMAC_FW_ADDR_U01    0x00100000U
#define AIC_RAM_FMAC_FW_ADDR_U02    0x00120000U
#define AIC_FW_RAM_ADID_ADDR_U01    0x002017E0U
#define AIC_FW_RAM_ADID_ADDR_U02    0x00201940U
#define AIC_FW_RAM_PATCH_ADDR_U01   0x0020B2B0U
#define AIC_FW_RAM_PATCH_ADDR_U02   0x0020B43CU
#define AIC_PATCH_DEFAULT_BUF_U02   0x001D7000U

#define AIC_PATCH_MAGIC_NUM         0x48435450U /* "PTCH" */
#define AIC_PATCH_MAGIC_NUM_2       0x50544348U /* "HCTP" */
#define AICBT_PT_TAG_MAGIC          "AICBT_PT_TAG"

enum aicbt_patch_table_type {
    AICBT_PT_INF    = 0x00U,
    AICBT_PT_TRAP   = 0x01U,
    AICBT_PT_B4     = 0x02U,
    AICBT_PT_BTMODE = 0x03U,
    AICBT_PT_PWRON  = 0x04U,
    AICBT_PT_AF     = 0x05U,
    AICBT_PT_VER    = 0x06U,
};

/* Patch table overrides applied before HOST_START_APP_AUTO (USE_5G=1) */
#define AIC_D80_PATCH_USER_CFG_OFF  0x00B4U
#define AIC_D80_PATCH_USER_CFG_5G   0xF3010001U /* bit0=1 enables 5 GHz band */
#define AIC_D80_PATCH_RX_AGGR_OFF   0x0170U
#define AIC_D80_PATCH_RX_AGGR_VAL   0x0001000AU
#define AIC_D80_PATCH_EXT_FLAGS_OFF 0x0188U
#define AIC_D80_PATCH_EXT_FLAGS_VAL 0x00000001U

enum aic_dbg_start_app_type {
    AIC_HOST_START_APP_AUTO   = 1U,
    AIC_HOST_START_APP_CUSTOM = 2U,
    AIC_HOST_START_APP_REBOOT = 3U,
};

/* --------------------------------------------------------------------------
 * LMAC Tasks & Message IDs (lmac_msg.h, aicbluetooth_cmds.h)
 * -------------------------------------------------------------------------- */
#define AIC_DRV_TASK_ID             100U
#define AIC_LMAC_FIRST_MSG(task)    ((uint16_t)((uint16_t)(task) << 10U))
#define AIC_IPC_MSGE2A_VALID_PAT    0xADDEDE2AU
#define AIC_IPC_DBG_VALID_PAT       0x000CACA0U
#define AIC_RX_DMA_OVER_PAT         0xAAAAAA00U
#define AIC_IPC_E2A_MSG_PARAM_SIZE  256U

enum aic_lmac_task_id {
    AIC_TASK_MM    = 0,
    AIC_TASK_DBG   = 1,
    AIC_TASK_SCAN  = 2,
    AIC_TASK_TDLS  = 3,
    AIC_TASK_SCANU = 4,
    AIC_TASK_ME    = 5,
    AIC_TASK_SM    = 6,
    AIC_TASK_APM   = 7,
    AIC_TASK_BAM   = 8,
    AIC_TASK_MESH  = 9,
    AIC_TASK_RXU   = 10,
    AIC_TASK_RM    = 11,
};

enum aic_dbg_msg_id {
    AIC_DBG_MEM_READ_REQ        = (1U << 10U) + 0,
    AIC_DBG_MEM_READ_CFM        = (1U << 10U) + 1,
    AIC_DBG_MEM_WRITE_REQ       = (1U << 10U) + 2,
    AIC_DBG_MEM_WRITE_CFM       = (1U << 10U) + 3,
    AIC_DBG_SET_MOD_FILTER_REQ  = (1U << 10U) + 4,
    AIC_DBG_SET_MOD_FILTER_CFM  = (1U << 10U) + 5,
    AIC_DBG_SET_SEV_FILTER_REQ  = (1U << 10U) + 6,
    AIC_DBG_SET_SEV_FILTER_CFM  = (1U << 10U) + 7,
    AIC_DBG_ERROR_IND           = (1U << 10U) + 8,
    AIC_DBG_GET_SYS_STAT_REQ    = (1U << 10U) + 9,
    AIC_DBG_GET_SYS_STAT_CFM    = (1U << 10U) + 10,
    AIC_DBG_MEM_BLOCK_WRITE_REQ = (1U << 10U) + 11,
    AIC_DBG_MEM_BLOCK_WRITE_CFM = (1U << 10U) + 12,
    AIC_DBG_START_APP_REQ       = (1U << 10U) + 13,
    AIC_DBG_START_APP_CFM       = (1U << 10U) + 14,
    AIC_DBG_START_NPC_REQ       = (1U << 10U) + 15,
    AIC_DBG_START_NPC_CFM       = (1U << 10U) + 16,
    AIC_DBG_MEM_MASK_WRITE_REQ  = (1U << 10U) + 17,
    AIC_DBG_MEM_MASK_WRITE_CFM  = (1U << 10U) + 18,
};

enum aic_mm_msg_id {
    AIC_MM_RESET_REQ         = 0,
    AIC_MM_RESET_CFM         = 1,
    AIC_MM_START_REQ         = 2,
    AIC_MM_START_CFM         = 3,
    AIC_MM_VERSION_REQ       = 4,
    AIC_MM_VERSION_CFM       = 5,
    AIC_MM_ADD_IF_REQ        = 6,
    AIC_MM_ADD_IF_CFM        = 7,
    AIC_MM_REMOVE_IF_REQ     = 8,
    AIC_MM_REMOVE_IF_CFM     = 9,
    AIC_MM_STA_ADD_REQ       = 10,
    AIC_MM_STA_ADD_CFM       = 11,
    AIC_MM_STA_DEL_REQ       = 12,
    AIC_MM_STA_DEL_CFM       = 13,
    AIC_MM_SET_FILTER_REQ    = 14,
    AIC_MM_SET_FILTER_CFM    = 15,
    AIC_MM_SET_CHANNEL_REQ   = 16,
    AIC_MM_SET_CHANNEL_CFM   = 17,
    AIC_MM_SET_DTIM_REQ      = 18,
    AIC_MM_SET_DTIM_CFM      = 19,
    AIC_MM_SET_BEACON_INT_REQ= 20,
    AIC_MM_SET_BEACON_INT_CFM= 21,
    AIC_MM_SET_BASIC_RATES_REQ= 22,
    AIC_MM_SET_BASIC_RATES_CFM= 23,
    AIC_MM_SET_BSSID_REQ     = 24,
    AIC_MM_SET_BSSID_CFM     = 25,
    AIC_MM_SET_EDCA_REQ      = 26,
    AIC_MM_SET_EDCA_CFM      = 27,
    AIC_MM_SET_MODE_REQ      = 28,
    AIC_MM_SET_MODE_CFM      = 29,
    AIC_MM_SET_VIF_STATE_REQ = 30,
    AIC_MM_SET_VIF_STATE_CFM = 31,
    AIC_MM_SET_SLOTTIME_REQ  = 32,
    AIC_MM_SET_SLOTTIME_CFM  = 33,
    AIC_MM_SET_IDLE_REQ      = 34,
    AIC_MM_SET_IDLE_CFM      = 35,
    AIC_MM_KEY_ADD_REQ       = 36,
    AIC_MM_KEY_ADD_CFM       = 37,
    AIC_MM_KEY_DEL_REQ       = 38,
    AIC_MM_KEY_DEL_CFM       = 39,
    AIC_MM_BA_ADD_REQ        = 40,
    AIC_MM_BA_ADD_CFM        = 41,
    AIC_MM_BA_DEL_REQ        = 42,
    AIC_MM_BA_DEL_CFM        = 43,
    AIC_MM_PRIMARY_TBTT_IND  = 44,
    AIC_MM_SECONDARY_TBTT_IND= 45,
    AIC_MM_SET_POWER_REQ     = 46,
    AIC_MM_SET_POWER_CFM     = 47,
    AIC_MM_DBG_TRIGGER_REQ     = 48,
    AIC_MM_SET_PS_MODE_REQ     = 49,
    AIC_MM_SET_PS_MODE_CFM     = 50,
    AIC_MM_SET_AGG_DISABLE_REQ     = 99,
    AIC_MM_SET_AGG_DISABLE_CFM     = 100,
    AIC_MM_SET_COEX_REQ            = 101,
    AIC_MM_SET_COEX_CFM            = 102,
    AIC_MM_SET_RF_CONFIG_REQ       = 103,
    AIC_MM_SET_RF_CONFIG_CFM       = 104,
    AIC_MM_SET_RF_CALIB_REQ        = 105,
    AIC_MM_SET_RF_CALIB_CFM        = 106,
    AIC_MM_SET_MU_EDCA_REQ         = 107,
    AIC_MM_SET_MU_EDCA_CFM         = 108,
    AIC_MM_SET_UORA_REQ            = 109,
    AIC_MM_SET_UORA_CFM            = 110,
    AIC_MM_SET_TXOP_RTS_THRES_REQ  = 111,
    AIC_MM_SET_TXOP_RTS_THRES_CFM  = 112,
    AIC_MM_SET_BSS_COLOR_REQ       = 113,
    AIC_MM_SET_BSS_COLOR_CFM       = 114,
    AIC_MM_GET_MAC_ADDR_REQ        = 115,
    AIC_MM_GET_MAC_ADDR_CFM        = 116,
    AIC_MM_GET_STA_INFO_REQ        = 117,
    AIC_MM_GET_STA_INFO_CFM        = 118,
    AIC_MM_SET_TXPWR_IDX_LVL_REQ   = 119,
    AIC_MM_SET_TXPWR_IDX_LVL_CFM   = 120,
    AIC_MM_SET_TXPWR_OFST_REQ      = 121,
    AIC_MM_SET_TXPWR_OFST_CFM      = 122,
    AIC_MM_SET_STACK_START_REQ     = 123,
    AIC_MM_SET_STACK_START_CFM     = 124,
    AIC_MM_APM_STALOSS_IND         = 125,
    AIC_MM_SET_VENDOR_HWCONFIG_REQ = 126,
    AIC_MM_SET_VENDOR_HWCONFIG_CFM = 127,
    AIC_MM_GET_FW_VERSION_REQ      = 128,
    AIC_MM_GET_FW_VERSION_CFM      = 129,
    AIC_MM_SET_RESUME_RESTORE_REQ  = 130,
    AIC_MM_SET_RESUME_RESTORE_CFM  = 131,
    AIC_MM_GET_WIFI_DISABLE_REQ    = 132,
    AIC_MM_GET_WIFI_DISABLE_CFM    = 133,
    AIC_MM_CFG_RSSI_CFM            = 134,
    AIC_MM_SET_VENDOR_SWCONFIG_REQ = 135,
    AIC_MM_SET_VENDOR_SWCONFIG_CFM = 136,
    AIC_MM_SET_TXPWR_LVL_ADJ_REQ   = 137,
    AIC_MM_SET_TXPWR_LVL_ADJ_CFM   = 138,
    AIC_MM_RADAR_DETECT_IND        = 139,
    AIC_MM_SET_APF_PROG_REQ        = 140,
    AIC_MM_SET_APF_PROG_CFM        = 141,
    AIC_MM_GET_APF_PROG_REQ        = 142,
    AIC_MM_GET_APF_PROG_CFM        = 143,
    AIC_MM_SET_TXPWR_PER_STA_REQ   = 144,
    AIC_MM_SET_TXPWR_PER_STA_CFM   = 145,
    AIC_MM_GET_STATISTIC_REQ       = 146,
    AIC_MM_GET_STATISTIC_CFM       = 147,
    AIC_MM_VENDOR_SWCONFIG_IND     = 148,
    AIC_MM_FW_PANIC_IND            = 149,
    AIC_MM_FW_ASSERT_IND           = 150,
    AIC_MM_MAX                     = 151,
};

enum aic_scanu_msg_id {
    AIC_SCANU_START_REQ            = (4U << 10U) + 0,
    AIC_SCANU_START_CFM            = (4U << 10U) + 1,
    AIC_SCANU_JOIN_REQ             = (4U << 10U) + 2,
    AIC_SCANU_JOIN_CFM             = (4U << 10U) + 3,
    AIC_SCANU_RESULT_IND           = (4U << 10U) + 4,
    AIC_SCANU_FAST_REQ             = (4U << 10U) + 5,
    AIC_SCANU_FAST_CFM             = (4U << 10U) + 6,
    AIC_SCANU_VENDOR_IE_REQ        = (4U << 10U) + 7,
    AIC_SCANU_VENDOR_IE_CFM        = (4U << 10U) + 8,
    AIC_SCANU_START_CFM_ADDTIONAL  = (4U << 10U) + 9,
    AIC_SCANU_CANCEL_REQ           = (4U << 10U) + 10,
    AIC_SCANU_CANCEL_CFM           = (4U << 10U) + 11,
};

enum aic_me_msg_id {
    AIC_ME_CONFIG_REQ           = (5U << 10U) + 0,
    AIC_ME_CONFIG_CFM           = (5U << 10U) + 1,
    AIC_ME_CHAN_CONFIG_REQ      = (5U << 10U) + 2,
    AIC_ME_CHAN_CONFIG_CFM      = (5U << 10U) + 3,
    AIC_ME_SET_CONTROL_PORT_REQ = (5U << 10U) + 4,
    AIC_ME_SET_CONTROL_PORT_CFM = (5U << 10U) + 5,
    AIC_ME_TKIP_MIC_FAILURE_IND = (5U << 10U) + 6,
    AIC_ME_STA_ADD_REQ          = (5U << 10U) + 7,
    AIC_ME_STA_ADD_CFM          = (5U << 10U) + 8,
    AIC_ME_STA_DEL_REQ          = (5U << 10U) + 9,
    AIC_ME_STA_DEL_CFM          = (5U << 10U) + 10,
    AIC_ME_TX_CREDITS_UPDATE_IND= (5U << 10U) + 11,
    AIC_ME_TRAFFIC_IND_REQ      = (5U << 10U) + 12,
    AIC_ME_TRAFFIC_IND_CFM      = (5U << 10U) + 13,
    AIC_ME_RC_STATS_REQ         = (5U << 10U) + 14,
    AIC_ME_RC_STATS_CFM         = (5U << 10U) + 15,
    AIC_ME_RC_SET_RATE_REQ      = (5U << 10U) + 16,
    AIC_ME_CONFIG_MONITOR_REQ   = (5U << 10U) + 17,
    AIC_ME_CONFIG_MONITOR_CFM   = (5U << 10U) + 18,
    AIC_ME_SET_PS_MODE_REQ      = (5U << 10U) + 19,
    AIC_ME_SET_PS_MODE_CFM      = (5U << 10U) + 20,
    AIC_ME_SET_LP_LEVEL_REQ     = (5U << 10U) + 21,
    AIC_ME_SET_LP_LEVEL_CFM     = (5U << 10U) + 22,
};

enum aic_sm_msg_id {
    AIC_SM_CONNECT_REQ                  = (6U << 10U) + 0,
    AIC_SM_CONNECT_CFM                  = (6U << 10U) + 1,
    AIC_SM_CONNECT_IND                  = (6U << 10U) + 2,
    AIC_SM_DISCONNECT_REQ               = (6U << 10U) + 3,
    AIC_SM_DISCONNECT_CFM               = (6U << 10U) + 4,
    AIC_SM_DISCONNECT_IND               = (6U << 10U) + 5,
    AIC_SM_EXTERNAL_AUTH_REQUIRED_IND   = (6U << 10U) + 6,
    AIC_SM_EXTERNAL_AUTH_REQUIRED_RSP   = (6U << 10U) + 7,
    AIC_SM_FT_AUTH_IND                  = (6U << 10U) + 8,
    AIC_SM_FT_AUTH_RSP                  = (6U << 10U) + 9,
    AIC_SM_RSP_TIMEOUT_IND              = (6U << 10U) + 10,
    AIC_SM_COEX_TS_TIMEOUT_IND          = (6U << 10U) + 11,
    AIC_SM_EXTERNAL_AUTH_REQUIRED_RSP_CFM = (6U << 10U) + 12,
};

/* --------------------------------------------------------------------------
 * MAC Primitives & Limits (lmac_mac.h, rwnx_tx.h, rwnx_rx.h)
 * -------------------------------------------------------------------------- */
#define AIC_MAC_ADDR_LEN             6U
#define AIC_MAC_SSID_LEN             32U
#define AIC_MAC_DOMAINCHANNEL_24G_MAX 14U
#define AIC_MAC_DOMAINCHANNEL_5G_MAX  28U
#define AIC_SCAN_CHANNEL_MAX         (AIC_MAC_DOMAINCHANNEL_24G_MAX + AIC_MAC_DOMAINCHANNEL_5G_MAX)
#define AIC_SCAN_SSID_MAX            3U
#define AIC_MAC_RATESET_LEN          12U
#define AIC_MAC_SEC_KEY_LEN          32U
#define AIC_MAX_MCS_LEN              16U
#define AIC_MAC_HE_MAC_CAPA_LEN      6U
#define AIC_MAC_HE_PHY_CAPA_LEN      11U
#define AIC_MAC_HE_PPE_THRES_MAX_LEN 25U
#define AIC_AC_MAX                   4U
#define AIC_PHY_CFG_BUF_SIZE         16U
#define AIC_SM_ASSOC_IE_LEN          800U

enum aic_mac_vif_type {
    AIC_VIF_STA = 0,
    AIC_VIF_IBSS = 1,
    AIC_VIF_AP = 2,
    AIC_VIF_MESH_POINT = 3,
    AIC_VIF_MONITOR = 4,
    AIC_VIF_UNKNOWN = 5,
};

enum aic_mac_chan_band {
    AIC_PHY_BAND_2G4 = 0,
    AIC_PHY_BAND_5G  = 1,
    AIC_PHY_BAND_MAX = 2,
};

enum aic_mac_chan_bw {
    AIC_PHY_CHNL_BW_20 = 0,
    AIC_PHY_CHNL_BW_40 = 1,
    AIC_PHY_CHNL_BW_80 = 2,
    AIC_PHY_CHNL_BW_160 = 3,
    AIC_PHY_CHNL_BW_80P80 = 4,
    AIC_PHY_CHNL_BW_OTHER = 5,
};

enum aic_mac_chan_flags {
    AIC_CHAN_NO_IR    = (1U << 0U),
    AIC_CHAN_DISABLED = (1U << 1U),
    AIC_CHAN_RADAR    = (1U << 2U),
};

enum aic_txu_cntrl_flags {
    AIC_TXU_CNTRL_RETRY       = (1U << 0U),
    AIC_TXU_CNTRL_MORE_DATA   = (1U << 2U),
    AIC_TXU_CNTRL_MGMT        = (1U << 3U),
    AIC_TXU_CNTRL_MGMT_NO_CCK = (1U << 4U),
    AIC_TXU_CNTRL_AMSDU       = (1U << 6U),
    AIC_TXU_CNTRL_MGMT_ROBUST = (1U << 7U),
    AIC_TXU_CNTRL_USE_4ADDR   = (1U << 8U),
    AIC_TXU_CNTRL_EOSP        = (1U << 9U),
    AIC_TXU_CNTRL_MESH_FWD    = (1U << 10U),
    AIC_TXU_CNTRL_TDLS        = (1U << 11U),
};

enum aic_rx_hd_decr_status {
    AIC_RX_HD_DECR_UNENC   = 0U,
    AIC_RX_HD_DECR_WEP     = 1U,
    AIC_RX_HD_DECR_TKIP    = 2U,
    AIC_RX_HD_DECR_CCMP128 = 3U,
    AIC_RX_HD_DECR_CCMP256 = 4U,
    AIC_RX_HD_DECR_GCMP128 = 5U,
    AIC_RX_HD_DECR_GCMP256 = 6U,
    AIC_RX_HD_DECR_WAPI    = 7U,
};

enum aic_mac_connection_flags {
    AIC_CONTROL_PORT_HOST   = (1U << 0U),
    AIC_CONTROL_PORT_NO_ENC = (1U << 1U),
    AIC_DISABLE_HT          = (1U << 2U),
    AIC_WPA_WPA2_IN_USE     = (1U << 3U),
    AIC_MFP_IN_USE          = (1U << 4U),
    AIC_REASSOCIATION       = (1U << 5U),
};

/* --------------------------------------------------------------------------
 * POD Wire Structures
 * -------------------------------------------------------------------------- */
typedef struct {
    uint16_t array[AIC_MAC_ADDR_LEN / 2U];
} aic_mac_addr_t;

typedef struct {
    uint8_t length;
    uint8_t array[AIC_MAC_SSID_LEN];
} aic_mac_ssid_t;

typedef struct {
    uint16_t freq;
    uint8_t  band;
    uint8_t  flags;
    int8_t   tx_power;
} aic_mac_chan_def_t;

typedef struct {
    uint8_t  band;
    uint8_t  type;
    uint16_t prim20_freq;
    uint16_t center1_freq;
    uint16_t center2_freq;
    int8_t   tx_power;
    uint8_t  flags;
} aic_mac_chan_op_t;

typedef struct {
    uint8_t  length;
    uint32_t array[AIC_MAC_SEC_KEY_LEN / 4U];
} aic_mac_sec_key_t;

typedef struct {
    uint16_t ht_capa_info;
    uint8_t  a_mpdu_param;
    uint8_t  mcs_rate[AIC_MAX_MCS_LEN];
    uint16_t ht_extended_capa;
    uint32_t tx_beamforming_capa;
    uint8_t  asel_capa;
} aic_mac_htcapability_t;

typedef struct {
    uint32_t vht_capa_info;
    uint16_t rx_mcs_map;
    uint16_t rx_highest;
    uint16_t tx_mcs_map;
    uint16_t tx_highest;
} aic_mac_vhtcapability_t;

typedef struct {
    uint16_t rx_mcs_80;
    uint16_t tx_mcs_80;
    uint16_t rx_mcs_160;
    uint16_t tx_mcs_160;
    uint16_t rx_mcs_80p80;
    uint16_t tx_mcs_80p80;
} aic_mac_he_mcs_nss_supp_t;

typedef struct {
    uint8_t                   mac_cap_info[AIC_MAC_HE_MAC_CAPA_LEN];
    uint8_t                   phy_cap_info[AIC_MAC_HE_PHY_CAPA_LEN];
    aic_mac_he_mcs_nss_supp_t mcs_supp;
    uint8_t                   ppe_thres[AIC_MAC_HE_PPE_THRES_MAX_LEN];
} aic_mac_hecapability_t;

/* TX Descriptor (ipc_shared.h struct hostdesc / txdesc_api / txdesc_host) */
typedef struct {
    uint16_t       packet_len;
    uint16_t       flags_ext;
    uint32_t       status_desc_addr;
    aic_mac_addr_t eth_dest_addr;
    aic_mac_addr_t eth_src_addr;
    uint16_t       ethertype; /* network byte order (big-endian) */
    uint8_t        ac;
    uint8_t        tid;
    uint8_t        vif_idx;
    uint8_t        staid;
    uint16_t       flags;
} aic_hostdesc_t;

typedef struct {
    aic_hostdesc_t host;
} aic_txdesc_api_t;

typedef struct {
    uint32_t         ready;
    aic_txdesc_api_t api;
} aic_txdesc_host_t;

/* RX Hardware Vectors & Header (rwnx_rx.h) */
typedef struct AIC_PACKED {
    uint8_t dyn_bw_in_non_ht : 1;
    uint8_t chn_bw_in_non_ht : 2;
    uint8_t rsvd_nht         : 4;
    uint8_t lsig_valid       : 1;
} aic_rx_leg_vect_t;

typedef struct AIC_PACKED {
    uint16_t sounding    : 1;
    uint16_t smoothing   : 1;
    uint16_t short_gi    : 1;
    uint16_t aggregation : 1;
    uint16_t stbc        : 1;
    uint16_t num_extn_ss : 2;
    uint16_t lsig_valid  : 1;
    uint16_t mcs         : 7;
    uint16_t fec         : 1;
    uint16_t length      : 16;
} aic_rx_ht_vect_t;

typedef struct AIC_PACKED {
    uint8_t  sounding         : 1;
    uint8_t  beamformed       : 1;
    uint8_t  short_gi         : 1;
    uint8_t  rsvd_vht1        : 1;
    uint8_t  stbc             : 1;
    uint8_t  doze_not_allowed : 1;
    uint8_t  first_user       : 1;
    uint8_t  rsvd_vht2        : 1;
    uint16_t partial_aid      : 9;
    uint16_t group_id         : 6;
    uint16_t rsvd_vht3        : 1;
    uint32_t mcs              : 4;
    uint32_t nss              : 3;
    uint32_t fec              : 1;
    uint32_t length           : 20;
    uint32_t rsvd_vht4        : 4;
} aic_rx_vht_vect_t;

typedef struct AIC_PACKED {
    uint8_t  sounding        : 1;
    uint8_t  beamformed      : 1;
    uint8_t  gi_type         : 2;
    uint8_t  stbc            : 1;
    uint8_t  rsvd_he1        : 3;

    uint8_t  uplink_flag     : 1;
    uint8_t  beam_change     : 1;
    uint8_t  dcm             : 1;
    uint8_t  he_ltf_type     : 2;
    uint8_t  doppler         : 1;
    uint8_t  rsvd_he2        : 2;

    uint8_t  bss_color       : 6;
    uint8_t  rsvd_he3        : 2;

    uint8_t  txop_duration   : 7;
    uint8_t  rsvd_he4        : 1;

    uint8_t  pe_duration     : 4;
    uint8_t  spatial_reuse   : 4;

    uint8_t  sig_b_comp_mode : 1;
    uint8_t  dcm_sig_b       : 1;
    uint8_t  mcs_sig_b       : 3;
    uint8_t  ru_size         : 3;

    uint32_t mcs             : 4;
    uint32_t nss             : 3;
    uint32_t fec             : 1;
    uint32_t length          : 20;
    uint32_t rsvd_he6        : 4;
} aic_rx_he_vect_t;

typedef struct AIC_PACKED {
    uint8_t  format_mod  : 4;
    uint8_t  ch_bw       : 3;
    uint8_t  pre_type    : 1;
    uint8_t  antenna_set : 8;
    int32_t  rssi_leg    : 8;
    uint32_t leg_length  : 12;
    uint32_t leg_rate    : 4;
    int32_t  rssi1       : 8;

    union {
        aic_rx_leg_vect_t leg;
        aic_rx_ht_vect_t  ht;
        aic_rx_vht_vect_t vht;
        aic_rx_he_vect_t  he;
    };
} aic_rx_vector_1_t;

typedef struct {
    uint32_t rcpi1 : 8;
    uint32_t rcpi2 : 8;
    uint32_t rcpi3 : 8;
    uint32_t rcpi4 : 8;

    uint32_t evm1  : 8;
    uint32_t evm2  : 8;
    uint32_t evm3  : 8;
    uint32_t evm4  : 8;
} aic_rx_vector_2_t;

typedef struct {
    uint32_t phy_band         : 8;
    uint32_t phy_channel_type : 8;
    uint32_t phy_prim20_freq  : 16;

    uint32_t phy_center1_freq : 16;
    uint32_t phy_center2_freq : 16;
} aic_phy_channel_info_desc_t;

typedef struct {
    uint32_t          len       : 16;
    uint32_t          reserved  : 8;
    uint32_t          mpdu_cnt  : 6;
    uint32_t          ampdu_cnt : 2;

    uint32_t          tsf_lo;
    uint32_t          tsf_hi;

    aic_rx_vector_1_t rx_vect1;
    aic_rx_vector_2_t rx_vect2;

    uint32_t          rx_vect2_valid    : 1;
    uint32_t          resp_frame        : 1;
    uint32_t          decr_status       : 3;
    uint32_t          rx_fifo_oflow     : 1;
    uint32_t          undef_err         : 1;
    uint32_t          phy_err           : 1;
    uint32_t          fcs_err           : 1;
    uint32_t          addr_mismatch     : 1;
    uint32_t          ga_frame          : 1;
    uint32_t          current_ac        : 2;
    uint32_t          frm_successful_rx : 1;
    uint32_t          desc_done_rx      : 1;
    uint32_t          key_sram_index    : 10;
    uint32_t          key_sram_v        : 1;
    uint32_t          type              : 2;
    uint32_t          subtype           : 4;
} aic_hw_vect_t;

typedef struct {
    aic_hw_vect_t               hwvect;
    aic_phy_channel_info_desc_t phy_info;

    uint32_t                    flags_is_amsdu      : 1;
    uint32_t                    flags_is_80211_mpdu : 1;
    uint32_t                    flags_is_4addr      : 1;
    uint32_t                    flags_new_peer      : 1;
    uint32_t                    flags_user_prio     : 1;
    uint32_t                    flags_need_reord    : 1;
    uint32_t                    flags_upload        : 1;
    uint32_t                    flags_rsvd0         : 1;
    uint32_t                    flags_vif_idx       : 8;
    uint32_t                    flags_sta_idx       : 8;
    uint32_t                    flags_dst_idx       : 8;

    uint32_t                    pattern;
} aic_hw_rxhdr_t;

/* Message Envelope Structures */
typedef struct {
    uint16_t id;
    uint16_t dest_id;
    uint16_t src_id;
    uint16_t param_len;
} aic_lmac_msg_hdr_t;

typedef struct {
    uint16_t id;
    uint16_t dummy_dest_id;
    uint16_t dummy_src_id;
    uint16_t param_len;
    uint32_t pattern;
} aic_ipc_e2a_msg_hdr_t;

typedef struct {
    uint16_t id;
    uint16_t dummy_dest_id;
    uint16_t dummy_src_id;
    uint16_t param_len;
    uint32_t pattern;
    uint32_t param[AIC_IPC_E2A_MSG_PARAM_SIZE];
} aic_ipc_e2a_msg_t;

/* Stage 1 Patch & Debug Structures */
typedef struct {
    uint32_t magic_num;
    uint32_t pair_start;
    uint32_t magic_num_2;
    uint32_t pair_count;
    uint32_t block_dst[4];
    uint32_t block_src[4];
    uint32_t block_size[4];
} aic_patch_t;

typedef struct {
    uint32_t memaddr;
} aic_dbg_mem_read_req_t;

typedef struct {
    uint32_t memaddr;
    uint32_t memdata;
} aic_dbg_mem_read_cfm_t;

typedef struct {
    uint32_t memaddr;
    uint32_t memdata;
} aic_dbg_mem_write_req_t;

typedef struct {
    uint32_t memaddr;
    uint32_t memdata;
} aic_dbg_mem_write_cfm_t;

typedef struct {
    uint32_t memaddr;
    uint32_t memmask;
    uint32_t memdata;
} aic_dbg_mem_mask_write_req_t;

typedef struct {
    uint32_t memaddr;
    uint32_t memsize;
    uint32_t memdata[256];
} aic_dbg_mem_block_write_req_t;

typedef struct {
    uint32_t wstatus;
} aic_dbg_mem_block_write_cfm_t;

typedef struct {
    uint32_t bootaddr;
    uint32_t boottype;
} aic_dbg_start_app_req_t;

typedef struct {
    uint32_t bootstatus;
} aic_dbg_start_app_cfm_t;

/* LMAC / MM / SCANU / ME / SM Payload Structures */
typedef struct {
    uint32_t parameters[AIC_PHY_CFG_BUF_SIZE];
} aic_phy_cfg_tag_t;

typedef struct {
    aic_phy_cfg_tag_t phy_cfg;
    uint32_t          uapsd_timeout;
    uint16_t          lp_clk_accuracy;
} aic_mm_start_req_t;

typedef struct {
    aic_mac_chan_op_t chan;
    uint8_t           index;
} aic_mm_set_channel_req_t;

typedef struct {
    uint8_t radio_idx;
    int8_t  power;
} aic_mm_set_channel_cfm_t;

typedef struct {
    uint8_t        type;
    aic_mac_addr_t addr;
    uint8_t        p2p; /* bool_l */
} aic_mm_add_if_req_t;

typedef struct {
    uint8_t status;
    uint8_t inst_nbr;
} aic_mm_add_if_cfm_t;

typedef struct {
    uint8_t inst_nbr;
} aic_mm_remove_if_req_t;

typedef struct {
    uint16_t aid;
    uint8_t  active; /* bool_l */
    uint8_t  inst_nbr;
} aic_mm_set_vif_state_req_t;

typedef struct {
    uint32_t version_lmac;
    uint32_t version_machw_1;
    uint32_t version_machw_2;
    uint32_t version_phy_1;
    uint32_t version_phy_2;
    uint32_t features;
    uint16_t max_sta_nb;
    uint8_t  max_vif_nb;
} aic_mm_version_cfm_t;

typedef struct {
    uint8_t           key_idx;
    uint8_t           sta_idx;
    aic_mac_sec_key_t key;
    uint8_t           cipher_suite;
    uint8_t           inst_nbr;
    uint8_t           spp;
    uint8_t           pairwise; /* bool_l */
} aic_mm_key_add_req_t;

typedef struct {
    uint8_t status;
    uint8_t hw_key_idx;
} aic_mm_key_add_cfm_t;

typedef struct {
    uint8_t hw_key_idx;
} aic_mm_key_del_req_t;

typedef struct {
    uint8_t new_state;
} aic_mm_set_ps_mode_req_t;

typedef struct {
    uint8_t disable;
    uint8_t staidx;
    uint8_t disable_rx;
} aic_mm_set_agg_disable_req_t;

typedef struct {
    uint8_t  bt_on;
    uint8_t  disable_coexnull;
    uint8_t  enable_nullcts;
    uint8_t  enable_periodic_timer;
    uint8_t  coex_timeslot_set;
    uint32_t coex_timeslot[2];
} aic_mm_set_coex_req_t;

typedef struct {
    aic_mac_chan_def_t chan[AIC_SCAN_CHANNEL_MAX];
    aic_mac_ssid_t     ssid[AIC_SCAN_SSID_MAX];
    aic_mac_addr_t     bssid;
    uint32_t           add_ies;
    uint16_t           add_ie_len;
    uint8_t            vif_idx;
    uint8_t            chan_cnt;
    uint8_t            ssid_cnt;
    uint8_t            no_cck; /* bool */
    uint32_t           duration;
} aic_scanu_start_req_t;

typedef struct {
    uint8_t vif_idx;
    uint8_t status;
    uint8_t result_cnt;
} aic_scanu_start_cfm_t;

typedef struct {
    uint16_t length;
    uint16_t framectrl;
    uint16_t center_freq;
    uint8_t  band;
    uint8_t  sta_idx;
    uint8_t  inst_nbr;
    int8_t   rssi;
    uint8_t  reserved[2];
} aic_scanu_result_ind_hdr_t;

typedef struct {
    aic_mac_htcapability_t  ht_cap;
    aic_mac_vhtcapability_t vht_cap;
    aic_mac_hecapability_t  he_cap;
    uint16_t                tx_lft;
    uint8_t                 phy_bw_max;
    uint8_t                 ht_supp;
    uint8_t                 vht_supp;
    uint8_t                 he_supp;
    uint8_t                 he_ul_on;
    uint8_t                 ps_on;
    uint8_t                 ant_div_on;
    uint8_t                 dpsm;
} aic_me_config_req_t;

typedef struct {
    aic_mac_chan_def_t chan2G4[AIC_MAC_DOMAINCHANNEL_24G_MAX];
    aic_mac_chan_def_t chan5G[AIC_MAC_DOMAINCHANNEL_5G_MAX];
    uint8_t            chan2G4_cnt;
    uint8_t            chan5G_cnt;
} aic_me_chan_config_req_t;

typedef struct {
    uint8_t sta_idx;
    uint8_t control_port_open; /* bool_l */
} aic_me_set_control_port_req_t;

typedef struct {
    uint8_t  sta_idx;
    uint16_t fixed_rate_cfg;
} aic_me_rc_set_rate_req_t;

typedef struct {
    aic_mac_ssid_t     ssid;
    aic_mac_addr_t     bssid;
    aic_mac_chan_def_t chan;
    uint32_t           flags;
    uint16_t           ctrl_port_ethertype;
    uint16_t           ie_len;
    uint16_t           listen_interval;
    uint8_t            dont_wait_bcmc;
    uint8_t            auth_type;
    uint8_t            uapsd_queues;
    uint8_t            vif_idx;
    uint32_t           ie_buf[64];
} aic_sm_connect_req_t;

typedef struct {
    uint8_t status;
} aic_sm_connect_cfm_t;

typedef struct {
    uint16_t       status_code;
    aic_mac_addr_t bssid;
    uint8_t        roamed;
    uint8_t        vif_idx;
    uint8_t        ap_idx;
    uint8_t        ch_idx;
    uint8_t        qos;
    uint8_t        acm;
    uint16_t       assoc_req_ie_len;
    uint16_t       assoc_rsp_ie_len;
    uint32_t       assoc_ie_buf[AIC_SM_ASSOC_IE_LEN / 4U];
    uint16_t       aid;
    uint8_t        band;
    uint16_t       center_freq;
    uint8_t        width;
    uint32_t       center_freq1;
    uint32_t       center_freq2;
    uint32_t       ac_param[AIC_AC_MAX];
} aic_sm_connect_ind_t;

typedef struct {
    uint16_t reason_code;
    uint8_t  vif_idx;
} aic_sm_disconnect_req_t;

typedef struct {
    uint16_t reason_code;
    uint8_t  vif_idx;
    uint8_t  ft_over_ds;
    uint8_t  reassoc;
} aic_sm_disconnect_ind_t;

typedef struct {
    uint8_t        vif_idx;
    aic_mac_ssid_t ssid;
    aic_mac_addr_t bssid;
    uint32_t       akm;
} aic_sm_external_auth_required_ind_t;

typedef struct {
    uint8_t  vif_idx;
    uint16_t status;
} aic_sm_external_auth_required_rsp_t;

/* --------------------------------------------------------------------------
 * Compile-Time ABI Layout Assertions
 * -------------------------------------------------------------------------- */
AIC_STATIC_ASSERT(sizeof(bool) == 1, "bool_l requires 1-byte bool");
AIC_STATIC_ASSERT(sizeof(aic_mac_addr_t) == 6, "mac_addr size");
AIC_STATIC_ASSERT(sizeof(aic_mac_ssid_t) == 33, "mac_ssid size");
AIC_STATIC_ASSERT(sizeof(aic_mac_chan_def_t) == 6, "mac_chan_def size");
AIC_STATIC_ASSERT(sizeof(aic_mac_chan_op_t) == 10, "mac_chan_op size");
AIC_STATIC_ASSERT(sizeof(aic_mac_sec_key_t) == 36, "mac_sec_key size");
AIC_STATIC_ASSERT(sizeof(aic_mac_htcapability_t) == 32, "mac_htcapability size");
AIC_STATIC_ASSERT(sizeof(aic_mac_vhtcapability_t) == 12, "mac_vhtcapability size");
AIC_STATIC_ASSERT(sizeof(aic_mac_hecapability_t) == 56, "mac_hecapability size");

AIC_STATIC_ASSERT(sizeof(aic_hostdesc_t) == 28, "hostdesc wire size");
AIC_STATIC_ASSERT(sizeof(aic_txdesc_api_t) == 28, "txdesc_api wire size");
AIC_STATIC_ASSERT(sizeof(aic_txdesc_host_t) == 32, "txdesc_host wire size");
AIC_STATIC_ASSERT(offsetof(aic_hostdesc_t, eth_dest_addr) == 8, "hostdesc eth_dest_addr off");
AIC_STATIC_ASSERT(offsetof(aic_hostdesc_t, eth_src_addr) == 14, "hostdesc eth_src_addr off");
AIC_STATIC_ASSERT(offsetof(aic_hostdesc_t, ethertype) == 20, "hostdesc ethertype off");
AIC_STATIC_ASSERT(offsetof(aic_hostdesc_t, ac) == 22, "hostdesc ac off");
AIC_STATIC_ASSERT(offsetof(aic_hostdesc_t, staid) == 25, "hostdesc staid off");
AIC_STATIC_ASSERT(offsetof(aic_hostdesc_t, flags) == 26, "hostdesc flags off");

AIC_STATIC_ASSERT(sizeof(aic_rx_leg_vect_t) == 1, "rx_leg_vect size");
AIC_STATIC_ASSERT(sizeof(aic_rx_ht_vect_t) == 4, "rx_ht_vect size");
AIC_STATIC_ASSERT(sizeof(aic_rx_vht_vect_t) == 7, "rx_vht_vect size");
AIC_STATIC_ASSERT(sizeof(aic_rx_he_vect_t) == 10, "rx_he_vect size");
AIC_STATIC_ASSERT(sizeof(aic_rx_vector_1_t) == 16, "rx_vector_1 size");
AIC_STATIC_ASSERT(sizeof(aic_rx_vector_2_t) == 8, "rx_vector_2 size");
AIC_STATIC_ASSERT(sizeof(aic_phy_channel_info_desc_t) == 8, "phy_channel_info_desc size");
AIC_STATIC_ASSERT(sizeof(aic_hw_vect_t) == 40, "hw_vect size");
AIC_STATIC_ASSERT(offsetof(aic_hw_vect_t, rx_vect2) == 28, "hw_vect rx_vect2 off");
AIC_STATIC_ASSERT(sizeof(aic_hw_rxhdr_t) == 56, "hw_rxhdr size");
AIC_STATIC_ASSERT(offsetof(aic_hw_rxhdr_t, pattern) == 52, "hw_rxhdr pattern off");

AIC_STATIC_ASSERT(sizeof(aic_lmac_msg_hdr_t) == 8, "lmac_msg hdr size");
AIC_STATIC_ASSERT(sizeof(aic_ipc_e2a_msg_hdr_t) == 12, "ipc_e2a_msg hdr size");
AIC_STATIC_ASSERT(sizeof(aic_ipc_e2a_msg_t) == 12 + 1024, "ipc_e2a_msg total size");
AIC_STATIC_ASSERT(sizeof(aic_patch_t) == 64, "aic_patch_t size");
AIC_STATIC_ASSERT(sizeof(aic_dbg_mem_block_write_req_t) == 1032, "dbg_mem_block_write_req size");
AIC_STATIC_ASSERT(sizeof(aic_dbg_start_app_req_t) == 8, "dbg_start_app_req size");

AIC_STATIC_ASSERT(sizeof(aic_mm_start_req_t) == 72, "mm_start_req size");
AIC_STATIC_ASSERT(sizeof(aic_mm_set_channel_req_t) == 12, "mm_set_channel_req size");
AIC_STATIC_ASSERT(sizeof(aic_mm_add_if_req_t) == 10, "mm_add_if_req size");
AIC_STATIC_ASSERT(sizeof(aic_mm_version_cfm_t) == 28, "mm_version_cfm size");
AIC_STATIC_ASSERT(sizeof(aic_mm_key_add_req_t) == 44, "mm_key_add_req size");
AIC_STATIC_ASSERT(sizeof(aic_mm_set_coex_req_t) == 16, "mm_set_coex_req size");
AIC_STATIC_ASSERT(sizeof(aic_scanu_result_ind_hdr_t) == 12, "scanu_result_ind hdr size");

/* Phase 1: Stage 2 D80 LMAC, RF Calibration, Power & USB Transfer Constants */
#define AIC_DBG_MEM_ADDR_CHIP_ID      0x40500000U
#define AIC_D80_CAL_CFG_24G           0x0F8FU
#define AIC_D80_CAL_CFG_5G            0x0F0FU
#define AIC_D80_PARAM_ALPHA           0x0C34C008U
#define AIC_D80_BT_CALIB_PARAM        0x00264203U
#define AIC_STACK_START_VENDOR_BIT    (1U << 5U)

#define AIC_USB_RX_URBS               20U
#define AIC_USB_RX_AGGR_MAX_PKT_SIZE  (2048U * 10U)
#define AIC_USB_MSG_RX_URBS           16U
#define AIC_USB_MSG_MAX_PKT_SIZE      2048U
#define AIC_USB_TX_URBS               200U
#define AIC_USB_TX_LOW_WATER          50U
#define AIC_USB_TX_HIGH_WATER         150U
#define AIC_CMD_TX_TIMEOUT_MS         5000U

#define AIC_RWNX_TX_LIFETIME_MS       1000U
#define AIC_RWNX_UAPSD_TIMEOUT_MS     300U
#define AIC_RWNX_LP_CLK_PPM           20U

/* Phase 1: LMAC Stage 2 Request & Confirmation Wire Structures */
typedef struct {
    uint32_t cal_cfg_24g;
    uint32_t cal_cfg_5g;
    uint32_t param_alpha;
    uint32_t bt_calib_en;
    uint32_t bt_calib_param;
    uint8_t  xtal_cap;
    uint8_t  xtal_cap_fine;
} aic_mm_set_rf_calib_req_t;

typedef struct {
    uint32_t rxgain_24g_addr;
    uint32_t rxgain_5g_addr;
    uint32_t txgain_24g_addr;
    uint32_t txgain_5g_addr;
} aic_mm_set_rf_calib_cfm_t;

typedef struct {
    uint32_t get;
} aic_mm_get_mac_addr_req_t;

typedef struct {
    uint8_t mac_addr[6];
} aic_mm_get_mac_addr_cfm_t;

typedef struct {
    uint8_t enable;
    uint8_t dsss;
    uint8_t ofdmlowrate_2g4;
    uint8_t ofdm64qam_2g4;
    uint8_t ofdm256qam_2g4;
    uint8_t ofdm1024qam_2g4;
    uint8_t ofdmlowrate_5g;
    uint8_t ofdm64qam_5g;
    uint8_t ofdm256qam_5g;
    uint8_t ofdm1024qam_5g;
} aic_txpwr_lvl_conf_t;

typedef struct {
    uint8_t enable;
    int8_t  pwrlvl_11b_11ag_2g4[12];
    int8_t  pwrlvl_11n_11ac_2g4[10];
    int8_t  pwrlvl_11ax_2g4[12];
} aic_txpwr_lvl_conf_v2_t;

typedef struct {
    uint8_t enable;
    int8_t  pwrlvl_11b_11ag_2g4[12];
    int8_t  pwrlvl_11n_11ac_2g4[10];
    int8_t  pwrlvl_11ax_2g4[12];
    int8_t  pwrlvl_11a_5g[12];
    int8_t  pwrlvl_11n_11ac_5g[10];
    int8_t  pwrlvl_11ax_5g[12];
} aic_txpwr_lvl_conf_v3_t;

typedef struct {
    uint8_t enable;
    int8_t  pwrlvl_11b_11ag_2g4[12];
    int8_t  pwrlvl_11n_11ac_2g4[10];
    int8_t  pwrlvl_11ax_2g4[12];
    int8_t  pwrlvl_11a_5g[8];
    int8_t  pwrlvl_11n_11ac_5g[10];
    int8_t  pwrlvl_11ax_5g[12];
    int8_t  pwrlvl_11a_6g[8];
    int8_t  pwrlvl_11n_11ac_6g[10];
    int8_t  pwrlvl_11ax_6g[12];
} aic_txpwr_lvl_conf_v4_t;

typedef struct {
    union {
        aic_txpwr_lvl_conf_t    txpwr_lvl;
        aic_txpwr_lvl_conf_v2_t txpwr_lvl_v2;
        aic_txpwr_lvl_conf_v3_t txpwr_lvl_v3;
        aic_txpwr_lvl_conf_v4_t txpwr_lvl_v4;
    };
} aic_mm_set_txpwr_lvl_req_t;

typedef struct {
    uint8_t enable;
    int8_t  pwrlvl_adj_tbl_2g4[3];
    int8_t  pwrlvl_adj_tbl_5g[6];
} aic_txpwr_lvl_adj_conf_t;

typedef struct {
    uint8_t enable;
    int8_t  pwrlvl_adj_tbl_2g4[3];
} aic_txpwr_lvl_adj_conf_v2_t;

typedef struct {
    union {
        aic_txpwr_lvl_adj_conf_t    txpwr_lvl_adj;
        aic_txpwr_lvl_adj_conf_v2_t txpwr_lvl_adj_v2;
    };
} aic_mm_set_txpwr_lvl_adj_req_t;

typedef struct {
    int8_t enable;
    int8_t chan_1_4;
    int8_t chan_5_9;
    int8_t chan_10_13;
    int8_t chan_36_64;
    int8_t chan_100_120;
    int8_t chan_122_140;
    int8_t chan_142_165;
} aic_txpwr_ofst_conf_t;

typedef struct {
    uint8_t enable;
    int8_t  pwrofst2x_tbl_2g4[3][3];
    int8_t  pwrofst2x_tbl_5g[3][6];
} aic_txpwr_ofst2x_conf_t;

typedef struct {
    uint8_t enable;
    uint8_t pwrofst_flags;
    int8_t  pwrofst2x_tbl_2g4_ant0[3][3];
    int8_t  pwrofst2x_tbl_2g4_ant1[3][3];
    int8_t  pwrofst2x_tbl_5g_ant0[6][3];
    int8_t  pwrofst2x_tbl_5g_ant1[6][3];
    int8_t  pwrofst2x_tbl_6g_ant0[15];
    int8_t  pwrofst2x_tbl_6g_ant1[15];
} aic_txpwr_ofst2x_conf_v2_t;

typedef struct {
    uint8_t enable;
    int8_t  pwrofst2x_tbl_2g4[3][3];
    uint8_t RESERVED[2];
} aic_txpwr_ofst2x_conf_v3_t;

typedef struct {
    union {
        aic_txpwr_ofst_conf_t      txpwr_ofst;
        aic_txpwr_ofst2x_conf_t    txpwr_ofst2x;
        aic_txpwr_ofst2x_conf_v2_t txpwr_ofst2x_v2;
        aic_txpwr_ofst2x_conf_v3_t txpwr_ofst2x_v3;
    };
} aic_mm_set_txpwr_ofst_req_t;

typedef struct {
    uint8_t is_stack_start;
    uint8_t efuse_valid;
    uint8_t set_vendor_info;
    uint8_t fwtrace_redir;
} aic_mm_set_stack_start_req_t;

typedef struct {
    uint8_t is_5g_support;
    uint8_t vendor_info;
} aic_mm_set_stack_start_cfm_t;

typedef struct {
    uint8_t fw_version_len;
    uint8_t fw_version[63];
} aic_mm_get_fw_version_cfm_t;

typedef struct {
    uint32_t queue_idx;
    uint32_t used_idx;
} aic_usb_data_cfm_t;

AIC_STATIC_ASSERT(sizeof(aic_mm_set_rf_calib_req_t) == 24, "mm_set_rf_calib_req size");
AIC_STATIC_ASSERT(sizeof(aic_mm_set_rf_calib_cfm_t) == 16, "mm_set_rf_calib_cfm size");
AIC_STATIC_ASSERT(sizeof(aic_mm_get_mac_addr_req_t) == 4, "mm_get_mac_addr_req size");
AIC_STATIC_ASSERT(sizeof(aic_mm_get_mac_addr_cfm_t) == 6, "mm_get_mac_addr_cfm size");
AIC_STATIC_ASSERT(sizeof(aic_txpwr_lvl_conf_t) == 10, "txpwr_lvl_conf_t size");
AIC_STATIC_ASSERT(sizeof(aic_txpwr_lvl_conf_v2_t) == 35, "txpwr_lvl_conf_v2_t size");
AIC_STATIC_ASSERT(sizeof(aic_txpwr_lvl_conf_v3_t) == 69, "txpwr_lvl_conf_v3_t size");
AIC_STATIC_ASSERT(sizeof(aic_txpwr_lvl_conf_v4_t) == 95, "txpwr_lvl_conf_v4_t size");
AIC_STATIC_ASSERT(sizeof(aic_mm_set_txpwr_lvl_req_t) == 95, "mm_set_txpwr_lvl_req size");
AIC_STATIC_ASSERT(sizeof(aic_txpwr_lvl_adj_conf_t) == 10, "txpwr_lvl_adj_conf_t size");
AIC_STATIC_ASSERT(sizeof(aic_txpwr_lvl_adj_conf_v2_t) == 4, "txpwr_lvl_adj_conf_v2_t size");
AIC_STATIC_ASSERT(sizeof(aic_mm_set_txpwr_lvl_adj_req_t) == 10, "mm_set_txpwr_lvl_adj_req size");
AIC_STATIC_ASSERT(sizeof(aic_txpwr_ofst_conf_t) == 8, "txpwr_ofst_conf_t size");
AIC_STATIC_ASSERT(sizeof(aic_txpwr_ofst2x_conf_t) == 28, "txpwr_ofst2x_conf_t size");
AIC_STATIC_ASSERT(sizeof(aic_txpwr_ofst2x_conf_v2_t) == 86, "txpwr_ofst2x_conf_v2_t size");
AIC_STATIC_ASSERT(sizeof(aic_txpwr_ofst2x_conf_v3_t) == 12, "txpwr_ofst2x_conf_v3_t size");
AIC_STATIC_ASSERT(sizeof(aic_mm_set_txpwr_ofst_req_t) == 86, "mm_set_txpwr_ofst_req size");
AIC_STATIC_ASSERT(sizeof(aic_mm_set_stack_start_req_t) == 4, "mm_set_stack_start_req size");
AIC_STATIC_ASSERT(sizeof(aic_mm_set_stack_start_cfm_t) == 2, "mm_set_stack_start_cfm size");
AIC_STATIC_ASSERT(sizeof(aic_mm_get_fw_version_cfm_t) == 64, "mm_get_fw_version_cfm size");
AIC_STATIC_ASSERT(sizeof(aic_usb_data_cfm_t) == 8, "usb_data_cfm size");

#endif /* AIC8800_ABI_H */
