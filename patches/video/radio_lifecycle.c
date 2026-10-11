/* Finite host-link PHY ownership. SPDX-License-Identifier: GPL-3.0-only */
#include "radio.h"
#include "platform.h"

#pragma clang section text=".text.video"

#ifndef VIDEO_RADIO_OWNER_VALID
#define VIDEO_RADIO_OWNER_VALID(ctx, generation) \
    (video_control_generation_valid((ctx), (generation)) && video_lease_valid(ctx))
#endif

static int video_radio_due(uint32_t deadline) {
    return deadline && (int32_t)(VIDEO_RADIO_TICK - deadline) >= 0;
}
static video_radio_sample video_radio_current(const video_radio_link *l) {
    return (video_radio_sample){l->tick, l->tx, l->rx, l->valid, 0};
}
static int video_radio_original(const video_radio_link *l) {
    return l->valid && l->before.valid && l->tx == l->before.tx && l->rx == l->before.rx;
}
static int video_radio_wait_command(customCfwContext *ctx, uint8_t id, uint32_t epoch,
                                    uint8_t operation, uint8_t tx, uint8_t rx,
                                    uint32_t permission, uint32_t generation,
                                    uint32_t deadline) {
    video_radio_state *s = &ctx->video_radio;
    uint32_t serial = 0;
    while (!video_radio_due(deadline)) {
        if (!VIDEO_RADIO_OWNER_VALID(ctx, generation)) return 0;
        VIDEO_RADIO_LOCK();
        video_radio_link *l = &s->links[id - 1];
        int live = l->connected && l->bound && l->epoch == epoch;
        int done = serial && s->completed == serial;
        int good = done && !s->result;
        VIDEO_RADIO_UNLOCK();
        if (!live || done) return live && good;
        if (!serial && video_radio_request(id, epoch, operation, tx, rx, permission))
            serial = s->serial;
        VIDEO_OS_DELAY(VIDEO_RADIO_RETRY_MS);
    }
    return 0;
}

/* Only task-context lifecycle service mutates owners. The fixed records and
 * epochs survive worker teardown; no owner contains a borrowed task pointer. */
int video_radio_generation_lost(uint32_t generation) {
    customCfwContext *ctx = VIDEO_RADIO_CONTEXT();
    if (!ctx || !generation) return 0;
    int lost = 0;
    VIDEO_RADIO_LOCK();
    for (uint32_t i = 0; i < VIDEO_RADIO_OWNERS; ++i) {
        const video_radio_owner *o = &ctx->video_radio.owners[i];
        if (o->generation != generation || !o->connection) continue;
        const video_radio_link *l = &ctx->video_radio.links[o->connection - 1];
        if (!l->connected || !l->bound || l->epoch != o->epoch) lost = 1;
    }
    VIDEO_RADIO_UNLOCK();
    return lost;
}

/* Terminal failure is explicit. In-flight SET is not assumed drained merely
 * because a deadline expired; quarantine prevents it crossing a new owner. */
static void video_radio_uncertain(video_radio_link *l) {
    l->uncertain = 1; l->phase = VIDEO_RADIO_UNCERTAIN;
    l->permission = 0; l->after.valid = 0;
}

