/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2015-2017 Iwan Timmer
 *
 * Moonlight is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Moonlight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Moonlight; if not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "xml.h"

#include <Limelight.h>

#include <stdbool.h>

#define MIN_SUPPORTED_GFE_VERSION 3
#define MAX_SUPPORTED_GFE_VERSION 7

typedef struct _SERVER_DATA {
    const char *uuid;
    const char *mac;
    const char *hostname;
    const char *gpuType;
    /** HttpsPort */
    unsigned short httpsPort;
    /** ExternalPort */
    unsigned short extPort;
    bool paired;
    bool supports4K;
    bool supportsHdr;
    bool unsupported;
    bool isGfe;
    /** Somebody is looking at this host's LOCK SCREEN right now (Apollo `SessionLocked`).
     *
     * A locked-but-signed-in PC answers serverinfo perfectly well, so `paired` and a
     * successful status query have never meant the host is USABLE. Only an Apollo that
     * reports the field can set this; anything else -- an older Apollo, GFE, or the
     * unpaired HTTP probe, which does not carry it -- leaves it FALSE. False is the safe
     * direction: it means "do not try to log in", and the cost of being wrong is a lock
     * screen the user dismisses by hand, not a password typed into a running game. */
    bool sessionLocked;
    /** Did the host ACTUALLY ANSWER the question above, or is `sessionLocked` just the
     *  default? False for stock Sunshine, for an Apollo built before the SessionLocked
     *  patch (PC2's, dated 2026-08-01, is one), and for the unpaired HTTP probe. A caller
     *  that must act on the answer needs to know it got one -- silence from an old host
     *  and a confident "nobody is at the lock screen" are not the same thing. */
    bool sessionLockedKnown;
    /** WHICH ENCODER THIS HOST WILL ACTUALLY PRODUCE A BITSTREAM WITH -- "nvidia",
     *  "amd", "intel", "software", or "" when the host did not answer.
     *
     *  THE ENCODER, NOT THE GPU, AND THE DIFFERENCE IS NOT PEDANTRY. A box can hold two
     *  adapters (an iGPU plus a card), Apollo's `adapter_name` can pin capture to one of
     *  them, and co-op splices BITSTREAMS: what decides whether two panes can share one
     *  SPS/PPS is the encoder that emitted them. Apollo derives this from the encoder its
     *  own probe chose, so a host with an idle second GPU cannot mislead us.
     *
     *  Only our Apollo emits it, and only over HTTPS -- see encoderVendorKnown. */
    const char *encoderVendor;
    /** Did the host ANSWER the question above, or is `encoderVendor` just empty? Same
     *  split as sessionLockedKnown, for the same reason: stock Sunshine, an older Apollo
     *  and the unpaired HTTP probe are all silent, and silence is not "no GPU". A caller
     *  that must choose a co-op codec needs to know it is guessing. */
    bool encoderVendorKnown;
    /** What this host can be ASKED to do for cross-vendor co-op. Bit 0: this build has the
     *  H.264 Main-profile control at all (stock Apollo/Sunshine has no profile selector --
     *  verified against master's config.cpp, which carries only nvenc_h264_cavlc). Bit 1:
     *  that control is currently ON.
     *
     *  WHY A CLIENT CARES. NVENC's HIGH profile sets transform_8x8_mode_flag=1 in the PPS
     *  and AMF omits the extension entirely; spliced under one PPS the picture
     *  desynchronises (Apollo nvenc_config.h, measured 2026-08-30). The client cannot fix
     *  that from its side -- the slice data itself differs -- so a mixed-vendor pairing
     *  whose NVIDIA host reports bit 0 clear cannot produce a correct picture at all. */
    int coopProfileCaps;
    /** Umbra's feature bits (serverinfo UmbraCaps; bit 0 = can pin a client to its wired card). KNOWN is kept
     *  apart from the value because Umbra always sends it -- even 0 -- and that alone identifies the build. */
    int umbraCaps;
    bool umbraCapsKnown;
    int currentGame;
    /** ServerCodecModeSupport AS THE HOST REPORTED IT. Not serverInfo.serverCodecModeSupport: the session
     *  overwrites that with the client's own codec set before connecting (session.c). For support logs. */
    int codecModeSupport;
    int serverMajorVersion;
    const char *gsVersion;
    PDISPLAY_MODE modes;
    SERVER_INFORMATION serverInfo;
} SERVER_DATA, *PSERVER_DATA;

typedef struct GS_CLIENT_T *GS_CLIENT;

GS_CLIENT gs_new(const char *keydir);

int gs_conf_init(const char *keydir);

void gs_destroy(GS_CLIENT hnd);

void gs_set_timeout(GS_CLIENT hnd, int timeout_secs);
/** Experimental session request. Requires an explicit host acknowledgement. */
void gs_set_gpu_hevc(GS_CLIENT hnd, bool enabled);

/** @p coop: this launch is one pane of a co-op session, so /launch and /resume carry umbraCoop -- 1 when
 *  @p tuning (an Umbra host, UmbraCaps bit 2, then applies its co-op encoder tuning to THIS session only),
 *  0 when not. Not co-op: nothing is sent. Stock hosts ignore the argument. */
void gs_set_coop(GS_CLIENT hnd, bool coop, bool tuning);

/** Ask an Umbra host (serverinfo UmbraCaps bit 4) to switch its physical monitor(s) off while this client
 *  streams: /launch and /resume carry umbraMonitorOff=1. Nothing is sent when false. */
void gs_set_host_monitor_off(GS_CLIENT hnd, bool off);

/** Ask the host to keep this client's stream on its WIRED card for the session (Umbra, serverinfo UmbraCaps
 *  bit 0): @p wired_mac is that card's MAC, @p own_macs / @p own_ips all of this device's MACs and IPv4
 *  addresses, comma-separated. NULL or empty @p wired_mac clears the request. Sent on /launch and /resume. */
void gs_set_wired_pin(GS_CLIENT hnd, const char *wired_mac, const char *own_macs, const char *own_ips);

int gs_get_status(GS_CLIENT hnd, PSERVER_DATA server, const char *address, uint16_t port, bool unsupported);

int gs_start_app(GS_CLIENT hnd, PSERVER_DATA server, PSTREAM_CONFIGURATION config, int appId, bool is_gfe, bool sops,
                 bool localaudio, int gamepad_mask, const char *surround_params);

int gs_applist(GS_CLIENT hnd, const SERVER_DATA *server, PAPP_LIST *app_list);

int gs_unpair(GS_CLIENT hnd, PSERVER_DATA server);

int gs_pair(GS_CLIENT hnd, PSERVER_DATA server, const char *pin);

int gs_quit_app(GS_CLIENT hnd, PSERVER_DATA server);

/** Umbra: the tail of the host's own log (GET /umbralog, paired client). On GS_OK *text is a malloc'd,
 *  NUL-terminated copy (caller frees). max_bytes is clamped by the host to 4 KB..1 MB. */
int gs_get_host_log(GS_CLIENT hnd, const SERVER_DATA *server, int max_bytes, char **text);

int gs_download_cover(GS_CLIENT hnd, const SERVER_DATA *server, int appId, const char *path);