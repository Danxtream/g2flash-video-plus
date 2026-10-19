/* Bounded, local-clock transport timing. SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>
#include "../message_transport.h"

enum {
    VIDEO_HOP_DIAGNOSTICS = 8, VIDEO_HOP_SLOTS = 16, VIDEO_HOP_STAGES = 7, VIDEO_HOP_BYTES = 172,
    VIDEO_HOP_SIZE_BYTES = 128, VIDEO_HOP_BUCKETS = 4,
    VIDEO_HOP_OPEN = 1, VIDEO_HOP_WIRE, VIDEO_HOP_LOCAL,
    VIDEO_HOP_PEER, VIDEO_HOP_RELAY
};
typedef struct { uint32_t count, sum, maximum; uint16_t bins[4]; } video_hop_row;
/* Slot flags: live1, forwarded2, peer4, local8, relayed16; an unclassified
 * early ACK uses64, plus128 after relay and its low two refusal/history bits.
 * State flags: saturation1, match exhaustion2, refusal/send failure4,
 * contention8, history upper bound16, late peak32, early-cache replacement64,
 * mismatched acceptance CRC128. Never treat a flagged sample as complete. */
typedef struct { uint32_t key, rx, forward, wire, peer, flags, checksum, bytes; } video_hop_slot;
typedef struct { uint32_t count, sum, maximum, bytes; } video_hop_bucket;
typedef struct {
    volatile uint32_t busy, dropped;
    uint32_t generation, stream, token, began, last, flags;
    uint16_t pending, maximum;
    video_hop_row rows[VIDEO_HOP_STAGES];
    video_hop_slot slots[VIDEO_HOP_SLOTS];
    video_hop_bucket buckets[VIDEO_HOP_BUCKETS];
    uint32_t overlapped, incoming;
} video_hop_state;
/* Return zero outside a START-owned video generation, without creating context. */
uint32_t video_hop_tick(void);
/* Record only NAL bodies; transport identity is independent of video sequence. */
void video_hop_open(const cfw_message_stream *, uint8_t, uint8_t, const uint8_t *);
/* Events are best effort; contention and overflow are exposed, never waited on. */
void video_hop_event(uint32_t, uint8_t, uint32_t);
/* Observe another classified, unconfirmed forward before enqueueing this packet.
 * The first-fragment result is retained until its body can be identified as NAL. */
uint32_t video_hop_overlap(uint32_t key);
/* Freeze one bounded page under a nonblocking timing lock. */
int video_hop_snapshot(uint8_t, uint8_t, uint32_t, uint8_t *, uint32_t mode);