void video_radio_step(void) {
    customCfwContext *ctx = VIDEO_RADIO_CONTEXT();
    if (!ctx) return;
    video_radio_state *s = &ctx->video_radio;
    for (uint32_t i = 0; i < VIDEO_RADIO_LINKS; ++i) {
        uint8_t operation = 0, tx = 0, rx = 0;
        uint32_t epoch = 0, permission = 0;
        VIDEO_RADIO_LOCK();
        video_radio_link *l = &s->links[i];
        if (!l->connected) {
            l->permission = l->owners = l->changed = l->uncertain = 0;
            l->phase = VIDEO_RADIO_OBSERVED; l->before.valid = l->after.valid = 0;
        } else if (!l->owners && l->phase >= VIDEO_RADIO_RESTORE_WAIT &&
                   l->phase <= VIDEO_RADIO_RESTORE_READ) {
            int pending = s->pending && s->connection == i + 1 && s->epoch == l->epoch;
            if (video_radio_due(l->deadline)) video_radio_uncertain(l);
            else if (!pending) {
                if (l->phase == VIDEO_RADIO_RESTORE_READ &&
                    s->completed == l->restore_serial && !s->result && video_radio_original(l)) {
                    l->after = video_radio_current(l); l->phase = VIDEO_RADIO_RESTORED;
                    l->permission = 0; l->changed = 0;
                } else if (!video_radio_due(l->retry_tick)) {
                    /* A just-queued command has not yet received a serial. */
                } else if (l->attempts >= VIDEO_RADIO_RESTORE_ATTEMPTS) video_radio_uncertain(l);
                else {
                    operation = video_radio_original(l) ? VIDEO_RADIO_READ : VIDEO_RADIO_SET;
                    tx = l->before.tx; rx = l->before.rx;
                    epoch = l->epoch; permission = l->permission;
                    if (!l->before.valid || !permission) { operation = 0; video_radio_uncertain(l); }
                }
            }
        }
        VIDEO_RADIO_UNLOCK();
        if (operation) {
            int queued = video_radio_request(i + 1, epoch, operation, tx, rx, permission);
            VIDEO_RADIO_LOCK();
            l = &s->links[i];
            if (l->epoch == epoch && !l->owners) {
                ++l->attempts; l->retry_tick = VIDEO_RADIO_TICK + VIDEO_RADIO_RETRY_MS;
                if (queued) {
                    l->restore_serial = s->serial;
                    l->phase = operation == VIDEO_RADIO_READ ? VIDEO_RADIO_RESTORE_READ : VIDEO_RADIO_RESTORE_SET;
                }
            }
            VIDEO_RADIO_UNLOCK();
        }
    }
}

void video_radio_release(uint32_t generation) {
    customCfwContext *ctx = VIDEO_RADIO_CONTEXT();
    if (!ctx || !generation) return;
    video_radio_state *s = &ctx->video_radio;
    VIDEO_RADIO_LOCK();
    for (uint32_t i = 0; i < VIDEO_RADIO_OWNERS; ++i) {
        video_radio_owner *o = &s->owners[i];
        if (o->generation != generation) continue;
        if (o->connection) {
            video_radio_link *l = &s->links[o->connection - 1];
            if (l->connected && l->epoch == o->epoch) {
                if (o->changing && l->owners) --l->owners;
                if (o->changing && !l->owners && l->permission) {
                    l->phase = VIDEO_RADIO_RESTORE_WAIT; l->attempts = 0;
                    l->deadline = VIDEO_RADIO_TICK + VIDEO_RADIO_RESTORE_MS;
                    l->retry_tick = VIDEO_RADIO_TICK ? VIDEO_RADIO_TICK : 1;
                } else if (!l->owners && !l->changed && !l->permission && !l->uncertain)
                    l->after = video_radio_current(l);
            }
        }
        *o = (video_radio_owner){0};
    }
    VIDEO_RADIO_UNLOCK();
    video_radio_step();
}

