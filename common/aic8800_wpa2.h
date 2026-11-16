/*
 * aic8800_wpa2.h - Zero-heap IEEE 802.11i WPA2-PSK 4-Way & Group Handshake
 *
 * Uses macOS built-in CommonCrypto (PBKDF2-HMAC-SHA1, HMAC-SHA1/SHA256,
 * AES-ECB for RFC 4493 AES-128-CMAC, RFC 3394 AES Key Unwrap, CCRandomGenerateBytes).
 * Self-contained header so offline ASan/UBSan tests verify exact RFC/IEEE vectors.
 */

#ifndef AIC8800_WPA2_H
#define AIC8800_WPA2_H

#include <CommonCrypto/CommonCryptor.h>
#include <CommonCrypto/CommonDigest.h>
#include <CommonCrypto/CommonHMAC.h>
#include <CommonCrypto/CommonKeyDerivation.h>
#include <CommonCrypto/CommonRandom.h>
#include <CommonCrypto/CommonSymmetricKeywrap.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "aic8800_framing.h"
#include "aic8800_wifi_ctl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AIC_EAPOL_ETHERTYPE        0x888EU
#define AIC_EAPOL_HDR_LEN          4U
#define AIC_EAPOL_KEY_BODY_MIN_LEN 95U

#define AIC_KEY_INFO_VER_MASK      0x0007U
#define AIC_KEY_INFO_VER_HMAC_MD5  1U
#define AIC_KEY_INFO_VER_HMAC_SHA1 2U
#define AIC_KEY_INFO_VER_AES_CMAC  3U
#define AIC_KEY_INFO_PAIRWISE      (1U << 3U)
#define AIC_KEY_INFO_INSTALL       (1U << 6U)
#define AIC_KEY_INFO_ACK           (1U << 7U)
#define AIC_KEY_INFO_MIC           (1U << 8U)
#define AIC_KEY_INFO_SECURE        (1U << 9U)
#define AIC_KEY_INFO_ENC_KEY_DATA  (1U << 12U)

static inline uint16_t aic_wpa_get_be16(const uint8_t *b) {
    return (uint16_t)((uint16_t)b[0] << 8U) | (uint16_t)b[1];
}

static inline void aic_wpa_put_be16(uint8_t *b, uint16_t v) {
    b[0] = (uint8_t)((v >> 8U) & 0xFFU);
    b[1] = (uint8_t)(v & 0xFFU);
}

static inline uint64_t aic_wpa_get_be64(const uint8_t *b) {
    uint64_t v = 0U;
    for (size_t i = 0U; i < 8U; ++i) {
        v = (v << 8U) | (uint64_t)b[i];
    }
    return v;
}

static inline void aic_wpa_put_be64(uint8_t *b, uint64_t v) {
    for (size_t i = 0U; i < 8U; ++i) {
        b[7U - i] = (uint8_t)(v & 0xFFU);
        v >>= 8U;
    }
}

/*
 * RFC 4493 AES-128-CMAC over arbitrary message bytes using AES-128 ECB.
 */
static inline void aic_aes128_cmac_left_shift(const uint8_t in[16], uint8_t out[16]) {
    uint8_t overflow = 0U;
    for (int i = 15; i >= 0; --i) {
        out[i] = (uint8_t)((in[i] << 1U) | overflow);
        overflow = (in[i] & 0x80U) ? 1U : 0U;
    }
}

