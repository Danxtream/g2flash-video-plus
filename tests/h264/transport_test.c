/* Production private transport with host zlib and copied stock queues.
 * SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "../../patches/message_transport.h"

static cfw_message_stream streams[2];
static uint32_t side = 2, received, refused, stock_calls, live;
static cfw_message_route last_route;
static uint8_t last_payload[4096];
static uint16_t last_size;
typedef struct { uint16_t size; uint8_t bytes[265]; } test_packet;
static test_packet ble[2048], bridge[2048];
static uint32_t ble_count, bridge_count;

static void *test_alloc(uint32_t n) {
    void *p = malloc(n); if (p) ++live; return p;
}
static void test_free(void *p) { if (p) { assert(live); --live; free(p); } }
static cfw_message_stream *test_stream(uint8_t origin) {
    assert(origin == 1 || origin == 2); return &streams[origin - 1];
}
static uint32_t test_stock(uint8_t pipe, const uint8_t *p, uint16_t n) {
    (void)pipe; (void)p; (void)n; ++stock_calls; return 17;
}
static uint32_t test_stock_bridge(uint32_t app, const uint8_t *p, uint32_t n, uint16_t event) {
    (void)app; (void)p; (void)n; (void)event; ++stock_calls; return 19;
}
static uint32_t test_side(void) { return side; }
static int test_ble(uint8_t pipe, uint8_t sid, const uint8_t *p, uint16_t n) {
    assert(pipe == 1 && sid == CFW_MESSAGE_SID && n <= 30 && ble_count < 2048);
    ble[ble_count].size = n; memcpy(ble[ble_count++].bytes, p, n); return 0;
}
static int test_bridge(uint16_t app, const uint8_t *p, uint16_t n, void *argument) {
    assert(app == CFW_MESSAGE_SID && !argument && n <= 265 && bridge_count < 2048);
    bridge[bridge_count].size = n; memcpy(bridge[bridge_count++].bytes, p, n); return 0;
}
#define CFW_STOCK_RECEIVE test_stock
#define CFW_STOCK_BRIDGE_RECEIVE test_stock_bridge
#define CFW_LENS_SIDE test_side
#define CFW_BLE_SEND test_ble
#define CFW_BRIDGE_SEND test_bridge
#define CFW_STREAM_STATE test_stream
#define CFW_MESSAGE_MALLOC test_alloc
#define CFW_MESSAGE_FREE test_free
#define CFW_ZSTREAM z_stream
#define CFW_ZINIT(s) inflateInit(s)
#define CFW_ZINFLATE(s) inflate(s, Z_NO_FLUSH)
#define CFW_ZEND(s) inflateEnd(s)
#include "../../patches/message_transport.c"

int cfw_message_received(const uint8_t *p, uint16_t n, uint16_t crc) {
    (void)p; (void)n; (void)crc; assert(0); return -1;
}
int cfw_message_received_routed(const uint8_t *p, uint16_t n, uint16_t crc,
                                const cfw_message_route *route) {
    assert(n <= sizeof(last_payload) && cfw_message_crc(p, n) == crc);
    ++received; last_route = *route; last_size = n; memcpy(last_payload, p, n);
    if (refused) return -1;
    if (n && p[0] == 31) {
        uint8_t reply[9] = {31, route->here, 1, 0, 0, 0, 0, 1, 1};
        assert(!cfw_message_video_reply(route, reply, sizeof(reply)));
    }
    return 0;
}

static void send_record(const uint8_t *p, uint16_t n, uint8_t targets,
                         uint8_t flags, uint8_t sequence, z_stream *z) {
    uint8_t wire[8192], record[8197];
    uint32_t wire_size = n;
    if (z) {
        z->next_in = (void *)p; z->avail_in = n;
        z->next_out = wire; z->avail_out = sizeof(wire);
        assert(deflate(z, Z_SYNC_FLUSH) == Z_OK && !z->avail_in);
        wire_size = sizeof(wire) - z->avail_out;
        flags |= CFW_MESSAGE_COMPRESSED;
    } else memcpy(wire, p, n);
    uint16_t crc = cfw_message_crc(p, n);
    record[0] = targets | flags; record[1] = wire_size; record[2] = wire_size >> 8;
    record[3] = crc; record[4] = crc >> 8; memcpy(record + 5, wire, wire_size);
    uint32_t total = wire_size + 5, offset = 0;
    do {
        uint32_t count = total - offset;
        if (count > 9) count = 9; /* MTU23: 20-byte notification/packet payload. */
        uint8_t packet[20] = {0xaa, 0x21, sequence++, count + 3, 1, 1, CFW_MESSAGE_SID, 0};
        packet[8] = targets | (!offset ? CFW_MESSAGE_RESET : 0) |
                    (offset + count == total ? CFW_MESSAGE_END : 0);
        memcpy(packet + 9, record + offset, count);
        crc = cfw_message_crc(packet + 8, count + 1);
        packet[9 + count] = crc; packet[10 + count] = crc >> 8;
        assert(!cfw_receive_packet(0, packet, count + 11));
        memset(packet, 0xa5, sizeof(packet)); /* Stock delivery copied it. */
        offset += count;
    } while (offset < total);
}
static void cleanup(void) {
    for (uint32_t i = 0; i < 2; ++i) {
        cfw_message_discard(&streams[i]); cfw_inflate_reset(&streams[i]);
    }
    assert(!live);
    memset(streams, 0, sizeof(streams));
    received = refused = ble_count = bridge_count = 0; side = 2;
}
int main(void) {
    uint8_t body[24] = {31, 0, 1};
    send_record(body, sizeof(body), CFW_MESSAGE_LEFT, CFW_MESSAGE_RESET_CONTEXT, 250, 0);
    assert(received == 1 && last_route.here == 1 && last_route.origin == 1 &&
           last_route.targets == 1 && last_route.reply_capacity == 9 &&
           !last_route.ordinal && last_size == 24 && !memcmp(body, last_payload, 24));
    assert(ble_count == 2 && ble[0].bytes[0] == 31 && ble[1].bytes[0] == CFW_MESSAGE_ACK);
    cleanup();

    send_record(body, sizeof(body), CFW_MESSAGE_RIGHT, CFW_MESSAGE_RESET_CONTEXT, 12, 0);
    assert(!received && !ble_count && bridge_count);
    uint32_t requests = bridge_count;
    side = 1;
    for (uint32_t i = 0; i < requests; ++i)
        assert(!cfw_message_bridge_received(CFW_MESSAGE_SID, bridge[i].bytes, bridge[i].size, 0));
    assert(received == 1 && last_route.here == 2 && last_route.origin == 1 &&
           last_route.targets == 2 && bridge_count == requests + 2 && !ble_count);
    side = 2;
    for (uint32_t i = requests; i < bridge_count; ++i)
        assert(!cfw_message_bridge_received(CFW_MESSAGE_SID, bridge[i].bytes, bridge[i].size, 0));
    assert(ble_count == 2 && ble[0].bytes[0] == 31 && ble[0].bytes[1] == 2 &&
           ble[1].bytes[0] == CFW_MESSAGE_ACK);
    bridge[requests].bytes[3] = 1; /* A peer cannot forge the origin's identity. */
    assert(cfw_message_bridge_received(CFW_MESSAGE_SID, bridge[requests].bytes,
                                       bridge[requests].size, 0) == 0xa);
    cleanup();

    z_stream z = {0}; assert(deflateInit(&z, 6) == Z_OK);
    body[0] = 7;
    send_record(body, 24, 1, CFW_MESSAGE_RESET_CONTEXT, 1, &z);
    assert(received == 1 && !memcmp(body, last_payload, 24));
    send_record(body, 24, 1, 0, 20, &z);
    assert(received == 2 && !memcmp(body, last_payload, 24));
    refused = 1;
    send_record(body, 24, 1, 0, 40, &z);
    assert(ble[ble_count - 1].bytes[0] == CFW_MESSAGE_NACK && !streams[0].context_valid);
    refused = 0;
    send_record(body, 24, 1, 0, 60, &z);
    assert(received == 3 && ble[ble_count - 1].bytes[0] == CFW_MESSAGE_NACK);
    assert(deflateEnd(&z) == Z_DATA_ERROR); /* A SYNC_FLUSH stream is unfinished. */
    z = (z_stream){0}; assert(deflateInit(&z, 6) == Z_OK);
    send_record(body, 24, 1, CFW_MESSAGE_RESET_CONTEXT, 80, &z);
    assert(received == 4 && ble[ble_count - 1].bytes[0] == CFW_MESSAGE_ACK);
    deflateEnd(&z); cleanup();
    uint8_t nal_record[4096]; memset(nal_record, 0x55, sizeof(nal_record));
    nal_record[0] = 31; nal_record[1] = 6; nal_record[10] = 0x67;
    send_record(nal_record, sizeof(nal_record), 1, CFW_MESSAGE_RESET_CONTEXT, 90, 0);
    assert(received == 1 && last_size == 4096 && !memcmp(last_payload, nal_record, 4096));
    cleanup(); z = (z_stream){0}; assert(deflateInit(&z, 6) == Z_OK);
    send_record(nal_record, sizeof(nal_record), 1, CFW_MESSAGE_RESET_CONTEXT, 91, &z);
    assert(received == 1 && last_size == 4096 && !memcmp(last_payload, nal_record, 4096));
    deflateEnd(&z); cleanup();
    assert(cfw_receive_packet(1, body, 24) == 17);
    assert(cfw_message_bridge_received(9, body, 24, 0) == 19 && stock_calls == 2);
    puts("CRC, persistent inflater/refusal/reset, MTU23 routed replies and stock fallthrough PASS");
}