int video_radio_acquire(uint32_t generation, int off) {
    customCfwContext *ctx = VIDEO_RADIO_CONTEXT();
    if (!ctx || !generation || !VIDEO_RADIO_OWNER_VALID(ctx, generation)) return 0;
    video_radio_state *s = &ctx->video_radio;
    uint32_t owner = VIDEO_RADIO_OWNERS, id = 0, count = 0;
    VIDEO_RADIO_LOCK();
    for (uint32_t i = 0; i < VIDEO_RADIO_OWNERS; ++i) {
        if (s->owners[i].generation == generation) { VIDEO_RADIO_UNLOCK(); return 1; }
        if (!s->owners[i].generation) owner = i;
    }
    for (uint32_t i = 0; i < VIDEO_RADIO_LINKS; ++i)
        if (s->links[i].connected && s->links[i].bound) { id = i + 1; ++count; }
    if (owner == VIDEO_RADIO_OWNERS || count > 1) { VIDEO_RADIO_UNLOCK(); return 0; }
    /* UART-only recipients have no host PHY to own. Never enumerate a CCB
     * to invent a phone/PC link merely because the peer forwarded START. */
    if (!id) {
        s->owners[owner] = (video_radio_owner){.generation = generation};
        VIDEO_RADIO_UNLOCK(); return 1;
    }
    video_radio_link *l = &s->links[id - 1];
    if (!off && (l->uncertain || (l->phase >= VIDEO_RADIO_RESTORE_WAIT &&
        l->phase <= VIDEO_RADIO_RESTORE_READ))) { VIDEO_RADIO_UNLOCK(); return 0; }
    uint32_t epoch = l->epoch;
    s->owners[owner] = (video_radio_owner){.generation = generation, .epoch = epoch,
                                        .connection = id, .changing = !off};
    if (off) {
        if (!l->owners && !l->permission && !l->uncertain) {
            l->before = video_radio_current(l); l->after.valid = 0;
        }
        VIDEO_RADIO_UNLOCK(); return 1;
    }
    if (l->owners) {
        if (l->phase != VIDEO_RADIO_ACTIVE && l->phase != VIDEO_RADIO_FALLBACK) {
            s->owners[owner] = (video_radio_owner){0}; VIDEO_RADIO_UNLOCK(); return 0;
        }
        ++l->owners; VIDEO_RADIO_UNLOCK(); return 1;
    }
    ++l->owners; l->phase = VIDEO_RADIO_CAPTURE; l->after.valid = l->before.valid = 0;
    VIDEO_RADIO_UNLOCK();
    uint32_t deadline = VIDEO_RADIO_TICK + VIDEO_RADIO_ACQUIRE_MS;
    if (!video_radio_wait_command(ctx, id, epoch, VIDEO_RADIO_READ, 0, 0, 0, generation, deadline)) {
        video_radio_release(generation); return 0;
    }
    VIDEO_RADIO_LOCK();
    l = &s->links[id - 1];
    if (!l->connected || l->epoch != epoch || !l->valid || s->permission_serial == UINT32_MAX) {
        VIDEO_RADIO_UNLOCK(); video_radio_release(generation); return 0;
    }
    l->before = video_radio_current(l);
    int already = l->tx == VIDEO_RADIO_PHY_2M && l->rx == VIDEO_RADIO_PHY_2M;
    uint32_t permission = already ? 0 : ++s->permission_serial;
    l->permission = permission; l->phase = already ? VIDEO_RADIO_ACTIVE : VIDEO_RADIO_SELECT;
    l->changed = !already;
    VIDEO_RADIO_UNLOCK();
    if (already) return 1;
    int done = video_radio_wait_command(ctx, id, epoch, VIDEO_RADIO_SET,
                                        VIDEO_RADIO_PHY_2M, VIDEO_RADIO_PHY_2M,
                                        permission, generation, deadline);
    VIDEO_RADIO_LOCK();
    l = &s->links[id - 1];
    int live = l->connected && l->epoch == epoch;
    int success = done && live && l->valid && l->tx == VIDEO_RADIO_PHY_2M &&
                  l->rx == VIDEO_RADIO_PHY_2M;
    if (success) l->phase = VIDEO_RADIO_ACTIVE;
    VIDEO_RADIO_UNLOCK();
    if (success) return 1;
    /* Never continue on an unconfirmed preference or a failed negotiation.
     * A fresh read of the exact original pair is the only safe fallback. */
    if (live && !(s->pending && s->operation == VIDEO_RADIO_SET) &&
        video_radio_wait_command(ctx, id, epoch, VIDEO_RADIO_READ, 0, 0, 0,
                                 generation, deadline)) {
        VIDEO_RADIO_LOCK(); l = &s->links[id - 1];
        int fallback = l->epoch == epoch && video_radio_original(l);
        if (fallback) { l->phase = VIDEO_RADIO_FALLBACK; l->changed = 0; l->permission = 0; }
        VIDEO_RADIO_UNLOCK(); if (fallback) return 1;
    }
    video_radio_release(generation); return 0;
}

int video_radio_maintenance(void) {
    customCfwContext *ctx = VIDEO_RADIO_CONTEXT();
    if (!ctx) return 0;
    int needed = 0;
    VIDEO_RADIO_LOCK();
    for (uint32_t i = 0; i < VIDEO_RADIO_OWNERS; ++i)
        if (ctx->video_radio.owners[i].generation) needed = 1;
    for (uint32_t i = 0; i < VIDEO_RADIO_LINKS; ++i) {
        const video_radio_link *l = &ctx->video_radio.links[i];
        if (l->phase >= VIDEO_RADIO_RESTORE_WAIT && l->phase <= VIDEO_RADIO_RESTORE_READ)
            needed = 1;
    }
    VIDEO_RADIO_UNLOCK(); return needed;
}

#pragma clang section text=""
