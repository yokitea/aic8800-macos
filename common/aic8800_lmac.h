#ifndef AIC8800_LMAC_H
#define AIC8800_LMAC_H

#include "aic8800_abi.h"
#include "aic8800_framing.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
 * 802.11 HT / VHT / HE Capability & Feature Constants (Linux mac80211 ABI)
 * ========================================================================== */

#define AIC_MM_FEAT_PS_BIT                6U
#define AIC_MM_FEAT_DPSM_BIT              8U
#define AIC_MM_FEAT_VHT_BIT               16U
#define AIC_MM_FEAT_BFMEE_BIT             17U
#define AIC_MM_FEAT_HE_BIT                30U

#define AIC_IEEE80211_HT_CAP_LDPC_CODING      0x0001U
#define AIC_IEEE80211_HT_CAP_SUP_WIDTH_20_40  0x0002U
#define AIC_IEEE80211_HT_CAP_SM_PS_DISABLED   0x000CU
#define AIC_IEEE80211_HT_CAP_SGI_20           0x0020U
#define AIC_IEEE80211_HT_CAP_SGI_40           0x0040U
#define AIC_IEEE80211_HT_CAP_RX_STBC_1SS      0x0100U
#define AIC_IEEE80211_HT_CAP_DSSSCCK40        0x1000U

#define AIC_IEEE80211_VHT_CAP_MAX_MPDU_7991   0x00000001U
#define AIC_IEEE80211_VHT_CAP_RXLDPC          0x00000010U
#define AIC_IEEE80211_VHT_CAP_SHORT_GI_80     0x00000020U
#define AIC_IEEE80211_VHT_CAP_RXSTBC_1        0x00000100U
#define AIC_IEEE80211_VHT_CAP_SU_BEAMFORMEE   0x00001000U
#define AIC_IEEE80211_VHT_CAP_BFMEE_STS_SHIFT 13U
#define AIC_IEEE80211_VHT_CAP_MAX_AMPDU_MASK  0x03800000U

#define AIC_IEEE80211_VHT_MCS_SUPPORT_0_9     0x0002U
#define AIC_IEEE80211_VHT_MCS_NOT_SUPPORTED   0x0003U
#define AIC_IEEE80211_HE_MCS_SUPPORT_0_11     0x0002U
#define AIC_IEEE80211_HE_MCS_NOT_SUPPORTED    0x0003U

#define AIC_HE_MAC_CAP2_ALL_ACK                         0x04U
#define AIC_HE_PHY_CAP0_BW40_IN_2G                      0x02U
#define AIC_HE_PHY_CAP0_BW40_80_IN_5G                   0x04U
#define AIC_HE_PHY_CAP1_LDPC_IN_PAYLOAD                 0x20U
#define AIC_HE_PHY_CAP1_LTF_GI_0_8US                    0x40U
#define AIC_HE_PHY_CAP1_MIDAMBLE_NSTS                   0x80U
#define AIC_HE_PHY_CAP2_MIDAMBLE_NSTS                   0x01U
#define AIC_HE_PHY_CAP2_NDP_4X_3_2US                    0x02U
#define AIC_HE_PHY_CAP2_STBC_RX_UNDER_80                0x08U
#define AIC_HE_PHY_CAP2_DOPPLER_RX                      0x10U
#define AIC_HE_PHY_CAP3_DCM_RX_16QAM                    0x10U
#define AIC_HE_PHY_CAP3_RX_PARTIAL_BW_SU_MU             0x40U
#define AIC_HE_PHY_CAP4_SU_BEAMFORMEE                   0x02U
#define AIC_HE_PHY_CAP4_BFMEE_MAX_STS_4                 0x0CU
#define AIC_HE_PHY_CAP5_NG16_SU_FB                      0x40U
#define AIC_HE_PHY_CAP5_NG16_MU_FB                      0x80U
#define AIC_HE_PHY_CAP6_CODEBOOK_42_SU                  0x01U
#define AIC_HE_PHY_CAP6_CODEBOOK_75_MU                  0x02U
#define AIC_HE_PHY_CAP6_TRIG_SU_FB                      0x04U
#define AIC_HE_PHY_CAP6_TRIG_MU_PARTIAL_BW_FB           0x08U
#define AIC_HE_PHY_CAP6_PARTIAL_BW_DL_MUMIMO            0x40U
#define AIC_HE_PHY_CAP6_PPE_THRESHOLD_PRESENT           0x80U
#define AIC_HE_PHY_CAP8_20_IN_40_HE_2G                  0x02U
#define AIC_HE_PHY_CAP9_RX_FULL_BW_SU_MU_COMP_SIGB      0x02U
#define AIC_HE_PHY_CAP9_RX_FULL_BW_SU_MU_NONCOMP_SIGB   0x04U

