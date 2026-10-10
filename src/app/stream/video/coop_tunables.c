#include "coop_tunables.h"

#include "app.h"
#include "app_settings.h"
#include "coop_session.h"
#include "logging.h"

#include <stdio.h>
#include <unistd.h>

/** Presence sentinel: the file existing IS the setting. */
static void sentinel_set(const char *path, bool on) {
    if (on) {
        FILE *fp = fopen(path, "w");
        if (fp != NULL) {
            fclose(fp);
        }
    } else {
        unlink(path);
    }
}

/** Value sentinel: @p value of 0 means "leave the built-in default alone", which is the
 *  absence of the file rather than a zero in it -- every reader validates its range and
 *  a 0 would simply be ignored, leaving no way to express "unset". */
static void sentinel_int(const char *path, int value) {
    if (value <= 0) {
        unlink(path);
        return;
    }
    FILE *fp = fopen(path, "w");
    if (fp == NULL) {
        return;
    }
    fprintf(fp, "%d\n", value);
    fclose(fp);
}

void coop_tunables_apply(int orientation) {
    const app_settings_t *config = app_configuration;
    if (config == NULL) {
        return;
    }
    /* ONE COLUMN AT A TIME. The dials are held per orientation because the two modes
     * compose by different paths and load opposite hardware; what gets written is the
     * column for the orientation this app will actually run. Changing the orientation
     * re-applies (the settings screen calls this on every change), so the switches on
     * disk always describe the mode that is about to start. */
    const int o = orientation == COOP_ORIENTATION_VERTICAL
                  ? COOP_ORIENTATION_VERTICAL : COOP_ORIENTATION_HORIZONTAL;

    /* Rate. With no cap both hosts free-run, which is the requirement -- each side is
     * its own monitor. The cap restores the old shared-rate behaviour verbatim for an
     * A/B, and the peer bias only means anything alongside it. */
    sentinel_int("/tmp/coop-fps", config->coop_fps_cap[o]);
    sentinel_int("/tmp/coop-fps-peer", config->coop_fps_peer[o]);

    /* Composition. Slices and /tmp/coop-live are NOT here: they are armed with the mode
     * and taken away with it, because both reach outside the composite. */
    sentinel_int("/tmp/coop-queue", config->coop_queue_depth[o]);

    /* Concealment. THE TWO DEFAULTS ARE ON, and their sentinels are NEGATIVE -- the file
     * exists to turn the feature OFF -- so the sense inverts here. Getting this backwards
     * would silently disable the two mechanisms that let two free-running hosts be
     * spliced at all. */
    sentinel_set("/tmp/coop-noskipconceal", !config->coop_skip_conceal[o]);
    sentinel_set("/tmp/coop-nocatchup", !config->coop_catchup[o]);
    sentinel_set("/tmp/coop-conceal", config->coop_repeat_conceal[o]);
    sentinel_set("/tmp/coop-repeat", config->coop_repeat_pane[o]);

    /* Audio buffer depth; the mode itself is a real setting the audio path reads. */
    sentinel_int("/tmp/coop-audio-depth", config->coop_audio_depth_ms[o]);

    /* Diagnostics and single-host test sources. The mirrors compose the second pane from
     * the REAL desktop, which is why they are the sanctioned way to test with one host. */
    sentinel_set("/tmp/coop-measure", config->coop_measure[o]);
    sentinel_set("/tmp/coop-pump", config->coop_pump[o]);
    /* SELF-MIRROR IS HORIZONTAL-ONLY, and that is not an omission. Vertical's own
     * selector (/tmp/coop-vmirror) already composes a single host into both tile columns
     * -- session_video.c calls hmirror "the horizontal counterpart" for exactly that
     * reason -- so offering a vertical mirror switch would mean writing the vertical
     * MODE selector from a diagnostics toggle, which silently changes the composition. */
    sentinel_set("/tmp/coop-hmirror", config->coop_self_mirror[o]);

    commons_log_info("Coop", "Tunables applied (%s): fps=%d/%d queue=%d skipconceal=%d "
                             "catchup=%d mirror=%d",
                     o == COOP_ORIENTATION_VERTICAL ? "vertical" : "horizontal",
                     config->coop_fps_cap[o], config->coop_fps_peer[o],
                     config->coop_queue_depth[o], config->coop_skip_conceal[o] ? 1 : 0,
                     config->coop_catchup[o] ? 1 : 0, config->coop_self_mirror[o] ? 1 : 0);
}

