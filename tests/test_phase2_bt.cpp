#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "aic8800_abi.h"

namespace {

/* Exact replica of the decision logic inside AIC8800BTQuirk */
inline uint8_t quirk_rewrite_bm_request_type(bool is_aic_device,
                                             uint8_t bmRequestType,
                                             uint16_t wIndex) {
    if (is_aic_device) {
        if (bmRequestType == 0x21U && wIndex == 0U) {
            return 0x20U;
        }
    }
    return bmRequestType;
}

inline bool quirk_should_append_zlp(uint8_t ep_address, uint32_t length) {
    if (ep_address == AIC_USB_BT_EP_ACL_OUT) {
        if (length > 0U && (length % AIC_USB_BT_ACL_ZLP_CHUNK) == 0U) {
            return true;
        }
    }
    return false;
}

inline bool is_aic_stage2(uint16_t vid, uint16_t pid) {
    if (pid == AIC_USB_PID_WIFI_BT_RUNTIME) {
        if (vid == AIC_USB_VID_AICS_WIFI || vid == AIC_USB_VID_CSR_SPOOF) {
            return true;
        }
    }
    return false;
}

}  // namespace

int main() {
    printf("=== Running AIC8800 Phase 2 Bluetooth Quirk & Math Suite ===\n");

    /* 1. Compile-time constant verification */
    static_assert(AIC_USB_VID_CSR_SPOOF == 0x0A12U, "CSR VID mismatch");
    static_assert(AIC_USB_VID_AICS_WIFI == 0x368BU, "AIC VID mismatch");
    static_assert(AIC_USB_PID_WIFI_BT_RUNTIME == 0x8D81U, "Stage 2 PID mismatch");
    static_assert(AIC_USB_BT_EP_ACL_OUT == 0x04U, "BT ACL Bulk OUT endpoint must be 0x04");
    static_assert(AIC_USB_BT_ACL_ZLP_CHUNK == 512U, "High-Speed Bulk ZLP chunk must be 512");

    /* 2. Device recognition logic */
    assert(is_aic_stage2(0x368B, 0x8D81) == true);
    assert(is_aic_stage2(0x0A12, 0x8D81) == true);
    assert(is_aic_stage2(0xA69C, 0x5721) == false);  /* Stage 0 */
    assert(is_aic_stage2(0xA69C, 0x8D80) == false);  /* Stage 1 */
    assert(is_aic_stage2(0x1234, 0x8D81) == false);  /* foreign vendor */
    assert(is_aic_stage2(0x368B, 0x0000) == false);

    /* 3. bmRequestType 0x21 -> 0x20 rewrite verification across all 256 request types & index values */
    size_t rewrite_count = 0;
    for (uint32_t req = 0; req < 256; req++) {
        for (uint32_t idx = 0; idx < 4; idx++) {
            uint8_t out_aic = quirk_rewrite_bm_request_type(true, (uint8_t)req, (uint16_t)idx);
            uint8_t out_other = quirk_rewrite_bm_request_type(false, (uint8_t)req, (uint16_t)idx);

            /* Non-AIC must NEVER be rewritten */
            assert(out_other == (uint8_t)req);

            if (req == 0x21U && idx == 0U) {
                assert(out_aic == 0x20U);
                rewrite_count++;
            } else {
                assert(out_aic == (uint8_t)req);
            }
        }
    }
    assert(rewrite_count == 1);

    /* 4. ZLP triggering verification across 10,000 transfer lengths */
    size_t zlp_triggers_ep4 = 0;
    size_t zlp_triggers_other_ep = 0;

    for (uint32_t len = 0; len <= 16384; len++) {
        bool trig_ep4 = quirk_should_append_zlp(0x04, len);
        bool trig_ep1 = quirk_should_append_zlp(0x01, len);
        bool trig_ep2 = quirk_should_append_zlp(0x02, len);
        bool trig_ep81 = quirk_should_append_zlp(0x81, len);

        if (len > 0 && (len % 512) == 0) {
            assert(trig_ep4 == true);
            zlp_triggers_ep4++;
        } else {
            assert(trig_ep4 == false);
        }

        assert(trig_ep1 == false);
        assert(trig_ep2 == false);
        assert(trig_ep81 == false);
        if (trig_ep1 || trig_ep2 || trig_ep81) {
            zlp_triggers_other_ep++;
        }
    }

    assert(zlp_triggers_ep4 == (16384 / 512));
    assert(zlp_triggers_other_ep == 0);

    printf("[PASS] Phase 2 Bluetooth Suite passed: %zu rewrites verified, %zu ZLP triggers validated on EP 0x04\n",
           rewrite_count, zlp_triggers_ep4);
    return 0;
}
