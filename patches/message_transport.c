#include "message_transport.h"
#include "memory.h"
#include "video/power.h"
#include "video/sync.h"
#include "video/profile.h"
#include "video/radio.h"
#include "video/hop.h"

#ifndef CFW_HOP_TICK
#define CFW_HOP_TICK video_hop_tick
#define CFW_HOP_OPEN video_hop_open
#define CFW_HOP_EVENT video_hop_event
#endif
typedef struct { uint32_t rx, forward, cost; } cfw_hop_packet;

static uint32_t cfw_hop_key(cfw_message_stream *s, uint8_t origin) {
    return s->stream_id | (uint32_t)s->message_id << 8 | (uint32_t)origin << 24;
}
static int cfw_hop_nal(const uint8_t *p, uint32_t n) {
    return n >= 6 && p[0] == 31 && (p[1] == 6 || p[1] == 24 || p[1] == 26 || p[1] == 27);
}
__attribute__((noinline)) static void cfw_hop_returns(const uint8_t *data, uint32_t length, uint8_t event, uint32_t detail) {
    if (!CFW_HOP_TICK()) return;
    for (uint32_t i = 3; i + 3 <= length; i = i == 3 ? 11 : i + CFW_ACK_ENTRY_SIZE) {
        uint32_t key = data[i] | (uint32_t)data[i + 1] << 8 | (uint32_t)data[i + 2] << 16 | (uint32_t)data[1] << 24;
        uint32_t crc = i == 3 ? 9 : i + 5;
        CFW_HOP_EVENT(key, event, detail | (event == VIDEO_HOP_PEER && i != 3 ? 2 : 0) |
            (uint32_t)data[crc] << 16 | (uint32_t)data[crc + 1] << 24);
    }
}

#ifndef CFW_STOCK_RECEIVE
#define CFW_STOCK_RECEIVE ((uint32_t (*)(uint8_t, const uint8_t *, uint16_t))0x004cf3e9u)
#define CFW_STOCK_BRIDGE_RECEIVE ((uint32_t (*)(uint32_t, const uint8_t *, uint32_t, uint16_t))0x0045d1a1u)
#define CFW_LENS_SIDE ((uint32_t (*)(void))0x0045cfddu)
/* Both APIs copy the supplied bytes into owned queue storage before returning.
 * SendDataToBoth also delivers a local echo; origin tags below suppress it. */
#define CFW_BRIDGE_SEND ((int (*)(uint16_t, const uint8_t *, uint16_t, void *))0x0046a58du)
#define CFW_BLE_SEND ((int (*)(uint8_t, uint8_t, const uint8_t *, uint16_t))0x0047d72du)
#endif

#define CFW_BRIDGE_REQUEST 1u
#define CFW_BRIDGE_RETURN 2u
#define CFW_BRIDGE_VIDEO_RETURN 3u
#define CFW_ACK_SIZE 9u
#define CFW_ACK_MAX_SIZE (CFW_ACK_SIZE + CFW_ACK_HISTORY * CFW_ACK_ENTRY_SIZE)

#ifndef CFW_STREAM_STATE
static cfw_message_stream *cfw_message_state(uint8_t origin) {
    customCfwContext *ctx = getCustomCfwContext();
    return ctx ? &ctx->message_streams[origin - 1] : 0;
}
#define CFW_STREAM_STATE cfw_message_state
/* Transport parsing runs in receive tasks, outside the image/display locks.
 * Protect live video reserves before and after stock-coordinated allocation. */
static void *cfw_video_message_allocate(uint32_t size) {
    if (!video_transport_room(size)) return 0;
    void *memory = cfw_heap13_malloc(size);
    if (memory && !video_transport_room(0)) {
        cfw_heap13_free(memory); return 0;
    }
    return memory;
}
#define CFW_MESSAGE_MALLOC cfw_video_message_allocate
#define CFW_MESSAGE_FREE cfw_heap13_free
#define CFW_PROFILE_BEGIN video_profile_begin
#define CFW_PROFILE_END video_profile_end
#endif

