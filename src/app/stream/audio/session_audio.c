#include "../avsync_probe.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>

#include <opus_multistream.h>
#include <SDL.h>

#include "ss4s.h"
#include "stream/connection/session_connection.h"
#include "stream/session_priv.h"
#include "app_launch.h"
#include "coop_audio.h"
#include "session_audio.h"
#include "logging.h"
#include "platform/webos/vendor_diag.h"

#define SAMPLES_PER_FRAME  240
/* Moonlight RtspConnection.c owns the fixed Opus rate; co-op session.c requests stereo. */
#define COOP_AUDIO_PREPARE_RATE 48000

static session_t *session = NULL;
static SS4S_Player *player = NULL;
static OpusMSDecoder *decoder = NULL;
static unsigned char *buffer = NULL;
static int frame_size = 0, unit_size = 0, aud_sample_rate = 48000;

/* CO-OP: THE MIX NEEDS ITS OWN CLOCK WHEN OUR HOST IS SILENT.
 *
 * coop_audio_mix() -- which also DRAINS the other PC's PCM -- only ran inside aud_feed(), i.e. only when OUR host
 * sent audio. Hosts send nothing while nothing plays (measured 2026-10-02: first audio packet 39.3 s in, exactly
 * when a tone started), so a quiet primary game silenced the other player completely: the secondary logged
 * "forwarded=68 dropped=840" and its tone never reached the speakers. While our host has been quiet for
 * COOP_PUMP_IDLE_MS, this thread feeds the mix on a real-time clock -- the other PC over silence, same gains and
 * listen mode. The moment our host's audio returns, aud_feed is the clock again. feed_lock keeps the two from
 * mixing or feeding at the same time. */
#define COOP_PUMP_IDLE_MS 40
static SDL_Thread *pump_thread = NULL;
static SDL_atomic_t pump_run;
static SDL_mutex *feed_lock = NULL;
static SDL_atomic_t last_self_feed_ms;
static int pump_sample_rate = 48000;
static unsigned pump_frames_fed = 0;
static bool device_paced = false;
static bool output_prepared = false;
static uint64_t primary_packets, primary_decoded_frames, primary_plc_calls, primary_plc_frames;

static void primary_decode_log(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const uint64_t now_us = (uint64_t) ts.tv_sec * 1000000u + (uint64_t) ts.tv_nsec / 1000u;
    commons_log_info("Session", "[COOP-AUD.decode] role=primary time_us=%llu packet_frames=%d packets=%llu "
                     "plc_calls=%llu plc_frames=%llu decoded_frames=%llu",
                     (unsigned long long) now_us, frame_size, (unsigned long long) primary_packets,
                     (unsigned long long) primary_plc_calls, (unsigned long long) primary_plc_frames,
                     (unsigned long long) primary_decoded_frames);
}

static int coop_pump_main(void *unused) {
    (void) unused;
    int16_t *silence = calloc((size_t) frame_size, (size_t) unit_size);
    if (silence == NULL) {
        return 0;
    }
    const double frame_ms = 1000.0 * frame_size / pump_sample_rate;
    double next_ms = -1.0;
    while (SDL_AtomicGet(&pump_run)) {
        const Uint32 now = SDL_GetTicks();
        if ((Sint32) (now - (Uint32) SDL_AtomicGet(&last_self_feed_ms)) < COOP_PUMP_IDLE_MS) {
            next_ms = -1.0;          /* our host's audio is the clock */
            SDL_Delay(2);
            continue;
        }
        if (next_ms < 0) {
            next_ms = now;
        }
        while ((double) now >= next_ms && SDL_AtomicGet(&pump_run)) {
            memset(silence, 0, (size_t) frame_size * unit_size);
            SDL_LockMutex(feed_lock);
            coop_audio_mix(silence, frame_size);
            SS4S_PlayerAudioFeed(player, (unsigned char *) silence, unit_size * frame_size);
            SDL_UnlockMutex(feed_lock);
            pump_frames_fed++;
            next_ms += frame_ms;
        }
        SDL_Delay(1);
    }
    free(silence);
    return 0;
}

