/*
 * test_phase1.cpp - Phase 1 Verification Suite (ASan/UBSan + NoHeapGuard)
 *
 * Tests Stage 2 AIC8800D80 LMAC builders, MSG URB demuxing (0x11/0x12/0x13),
 * synchronized command matcher, TX watermark flow control hysteresis,
 * and combined TX/RX + ZLP zero-heap invariants across >= 10,000 vectors.
 */

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "aic8800_abi.h"
#include "aic8800_framing.h"
#include "aic8800_lmac.h"

namespace {

thread_local bool     gTrapNewAlloc = false;
thread_local uint64_t gNewAllocCount = 0;

struct NoHeapGuard {
    NoHeapGuard() { gTrapNewAlloc = true; }
    ~NoHeapGuard() { gTrapNewAlloc = false; }
    NoHeapGuard(const NoHeapGuard &) = delete;
    NoHeapGuard &operator=(const NoHeapGuard &) = delete;
};

} // namespace

void *operator new(std::size_t sz) {
    if (gTrapNewAlloc) {
        gNewAllocCount++;
        std::fprintf(stderr, "FATAL: unexpected C++ operator new(%zu) inside hot path\n", sz);
        std::abort();
    }
    void *p = std::malloc(sz ? sz : 1U);
    if (!p) throw std::bad_alloc();
    return p;
}

void *operator new[](std::size_t sz) {
    return ::operator new(sz);
}

void operator delete(void *p) noexcept {
    std::free(p);
}

void operator delete[](void *p) noexcept {
    std::free(p);
}

void operator delete(void *p, std::size_t) noexcept {
    std::free(p);
}

void operator delete[](void *p, std::size_t) noexcept {
    std::free(p);
}

/* Compile-time Stage 2 wire ABI sanity checks */
static_assert(sizeof(aic_mm_set_txpwr_lvl_req_t) == 95, "txpwr_lvl_req size");
static_assert(sizeof(aic_mm_set_rf_calib_req_t) == 24, "rf_calib_req size");
static_assert(sizeof(aic_mm_get_mac_addr_req_t) == 4, "get_mac_addr_req size");
static_assert(sizeof(aic_mm_get_mac_addr_cfm_t) == 6, "get_mac_addr_cfm size");
static_assert(sizeof(aic_mm_set_stack_start_req_t) == 4, "set_stack_start_req size");
static_assert(sizeof(aic_mm_set_stack_start_cfm_t) == 2, "set_stack_start_cfm size");
static_assert(sizeof(aic_mm_get_fw_version_cfm_t) == 64, "get_fw_version_cfm size");
static_assert(sizeof(aic_usb_data_cfm_t) == 8, "usb_data_cfm size");
static_assert(sizeof(aic_me_config_req_t) == 112, "me_config_req size");
static_assert(sizeof(aic_me_chan_config_req_t) == 254, "me_chan_config_req size");

static size_t append_msg_pkt(uint8_t *dst, size_t dst_cap, uint8_t type, const void *payload, uint16_t payload_len) {
    const size_t aligned = ((size_t)payload_len + 3U) & ~(size_t)3U;
    assert(4U + aligned <= dst_cap);
    aic_put_le16(&dst[0], payload_len);
    dst[2] = type;
    dst[3] = 0x00U;
    if (payload_len > 0U) {
        std::memcpy(&dst[4], payload, payload_len);
    }
    if (aligned > payload_len) {
        std::memset(&dst[4U + payload_len], 0, aligned - payload_len);
    }
    return 4U + aligned;
}

static size_t build_e2a_bytes(uint8_t *out, size_t out_cap,
                              uint16_t id, uint16_t dest_id, uint16_t src_id,
                              uint32_t pattern,
                              const void *param, uint16_t param_len) {
    assert(sizeof(aic_ipc_e2a_msg_hdr_t) + param_len <= out_cap);
    aic_ipc_e2a_msg_hdr_t hdr{};
    hdr.id            = id;
    hdr.dummy_dest_id = dest_id;
    hdr.dummy_src_id  = src_id;
    hdr.param_len     = param_len;
    hdr.pattern       = pattern;
    std::memcpy(out, &hdr, sizeof(hdr));
    if (param_len > 0U && param) {
        std::memcpy(out + sizeof(hdr), param, param_len);
    }
    return sizeof(hdr) + param_len;
}