#ifndef CFW_PROFILE_BEGIN
#define CFW_PROFILE_BEGIN() ((video_profile_capture){0})
#define CFW_PROFILE_END(c, s, n) ((void)(c), (void)(s), (void)(n))
#endif
#ifndef CFW_CONNECTION_EPOCH
#define CFW_CONNECTION_EPOCH video_radio_transport_epoch
#endif

#include "transport_compression.c"

static uint16_t cfw_message_crc(const uint8_t *data, uint16_t size) {
    uint16_t crc = 0xffffu;
    for (uint16_t i = 0; i < size; ++i) {
        crc ^= (uint16_t)data[i] << 8;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (uint16_t)((crc << 1) ^ ((crc & 0x8000u) ? 0x1021u : 0));
    }
    return crc;
}

static uint8_t cfw_message_lens(void) {
    uint32_t side = CFW_LENS_SIDE(); /* Stock identity: 1 = right, 2 = left. */
    return side == 1 ? CFW_MESSAGE_RIGHT : side == 2 ? CFW_MESSAGE_LEFT : 0;
}

static uint32_t cfw_message_validate(const uint8_t *packet, uint16_t length) {
    if (length < 11 || packet[3] != length - 8) return 0xbu;
    if (packet[0] != 0xaa || packet[1] != 0x21 || packet[4] != 1 ||
        packet[5] != 1 || packet[6] != CFW_MESSAGE_SID || packet[7] != 0 ||
        (packet[8] & ~(CFW_MESSAGE_BOTH | CFW_MESSAGE_RESET | CFW_MESSAGE_END))) return 0xau;
    uint16_t expected = (uint16_t)packet[length - 2] |
                        ((uint16_t)packet[length - 1] << 8);
    return cfw_message_crc(packet + 8, length - 10) == expected ? 0 : 0xau;
}

/* Private bridge envelope: kind, ingress-lens bit, body. No borrowed pointer
 * escapes this function. The stock queue wakes its worker on enqueue. */
static int cfw_message_bridge_send(uint8_t kind, uint8_t origin,
                                   const uint8_t *body, uint16_t length, uint32_t epoch) {
    uint8_t envelope[269]; /* Maximum TPL packet plus origin and connection epoch. */
    uint16_t header = kind == CFW_BRIDGE_REQUEST ? 6 : 2;
    if (length > sizeof(envelope) - header) return -1;
    video_profile_capture captured = CFW_PROFILE_BEGIN();
    envelope[0] = kind;
    envelope[1] = origin;
    if (header == 6) {
        for (uint32_t i = 0; i < 4; ++i) envelope[2 + i] = epoch >> (8 * i);
    }
    memcpy(envelope + header, body, length);
    int result = CFW_BRIDGE_SEND(CFW_MESSAGE_SID, envelope, length + header, 0);
    CFW_PROFILE_END(captured, VIDEO_PROFILE_BRIDGE, length + 2);
    return result;
}

int cfw_message_video_reply(const cfw_message_route *route,
                             const uint8_t *reply, uint16_t length) {
    if (!route || !reply || length < 9 || length > route->reply_capacity ||
        length > CFW_VIDEO_REPLY_MAX_SIZE || reply[0] != 31 || reply[1] != route->here ||
        (route->origin != CFW_MESSAGE_LEFT && route->origin != CFW_MESSAGE_RIGHT) ||
        (route->here != CFW_MESSAGE_LEFT && route->here != CFW_MESSAGE_RIGHT) ||
        !reply[7] || reply[7] > CFW_VIDEO_REPLY_MAX_PAGES || reply[6] >= reply[7]) return -1;
    return route->here == route->origin ? CFW_BLE_SEND(1, CFW_MESSAGE_SID, reply, length) :
        cfw_message_bridge_send(CFW_BRIDGE_VIDEO_RETURN, route->origin, reply, length, 0);
}

