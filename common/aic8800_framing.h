/*
 * aic8800_framing.h - Zero-heap wire framing helpers for AIC8800D80 (Wi-Fi & BT)
 *
 * Strictly uses caller-provided buffers (no malloc/new/IOMalloc anywhere).
 * Matches Linux aic_load_fw + aic8800_fdrv (aicwf_usb.c, aicwf_txrxif.c,
 * rwnx_cmds.c, rwnx_tx.c, rwnx_rx.c) and macOS IOUSBHost ZLP rules.
 */

#ifndef AIC8800_FRAMING_H
#define AIC8800_FRAMING_H

#include "aic8800_abi.h"

#ifdef __cplusplus
#include <string.h>
#else
#include <string.h>
#endif

static inline uint16_t aic_get_le16(const uint8_t *b) {
    return (uint16_t)b[0] | (uint16_t)((uint16_t)b[1] << 8U);
}

static inline uint32_t aic_get_le32(const uint8_t *b) {
    return (uint32_t)b[0] |
           ((uint32_t)b[1] << 8U) |
           ((uint32_t)b[2] << 16U) |
           ((uint32_t)b[3] << 24U);
}

static inline uint16_t aic_get_be16(const uint8_t *b) {
    return (uint16_t)((uint16_t)b[0] << 8U) | (uint16_t)b[1];
}

static inline void aic_put_le16(uint8_t *b, uint16_t v) {
    b[0] = (uint8_t)(v & 0xFFU);
    b[1] = (uint8_t)((v >> 8U) & 0xFFU);
}

static inline void aic_put_le32(uint8_t *b, uint32_t v) {
    b[0] = (uint8_t)(v & 0xFFU);
    b[1] = (uint8_t)((v >> 8U) & 0xFFU);
    b[2] = (uint8_t)((v >> 16U) & 0xFFU);
    b[3] = (uint8_t)((v >> 24U) & 0xFFU);
}

/* Returns true iff a USB bulk OUT transfer requires a trailing 0-byte ZLP. */
static inline bool aic_usb_needs_zlp(uint32_t transfer_len, uint16_t max_packet_size) {
    return (transfer_len > 0U) &&
           (max_packet_size > 0U) &&
           ((transfer_len % (uint32_t)max_packet_size) == 0U);
}

/* Populate the 3 Stage-1 config patches for AIC8800D80 (5 GHz selectable). */
static inline void aic_stage1_build_d80_patch_pairs(uint32_t config_base,
                                                    bool enable_5g,
                                                    uint32_t out_pairs[3][2]) {
    out_pairs[0][0] = config_base + AIC_D80_PATCH_USER_CFG_OFF;
    out_pairs[0][1] = enable_5g ? AIC_D80_PATCH_USER_CFG_5G : 0xF3010000U;
    out_pairs[1][0] = config_base + AIC_D80_PATCH_RX_AGGR_OFF;
    out_pairs[1][1] = AIC_D80_PATCH_RX_AGGR_VAL;
    out_pairs[2][0] = config_base + AIC_D80_PATCH_EXT_FLAGS_OFF;
    out_pairs[2][1] = AIC_D80_PATCH_EXT_FLAGS_VAL;
}

/*
 * Build the 31-byte USB Mass Storage Bulk-Only Transport (BOT) Command Block
 * Wrapper (CBW) for SCSI START STOP UNIT (LoEj=1, Start=0 -> Eject) so Stage 0
 * (0xA69C:0x5721) switches cleanly into Stage 1 Bootloader (0xA69C:0x8D80).
 */
static inline void aic_build_bot_eject_cbw(uint8_t out_cbw[AIC_USB_BOT_CBW_LEN],
                                           uint32_t tag) {
    memset(out_cbw, 0, AIC_USB_BOT_CBW_LEN);
    aic_put_le32(&out_cbw[0], AIC_USB_BOT_CBW_SIGNATURE);
    aic_put_le32(&out_cbw[4], tag);
    /* dCBWDataTransferLength = 0, bmCBWFlags = 0x00, bCBWLUN = 0 */
    out_cbw[14] = 6U;   /* bCBWCBLength = 6 bytes */
    out_cbw[15] = 0x1BU;/* SCSI START STOP UNIT */
    out_cbw[19] = 0x02U;/* LoEj=1, Start=0 -> Eject media */
}

