#include <stdint.h>
#include "memory.h"
#include "cfw_context.h"
#include "malloc.h"
#include "protobuf.h"

/*
 * Microphone-control + multi-channel routing CFW extension for the G2
 * (SybilSight "glasses -> microphones").
 *
 * TOPOLOGY. Each temple is its own Apollo510 and its own BLE endpoint, and each
 * temple carries a PAIR of microphones (front + rear along the temple). So the
 * system is two independent 2-mic arrays: the phone connects to Left and Right
 * separately and receives at most 2 channels from each. There is no shared audio
 * hardware between temples, so each temple captures, encodes, and streams its own
 * pair on its own clock and its own link. The phone reassembles the four channels
 * and does array processing (beamforming / noise isolation / direction detection,
 * fused with the compass + IMU heading it already receives) itself.
 *
 * STOCK PIPELINE (openCFW recovery of the original g2_2.2.6.10 image; every
 * callable entry below was re-matched against g2_2.3.0.24 by its complete
 * normalized function body). The behavior is pinned by the manifests in
 * evenRealities-openCFW/g2/tools/manifests/g2-service-audio-*.tsv,
 * g2-service-algo-*.tsv, and g2-production-mic-*.tsv, with the narrative in
 * g2/docs/research/g2-service-audio-recovery.md, g2-service-algo-recovery.md,
 * and g2-production-mic-recovery.md):
 *   service_audio.c          0x00599288...             two PCM app slots + LC3
 *   service_algo_process     0x005b1580                per-frame SSR + TDOA angle
 *   production mic init      0x005aef32...0x005A8E6E codec/PDM, mono/stereo
 *   drv_pdm_production.c      follows service_audio    Ambiq PDM capture driver
 * The stock two-channel capture already exists (codec front end, stereo callback,
 * source slot 0), and service_algo_process already returns a signed
 * TDOA angle + SSR per frame -- the bearing a beamformer wants. What stock lacks,
 * and this file adds, is (a) a control plane so the phone can choose front end /
 * channels / rate / codec / bitrate, and (b) a routing path that forwards BOTH
 * channels (instead of the stock mono average) plus the angle and a timestamp, so
 * the phone can beamform.
 *
 * CONTRACT. Rides the already-wired sid-0x09 settings hooks (settings_send_wrapper
 * / settings_decode_wrapper), so NO new binary patch offsets are introduced -- the
 * injected blob just grows and gen_patches.py recomputes sizes/checksums.
 *
 *   field 103 (RX)  ['M','C', ver=1, op, <op payload>]
 *     op 1 CONFIGURE  [src, chanMask, codec, fmt, rateLo, rateHi, brLo, brHi, flags]
 *          src      0 = codec DMIC/I2S front end, 1 = Ambiq PDM mics
 *          chanMask bit0/bit1 = enable this temple's front/rear mic
 *          codec    0 = LC3 encoded, 1 = raw PCM passthrough
 *          fmt      0 = 16-bit, 1 = 24-bit, 2 = 32-bit PCM sample width
 *          rate     LE16, sample rate in units of 100 Hz  (160 = 16 kHz; clamp 80..480)
 *          br       LE16, LC3 target bitrate in units of 100 bps (0 = default; <=5000)
 *          flags    bit0 MIC_FLAG_BEAMFORM  append the SSR/TDOA angle to each frame
 *                   bit1 MIC_FLAG_ARM_HW    bring up capture + streaming (GATED)
 *          Arming also starts a fail-open 90 s streaming lease (below).
 *     op 2 QUERY      push the live config now as a field-104 notify (and it is
 *                     also appended to every sid-0x09 settings READ response)
 *     op 3 STOP       tear down the CFW capture session, restore stock
 *     op 4 RENEW      renew the streaming lease without touching the config
 *
 *   field 104 (TX)  ['M','C', ver=1, active, src, chanMask, codec, fmt,
 *                    rateLo, rateHi, brLo, brHi, flags, hwArmed, sideId,
 *                    framesLo..framesHi(32), effRateLo, effRateHi]
 *                    + diagnostics extension (19 bytes, see mic_status_body):
 *                    tapCalls u32, tapLastBytes u16, tapLastSlot, skipIdle u16,
 *                    skipLease u16, skipAlloc u16, regRet s8, unregRet s8,
 *                    notifyRet s8, slotTable, leaseRemainingS u16
 *     `rate`/`br` echo the REQUESTED values; `effRate` is what capture actually
 *     runs at (see EFFECTIVE vs REQUESTED below). `sideId`: 1 = right temple,
 *     2 = left temple. Older parsers read only the first 21 bytes.
 *
 * The phone sends an IDENTICAL CONFIGURE to both temples for a consistent array;
 * each temple answers field 104 on its own link so SybilSight can confirm they
 * match before enabling its radar-style beam view.
 *
 * MULTI-CHANNEL STREAM FRAME (glasses -> phone, via the stock streaming-notify
 * BLE facade), fixed 21-byte header:
 *   [0]  'S'   [1] 'M'   [2] ver=1
 *   [3]  flags       config MIC_FLAG_* bits, plus bit7 = payload truncated
 *   [4]  seqLo  [5] seqHi        (low 16 bits of the frame counter)
 *   [6..9]  tick     u32 LE, this temple's 1 ms OS tick at packetization
 *   [10] nCh         channels in the payload (1 or 2)
 *   [11..12] rateDiv u16 LE, EFFECTIVE sample rate in units of 100 Hz
 *   [13] fmt         PCM width code as configured (0=16/1=24/2=32-bit)
 *   [14] codec       what the payload ACTUALLY is (0 = LC3, 1 = raw PCM)
 *   [15..16] angle   s16 LE, on-device TDOA angle (degrees; 0 if not computed)
 *   [17..18] ssr     s16 LE, on-device SSR ratio (0 if not computed)
 *   [19..20] payLen  u16 LE, payload bytes that follow
 * `tick` gives coarse host-side L/R alignment; `angle`/`ssr` are this temple's
 * own-pair estimate (only computed when MIC_FLAG_BEAMFORM is set AND the frame
 * is 2-channel 16-bit -- the recovered algo object expects interleaved stereo
 * 16-bit input). CHANNEL LAYOUT: the payload is the slot-0 dispatch buffer
 * verbatim, which is INTERLEAVED stereo s16 (ch0, ch1, ch0, ch1, ...): the stock
 * empty-slot fallback hands that same buffer to service_algo_process, whose
 * preprocessing (2.3.0.24 0x5b0f56) reads ch0 = pcm[4i], ch1 = pcm[4i+2] for
 * 800 frames (3200 bytes, 50 ms at 16 kHz). Which physical mic is ch0 is not yet
 * confirmed on hardware. A PDM session forwards the slot-1 dispatch buffer
 * instead, whose layout has not been checked.
 *
 * EFFECTIVE vs REQUESTED. The recovered init entries take no rate/width/bitrate
 * arguments -- stock capture runs the LC3 voice pipeline's fixed 16 kHz. The
 * requested rate/bitrate are therefore stored and echoed (so the UI round-trips
 * user intent) but capture runs at MIC_RATE_DEFAULT until the codec/PDM
 * reconfiguration seams are recovered; stream frames carry the EFFECTIVE rate,
 * which is the one the host DSP must trust. Likewise a requested codec=LC3 keeps
 * streaming raw PCM (frame byte 14 says so) until the on-device per-channel
 * LC3 encode path (SVC_Lc3EncodeMono) has a validated ABI: raw
 * frames always carry the true multi-channel samples the beamformer needs, so
 * "best quality" is the default rather than a failure mode.
 *
 * STREAMING LEASE (fail-open, same paradigm as the wake + framebuffer leases).
 * An armed session is only kept alive while the phone renews it: CONFIGURE and
 * RENEW both push the deadline MIC_LEASE_MS out and (re)arm a one-shot osTimer
 * watchdog. If the phone disappears, mic_pcm_tap stops emitting immediately at
 * the deadline (cheap signed tick compare) and the watchdog tears the capture
 * hardware down from the RTOS timer thread. Mode-11 session cleanup
 * (cfw_cleanup_session) also tears the session down, so a departing custom app
 * cannot leave the mics running.
 *
 * SAFETY / STATUS. A CONFIGURE without MIC_FLAG_ARM_HW only stores and advertises
 * the configuration -- it touches no audio hardware and cannot fault. The capture
 * + streaming path (mic_session_start / mic_pcm_tap / mic_session_stop) is
 * compiled in but runs ONLY when the phone sets MIC_FLAG_ARM_HW, and every
 * firmware entry it uses is address-pinned. The PCM slot register/unregister,
 * tap callback and service_algo_process ABIs are confirmed by disassembly; the
 * codec-init channel selector and the streaming-notify sender args are still
 * recovered behaviourally, not to register level. Those seams
 * MUST be confirmed on sacrificial hardware before a phone build ships with
 * ARM_HW enabled. Self-contained: no external symbols, no writable globals;
 * state lives in the customCfwContext singleton, guarded by magic + bounds.
 */

