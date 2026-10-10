/* Host facts and launch planning: no TV, network or encoder required. */
#include "coop_stream_mode.h"
#include "libgamestream/client.h"

#include <assert.h>
#include <stdio.h>

int main(void) {
    SERVER_DATA nv = {.encoderVendor = "nvidia", .encoderVendorKnown = true,
                      .supportsHdr = true, .coopProfileCaps = COOP_PROFILE_CAP_MAIN_CONTROL};
    SERVER_DATA amd = {.encoderVendor = "amd", .encoderVendorKnown = true, .supportsHdr = false};
    coop_host_facts_t a = coop_host_facts_from_server(&nv, COOP_GPU_AMD);
    coop_host_facts_t b = coop_host_facts_from_server(&amd, COOP_GPU_NVIDIA);
    assert(a.vendor == COOP_GPU_NVIDIA && b.vendor == COOP_GPU_AMD);
    coop_stream_plan_t plan = coop_stream_plan(&a, &b, COOP_STREAM_AUTO, true);
    assert(plan.mode == COOP_STREAM_CORE && plan.gpu_hevc && plan.mixed_vendor && plan.hdr_allowed);
    plan = coop_stream_plan(&b, &a, COOP_STREAM_AUTO, true);
    assert(plan.mode == COOP_STREAM_CORE && plan.gpu_hevc && plan.hdr_allowed);

    /* A scripted launch can precede peer serverinfo. Saved vendor is still evidence;
     * missing HDR capabilities are unknown, not a fabricated negative report. */
    b = coop_host_facts_from_server(NULL, COOP_GPU_AMD);
    assert(b.vendor_known && !b.supports_hdr_known);
    plan = coop_stream_plan(&a, &b, COOP_STREAM_AUTO, true);
    assert(plan.mode == COOP_STREAM_CORE && plan.gpu_hevc && plan.hdr_allowed);
    b = coop_host_facts_from_server(NULL, COOP_GPU_NVIDIA);
    plan = coop_stream_plan(&a, &b, COOP_STREAM_AUTO, true);
    assert(plan.mode == COOP_STREAM_CORE && plan.hdr_allowed);

    b = coop_host_facts_from_server(&nv, COOP_GPU_UNKNOWN);
    plan = coop_stream_plan(&a, &b, COOP_STREAM_AUTO, true);
    assert(plan.mode == COOP_STREAM_CORE && plan.hdr_allowed);
    nv.supportsHdr = false;
    b = coop_host_facts_from_server(&nv, COOP_GPU_UNKNOWN);
    plan = coop_stream_plan(&a, &b, COOP_STREAM_AUTO, true);
    assert(plan.mode == COOP_STREAM_CORE && !plan.hdr_allowed && plan.hdr_blocked_by_host);

    b = coop_host_facts_from_server(NULL, COOP_GPU_UNKNOWN);
    assert(!b.vendor_known && !b.supports_hdr_known);
    plan = coop_stream_plan(&a, &b, COOP_STREAM_AUTO, true);
    assert(plan.mode == COOP_STREAM_CORE && !plan.vendors_known);
    plan = coop_stream_plan(&a, &b, COOP_STREAM_COMPAT, true);
    assert(plan.mode == COOP_STREAM_CORE && !plan.gpu_hevc && !plan.forced && plan.hdr_allowed);
    b = coop_host_facts_from_server(&amd, COOP_GPU_UNKNOWN);
    plan = coop_stream_plan(&a, &b, COOP_STREAM_CORE, false);
    assert(plan.mode == COOP_STREAM_CORE && plan.gpu_hevc && !plan.forced && !plan.hdr_allowed);
    // Beta pair routing overrides stale compatibility preferences but preserves NV+NV.
    plan = coop_stream_plan_ex(&a, &a, COOP_STREAM_AUTO, true, true);
    assert(!plan.gpu_hevc && plan.mode == COOP_STREAM_CORE);
    plan = coop_stream_plan_ex(&a, &b, COOP_STREAM_AUTO, true, true);
    assert(plan.gpu_hevc && plan.mode == COOP_STREAM_CORE && plan.hdr_allowed);
    plan = coop_stream_plan_ex(&b, &a, COOP_STREAM_AUTO, true, true);
    assert(plan.gpu_hevc && plan.mode == COOP_STREAM_CORE && plan.hdr_allowed);
    plan = coop_stream_plan_ex(&b, &b, COOP_STREAM_AUTO, false, true);
    assert(plan.gpu_hevc && plan.mode == COOP_STREAM_CORE && !plan.hdr_allowed);
    plan = coop_stream_plan_ex(&a, &b, COOP_STREAM_AUTO, true, false);
    assert(!plan.gpu_hevc && plan.mode == COOP_STREAM_CORE);
    plan = coop_stream_plan_ex(&a, &b, COOP_STREAM_COMPAT, true, true);
    assert(plan.gpu_hevc && plan.mode == COOP_STREAM_CORE && !plan.forced);
    plan = coop_stream_plan_ex(&a, &a, COOP_STREAM_COMPAT, true, true);
    assert(!plan.gpu_hevc && plan.mode == COOP_STREAM_CORE && !plan.forced);
    coop_host_facts_t unknown = {0};
    plan = coop_stream_plan_ex(&a, &unknown, COOP_STREAM_AUTO, true, true);
    assert(!plan.gpu_hevc);
    for (int legacy = 0; legacy <= 2; ++legacy) {
        plan = coop_stream_plan(&a, &b, (coop_stream_mode_t)legacy, false);
        assert(plan.gpu_hevc && plan.mode == COOP_STREAM_CORE);
        plan = coop_stream_plan(&b, &a, (coop_stream_mode_t)legacy, false);
        assert(plan.gpu_hevc && plan.mode == COOP_STREAM_CORE);
        plan = coop_stream_plan(&a, &a, (coop_stream_mode_t)legacy, false);
        assert(!plan.gpu_hevc && plan.mode == COOP_STREAM_CORE);
    }
    /* coop_encoder (Settings > Co-op > Encoder): AUTO == the automatic plan; overrides are forced. */
    plan = coop_stream_plan_choose(&a, &b, COOP_ENCODER_AUTO, true);
    assert(plan.gpu_hevc && !plan.forced && !plan.host_support_missing);
    plan = coop_stream_plan_choose(&a, &a, COOP_ENCODER_AUTO, true);
    assert(!plan.gpu_hevc && !plan.forced);
    plan = coop_stream_plan_choose(&a, &b, COOP_ENCODER_NATIVE, true);
    assert(!plan.gpu_hevc && plan.forced && plan.mixed_vendor);
    plan = coop_stream_plan_choose(&a, &a, COOP_ENCODER_GPU_HEVC, true);
    assert(plan.gpu_hevc && plan.forced && plan.hdr_allowed);
    plan = coop_stream_plan_choose(&a, &unknown, COOP_ENCODER_GPU_HEVC, false);
    assert(plan.gpu_hevc && plan.forced && !plan.hdr_allowed);

    /* UmbraCaps: bit 2 without bit 3 is a KNOWN "no encoder"; no bit 2 (older/stock) is unknown. */
    SERVER_DATA amd_umbra = amd;
    amd_umbra.umbraCapsKnown = true;
    amd_umbra.umbraCaps = 0x4 | 0x8;
    coop_host_facts_t c = coop_host_facts_from_server(&amd_umbra, COOP_GPU_UNKNOWN);
    assert(c.gpu_hevc_known && c.gpu_hevc_installed);
    plan = coop_stream_plan_choose(&a, &c, COOP_ENCODER_AUTO, true);
    assert(plan.gpu_hevc && !plan.host_support_missing);
    amd_umbra.umbraCaps = 0x4;
    c = coop_host_facts_from_server(&amd_umbra, COOP_GPU_UNKNOWN);
    assert(c.gpu_hevc_known && !c.gpu_hevc_installed);
    plan = coop_stream_plan_choose(&a, &c, COOP_ENCODER_AUTO, true);
    assert(plan.gpu_hevc && plan.host_support_missing);
    plan = coop_stream_plan_choose(&a, &a, COOP_ENCODER_AUTO, true);
    assert(!plan.host_support_missing);
    amd_umbra.umbraCaps = 0x3;   /* an Umbra from before bit 2: says nothing about the encoder */
    c = coop_host_facts_from_server(&amd_umbra, COOP_GPU_UNKNOWN);
    assert(!c.gpu_hevc_known);
    plan = coop_stream_plan_choose(&a, &c, COOP_ENCODER_AUTO, true);
    assert(plan.gpu_hevc && !plan.host_support_missing);
    puts("PASS: vendor precedence, missing serverinfo, both primary roles, HDR and explicit modes");
    return 0;
}