static void coop_pump_start(int sample_rate) {
    if (pump_thread != NULL || !coop_audio_coord_active() || decoder == NULL) {
        return;
    }
    pump_sample_rate = sample_rate > 0 ? sample_rate : 48000;
    pump_frames_fed = 0;
    if (feed_lock == NULL) {
        feed_lock = SDL_CreateMutex();
    }
    SDL_AtomicSet(&last_self_feed_ms, (int) SDL_GetTicks());
    SDL_AtomicSet(&pump_run, 1);
    pump_thread = SDL_CreateThread(coop_pump_main, "coop-audio-pump", NULL);
}

static void coop_pump_stop(void) {
    if (pump_thread == NULL) {
        return;
    }
    SDL_AtomicSet(&pump_run, 0);
    SDL_WaitThread(pump_thread, NULL);
    pump_thread = NULL;
    commons_log_info("Session", "Co-op audio clock: %u frames mixed while our host was silent", pump_frames_fed);
    commons_log_info("Session", "[COOP-AUD.decode] role=pump pump_pcm_frames=%llu",
                     (unsigned long long) pump_frames_fed * (unsigned) frame_size);
}
/* Desktop (Windows, Linux): no usable sound output, so this session streams the picture without sound. */
static bool audio_unavailable = false;

AUDIO_INFO audio_stream_info;

static size_t opus_head_serialize(const OPUS_MULTISTREAM_CONFIGURATION *config, unsigned char *data);
static void aud_cleanup(void);

bool session_audio_prepare_coop_output(session_t *context) {
    if (!context || !context->player || !app_launch_coop_requested() ||
        context->config.stream.audioConfiguration != AUDIO_CONFIGURATION_STEREO ||
        (!SS4S_AudioHasPull() && !SS4S_AudioHasClock())) return false;
    SS4S_AudioInfo info = {.codec=SS4S_AUDIO_PCM_S16LE, .numOfChannels=2,
        .sampleRate=COOP_AUDIO_PREPARE_RATE, .samplesPerFrame=SAMPLES_PER_FRAME,
        .appName="Eclipse", .streamName="Streaming"};
    if (SS4S_AudioHasPull()) {
        /* Allocate gate, ingress and diagnostics before the first device callback. */
        coop_audio_coord_start(COOP_AUDIO_PREPARE_RATE, 2);
        if (!coop_audio_output_open_pull(context->player, &info)) {
            coop_audio_coord_stop();
            return false;
        }
        session=context; player=context->player;
        device_paced=output_prepared=true;
        commons_log_info("Session", "[COOP-AUD.pull] ready before host connection and secondary launch");
        return true;
    }
    if (SS4S_PlayerAudioOpen(context->player, &info) != SS4S_AUDIO_OPEN_OK) return false;
    SS4S_AudioClock clock;
    if (!SS4S_PlayerAudioGetClock(context->player, &clock) || clock.sample_rate != COOP_AUDIO_PREPARE_RATE) {
        SS4S_PlayerAudioClose(context->player);
        return false;
    }
    coop_audio_coord_start(COOP_AUDIO_PREPARE_RATE, 2);
    if (!coop_audio_output_start(context->player)) {
        coop_audio_coord_stop();
        SS4S_PlayerAudioClose(context->player);
        return false;
    }
    session = context;
    player = context->player;
    device_paced = output_prepared = true;
    commons_log_info("Session", "[COOP-AUD.output] ready before host connection and secondary launch");
    return true;
}

void session_audio_finish_output(SS4S_Player *target) {
    /* Failed video/connection setup can bypass AudioCallbacks.cleanup entirely.
     * The output worker must still join before its player can be destroyed. */
    if (target && player == target) aud_cleanup();
}


/* Co-op split-screen SPIKE #3 — two concurrent PCM outs.
 *
 * The design's preferred audio path is "each process plays its own session audio
 * and webOS mixes them", which is only true if the TV grants a SECOND audio
 * output while one is already playing. This opens a second player's audio with
 * the primary's exact config and reports the result. It runs only after the
 * PRIMARY audio has opened, so a grant here really is concurrent — probing
 * before the primary opened would prove nothing.
 *
 * Gated on /tmp/coop-probe-audio, separate from the video sentinel so the two
 * spikes can be run independently (they contend different resources). No-op in
 * normal use. Nothing is fed, so no sound is produced either way.
 */