/* --- Recovered stock audio entry points (Thumb bit set for blx via const ptr).
 * ABI-INFERRED where noted; every use is gated behind MIC_FLAG_ARM_HW. --- */
typedef void (*mic_sel_fn)(uint32_t selector);
typedef void (*mic_void_fn)(void);
/* SVC_PcmAppRegister(owner_id, slot, callback) — one callback per PCM source
 * slot; SVC_PcmAppProcessData (2.3.0.24 0x599780) dispatches to it as
 * cb(slot, pcm, bytes) INSTEAD of the stock mono-average fallback. Confirmed by
 * disassembly (2.3.0.24): register rejects slot >= 2 and stores {owner, slot, cb};
 * re-registering an occupied slot overwrites it. Unregister(owner_id, slot)
 * returns 0 when it clears the slot or the slot is already empty, and -1 on an
 * owner mismatch. */
typedef int  (*pcm_register_fn)(uint32_t owner_id, uint32_t slot, void *cb);
typedef int  (*pcm_unregister_fn)(uint32_t owner_id, uint32_t slot);
/* service_algo_process(interleaved2ch16, bytes, &ssr, &angle) — "preprocesses
 * one frame and returns SSR and angle results through two shorts". Four
 * arguments, confirmed from the stock fallback call site (2.3.0.24 0x599812);
 * it rejects bytes > 3200 or not a multiple of 4. */