static void cfw_message_discard(cfw_message_stream *stream) {
    if (stream->buffer) CFW_MESSAGE_FREE(stream->buffer);
    stream->buffer = 0;
    stream->size = stream->used = 0;
    stream->length_bytes = stream->active = 0;
}

static int cfw_message_reply(cfw_message_stream *stream, uint8_t here,
                             uint8_t origin, uint8_t kind, uint16_t size) {
    uint8_t reply[CFW_ACK_MAX_SIZE] = {kind, stream->stream_id,
        (uint8_t)stream->message_id, (uint8_t)(stream->message_id >> 8), here,
        (uint8_t)size, (uint8_t)(size >> 8),
        (uint8_t)stream->checksum, (uint8_t)(stream->checksum >> 8)};
    uint16_t length = CFW_ACK_SIZE;
    if (kind == CFW_MESSAGE_ACK) {
        /* Explicit successes, newest first; never imply success across a gap.
         * Remember processing even if enqueueing this reply fails, so a later
         * reply can repair that loss without executing the command again. */
        unsigned count = (stream->ack_capacity - CFW_ACK_SIZE) / CFW_ACK_ENTRY_SIZE;
        if (count > stream->ack_count) count = stream->ack_count;
        length += count * CFW_ACK_ENTRY_SIZE;
        memcpy(reply + CFW_ACK_SIZE, stream->ack_history,
               count * CFW_ACK_ENTRY_SIZE);
        for (unsigned i = CFW_ACK_HISTORY - 1; i > 0; --i)
            memcpy(stream->ack_history[i], stream->ack_history[i - 1], CFW_ACK_ENTRY_SIZE);
        memcpy(stream->ack_history[0], reply + 1, 3);
        memcpy(stream->ack_history[0] + 3, reply + 5, 4);
        if (stream->ack_count < CFW_ACK_HISTORY) ++stream->ack_count;
    } else {
        stream->ack_count = 0;
    }
    return here == origin ? CFW_BLE_SEND(1, CFW_MESSAGE_SID, reply, length) :
        cfw_message_bridge_send(CFW_BRIDGE_RETURN, origin, reply, length, 0);
}

/* An incomplete record will never reach the decoded-CRC check. Report the
 * original attempt before discarding it, otherwise the phone waits 3.5 seconds
 * for an ACK while later messages fill its window. Only report once per abort. */
static void cfw_message_abort(cfw_message_stream *stream, uint8_t here, uint8_t origin) {
    if (!stream || !stream->active) return;
    cfw_message_reply(stream, here, origin, CFW_MESSAGE_NACK, 0);
    cfw_inflate_reset(stream);
    stream->context_valid = 0;
    cfw_message_discard(stream);
}

/* ACK identifies the stream and message ordinal, independent of packet splits.
 * Several messages ending in one packet must still have distinct ACKs. */