static void coop_second_audio_probe(const SS4S_AudioInfo *primary) {
    if (access("/tmp/coop-probe-audio", F_OK) != 0) {
        return;
    }
    FILE *log = fopen(AURORA_DIAG_LOG, "a");
    if (log == NULL) {
        return;
    }
    SS4S_Player *p2 = SS4S_PlayerOpen();
    if (p2 == NULL) {
        fprintf(log, "[COOP-PROBE] audio: FAIL SS4S_PlayerOpen() == NULL (no 2nd player)\n");
        fclose(log);
        return;
    }
    /* Do NOT open video on the second player here. Trying to take a decoder during
     * audio init, while the primary pipeline is still coming up, failed (-1) and
     * broke the primary stream with "decoder reported error". It is also
     * unnecessary: the audio open below is granted without it, so a loaded video
     * pipeline is not a precondition for a second PCM out. */
    SS4S_AudioInfo info2 = *primary;
    info2.streamName = "Streaming2";
    SS4S_AudioOpenResult r = SS4S_PlayerAudioOpen(p2, &info2);
    /* Tag every line with a per-run id. Rotating /tmp/aurora-diag.log while the
     * app holds it open does NOT give a clean file, so without this it is not
     * possible to tell which run a line came from. */
    fprintf(log,
            "[COOP-PROBE run=%ld] audio: 2nd SS4S_PlayerAudioOpen result=%d codec=%s %dch/%dHz => %s\n",
            (long) time(NULL), (int) r, SS4S_AudioCodecName(info2.codec), info2.numOfChannels, info2.sampleRate,
            r == SS4S_AUDIO_OPEN_OK ? "SECOND PCM OUT GRANTED (system-mixer audio path viable)"
                                    : "SECOND PCM OUT DENIED (fall back to mixing in the coordinator)");
    fflush(log);
    /* Release immediately — holding a second decoder while the primary pipeline
     * is still coming up is what broke the stream on the first run. */
    SS4S_PlayerAudioClose(p2);
    SS4S_PlayerClose(p2);
    fprintf(log, "[COOP-PROBE] audio: cleaned up 2nd player\n");
    fclose(log);
}

