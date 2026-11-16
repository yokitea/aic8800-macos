#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "aic8800_abi.h"
#include "aic8800_framing.h"
#include "aic8800_lmac.h"
#include "aic8800_reord.h"
#include "aic8800_wifi_ctl.h"
#include "aic8800_wpa2.h"

/* Official RFC 4493 AES-128-CMAC key and vectors */
static void test_rfc4493_aes_cmac(void) {
    static const uint8_t key[16] = {
        0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
        0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c
    };
    static const uint8_t msg[64] = {
        0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96,
        0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a,
        0xae, 0x2d, 0x8a, 0x57, 0x1e, 0x03, 0xac, 0x9c,
        0x9e, 0xb7, 0x6f, 0xac, 0x45, 0xaf, 0x8e, 0x51,
        0x30, 0xc8, 0x1c, 0x46, 0xa3, 0x5c, 0xe4, 0x11,
        0xe5, 0xfb, 0xc1, 0x19, 0x1a, 0x0a, 0x52, 0xef,
        0xf6, 0x9f, 0x24, 0x45, 0xdf, 0x4f, 0x9b, 0x17,
        0xad, 0x2b, 0x41, 0x7b, 0xe6, 0x6c, 0x37, 0x10
    };
    static const uint8_t exp0[16] = {
        0xbb, 0x1d, 0x69, 0x29, 0xe9, 0x59, 0x37, 0x28,
        0x7f, 0xa3, 0x7d, 0x12, 0x9b, 0x75, 0x67, 0x46
    };
    static const uint8_t exp16[16] = {
        0x07, 0x0a, 0x16, 0xb4, 0x6b, 0x4d, 0x41, 0x44,
        0xf7, 0x9b, 0xdd, 0x9d, 0xd0, 0x4a, 0x28, 0x7c
    };
    static const uint8_t exp40[16] = {
        0xdf, 0xa6, 0x67, 0x47, 0xde, 0x9a, 0xe6, 0x30,
        0x30, 0xca, 0x32, 0x61, 0x14, 0x97, 0xc8, 0x27
    };
    static const uint8_t exp64[16] = {
        0x51, 0xf0, 0xbe, 0xbf, 0x7e, 0x3b, 0x9d, 0x92,
        0xfc, 0x49, 0x74, 0x17, 0x79, 0x36, 0x3c, 0xfe
    };

    uint8_t mac[16];
    aic_aes128_cmac(key, msg, 0U, mac);
    assert(memcmp(mac, exp0, 16U) == 0);
    aic_aes128_cmac(key, msg, 16U, mac);
    assert(memcmp(mac, exp16, 16U) == 0);
    aic_aes128_cmac(key, msg, 40U, mac);
    assert(memcmp(mac, exp40, 16U) == 0);
    aic_aes128_cmac(key, msg, 64U, mac);
    assert(memcmp(mac, exp64, 16U) == 0);
}

/* Official IEEE 802.11i PBKDF2 vector: pass="password", ssid="IEEE" */
static void test_ieee80211_pbkdf2(void) {
    static const uint8_t expected_psk[32] = {
        0xf4, 0x2c, 0x6f, 0xc5, 0x2d, 0xf0, 0xeb, 0xef,
        0x9e, 0xbb, 0x4b, 0x90, 0xb3, 0x8a, 0x5f, 0x90,
        0x2e, 0x83, 0xfe, 0x1b, 0x13, 0x5a, 0x70, 0xe2,
        0x3a, 0xed, 0x76, 0x2e, 0x97, 0x10, 0xa1, 0x2e
    };
    uint8_t pmk[32] = {0};
    assert(aic_wpa2_derive_pmk("password", 8U, (const uint8_t *)"IEEE", 4U, pmk) == 0);
    assert(memcmp(pmk, expected_psk, 32U) == 0);
}