/*
 * Encode an outbound LMAC command/debug message (`0x11`) into caller buffer.
 * Matches rwnx_cmds.c:aicwf_set_cmd_tx():
 *   [0..1] = (8 + param_len + 4) le16 (& 0x0FFF)
 *   [2]    = 0x11
 *   [3]    = 0x00
 *   [4..7] = 0x00000000 (dummy word)
 *   [8..15]= aic_lmac_msg_hdr_t
 *   [16..] = param bytes
 */
static inline int aic_encode_lmac_msg(uint8_t *out_buf,
                                      size_t out_cap,
                                      uint16_t msg_id,
                                      uint16_t dest_id,
                                      uint16_t src_id,
                                      const void *param,
                                      uint16_t param_len,
                                      size_t *out_len) {
    if (!out_buf || !out_len)
        return -1;
    if (param_len > 0U && !param)
        return -1;

    size_t lmac_len = sizeof(aic_lmac_msg_hdr_t) + (size_t)param_len;
    size_t wire_len = 8U + lmac_len;
    if (wire_len > AIC_USB_MSG_MAX_PKT_SIZE || wire_len > out_cap)
        return -1;

    uint16_t hdr_len = (uint16_t)(lmac_len + 4U);
    out_buf[0] = (uint8_t)(hdr_len & 0xFFU);
    out_buf[1] = (uint8_t)((hdr_len >> 8U) & 0x0FU);
    out_buf[2] = AIC_USB_TYPE_CFG_CMD_RSP; /* 0x11 */
    out_buf[3] = 0x00U;
    out_buf[4] = 0x00U;
    out_buf[5] = 0x00U;
    out_buf[6] = 0x00U;
    out_buf[7] = 0x00U;

    aic_put_le16(&out_buf[8], msg_id);
    aic_put_le16(&out_buf[10], dest_id);
    aic_put_le16(&out_buf[12], src_id);
    aic_put_le16(&out_buf[14], param_len);

    if (param_len > 0U)
        memcpy(&out_buf[16], param, param_len);

    *out_len = wire_len;
    return 0;
}

/*
 * Encode an Ethernet II frame into AIC8800D80 USB TX Data format (`0x01`).
 * Matches rwnx_tx.c (14B eth header -> hostdesc) + aicwf_usb.c (4B USB header):
 *   [0..3]   = [len_lo, (len>>8)&0x0F, 0x01, 0x00]
 *   [4..31]  = aic_txdesc_api_t (28 bytes)
 *   [32..]   = payload (eth_len - 14 bytes) + optional 4B tail zero-padding
 */
static inline int aic_tx_encode_eth_frame(const uint8_t *eth_frame,
                                          size_t eth_len,
                                          uint8_t sta_idx,
                                          uint8_t vif_idx,
                                          uint8_t tid,
                                          uint8_t ac,
                                          uint16_t flags,
                                          bool pad_to_4,
                                          uint8_t *out_buf,
                                          size_t out_cap,
                                          size_t *out_len) {
    if (!eth_frame || !out_buf || !out_len)
        return -1;
    if (eth_len < 14U || eth_len > AIC_DATA_BUF_MAX)
        return -1;

    size_t payload_len = eth_len - 14U;
    size_t wire_len = 4U + sizeof(aic_txdesc_api_t) + payload_len;
    size_t padded_len = pad_to_4 ? ((wire_len + (AIC_TX_ALIGNMENT - 1U)) & ~(size_t)(AIC_TX_ALIGNMENT - 1U))
                                 : wire_len;
    if (padded_len > out_cap || padded_len > 4095U)
        return -1;

    aic_txdesc_api_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.host.packet_len = (uint16_t)payload_len;
    desc.host.flags_ext = 0U;
    desc.host.status_desc_addr = 0U;
    memcpy(desc.host.eth_dest_addr.array, &eth_frame[0], AIC_MAC_ADDR_LEN);
    memcpy(desc.host.eth_src_addr.array, &eth_frame[6], AIC_MAC_ADDR_LEN);
    memcpy(&desc.host.ethertype, &eth_frame[12], sizeof(uint16_t));
    desc.host.ac = ac;
    desc.host.tid = tid;
    desc.host.vif_idx = vif_idx;
    desc.host.staid = sta_idx;
    desc.host.flags = flags;

    out_buf[0] = (uint8_t)(padded_len & 0xFFU);
    out_buf[1] = (uint8_t)((padded_len >> 8U) & 0x0FU);
    out_buf[2] = AIC_USB_TYPE_TX_DATA_OUT; /* 0x01 */
    out_buf[3] = 0x00U;

    memcpy(&out_buf[4], &desc, sizeof(desc));
    if (payload_len > 0U)
        memcpy(&out_buf[4U + sizeof(desc)], &eth_frame[14], payload_len);
    if (padded_len > wire_len)
        memset(&out_buf[wire_len], 0, padded_len - wire_len);

    *out_len = padded_len;
    return 0;
}