static inline void aic_aes128_cmac(const uint8_t key[16],
                                   const uint8_t *msg,
                                   size_t msg_len,
                                   uint8_t mac_out[16]) {
    uint8_t zero[16] = {0};
    uint8_t L[16]    = {0};
    size_t out_len   = 0U;
    CCCrypt(kCCEncrypt, kCCAlgorithmAES128, kCCOptionECBMode,
            key, 16, NULL, zero, 16, L, 16, &out_len);

    uint8_t K1[16];
    aic_aes128_cmac_left_shift(L, K1);
    if ((L[0] & 0x80U) != 0U) {
        K1[15] ^= 0x87U;
    }
    uint8_t K2[16];
    aic_aes128_cmac_left_shift(K1, K2);
    if ((K1[0] & 0x80U) != 0U) {
        K2[15] ^= 0x87U;
    }

    size_t n = (msg_len + 15U) / 16U;
    bool complete = true;
    if (n == 0U) {
        n = 1U;
        complete = false;
    } else if ((msg_len % 16U) != 0U) {
        complete = false;
    }

    uint8_t X[16] = {0};
    uint8_t Y[16] = {0};
    for (size_t i = 0U; i < n - 1U; ++i) {
        for (size_t j = 0U; j < 16U; ++j) {
            Y[j] = X[j] ^ msg[i * 16U + j];
        }
        CCCrypt(kCCEncrypt, kCCAlgorithmAES128, kCCOptionECBMode,
                key, 16, NULL, Y, 16, X, 16, &out_len);
    }

    uint8_t M_last[16] = {0};
    const size_t last_off = (n - 1U) * 16U;
    const size_t rem = (msg_len >= last_off) ? (msg_len - last_off) : 0U;
    if (complete) {
        for (size_t j = 0U; j < 16U; ++j) {
            M_last[j] = msg[last_off + j] ^ K1[j];
        }
    } else {
        uint8_t padded[16] = {0};
        if (rem > 0U) {
            memcpy(padded, msg + last_off, rem);
        }
        padded[rem] = 0x80U;
        for (size_t j = 0U; j < 16U; ++j) {
            M_last[j] = padded[j] ^ K2[j];
        }
    }

    for (size_t j = 0U; j < 16U; ++j) {
        Y[j] = X[j] ^ M_last[j];
    }
    CCCrypt(kCCEncrypt, kCCAlgorithmAES128, kCCOptionECBMode,
            key, 16, NULL, Y, 16, mac_out, 16, &out_len);
}

/*
 * IEEE 802.11i PBKDF2-HMAC-SHA1 (4096 rounds) -> 32-byte PMK.
 */
static inline int aic_wpa2_derive_pmk(const char *passphrase,
                                      size_t pass_len,
                                      const uint8_t *ssid,
                                      size_t ssid_len,
                                      uint8_t pmk_out[32]) {
    if (!passphrase || !ssid || pass_len < 8U || pass_len > 63U ||
        ssid_len == 0U || ssid_len > 32U || !pmk_out) {
        return -1;
    }
    const int rc = CCKeyDerivationPBKDF(kCCPBKDF2,
                                        passphrase,
                                        pass_len,
                                        ssid,
                                        ssid_len,
                                        kCCPRFHmacAlgSHA1,
                                        4096U,
                                        pmk_out,
                                        32U);
    return (rc == kCCSuccess) ? 0 : -1;
}

/*
 * IEEE 802.11 PRF-512 (SHA1) / KDF-384 (SHA256) for PTK derivation (64 bytes).
 */