typedef void (*algo_process_fn)(const void *pcm, uint32_t bytes, int16_t *ssr, int16_t *angle);
/* Thread_MsgStreamingNotifyByBle @ 0x0047f17c — the facade the stock fallback
 * path forwards its completed LC3 packet through ("transport-one subtype-one
 * wrapper"). ABI inferred as (buf, len). */
typedef int  (*audio_notify_fn)(const void *buf, uint32_t len);
typedef uint32_t (*lens_side_fn2)(void);

#define FW_CODEC_MIC_INIT    ((mic_sel_fn)0x005aef33U)      /* production_codec_mic_func_init  */
#define FW_CODEC_MIC_DEINIT  ((mic_void_fn)0x005aefe3U)     /* production_codec_mic_func_deinit */
#define FW_PDM_MIC_INIT      ((mic_sel_fn)0x005af049U)      /* production_pdm_mic_func_init    */
#define FW_PDM_MIC_DEINIT    ((mic_void_fn)0x005af09fU)     /* production_pdm_mic_func_deinit  */
#define FW_PCM_REGISTER      ((pcm_register_fn)0x00599501U) /* SVC_PcmAppRegister   */
#define FW_PCM_UNREGISTER    ((pcm_unregister_fn)0x00599659U)/* SVC_PcmAppUnregister */
#define FW_ALGO_PROCESS      ((algo_process_fn)0x005b1581U) /* service_algo_process */
#define FW_AUDIO_NOTIFY      ((audio_notify_fn)0x0047f17dU) /* streaming notify     (ABI inferred) */
#define FW_MIC_SIDE          ((lens_side_fn2)0x00465d4dU)   /* 1 = right temple, 2 = left temple */
/* Future validation-gate seam, unused until its ABI is confirmed: on-device LC3
 * (SVC_Lc3EncodeMono, "encodes one or more mono or interleaved PCM
 * frames through liblc3") would let codec=LC3 honor mic_bitrate_100. */

/* wire contract */
#define MIC_CONTROL_FIELD    103u
#define MIC_STATUS_FIELD     104u
#define MIC_PROTO_VERSION    1u
#define MIC_OP_CONFIGURE     1u
#define MIC_OP_QUERY         2u
#define MIC_OP_STOP          3u
#define MIC_OP_RENEW         4u

#define MIC_SRC_CODEC        0u
#define MIC_SRC_PDM          1u
#define MIC_CODEC_LC3        0u
#define MIC_CODEC_RAW        1u
#define MIC_FLAG_BEAMFORM    0x01u
#define MIC_FLAG_ARM_HW      0x02u
#define MIC_STREAM_TRUNC     0x80u  /* stream-frame flags bit: payload was truncated */

#define MIC_RATE_MIN_100HZ   80u    /* 8 kHz  */
#define MIC_RATE_MAX_100HZ   480u   /* 48 kHz */
#define MIC_RATE_DEFAULT     160u   /* 16 kHz — the stock LC3 capture rate */
#define MIC_BR_MAX_100BPS    5000u  /* <= 500 kbps */
#define MIC_LEASE_MS         90000u /* same fail-open cadence as the wake/fb leases */

/* Stream-frame constants. */
#define MIC_STREAM_MAGIC0    'S'
#define MIC_STREAM_MAGIC1    'M'
#define MIC_STREAM_HDR_BYTES 21u
/* A stereo dispatch is 3200 B (800 interleaved frames), so this cap keeps the
 * first 400 frames (25 ms) and flags the frame truncated. Interleaving keeps the
 * truncated payload a valid stereo buffer. */