static size_t build_fake_beacon_ind(uint8_t *buf,
                                    size_t cap,
                                    const uint8_t bssid[6],
                                    const char *ssid,
                                    uint16_t freq,
                                    uint8_t band,
                                    int8_t rssi,
                                    const uint8_t *extra_ie,
                                    size_t extra_ie_len,
                                    bool privacy_bit) {
    assert(cap >= 128U + extra_ie_len);
    const size_t ssid_len = ssid ? strlen(ssid) : 0U;
    uint8_t *mgmt = buf + sizeof(aic_scanu_result_ind_hdr_t);
    memset(mgmt, 0, 36U);
    mgmt[0] = 0x80U; /* Beacon */
    memcpy(&mgmt[10], bssid, 6U);
    memcpy(&mgmt[16], bssid, 6U);
    aic_put_le16(&mgmt[34], privacy_bit ? 0x0411U : 0x0401U);

    size_t mlen = 36U;
    mgmt[mlen++] = 0U;
    mgmt[mlen++] = (uint8_t)ssid_len;
    if (ssid_len > 0U) {
        memcpy(&mgmt[mlen], ssid, ssid_len);
        mlen += ssid_len;
    }
    if (extra_ie && extra_ie_len > 0U) {
        memcpy(&mgmt[mlen], extra_ie, extra_ie_len);
        mlen += extra_ie_len;
    }

    aic_scanu_result_ind_hdr_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.length      = (uint16_t)mlen;
    hdr.framectrl   = 0x0080U;
    hdr.center_freq = freq;
    hdr.band        = band;
    hdr.rssi        = rssi;
    memcpy(buf, &hdr, sizeof(hdr));
    return sizeof(hdr) + mlen;
}

static void test_scanu_parsing(void) {
    aic_scanu_start_req_t sreq;
    aic_build_scanu_start_req(&sreq, 0U, true, (const uint8_t *)"MyWiFi", 6U, false);
    assert(sreq.chan_cnt == 38U && sreq.chan_cnt <= AIC_SCAN_CHANNEL_MAX);
    assert(sreq.ssid_cnt == 1U && sreq.ssid[0].length == 6U);

    uint8_t rsn[22];
    (void)aic_build_wpa2_rsn_ie(rsn, AIC_CIPHER_CCMP, false);
    const uint8_t bssid[6] = {0xAA, 0xBB, 0xCC, 0x11, 0x22, 0x33};
    uint8_t ind[256];
    const size_t ilen = build_fake_beacon_ind(ind, sizeof(ind), bssid, "HomeWiFi5G",
                                              5180U, AIC_PHY_BAND_5G, -42, rsn, sizeof(rsn), true);
    aic_wifi_scan_bss_t parsed;
    assert(aic_parse_scanu_result_ind(ind, ilen, &parsed) == 0);
    assert(memcmp(parsed.bssid, bssid, 6U) == 0);
    assert(parsed.ssid_len == 10U && memcmp(parsed.ssid, "HomeWiFi5G", 10U) == 0);
    assert(parsed.freq == 5180U && parsed.band == AIC_PHY_BAND_5G && parsed.rssi == -42);
    assert((parsed.sec_mask & AIC_SEC_WPA2_PSK) != 0U);
    assert((parsed.sec_mask & AIC_SEC_CCMP) != 0U);
    assert(parsed.pairwise_cipher == AIC_CIPHER_CCMP);

    /* Fuzz truncated RSN IE & oversized TLV length: must never crash under ASan */
    uint8_t bad_ie[8] = {48U, 4U, 0x01U, 0x00U, 0x00U, 0x0FU, 0xDDU, 0xFFU};
    const size_t blen = build_fake_beacon_ind(ind, sizeof(ind), bssid, "FuzzAP",
                                              2412U, AIC_PHY_BAND_2G4, -70, bad_ie, sizeof(bad_ie), true);
    assert(aic_parse_scanu_result_ind(ind, blen, &parsed) == 0);
    assert(parsed.ssid_len == 6U);
}