static inline void aic_wpa2_derive_ptk(const uint8_t pmk[32],
                                       const uint8_t aa[6],
                                       const uint8_t spa[6],
                                       const uint8_t anonce[32],
                                       const uint8_t snonce[32],
                                       bool use_sha256_kdf,
                                       uint8_t ptk_out[64]) {
    static const char kLabel[] = "Pairwise key expansion"; /* 22 bytes */
    uint8_t data[76];

    if (memcmp(aa, spa, 6U) < 0) {
        memcpy(&data[0], aa, 6U);
        memcpy(&data[6], spa, 6U);
    } else {
        memcpy(&data[0], spa, 6U);
        memcpy(&data[6], aa, 6U);
    }

    if (memcmp(anonce, snonce, 32U) < 0) {
        memcpy(&data[12], anonce, 32U);
        memcpy(&data[44], snonce, 32U);
    } else {
        memcpy(&data[12], snonce, 32U);
        memcpy(&data[44], anonce, 32U);
    }

    if (!use_sha256_kdf) {
        /* IEEE 802.11 PRF-512 using HMAC-SHA1 */
        uint8_t input[sizeof(kLabel) + sizeof(data) + 1U];
        memcpy(input, kLabel, sizeof(kLabel)); /* includes trailing '\0' */
        memcpy(input + sizeof(kLabel), data, sizeof(data));

        uint8_t digest[CC_SHA1_DIGEST_LENGTH];
        size_t pos = 0U;
        for (uint8_t ctr = 0U; pos < 64U; ++ctr) {
            input[sizeof(kLabel) + sizeof(data)] = ctr;
            CCHmac(kCCHmacAlgSHA1, pmk, 32U, input, sizeof(input), digest);
            const size_t take = (64U - pos < CC_SHA1_DIGEST_LENGTH) ? (64U - pos) : CC_SHA1_DIGEST_LENGTH;
            memcpy(ptk_out + pos, digest, take);
            pos += take;
        }
    } else {
        /* IEEE 802.11-2016 §12.7.1.7.2 KDF-Hash-Length with SHA256 (Length = 384 bits = 48 bytes) */
        uint8_t input[2U + 22U + sizeof(data) + 2U];
        uint8_t digest[CC_SHA256_DIGEST_LENGTH];
        memset(ptk_out, 0, 64U);
        for (uint16_t ctr = 1U; ctr <= 2U; ++ctr) {
            aic_put_le16(&input[0], ctr);
            memcpy(&input[2], kLabel, 22U); /* no NUL terminator in KDF-SHA256 */
            memcpy(&input[24], data, sizeof(data));
            aic_put_le16(&input[24 + sizeof(data)], 384U);
            CCHmac(kCCHmacAlgSHA256, pmk, 32U, input, sizeof(input), digest);
            const size_t off = (size_t)(ctr - 1U) * 32U;
            const size_t take = (48U - off < 32U) ? (48U - off) : 32U;
            memcpy(ptk_out + off, digest, take);
        }
    }
}

/*
 * Compute 16-byte EAPOL-Key MIC (over whole EAPOL PDU with MIC field [81..96] zeroed).
 */
static inline int aic_wpa2_compute_eapol_mic(uint16_t key_ver,
                                             const uint8_t kck[16],
                                             const uint8_t *eapol_pdu,
                                             size_t eapol_len,
                                             uint8_t mic_out[16]) {
    if (!kck || !eapol_pdu || !mic_out ||
        eapol_len < AIC_EAPOL_HDR_LEN + AIC_EAPOL_KEY_BODY_MIN_LEN ||
        eapol_len > AIC_WIFI_EAPOL_MAX_FRAME) {
        return -1;
    }
    uint8_t copy[AIC_WIFI_EAPOL_MAX_FRAME];
    memcpy(copy, eapol_pdu, eapol_len);
    memset(&copy[AIC_EAPOL_HDR_LEN + 77U], 0, 16U);

    if (key_ver == AIC_KEY_INFO_VER_HMAC_SHA1) {
        uint8_t digest[CC_SHA1_DIGEST_LENGTH];
        CCHmac(kCCHmacAlgSHA1, kck, 16U, copy, eapol_len, digest);
        memcpy(mic_out, digest, 16U);
        return 0;
    }
    if (key_ver == AIC_KEY_INFO_VER_AES_CMAC) {
        aic_aes128_cmac(kck, copy, eapol_len, mic_out);
        return 0;
    }
    if (key_ver == AIC_KEY_INFO_VER_HMAC_MD5) {
        uint8_t digest[CC_MD5_DIGEST_LENGTH];
        CCHmac(kCCHmacAlgMD5, kck, 16U, copy, eapol_len, digest);
        memcpy(mic_out, digest, 16U);
        return 0;
    }
    return -1;
}

/*
 * Unwrap encrypted Key Data from EAPOL M3 / Group M1 using KEK (RFC 3394 AES Key Unwrap)
 * and extract the GTK KDE (OUI 00-0F-AC type 01).
 */