#define AIC_D80_DEFAULT_TX_POWER_DBM                    20
#define AIC_LMAC_MAX_PARAM_BYTES                        (AIC_IPC_E2A_MSG_PARAM_SIZE * 4U)
#define AIC_LMAC_MAX_PRINT_LEN                          1024U

/* ==========================================================================
 * Freestanding Zero / Copy Helpers (Safe under -mkernel -nostdlib)
 * ========================================================================== */

static inline void aic_lmac_zero(void *dst, size_t len) {
    uint8_t *p = (uint8_t *)dst;
    for (size_t i = 0; i < len; ++i) {
        p[i] = 0;
    }
}

static inline void aic_lmac_copy(void *dst, const void *src, size_t len) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < len; ++i) {
        d[i] = s[i];
    }
}

/* ==========================================================================
 * Stage 2 AIC8800D80 LMAC Request Builders (Bit-Identical to Linux rwnx_msg_tx)
 * ========================================================================== */

/**
 * Populate MM_SET_TXPWR_IDX_LVL_REQ with AIC8800D80 v3 default power tables
 * from aic_userconfig_8800d80.txt inside the full 95-byte union payload.
 */
static inline void aic_fill_d80_txpwr_lvl_v3(aic_mm_set_txpwr_lvl_req_t *req) {
    static const int8_t k2g4_11b_11ag[12] = {18, 18, 18, 18, 18, 18, 18, 18, 16, 16, 15, 15};
    static const int8_t k2g4_11n_11ac[10] = {18, 18, 18, 18, 16, 16, 15, 15, 14, 14};
    static const int8_t k2g4_11ax[12]     = {18, 18, 18, 18, 16, 16, 15, 15, 14, 14, 13, 13};
    static const int8_t k5g_11a[12]       = {18, 18, 18, 18, 16, 16, 15, 15,  0,  0,  0,  0};
    static const int8_t k5g_11n_11ac[10]  = {18, 18, 18, 18, 16, 16, 15, 15, 14, 14};
    static const int8_t k5g_11ax[12]      = {18, 18, 18, 18, 16, 16, 14, 14, 13, 13, 12, 12};

    aic_lmac_zero(req, sizeof(*req));
    req->txpwr_lvl_v3.enable = 1U;
    aic_lmac_copy(req->txpwr_lvl_v3.pwrlvl_11b_11ag_2g4, k2g4_11b_11ag, sizeof(k2g4_11b_11ag));
    aic_lmac_copy(req->txpwr_lvl_v3.pwrlvl_11n_11ac_2g4, k2g4_11n_11ac, sizeof(k2g4_11n_11ac));
    aic_lmac_copy(req->txpwr_lvl_v3.pwrlvl_11ax_2g4,     k2g4_11ax,     sizeof(k2g4_11ax));
    aic_lmac_copy(req->txpwr_lvl_v3.pwrlvl_11a_5g,       k5g_11a,       sizeof(k5g_11a));
    aic_lmac_copy(req->txpwr_lvl_v3.pwrlvl_11n_11ac_5g,  k5g_11n_11ac,  sizeof(k5g_11n_11ac));
    aic_lmac_copy(req->txpwr_lvl_v3.pwrlvl_11ax_5g,      k5g_11ax,      sizeof(k5g_11ax));
}

/**
 * Populate MM_SET_RF_CALIB_REQ for AIC8800D80 (PRODUCT_ID_AIC8800D81).
 */
static inline void aic_build_d80_rf_calib_req(aic_mm_set_rf_calib_req_t *req) {
    aic_lmac_zero(req, sizeof(*req));
    req->cal_cfg_24g    = AIC_D80_CAL_CFG_24G;
    req->cal_cfg_5g     = AIC_D80_CAL_CFG_5G;
    req->param_alpha    = AIC_D80_PARAM_ALPHA;
    req->bt_calib_en    = 0U;
    req->bt_calib_param = AIC_D80_BT_CALIB_PARAM;
    req->xtal_cap       = 0U;
    req->xtal_cap_fine  = 0U;
}

