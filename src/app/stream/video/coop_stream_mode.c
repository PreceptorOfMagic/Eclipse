#include "coop_stream_mode.h"
#include "libgamestream/client.h"

#include <string.h>

coop_gpu_vendor_t coop_gpu_vendor_parse(const char *token) {
    if (token == NULL || token[0] == '\0') {
        return COOP_GPU_UNKNOWN;
    }
    /* The tokens Apollo emits, one per encoder family its own probe can choose. Matched
     * exactly rather than by prefix: a future "nvidia-something" should read as UNKNOWN
     * and take the conservative path, not be silently folded into NVIDIA. */
    if (strcmp(token, "nvidia") == 0) { return COOP_GPU_NVIDIA; }
    if (strcmp(token, "amd") == 0) { return COOP_GPU_AMD; }
    if (strcmp(token, "intel") == 0) { return COOP_GPU_INTEL; }
    if (strcmp(token, "software") == 0) { return COOP_GPU_SOFTWARE; }
    if (strcmp(token, "other") == 0) { return COOP_GPU_OTHER; }
    return COOP_GPU_UNKNOWN;
}

const char *coop_gpu_vendor_name(coop_gpu_vendor_t vendor) {
    switch (vendor) {
        case COOP_GPU_NVIDIA: return "NVIDIA";
        case COOP_GPU_AMD: return "AMD";
        case COOP_GPU_INTEL: return "Intel";
        case COOP_GPU_SOFTWARE: return "software";
        case COOP_GPU_UNKNOWN:
        default: return "unknown";
    }
}

const char *coop_gpu_vendor_token(coop_gpu_vendor_t vendor) {
    switch (vendor) {
        case COOP_GPU_NVIDIA: return "nvidia";
        case COOP_GPU_AMD: return "amd";
        case COOP_GPU_INTEL: return "intel";
        case COOP_GPU_SOFTWARE: return "software";
        case COOP_GPU_OTHER: return "other";
        case COOP_GPU_UNKNOWN:
        default: return "unknown";
    }
}

/** Tested here means: this project has actually run co-op on it. That is NVIDIA and AMD
 *  and nothing else -- Intel has never been in the rig, and the software encoder has
 *  never been asked to feed a 4K composite. */
coop_host_facts_t coop_host_facts_from_server(const SERVER_DATA *server,
                                             coop_gpu_vendor_t saved_vendor) {
    coop_host_facts_t facts = {0};
    facts.vendor = saved_vendor;
    if (server != NULL) {
        if (server->encoderVendorKnown) {
            facts.vendor = coop_gpu_vendor_parse(server->encoderVendor);
        }
        facts.profile_caps = server->coopProfileCaps;
        facts.supports_hdr = server->supportsHdr;
        facts.supports_hdr_known = true;
        /* UmbraCaps bit 2 (UMBRA_CAP_COOP_PANE) = this Umbra build reports bit 3 (UMBRA_CAP_GPU_HEVC,
         * encoder installed). Values mirror backend/aurora_host_caps.h. */
        facts.gpu_hevc_known = server->umbraCapsKnown && (server->umbraCaps & 0x4) != 0;
        facts.gpu_hevc_installed = facts.gpu_hevc_known && (server->umbraCaps & 0x8) != 0;
    }
    facts.vendor_known = facts.vendor != COOP_GPU_UNKNOWN;
    return facts;
}

static bool vendor_is_tested(coop_gpu_vendor_t vendor) {
    return vendor == COOP_GPU_NVIDIA || vendor == COOP_GPU_AMD;
}

coop_stream_plan_t coop_stream_plan(const coop_host_facts_t *primary, const coop_host_facts_t *secondary,
                                    coop_stream_mode_t setting, bool hdr_requested) {
    return coop_stream_plan_ex(primary, secondary, setting, hdr_requested, true);
}

