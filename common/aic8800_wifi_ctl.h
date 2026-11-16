/*
 * aic8800_wifi_ctl.h - Shared UserClient ABI & 802.11 FullMAC helpers
 *
 * Strictly POD C structs (<= 4096 B inline UserClient payloads) and zero-heap
 * parsers safe for x86_64 Kernel.framework (-mkernel -nostdlib) and user space.
 */

#ifndef AIC8800_WIFI_CTL_H
#define AIC8800_WIFI_CTL_H

#include "aic8800_abi.h"
#include "aic8800_framing.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AIC_WIFI_CTL_VERSION        1U
#define AIC_WIFI_USERCLIENT_TYPE    0x41494357U /* "AICW" */

#define AIC_WIFI_MAX_SCAN_BSS       64U
#define AIC_WIFI_EAPOL_RING_CAP     16U
#define AIC_WIFI_EAPOL_MAX_FRAME    1600U
#define AIC_WIFI_CONNECT_IE_MAX     256U

/* Firmware AC / TID defaults for STA data & EAPOL frames (rwnx_txq.h) */
#define AIC_RWNX_HWQ_BE             1U
#define AIC_STA_DEFAULT_TID         0U
#define AIC_STA_NONQOS_TID          0xFFU

/* LMAC mac_cipher_suite enum (lmac_mac.h:150-178) */
enum aic_mac_cipher_suite {
    AIC_CIPHER_WEP40        = 0U,
    AIC_CIPHER_TKIP         = 1U,
    AIC_CIPHER_CCMP         = 2U,
    AIC_CIPHER_WEP104       = 3U,
    AIC_CIPHER_WPI_SMS4     = 4U,
    AIC_CIPHER_BIP_CMAC_128 = 5U,
    AIC_CIPHER_GCMP_128     = 6U,
    AIC_CIPHER_GCMP_256     = 7U,
    AIC_CIPHER_CCMP_256     = 8U,
    AIC_CIPHER_BIP_GMAC_128 = 9U,
    AIC_CIPHER_BIP_GMAC_256 = 10U,
    AIC_CIPHER_BIP_CMAC_256 = 11U,
    AIC_CIPHER_INVALID      = 0xFFU,
};

/* Security capability bitmask extracted from Beacon/ProbeResp IEs */
enum aic_wifi_sec_bits {
    AIC_SEC_OPEN      = 0U,
    AIC_SEC_PRIVACY   = (1U << 0U),
    AIC_SEC_WPA_PSK   = (1U << 1U),
    AIC_SEC_WPA2_PSK  = (1U << 2U),
    AIC_SEC_WPA3_SAE  = (1U << 3U),
    AIC_SEC_CCMP      = (1U << 4U),
    AIC_SEC_TKIP      = (1U << 5U),
    AIC_SEC_SHA256    = (1U << 6U),
    AIC_SEC_MFP_REQ   = (1U << 7U),
    AIC_SEC_8021X     = (1U << 8U),
};

/* IOUserClient external method selectors */
enum aic_wifi_uc_selector {
    AIC_WIFI_UC_GET_STATUS       = 0U,
    AIC_WIFI_UC_START_SCAN       = 1U,
    AIC_WIFI_UC_GET_SCAN_RESULTS = 2U,
    AIC_WIFI_UC_CONNECT          = 3U,
    AIC_WIFI_UC_DISCONNECT       = 4U,
    AIC_WIFI_UC_POLL_EAPOL       = 5U,
    AIC_WIFI_UC_SEND_EAPOL       = 6U,
    AIC_WIFI_UC_ADD_KEY          = 7U,
    AIC_WIFI_UC_SET_PORT_LINK    = 8U,
    AIC_WIFI_UC_METHOD_COUNT     = 9U,
};