/**
 * Build ME_CONFIG_REQ matching D80 1x1 80MHz 802.11ax STA parameters and
 * gating VHT/HE/PS/DPSM on firmware feature flags from MM_VERSION_CFM.
 */
static inline void aic_build_d80_me_config_req(aic_me_config_req_t *req,
                                               uint32_t fw_features,
                                               uint8_t is_5g_support) {
    (void)is_5g_support;
    aic_lmac_zero(req, sizeof(*req));

    /* HT capability: 1SS MCS 0..7, 40 MHz, LDPC, SGI 20/40, 1SS RX STBC */
    req->ht_cap.ht_capa_info = (uint16_t)(AIC_IEEE80211_HT_CAP_LDPC_CODING |
                                          AIC_IEEE80211_HT_CAP_SUP_WIDTH_20_40 |
                                          AIC_IEEE80211_HT_CAP_SM_PS_DISABLED |
                                          AIC_IEEE80211_HT_CAP_SGI_20 |
                                          AIC_IEEE80211_HT_CAP_SGI_40 |
                                          AIC_IEEE80211_HT_CAP_RX_STBC_1SS |
                                          AIC_IEEE80211_HT_CAP_DSSSCCK40);
    req->ht_cap.a_mpdu_param = 0x1FU; /* max A-MPDU exponent=3 (64K), spacing=7 */
    req->ht_cap.mcs_rate[0]  = 0xFFU; /* MCS 0..7 enabled on 1SS */
    req->ht_cap.mcs_rate[12] = 0x01U; /* TX MCS set defined */

    /* VHT capability: 1SS MCS 0..9, 80 MHz */
    req->vht_cap.vht_capa_info =
        AIC_IEEE80211_VHT_CAP_MAX_MPDU_7991 |
        AIC_IEEE80211_VHT_CAP_RXLDPC |
        AIC_IEEE80211_VHT_CAP_SHORT_GI_80 |
        AIC_IEEE80211_VHT_CAP_RXSTBC_1 |
        AIC_IEEE80211_VHT_CAP_SU_BEAMFORMEE |
        (3U << AIC_IEEE80211_VHT_CAP_BFMEE_STS_SHIFT) |
        AIC_IEEE80211_VHT_CAP_MAX_AMPDU_MASK;

    const uint16_t vht_mcs_1ss =
        (uint16_t)(AIC_IEEE80211_VHT_MCS_SUPPORT_0_9 |
                   (AIC_IEEE80211_VHT_MCS_NOT_SUPPORTED << 2U) |
                   (AIC_IEEE80211_VHT_MCS_NOT_SUPPORTED << 4U) |
                   (AIC_IEEE80211_VHT_MCS_NOT_SUPPORTED << 6U) |
                   (AIC_IEEE80211_VHT_MCS_NOT_SUPPORTED << 8U) |
                   (AIC_IEEE80211_VHT_MCS_NOT_SUPPORTED << 10U) |
                   (AIC_IEEE80211_VHT_MCS_NOT_SUPPORTED << 12U) |
                   (AIC_IEEE80211_VHT_MCS_NOT_SUPPORTED << 14U));
    req->vht_cap.rx_mcs_map = vht_mcs_1ss;
    req->vht_cap.tx_mcs_map = vht_mcs_1ss;
    req->vht_cap.rx_highest = 0U;
    req->vht_cap.tx_highest = 0U;

    /* HE capability: 1SS MCS 0..11, 80 MHz */
    req->he_cap.mac_cap_info[2] = AIC_HE_MAC_CAP2_ALL_ACK;
    req->he_cap.phy_cap_info[0] = AIC_HE_PHY_CAP0_BW40_IN_2G | AIC_HE_PHY_CAP0_BW40_80_IN_5G;
    req->he_cap.phy_cap_info[1] = AIC_HE_PHY_CAP1_LDPC_IN_PAYLOAD |
                                  AIC_HE_PHY_CAP1_LTF_GI_0_8US |
                                  AIC_HE_PHY_CAP1_MIDAMBLE_NSTS;
    req->he_cap.phy_cap_info[2] = AIC_HE_PHY_CAP2_MIDAMBLE_NSTS |
                                  AIC_HE_PHY_CAP2_NDP_4X_3_2US |
                                  AIC_HE_PHY_CAP2_STBC_RX_UNDER_80 |
                                  AIC_HE_PHY_CAP2_DOPPLER_RX;
    req->he_cap.phy_cap_info[3] = AIC_HE_PHY_CAP3_DCM_RX_16QAM | AIC_HE_PHY_CAP3_RX_PARTIAL_BW_SU_MU;
    req->he_cap.phy_cap_info[4] = AIC_HE_PHY_CAP4_SU_BEAMFORMEE | AIC_HE_PHY_CAP4_BFMEE_MAX_STS_4;
    req->he_cap.phy_cap_info[5] = AIC_HE_PHY_CAP5_NG16_SU_FB | AIC_HE_PHY_CAP5_NG16_MU_FB;
    req->he_cap.phy_cap_info[6] = AIC_HE_PHY_CAP6_CODEBOOK_42_SU |
                                  AIC_HE_PHY_CAP6_CODEBOOK_75_MU |
                                  AIC_HE_PHY_CAP6_TRIG_SU_FB |
                                  AIC_HE_PHY_CAP6_TRIG_MU_PARTIAL_BW_FB |
                                  AIC_HE_PHY_CAP6_PARTIAL_BW_DL_MUMIMO |
                                  AIC_HE_PHY_CAP6_PPE_THRESHOLD_PRESENT;
    req->he_cap.phy_cap_info[8] = AIC_HE_PHY_CAP8_20_IN_40_HE_2G;
    req->he_cap.phy_cap_info[9] = AIC_HE_PHY_CAP9_RX_FULL_BW_SU_MU_COMP_SIGB |
                                  AIC_HE_PHY_CAP9_RX_FULL_BW_SU_MU_NONCOMP_SIGB;

    const uint16_t he_mcs_1ss =
        (uint16_t)(AIC_IEEE80211_HE_MCS_SUPPORT_0_11 |
                   (AIC_IEEE80211_HE_MCS_NOT_SUPPORTED << 2U) |
                   (AIC_IEEE80211_HE_MCS_NOT_SUPPORTED << 4U) |
                   (AIC_IEEE80211_HE_MCS_NOT_SUPPORTED << 6U) |
                   (AIC_IEEE80211_HE_MCS_NOT_SUPPORTED << 8U) |
                   (AIC_IEEE80211_HE_MCS_NOT_SUPPORTED << 10U) |
                   (AIC_IEEE80211_HE_MCS_NOT_SUPPORTED << 12U) |
                   (AIC_IEEE80211_HE_MCS_NOT_SUPPORTED << 14U));
    req->he_cap.mcs_supp.rx_mcs_80    = he_mcs_1ss;
    req->he_cap.mcs_supp.tx_mcs_80    = he_mcs_1ss;
    req->he_cap.mcs_supp.rx_mcs_160   = 0xFFFFU;
    req->he_cap.mcs_supp.tx_mcs_160   = 0xFFFFU;
    req->he_cap.mcs_supp.rx_mcs_80p80 = 0xFFFFU;
    req->he_cap.mcs_supp.tx_mcs_80p80 = 0xFFFFU;
    req->he_cap.ppe_thres[0]          = 0x38U;
    req->he_cap.ppe_thres[1]          = 0x1CU;
    req->he_cap.ppe_thres[2]          = 0xC7U;
    req->he_cap.ppe_thres[3]          = 0x01U;

    req->tx_lft     = AIC_RWNX_TX_LIFETIME_MS;
    req->phy_bw_max = AIC_PHY_CHNL_BW_80;
    req->ht_supp    = 1U;
    req->vht_supp   = ((fw_features & (1U << AIC_MM_FEAT_VHT_BIT)) != 0U) ? 1U : 0U;
    req->he_supp    = ((fw_features & (1U << AIC_MM_FEAT_HE_BIT))  != 0U) ? 1U : 0U;
    req->he_ul_on   = req->he_supp;
    req->ps_on      = ((fw_features & (1U << AIC_MM_FEAT_PS_BIT))   != 0U) ? 1U : 0U;
    req->ant_div_on = 0U;
    req->dpsm       = ((fw_features & (1U << AIC_MM_FEAT_DPSM_BIT)) != 0U && req->ps_on) ? 1U : 0U;
}