static uint32_t cfw_message_complete(cfw_message_stream *stream, uint8_t here,
                                     uint8_t origin) {
    uint8_t empty = 0;
    const uint8_t *data = stream->buffer ? stream->buffer : &empty;
    uint8_t *decoded = 0;
    uint32_t size = stream->size;
    int valid = !(stream->flags & ~(CFW_MESSAGE_BOTH | CFW_MESSAGE_COMPRESSED | CFW_MESSAGE_RESET_CONTEXT))
        && (stream->flags & CFW_MESSAGE_BOTH) == stream->options;
    uint8_t ack_capacity = stream->packet_capacity < CFW_ACK_MAX_SIZE ?
        stream->packet_capacity : CFW_ACK_MAX_SIZE;
    if (stream->flags & CFW_MESSAGE_RESET_CONTEXT) {
        cfw_inflate_reset(stream);
        /* Also bounds history across reconnects, retries and target changes. */
        stream->ack_count = 0;
        stream->ack_capacity = ack_capacity;
        stream->context_valid = 1;
    }
    if (stream->ack_capacity < ack_capacity)
        stream->ack_capacity = ack_capacity;
    if (stream->epoch && stream->video_capacity < stream->packet_capacity)
        stream->video_capacity = stream->packet_capacity;
    valid = valid && stream->context_valid;
    if (stream->flags & CFW_MESSAGE_COMPRESSED) {
        decoded = CFW_MESSAGE_MALLOC(CFW_MESSAGE_MAX + 1u);
        valid = valid && stream->context_valid && decoded && cfw_inflate_message(stream, decoded, &size);
        data = decoded;
    }
    uint16_t crc = valid ? cfw_message_crc(data, (uint16_t)size) : 0;
    valid = valid && crc == stream->checksum;
    cfw_message_route route = {here, origin, stream->options,
                               stream->epoch ? stream->video_capacity : stream->packet_capacity, stream->message_id};
    int nal = valid && CFW_HOP_TICK() && cfw_hop_nal(data, size);
    if (nal) {
        if (stream->flags & CFW_MESSAGE_COMPRESSED) CFW_HOP_OPEN(stream, origin, here, data);
        CFW_HOP_EVENT(cfw_hop_key(stream, origin), VIDEO_HOP_WIRE, stream->hop_enqueue_ticks);
    }
    if (valid && cfw_message_received_routed(data, (uint16_t)size, crc, &route) != 0) valid = 0;
    if (!valid) {
        cfw_inflate_reset(stream);
        stream->context_valid = 0; /* only an explicit record reset can recover */
    }
    int result = cfw_message_reply(stream, here, origin,
        valid ? CFW_MESSAGE_ACK : CFW_MESSAGE_NACK, (uint16_t)size);
    if (nal) CFW_HOP_EVENT(cfw_hop_key(stream, origin), VIDEO_HOP_LOCAL, !valid || result != 0);
    if (decoded) CFW_MESSAGE_FREE(decoded);
    if (stream->buffer) CFW_MESSAGE_FREE(stream->buffer);
    stream->buffer = 0;
    stream->size = stream->used = stream->length_bytes = 0;
    ++stream->message_id;
    return result == 0 ? 0 : 6;
}

/* The clear five-byte header may span packets. Allocate only after it arrives.
 * No packet boundary has any meaning to the record parser. */