static inline int aic_wpa2_extract_gtk(const uint8_t kek[16],
                                       const uint8_t *enc_data,
                                       size_t enc_len,
                                       uint8_t *out_gtk_idx,
                                       uint8_t out_gtk[AIC_MAC_SEC_KEY_LEN],
                                       uint8_t *out_gtk_len) {
    if (!kek || !enc_data || !out_gtk_idx || !out_gtk || !out_gtk_len ||
        enc_len < 24U || (enc_len % 8U) != 0U || enc_len > 512U) {
        return -1;
    }

    uint8_t plain[512];
    size_t plain_len = sizeof(plain);
    const int rc = CCSymmetricKeyUnwrap(kCCWRAPAES,
                                        CCrfc3394_iv,
                                        CCrfc3394_ivLen,
                                        kek,
                                        16U,
                                        enc_data,
                                        enc_len,
                                        plain,
                                        &plain_len);
    if (rc != kCCSuccess) {
        return -1;
    }

    size_t pos = 0U;
    while (pos + 2U <= plain_len) {
        const uint8_t tag = plain[pos];
        const uint8_t len = plain[pos + 1U];
        pos += 2U;
        if (tag == 0xDDU && len == 0U) {
            break; /* IEEE 802.11i trailing zero padding */
        }
        if ((size_t)len > plain_len - pos) {
            break;
        }
        const uint8_t *val = &plain[pos];
        if (tag == 0xDDU && len >= 6U &&
            val[0] == 0x00U && val[1] == 0x0FU && val[2] == 0xACU && val[3] == 0x01U) {
            const uint8_t klen = (uint8_t)(len - 6U);
            if (klen > 0U && klen <= AIC_MAC_SEC_KEY_LEN) {
                *out_gtk_idx = (uint8_t)(val[4] & 0x03U);
                *out_gtk_len = klen;
                memcpy(out_gtk, &val[6], klen);
                return 0;
            }
        }
        pos += (size_t)len;
    }
    return -1;
}

typedef struct {
    uint8_t  aa[AIC_MAC_ADDR_LEN];
    uint8_t  spa[AIC_MAC_ADDR_LEN];
    uint8_t  pmk[32];
    uint8_t  anonce[32];
    uint8_t  snonce[32];
    uint8_t  ptk[64]; /* KCK[0..15] KEK[16..31] TK[32..47] */
    uint8_t  gtk[AIC_MAC_SEC_KEY_LEN];
    uint8_t  gtk_len;
    uint8_t  gtk_idx;
    uint8_t  rsn_ie[24];
    uint8_t  rsn_ie_len;
    uint8_t  pairwise_cipher;
    uint8_t  group_cipher;
    uint16_t key_ver;
    uint64_t last_replay;
    bool     has_ptk;
    bool     has_gtk;
} aic_wpa2_ctx_t;

static inline void aic_wpa2_ctx_init(aic_wpa2_ctx_t *ctx,
                                     const uint8_t spa[6],
                                     const uint8_t aa[6],
                                     const uint8_t pmk[32],
                                     const uint8_t *rsn_ie,
                                     uint8_t rsn_ie_len,
                                     uint8_t pairwise_cipher,
                                     uint8_t group_cipher) {
    memset(ctx, 0, sizeof(*ctx));
    memcpy(ctx->spa, spa, 6U);
    memcpy(ctx->aa, aa, 6U);
    memcpy(ctx->pmk, pmk, 32U);
    if (rsn_ie && rsn_ie_len > 0U && rsn_ie_len <= sizeof(ctx->rsn_ie)) {
        memcpy(ctx->rsn_ie, rsn_ie, rsn_ie_len);
        ctx->rsn_ie_len = rsn_ie_len;
    }
    ctx->pairwise_cipher = pairwise_cipher;
    ctx->group_cipher    = group_cipher;
}