typedef struct {
    uint32_t version;
    uint8_t  lmac_ready;
    uint8_t  scanning;
    uint8_t  scan_done;
    uint8_t  associated;

    uint8_t  link_up;
    uint8_t  is_5g_supported;
    uint8_t  vif_idx;
    uint8_t  ap_sta_idx;

    uint8_t  ap_qos;
    uint8_t  ap_band;
    uint8_t  ap_width;
    uint8_t  ssid_len;

    uint16_t ap_center_freq;
    uint16_t assoc_status;

    uint16_t disconnect_reason;
    uint16_t scan_count;

    uint32_t chip_id;
    uint32_t lmac_version;
    uint32_t fw_features;
    uint32_t eapol_rx_queued;

    uint64_t tx_packets;
    uint64_t rx_packets;
    uint64_t eapol_rx_total;

    uint8_t  mac_addr[AIC_MAC_ADDR_LEN];
    uint8_t  ap_bssid[AIC_MAC_ADDR_LEN];
    uint8_t  ssid[AIC_MAC_SSID_LEN];
    uint8_t  fw_version[64];
} aic_wifi_status_t;

typedef struct {
    uint8_t ssid_len;
    uint8_t passive;
    uint8_t no_cck;
    uint8_t reserved;
    uint8_t ssid[AIC_MAC_SSID_LEN];
} aic_wifi_scan_req_t;

typedef struct {
    uint8_t  bssid[AIC_MAC_ADDR_LEN];
    uint8_t  ssid_len;
    uint8_t  band;
    uint16_t freq;
    uint16_t cap_info;
    int8_t   rssi;
    uint8_t  pairwise_cipher;
    uint8_t  group_cipher;
    uint8_t  reserved;
    uint32_t sec_mask;
    uint8_t  ssid[AIC_MAC_SSID_LEN];
    uint16_t rsn_capab;
    uint16_t pad;
} aic_wifi_scan_bss_t; /* 56 bytes */

typedef struct {
    uint16_t            count;
    uint8_t             scanning;
    uint8_t             scan_done;
    uint32_t            reserved;
    aic_wifi_scan_bss_t bss[AIC_WIFI_MAX_SCAN_BSS];
} aic_wifi_scan_list_t; /* 8 + 64*56 = 3592 bytes <= 4096 inline IOKit limit */

typedef struct {
    uint8_t  ssid_len;
    uint8_t  auth_type; /* 0 = OPEN_SYSTEM, 3 = SAE */
    uint8_t  band;
    uint8_t  reserved;
    uint16_t freq;
    uint16_t ie_len;
    uint32_t flags;               /* aic_mac_connection_flags */
    uint16_t ctrl_port_ethertype; /* network byte order (htons(0x888E)) */
    uint8_t  bssid[AIC_MAC_ADDR_LEN];
    uint8_t  ssid[AIC_MAC_SSID_LEN];
    uint8_t  ie_buf[AIC_WIFI_CONNECT_IE_MAX];
    uint8_t  out_status;
    uint8_t  pad[3];
} aic_wifi_connect_req_t;

typedef struct {
    uint16_t reason_code;
    uint16_t reserved;
} aic_wifi_disconnect_req_t;

typedef struct {
    uint16_t len;
    uint8_t  has_frame;
    uint8_t  reserved;
    uint8_t  data[AIC_WIFI_EAPOL_MAX_FRAME];
} aic_wifi_eapol_frame_t;

typedef struct {
    uint8_t key_idx;
    uint8_t pairwise;
    uint8_t cipher_suite;
    uint8_t key_len;
    uint8_t key[AIC_MAC_SEC_KEY_LEN];
    uint8_t out_hw_key_idx;
    uint8_t out_status;
    uint8_t reserved[2];
} aic_wifi_key_req_t;

typedef struct {
    uint8_t control_port_open;
    uint8_t link_up;
    uint8_t reserved[2];
} aic_wifi_port_req_t;

AIC_STATIC_ASSERT(sizeof(aic_wifi_status_t) <= 4096U, "status fits inline UserClient");
AIC_STATIC_ASSERT(sizeof(aic_wifi_scan_bss_t) == 56U, "scan_bss packed 56B");
AIC_STATIC_ASSERT(sizeof(aic_wifi_scan_list_t) <= 4096U, "scan_list MUST fit 4096B inline UserClient");
AIC_STATIC_ASSERT(sizeof(aic_wifi_connect_req_t) <= 4096U, "connect_req fits inline UserClient");
AIC_STATIC_ASSERT(sizeof(aic_wifi_eapol_frame_t) <= 4096U, "eapol_frame fits inline UserClient");