static int aud_init(int audioConfiguration, const POPUS_MULTISTREAM_CONFIGURATION opusConfig, void *context,
                    int arFlags) {
    (void) audioConfiguration;
    (void) arFlags;
    memset(&audio_stream_info, 0, sizeof(audio_stream_info));
    session = context;
    player = session->player;
    if (output_prepared && (opusConfig->sampleRate != COOP_AUDIO_PREPARE_RATE || opusConfig->channelCount != 2)) {
        commons_log_error("Session", "Prepared co-op PCM format differs from negotiated audio");
        return SS4S_AUDIO_OPEN_UNSUPPORTED_CODEC;
    }
    SS4S_AudioCodec codec = SS4S_AUDIO_PCM_S16LE;
    size_t codecDataLen = 0;
    SS4S_AudioInfo info = {
            .numOfChannels = opusConfig->channelCount,
            .appName = "Eclipse",
            .streamName = "Streaming",
            .sampleRate = opusConfig->sampleRate,
            .samplesPerFrame = SAMPLES_PER_FRAME,
    };
    /* CO-OP MUST DECODE OPUS HERE, NEVER PASS IT THROUGH.
     *
     * Handing the compressed stream to the player is the cheaper path and is what
     * the TV prefers for 6 channels, but there is nothing to mix a second host
     * into: the samples only exist inside the pipeline. Co-op therefore takes the
     * libopus path unconditionally, which costs one stereo Opus decode and gives
     * coop_audio_mix() PCM to work with. (For stereo the TV asks for PCM anyway,
     * so on this panel this changes nothing in practice.) */
    const bool coop_audio = app_launch_coop_requested();
    if (!coop_audio && session->audio_cap.codecs & SS4S_AUDIO_OPUS &&
        SS4S_GetAudioPreferredCodecs(&info) & SS4S_AUDIO_OPUS) {
        codec = SS4S_AUDIO_OPUS;
        decoder = NULL;
        buffer = calloc(1024, sizeof(unsigned char));
        if (buffer == NULL) {
            commons_log_error("Session", "Audio init: failed to allocate codec data buffer");
            return -1;
        }
        codecDataLen = opus_head_serialize(opusConfig, buffer);
    } else {
        int rc;
        decoder = opus_multistream_decoder_create(opusConfig->sampleRate, opusConfig->channelCount, opusConfig->streams,
                                                  opusConfig->coupledStreams, opusConfig->mapping, &rc);
        if (rc != 0) {
            return rc;
        }
        frame_size = opusConfig->samplesPerFrame;
        primary_packets = primary_decoded_frames = primary_plc_calls = primary_plc_frames = 0;
        primary_decode_log();
        unit_size = (int) (opusConfig->channelCount * sizeof(int16_t));
        aud_sample_rate = opusConfig->sampleRate;
        buffer = calloc(unit_size, frame_size);
        if (buffer == NULL) {
            commons_log_error("Session", "Audio init: failed to allocate decode buffer");
            opus_multistream_decoder_destroy(decoder);
            decoder = NULL;
            return -1;
        }
    }
    audio_stream_info.format = SS4S_AudioCodecName(codec);
    switch (audioConfiguration) {
        case AUDIO_CONFIGURATION_STEREO:
            audio_stream_info.channels = "Stereo";
            break;
        case AUDIO_CONFIGURATION_51_SURROUND:
            audio_stream_info.channels = "5.1ch";
            break;
        case AUDIO_CONFIGURATION_71_SURROUND:
            audio_stream_info.channels = "7.1ch";
            break;
        default:
            audio_stream_info.channels = "Unknown";
            break;
    }
    info.codec = codec;
    info.codecData = buffer;
    info.codecDataLen = codecDataLen;
    SS4S_AudioOpenResult primary_result = SS4S_AUDIO_OPEN_OK;
    if (!output_prepared) {
        vendor_diag_hang_arm("SS4S_PlayerAudioOpen");
        primary_result = SS4S_PlayerAudioOpen(player, &info);
        vendor_diag_hang_disarm(primary_result == SS4S_AUDIO_OPEN_OK ? "OK" : "FAILED");
    }
    if (primary_result == SS4S_AUDIO_OPEN_OK) {
        coop_second_audio_probe(&info);
        /* Only now: the coordinator's socket exists exactly as long as there is a
         * player to mix into, so the secondary can never connect to a session that
         * cannot play what it sends. */
        if (coop_audio && !output_prepared) {
            coop_audio_coord_start(opusConfig->sampleRate, opusConfig->channelCount);
            device_paced = coop_audio_output_start(player);
            if (!device_paced) coop_pump_start(opusConfig->sampleRate);
        }
    }
#if defined(OS_WINDOWS) || (defined(OS_LINUX) && !defined(TARGET_WEBOS) && !defined(TARGET_STEAMLINK)) || \
    defined(TARGET_ANDROID)
    else {
        /* A desktop can lack a usable sound output: a Linux one a working sound server (plain
         * ALSA, PipeWire without its PulseAudio layer, a session without XDG_RUNTIME_DIR), a
         * Windows one a default playback device (outputs unplugged or disabled, a headless PC).
         * Android, like the desktops, lets the user choose an output (Settings -> Audio) that can be
         * switched off when a stream starts, Bluetooth headphones for one.
         * The audio module's check only proves its library loads. A failure here aborts the
         * WHOLE connection ("stage 10 (audio stream establishment)"), so stream the picture
         * without sound instead. */
        commons_log_warn("Session", "No usable audio output (open result %d): streaming without sound",
                         (int) primary_result);
        audio_unavailable = true;
        return 0;
    }
#endif
    return primary_result;
}

static void aud_cleanup() {
    coop_pump_stop();          /* before the mixer and the player it feeds go away */
    if (decoder != NULL) primary_decode_log();
    coop_audio_coord_stop();
    device_paced = output_prepared = false;
    if (player != NULL) {
        if (!audio_unavailable) {
            SS4S_PlayerAudioClose(player);
        }
        player = NULL;
    }
    audio_unavailable = false;
    if (decoder != NULL) {
        opus_multistream_decoder_destroy(decoder);
        decoder = NULL;
    }
    if (buffer != NULL) {
        free(buffer);
        buffer = NULL;
    }
    session = NULL;
}