static uint64_t test_stage2_d80_builders() {
    uint64_t vectors = 0;

    /* 1. TX power v3 union */
    aic_mm_set_txpwr_lvl_req_t pwr{};
    std::memset(&pwr, 0xAA, sizeof(pwr));
    aic_fill_d80_txpwr_lvl_v3(&pwr);
    assert(pwr.txpwr_lvl_v3.enable == 1U);
    assert(pwr.txpwr_lvl_v3.pwrlvl_11b_11ag_2g4[0] == 18);
    assert(pwr.txpwr_lvl_v3.pwrlvl_11b_11ag_2g4[11] == 15);
    assert(pwr.txpwr_lvl_v3.pwrlvl_11a_5g[8] == 0);
    assert(pwr.txpwr_lvl_v3.pwrlvl_11ax_5g[11] == 12);
    const uint8_t *raw_pwr = reinterpret_cast<const uint8_t *>(&pwr);
    for (size_t i = sizeof(aic_txpwr_lvl_conf_v3_t); i < sizeof(pwr); ++i) {
        assert(raw_pwr[i] == 0U);
    }
    vectors++;

    /* 2. RF calib D80 */
    aic_mm_set_rf_calib_req_t calib{};
    aic_build_d80_rf_calib_req(&calib);
    assert(calib.cal_cfg_24g == AIC_D80_CAL_CFG_24G);
    assert(calib.cal_cfg_5g == AIC_D80_CAL_CFG_5G);
    assert(calib.param_alpha == AIC_D80_PARAM_ALPHA);
    assert(calib.bt_calib_en == 0U);
    assert(calib.bt_calib_param == AIC_D80_BT_CALIB_PARAM);
    assert(calib.xtal_cap == 0U && calib.xtal_cap_fine == 0U);
    vectors++;

    /* 3. ME_CONFIG_REQ feature gating */
    aic_me_config_req_t me_full{};
    const uint32_t all_feats = (1U << AIC_MM_FEAT_VHT_BIT) |
                               (1U << AIC_MM_FEAT_HE_BIT) |
                               (1U << AIC_MM_FEAT_PS_BIT) |
                               (1U << AIC_MM_FEAT_DPSM_BIT) |
                               (1U << AIC_MM_FEAT_BFMEE_BIT);
    aic_build_d80_me_config_req(&me_full, all_feats, 1U);
    assert(me_full.ht_supp == 1U && me_full.vht_supp == 1U && me_full.he_supp == 1U);
    assert(me_full.he_ul_on == 1U && me_full.ps_on == 1U && me_full.dpsm == 1U);
    assert(me_full.phy_bw_max == AIC_PHY_CHNL_BW_80);
    assert(me_full.tx_lft == AIC_RWNX_TX_LIFETIME_MS);
    assert(me_full.ht_cap.mcs_rate[0] == 0xFFU && me_full.ht_cap.mcs_rate[12] == 0x01U);
    assert(me_full.vht_cap.rx_mcs_map == 0xFFFEU && me_full.vht_cap.tx_mcs_map == 0xFFFEU);
    assert(me_full.he_cap.mcs_supp.rx_mcs_80 == 0xFFFEU);
    assert(me_full.he_cap.mcs_supp.tx_mcs_80 == 0xFFFEU);
    assert(me_full.he_cap.ppe_thres[0] == 0x38U && me_full.he_cap.ppe_thres[1] == 0x1CU &&
           me_full.he_cap.ppe_thres[2] == 0xC7U && me_full.he_cap.ppe_thres[3] == 0x01U);
    vectors++;

    aic_me_config_req_t me_min{};
    aic_build_d80_me_config_req(&me_min, (1U << AIC_MM_FEAT_DPSM_BIT), 0U);
    assert(me_min.ht_supp == 1U && me_min.vht_supp == 0U && me_min.he_supp == 0U);
    assert(me_min.ps_on == 0U && me_min.dpsm == 0U);
    vectors++;

    /* 4. ME_CHAN_CONFIG_REQ 2.4G + 5G + DFS validation */
    aic_me_chan_config_req_t ch{};
    aic_build_d80_me_chan_config_req(&ch, 1U);
    assert(ch.chan2G4_cnt == 14U && ch.chan5G_cnt == 25U);
    assert(ch.chan2G4[0].freq == 2412U && ch.chan2G4[0].flags == 0U && ch.chan2G4[0].band == AIC_PHY_BAND_2G4);
    assert(ch.chan2G4[10].freq == 2462U && ch.chan2G4[10].flags == 0U);
    assert(ch.chan2G4[11].freq == 2467U && ch.chan2G4[11].flags == AIC_CHAN_NO_IR);
    assert(ch.chan2G4[13].freq == 2484U && ch.chan2G4[13].flags == AIC_CHAN_NO_IR);

    int dfs_cnt = 0;
    for (uint8_t i = 0; i < ch.chan5G_cnt; ++i) {
        assert(ch.chan5G[i].band == AIC_PHY_BAND_5G);
        assert(ch.chan5G[i].tx_power == AIC_D80_DEFAULT_TX_POWER_DBM);
        const uint16_t f = ch.chan5G[i].freq;
        const bool expected_dfs = (f >= 5260U && f <= 5320U) || (f >= 5500U && f <= 5720U);
        if (expected_dfs) {
            assert(ch.chan5G[i].flags == (AIC_CHAN_NO_IR | AIC_CHAN_RADAR));
            dfs_cnt++;
        } else {
            assert(ch.chan5G[i].flags == 0U);
        }
    }
    assert(dfs_cnt == 16);
    vectors++;

    aic_me_chan_config_req_t ch_2g{};
    aic_build_d80_me_chan_config_req(&ch_2g, 0U);
    assert(ch_2g.chan2G4_cnt == 14U && ch_2g.chan5G_cnt == 0U);
    assert(ch_2g.chan5G[0].freq == 0U);
    vectors++;

    /* 5. MM_START, MM_SET_COEX, MM_ADD_IF, MM_SET_AGG_DISABLE */
    aic_mm_start_req_t st{};
    aic_build_d80_mm_start_req(&st);
    assert(st.uapsd_timeout == 300U && st.lp_clk_accuracy == 20U);
    aic_mm_set_coex_req_t cx{};
    aic_build_d80_set_coex_req(&cx);
    assert(cx.bt_on == 1U && cx.disable_coexnull == 0U && cx.enable_nullcts == 1U);
    const uint8_t mac[6] = {0x10, 0x22, 0x33, 0x44, 0x55, 0x66};
    aic_mm_add_if_req_t aif{};
    aic_build_d80_add_if_sta_req(&aif, mac);
    assert(aif.type == AIC_VIF_STA && std::memcmp(aif.addr.array, mac, 6U) == 0 && aif.p2p == 0U);
    aic_mm_set_agg_disable_req_t agg{};
    aic_build_d80_agg_disable_req(&agg);
    assert(agg.disable == 1U && agg.staidx == 0U && agg.disable_rx == 1U);
    vectors += 4;

    return vectors;
}