static inline uint8_t aic_rsn_cipher_suite_to_mac(const uint8_t suite[4]) {
    /* OUI 00-0F-AC (RSN) or 00-50-F2 (WPA) */
    if ((suite[0] == 0x00U && suite[1] == 0x0FU && suite[2] == 0xACU) ||
        (suite[0] == 0x00U && suite[1] == 0x50U && suite[2] == 0xF2U)) {
        switch (suite[3]) {
            case 1U:  return AIC_CIPHER_WEP40;
            case 2U:  return AIC_CIPHER_TKIP;
            case 4U:  return AIC_CIPHER_CCMP;
            case 5U:  return AIC_CIPHER_WEP104;
            case 6U:  return AIC_CIPHER_BIP_CMAC_128;
            case 8U:  return AIC_CIPHER_GCMP_128;
            case 9U:  return AIC_CIPHER_GCMP_256;
            case 10U: return AIC_CIPHER_CCMP_256;
            default:  return AIC_CIPHER_INVALID;
        }
    }
    return AIC_CIPHER_INVALID;
}

/*
 * Parse an IEEE 802.11 RSN IE body (excluding Tag ID & Length bytes) into security mask/ciphers.
 */
static inline void aic_parse_rsn_ie_body(const uint8_t *body,
                                         size_t body_len,
                                         aic_wifi_scan_bss_t *out) {
    if (!body || !out || body_len < 2U) {
        return;
    }
    size_t pos = 2U; /* skip 2-byte Version (0x0001) */
    if (pos + 4U > body_len) {
        return;
    }
    out->group_cipher = aic_rsn_cipher_suite_to_mac(&body[pos]);
    pos += 4U;

    if (pos + 2U > body_len) {
        return;
    }
    const uint16_t pw_cnt = aic_get_le16(&body[pos]);
    pos += 2U;
    if ((size_t)pw_cnt * 4U > body_len - pos) {
        return;
    }
    uint8_t best_pw = AIC_CIPHER_INVALID;
    for (uint16_t i = 0U; i < pw_cnt; ++i) {
        const uint8_t c = aic_rsn_cipher_suite_to_mac(&body[pos + (size_t)i * 4U]);
        if (c == AIC_CIPHER_CCMP) {
            out->sec_mask |= AIC_SEC_CCMP;
            best_pw = AIC_CIPHER_CCMP;
        } else if (c == AIC_CIPHER_TKIP) {
            out->sec_mask |= AIC_SEC_TKIP;
            if (best_pw == AIC_CIPHER_INVALID) {
                best_pw = AIC_CIPHER_TKIP;
            }
        }
    }
    if (best_pw != AIC_CIPHER_INVALID) {
        out->pairwise_cipher = best_pw;
    }
    pos += (size_t)pw_cnt * 4U;

    if (pos + 2U > body_len) {
        return;
    }
    const uint16_t akm_cnt = aic_get_le16(&body[pos]);
    pos += 2U;
    if ((size_t)akm_cnt * 4U > body_len - pos) {
        return;
    }
    for (uint16_t i = 0U; i < akm_cnt; ++i) {
        const uint8_t *akm = &body[pos + (size_t)i * 4U];
        if (akm[0] == 0x00U && akm[1] == 0x0FU && akm[2] == 0xACU) {
            if (akm[3] == 2U) {
                out->sec_mask |= AIC_SEC_WPA2_PSK;
            } else if (akm[3] == 6U) {
                out->sec_mask |= (AIC_SEC_WPA2_PSK | AIC_SEC_SHA256);
            } else if (akm[3] == 8U) {
                out->sec_mask |= (AIC_SEC_WPA3_SAE | AIC_SEC_SHA256);
            } else if (akm[3] == 1U || akm[3] == 5U) {
                out->sec_mask |= AIC_SEC_8021X;
            }
        }
    }
    pos += (size_t)akm_cnt * 4U;

    if (pos + 2U <= body_len) {
        out->rsn_capab = aic_get_le16(&body[pos]);
        if ((out->rsn_capab & 0x0040U) != 0U) {
            out->sec_mask |= AIC_SEC_MFP_REQ;
        }
    }
}