void coop_tunables_arm_mode(int orientation) {
    /* THE SELECTOR, AND THE ONE IT MUST NOT BE CONFUSED WITH. Exactly one is on disk. */
    const bool vertical = orientation == COOP_ORIENTATION_VERTICAL;
    sentinel_set("/tmp/coop-vmirror", vertical);
    sentinel_set("/tmp/coop-hh-tiles", !vertical);
}

void coop_tunables_arm(int orientation) {
    const app_settings_t *config = app_configuration;
    if (config == NULL) {
        return;
    }
    const int o = orientation == COOP_ORIENTATION_VERTICAL
                  ? COOP_ORIENTATION_VERTICAL : COOP_ORIENTATION_HORIZONTAL;
    coop_tunables_apply(o);

    coop_tunables_arm_mode(o);
    const bool vertical = o == COOP_ORIENTATION_VERTICAL;

    /* /tmp/coop-live IS THE COMPOSITE ITSELF, not a hint about geometry.
     *
     * session_video.c declares it "real two-source splice": it gates the pane buffer,
     * the splice buffer and the whole live composition path, and it is what makes the
     * decoder be set up for the composed height rather than the half each host sends.
     * Without it a co-op session connects two hosts and composes NOTHING. Every proven
     * run has it -- run_coop.sh touches it on every launch, unconditionally, in both
     * modes -- so it belongs with the mode, not in a list of dials someone might leave
     * off. It was a switch defaulting to OFF on the settings screen until 2026-08-06,
     * which would have shipped exactly that silent failure. */
    sentinel_set("/tmp/coop-live", true);
    /* One slice per host is the measured configuration for BOTH modes (docs/coop-modes.md).
     * The built-in 4 is the single-host default and is wrong for a splice. */
    sentinel_int("/tmp/coop-slices", config->coop_slices[o] > 0 ? config->coop_slices[o] : 1);

    commons_log_info("Coop", "Armed %s: vmirror=%d hh-tiles=%d slices=%d",
                     vertical ? "vertical" : "horizontal", vertical ? 1 : 0, vertical ? 0 : 1,
                     config->coop_slices[o] > 0 ? config->coop_slices[o] : 1);
}

void coop_tunables_disarm(void) {
    /* THE GUARD ROWS GO WITH THE MODE. /tmp/coop-guard is only ever OVERWRITTEN by
     * run_coop.sh, so without this a co-op launched from the DIALOG after a guarded scripted
     * run inherits the previous run's geometry -- both hosts still agree, so it is not a
     * splice mismatch, it is silently the wrong composite height. Same class as the stale-ini
     * trap at session.c:390. */
    sentinel_set("/tmp/coop-guard", false);
    sentinel_set("/tmp/coop-vmirror", false);
    sentinel_set("/tmp/coop-hh-tiles", false);
    sentinel_set("/tmp/coop-slices", false);
    sentinel_set("/tmp/coop-hmirror", false);
    sentinel_set("/tmp/coop-live", false);
}

int coop_tunables_bitrate(void) {
    const app_settings_t *config = app_configuration;
    if (config == NULL) {
        return 0;
    }
    const int o = coop_orientation_resolved();
    return config->coop_bitrate[o];
}

int coop_tunables_fps(void) {
    const app_settings_t *config = app_configuration;
    if (config == NULL) {
        return 0;
    }
    const int o = coop_orientation_resolved();
    return config->coop_fps[o];
}

int coop_tunables_height(void) {
    const app_settings_t *config = app_configuration;
    if (config == NULL) {
        return 0;
    }
    const int o = coop_orientation_resolved();
    return config->coop_height[o];
}