static uint64_t test_msg_urb_matcher_and_demux() {
    uint64_t vectors = 0;
    aic_lmac_cmd_matcher_t matcher{};
    aic_lmac_cmd_matcher_init(&matcher);

    uint8_t e2a_buf[512]{};
    uint8_t urb_buf[1024]{};
    aic_msg_rx_dispatch_t summary{};

    /* Arm for MM_GET_MAC_ADDR_CFM */
    aic_lmac_cmd_matcher_arm(&matcher, AIC_MM_GET_MAC_ADDR_CFM);

    /* 1. Async indication should NOT satisfy armed matcher */
    const uint8_t async_param[4] = {1, 2, 3, 4};
    size_t e2a_len = build_e2a_bytes(e2a_buf, sizeof(e2a_buf),
                                     AIC_SCANU_RESULT_IND, AIC_DRV_TASK_ID, AIC_TASK_SCANU,
                                     AIC_IPC_MSGE2A_VALID_PAT, async_param, sizeof(async_param));
    size_t urb_len = append_msg_pkt(urb_buf, sizeof(urb_buf), AIC_USB_TYPE_CFG_CMD_RSP,
                                    e2a_buf, static_cast<uint16_t>(e2a_len));
    assert(aic_usb_dispatch_msg_urb(&matcher, urb_buf, urb_len, &summary) == 0);
    assert(!summary.matched_cmd_cfm && !matcher.completed && matcher.waiting);
    assert(matcher.async_ind_count == 1U);
    vectors++;

    /* 2. Captured live USB MM_GET_MAC_ADDR_CFM with arbitrary dummy_dest_id & non-SHM pattern MUST match */
    aic_lmac_cmd_matcher_t usb_matcher{};
    aic_lmac_cmd_matcher_init(&usb_matcher);
    aic_lmac_cmd_matcher_arm(&usb_matcher, AIC_MM_GET_MAC_ADDR_CFM);
    const uint8_t hw_mac[6] = {0x90, 0xDE, 0x80, 0x10, 0x4B, 0x09};
    e2a_len = build_e2a_bytes(e2a_buf, sizeof(e2a_buf),
                              AIC_MM_GET_MAC_ADDR_CFM, 0x20B6U, 0x16E3U,
                              0xFEE0B800U, hw_mac, sizeof(hw_mac));
    urb_len = append_msg_pkt(urb_buf, sizeof(urb_buf), AIC_USB_TYPE_CFG_CMD_RSP,
                             e2a_buf, static_cast<uint16_t>(e2a_len));
    assert(aic_usb_dispatch_msg_urb(&usb_matcher, urb_buf, urb_len, &summary) == 0);
    assert(summary.matched_cmd_cfm && usb_matcher.completed && usb_matcher.status == 0);
    assert(usb_matcher.rx_param_len == 6U && std::memcmp(usb_matcher.rx_param, hw_mac, 6U) == 0);
    assert(usb_matcher.bad_pattern_count == 1U && usb_matcher.invalid_pkt_count == 0U);
    vectors++;

    /* 3. Captured live USB DBG_MEM_READ_CFM (CHIP_ID_REG=0xf9078820) + async IND with non-SHM pattern */
    aic_lmac_cmd_matcher_arm(&usb_matcher, AIC_DBG_MEM_READ_CFM);
    const uint8_t chip_cfm_bytes[8] = {0x00, 0x00, 0x50, 0x40, 0x20, 0x88, 0x07, 0xF9};
    e2a_len = build_e2a_bytes(e2a_buf, sizeof(e2a_buf),
                              AIC_DBG_MEM_READ_CFM, 0xF1E1U, 0x4667U,
                              0x7BCECD5FU, chip_cfm_bytes, sizeof(chip_cfm_bytes));
    urb_len = append_msg_pkt(urb_buf, sizeof(urb_buf), AIC_USB_TYPE_CFG_CMD_RSP,
                             e2a_buf, static_cast<uint16_t>(e2a_len));
    assert(aic_usb_dispatch_msg_urb(&usb_matcher, urb_buf, urb_len, &summary) == 0);
    assert(summary.matched_cmd_cfm && usb_matcher.completed && usb_matcher.rx_param_len == 8U);
    assert(aic_get_le32(&usb_matcher.rx_param[4]) == 0xF9078820U);

    e2a_len = build_e2a_bytes(e2a_buf, sizeof(e2a_buf),
                              AIC_MM_VERSION_CFM, 0xD03AU, 0x77C0U,
                              0x150A270BU, hw_mac, sizeof(hw_mac));
    urb_len = append_msg_pkt(urb_buf, sizeof(urb_buf), AIC_USB_TYPE_CFG_CMD_RSP,
                             e2a_buf, static_cast<uint16_t>(e2a_len));
    assert(aic_usb_dispatch_msg_urb(&usb_matcher, urb_buf, urb_len, &summary) == 0);
    assert(!summary.matched_cmd_cfm && usb_matcher.async_ind_count == 1U && usb_matcher.invalid_pkt_count == 0U);
    vectors++;

    /* 4. Multi-packet URB: 0x12 DATA_CFM + 0x13 PRINT + 0x11 matched CMD_RSP */
    size_t off = 0;
    const aic_usb_data_cfm_t dcfm = { 12U, 34U };
    off += append_msg_pkt(&urb_buf[off], sizeof(urb_buf) - off, AIC_USB_TYPE_CFG_DATA_CFM,
                          &dcfm, sizeof(dcfm));
    const char print_msg[] = "D80:ok\n";
    off += append_msg_pkt(&urb_buf[off], sizeof(urb_buf) - off, AIC_USB_TYPE_CFG_PRINT,
                          print_msg, static_cast<uint16_t>(sizeof(print_msg) - 1U));
    const uint8_t real_mac[6] = {0x98, 0x03, 0xCF, 0x11, 0x22, 0x33};
    e2a_len = build_e2a_bytes(e2a_buf, sizeof(e2a_buf),
                              AIC_MM_GET_MAC_ADDR_CFM, AIC_DRV_TASK_ID, AIC_TASK_MM,
                              AIC_IPC_MSGE2A_VALID_PAT, real_mac, sizeof(real_mac));
    off += append_msg_pkt(&urb_buf[off], sizeof(urb_buf) - off, AIC_USB_TYPE_CFG_CMD_RSP,
                          e2a_buf, static_cast<uint16_t>(e2a_len));

    assert(aic_usb_dispatch_msg_urb(&matcher, urb_buf, off, &summary) == 0);
    assert(summary.has_data_cfm && summary.last_data_cfm.queue_idx == 12U && summary.last_data_cfm.used_idx == 34U);
    assert(summary.print_ptr != nullptr && summary.print_len == 7U);
    assert(std::memcmp(summary.print_ptr, "D80:ok\n", 7U) == 0);
    assert(summary.matched_cmd_cfm);
    assert(matcher.completed && matcher.status == 0 && matcher.rx_param_len == 6U);
    assert(std::memcmp(matcher.rx_param, real_mac, 6U) == 0);
    assert(matcher.data_cfm_count == 1U && matcher.fw_print_count == 1U && matcher.matched_cfm_count == 1U);
    vectors++;

    /* 5. Truncated param_len with matching id/dest completes with error */
    aic_lmac_cmd_matcher_arm(&matcher, AIC_MM_VERSION_CFM);
    uint8_t trunc_param[4] = {0, 0, 0, 0};
    e2a_len = build_e2a_bytes(e2a_buf, sizeof(e2a_buf),
                              AIC_MM_VERSION_CFM, AIC_DRV_TASK_ID, AIC_TASK_MM,
                              AIC_IPC_MSGE2A_VALID_PAT, trunc_param, sizeof(trunc_param));
    /* Forge oversized param_len=200 while sending only 16B total */
    e2a_buf[6] = 200U;
    e2a_buf[7] = 0U;
    urb_len = append_msg_pkt(urb_buf, sizeof(urb_buf), AIC_USB_TYPE_CFG_CMD_RSP, e2a_buf, 16U);
    assert(aic_usb_dispatch_msg_urb(&matcher, urb_buf, urb_len, &summary) == 0);
    assert(matcher.completed && matcher.status == -2 && !matcher.waiting);
    vectors++;

    return vectors;
}