/**
 * Populate ME_CHAN_CONFIG_REQ with Linux default 14 x 2.4 GHz channels and
 * (when include_5g != 0) 25 x 5 GHz channels with DFS markers on 52..64 and 100..144.
 */
static inline void aic_build_d80_me_chan_config_req(aic_me_chan_config_req_t *req,
                                                    uint8_t include_5g) {
    static const uint16_t kChan2g4Freqs[14] = {
        2412, 2417, 2422, 2427, 2432, 2437, 2442,
        2447, 2452, 2457, 2462, 2467, 2472, 2484
    };
    static const uint16_t kChan5gFreqs[25] = {
        5180, 5200, 5220, 5240, 5260, 5280, 5300, 5320,
        5500, 5520, 5540, 5560, 5580, 5600, 5620, 5640, 5660, 5680, 5700, 5720,
        5745, 5765, 5785, 5805, 5825
    };

    aic_lmac_zero(req, sizeof(*req));

    req->chan2G4_cnt = 14U;
    for (uint8_t i = 0; i < req->chan2G4_cnt; ++i) {
        req->chan2G4[i].freq     = kChan2g4Freqs[i];
        req->chan2G4[i].band     = AIC_PHY_BAND_2G4;
        req->chan2G4[i].tx_power = AIC_D80_DEFAULT_TX_POWER_DBM;
        req->chan2G4[i].flags    = (i >= 11U) ? AIC_CHAN_NO_IR : 0U;
    }

    if (include_5g != 0U) {
        req->chan5G_cnt = 25U;
        for (uint8_t i = 0; i < req->chan5G_cnt; ++i) {
            const uint16_t f = kChan5gFreqs[i];
            const bool is_dfs = ((f >= 5260U && f <= 5320U) || (f >= 5500U && f <= 5720U));
            req->chan5G[i].freq     = f;
            req->chan5G[i].band     = AIC_PHY_BAND_5G;
            req->chan5G[i].tx_power = AIC_D80_DEFAULT_TX_POWER_DBM;
            req->chan5G[i].flags    = is_dfs ? (uint8_t)(AIC_CHAN_NO_IR | AIC_CHAN_RADAR) : 0U;
        }
    }
}