typedef void (*aic_rx_usb_data_cb_t)(const uint8_t *chunk, size_t chunk_len, void *ctx);
typedef void (*aic_rx_usb_cfg_cb_t)(uint8_t cfg_type, const uint8_t *payload, size_t payload_len, void *ctx);

/*
 * Demux an aggregated or single USB Bulk IN buffer (aicwf_txrxif.c:741-807).
 * Returns number of decoded packets >= 0, or -1 on truncated header.
 */
static inline int aic_rx_demux_usb_aggr(const uint8_t *buf,
                                        size_t buf_len,
                                        aic_rx_usb_data_cb_t on_data,
                                        aic_rx_usb_cfg_cb_t on_cfg,
                                        void *ctx) {
    if (!buf)
        return -1;

    size_t pos = 0U;
    int count = 0;

    while (pos < buf_len) {
        size_t rem = buf_len - pos;
        if (rem < 4U)
            return -1;

        const uint8_t *p = &buf[pos];
        uint16_t pkt_len = aic_get_le16(p);
        uint8_t pkt_type = p[2];

        if ((pkt_type & AIC_USB_TYPE_CFG) != AIC_USB_TYPE_CFG) {
            size_t aggr_len = (size_t)pkt_len + AIC_RX_HWHDR_LEN;
            if (pkt_len == 0U || aggr_len > rem)
                return -1;
            if (on_data)
                on_data(p, aggr_len, ctx);
            pos += aggr_len;
        } else {
            size_t aggr_len = (size_t)pkt_len;
            size_t adjust_len = (aggr_len + (AIC_RX_ALIGNMENT - 1U)) & ~(size_t)(AIC_RX_ALIGNMENT - 1U);
            if (4U + aggr_len > rem)
                return -1;
            if (on_cfg)
                on_cfg((uint8_t)(pkt_type & 0x7FU), &p[4], aggr_len, ctx);
            size_t step = (4U + adjust_len <= rem) ? (4U + adjust_len) : (4U + aggr_len);
            pos += step;
        }
        count++;
    }

    return count;
}

typedef struct {
    uint8_t  vif_idx;
    uint8_t  sta_idx;
    uint8_t  tid;
    uint16_t seq_num;
    uint16_t ethertype_be;
    bool     is_eapol;
    bool     is_amsdu;
    bool     is_mgmt_mpdu;
} aic_rx_frame_meta_t;

typedef void (*aic_rx_eth_cb_t)(const uint8_t *eth_frame,
                                size_t eth_len,
                                const aic_rx_frame_meta_t *meta,
                                void *ctx);

/*
 * Convert one AIC8800D80 RX data chunk (60-byte hw_rxhdr prefix + 802.11 frame)
 * into zero or more standard 802.3 Ethernet frames into caller's scratch_eth_buf.
 * Matches rwnx_rx.c:rwnx_rxdataind_aicwf + rwnx_rxdata_process_amsdu.
 * Returns number of Ethernet frames emitted (>=0), or -1 on invalid/truncated input.
 */