static size_t build_auth_eapol(uint8_t *out_eth,
                               const uint8_t aa[6],
                               const uint8_t spa[6],
                               uint16_t key_info,
                               uint64_t replay,
                               const uint8_t *nonce,
                               const uint8_t *kd,
                               uint16_t kd_len,
                               const uint8_t *kck_or_null,
                               uint16_t key_ver) {
    const size_t body_len = AIC_EAPOL_KEY_BODY_MIN_LEN + kd_len;
    const size_t pdu_len  = AIC_EAPOL_HDR_LEN + body_len;
    const size_t eth_len  = 14U + pdu_len;
    memset(out_eth, 0, eth_len);
    memcpy(&out_eth[0], spa, 6U);
    memcpy(&out_eth[6], aa, 6U);
    out_eth[12] = 0x88U;
    out_eth[13] = 0x8EU;

    uint8_t *pdu = &out_eth[14];
    pdu[0] = 2U;
    pdu[1] = 3U;
    aic_wpa_put_be16(&pdu[2], (uint16_t)body_len);
    uint8_t *kb = &pdu[4];
    kb[0] = 2U;
    aic_wpa_put_be16(&kb[1], key_info);
    aic_wpa_put_be16(&kb[3], 16U);
    aic_wpa_put_be64(&kb[5], replay);
    if (nonce) memcpy(&kb[13], nonce, 32U);
    aic_wpa_put_be16(&kb[93], kd_len);
    if (kd && kd_len > 0U) memcpy(&kb[95], kd, kd_len);

    if (kck_or_null) {
        uint8_t mic[16];
        assert(aic_wpa2_compute_eapol_mic(key_ver, kck_or_null, pdu, pdu_len, mic) == 0);
        memcpy(&kb[77], mic, 16U);
    }
    return eth_len;
}