coop_stream_plan_t coop_stream_plan_ex(const coop_host_facts_t *primary, const coop_host_facts_t *secondary,
                                       coop_stream_mode_t setting, bool hdr_requested, bool experimental_gpu_hevc) {
    coop_stream_plan_t plan;
    memset(&plan, 0, sizeof(plan));
    if (primary == NULL || secondary == NULL) {
        plan.mode = COOP_STREAM_CORE;
        plan.hdr_allowed = hdr_requested;
        plan.hdr_requested = hdr_requested;
        return plan;
    }

    plan.vendors_known = primary->vendor_known && secondary->vendor_known;
    plan.mixed_vendor = plan.vendors_known && primary->vendor != secondary->vendor;
    /* Only claimed about a host that ANSWERED. An unknown vendor is not an untested one;
     * saying "untested" at every stock host would make the word mean nothing. */
    plan.untested_vendor = (primary->vendor_known && !vendor_is_tested(primary->vendor)) ||
                           (secondary->vendor_known && !vendor_is_tested(secondary->vendor));

    /* Saved AVC/core preferences are legacy input. Both supported pairings now
     * use HEVC; the GPU mapping chooses the encoder on BOTH hosts. */
    (void) setting;
    plan.mode = COOP_STREAM_CORE;
    plan.gpu_hevc = experimental_gpu_hevc && plan.vendors_known &&
                    vendor_is_tested(primary->vendor) && vendor_is_tested(secondary->vendor) &&
                    (primary->vendor == COOP_GPU_AMD || secondary->vendor == COOP_GPU_AMD);

    /* ---- HDR, decided on its own evidence -------------------------------------
     *
     * NOT FOLDED INTO THE CODEC GATE. The user's HDR setting is overruled by the
     * PAIRING in exactly one case -- a host that cannot do 10-bit -- and that case has
     * nothing to do with vendors matching: two NVIDIA cards where one host reports no
     * 10-bit support must still drop it, and two different vendors that both support it
     * lose HDR only because H.264 has no 10-bit form to carry it in.
     *
     * BOTH HOSTS, NOT EITHER. The composite is one bitstream with one SPS: two panes at
     * different bit depths cannot be spliced at all, so the pairing gets the lower
     * common capability rather than a per-pane answer. */
    plan.hdr_requested = hdr_requested;
    if (hdr_requested) {
        /* A KNOWN NO, from either host. Silence is not a no: an unpolled or sleeping
         * host says nothing about its 10-bit support, and treating that as "cannot" is
         * how a setting the user chose gets turned off by an empty cache. */
        plan.hdr_blocked_by_host = (primary->supports_hdr_known && !primary->supports_hdr) ||
                                   (secondary->supports_hdr_known && !secondary->supports_hdr);
        // Fixed-function capabilities do not describe the programmable backend.
        // Opening the requested depth must succeed on both hosts; never downgrade it.
        if (plan.gpu_hevc) plan.hdr_blocked_by_host = false;
        plan.hdr_lost_to_codec = false;
    }
    plan.hdr_allowed = hdr_requested && !plan.hdr_blocked_by_host && !plan.hdr_lost_to_codec;
    return plan;
}

coop_stream_plan_t coop_stream_plan_choose(const coop_host_facts_t *primary, const coop_host_facts_t *secondary,
                                           coop_encoder_choice_t encoder, bool hdr_requested) {
    coop_stream_plan_t plan = coop_stream_plan_ex(primary, secondary, COOP_STREAM_AUTO, hdr_requested, true);
    if (primary == NULL || secondary == NULL) {
        return plan;
    }
    if (encoder == COOP_ENCODER_NATIVE || encoder == COOP_ENCODER_GPU_HEVC) {
        /* AN OVERRIDE, AND SAID AS ONE: the dialog names a forced choice that contradicts the
         * pair (native encoders on a mixed pair cannot share one picture). HDR is re-derived
         * because the GPU HEVC encoder opens the requested depth itself while a native pair
         * needs both hosts to report 10-bit. */
        const bool gpu = encoder == COOP_ENCODER_GPU_HEVC;
        coop_stream_plan_t forced = coop_stream_plan_ex(primary, secondary, COOP_STREAM_AUTO, hdr_requested, gpu);
        if (gpu && !forced.gpu_hevc) {
            /* AUTO would not have picked it (NVIDIA+NVIDIA, unknown vendors): ask for it anyway. */
            forced.gpu_hevc = true;
            forced.hdr_blocked_by_host = false;
            forced.hdr_allowed = hdr_requested;
        }
        forced.forced = true;
        plan = forced;
    }
    if (plan.gpu_hevc) {
        plan.host_support_missing = (primary->gpu_hevc_known && !primary->gpu_hevc_installed) ||
                                    (secondary->gpu_hevc_known && !secondary->gpu_hevc_installed);
    }
    return plan;
}
