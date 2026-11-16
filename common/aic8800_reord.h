/*
 * aic8800_reord.h - Zero-heap static 802.11n/ac/ax RX BlockAck reorder window
 *
 * Preallocated 8 TIDs x 64-slot circular reorder buffers (zero dynamic heap
 * allocation on hot path), matching Linux aic8800_fdrv (SN_LESS, wsize=64).
 */

#ifndef AIC8800_REORD_H
#define AIC8800_REORD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AIC_REORD_TID_CNT     8U
#define AIC_REORD_WINSIZE     64U
#define AIC_REORD_SN_MASK     0x0FFFU
#define AIC_REORD_SN_UNINIT   0xFFFFU
#define AIC_REORD_TIMEOUT_MS  50U

/* 12-bit modular less-than matching rwnx_rx.h SN_LESS(a, b) */
static inline bool aic_reord_sn_less(uint16_t a, uint16_t b) {
    return (((uint16_t)(a - b)) & 0x0800U) != 0U;
}

typedef void (*aic_reord_deliver_fn)(void *head_pkt, uint32_t total_bytes, void *ctx);
typedef void (*aic_reord_append_fn)(void *tail_pkt, void *new_pkt);
typedef void (*aic_reord_drop_fn)(void *head_pkt, void *ctx);

typedef struct {
    void    *head;
    void    *tail;
    uint32_t total_bytes;
    uint16_t sn;
    uint8_t  occupied;
    uint8_t  reserved;
} aic_reord_slot_t;

typedef struct {
    uint16_t         ind_sn;
    uint16_t         pending_count;
    aic_reord_slot_t slots[AIC_REORD_WINSIZE];
} aic_reord_tid_t;

typedef struct {
    aic_reord_tid_t tids[AIC_REORD_TID_CNT];
    uint64_t        delivered_in_order;
    uint64_t        buffered_reorders;
    uint64_t        dup_drops;
    uint64_t        timeout_flushes;
    uint64_t        window_jumps;
} aic_rx_reord_t;

static inline void aic_rx_reord_init(aic_rx_reord_t *r) {
    if (!r) {
        return;
    }
    memset(r, 0, sizeof(*r));
    for (uint8_t t = 0U; t < AIC_REORD_TID_CNT; ++t) {
        r->tids[t].ind_sn = AIC_REORD_SN_UNINIT;
    }
}

static inline void aic_rx_reord_drain_contiguous(aic_rx_reord_t *r,
                                                 aic_reord_tid_t *q,
                                                 aic_reord_deliver_fn deliver,
                                                 void *ctx) {
    while (q->pending_count > 0U && q->ind_sn != AIC_REORD_SN_UNINIT) {
        aic_reord_slot_t *s = &q->slots[q->ind_sn & (AIC_REORD_WINSIZE - 1U)];
        if (!s->occupied || s->sn != q->ind_sn) {
            break;
        }
        void *head = s->head;
        const uint32_t bytes = s->total_bytes;
        memset(s, 0, sizeof(*s));
        q->pending_count--;
        q->ind_sn = (uint16_t)((q->ind_sn + 1U) & AIC_REORD_SN_MASK);
        if (deliver && head) {
            deliver(head, bytes, ctx);
        }
        r->delivered_in_order++;
    }
}

/*
 * Flush all pending packets in a single TID (in SN order, starting at ind_sn)
 * and reset ind_sn to uninitialized. Call with drop=true to discard instead of
 * delivering (e.g., on disconnect/stop).
 */
