/*
 * Co-op split-screen: AUDIO.
 *
 * The two hosts are two independent games, so there are two independent Opus
 * streams arriving at two separate processes. The TV has ONE audio output, and
 * SPIKE #3 established that a second PCM out cannot be opened alongside the
 * first (in-process it returns -1 AND kills the primary stream -- one media
 * pipeline per process, same cause as the second decoder). So the system mixer
 * is not available to us and the design's FALLBACK is the path: the secondary
 * decodes its Opus to PCM and ships it to the coordinator, which mixes it with
 * its own PCM and feeds the single existing player.
 *
 * The result is what local co-op on the 360 sounded like (Halo, MW2): one
 * shared soundstage out of the one TV, not two streams fighting for a device.
 *
 * RULES THIS PATH OBEYS
 * - The peer NEVER stalls our audio. Every socket operation is non-blocking and
 *   a missing peer mixes as silence; audio glitches are the one artefact the
 *   user cannot un-hear.
 * - Where the backend provides an output clock, one worker owns playout and
 *   both decoders only enqueue indexed PCM. The legacy callback/pump path is
 *   retained for backends whose consumption contract is not yet qualified.
 * - Every message has an atomic boundary. POSIX gets that from SOCK_SEQPACKET;
 *   Windows' AF_UNIX only has SOCK_STREAM, so coop_msg_channel adds validated
 *   framing and retains partial reads/writes without blocking either callback.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <Limelight.h>
#include <ss4s.h>

/** COOP_AUDIO_MIX / _PRIMARY_ONLY / _SECONDARY_ONLY live with the setting they
 *  serve, next to the co-op orientation. */
#include "app_settings.h"

/** Where the coordinator listens. Separate socket from the video link so a
 *  large access unit can never delay a 5 ms audio frame behind it.
 *  PER-VARIANT via -DCOOP_AUDIO_SOCKET (see coop_input.h); this literal is the fallback. */
#ifndef COOP_AUDIO_SOCKET
#define COOP_AUDIO_SOCKET "/tmp/aurora-coop-audio.sock"
#endif

/**
 * Coordinator: start listening for the secondary's PCM.
 * @p sample_rate / @p channels are OUR stream's format; the peer must match or
 * its audio is refused (mixing is sample-wise, a resampler is not warranted).
 * No-op unless this is a co-op launch. Failure is non-fatal: our own audio
 * plays unmixed.
 */
void coop_audio_coord_start(int sample_rate, int channels);

/** Coordinator: stop listening and release the socket. Safe if never started. */
void coop_audio_coord_stop(void);

/**
 * Coordinator: mix the peer into @p pcm IN PLACE, and apply the listen mode.
 * @p frames is samples PER CHANNEL. Call once per decoded audio frame, right
 * before feeding the player. Never blocks. A no-op when co-op audio is off.
 */
void coop_audio_mix(int16_t *pcm, int frames);

/** Start a single output-paced worker when the backend has a qualified clock.
 * Call after output open, before decode callbacks; stop/join before output close.
 * False leaves the existing callback path selected. */
bool coop_audio_output_start(SS4S_Player *player);
/** Preferred callback-owned PCM output. Coordinator and all callback state are
 * prepared before the device starts. Stop closes/joins this device before free. */
bool coop_audio_output_open_pull(SS4S_Player *player, const SS4S_AudioInfo *info);
void coop_audio_output_stop(void);
/** Clocked path only: append stable source-zero PCM; false means explicit rejection.
 * ready_us is same-machine monotonic decode completion; first_frame includes PLC. */
bool coop_audio_output_submit(const int16_t *pcm, unsigned frames, uint64_t first_frame,
                               uint64_t ready_us, bool plc);

/** True when this process should mix (co-op coordinator with the socket up). */
bool coop_audio_coord_active(void);

/**
 * Secondary: audio callbacks that decode Opus and ship PCM to the coordinator.
 * They open NO player and touch no media pipeline -- the same shape as the
 * secondary's video callbacks, which is the only shape that ever worked
 * alongside a live primary.
 */
extern AUDIO_RENDERER_CALLBACKS coop_secondary_aud_callbacks;

/**
 * WHEN ONE CO-OP HOST IS THIS PC, ITS SOUND STILL GOES THROUGH THE MIX (user requirement 2026-10-02: in every
 * setup co-op audio is the mix of both PCs unless the Audio setting says otherwise).
 *
 * Call in the coordinator when a co-op session starts, BEFORE either launch, with whether either host is this
 * PC. Windows only (elsewhere this PC cannot be a co-op host); a no-op otherwise.
 *
 * STREAMED (the normal case): this PC's host is asked NOT to play its sound locally, so -- like any host -- it
 * moves this PC's default output to its virtual sink ("Steam Streaming Speakers") and streams the game's
 * sound, which the mixer mixes like the other PC's. Eclipse's OWN output is pinned to the real speakers that
 * were the default a moment ago (SS4S_SetAudioDevice): following the default would put the mix onto the
 * virtual sink, i.e. into this PC's own stream -- a feedback loop.
 *
 * LOCAL (fallback, logged): no virtual sink on this PC, or the default output already IS the virtual sink.
 * Then the host would capture the very device Eclipse plays to, so this PC plays its sound itself and its
 * streamed copy is not mixed (the behaviour before 2026-10-02).
 *
 * The answer is published in the environment (COOP_LOCAL_AUDIO) so the secondary process, spawned later,
 * launches this PC's host the same way.
 */
void coop_audio_prepare_local_host(bool this_pc_is_a_host);

/** True when this PC is a co-op host whose sound is streamed and mixed (see above). Any process. */
bool coop_audio_local_streamed(void);

/** Co-op is over: give the output device back to the user's Audio setting. Safe to call any time. */
void coop_audio_release_local_host(void);

/** Diagnostics: PCM messages received / sent, and peer frames dropped. */
unsigned coop_audio_received(void);
unsigned coop_audio_sent(void);
unsigned coop_audio_dropped(void);