static void test_wpa2_4way_and_gtk_rekey(void) {
    const uint8_t aa[6]  = {0xD4, 0x35, 0x1D, 0x22, 0x33, 0x44};
    const uint8_t spa[6] = {0x90, 0xDE, 0x80, 0x10, 0x4B, 0x09};
    const char *ssid     = "TestWPA2Net";
    const char *pass     = "CorrectHorseBatteryStaple";

    uint8_t pmk[32];
    assert(aic_wpa2_derive_pmk(pass, strlen(pass), (const uint8_t *)ssid, strlen(ssid), pmk) == 0);
    uint8_t rsn[22];
    const size_t rsn_len = aic_build_wpa2_rsn_ie(rsn, AIC_CIPHER_CCMP, false);

    aic_wpa2_ctx_t ctx;
    aic_wpa2_ctx_init(&ctx, spa, aa, pmk, rsn, (uint8_t)rsn_len, AIC_CIPHER_CCMP, AIC_CIPHER_CCMP);

    uint8_t anonce[32];
    for (int i = 0; i < 32; ++i) anonce[i] = (uint8_t)(0xA0 + i);

    /* AP sends M1 */
    uint8_t m1[256];
    const size_t m1_len = build_auth_eapol(m1, aa, spa,
                                           AIC_KEY_INFO_VER_HMAC_SHA1 | AIC_KEY_INFO_PAIRWISE | AIC_KEY_INFO_ACK,
                                           1ULL, anonce, NULL, 0U, NULL, 0U);
    uint8_t reply[AIC_WIFI_EAPOL_MAX_FRAME];
    size_t reply_len = 0U;
    bool keys_ready = false, gtk_rekey = false;
    assert(aic_wpa2_handle_rx_eapol(&ctx, m1, m1_len, reply, sizeof(reply), &reply_len, &keys_ready, &gtk_rekey) == 1);
    assert(!keys_ready && ctx.has_ptk && reply_len == 14U + 4U + 95U + rsn_len);

    /* Verify M2 MIC independently as AP */
    uint8_t m2_mic[16];
    assert(aic_wpa2_compute_eapol_mic(AIC_KEY_INFO_VER_HMAC_SHA1, &ctx.ptk[0], &reply[14], reply_len - 14U, m2_mic) == 0);
    assert(memcmp(m2_mic, &reply[14 + 4 + 77], 16U) == 0);

    /* AP wraps GTK-1 inside Key Data using KEK (ptk[16..31]) */
    static const uint8_t gtk1[16] = {0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f};
    uint8_t plain_kd[32] = {0xDD, 22, 0x00, 0x0F, 0xAC, 0x01, 0x01, 0x00};
    memcpy(&plain_kd[8], gtk1, 16U);
    plain_kd[24] = 0xDD; /* padding up to 32B (multiple of 8B) */
    uint8_t wrapped_kd[64];
    size_t wrapped_len = sizeof(wrapped_kd);
    assert(CCSymmetricKeyWrap(kCCWRAPAES, CCrfc3394_iv, CCrfc3394_ivLen,
                              &ctx.ptk[16], 16U, plain_kd, 32U, wrapped_kd, &wrapped_len) == kCCSuccess);

    uint8_t m3[256];
    const uint16_t m3_info = AIC_KEY_INFO_VER_HMAC_SHA1 | AIC_KEY_INFO_PAIRWISE |
                             AIC_KEY_INFO_INSTALL | AIC_KEY_INFO_ACK |
                             AIC_KEY_INFO_MIC | AIC_KEY_INFO_SECURE | AIC_KEY_INFO_ENC_KEY_DATA;
    const size_t m3_len = build_auth_eapol(m3, aa, spa, m3_info, 2ULL, anonce,
                                           wrapped_kd, (uint16_t)wrapped_len, &ctx.ptk[0], AIC_KEY_INFO_VER_HMAC_SHA1);

    assert(aic_wpa2_handle_rx_eapol(&ctx, m3, m3_len, reply, sizeof(reply), &reply_len, &keys_ready, &gtk_rekey) == 1);
    assert(keys_ready && !gtk_rekey && ctx.has_gtk);
    assert(ctx.gtk_idx == 1U && ctx.gtk_len == 16U && memcmp(ctx.gtk, gtk1, 16U) == 0);

    /* Tampered MIC in M3 MUST be rejected */
    m3[m3_len - 5U] ^= 0xFFU;
    assert(aic_wpa2_handle_rx_eapol(&ctx, m3, m3_len, reply, sizeof(reply), &reply_len, &keys_ready, &gtk_rekey) == -1);

    /* Group Key Rekey M1 -> Group M2 */
    static const uint8_t gtk2[16] = {0x20,0x21,0x22,0x23,0x24,0x25,0x26,0x27,0x28,0x29,0x2a,0x2b,0x2c,0x2d,0x2e,0x2f};
    memcpy(&plain_kd[8], gtk2, 16U);
    plain_kd[6] = 0x02U; /* GTK id = 2 */
    wrapped_len = sizeof(wrapped_kd);
    assert(CCSymmetricKeyWrap(kCCWRAPAES, CCrfc3394_iv, CCrfc3394_ivLen,
                              &ctx.ptk[16], 16U, plain_kd, 32U, wrapped_kd, &wrapped_len) == kCCSuccess);

    uint8_t gm1[256];
    const uint16_t gm1_info = AIC_KEY_INFO_VER_HMAC_SHA1 | AIC_KEY_INFO_ACK |
                              AIC_KEY_INFO_MIC | AIC_KEY_INFO_SECURE | AIC_KEY_INFO_ENC_KEY_DATA;
    const size_t gm1_len = build_auth_eapol(gm1, aa, spa, gm1_info, 3ULL, NULL,
                                            wrapped_kd, (uint16_t)wrapped_len, &ctx.ptk[0], AIC_KEY_INFO_VER_HMAC_SHA1);
    assert(aic_wpa2_handle_rx_eapol(&ctx, gm1, gm1_len, reply, sizeof(reply), &reply_len, &keys_ready, &gtk_rekey) == 1);
    assert(gtk_rekey && !keys_ready && ctx.gtk_idx == 2U && memcmp(ctx.gtk, gtk2, 16U) == 0);
}

static int g_async_hits = 0;
static uint16_t g_last_async_id = 0U;

static void on_test_async(uint16_t msg_id, const uint8_t *param, uint16_t param_len, void *ctx) {
    (void)param;
    (void)param_len;
    (void)ctx;
    g_async_hits++;
    g_last_async_id = msg_id;
}