static uint32_t cfw_message_process(const uint8_t *packet, uint16_t length,
                                    uint8_t here, uint8_t origin, uint32_t epoch, cfw_hop_packet hop) {
    cfw_message_stream *stream = CFW_STREAM_STATE(origin);
    if (!stream) return 6;
    if (stream->epoch != epoch) {
        cfw_message_discard(stream); cfw_inflate_reset(stream);
        stream->context_valid = 0; stream->video_capacity = CFW_ACK_SIZE;
        stream->epoch = epoch;
    }
    uint8_t options = packet[8] & CFW_MESSAGE_BOTH;
    if (packet[8] & CFW_MESSAGE_RESET) {
        if (stream->length_bytes) cfw_message_abort(stream, here, origin);
        cfw_message_discard(stream);
        stream->active = 1;
        stream->next_sequence = stream->stream_id = packet[2];
        stream->message_id = 0;
        stream->options = options;
        stream->packet_capacity = CFW_ACK_SIZE;
    }
    if (!stream->active || stream->next_sequence != packet[2] || stream->options != options) {
        cfw_message_abort(stream, here, origin);
        cfw_message_discard(stream);
        return 0xau;
    }
    ++stream->next_sequence; /* uint8 wrap is intentional. */
    /* Replies must still fit one ATT notification, including on MTU 23 links.
     * An observed request proves the ingress can carry that many bytes. Track
     * this per compression context (reset on reconnect). Ordinary ACKs retain
     * their 40-byte envelope bound; video has its own 200-byte body bound.
     * The bridge sees the same request sizes. */
    /* Stock TPL reserves 11 bytes per fragment, including its CRC allowance. */
    unsigned capacity = length - 11;
    if (capacity > CFW_VIDEO_REPLY_MAX_SIZE) capacity = CFW_VIDEO_REPLY_MAX_SIZE;
    if (capacity > stream->packet_capacity) stream->packet_capacity = capacity;
    uint32_t status = 0;
    uint16_t end = length - 2, cursor = 9;
    while (cursor < end) {
        if (stream->length_bytes < 5) {
            if (!stream->length_bytes && hop.rx) {
                stream->hop_rx = hop.rx; stream->hop_forward = hop.forward; stream->hop_enqueue_ticks = 0;
            }
            uint8_t byte = packet[cursor++];
            switch (stream->length_bytes++) {
            case 0: stream->flags = byte; break;
            case 1: stream->size = byte; break;
            case 2: stream->size |= (uint16_t)byte << 8; break;
            case 3: stream->checksum = byte; break;
            case 4: stream->checksum |= (uint16_t)byte << 8; break;
            }
            if (stream->length_bytes < 5) continue;
            if (stream->size) {
                stream->buffer = CFW_MESSAGE_MALLOC(stream->size);
                if (!stream->buffer) {
                    cfw_message_abort(stream, here, origin);
                    return 6; /* Cannot resume at a guessed record boundary. */
                }
            }
        }
        uint16_t count = stream->size - stream->used;
        if (count > end - cursor) count = end - cursor;
        if (count) memcpy(stream->buffer + stream->used, packet + cursor, count);
        uint16_t previous = stream->used;
        stream->used += count;
        if (hop.rx) stream->hop_enqueue_ticks += hop.cost;
        if (hop.rx && !(stream->flags & CFW_MESSAGE_COMPRESSED) && previous < 6 &&
            cfw_hop_nal(stream->buffer, stream->used)) CFW_HOP_OPEN(stream, origin, here, stream->buffer);
        cursor += count;
        if (stream->used == stream->size) {
            uint32_t result = cfw_message_complete(stream, here, origin);
            if (result) status = result;
        }
    }
    if (packet[8] & CFW_MESSAGE_END) {
        if (stream->length_bytes) {
            cfw_message_abort(stream, here, origin);
            status = 0xbu; /* Truncated header or payload. */
        }
        cfw_message_discard(stream);
    }
    return status;
}

uint32_t cfw_receive_packet(uint8_t pipe, const uint8_t *packet, uint16_t length) {
    if (pipe != 0 || !packet || length < 8 || packet[0] != 0xaa || packet[6] != CFW_MESSAGE_SID)
        return CFW_STOCK_RECEIVE(pipe, packet, length);
    /* Consume malformed private packets before the stock multipart allocator. */
    uint32_t status = cfw_message_validate(packet, length);
    if (status) {
        uint8_t here = cfw_message_lens();
        if (here) cfw_message_abort(CFW_STREAM_STATE(here), here, here);
        return status;
    }
    uint8_t here = cfw_message_lens();
    if (!here) return 6;
    uint32_t epoch = CFW_CONNECTION_EPOCH();
    video_profile_capture captured = CFW_PROFILE_BEGIN();
    cfw_hop_packet hop = {CFW_HOP_TICK(), 0, 0};
    if (packet[8] & (here ^ CFW_MESSAGE_BOTH))
    {
        hop.forward = CFW_HOP_TICK();
        if (cfw_message_bridge_send(CFW_BRIDGE_REQUEST, here, packet, length, epoch) != 0) status = 6;
        if (hop.rx) hop.cost = CFW_HOP_TICK() - hop.forward;
    }
    /* A failed peer enqueue does not prevent the selected local lens processing. */
    if (packet[8] & here) {
        uint32_t local_status = cfw_message_process(packet, length, here, here, epoch, hop);
        if (local_status) status = local_status;
    }
    CFW_PROFILE_END(captured, VIDEO_PROFILE_INGRESS, length);
    return status;
}