#define MIC_STREAM_MAX_PAY   1600u
/* PCM app slots, one per front end (2.3.0.24): the codec capture dispatches
 * into slot 0 (0x5596fe), whose empty-slot fallback is the stock mono-average
 * LC3 path; PDM capture dispatches into slot 1 (0x55973a). The production
 * inits register their stock callback in the same slot, and the tap takes it
 * over. */
#define MIC_PCM_SLOT_CODEC   0u
#define MIC_PCM_SLOT_PDM     1u
/* Owner id the stock production mic inits register under (2.3.0.24 0x5aef7e
 * codec: SVC_PcmAppRegister(0x10B, 0, cb); 0x5af086 PDM: (0x10B, 1, cb)). The
 * tap reuses it so the stock deinits' SVC_PcmAppUnregister(0x10B, slot) still
 * matches: on an owner mismatch the codec deinit returns early and leaves the
 * hardware running. */
#define MIC_PCM_OWNER_ID     0x10Bu
/* SVC_PcmAppRegister's table (2.3.0.24, literal at 0x599d78): two 12-byte
 * entries {owner u32, slot u8 @4, callback @8}, indexed by slot. Read only, for
 * diagnostics. */
#define FW_PCM_APP_TABLE     ((volatile const uint8_t *)0x20076d4cU)
#define FW_PCM_APP_ENTRY     12u
/* Diagnostic result byte for a firmware call that has not happened yet. */
#define MIC_RET_NONE         127
#define MIC_STATUS_BASE_BYTES 21u
#define MIC_STATUS_BYTES     40u

static uint32_t mic_pcm_slot(const customCfwContext *ctx) {
    return ctx->mic_source == MIC_SRC_PDM ? MIC_PCM_SLOT_PDM : MIC_PCM_SLOT_CODEC;
}

static uint8_t mic_popcount2(uint8_t mask) {
    return (uint8_t)((mask & 1u) + ((mask >> 1) & 1u));
}

/* What capture actually runs at: the recovered init entries take no rate
 * argument, so an armed session is the stock fixed 16 kHz pipeline regardless
 * of the requested rate (see EFFECTIVE vs REQUESTED above). */
static uint16_t mic_effective_rate(const customCfwContext *ctx) {
    (void)ctx;
    return (uint16_t)MIC_RATE_DEFAULT;
}

/* Channels a live session actually delivers. The recovered PDM init registers
 * only the SINGLE-channel callback (capture mode 1), so PDM is mono in stock;
 * only the codec front end has the stereo callback. A rear-only mask (0x2)
 * still needs stereo capture — the phone drops the front channel. */
static uint8_t mic_effective_channels(const customCfwContext *ctx) {
    if (ctx->mic_source == MIC_SRC_PDM) return 1u;
    return ctx->mic_chan_mask == 0x1u ? 1u : 2u;
}

static int mic_lease_live(const customCfwContext *ctx) {
    return ctx->mic_lease_deadline != 0 &&
           (int32_t)(ctx->mic_lease_deadline - FW_MS_TICK) > 0;
}

/* Clamp a firmware int result into a diagnostic byte (MIC_RET_NONE is reserved). */
static int8_t mic_ret8(int ret) {
    if (ret >= MIC_RET_NONE) return MIC_RET_NONE - 1;
    if (ret < -128) return -128;
    return (int8_t)ret;
}

__attribute__((used, noinline)) void mic_pcm_tap(uint32_t source, const void *pcm, uint32_t bytes);

/* Two bits per PCM slot from the stock app table: 0 empty, 1 another
 * callback, 2 our tap (slot 0 in bits 0-1, slot 1 in bits 2-3). Non-static
 * and noinline so it is emitted here, beside mic_pcm_tap: the Thumb MOVW/MOVT
 * relocation for &mic_pcm_tap has a signed 16-bit addend (see
 * cfw_create_buzzer_timer). */
__attribute__((noinline)) uint8_t mic_slot_table_state(void) {
    uint8_t state = 0;
    for (uint32_t slot = 0; slot < 2u; slot++) {
        uint32_t cb = *(volatile const uint32_t *)(FW_PCM_APP_TABLE + slot * FW_PCM_APP_ENTRY + 8u);
        uint8_t value = cb == 0 ? 0u : cb == (uint32_t)(uintptr_t)&mic_pcm_tap ? 2u : 1u;
        state |= (uint8_t)(value << (slot * 2u));
    }
    return state;
}

/* Whole seconds left on the streaming lease (0 = none or lapsed). */
static uint32_t mic_lease_remaining_s(const customCfwContext *ctx) {
    if (!mic_lease_live(ctx)) return 0;
    return (ctx->mic_lease_deadline - FW_MS_TICK + 999u) / 1000u;
}

/* ---- capture + streaming (GATED behind MIC_FLAG_ARM_HW) -------------------- */