static inline int aic_rx_chunk_to_eth_frames(const uint8_t *chunk,
                                             size_t chunk_len,
                                             uint8_t *scratch_eth_buf,
                                             size_t scratch_cap,
                                             aic_rx_eth_cb_t on_eth,
                                             void *ctx) {
    if (!chunk || !scratch_eth_buf || chunk_len < AIC_RX_HWHDR_LEN + 24U)
        return -1;

    aic_hw_rxhdr_t rxhdr;
    memcpy(&rxhdr, chunk, sizeof(rxhdr));

    const uint8_t *mac = &chunk[AIC_RX_HWHDR_LEN];
    size_t mac_len = chunk_len - AIC_RX_HWHDR_LEN;

    aic_rx_frame_meta_t meta;
    memset(&meta, 0, sizeof(meta));
    meta.vif_idx = (uint8_t)rxhdr.flags_vif_idx;
    meta.sta_idx = (uint8_t)rxhdr.flags_sta_idx;

    if (rxhdr.flags_is_80211_mpdu) {
        meta.is_mgmt_mpdu = true;
        return 0;
    }

    /* Only 802.11 Data frames ((FC0 & 0x0F) == 0x08) convert to Ethernet II */
    if ((mac[0] & 0x0FU) != 0x08U)
        return 0;

    size_t hdr_len = 24U;
    bool is_qos = ((mac[0] & 0x80U) == 0x80U);
    bool is_htc = ((mac[1] & 0x80U) != 0U);
    bool is_amsdu = false;

    if (is_qos) {
        if (mac_len < hdr_len + 2U)
            return -1;
        meta.tid = (uint8_t)(mac[24] & 0x0FU);
        is_amsdu = ((mac[24] & 0x80U) != 0U);
        hdr_len += 2U;
    }
    if (is_htc)
        hdr_len += 4U;

    meta.seq_num = (uint16_t)(((mac[22] & 0xF0U) >> 4U) | ((uint32_t)mac[23] << 4U));

    uint8_t ra[AIC_MAC_ADDR_LEN];
    uint8_t ta[AIC_MAC_ADDR_LEN];
    uint8_t ds = (uint8_t)(mac[1] & 0x03U);
    if (ds == 0x01U) {
        memcpy(ra, &mac[16], AIC_MAC_ADDR_LEN);
        memcpy(ta, &mac[10], AIC_MAC_ADDR_LEN);
    } else if (ds == 0x02U) {
        memcpy(ta, &mac[16], AIC_MAC_ADDR_LEN);
        memcpy(ra, &mac[4], AIC_MAC_ADDR_LEN);
    } else {
        memcpy(ra, &mac[4], AIC_MAC_ADDR_LEN);
        memcpy(ta, &mac[10], AIC_MAC_ADDR_LEN);
    }

    size_t iv_len = 0U;
    switch (rxhdr.hwvect.decr_status) {
        case AIC_RX_HD_DECR_CCMP128:
        case AIC_RX_HD_DECR_CCMP256:
        case AIC_RX_HD_DECR_GCMP128:
        case AIC_RX_HD_DECR_GCMP256:
        case AIC_RX_HD_DECR_TKIP:
            iv_len = 8U;
            break;
        case AIC_RX_HD_DECR_WEP:
            iv_len = 4U;
            break;
        case AIC_RX_HD_DECR_WAPI:
            iv_len = 18U;
            break;
        default:
            iv_len = 0U;
            break;
    }

    size_t llc_off = hdr_len + iv_len;
    size_t pull_len = llc_off + 8U;
    if (pull_len > mac_len)
        return -1;

    uint8_t ether_type[2];
    ether_type[0] = mac[llc_off + 6U];
    ether_type[1] = mac[llc_off + 7U];
    meta.ethertype_be = aic_get_be16(ether_type);
    meta.is_eapol = (ether_type[0] == 0x88U && ether_type[1] == 0x8EU) ||
                    (ether_type[0] == 0x8EU && ether_type[1] == 0x88U);

    /* Netgear R7000 A-MSDU workaround matching rwnx_rx.c:2575 */
    if (is_amsdu) {
        if (mac[llc_off] == 0xAAU &&
            mac[llc_off + 1U] == 0xAAU &&
            mac[llc_off + 6U] > 0x06U) {
            is_amsdu = false;
        }
    }
    meta.is_amsdu = is_amsdu;

    if (!is_amsdu) {
        size_t payload_len = mac_len - pull_len;
        size_t eth_len = 14U + payload_len;
        if (eth_len > scratch_cap)
            return -1;

        memcpy(&scratch_eth_buf[0], ra, AIC_MAC_ADDR_LEN);
        memcpy(&scratch_eth_buf[6], ta, AIC_MAC_ADDR_LEN);
        memcpy(&scratch_eth_buf[12], ether_type, 2U);
        if (payload_len > 0U)
            memcpy(&scratch_eth_buf[14], &mac[pull_len], payload_len);

        if (on_eth)
            on_eth(scratch_eth_buf, eth_len, &meta, ctx);
        return 1;
    }

    /* A-MSDU subframe iteration starting at mac[llc_off] (pull_len - 8) */
    const uint8_t *sub = &mac[llc_off];
    size_t sub_rem = mac_len - llc_off;
    int emitted = 0;

    while (sub_rem > 16U) {
        uint16_t sublen = aic_get_be16(&sub[12]);
        if (sublen < 8U || (size_t)sublen + 14U > sub_rem)
            return -1;

        size_t len_aligned = (sub_rem > (size_t)sublen + 14U)
                                 ? (((size_t)sublen + 14U + 3U) & ~(size_t)3U)
                                 : ((size_t)sublen + 14U);
        if (len_aligned > sub_rem)
            len_aligned = (size_t)sublen + 14U;

        size_t eth_len = (size_t)sublen + 6U; /* DA(6)+SA(6)+(sublen-6) */
        if (eth_len > scratch_cap)
            return -1;

        memcpy(&scratch_eth_buf[0], &sub[0], AIC_MAC_ADDR_LEN);
        memcpy(&scratch_eth_buf[6], &sub[6], AIC_MAC_ADDR_LEN);
        memcpy(&scratch_eth_buf[12], &sub[20], (size_t)sublen - 6U);

        aic_rx_frame_meta_t sub_meta = meta;
        sub_meta.ethertype_be = aic_get_be16(&scratch_eth_buf[12]);
        sub_meta.is_eapol = (scratch_eth_buf[12] == 0x88U && scratch_eth_buf[13] == 0x8EU) ||
                            (scratch_eth_buf[12] == 0x8EU && scratch_eth_buf[13] == 0x88U);
        if (on_eth)
            on_eth(scratch_eth_buf, eth_len, &sub_meta, ctx);

        sub += len_aligned;
        sub_rem -= len_aligned;
        emitted++;
    }

    return emitted;
}