static inline int aic_wpa2_build_eapol_reply(const aic_wpa2_ctx_t *ctx,
                                             uint8_t eapol_ver,
                                             uint8_t desc_type,
                                             uint16_t key_ver,
                                             uint16_t reply_info,
                                             uint64_t replay,
                                             const uint8_t *nonce,
                                             const uint8_t *kd,
                                             uint16_t kd_bytes,
                                             uint8_t *out_eth,
                                             size_t out_cap,
                                             size_t *out_len) {
    const size_t reply_body_len = AIC_EAPOL_KEY_BODY_MIN_LEN + (size_t)kd_bytes;
    const size_t reply_pdu_len  = AIC_EAPOL_HDR_LEN + reply_body_len;
    const size_t reply_eth_len  = 14U + reply_pdu_len;
    if (reply_eth_len > out_cap) {
        return -1;
    }
    memset(out_eth, 0, reply_eth_len);
    memcpy(&out_eth[0], ctx->aa, 6U);
    memcpy(&out_eth[6], ctx->spa, 6U);
    out_eth[12] = 0x88U;
    out_eth[13] = 0x8EU;

    uint8_t *rpdu = &out_eth[14];
    rpdu[0] = eapol_ver;
    rpdu[1] = 3U; /* EAPOL-Key */
    aic_wpa_put_be16(&rpdu[2], (uint16_t)reply_body_len);

    uint8_t *rbody = &rpdu[4];
    rbody[0] = desc_type;
    aic_wpa_put_be16(&rbody[1], reply_info);
    aic_wpa_put_be16(&rbody[3], 0U);
    aic_wpa_put_be64(&rbody[5], replay);
    if (nonce) {
        memcpy(&rbody[13], nonce, 32U);
    }
    aic_wpa_put_be16(&rbody[93], kd_bytes);
    if (kd && kd_bytes > 0U) {
        memcpy(&rbody[95], kd, kd_bytes);
    }

    uint8_t mic[16];
    if (aic_wpa2_compute_eapol_mic(key_ver, &ctx->ptk[0], rpdu, reply_pdu_len, mic) != 0) {
        return -1;
    }
    memcpy(&rbody[77], mic, 16U);
    *out_len = reply_eth_len;
    return 1;
}

/*
 * Process an incoming Ethernet II EAPOL frame (`0x888E`), and optionally emit
 * a reply Ethernet II EAPOL frame (M2, M4, or Group M2).
 * Returns:
 *   1 -> reply written to `out_eth`, install_keys_now set if PTK+GTK ready
 *   0 -> frame ignored cleanly
 *  -1 -> malformed / MIC failure / unwrap failure
 */