static void test_async_dispatch_ex(void) {
    aic_lmac_cmd_matcher_t matcher;
    aic_lmac_cmd_matcher_init(&matcher);
    aic_lmac_cmd_matcher_arm(&matcher, AIC_SCANU_START_CFM_ADDTIONAL);

    uint8_t payload[sizeof(aic_scanu_result_ind_hdr_t) + 64U];
    memset(payload, 0, sizeof(payload));
    uint8_t e2a[128];
    const size_t e2a_len = sizeof(aic_ipc_e2a_msg_hdr_t) + sizeof(payload);
    aic_ipc_e2a_msg_hdr_t eh = { AIC_SCANU_RESULT_IND, AIC_DRV_TASK_ID, AIC_TASK_SCANU, (uint16_t)sizeof(payload), AIC_IPC_MSGE2A_VALID_PAT };
    memcpy(e2a, &eh, sizeof(eh));
    memcpy(e2a + sizeof(eh), payload, sizeof(payload));

    uint8_t urb[256];
    urb[0] = (uint8_t)(e2a_len & 0xFFU);
    urb[1] = (uint8_t)(e2a_len >> 8U);
    urb[2] = AIC_USB_TYPE_CFG_CMD_RSP;
    urb[3] = 0U;
    memcpy(&urb[4], e2a, e2a_len);

    aic_msg_rx_dispatch_t sum;
    assert(aic_usb_dispatch_msg_urb_ex(&matcher, urb, 4U + e2a_len, &on_test_async, NULL, &sum) == 0);
    assert(g_async_hits == 1 && g_last_async_id == AIC_SCANU_RESULT_IND);
}

struct FakePkt {
    uint16_t sn;
    uint16_t sub;
    FakePkt *next;
};

struct ReordTestCtx {
    uint32_t delivered_sns[512];
    uint32_t delivered_subs[512];
    size_t   delivered_cnt;
    size_t   dropped_cnt;
};

static void fake_deliver(void *head_pkt, uint32_t total_bytes, void *ctx) {
    (void)total_bytes;
    ReordTestCtx *tc = (ReordTestCtx *)ctx;
    FakePkt *cur = (FakePkt *)head_pkt;
    while (cur) {
        assert(tc->delivered_cnt < 512U);
        tc->delivered_sns[tc->delivered_cnt]  = cur->sn;
        tc->delivered_subs[tc->delivered_cnt] = cur->sub;
        tc->delivered_cnt++;
        cur = cur->next;
    }
}

static void fake_append(void *tail_pkt, void *new_pkt) {
    FakePkt *t = (FakePkt *)tail_pkt;
    FakePkt *n = (FakePkt *)new_pkt;
    t->next = n;
    n->next = NULL;
}

static void fake_drop(void *head_pkt, void *ctx) {
    ReordTestCtx *tc = (ReordTestCtx *)ctx;
    FakePkt *cur = (FakePkt *)head_pkt;
    while (cur) {
        tc->dropped_cnt++;
        cur = cur->next;
    }
}

