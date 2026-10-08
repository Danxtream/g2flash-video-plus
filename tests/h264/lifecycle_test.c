/* SPDX-License-Identifier: GPL-3.0-only */
#include "../../patches/video/lifecycle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "lifecycle check %d\n", __LINE__); exit(1); } } while (0)

static uint32_t start(video_lifecycle *s) {
    uint32_t token = video_lifecycle_claim(s); CHECK(token);
    CHECK(!video_lifecycle_claim(s));
    CHECK(video_lifecycle_publish(s, token));
    CHECK(video_lifecycle_ready(s, token));
    CHECK(video_lifecycle_state(s) == VIDEO_READY);
    return token;
}
static void stop(video_lifecycle *s, uint32_t token) {
    video_lifecycle_stop(s);
    CHECK(video_lifecycle_cancelled(s, token));
    CHECK(!video_lifecycle_can_reclaim(s));
    CHECK(!video_lifecycle_can_terminate(s));
    video_lifecycle_park(s, token, 0);
    CHECK(video_lifecycle_can_terminate(s));
    video_lifecycle_terminated(s);
}
static void generations(void) {
    video_lifecycle s = {0}; CHECK(video_lifecycle_state(&s) == VIDEO_IDLE);
    for (uint32_t n = 0; n < 100; ++n) {
        uint32_t token = start(&s); stop(&s, token);
        CHECK(!video_lifecycle_ready(&s, token));
        CHECK(video_lifecycle_reclaimed(&s));
        CHECK(!video_lifecycle_publish(&s, token));
        CHECK(!video_lifecycle_unpin(&s, token, 0));
        video_lifecycle_stop(&s); CHECK(s.state == VIDEO_IDLE);
        CHECK(!video_lifecycle_reclaimed(&s));
    }
    s.generation = UINT32_MAX; CHECK(!video_lifecycle_claim(&s));
    CHECK(s.state == VIDEO_QUARANTINED);
}
static void startup(void) {
    for (int worker = 0; worker < 2; ++worker) {
        video_lifecycle s = {0}; uint32_t token = video_lifecycle_claim(&s);
        video_lifecycle_stop(&s);
        CHECK(!video_lifecycle_publish(&s, token));
        CHECK(!video_lifecycle_can_reclaim(&s));
        CHECK(!video_lifecycle_claim(&s));
        video_lifecycle_abort_start(&s, token, worker);
        if (worker) {
            CHECK(!video_lifecycle_can_terminate(&s));
            video_lifecycle_park(&s, token, 0);
            video_lifecycle_terminated(&s);
        }
        CHECK(video_lifecycle_reclaimed(&s));
    }
}
static void faults(void) {
    /* Constructor and later decode failures both require the same last signal. */
    for (int ready = 0; ready < 2; ++ready) {
        video_lifecycle s = {0}; uint32_t token = video_lifecycle_claim(&s);
        CHECK(video_lifecycle_publish(&s, token));
        if (ready) CHECK(video_lifecycle_ready(&s, token));
        video_lifecycle_park(&s, token, 7);
        CHECK(video_lifecycle_state(&s) == VIDEO_STOPPING && s.fault == 7);
        CHECK(s.generation != token && !video_lifecycle_ready(&s, token));
        CHECK(!video_lifecycle_can_reclaim(&s));
        video_lifecycle_terminated(&s); CHECK(video_lifecycle_reclaimed(&s));
    }
}
static void quarantine(void) {
    for (int parked = 0; parked < 2; ++parked) {
        video_lifecycle s = {0}; uint32_t token = start(&s);
        video_lifecycle_stop(&s);
        if (parked) video_lifecycle_park(&s, token, 0);
        /* Timeout or failed termination must never authorize free/restart. */
        video_lifecycle_quarantine(&s);
        video_lifecycle_terminated(&s);
        CHECK(!video_lifecycle_can_reclaim(&s));
        CHECK(!video_lifecycle_reclaimed(&s) && !video_lifecycle_claim(&s));
    }
}
static void users(void) {
    video_lifecycle s = {0}; uint32_t token = start(&s);
    CHECK(video_lifecycle_pin(&s, token, 0));
    CHECK(video_lifecycle_pin(&s, token, 1));
    stop(&s, token); CHECK(!video_lifecycle_can_reclaim(&s));
    CHECK(!video_lifecycle_pin(&s, token, 0));
    CHECK(!video_lifecycle_unpin(&s, token + 1, 1));
    CHECK(video_lifecycle_unpin(&s, token, 0));
    CHECK(!video_lifecycle_can_reclaim(&s));
    CHECK(video_lifecycle_unpin(&s, token, 1));
    CHECK(!video_lifecycle_unpin(&s, token, 1));
    CHECK(video_lifecycle_reclaimed(&s));
    uint32_t next = start(&s); CHECK(next != token);
    video_lifecycle_park(&s, token, 8); CHECK(!s.parked && !s.fault);
    CHECK(!video_lifecycle_ready(&s, token));
    stop(&s, next); CHECK(video_lifecycle_reclaimed(&s));
}
int main(int argc, char **argv) {
    CHECK(argc == 2);
    if (!strcmp(argv[1], "generations")) generations();
    else if (!strcmp(argv[1], "startup")) startup();
    else if (!strcmp(argv[1], "faults")) faults();
    else if (!strcmp(argv[1], "quarantine")) quarantine();
    else if (!strcmp(argv[1], "users")) users();
    else CHECK(0);
    puts("session lifecycle PASS"); return 0;
}