static inline void aic_build_d80_mm_start_req(aic_mm_start_req_t *req) {
    aic_lmac_zero(req, sizeof(*req));
    req->uapsd_timeout   = AIC_RWNX_UAPSD_TIMEOUT_MS;
    req->lp_clk_accuracy = AIC_RWNX_LP_CLK_PPM;
}

static inline void aic_build_d80_set_coex_req(aic_mm_set_coex_req_t *req) {
    aic_lmac_zero(req, sizeof(*req));
    req->bt_on                 = 1U;
    req->disable_coexnull      = 0U;
    req->enable_nullcts        = 1U;
    req->enable_periodic_timer = 0U;
    req->coex_timeslot_set     = 0U;
}

static inline void aic_build_d80_add_if_sta_req(aic_mm_add_if_req_t *req,
                                                  const uint8_t mac[6]) {
    aic_lmac_zero(req, sizeof(*req));
    req->type = AIC_VIF_STA;
    aic_lmac_copy(req->addr.array, mac, 6U);
    req->p2p  = 0U;
}

static inline void aic_build_d80_agg_disable_req(aic_mm_set_agg_disable_req_t *req) {
    aic_lmac_zero(req, sizeof(*req));
    req->disable    = 1U;
    req->staidx     = 0U;
    req->disable_rx = 1U;
}

/* ==========================================================================
 * Zero-Heap LMAC Command Matcher & Bulk-In Msg Demuxer
 * ========================================================================== */

enum aic_msg_rx_outcome {
    AIC_MSG_RX_MATCHED_CFM = 0,
    AIC_MSG_RX_ASYNC_IND   = 1,
    AIC_MSG_RX_DATA_CFM    = 2,
    AIC_MSG_RX_PRINT       = 3,
    AIC_MSG_RX_INVALID     = -1,
};

typedef struct {
    bool     waiting;
    uint16_t expected_cfm_id;
    uint16_t expected_dest_id;
    bool     completed;
    int      status;
    uint16_t rx_param_len;
    uint8_t  rx_param[AIC_LMAC_MAX_PARAM_BYTES];

    uint64_t matched_cfm_count;
    uint64_t async_ind_count;
    uint64_t bad_pattern_count;
    uint64_t data_cfm_count;
    uint64_t fw_print_count;
    uint64_t invalid_pkt_count;
} aic_lmac_cmd_matcher_t;