static void aud_feed(char *sampleData, int sampleLength) {
    if (audio_unavailable) {
        return;
    }
    if (decoder != NULL) {
        int decode_len = opus_multistream_decode(decoder, (unsigned char *) sampleData, sampleLength,
                                                 (opus_int16 *) buffer, frame_size, 0);
        primary_packets++;
        if (sampleData == NULL) primary_plc_calls++;
        if (decode_len <= 0) {
            if (decode_len < 0) {
                commons_log_warn("Session", "Opus audio decode failed: %s (%d)",
                                 opus_strerror(decode_len), decode_len);
            }
            return;
        }
        struct timespec decoded_at;
        clock_gettime(CLOCK_MONOTONIC, &decoded_at);
        const uint64_t ready_us = (uint64_t)decoded_at.tv_sec * 1000000u + (uint64_t)decoded_at.tv_nsec / 1000u;
        primary_decoded_frames += decode_len;
        if (sampleData == NULL) primary_plc_frames += decode_len;
        /* The callback-owned path publishes its ledger from a service thread.
         * Keep decode accounting here, with its complete report at cleanup. */
        if (!device_paced && primary_packets % 1000u == 0u) primary_decode_log();
        if (device_paced) {
            if (!coop_audio_output_submit((int16_t *)buffer, (unsigned)decode_len,
                    primary_decoded_frames - (unsigned)decode_len, ready_us, sampleData == NULL))
                commons_log_warn("Session", "[COOP-AUD.output] primary PCM queue full: %d frames rejected", decode_len);
            return;
        }
        /* The other host's game, summed in place. A no-op outside co-op, and when
         * the peer is absent or late it mixes silence -- our own audio is never
         * held up for it. */
        if (pump_thread != NULL) {
            SDL_AtomicSet(&last_self_feed_ms, (int) SDL_GetTicks());
            SDL_LockMutex(feed_lock);
        }
        coop_audio_mix((int16_t *) buffer, decode_len);
        avsync_probe_audio((const int16_t *) buffer, decode_len, unit_size / (int) sizeof(int16_t), aud_sample_rate);
        SS4S_PlayerAudioFeed(player, buffer, unit_size * decode_len);
        if (pump_thread != NULL) {
            SDL_UnlockMutex(feed_lock);
        }
    } else {
        SS4S_PlayerAudioFeed(player, (unsigned char *) sampleData, sampleLength);
    }
}

static size_t opus_head_serialize(const OPUS_MULTISTREAM_CONFIGURATION *config, unsigned char *data) {
    unsigned char *ptr = data;
    // 1. Magic Signature:
    ptr = memcpy(ptr, "OpusHead", 8) + 8;
    // 2. Version (8 bits, unsigned):
    *ptr++ = 1;
    // 3. Output Channel Count 'C' (8 bits, unsigned):
    *ptr++ = config->channelCount;
    // 4. Pre-skip (16 bits, unsigned, little-endian):
    uint16_t preskip = SDL_SwapLE16(0);
    ptr = memcpy(ptr, &preskip, 2) + 2;
    // 5. Input Sample Rate (32 bits, unsigned, little-endian):
    uint32_t sampleRate = SDL_SwapLE32(config->sampleRate);
    ptr = memcpy(ptr, &sampleRate, 4) + 4;
    // 6. Output Gain (16 bits, signed, little-endian):
    int16_t outputGain = SDL_SwapLE16(0);
    ptr = memcpy(ptr, &outputGain, 2) + 2;
    // 7. Channel Mapping Family (8 bits, unsigned):
    *ptr++ = 1;
    // 8.1. Stream Count 'N' (8 bits, unsigned):
    *ptr++ = config->streams;
    // 8.2. Coupled Stream Count 'M' (8 bits, unsigned):
    *ptr++ = config->coupledStreams;
    // 8.3. Channel Mapping (8*C bits):
    ptr = memcpy(ptr, config->mapping, config->channelCount) + config->channelCount;
    return ptr - data;
}

AUDIO_RENDERER_CALLBACKS ss4s_aud_callbacks = {
        .init = aud_init,
        .cleanup = aud_cleanup,
        .decodeAndPlaySample = aud_feed,
        .capabilities = CAPABILITY_DIRECT_SUBMIT,
};