static void test_rx_reorder_window(void) {
    aic_rx_reord_t r;
    aic_rx_reord_init(&r);
    ReordTestCtx tc;
    memset(&tc, 0, sizeof(tc));

    /* 1. 12-bit SN wrap-around out-of-order burst: anchor 4092, then 4095..4093 reverse, plus A-MSDU at 4094 & 0..2 */
    FakePkt pkts[32];
    memset(pkts, 0, sizeof(pkts));
    bool need_timer = false;

    pkts[0].sn = 4092U;
    aic_rx_reord_submit(&r, 0U, 4092U, &pkts[0], 100U, false, fake_deliver, fake_append, fake_drop, &tc, &need_timer);
    assert(tc.delivered_cnt == 1U && tc.delivered_sns[0] == 4092U && !need_timer);

    /* Send 4095, 4094 (with 2 A-MSDU subframes!), 1, 0 out-of-order; duplicate 4095 must be dropped */
    pkts[1].sn = 4095U;
    aic_rx_reord_submit(&r, 0U, 4095U, &pkts[1], 100U, false, fake_deliver, fake_append, fake_drop, &tc, &need_timer);
    assert(tc.delivered_cnt == 1U && need_timer);

    pkts[2].sn = 4095U;
    aic_rx_reord_submit(&r, 0U, 4095U, &pkts[2], 100U, false, fake_deliver, fake_append, fake_drop, &tc, &need_timer);
    assert(tc.dropped_cnt == 1U && r.dup_drops == 1U);

    pkts[3].sn = 4094U; pkts[3].sub = 0U;
    pkts[4].sn = 4094U; pkts[4].sub = 1U;
    aic_rx_reord_submit(&r, 0U, 4094U, &pkts[3], 100U, false, fake_deliver, fake_append, fake_drop, &tc, &need_timer);
    aic_rx_reord_submit(&r, 0U, 4094U, &pkts[4], 100U, true,  fake_deliver, fake_append, fake_drop, &tc, &need_timer);

    pkts[5].sn = 1U;
    pkts[6].sn = 0U;
    aic_rx_reord_submit(&r, 0U, 1U, &pkts[5], 100U, false, fake_deliver, fake_append, fake_drop, &tc, &need_timer);
    aic_rx_reord_submit(&r, 0U, 0U, &pkts[6], 100U, false, fake_deliver, fake_append, fake_drop, &tc, &need_timer);
    assert(tc.delivered_cnt == 1U);

    /* Fill gap 4093 -> MUST flush 4093, 4094(sub0), 4094(sub1), 4095, 0, 1 in exact order! */
    pkts[7].sn = 4093U;
    aic_rx_reord_submit(&r, 0U, 4093U, &pkts[7], 100U, false, fake_deliver, fake_append, fake_drop, &tc, &need_timer);
    assert(tc.delivered_cnt == 7U && !need_timer);
    const uint32_t exp_sns[7]  = {4092, 4093, 4094, 4094, 4095, 0, 1};
    const uint32_t exp_subs[7] = {0,    0,    0,    1,    0,    0, 0};
    for (size_t i = 0U; i < 7U; ++i) {
        assert(tc.delivered_sns[i] == exp_sns[i] && tc.delivered_subs[i] == exp_subs[i]);
    }
    assert(r.tids[0].ind_sn == 2U && r.tids[0].pending_count == 0U);

    /* 2. Timeout flush test across a missing SN (gap at 2, buffer 3, 4, gap at 5, buffer 6) */
    tc.delivered_cnt = 0U;
    pkts[8].sn = 3U; pkts[9].sn = 4U; pkts[10].sn = 6U;
    aic_rx_reord_submit(&r, 0U, 3U, &pkts[8],  100U, false, fake_deliver, fake_append, fake_drop, &tc, &need_timer);
    aic_rx_reord_submit(&r, 0U, 4U, &pkts[9],  100U, false, fake_deliver, fake_append, fake_drop, &tc, &need_timer);
    aic_rx_reord_submit(&r, 0U, 6U, &pkts[10], 100U, false, fake_deliver, fake_append, fake_drop, &tc, &need_timer);
    assert(tc.delivered_cnt == 0U);
    assert(aic_rx_reord_timeout_flush(&r, fake_deliver, &tc) == true); /* delivers 3, 4; 6 still pending */
    assert(tc.delivered_cnt == 2U && tc.delivered_sns[0] == 3U && tc.delivered_sns[1] == 4U);
    assert(aic_rx_reord_timeout_flush(&r, fake_deliver, &tc) == false); /* delivers 6 */
    assert(tc.delivered_cnt == 3U && tc.delivered_sns[2] == 6U);

    /* 3. Window jump (> 64 distance) must advance window cleanly without slot aliasing */
    tc.delivered_cnt = 0U;
    pkts[11].sn = 10U; pkts[12].sn = 100U;
    aic_rx_reord_submit(&r, 0U, 10U,  &pkts[11], 100U, false, fake_deliver, fake_append, fake_drop, &tc, &need_timer);
    aic_rx_reord_submit(&r, 0U, 100U, &pkts[12], 100U, false, fake_deliver, fake_append, fake_drop, &tc, &need_timer);
    assert(tc.delivered_cnt == 1U && tc.delivered_sns[0] == 10U && r.window_jumps == 1U);
    aic_rx_reord_flush_all(&r, false, fake_deliver, fake_drop, &tc);
    assert(tc.delivered_cnt == 2U && tc.delivered_sns[1] == 100U && r.tids[0].pending_count == 0U);
}

int main(void) {
    test_rfc4493_aes_cmac();
    test_ieee80211_pbkdf2();
    test_scanu_parsing();
    test_wpa2_4way_and_gtk_rekey();
    test_async_dispatch_ex();
    test_rx_reorder_window();
    printf("[PASS] Phase 3 Wi-Fi suite passed (RFC 4493 CMAC, IEEE PBKDF2/4-way+GTK, SCANU, RX BA reorder)\n");
    return 0;
}