static inline void aic_lmac_cmd_matcher_init(aic_lmac_cmd_matcher_t *m) {
    aic_lmac_zero(m, sizeof(*m));
    m->expected_dest_id = AIC_DRV_TASK_ID;
}

static inline void aic_lmac_cmd_matcher_arm(aic_lmac_cmd_matcher_t *m,
                                            uint16_t expected_cfm_id) {
    m->waiting          = true;
    m->expected_cfm_id  = expected_cfm_id;
    m->expected_dest_id = AIC_DRV_TASK_ID;
    m->completed        = false;
    m->status           = -1;
    m->rx_param_len     = 0U;
}

static inline void aic_lmac_cmd_matcher_disarm(aic_lmac_cmd_matcher_t *m) {
    m->waiting = false;
}

/**
 * Consume an E2A IPC message payload (stripped of the 4-byte USB header).
 *
 * Over USB (AIC8800D80 Stage 2 FMAC), firmware matches Linux rwnx_cmds.c
 * (`cmd->reqid == msg->id`): it does not populate PCIe shared-RAM pattern
 * (0xADDEDE2A) or dummy_dest_id (DRV_TASK_ID), leaving arbitrary values in both
 * fields. Match strictly on `hdr.id == m->expected_cfm_id` and bounds-check
 * `hdr.param_len`; count non-SHM patterns in `bad_pattern_count` as telemetry.
 */
static inline int aic_lmac_cmd_on_e2a_msg(aic_lmac_cmd_matcher_t *m,
                                          const uint8_t *payload,
                                          size_t payload_len,
                                          uint16_t *out_msg_id) {
    if (!m || !payload || payload_len < sizeof(aic_ipc_e2a_msg_hdr_t)) {
        if (m) {
            m->invalid_pkt_count++;
        }
        return AIC_MSG_RX_INVALID;
    }

    aic_ipc_e2a_msg_hdr_t hdr;
    aic_lmac_copy(&hdr, payload, sizeof(hdr));
    if (out_msg_id) {
        *out_msg_id = hdr.id;
    }

    if (hdr.pattern != AIC_IPC_MSGE2A_VALID_PAT) {
        m->bad_pattern_count++;
    }

    if ((size_t)hdr.param_len > payload_len - sizeof(hdr) ||
        (size_t)hdr.param_len > sizeof(m->rx_param)) {
        m->invalid_pkt_count++;
        if (m->waiting && hdr.id == m->expected_cfm_id) {
            m->rx_param_len = 0U;
            m->status       = -2;
            m->completed    = true;
            m->waiting      = false;
        }
        return AIC_MSG_RX_INVALID;
    }

    if (m->waiting && hdr.id == m->expected_cfm_id) {
        m->rx_param_len = hdr.param_len;
        if (hdr.param_len > 0U) {
            aic_lmac_copy(m->rx_param, payload + sizeof(hdr), hdr.param_len);
        }
        m->status    = 0;
        m->completed = true;
        m->waiting   = false;
        m->matched_cfm_count++;
        return AIC_MSG_RX_MATCHED_CFM;
    }

    m->async_ind_count++;
    return AIC_MSG_RX_ASYNC_IND;
}

typedef struct {
    aic_lmac_cmd_matcher_t *matcher;
    aic_usb_data_cfm_t      last_data_cfm;
    bool                    has_data_cfm;
    bool                    matched_cmd_cfm;
    uint16_t                last_msg_id;
    const uint8_t          *print_ptr;
    uint16_t                print_len;
} aic_msg_rx_dispatch_t;

static inline int aic_usb_handle_ctrl_pkt(aic_msg_rx_dispatch_t *d,
                                          uint8_t raw_type,
                                          const uint8_t *payload,
                                          uint16_t payload_len) {
    const uint8_t type = (uint8_t)(raw_type & 0x7FU);
    if (!d || !d->matcher) {
        return AIC_MSG_RX_INVALID;
    }

    if (type == AIC_USB_TYPE_CFG_CMD_RSP) {
        uint16_t msg_id = 0U;
        const int rc = aic_lmac_cmd_on_e2a_msg(d->matcher, payload, payload_len, &msg_id);
        d->last_msg_id = msg_id;
        if (rc == AIC_MSG_RX_MATCHED_CFM) {
            d->matched_cmd_cfm = true;
        }
        return rc;
    }

    if (type == AIC_USB_TYPE_CFG_DATA_CFM) {
        if (payload_len != sizeof(aic_usb_data_cfm_t)) {
            d->matcher->invalid_pkt_count++;
            return AIC_MSG_RX_INVALID;
        }
        aic_lmac_copy(&d->last_data_cfm, payload, sizeof(aic_usb_data_cfm_t));
        d->has_data_cfm = true;
        d->matcher->data_cfm_count++;
        return AIC_MSG_RX_DATA_CFM;
    }

    if (type == AIC_USB_TYPE_CFG_PRINT) {
        d->print_ptr = payload;
        d->print_len = (payload_len > AIC_LMAC_MAX_PRINT_LEN)
                           ? (uint16_t)AIC_LMAC_MAX_PRINT_LEN
                           : payload_len;
        d->matcher->fw_print_count++;
        return AIC_MSG_RX_PRINT;
    }

    d->matcher->invalid_pkt_count++;
    return AIC_MSG_RX_INVALID;
}