/* Intercept bridge delivery BEFORE SendUserDataToThreadPool. Its eight workers
 * can reorder callbacks; reconstruction instead runs on ordered bridge arrival.
 * Ordinary app IDs retain the original worker dispatch. The buffer is borrowed
 * only until return. Local echoes never process or forward a request. */
uint32_t cfw_message_bridge_received(uint32_t app_id, const uint8_t *data,
                                     uint32_t length, uint16_t event) {
    if (app_id != CFW_MESSAGE_SID)
        return CFW_STOCK_BRIDGE_RECEIVE(app_id, data, length, event);
    if (!data || length < 2) return 0xbu;
    uint8_t here = cfw_message_lens(), origin = data[1];
    if (!here || (origin != CFW_MESSAGE_LEFT && origin != CFW_MESSAGE_RIGHT)) return 0xau;
    if (data[0] == VIDEO_POWER_BRIDGE) return video_power_received(data, length);
    if (data[0] == VIDEO_SYNC_BRIDGE) return here == origin ? 0 : video_sync_received(data, length);
    if (data[0] == CFW_BRIDGE_REQUEST) {
        if (here == origin) return 0;
        if (length < 17 || length > 269) return 0xbu;
        uint32_t epoch = (uint32_t)data[2] | (uint32_t)data[3] << 8 |
            (uint32_t)data[4] << 16 | (uint32_t)data[5] << 24;
        uint32_t status = cfw_message_validate(data + 6, (uint16_t)(length - 6));
        if (status) {
            cfw_message_abort(CFW_STREAM_STATE(origin), here, origin);
            return status;
        }
        video_profile_capture captured = CFW_PROFILE_BEGIN();
        cfw_hop_packet hop = {CFW_HOP_TICK(), 0, 0};
        status = (data[14] & here) ? cfw_message_process(data + 6, length - 6, here, origin, epoch, hop) : 0;
        CFW_PROFILE_END(captured, VIDEO_PROFILE_INGRESS, length - 2);
        return status;
    }
    if (data[0] == CFW_BRIDGE_RETURN) {
        if (here != origin) return 0;
        if (length < CFW_ACK_SIZE + 2 || length > CFW_ACK_MAX_SIZE + 2 ||
            (length - CFW_ACK_SIZE - 2) % CFW_ACK_ENTRY_SIZE != 0) return 0xbu;
        if ((data[2] != CFW_MESSAGE_ACK && data[2] != CFW_MESSAGE_NACK) || data[6] != (origin ^ CFW_MESSAGE_BOTH)) return 0xau;
        if (data[2] == CFW_MESSAGE_NACK && length != CFW_ACK_SIZE + 2) return 0xbu;
        /* ACK history repairs lost replies too; mark their later arrival as an
         * upper bound, rather than pretending it was the original arrival. */
        cfw_hop_returns(data, length, VIDEO_HOP_PEER, data[2] == CFW_MESSAGE_NACK);
        int sent = CFW_BLE_SEND(1, CFW_MESSAGE_SID, data + 2, length - 2);
        cfw_hop_returns(data, length, VIDEO_HOP_RELAY, sent != 0);
        return sent == 0 ? 0 : 6;
    }
    if (data[0] == CFW_BRIDGE_VIDEO_RETURN) {
        if (here != origin) return 0;
        if (length < 11 || length > CFW_VIDEO_REPLY_MAX_SIZE + 2) return 0xbu;
        if (data[2] != 31 || data[3] != (origin ^ CFW_MESSAGE_BOTH) ||
            !data[9] || data[9] > CFW_VIDEO_REPLY_MAX_PAGES || data[8] >= data[9]) return 0xau;
        return CFW_BLE_SEND(1, CFW_MESSAGE_SID, data + 2, length - 2) == 0 ? 0 : 6;
    }
    return 0xau;
}