/*
 * Parse SCANU_RESULT_IND IPC message payload (12B hdr + IEEE 802.11 Beacon/ProbeResp).
 * Pure zero-heap parser usable in kernel RX thread and offline unit tests.
 */
static inline int aic_parse_scanu_result_ind(const uint8_t *param,
                                             size_t param_len,
                                             aic_wifi_scan_bss_t *out) {
    if (!param || !out || param_len < sizeof(aic_scanu_result_ind_hdr_t)) {
        return -1;
    }
    aic_scanu_result_ind_hdr_t hdr;
    memcpy(&hdr, param, sizeof(hdr));

    const uint8_t *mgmt = param + sizeof(hdr);
    const size_t avail = param_len - sizeof(hdr);
    const size_t frame_len = ((size_t)hdr.length <= avail) ? (size_t)hdr.length : avail;

    /* 24B 802.11 mgmt header + 12B fixed beacon/probe-resp body = 36B minimum */
    if (frame_len < 36U) {
        return -1;
    }

    memset(out, 0, sizeof(*out));
    memcpy(out->bssid, &mgmt[16], AIC_MAC_ADDR_LEN);
    out->freq     = hdr.center_freq;
    out->band     = hdr.band;
    out->rssi     = hdr.rssi;
    out->cap_info = aic_get_le16(&mgmt[34]);
    out->pairwise_cipher = AIC_CIPHER_INVALID;
    out->group_cipher    = AIC_CIPHER_INVALID;
    if ((out->cap_info & 0x0010U) != 0U) {
        out->sec_mask |= AIC_SEC_PRIVACY;
    }

    size_t pos = 36U;
    while (pos + 2U <= frame_len) {
        const uint8_t tag = mgmt[pos];
        const uint8_t len = mgmt[pos + 1U];
        pos += 2U;
        if ((size_t)len > frame_len - pos) {
            break;
        }
        const uint8_t *val = &mgmt[pos];

        if (tag == 0U) { /* SSID */
            const uint8_t cpy = (len <= AIC_MAC_SSID_LEN) ? len : (uint8_t)AIC_MAC_SSID_LEN;
            out->ssid_len = cpy;
            if (cpy > 0U) {
                memcpy(out->ssid, val, cpy);
            }
        } else if (tag == 48U) { /* RSN IE */
            aic_parse_rsn_ie_body(val, len, out);
        } else if (tag == 221U && len >= 4U &&
                   val[0] == 0x00U && val[1] == 0x50U && val[2] == 0xF2U && val[3] == 0x01U) {
            /* Legacy WPA Vendor IE */
            out->sec_mask |= AIC_SEC_WPA_PSK;
            if (out->pairwise_cipher == AIC_CIPHER_INVALID) {
                out->pairwise_cipher = AIC_CIPHER_TKIP;
            }
        }

        pos += (size_t)len;
    }

    return 0;
}

/*
 * Build SCANU_START_REQ matching Linux rwnx_send_scanu_req (<= 42 channels).
 */
