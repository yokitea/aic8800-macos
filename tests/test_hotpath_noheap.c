#include <assert.h>
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aic8800_abi.h"
#include "aic8800_framing.h"
#include "aic8800_lmac.h"

static bool g_in_hot_path = false;
static uint64_t g_hot_path_heap_allocs = 0;

/* Intercept C heap allocations inside hot loops without disabling libSystem init. */
void *malloc(size_t sz) {
    if (g_in_hot_path) {
        g_hot_path_heap_allocs++;
    }
    typedef void *(*fn_t)(size_t);
    static fn_t real_fn = NULL;
    if (!real_fn) {
        real_fn = (fn_t)dlsym(RTLD_NEXT, "malloc");
    }
    return real_fn(sz);
}

void *calloc(size_t n, size_t sz) {
    if (g_in_hot_path) {
        g_hot_path_heap_allocs++;
    }
    typedef void *(*fn_t)(size_t, size_t);
    static fn_t real_fn = NULL;
    if (!real_fn) {
        real_fn = (fn_t)dlsym(RTLD_NEXT, "calloc");
    }
    return real_fn(n, sz);
}

void *realloc(void *ptr, size_t sz) {
    if (g_in_hot_path) {
        g_hot_path_heap_allocs++;
    }
    typedef void *(*fn_t)(void *, size_t);
    static fn_t real_fn = NULL;
    if (!real_fn) {
        real_fn = (fn_t)dlsym(RTLD_NEXT, "realloc");
    }
    return real_fn(ptr, sz);
}

static void noop_data_cb(const uint8_t *chunk, size_t chunk_len, void *ctx) {
    (void)chunk;
    (void)chunk_len;
    (void)ctx;
}

static void noop_eth_cb(const uint8_t *eth, size_t len, const aic_rx_frame_meta_t *meta, void *ctx) {
    (void)eth;
    (void)len;
    (void)meta;
    (void)ctx;
}

int main(void) {
    uint8_t eth[1514];
    uint8_t usb_tx[2048];
    uint8_t usb_rx[128];
    uint8_t scratch[2048];
    aic_fixed_slot_pool_t pool;
    assert(aic_slot_pool_init(&pool, 256U) == 0);

    memset(eth, 0x42, sizeof(eth));
    eth[12] = 0x08U;
    eth[13] = 0x00U;

    memset(usb_rx, 0, sizeof(usb_rx));
    aic_put_le16(&usb_rx[0], 40U);
    usb_rx[2] = AIC_USB_TYPE_DATA;
    usb_rx[AIC_RX_HWHDR_LEN + 0] = 0x08U;
    usb_rx[AIC_RX_HWHDR_LEN + 1] = 0x02U;
    usb_rx[AIC_RX_HWHDR_LEN + 24 + 6] = 0x08U;

    uint8_t msg_out[AIC_USB_MSG_MAX_PKT_SIZE];
    uint8_t msg_urb[64];
    aic_lmac_cmd_matcher_t matcher;
    aic_tx_flow_ctrl_t fc;
    aic_mm_set_txpwr_lvl_req_t pwr_req;
    aic_mm_set_rf_calib_req_t calib_req;
    aic_me_config_req_t me_req;
    aic_me_chan_config_req_t chan_req;
    aic_msg_rx_dispatch_t disp;

    aic_lmac_cmd_matcher_init(&matcher);
    aic_tx_flow_ctrl_init(&fc, 256U, AIC_USB_TX_LOW_WATER, AIC_USB_TX_HIGH_WATER);

    memset(msg_urb, 0, sizeof(msg_urb));
    /* [0..3]=USB msg hdr(18B, 0x11), [4..15]=e2a hdr(id=MAC_CFM, dest=DRV, len=6), [16..21]=MAC */
    aic_put_le16(&msg_urb[0], (uint16_t)(sizeof(aic_ipc_e2a_msg_hdr_t) + 6U));
    msg_urb[2] = AIC_USB_TYPE_CFG_CMD_RSP;
    aic_ipc_e2a_msg_hdr_t e2a = {
        .id = AIC_MM_GET_MAC_ADDR_CFM,
        .dummy_dest_id = AIC_DRV_TASK_ID,
        .dummy_src_id = AIC_TASK_MM,
        .param_len = 6U,
        .pattern = AIC_IPC_MSGE2A_VALID_PAT,
    };
    memcpy(&msg_urb[4], &e2a, sizeof(e2a));

    g_in_hot_path = true;
    for (int i = 0; i < 10000; i++) {
        uint16_t slot = 0;
        assert(aic_tx_flow_ctrl_can_accept(&fc, aic_slot_pool_in_flight(&pool)));
        assert(aic_slot_pool_acquire(&pool, &slot) == 0);
        (void)aic_tx_flow_ctrl_on_acquire(&fc, aic_slot_pool_in_flight(&pool));

        size_t out_len = 0;
        assert(aic_tx_encode_eth_frame(eth, 64U + (size_t)(i % 1400), 0, 0, 0, 0, 0, true,
                                       usb_tx, sizeof(usb_tx), &out_len) == 0);
        (void)aic_usb_needs_zlp((uint32_t)out_len, AIC_USB_BULK_MPS_HS);
        assert(aic_rx_demux_usb_aggr(usb_rx, AIC_RX_HWHDR_LEN + 40U, noop_data_cb, NULL, NULL) == 1);
        assert(aic_rx_chunk_to_eth_frames(usb_rx, AIC_RX_HWHDR_LEN + 40U, scratch, sizeof(scratch),
                                          noop_eth_cb, NULL) == 1);

        aic_fill_d80_txpwr_lvl_v3(&pwr_req);
        aic_build_d80_rf_calib_req(&calib_req);
        aic_build_d80_me_config_req(&me_req, (1U << AIC_MM_FEAT_VHT_BIT) | (1U << AIC_MM_FEAT_HE_BIT), 1U);
        aic_build_d80_me_chan_config_req(&chan_req, 1U);

        size_t cmd_len = 0U;
        assert(aic_encode_lmac_msg(msg_out, sizeof(msg_out), AIC_MM_VERSION_REQ, AIC_TASK_MM, AIC_DRV_TASK_ID,
                                   NULL, 0U, &cmd_len) == 0);
        aic_lmac_cmd_matcher_arm(&matcher, AIC_MM_GET_MAC_ADDR_CFM);
        assert(aic_usb_dispatch_msg_urb(&matcher, msg_urb, 4U + sizeof(e2a) + 6U, &disp) == 0);
        assert(disp.matched_cmd_cfm);

        assert(aic_slot_pool_release(&pool, slot) == 0);
        (void)aic_tx_flow_ctrl_on_release(&fc, aic_slot_pool_in_flight(&pool));
    }
    g_in_hot_path = false;

    assert(g_hot_path_heap_allocs == 0);
    printf("Zero-heap hot-path C trap verified: 10000 iterations, hot_path_heap_allocs=%llu.\n",
           (unsigned long long)g_hot_path_heap_allocs);
    return 0;
}

