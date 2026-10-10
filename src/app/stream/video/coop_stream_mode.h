/*
 * Co-op path selection uses the existing host report / saved GPU mapping.
 * NVIDIA+NVIDIA retains ordinary
 * hardware HEVC. NVIDIA+AMD and AMD+AMD request the same experimental GPU
 * encoder on BOTH hosts and feed the existing HEVC compositor. This selection
 * supersedes a saved legacy stream-mode preference for these known pairs.
 * Unknown/untested pairs retain HEVC; the H.264 co-op method is removed.
 * Runtime quality, latency and HDR presentation still require panel validation;
 * selecting an encoder does not establish those properties.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** What a host's encoder is, as the host itself reports it. UNKNOWN is the honest value
 *  for a stock Sunshine/Apollo host: it is not "no GPU", it is "nobody asked and nobody
 *  answered". */
typedef enum {
    COOP_GPU_UNKNOWN = 0,
    COOP_GPU_NVIDIA,
    COOP_GPU_AMD,
    COOP_GPU_INTEL,
    COOP_GPU_SOFTWARE,
    /** The user told us it is none of the above. A REAL ANSWER, not a synonym for UNKNOWN:
     *  it means "do not ask again", and it is treated as its own vendor -- so two hosts both
     *  marked OTHER count as matching, and OTHER paired with anything else counts as mixed. */
    COOP_GPU_OTHER,
} coop_gpu_vendor_t;

/** The user's standing preference (moonlight.ini `coop_stream_mode`) AND the resolved
 *  answer, sharing a type on purpose: AUTO is only ever an input, CORE is the only resolved mode. */
typedef enum {
    COOP_STREAM_AUTO = 0,
    COOP_STREAM_CORE = 1,     /**< HEVC compositor; the selected encoder determines capabilities. */
    COOP_STREAM_COMPAT = 2,   /**< Retired saved value; normalized to HEVC, never selected. */
} coop_stream_mode_t;

/** The user's co-op encoder choice (moonlight.ini `coop_encoder`). AUTO is the only value a fresh
 *  install has and the only one that is right for every pair; the other two are overrides. */
typedef enum {
    COOP_ENCODER_AUTO = 0,     /**< from the pair: NVIDIA+NVIDIA native, any AMD -> GPU HEVC on both */
    COOP_ENCODER_NATIVE = 1,   /**< each PC's own hardware encoder (NVENC/AMF), whatever the pair */
    COOP_ENCODER_GPU_HEVC = 2, /**< the GPU HEVC encoder on BOTH PCs, whatever the pair */
} coop_encoder_choice_t;

/** `coopProfileCaps` bits as published by Umbra in serverinfo. */
#define COOP_PROFILE_CAP_MAIN_CONTROL 0x1  /**< the build HAS the H.264 Main-profile control */
#define COOP_PROFILE_CAP_MAIN_ON      0x2  /**< ...and it is currently enabled */

/** Everything the dialog needs to both act and explain itself. */
typedef struct {
    /** Always CORE (HEVC), never AUTO. */
    coop_stream_mode_t mode;
    bool gpu_hevc;  /**< Beta encoder request on BOTH hosts. */
    /** Requested depth for the common picture. Legacy paths honor fixed-function
     * host capabilities. The programmable beta opens the requested depth on both
     * hosts directly; its capability is independent of the AMF/NVENC report. */
    bool hdr_allowed;
    /** The user's setting decided this, not the hardware. */
    bool forced;
    /** Both hosts named their encoder. False means every field below is a guess. */
    bool vendors_known;
    /** Both GPU vendors are known and differ. */
    bool mixed_vendor;
    /** A host reports something this project has never run on (Intel, software). */
    bool untested_vendor;
    /** The plan needs the GPU HEVC encoder and a host has reported it is NOT installed
     *  (UmbraCaps bit 2 without bit 3), so that host cannot open its pane. */
    bool host_support_missing;
    /** The user asked for HDR (the standing `hdr` setting). Everything below is only
     *  worth saying when this is true. */
    bool hdr_requested;
    /** HDR is off because a PARTICIPATING HOST does not report 10-bit support -- the one
     *  case the user's HDR setting should ever be overruled by the pairing. Read from
     *  each host's own ServerCodecModeSupport, which every Sunshine and Apollo reports,
     *  so this needs no fork on either machine. */
    bool hdr_blocked_by_host;
    /** HDR is off because this session is H.264 and no 10-bit H.264 format exists. Not a
     *  capability of either machine, and stated separately so the dialog can say which
     *  of the two reasons applies. */
    bool hdr_lost_to_codec;
} coop_stream_plan_t;