/* The registered PCM tap. Receives this temple's capture dispatch, packs a
 * stream frame with the timestamp and on-device angle/SSR, and hands it to the
 * streaming-notify facade. Runs on the audio service's thread. The dispatcher
 * calls it as (slot, pcm, bytes). Nonstatic + noinline: registered by address
 * via `&`. */
__attribute__((used, noinline)) void mic_pcm_tap(uint32_t source, const void *pcm, uint32_t bytes) {
    customCfwContext *ctx = peekCustomCfwContext();
    if (!ctx) return;
    ctx->mic_tap_calls++;
    ctx->mic_tap_last_bytes = (uint16_t)(bytes > 0xffffu ? 0xffffu : bytes);
    ctx->mic_tap_last_slot = (uint8_t)source;
    if (!ctx->mic_active || !ctx->mic_hw_armed || pcm == 0 || bytes == 0) {
        ctx->mic_skip_idle++;
        return;
    }
    /* Fail-open: the phone stopped renewing — stop emitting immediately. The
     * watchdog timer does the actual hardware teardown from the timer thread
     * (deinit from inside the capture callback would be re-entrant). */
    if (!mic_lease_live(ctx)) {
        ctx->mic_skip_lease++;
        return;
    }

    uint8_t nch = mic_effective_channels(ctx);
    int16_t ssr = 0, angle = 0;
    /* The recovered algo object splits 800 interleaved stereo 16-bit frames;
     * feeding it anything else would return garbage bearings. */
    if ((ctx->mic_flags & MIC_FLAG_BEAMFORM) && nch == 2u && ctx->mic_format == 0u)
        FW_ALGO_PROCESS(pcm, bytes, &ssr, &angle);

    uint8_t flags = ctx->mic_flags;
    uint32_t pay = bytes;
    if (pay > MIC_STREAM_MAX_PAY) { pay = MIC_STREAM_MAX_PAY; flags |= MIC_STREAM_TRUNC; }

    uint8_t *f = (uint8_t *)cfw_malloc(MIC_STREAM_HDR_BYTES + pay);
    if (!f) {
        ctx->mic_skip_alloc++;
        return;
    }
    uint32_t tick = FW_MS_TICK;
    uint16_t seq = (uint16_t)ctx->mic_frames;
    uint16_t rate = mic_effective_rate(ctx);
    f[0] = MIC_STREAM_MAGIC0; f[1] = MIC_STREAM_MAGIC1; f[2] = MIC_PROTO_VERSION;
    f[3] = flags;
    f[4] = (uint8_t)seq; f[5] = (uint8_t)(seq >> 8);
    f[6] = (uint8_t)tick; f[7] = (uint8_t)(tick >> 8);
    f[8] = (uint8_t)(tick >> 16); f[9] = (uint8_t)(tick >> 24);
    f[10] = nch;
    f[11] = (uint8_t)rate; f[12] = (uint8_t)(rate >> 8);
    f[13] = ctx->mic_format;
    f[14] = MIC_CODEC_RAW;   /* payload is raw PCM until the LC3 seam is validated */
    f[15] = (uint8_t)angle; f[16] = (uint8_t)((uint16_t)angle >> 8);
    f[17] = (uint8_t)ssr;   f[18] = (uint8_t)((uint16_t)ssr >> 8);
    f[19] = (uint8_t)pay;   f[20] = (uint8_t)(pay >> 8);
    memcpy(f + MIC_STREAM_HDR_BYTES, pcm, pay);

    ctx->mic_notify_ret = mic_ret8(FW_AUDIO_NOTIFY(f, MIC_STREAM_HDR_BYTES + pay));
    ctx->mic_frames++;
    FW_FREE(f);
}

/* Bring up this temple's selected front end + channel pair and register the tap.
 * Releases the other front end first so both are never held at once. The codec
 * init's one-byte argument "selects the single or stereo callback"; the
 * boolean encoding (0 = single, 1 = stereo) is inferred — validation-gate item. */
static void mic_session_start(customCfwContext *ctx) {
    if (!(ctx->mic_flags & MIC_FLAG_ARM_HW)) return;
    if (ctx->mic_source == MIC_SRC_PDM) {
        FW_CODEC_MIC_DEINIT();
        FW_PDM_MIC_INIT(0);                 /* PDM init has only the single callback */
    } else {
        FW_PDM_MIC_DEINIT();
        FW_CODEC_MIC_INIT(mic_effective_channels(ctx) == 2u ? 1u : 0u);
    }
    ctx->mic_reg_ret = mic_ret8(FW_PCM_REGISTER(MIC_PCM_OWNER_ID, mic_pcm_slot(ctx), (void *)&mic_pcm_tap));
    ctx->mic_hw_armed = 1;
}