/**
 * Parse a raw USB MSG/IN buffer from bulkInMsgPipe_ into CMD_RSP / DATA_CFM / PRINT.
 * Supports single CFG frames and 4-byte-padded multi-packet MSG URBs cleanly.
 */
static inline int aic_usb_dispatch_msg_urb(aic_lmac_cmd_matcher_t *matcher,
                                           const uint8_t *urb_buf,
                                           size_t urb_len,
                                           aic_msg_rx_dispatch_t *out_summary) {
    if (!matcher || !urb_buf || urb_len < 4U) {
        if (matcher) {
            matcher->invalid_pkt_count++;
        }
        return -1;
    }

    aic_msg_rx_dispatch_t local;
    aic_lmac_zero(&local, sizeof(local));
    local.matcher = matcher;

    size_t off = 0U;
    size_t packets = 0U;
    while (off + 4U <= urb_len) {
        const uint16_t pkt_len = aic_get_le16(&urb_buf[off]);
        const uint8_t raw_type = urb_buf[off + 2U];

        if (pkt_len == 0U || (size_t)pkt_len > urb_len - (off + 4U)) {
            if (packets == 0U) {
                matcher->invalid_pkt_count++;
                return -1;
            }
            break;
        }

        (void)aic_usb_handle_ctrl_pkt(&local, raw_type, urb_buf + off + 4U, pkt_len);
        packets++;

        const size_t adv = 4U + (size_t)((pkt_len + 3U) & ~3U);
        if (adv <= 4U || adv > urb_len - off) {
            break;
        }
        off += adv;
    }

    if (out_summary) {
        *out_summary = local;
    }
    return (packets > 0U) ? 0 : -1;
}

/* ==========================================================================
 * TX Flow Control Watermark Hysteresis (High=150, Low=50, Cap=200)
 * ========================================================================== */

typedef struct {
    uint16_t capacity;
    uint16_t low_water;
    uint16_t high_water;
    bool     stalled;
    uint64_t stall_events;
    uint64_t wake_events;
} aic_tx_flow_ctrl_t;

static inline void aic_tx_flow_ctrl_init(aic_tx_flow_ctrl_t *fc,
                                         uint16_t capacity,
                                         uint16_t low_water,
                                         uint16_t high_water) {
    aic_lmac_zero(fc, sizeof(*fc));
    fc->capacity   = capacity;
    fc->low_water  = low_water;
    fc->high_water = high_water;
}

static inline bool aic_tx_flow_ctrl_can_accept(const aic_tx_flow_ctrl_t *fc,
                                               uint16_t in_flight) {
    return (fc != NULL) && !fc->stalled && (in_flight < fc->capacity);
}

static inline bool aic_tx_flow_ctrl_on_acquire(aic_tx_flow_ctrl_t *fc,
                                               uint16_t new_in_flight) {
    if (!fc) {
        return false;
    }
    if (!fc->stalled && new_in_flight >= fc->high_water) {
        fc->stalled = true;
        fc->stall_events++;
    }
    return fc->stalled;
}

/**
 * Returns true if TX queue just transitioned from stalled -> resumed.
 */
static inline bool aic_tx_flow_ctrl_on_release(aic_tx_flow_ctrl_t *fc,
                                               uint16_t new_in_flight) {
    if (!fc) {
        return false;
    }
    if (fc->stalled && new_in_flight <= fc->low_water) {
        fc->stalled = false;
        fc->wake_events++;
        return true;
    }
    return false;
}

#ifdef __cplusplus
}
#endif

#endif /* AIC8800_LMAC_H */