/** Map a serverinfo `EncoderVendor` token to the enum. NULL/empty -> UNKNOWN. */
coop_gpu_vendor_t coop_gpu_vendor_parse(const char *token);

/** Human-readable vendor, for the dialog. Never NULL. */
const char *coop_gpu_vendor_name(coop_gpu_vendor_t vendor);

/** The wire/config token for a vendor -- the exact string coop_gpu_vendor_parse() accepts, so
 *  a value written to hosts.ini reads back as itself. Never NULL. */
const char *coop_gpu_vendor_token(coop_gpu_vendor_t vendor);

/** Both entry points are HEVC-only. Normal launches enable mapped beta routing. */
/** One host's side of the question. Grouped because the plan needs three facts per
 *  machine and a six-argument call was already at the edge of readable. */
typedef struct {
    coop_gpu_vendor_t vendor;
    /** The host NAMED its encoder. False for stock Sunshine and for an unpaired probe. */
    bool vendor_known;
    /** `coopProfileCaps` as reported; 0 means "made no claim". */
    int profile_caps;
    /** This host advertises a 10-bit codec (SERVER_DATA.supportsHdr). Standard field,
     *  decoded from ServerCodecModeSupport, so it is trustworthy on ANY host -- unlike
     *  vendor, it needs no companion build. */
    bool supports_hdr;
    /** Did we get an ANSWER about the line above? False when this host has no server
     *  data yet -- never polled, asleep, or dropped. **Only a KNOWN "no" may turn the
     *  user's HDR off**; silence must not, or an HDR session would quietly become SDR
     *  because a host had not been polled when the popup opened. */
    bool supports_hdr_known;
    /** The host ANSWERED whether it has the GPU HEVC co-op encoder: it is an Umbra build that
     *  reports UmbraCaps bit 2. False for stock hosts and older Umbra builds -- unknown, not "no". */
    bool gpu_hevc_known;
    /** ...and its answer (UmbraCaps bit 3). Only meaningful when gpu_hevc_known. */
    bool gpu_hevc_installed;
} coop_host_facts_t;

struct _SERVER_DATA;

/** Prefer the host's current report, falling back to the user's saved vendor even
 * when this host has not yet returned serverinfo. Missing HDR data stays unknown. */
coop_host_facts_t coop_host_facts_from_server(const struct _SERVER_DATA *server,
                                             coop_gpu_vendor_t saved_vendor);

coop_stream_plan_t coop_stream_plan(const coop_host_facts_t *primary, const coop_host_facts_t *secondary,
                                    coop_stream_mode_t setting, bool hdr_requested);
coop_stream_plan_t coop_stream_plan_ex(const coop_host_facts_t *primary, const coop_host_facts_t *secondary,
                                       coop_stream_mode_t setting, bool hdr_requested, bool experimental_gpu_hevc);
/** The plan with the user's co-op encoder choice (Settings > Co-op, `coop_encoder`). AUTO is exactly
 *  coop_stream_plan_ex(.., true); NATIVE and GPU_HEVC set `forced`. Any plan that uses the GPU HEVC
 *  encoder sets `host_support_missing` when a host has SAID it does not have it (UmbraCaps bit 2
 *  without bit 3) -- silence from an older or stock host is not a "no". */
coop_stream_plan_t coop_stream_plan_choose(const coop_host_facts_t *primary, const coop_host_facts_t *secondary,
                                           coop_encoder_choice_t encoder, bool hdr_requested);

#ifdef __cplusplus
}
#endif
