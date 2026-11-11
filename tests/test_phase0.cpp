#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <malloc/malloc.h>

#include "aic8800_abi.h"
#include "aic8800_framing.h"

namespace {

thread_local bool gTrapNewAlloc = false;
std::atomic<uint64_t> gCppNewAllocs{0};

struct NoHeapGuard {
    NoHeapGuard() { gTrapNewAlloc = true; }
    ~NoHeapGuard() { gTrapNewAlloc = false; }
};

uint32_t next_prng(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

void build_80211_header(uint8_t *buf,
                        uint8_t ds_bits,
                        bool is_qos,
                        bool is_htc,
                        bool is_amsdu,
                        uint8_t tid,
                        uint16_t seq,
                        const uint8_t ra[6],
                        const uint8_t ta[6],
                        const uint8_t bssid[6],
                        size_t *out_hdr_len) {
    memset(buf, 0, 32);
    buf[0] = is_qos ? 0x88U : 0x08U;
    buf[1] = (uint8_t)(ds_bits & 0x03U);
    if (is_htc) {
        buf[1] |= 0x80U;
    }

    if (ds_bits == 0x01U) {
        memcpy(&buf[4], bssid, 6);
        memcpy(&buf[10], ta, 6);
        memcpy(&buf[16], ra, 6);
    } else if (ds_bits == 0x02U) {
        memcpy(&buf[4], ra, 6);
        memcpy(&buf[10], bssid, 6);
        memcpy(&buf[16], ta, 6);
    } else {
        memcpy(&buf[4], ra, 6);
        memcpy(&buf[10], ta, 6);
        memcpy(&buf[16], bssid, 6);
    }

    buf[22] = (uint8_t)((seq & 0x0FU) << 4U);
    buf[23] = (uint8_t)((seq >> 4U) & 0xFFU);

    size_t len = 24;
    if (is_qos) {
        buf[24] = (uint8_t)((tid & 0x0FU) | (is_amsdu ? 0x80U : 0x00U));
        buf[25] = 0x00U;
        len += 2;
    }
    if (is_htc) {
        len += 4;
    }
    *out_hdr_len = len;
}

void write_llc_snap(uint8_t *dst, uint16_t ethertype_be) {
    static const uint8_t kSnap[6] = {0xAAU, 0xAAU, 0x03U, 0x00U, 0x00U, 0x00U};
    memcpy(dst, kSnap, 6);
    dst[6] = (uint8_t)(ethertype_be >> 8U);
    dst[7] = (uint8_t)(ethertype_be & 0xFFU);
}

struct DemuxCapture {
    int data_count = 0;
    int cfg_count = 0;
    uint16_t last_data_lens[8] = {};
    uint8_t cfg_types[8] = {};
    size_t cfg_lens[8] = {};
};

void on_usb_data(const uint8_t *chunk, size_t chunk_len, void *ctx) {
    (void)chunk;
    auto *cap = static_cast<DemuxCapture *>(ctx);
    assert(chunk_len >= AIC_RX_HWHDR_LEN);
    if (cap->data_count < 8) {
        cap->last_data_lens[cap->data_count] = static_cast<uint16_t>(chunk_len);
    }
    cap->data_count++;
}

void on_usb_cfg(uint8_t cfg_type, const uint8_t *payload, size_t payload_len, void *ctx) {
    (void)payload;
    auto *cap = static_cast<DemuxCapture *>(ctx);
    if (cap->cfg_count < 8) {
        cap->cfg_types[cap->cfg_count] = cfg_type;
        cap->cfg_lens[cap->cfg_count] = payload_len;
    }
    cap->cfg_count++;
}

struct EthCapture {
    int count = 0;
    size_t lens[8] = {};
    aic_rx_frame_meta_t metas[8] = {};
    uint8_t first_bytes[8][64] = {};
};

void on_eth_frame(const uint8_t *eth, size_t len, const aic_rx_frame_meta_t *meta, void *ctx) {
    auto *cap = static_cast<EthCapture *>(ctx);
    if (cap->count < 8) {
        cap->lens[cap->count] = len;
        cap->metas[cap->count] = *meta;
        size_t cp = len < 64 ? len : 64;
        memcpy(cap->first_bytes[cap->count], eth, cp);
    }
    cap->count++;
}

}  // namespace

void *operator new(std::size_t sz) {
    if (gTrapNewAlloc) {
        gCppNewAllocs.fetch_add(1, std::memory_order_relaxed);
    }
    void *p = std::malloc(sz ? sz : 1);
    if (!p) {
        throw std::bad_alloc();
    }
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

int main() {
    uint64_t vectors = 0;
    uint32_t rng = 0xA1C8800DU ^ 0x5A17C321U;

    /* 1. Stage 0 BOT CBW Eject + Stage 1 5 GHz patch pairs + ZLP vectors */
    {
        NoHeapGuard guard;
        uint8_t cbw[AIC_USB_BOT_CBW_LEN];
        aic_build_bot_eject_cbw(cbw, 0x11223344U);
        assert(aic_get_le32(&cbw[0]) == AIC_USB_BOT_CBW_SIGNATURE);
        assert(aic_get_le32(&cbw[4]) == 0x11223344U);
        assert(cbw[14] == 6U && cbw[15] == 0x1BU && cbw[19] == 0x02U);
        vectors++;

        uint32_t pairs5g[3][2] = {};
        aic_stage1_build_d80_patch_pairs(0x001D7000U, true, pairs5g);
        assert(pairs5g[0][0] == 0x001D70B4U && pairs5g[0][1] == 0xF3010001U);
        assert(pairs5g[1][0] == 0x001D7170U && pairs5g[1][1] == AIC_D80_PATCH_RX_AGGR_VAL);
        assert(pairs5g[2][0] == 0x001D7188U && pairs5g[2][1] == AIC_D80_PATCH_EXT_FLAGS_VAL);

        uint32_t pairs24g[3][2] = {};
        aic_stage1_build_d80_patch_pairs(0U, false, pairs24g);
        assert(pairs24g[0][0] == 0x00B4U && pairs24g[0][1] == 0xF3010000U);
        vectors += 2;

        for (uint32_t len = 0; len <= 2048U; len++) {
            bool expect512 = (len > 0U) && ((len % 512U) == 0U);
            assert(aic_usb_needs_zlp(len, 512U) == expect512);
            bool expect1024 = (len > 0U) && ((len % 1024U) == 0U);
            assert(aic_usb_needs_zlp(len, 1024U) == expect1024);
            vectors++;
        }
    }

    /* 2. Fixed 256-slot pool stress (no duplicates, exact capacity bounds) */
    {
        NoHeapGuard guard;
        aic_fixed_slot_pool_t pool;
        assert(aic_slot_pool_init(&pool, 0) == -1);
        assert(aic_slot_pool_init(&pool, 257) == -1);
        assert(aic_slot_pool_init(&pool, 256) == 0);

        bool held[256] = {};
        for (int cycle = 0; cycle < 10; cycle++) {
            for (int i = 0; i < 256; i++) {
                uint16_t idx = 0xFFFFU;
                assert(aic_slot_pool_acquire(&pool, &idx) == 0);
                assert(idx < 256U && !held[idx]);
                held[idx] = true;
                vectors++;
            }
            uint16_t extra = 0;
            assert(aic_slot_pool_acquire(&pool, &extra) == -1);
            for (uint16_t i = 0; i < 256; i++) {
                assert(aic_slot_pool_release(&pool, i) == 0);
                held[i] = false;
                vectors++;
            }
            assert(aic_slot_pool_release(&pool, 256) == -1);
        }
    }

    /* 3. LMAC control/debug message TX encoding (0x11 + 4B dummy word) */
    {
        NoHeapGuard guard;
        uint8_t out_msg[AIC_USB_MSG_MAX_PKT_SIZE];
        uint8_t param[64];
        for (int i = 0; i < 500; i++) {
            uint16_t plen = static_cast<uint16_t>(next_prng(&rng) % 65U);
            for (uint16_t j = 0; j < plen; j++) {
                param[j] = static_cast<uint8_t>(next_prng(&rng));
            }
            size_t out_len = 0;
            uint16_t msg_id = static_cast<uint16_t>(AIC_DBG_MEM_WRITE_REQ + (i % 5));
            assert(aic_encode_lmac_msg(out_msg, sizeof(out_msg), msg_id, AIC_TASK_DBG,
                                       AIC_DRV_TASK_ID, plen ? param : nullptr, plen, &out_len) == 0);
            assert(out_len == 16U + plen);
            uint16_t hdr_len = aic_get_le16(&out_msg[0]) & 0x0FFFU;
            assert(hdr_len == 8U + plen + 4U);
            assert(out_msg[2] == AIC_USB_TYPE_CFG_CMD_RSP);
            assert(aic_get_le32(&out_msg[4]) == 0U);
            assert(aic_get_le16(&out_msg[8]) == msg_id);
            assert(aic_get_le16(&out_msg[14]) == plen);
            if (plen > 0U) {
                assert(memcmp(&out_msg[16], param, plen) == 0);
            }
            vectors++;
        }
        size_t bad_len = 0;
        assert(aic_encode_lmac_msg(out_msg, 12, AIC_DBG_MEM_READ_REQ, AIC_TASK_DBG,
                                   AIC_DRV_TASK_ID, param, 8, &bad_len) == -1);
        vectors++;
    }

    /* 4. TX Ethernet II -> AIC8800 USB TX frame (4000 randomized + edge vectors) */
    {
        NoHeapGuard guard;
        uint8_t eth[AIC_DATA_BUF_MAX];
        uint8_t usb_tx[AIC_DATA_BUF_MAX + 64U];
        for (int i = 0; i < 4000; i++) {
            size_t eth_len = 14U + (next_prng(&rng) % (1514U - 14U + 1U));
            if (i == 0) eth_len = 14U;
            if (i == 1) eth_len = 1514U;
            if (i == 2) eth_len = AIC_DATA_BUF_MAX;
            for (size_t j = 0; j < eth_len; j++) {
                eth[j] = static_cast<uint8_t>(next_prng(&rng));
            }
            bool pad4 = (i % 2) == 0;
            uint8_t sta = static_cast<uint8_t>(i & 0x0FU);
            uint8_t vif = static_cast<uint8_t>((i >> 4) & 0x03U);
            uint8_t tid = static_cast<uint8_t>(i & 0x07U);
            uint8_t ac = static_cast<uint8_t>(i & 0x03U);
            size_t out_len = 0;
            assert(aic_tx_encode_eth_frame(eth, eth_len, sta, vif, tid, ac, 0x0042U,
                                           pad4, usb_tx, sizeof(usb_tx), &out_len) == 0);
            size_t unpadded = 4U + sizeof(aic_txdesc_api_t) + (eth_len - 14U);
            size_t expected = pad4 ? ((unpadded + 3U) & ~size_t(3U)) : unpadded;
            assert(out_len == expected);
            assert((aic_get_le16(&usb_tx[0]) & 0x0FFFU) == expected);
            assert(usb_tx[2] == AIC_USB_TYPE_TX_DATA_OUT);
            aic_txdesc_api_t desc;
            memcpy(&desc, &usb_tx[4], sizeof(desc));
            assert(desc.host.packet_len == (eth_len - 14U));
            assert(memcmp(desc.host.eth_dest_addr.array, &eth[0], 6) == 0);
            assert(memcmp(desc.host.eth_src_addr.array, &eth[6], 6) == 0);
            assert(memcmp(&desc.host.ethertype, &eth[12], 2) == 0);
            assert(desc.host.ac == ac && desc.host.tid == tid);
            assert(desc.host.vif_idx == vif && desc.host.staid == sta);
            assert(desc.host.flags == 0x0042U);
            if (eth_len > 14U) {
                assert(memcmp(&usb_tx[4 + sizeof(desc)], &eth[14], eth_len - 14U) == 0);
            }
            vectors++;
        }
        size_t bad = 0;
        assert(aic_tx_encode_eth_frame(eth, 13U, 0, 0, 0, 0, 0, true, usb_tx, sizeof(usb_tx), &bad) == -1);
        vectors++;
    }

    /* 5. RX USB Aggregation Demux (mixed exact DATA stride vs 4B-padded CFG stride) */
    {
        NoHeapGuard guard;
        uint8_t usb_rx[8192];
        for (int i = 0; i < 2000; i++) {
            size_t pos = 0;
            uint16_t d1 = static_cast<uint16_t>(33U + (next_prng(&rng) % 250U)); /* unaligned size! */
            aic_put_le16(&usb_rx[pos], d1);
            usb_rx[pos + 2] = AIC_USB_TYPE_DATA;
            usb_rx[pos + 3] = 0x00U;
            memset(&usb_rx[pos + 4], 0x11, (AIC_RX_HWHDR_LEN - 4U) + d1);
            pos += AIC_RX_HWHDR_LEN + d1; /* DATA has NO 4B tail pad */

            uint16_t c1 = static_cast<uint16_t>(13U + (next_prng(&rng) % 45U));
            aic_put_le16(&usb_rx[pos], c1);
            usb_rx[pos + 2] = AIC_USB_TYPE_CFG_CMD_RSP;
            usb_rx[pos + 3] = 0x00U;
            memset(&usb_rx[pos + 4], 0x22, c1);
            size_t c1_pad = (c1 + 3U) & ~size_t(3U);
            if (c1_pad > c1) {
                memset(&usb_rx[pos + 4U + c1], 0x00, c1_pad - c1);
            }
            pos += 4U + c1_pad;

            uint16_t d2 = static_cast<uint16_t>(41U + (next_prng(&rng) % 300U));
            aic_put_le16(&usb_rx[pos], d2);
            usb_rx[pos + 2] = AIC_USB_TYPE_DATA;
            usb_rx[pos + 3] = 0x00U;
            memset(&usb_rx[pos + 4], 0x33, (AIC_RX_HWHDR_LEN - 4U) + d2);
            pos += AIC_RX_HWHDR_LEN + d2;

            DemuxCapture cap;
            int cnt = aic_rx_demux_usb_aggr(usb_rx, pos, on_usb_data, on_usb_cfg, &cap);
            assert(cnt == 3);
            assert(cap.data_count == 2 && cap.cfg_count == 1);
            assert(cap.last_data_lens[0] == AIC_RX_HWHDR_LEN + d1);
            assert(cap.last_data_lens[1] == AIC_RX_HWHDR_LEN + d2);
            assert(cap.cfg_types[0] == AIC_USB_TYPE_CFG_CMD_RSP && cap.cfg_lens[0] == c1);
            vectors += 3;
        }
        /* Truncated RX header returns -1 cleanly */
        uint8_t short_buf[3] = {1, 0, 0};
        assert(aic_rx_demux_usb_aggr(short_buf, sizeof(short_buf), on_usb_data, on_usb_cfg, nullptr) == -1);
        vectors++;
    }

    /* 6. RX 802.11 -> 802.3 (DS modes, QoS, HTC, Crypto IVs, EAPOL, A-MSDU, R7000 guard) */
    {
        NoHeapGuard guard;
        uint8_t chunk[2500];
        uint8_t scratch[AIC_DATA_BUF_MAX];
        const uint8_t ra[6] = {0xAA, 0xBB, 0xCC, 0x01, 0x02, 0x03};
        const uint8_t ta[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
        const uint8_t bssid[6] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01};
        static const uint8_t kDecrModes[8] = {
            AIC_RX_HD_DECR_UNENC, AIC_RX_HD_DECR_WEP, AIC_RX_HD_DECR_TKIP, AIC_RX_HD_DECR_CCMP128,
            AIC_RX_HD_DECR_CCMP256, AIC_RX_HD_DECR_GCMP128, AIC_RX_HD_DECR_GCMP256, AIC_RX_HD_DECR_WAPI
        };
        static const size_t kIvLens[8] = {0, 4, 8, 8, 8, 8, 8, 18};

        for (int i = 0; i < 2000; i++) {
            uint8_t ds = static_cast<uint8_t>(i & 0x03U);
            bool qos = ((i >> 2) & 1) != 0;
            bool htc = ((i >> 3) & 1) != 0;
            int dm = (i >> 4) & 7;
            bool eapol = (i % 13) == 0;
            uint16_t etype = eapol ? 0x888EU : 0x0800U;
            size_t payload_len = 18U + (next_prng(&rng) % 200U);

            memset(chunk, 0, sizeof(chunk));
            aic_hw_rxhdr_t rxhdr;
            memset(&rxhdr, 0, sizeof(rxhdr));
            rxhdr.hwvect.decr_status = kDecrModes[dm];
            rxhdr.flags_vif_idx = static_cast<uint32_t>(i & 3);
            rxhdr.flags_sta_idx = static_cast<uint32_t>(i & 7);
            memcpy(chunk, &rxhdr, sizeof(rxhdr));

            size_t hdr_len = 0;
            uint8_t *mac = &chunk[AIC_RX_HWHDR_LEN];
            build_80211_header(mac, ds, qos, htc, false, static_cast<uint8_t>(i & 7),
                               static_cast<uint16_t>(i & 0x0FFFU), ra, ta, bssid, &hdr_len);
            size_t iv_len = kIvLens[dm];
            if (iv_len > 0) {
                memset(&mac[hdr_len], 0x5A, iv_len);
            }
            write_llc_snap(&mac[hdr_len + iv_len], etype);
            for (size_t p = 0; p < payload_len; p++) {
                mac[hdr_len + iv_len + 8U + p] = static_cast<uint8_t>(p ^ i);
            }

            EthCapture cap;
            size_t total_chunk = AIC_RX_HWHDR_LEN + hdr_len + iv_len + 8U + payload_len;
            int rc = aic_rx_chunk_to_eth_frames(chunk, total_chunk, scratch, sizeof(scratch),
                                                on_eth_frame, &cap);
            assert(rc == 1 && cap.count == 1);
            assert(cap.lens[0] == 14U + payload_len);
            assert(memcmp(&cap.first_bytes[0][0], ra, 6) == 0);
            assert(memcmp(&cap.first_bytes[0][6], ta, 6) == 0);
            assert(cap.metas[0].ethertype_be == etype);
            assert(cap.metas[0].is_eapol == eapol);
            assert(!cap.metas[0].is_amsdu);
            vectors++;
        }

        /* A-MSDU 3-subframe test + Netgear R7000 false A-MSDU guard */
        for (int i = 0; i < 500; i++) {
            memset(chunk, 0, sizeof(chunk));
            aic_hw_rxhdr_t rxhdr;
            memset(&rxhdr, 0, sizeof(rxhdr));
            rxhdr.hwvect.decr_status = AIC_RX_HD_DECR_CCMP128;
            memcpy(chunk, &rxhdr, sizeof(rxhdr));

            size_t hdr_len = 0;
            uint8_t *mac = &chunk[AIC_RX_HWHDR_LEN];
            build_80211_header(mac, 0x02U, true, false, true, 5, 123, ra, ta, bssid, &hdr_len);
            size_t pos = hdr_len + 8U; /* CCMP IV */

            const uint16_t pay_lens[3] = {20U, 25U, 19U};
            for (int s = 0; s < 3; s++) {
                uint8_t *sub = &mac[pos];
                memcpy(&sub[0], ra, 6);
                sub[5] = static_cast<uint8_t>(s);
                memcpy(&sub[6], ta, 6);
                uint16_t msdu_len = static_cast<uint16_t>(8U + pay_lens[s]);
                sub[12] = static_cast<uint8_t>(msdu_len >> 8U);
                sub[13] = static_cast<uint8_t>(msdu_len & 0xFFU);
                write_llc_snap(&sub[14], (s == 2) ? 0x86DDU : 0x0800U);
                memset(&sub[22], static_cast<uint8_t>(0xA0 + s), pay_lens[s]);
                size_t raw_sub = 14U + msdu_len;
                pos += (s < 2) ? ((raw_sub + 3U) & ~size_t(3U)) : raw_sub;
            }

            EthCapture cap;
            int rc = aic_rx_chunk_to_eth_frames(chunk, AIC_RX_HWHDR_LEN + pos,
                                                scratch, sizeof(scratch), on_eth_frame, &cap);
            assert(rc == 3 && cap.count == 3);
            assert(cap.lens[0] == 14U + 20U && cap.lens[1] == 14U + 25U && cap.lens[2] == 14U + 19U);
            assert(cap.metas[0].is_amsdu && cap.metas[2].ethertype_be == 0x86DDU);
            vectors += 3;

            /* Netgear R7000 guard: QoS amsdu bit=1, but LLC AA AA 03 .. ethertype 0x0800 > 0x0600 */
            memset(chunk, 0, sizeof(chunk));
            memcpy(chunk, &rxhdr, sizeof(rxhdr));
            build_80211_header(mac, 0x02U, true, false, true, 0, 7, ra, ta, bssid, &hdr_len);
            write_llc_snap(&mac[hdr_len + 8U], 0x0800U);
            memset(&mac[hdr_len + 8U + 8U], 0x77, 40U);
            EthCapture r7000;
            int rrc = aic_rx_chunk_to_eth_frames(chunk, AIC_RX_HWHDR_LEN + hdr_len + 8U + 8U + 40U,
                                                 scratch, sizeof(scratch), on_eth_frame, &r7000);
            assert(rrc == 1 && r7000.count == 1 && !r7000.metas[0].is_amsdu);
            assert(r7000.lens[0] == 14U + 40U && r7000.metas[0].ethertype_be == 0x0800U);
            vectors++;
        }

        /* Management MPDU skipped cleanly (0 frames) */
        memset(chunk, 0, sizeof(chunk));
        aic_hw_rxhdr_t mpdu_hdr;
        memset(&mpdu_hdr, 0, sizeof(mpdu_hdr));
        mpdu_hdr.flags_is_80211_mpdu = 1U;
        memcpy(chunk, &mpdu_hdr, sizeof(mpdu_hdr));
        assert(aic_rx_chunk_to_eth_frames(chunk, AIC_RX_HWHDR_LEN + 24U, scratch, sizeof(scratch),
                                          on_eth_frame, nullptr) == 0);
        vectors++;
    }

    assert(gCppNewAllocs.load() == 0);
    assert(vectors >= 10000ULL);
    printf("Phase 0 ASan/UBSan suite passed: %llu vectors tested, cpp_new_allocs=%llu.\n",
           static_cast<unsigned long long>(vectors),
           static_cast<unsigned long long>(gCppNewAllocs.load()));
    return 0;
}