static inline void aic_build_scanu_start_req(aic_scanu_start_req_t *req,
                                             uint8_t vif_idx,
                                             bool include_5g,
                                             const uint8_t *ssid,
                                             uint8_t ssid_len,
                                             bool no_cck) {
    static const uint16_t kChan2g4Freqs[13] = {
        2412, 2417, 2422, 2427, 2432, 2437, 2442,
        2447, 2452, 2457, 2462, 2467, 2472
    };
    static const uint16_t kChan5gFreqs[25] = {
        5180, 5200, 5220, 5240, 5260, 5280, 5300, 5320,
        5500, 5520, 5540, 5560, 5580, 5600, 5620, 5640, 5660, 5680, 5700, 5720,
        5745, 5765, 5785, 5805, 5825
    };

    memset(req, 0, sizeof(*req));
    req->vif_idx = vif_idx;
    memset(req->bssid.array, 0xFF, AIC_MAC_ADDR_LEN);
    req->no_cck = no_cck ? 1U : 0U;

    /* Active wildcard scan if ssid_len == 0, directed probe if ssid_len > 0 */
    req->ssid_cnt = 1U;
    if (ssid && ssid_len > 0U) {
        const uint8_t cpy = (ssid_len <= AIC_MAC_SSID_LEN) ? ssid_len : (uint8_t)AIC_MAC_SSID_LEN;
        req->ssid[0].length = cpy;
        memcpy(req->ssid[0].array, ssid, cpy);
    } else {
        req->ssid[0].length = 0U;
    }

    uint8_t idx = 0U;
    for (uint8_t i = 0U; i < 13U && idx < AIC_SCAN_CHANNEL_MAX; ++i) {
        req->chan[idx].freq     = kChan2g4Freqs[i];
        req->chan[idx].band     = AIC_PHY_BAND_2G4;
        req->chan[idx].tx_power = 20;
        req->chan[idx].flags    = (i >= 11U) ? AIC_CHAN_NO_IR : 0U;
        idx++;
    }
    if (include_5g) {
        for (uint8_t i = 0U; i < 25U && idx < AIC_SCAN_CHANNEL_MAX; ++i) {
            const uint16_t f = kChan5gFreqs[i];
            const bool is_dfs = ((f >= 5260U && f <= 5320U) || (f >= 5500U && f <= 5720U));
            req->chan[idx].freq     = f;
            req->chan[idx].band     = AIC_PHY_BAND_5G;
            req->chan[idx].tx_power = 20;
            req->chan[idx].flags    = is_dfs ? (uint8_t)(AIC_CHAN_NO_IR | AIC_CHAN_RADAR) : 0U;
            idx++;
        }
    }
    req->chan_cnt = idx;
}

/*
 * Build standard 22-byte RSN IE (Tag 48) for WPA2-PSK Association Request.
 */
static inline size_t aic_build_wpa2_rsn_ie(uint8_t out_ie[22],
                                           uint8_t group_cipher_mac,
                                           bool use_akm_sha256) {
    uint8_t grp_suite = 4U; /* CCMP */
    if (group_cipher_mac == AIC_CIPHER_TKIP) {
        grp_suite = 2U;
    } else if (group_cipher_mac == AIC_CIPHER_WEP40) {
        grp_suite = 1U;
    } else if (group_cipher_mac == AIC_CIPHER_WEP104) {
        grp_suite = 5U;
    }

    out_ie[0]  = 48U;   /* EID: RSN */
    out_ie[1]  = 20U;   /* Length */
    out_ie[2]  = 0x01U; /* Version LO */
    out_ie[3]  = 0x00U; /* Version HI */
    out_ie[4]  = 0x00U; /* Group Cipher OUI */
    out_ie[5]  = 0x0FU;
    out_ie[6]  = 0xACU;
    out_ie[7]  = grp_suite;
    out_ie[8]  = 0x01U; /* Pairwise Count = 1 */
    out_ie[9]  = 0x00U;
    out_ie[10] = 0x00U; /* Pairwise Cipher: CCMP */
    out_ie[11] = 0x0FU;
    out_ie[12] = 0xACU;
    out_ie[13] = 0x04U;
    out_ie[14] = 0x01U; /* AKM Count = 1 */
    out_ie[15] = 0x00U;
    out_ie[16] = 0x00U; /* AKM: PSK (02) or PSK-SHA256 (06) */
    out_ie[17] = 0x0FU;
    out_ie[18] = 0xACU;
    out_ie[19] = use_akm_sha256 ? 0x06U : 0x02U;
    out_ie[20] = 0x00U; /* RSN Capabilities */
    out_ie[21] = 0x00U;
    return 22U;
}

#ifdef __cplusplus
}
#endif

#endif /* AIC8800_WIFI_CTL_H */