static void mic_session_stop(customCfwContext *ctx) {
    if (!ctx->mic_hw_armed) return;
    ctx->mic_hw_armed = 0;                  /* silence the tap before teardown */
    ctx->mic_unreg_ret = mic_ret8(FW_PCM_UNREGISTER(MIC_PCM_OWNER_ID, mic_pcm_slot(ctx)));
    FW_CODEC_MIC_DEINIT();
    FW_PDM_MIC_DEINIT();
}

/* Watchdog callback, on the RTOS timer thread (the same context stock deinit
 * paths run in). Tears down an armed session whose lease lapsed; if the lease
 * was renewed since arming, re-arms itself for the remaining time. */
void mic_watchdog_tick(void *arg) {
    customCfwContext *ctx = (customCfwContext *)arg;
    if (!ctx || ctx->magic != CFW_CTX_MAGIC || !ctx->mic_hw_armed) return;
    if (mic_lease_live(ctx)) {
        uint32_t left = ctx->mic_lease_deadline - FW_MS_TICK;
        if (ctx->mic_watchdog_timer) FW_TIMER_START(ctx->mic_watchdog_timer, left ? left : 1u);
        return;
    }
    ctx->mic_lease_deadline = 0;
    mic_session_stop(ctx);
}

static void mic_lease_renew(customCfwContext *ctx) {
    ctx->mic_lease_deadline = FW_MS_TICK + MIC_LEASE_MS;
    if (!ctx->mic_hw_armed) return;
    if (ctx->mic_watchdog_timer == 0)
        ctx->mic_watchdog_timer = FW_TIMER_NEW((void *)&mic_watchdog_tick, 0, ctx, 0);
    if (ctx->mic_watchdog_timer) {
        FW_TIMER_STOP(ctx->mic_watchdog_timer);
        FW_TIMER_START(ctx->mic_watchdog_timer, MIC_LEASE_MS);
    }
}

/* Full teardown for STOP / mode-11 session cleanup (cfw_cleanup_session).
 * Idempotent, same retry contract as the other cleanup-owned timers: a timer
 * whose delete fails stays in the context so a later cleanup can retry. */
static void mic_cleanup_session(void) {
    customCfwContext *ctx = peekCustomCfwContext();
    if (!ctx) return;
    ctx->mic_lease_deadline = 0;
    mic_session_stop(ctx);
    ctx->mic_active = 0;
    if (ctx->mic_watchdog_timer) {
        FW_TIMER_STOP(ctx->mic_watchdog_timer);
        if (FW_TIMER_DELETE(ctx->mic_watchdog_timer) == 0) ctx->mic_watchdog_timer = 0;
    }
}

/* ---- control plane (always active; touches no hardware unless armed) ------- */

static unsigned mic_status_body(customCfwContext *ctx, unsigned char *body);

/* Push the live config to the phone right now as a standalone sid-0x09 notify
 * (G2SettingPackage{commandId=3, magic=0, field104}), the same wire shape as
 * the Faceclaw wake event. Unlike that event this is NOT right-arm gated: the
 * phone configures each temple over its own link and each answers for itself.
 * The buffer lives in the singleton because the stock sender's copy/queue
 * lifetime is intentionally treated as opaque. */
static void mic_send_status_notify(customCfwContext *ctx) {
    unsigned char *p = ctx->mic_notify_buf;
    p[0] = 0x08; p[1] = 0x03;                       /* field 1: commandId=3 */
    p[2] = 0x10; p[3] = 0x00;                       /* field 2: magic=0 */
    p[4] = 0xC2; p[5] = 0x06;                       /* field 104, wire type 2: tag 834 */
    unsigned n = mic_status_body(ctx, p + 7);
    p[6] = (unsigned char)n;
    ((send_fn)FW_SEND)(1, 9, p, 7 + n);
}

/* Parse a field-103 record. Called from faceclaw_scan_settings_control for each
 * sid-0x09 settings WRITE, before the stock decoder runs. */