static uint64_t test_tx_hysteresis_and_hotpath_loop() {
    uint64_t vectors = 0;

    aic_fixed_slot_pool_t pool{};
    aic_tx_flow_ctrl_t fc{};
    assert(aic_slot_pool_init(&pool, AIC_USB_TX_URBS) == 0);
    aic_tx_flow_ctrl_init(&fc, AIC_USB_TX_URBS, AIC_USB_TX_LOW_WATER, AIC_USB_TX_HIGH_WATER);

    uint16_t acquired[AIC_USB_TX_URBS]{};

    {
        NoHeapGuard guard;

        /* Drive up to HIGH_WATER = 150 */
        for (uint16_t i = 0; i < AIC_USB_TX_HIGH_WATER; ++i) {
            assert(aic_tx_flow_ctrl_can_accept(&fc, aic_slot_pool_in_flight(&pool)));
            assert(aic_slot_pool_acquire(&pool, &acquired[i]) == 0);
            const bool stalled = aic_tx_flow_ctrl_on_acquire(&fc, aic_slot_pool_in_flight(&pool));
            if (i + 1U < AIC_USB_TX_HIGH_WATER) {
                assert(!stalled);
            } else {
                assert(stalled);
            }
            vectors++;
        }

        assert(fc.stalled && fc.stall_events == 1U);
        assert(!aic_tx_flow_ctrl_can_accept(&fc, aic_slot_pool_in_flight(&pool)));

        /* Drain down through hysteresis region 149 -> 51: must stay stalled */
        uint16_t rel_idx = 0U;
        while (aic_slot_pool_in_flight(&pool) > AIC_USB_TX_LOW_WATER + 1U) {
            assert(aic_slot_pool_release(&pool, acquired[rel_idx++]) == 0);
            const bool woke = aic_tx_flow_ctrl_on_release(&fc, aic_slot_pool_in_flight(&pool));
            assert(!woke && fc.stalled);
            vectors++;
        }

        /* Cross LOW_WATER = 50: must wake exactly once */
        assert(aic_slot_pool_release(&pool, acquired[rel_idx++]) == 0);
        assert(aic_slot_pool_in_flight(&pool) == AIC_USB_TX_LOW_WATER);
        assert(aic_tx_flow_ctrl_on_release(&fc, aic_slot_pool_in_flight(&pool)));
        assert(!fc.stalled && fc.wake_events == 1U);
        assert(aic_tx_flow_ctrl_can_accept(&fc, aic_slot_pool_in_flight(&pool)));
        vectors++;

        while (rel_idx < AIC_USB_TX_HIGH_WATER) {
            assert(aic_slot_pool_release(&pool, acquired[rel_idx++]) == 0);
            assert(!aic_tx_flow_ctrl_on_release(&fc, aic_slot_pool_in_flight(&pool)));
        }
        assert(aic_slot_pool_in_flight(&pool) == 0U);

        /* ZLP exact boundary checks */
        assert(aic_usb_needs_zlp(512U, 512U));
        assert(aic_usb_needs_zlp(1024U, 512U));
        assert(!aic_usb_needs_zlp(0U, 512U));
        assert(!aic_usb_needs_zlp(511U, 512U));
        assert(!aic_usb_needs_zlp(513U, 512U));
        vectors += 5;

        /* 10,000 hot-path TX + LMAC msg encode + MSG URB dispatch + RX demux cycles */
        uint8_t eth_in[1514]{};
        for (size_t i = 0; i < sizeof(eth_in); ++i) eth_in[i] = static_cast<uint8_t>(i);
        eth_in[12] = 0x08;
        eth_in[13] = 0x00;

        uint8_t slot_buf[AIC_DATA_BUF_MAX]{};
        uint8_t msg_out[AIC_USB_MSG_MAX_PKT_SIZE]{};
        uint8_t e2a_pkt[64]{};
        uint8_t msg_urb[128]{};
        uint8_t rx_urb[128]{};
        uint8_t scratch[AIC_DATA_BUF_MAX]{};
        aic_lmac_cmd_matcher_t hot_matcher{};
        aic_lmac_cmd_matcher_init(&hot_matcher);

        for (uint32_t iter = 0; iter < 10000U; ++iter) {
            const size_t flen = 60U + (iter % 1440U);
            uint16_t slot = 0U;
            assert(aic_slot_pool_acquire(&pool, &slot) == 0);
            size_t usb_len = 0U;
            assert(aic_tx_encode_eth_frame(eth_in, flen, 0U, 0U, 0U, 0U, 0U, true,
                                           slot_buf, sizeof(slot_buf), &usb_len) == 0);
            assert(usb_len <= sizeof(slot_buf));
            assert(aic_slot_pool_release(&pool, slot) == 0);

            size_t cmd_len = 0U;
            const aic_mm_get_mac_addr_req_t mreq = { 1U };
            assert(aic_encode_lmac_msg(msg_out, sizeof(msg_out),
                                       AIC_MM_GET_MAC_ADDR_REQ, AIC_TASK_MM, AIC_DRV_TASK_ID,
                                       &mreq, sizeof(mreq), &cmd_len) == 0);
            aic_lmac_cmd_matcher_arm(&hot_matcher, AIC_MM_GET_MAC_ADDR_CFM);

            const uint8_t mac[6] = {0x02, 0x00, 0x00, 0x00, static_cast<uint8_t>(iter >> 8), static_cast<uint8_t>(iter)};
            const size_t el = build_e2a_bytes(e2a_pkt, sizeof(e2a_pkt),
                                              AIC_MM_GET_MAC_ADDR_CFM, AIC_DRV_TASK_ID, AIC_TASK_MM,
                                              AIC_IPC_MSGE2A_VALID_PAT, mac, 6U);
            const size_t ul = append_msg_pkt(msg_urb, sizeof(msg_urb), AIC_USB_TYPE_CFG_CMD_RSP,
                                             e2a_pkt, static_cast<uint16_t>(el));
            aic_msg_rx_dispatch_t sum{};
            assert(aic_usb_dispatch_msg_urb(&hot_matcher, msg_urb, ul, &sum) == 0);
            assert(sum.matched_cmd_cfm && hot_matcher.completed && hot_matcher.rx_param[5] == static_cast<uint8_t>(iter));

            /* Valid RX chunk: 60B hw_rxhdr (overlaps 4B USB len/type at [0..3]) + 24B MAC + 8B LLC + 4B payload = 96B */
            std::memset(rx_urb, 0, sizeof(rx_urb));
            const uint16_t hw_frame_len = 36U;
            aic_put_le16(&rx_urb[0], hw_frame_len);
            rx_urb[2] = AIC_USB_TYPE_DATA;
            uint8_t *dot11 = &rx_urb[AIC_RX_HWHDR_LEN];
            dot11[0] = 0x08U; /* Data */
            dot11[1] = 0x02U; /* FromDS */
            dot11[24 + 0] = 0xAA; dot11[24 + 1] = 0xAA; dot11[24 + 2] = 0x03;
            dot11[24 + 6] = 0x08; dot11[24 + 7] = 0x00;

            struct RxSeen { int frames; } seen{0};
            auto on_eth = [](const uint8_t *, size_t elen, const aic_rx_frame_meta_t *, void *ctx) {
                assert(elen == 18U);
                static_cast<RxSeen *>(ctx)->frames++;
            };
            auto on_data = [](const uint8_t *chunk, size_t clen, void *ctx) {
                struct Pair { uint8_t *sc; aic_rx_eth_cb_t cb; RxSeen *s; } *p = static_cast<Pair *>(ctx);
                (void)aic_rx_chunk_to_eth_frames(chunk, clen, p->sc, AIC_DATA_BUF_MAX, p->cb, p->s);
            };
            struct Pair { uint8_t *sc; aic_rx_eth_cb_t cb; RxSeen *s; } pair{scratch, on_eth, &seen};
            assert(aic_rx_demux_usb_aggr(rx_urb, AIC_RX_HWHDR_LEN + hw_frame_len,
                                         on_data, nullptr, &pair) == 1);
            assert(seen.frames == 1);
            vectors++;
        }
    }

    assert(gNewAllocCount == 0U);
    return vectors;
}

int main() {
    uint64_t total = 0U;
    total += test_stage2_d80_builders();
    total += test_msg_urb_matcher_and_demux();
    total += test_tx_hysteresis_and_hotpath_loop();
    std::printf("Phase 1 verification suite passed: %llu vectors tested, cpp_new_allocs=%llu\n",
                static_cast<unsigned long long>(total),
                static_cast<unsigned long long>(gNewAllocCount));
    return 0;
}