/*
 * Lock-free-friendly fixed O(1) slot pool (preallocated by caller, zero heap ops).
 */
typedef struct {
    uint16_t capacity;
    uint16_t free_count;
    uint16_t free_stack[256];
} aic_fixed_slot_pool_t;

static inline int aic_slot_pool_init(aic_fixed_slot_pool_t *pool, uint16_t capacity) {
    if (!pool || capacity == 0U || capacity > 256U)
        return -1;
    pool->capacity = capacity;
    pool->free_count = capacity;
    for (uint16_t i = 0U; i < capacity; i++)
        pool->free_stack[i] = (uint16_t)(capacity - 1U - i);
    return 0;
}

static inline int aic_slot_pool_acquire(aic_fixed_slot_pool_t *pool, uint16_t *out_idx) {
    if (!pool || !out_idx || pool->free_count == 0U)
        return -1;
    pool->free_count--;
    *out_idx = pool->free_stack[pool->free_count];
    return 0;
}

static inline int aic_slot_pool_release(aic_fixed_slot_pool_t *pool, uint16_t idx) {
    if (!pool || idx >= pool->capacity || pool->free_count >= pool->capacity)
        return -1;
    pool->free_stack[pool->free_count] = idx;
    pool->free_count++;
    return 0;
}

static inline uint16_t aic_slot_pool_in_flight(const aic_fixed_slot_pool_t *pool) {
    if (!pool || pool->free_count > pool->capacity)
        return 0U;
    return (uint16_t)(pool->capacity - pool->free_count);
}

#endif /* AIC8800_FRAMING_H */