void mic_apply_control(const uint8_t *data, uint32_t len) {
    if (len < 4u || data[0] != 'M' || data[1] != 'C' ||
        data[2] != MIC_PROTO_VERSION) return;
    customCfwContext *ctx = getCustomCfwContext();
    if (!ctx) return;
    uint8_t op = data[3];

    if (op == MIC_OP_CONFIGURE) {
        if (len < 4u + 9u) return;
        const uint8_t *c = data + 4;

        /* Reconfiguring a live session: quiesce the old one first. */
        mic_session_stop(ctx);

        ctx->mic_source    = c[0] > MIC_SRC_PDM ? MIC_SRC_CODEC : c[0];
        ctx->mic_chan_mask = c[1] & 0x03u;
        if (ctx->mic_chan_mask == 0) ctx->mic_chan_mask = 0x01u;
        ctx->mic_codec  = c[2] > MIC_CODEC_RAW ? MIC_CODEC_LC3 : c[2];
        ctx->mic_format = c[3] > 2u ? 0u : c[3];

        uint32_t rate = (uint32_t)c[4] | ((uint32_t)c[5] << 8);
        if (rate < MIC_RATE_MIN_100HZ) rate = MIC_RATE_MIN_100HZ;
        if (rate > MIC_RATE_MAX_100HZ) rate = MIC_RATE_MAX_100HZ;
        ctx->mic_rate_hz_div = (uint16_t)rate;

        uint32_t br = (uint32_t)c[6] | ((uint32_t)c[7] << 8);
        if (br > MIC_BR_MAX_100BPS) br = MIC_BR_MAX_100BPS;
        ctx->mic_bitrate_100 = (uint16_t)br;

        ctx->mic_flags    = c[8] & (MIC_FLAG_BEAMFORM | MIC_FLAG_ARM_HW);
        ctx->mic_channels = mic_popcount2(ctx->mic_chan_mask);
        ctx->mic_active = 1;
        ctx->mic_frames = 0;
        ctx->mic_tap_calls = 0;
        ctx->mic_tap_last_bytes = 0;
        ctx->mic_tap_last_slot = 0;
        ctx->mic_skip_idle = ctx->mic_skip_lease = ctx->mic_skip_alloc = 0;
        ctx->mic_notify_ret = MIC_RET_NONE;
        mic_session_start(ctx);              /* no-op unless MIC_FLAG_ARM_HW set */
        mic_lease_renew(ctx);
        mic_send_status_notify(ctx);         /* confirm the applied config */
    } else if (op == MIC_OP_QUERY) {
        mic_send_status_notify(ctx);
    } else if (op == MIC_OP_STOP) {
        mic_cleanup_session();
        mic_send_status_notify(ctx);
    } else if (op == MIC_OP_RENEW) {
        if (ctx->mic_active) mic_lease_renew(ctx);
    }
}

/* Serialize the field-104 status body (MIC_STATUS_BYTES; see the contract above). */
static unsigned mic_status_body(customCfwContext *ctx, unsigned char *body) {
    unsigned n = 0;
    body[n++] = 'M'; body[n++] = 'C'; body[n++] = (unsigned char)MIC_PROTO_VERSION;
    body[n++] = ctx->mic_active;
    body[n++] = ctx->mic_source;
    body[n++] = ctx->mic_chan_mask;
    body[n++] = ctx->mic_codec;
    body[n++] = ctx->mic_format;
    body[n++] = (unsigned char)ctx->mic_rate_hz_div;
    body[n++] = (unsigned char)(ctx->mic_rate_hz_div >> 8);
    body[n++] = (unsigned char)ctx->mic_bitrate_100;
    body[n++] = (unsigned char)(ctx->mic_bitrate_100 >> 8);
    body[n++] = ctx->mic_flags;
    body[n++] = ctx->mic_hw_armed;
    body[n++] = (unsigned char)FW_MIC_SIDE();     /* 1 = right, 2 = left */
    body[n++] = (unsigned char)ctx->mic_frames;
    body[n++] = (unsigned char)(ctx->mic_frames >> 8);
    body[n++] = (unsigned char)(ctx->mic_frames >> 16);
    body[n++] = (unsigned char)(ctx->mic_frames >> 24);
    uint16_t eff = mic_effective_rate(ctx);
    body[n++] = (unsigned char)eff;
    body[n++] = (unsigned char)(eff >> 8);
    /* Diagnostics extension. */
    body[n++] = (unsigned char)ctx->mic_tap_calls;
    body[n++] = (unsigned char)(ctx->mic_tap_calls >> 8);
    body[n++] = (unsigned char)(ctx->mic_tap_calls >> 16);
    body[n++] = (unsigned char)(ctx->mic_tap_calls >> 24);
    body[n++] = (unsigned char)ctx->mic_tap_last_bytes;
    body[n++] = (unsigned char)(ctx->mic_tap_last_bytes >> 8);
    body[n++] = ctx->mic_tap_last_slot;
    body[n++] = (unsigned char)ctx->mic_skip_idle;
    body[n++] = (unsigned char)(ctx->mic_skip_idle >> 8);
    body[n++] = (unsigned char)ctx->mic_skip_lease;
    body[n++] = (unsigned char)(ctx->mic_skip_lease >> 8);
    body[n++] = (unsigned char)ctx->mic_skip_alloc;
    body[n++] = (unsigned char)(ctx->mic_skip_alloc >> 8);
    body[n++] = (unsigned char)ctx->mic_reg_ret;
    body[n++] = (unsigned char)ctx->mic_unreg_ret;
    body[n++] = (unsigned char)ctx->mic_notify_ret;
    body[n++] = mic_slot_table_state();
    uint32_t lease_s = mic_lease_remaining_s(ctx);
    body[n++] = (unsigned char)lease_s;
    body[n++] = (unsigned char)(lease_s >> 8);
    return n;
}