static inline void aic_rx_reord_flush_tid(aic_rx_reord_t *r,
                                          uint8_t tid,
                                          bool drop_only,
                                          aic_reord_deliver_fn deliver,
                                          aic_reord_drop_fn drop,
                                          void *ctx) {
    if (!r || tid >= AIC_REORD_TID_CNT) {
        return;
    }
    aic_reord_tid_t *q = &r->tids[tid];
    if (q->pending_count > 0U && q->ind_sn != AIC_REORD_SN_UNINIT) {
        for (uint16_t step = 0U; step < AIC_REORD_WINSIZE && q->pending_count > 0U; ++step) {
            const uint16_t cur_sn = (uint16_t)((q->ind_sn + step) & AIC_REORD_SN_MASK);
            aic_reord_slot_t *s = &q->slots[cur_sn & (AIC_REORD_WINSIZE - 1U)];
            if (s->occupied) {
                void *head = s->head;
                const uint32_t bytes = s->total_bytes;
                memset(s, 0, sizeof(*s));
                q->pending_count--;
                if (drop_only) {
                    if (drop && head) {
                        drop(head, ctx);
                    }
                } else {
                    if (deliver && head) {
                        deliver(head, bytes, ctx);
                    }
                    r->delivered_in_order++;
                }
            }
        }
    }
    memset(q->slots, 0, sizeof(q->slots));
    q->pending_count = 0U;
    q->ind_sn        = AIC_REORD_SN_UNINIT;
}

static inline void aic_rx_reord_flush_all(aic_rx_reord_t *r,
                                          bool drop_only,
                                          aic_reord_deliver_fn deliver,
                                          aic_reord_drop_fn drop,
                                          void *ctx) {
    if (!r) {
        return;
    }
    for (uint8_t t = 0U; t < AIC_REORD_TID_CNT; ++t) {
        aic_rx_reord_flush_tid(r, t, drop_only, deliver, drop, ctx);
    }
}

/*
 * Force-advance reorder windows across all TIDs on 50ms timeout.
 * Returns true if any TID still has buffered future packets needing re-arm.
 */
static inline bool aic_rx_reord_timeout_flush(aic_rx_reord_t *r,
                                              aic_reord_deliver_fn deliver,
                                              void *ctx) {
    if (!r) {
        return false;
    }
    bool any_pending = false;
    for (uint8_t t = 0U; t < AIC_REORD_TID_CNT; ++t) {
        aic_reord_tid_t *q = &r->tids[t];
        if (q->pending_count == 0U || q->ind_sn == AIC_REORD_SN_UNINIT) {
            continue;
        }
        bool advanced = false;
        for (uint16_t step = 0U; step < AIC_REORD_WINSIZE; ++step) {
            const uint16_t probe = (uint16_t)((q->ind_sn + step) & AIC_REORD_SN_MASK);
            const aic_reord_slot_t *s = &q->slots[probe & (AIC_REORD_WINSIZE - 1U)];
            if (s->occupied && s->sn == probe) {
                q->ind_sn = probe;
                advanced = true;
                break;
            }
        }
        if (advanced) {
            r->timeout_flushes++;
            aic_rx_reord_drain_contiguous(r, q, deliver, ctx);
        }
        if (q->pending_count > 0U) {
            any_pending = true;
        }
    }
    return any_pending;
}

/*
 * Process one incoming Ethernet packet through the BlockAck reorder window.
 * Caller owns `pkt`; `reord` either delivers immediately, stores it into the
 * slot ring (returning needs_timer = true if buffered out-of-order), or drops
 * duplicates via `drop`.
 */