static inline int aic_wpa2_handle_rx_eapol(aic_wpa2_ctx_t *ctx,
                                           const uint8_t *eth_in,
                                           size_t eth_in_len,
                                           uint8_t *out_eth,
                                           size_t out_cap,
                                           size_t *out_len,
                                           bool *out_keys_ready,
                                           bool *out_gtk_rekey) {
    if (out_len) *out_len = 0U;
    if (out_keys_ready) *out_keys_ready = false;
    if (out_gtk_rekey) *out_gtk_rekey = false;

    if (!ctx || !eth_in || !out_eth || !out_len ||
        eth_in_len < 14U + AIC_EAPOL_HDR_LEN + AIC_EAPOL_KEY_BODY_MIN_LEN) {
        return -1;
    }

    const uint8_t *pdu = &eth_in[14];
    const size_t pdu_len = eth_in_len - 14U;
    if (pdu[1] != 3U) { /* not EAPOL-Key */
        return 0;
    }
    const uint16_t body_len = aic_wpa_get_be16(&pdu[2]);
    if (body_len < AIC_EAPOL_KEY_BODY_MIN_LEN || (size_t)body_len + 4U > pdu_len) {
        return -1;
    }

    const uint8_t *key_body = &pdu[4];
    const uint8_t desc_type = key_body[0];
    if (desc_type != 2U && desc_type != 254U) {
        return 0;
    }

    const uint16_t key_info = aic_wpa_get_be16(&key_body[1]);
    const uint16_t key_ver  = (uint16_t)(key_info & AIC_KEY_INFO_VER_MASK);
    const uint64_t replay   = aic_wpa_get_be64(&key_body[5]);
    const uint16_t kd_len   = aic_wpa_get_be16(&key_body[93]);
    if (AIC_EAPOL_KEY_BODY_MIN_LEN + (size_t)kd_len > (size_t)body_len) {
        return -1;
    }

    const bool is_pw  = (key_info & AIC_KEY_INFO_PAIRWISE) != 0U;
    const bool is_ack = (key_info & AIC_KEY_INFO_ACK) != 0U;
    const bool is_mic = (key_info & AIC_KEY_INFO_MIC) != 0U;
    const bool is_enc = (key_info & AIC_KEY_INFO_ENC_KEY_DATA) != 0U;

    if (!is_ack) {
        return 0;
    }

    /* 1. Pairwise M1: Pairwise=1, Ack=1, MIC=0 */
    if (is_pw && !is_mic) {
        if (CCRandomGenerateBytes(ctx->snonce, sizeof(ctx->snonce)) != kCCSuccess) {
            return -1;
        }
        memcpy(ctx->anonce, &key_body[13], 32U);
        ctx->key_ver     = key_ver;
        ctx->last_replay = replay;
        aic_wpa2_derive_ptk(ctx->pmk,
                            ctx->aa,
                            ctx->spa,
                            ctx->anonce,
                            ctx->snonce,
                            (key_ver == AIC_KEY_INFO_VER_AES_CMAC),
                            ctx->ptk);
        ctx->has_ptk = true;

        const uint16_t m2_info = (uint16_t)(key_ver | AIC_KEY_INFO_PAIRWISE | AIC_KEY_INFO_MIC);
        return aic_wpa2_build_eapol_reply(ctx, pdu[0], desc_type, key_ver, m2_info, replay,
                                          ctx->snonce, ctx->rsn_ie, ctx->rsn_ie_len,
                                          out_eth, out_cap, out_len);
    }

    /* All signed messages (M3 or Group M1) require valid PTK + MIC verification */
    if (is_mic) {
        if (!ctx->has_ptk) {
            return -1;
        }
        uint8_t calc_mic[16];
        if (aic_wpa2_compute_eapol_mic(key_ver, &ctx->ptk[0], pdu, AIC_EAPOL_HDR_LEN + body_len, calc_mic) != 0 ||
            memcmp(calc_mic, &key_body[77], 16U) != 0) {
            return -1;
        }
        ctx->last_replay = replay;
    }

    /* 2. Pairwise M3: Pairwise=1, Ack=1, MIC=1, EncryptedKeyData=1 */
    if (is_pw && is_mic) {
        if (memcmp(ctx->anonce, &key_body[13], 32U) != 0) {
            return -1;
        }
        if (is_enc && kd_len > 0U) {
            if (aic_wpa2_extract_gtk(&ctx->ptk[16],
                                     &key_body[95],
                                     kd_len,
                                     &ctx->gtk_idx,
                                     ctx->gtk,
                                     &ctx->gtk_len) == 0) {
                ctx->has_gtk = true;
            }
        }
        const uint16_t m4_info = (uint16_t)(key_ver | AIC_KEY_INFO_PAIRWISE |
                                            AIC_KEY_INFO_MIC | AIC_KEY_INFO_SECURE);
        const int rc = aic_wpa2_build_eapol_reply(ctx, pdu[0], desc_type, key_ver, m4_info, replay,
                                                  NULL, NULL, 0U, out_eth, out_cap, out_len);
        if (rc == 1 && out_keys_ready) {
            *out_keys_ready = ctx->has_ptk;
        }
        return rc;
    }

    /* 3. Group Key Handshake M1: Pairwise=0, Ack=1, MIC=1, EncryptedKeyData=1 */
    if (!is_pw && is_mic && is_enc && kd_len > 0U) {
        if (aic_wpa2_extract_gtk(&ctx->ptk[16],
                                 &key_body[95],
                                 kd_len,
                                 &ctx->gtk_idx,
                                 ctx->gtk,
                                 &ctx->gtk_len) != 0) {
            return -1;
        }
        ctx->has_gtk = true;
        const uint16_t gm2_info = (uint16_t)(key_ver | AIC_KEY_INFO_MIC | AIC_KEY_INFO_SECURE);
        const int rc = aic_wpa2_build_eapol_reply(ctx, pdu[0], desc_type, key_ver, gm2_info, replay,
                                                  NULL, NULL, 0U, out_eth, out_cap, out_len);
        if (rc == 1 && out_gtk_rekey) {
            *out_gtk_rekey = true;
        }
        return rc;
    }

    return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* AIC8800_WPA2_H */