/* Append the live mic configuration as settings field 104 (wire type 2) to the
 * sid-0x09 settings READ response. A missing context reports an all-zero
 * (inactive) body of the same shape so the phone parser never needs a special
 * case. Returns the new message length, or the original length if it will not
 * fit in the settings response buffer. */
unsigned mic_append_status(unsigned char *buf, unsigned len, unsigned capacity) {
    customCfwContext *ctx = peekCustomCfwContext();
    unsigned char body[MIC_STATUS_BYTES];
    unsigned n;
    if (ctx) {
        n = mic_status_body(ctx, body);
    } else {
        n = 0;
        body[n++] = 'M'; body[n++] = 'C'; body[n++] = (unsigned char)MIC_PROTO_VERSION;
        while (n < MIC_STATUS_BASE_BYTES) body[n++] = 0;
    }
    return pb_append_bytes_field(buf, len, capacity, 104u, body, n);
}

/* Debug overlay line (debug.c): this temple's mic session state and tap
 * counters, so each lens shows its own even though only one temple can send
 * status to the phone. Example:
 *   mic R arm pdm reg 0 tbl -T lease 87s tap 120 3200B@1 skip 0/0/0 fr 120 ntf 0 unreg - */
/* Out-of-line wrappers: the string helpers inline at every call site, which
 * made the ~25-call overlay formatter below over 13 KB. */
static __attribute__((noinline)) void mic_cat(char *out, const char *text, uint32_t maxlen) {
    strlcat(out, text, maxlen);
}

static __attribute__((noinline)) void mic_dec(char *out, uint32_t value, uint32_t maxlen) {
    u_to_dec(out, value, maxlen);
}

static __attribute__((noinline)) void mic_append_signed(char *out, int value, uint32_t maxlen) {
    if (value == MIC_RET_NONE) { mic_cat(out, "-", maxlen); return; }
    if (value < 0) { mic_cat(out, "-", maxlen); value = -value; }
    mic_dec(out, (uint32_t)value, maxlen);
}

/* Noinline: debug.c's overlay is inlined into display_copy_hook, and inlining
 * this formatter there roughly doubled that function's size. */
static __attribute__((noinline)) void mic_append_overlay(char *line, uint32_t maxlen) {
    customCfwContext *ctx = peekCustomCfwContext();
    uint32_t side = FW_MIC_SIDE();
    mic_cat(line, side == 1u ? "mic R " : side == 2u ? "mic L " : "mic ? ", maxlen);
    if (!ctx) { mic_cat(line, "no ctx", maxlen); return; }
    mic_cat(line, ctx->mic_hw_armed ? "arm " : ctx->mic_active ? "cfg " : "off ", maxlen);
    mic_cat(line, ctx->mic_source == MIC_SRC_PDM ? "pdm" : "codec", maxlen);
    mic_cat(line, " reg ", maxlen);
    mic_append_signed(line, ctx->mic_reg_ret, maxlen);
    mic_cat(line, " tbl ", maxlen);
    uint8_t table = mic_slot_table_state();
    for (uint32_t slot = 0; slot < 2u; slot++) {
        uint8_t value = (uint8_t)((table >> (slot * 2u)) & 3u);
        mic_cat(line, value == 2u ? "T" : value == 1u ? "S" : "-", maxlen);
    }
    mic_cat(line, " lease ", maxlen);
    mic_dec(line, mic_lease_remaining_s(ctx), maxlen);
    mic_cat(line, "s tap ", maxlen);
    mic_dec(line, ctx->mic_tap_calls, maxlen);
    mic_cat(line, " ", maxlen);
    mic_dec(line, ctx->mic_tap_last_bytes, maxlen);
    mic_cat(line, "B@", maxlen);
    mic_dec(line, ctx->mic_tap_last_slot, maxlen);
    mic_cat(line, " skip ", maxlen);
    mic_dec(line, ctx->mic_skip_idle, maxlen);
    mic_cat(line, "/", maxlen);
    mic_dec(line, ctx->mic_skip_lease, maxlen);
    mic_cat(line, "/", maxlen);
    mic_dec(line, ctx->mic_skip_alloc, maxlen);
    mic_cat(line, " fr ", maxlen);
    mic_dec(line, ctx->mic_frames, maxlen);
    mic_cat(line, " ntf ", maxlen);
    mic_append_signed(line, ctx->mic_notify_ret, maxlen);
    mic_cat(line, " unreg ", maxlen);
    mic_append_signed(line, ctx->mic_unreg_ret, maxlen);
}