static inline void aic_rx_reord_submit(aic_rx_reord_t *r,
                                       uint8_t tid,
                                       uint16_t sn,
                                       void *pkt,
                                       uint32_t pkt_bytes,
                                       bool same_mpdu_subframe,
                                       aic_reord_deliver_fn deliver,
                                       aic_reord_append_fn append,
                                       aic_reord_drop_fn drop,
                                       void *ctx,
                                       bool *out_needs_timer) {
    if (out_needs_timer) {
        *out_needs_timer = false;
    }
    if (!r || !pkt) {
        return;
    }
    if (tid >= AIC_REORD_TID_CNT) {
        if (deliver) {
            deliver(pkt, pkt_bytes, ctx);
        }
        return;
    }

    sn &= AIC_REORD_SN_MASK;
    aic_reord_tid_t *q = &r->tids[tid];

    /* Continuation subframes of an A-MSDU share the same MPDU SN */
    if (same_mpdu_subframe) {
        aic_reord_slot_t *s = &q->slots[sn & (AIC_REORD_WINSIZE - 1U)];
        if (s->occupied && s->sn == sn && append) {
            append(s->tail, pkt);
            s->tail         = pkt;
            s->total_bytes += pkt_bytes;
            if (out_needs_timer) {
                *out_needs_timer = (q->pending_count > 0U);
            }
            return;
        }
        /* First subframe was already delivered in-order; deliver tail subframe immediately */
        if (deliver) {
            deliver(pkt, pkt_bytes, ctx);
        }
        return;
    }

    if (q->ind_sn == AIC_REORD_SN_UNINIT) {
        q->ind_sn = sn;
    }

    /* Behind expected window: deliver immediately without stalling */
    if (aic_reord_sn_less(sn, q->ind_sn)) {
        if (deliver) {
            deliver(pkt, pkt_bytes, ctx);
        }
        r->delivered_in_order++;
        if (out_needs_timer) {
            *out_needs_timer = (q->pending_count > 0U);
        }
        return;
    }

    /* Exact expected SN: deliver immediately and drain contiguous successor slots */
    if (sn == q->ind_sn) {
        if (deliver) {
            deliver(pkt, pkt_bytes, ctx);
        }
        r->delivered_in_order++;
        q->ind_sn = (uint16_t)((q->ind_sn + 1U) & AIC_REORD_SN_MASK);
        aic_rx_reord_drain_contiguous(r, q, deliver, ctx);
        if (out_needs_timer) {
            *out_needs_timer = (q->pending_count > 0U);
        }
        return;
    }

    /* Ahead of window: check if window advance jump is required */
    const uint16_t dist = (uint16_t)((sn - q->ind_sn) & AIC_REORD_SN_MASK);
    if (dist >= AIC_REORD_WINSIZE) {
        r->window_jumps++;
        const uint16_t new_ind = (uint16_t)((sn - (AIC_REORD_WINSIZE - 1U)) & AIC_REORD_SN_MASK);
        while (q->ind_sn != new_ind) {
            aic_reord_slot_t *s = &q->slots[q->ind_sn & (AIC_REORD_WINSIZE - 1U)];
            if (s->occupied) {
                void *head = s->head;
                const uint32_t bytes = s->total_bytes;
                memset(s, 0, sizeof(*s));
                q->pending_count--;
                if (deliver && head) {
                    deliver(head, bytes, ctx);
                }
                r->delivered_in_order++;
            }
            q->ind_sn = (uint16_t)((q->ind_sn + 1U) & AIC_REORD_SN_MASK);
        }
        /* Flush any newly aligned head slots up to first gap */
        aic_rx_reord_drain_contiguous(r, q, deliver, ctx);
    }

    aic_reord_slot_t *dst = &q->slots[sn & (AIC_REORD_WINSIZE - 1U)];
    if (dst->occupied) {
        if (dst->sn == sn) {
            /* Duplicate 802.11 retry of an already-buffered SN */
            r->dup_drops++;
            if (drop) {
                drop(pkt, ctx);
            }
            if (out_needs_timer) {
                *out_needs_timer = (q->pending_count > 0U);
            }
            return;
        }
        /* Stale slot eviction fallback */
        void *old_head = dst->head;
        const uint32_t old_bytes = dst->total_bytes;
        memset(dst, 0, sizeof(*dst));
        q->pending_count--;
        if (deliver && old_head) {
            deliver(old_head, old_bytes, ctx);
        }
    }

    dst->head        = pkt;
    dst->tail        = pkt;
    dst->total_bytes = pkt_bytes;
    dst->sn          = sn;
    dst->occupied    = 1U;
    q->pending_count++;
    r->buffered_reorders++;

    if (out_needs_timer) {
        *out_needs_timer = (q->pending_count > 0U);
    }
}

#ifdef __cplusplus
}
#endif

#endif /* AIC8800_REORD_H */
