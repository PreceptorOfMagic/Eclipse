/* HOST-AND-CLIENT CO-OP ON LINUX, KDE PLASMA (see local_pane_kde.h for what was measured). */
#include "local_pane_kde.h"

#include <wayland-client.h>

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zkde-screencast-unstable-v1-client-protocol.h"
#include "kde-output-device-v2-client-protocol.h"
#include "kde-output-management-v2-client-protocol.h"
#include "fake-input-client-protocol.h"

#define MAX_OUT 16
#define MAX_MODES 512
#define NO_NODE 0xffffffffu
#define OUTPUT_NAME "virtual-eclipse-pane"

typedef struct {
    struct kde_output_device_v2 *obj;
    char name[128];
    int x, y, w, h, enabled, alive;
    unsigned priority;
    wl_fixed_t scale;
    struct kde_output_device_mode_v2 *cur;
} kout_t;

typedef struct {
    struct kde_output_device_mode_v2 *obj;
    int w, h;
} kmode_t;

static struct wl_display *dpy;
static struct wl_registry *reg;
static struct zkde_screencast_unstable_v1 *sc;
static struct zkde_screencast_stream_unstable_v1 *stream;
static struct kde_output_management_v2 *mgmt;
static struct kde_output_device_registry_v2 *devreg;
static struct org_kde_kwin_fake_input *fake;
static kout_t outs[MAX_OUT];
static int nouts;
static kmode_t modes[MAX_MODES];
static int nmodes;
static uint32_t legacy_id[MAX_OUT], legacy_v[MAX_OUT];
static int nlegacy;
static bool bound_initial;
static uint32_t node = NO_NODE;
static bool stream_failed;
static int applied;
static SDL_Rect geo_client;
/* every output's position before the session (KWin may move them; negative positions are refused): put back at the end */
static struct { char name[128]; int x, y; } moved[MAX_OUT];
static int nmoved;
static double geo_client_scale = 1.0;
static SDL_Thread *pump;
static SDL_atomic_t pump_stop;
static void (*plog)(const char *fmt, ...);

/* THE ORIGINAL LAYOUT ON DISK, so a session that never reaches its end (Eclipse killed or crashed) is undone at the next
 * start. MEASURED 2026-11-15: Eclipse killed mid-session -> KWin removed the virtual output but HDMI-A-2 stayed at
 * 960,0. One "name x y" line per output; written before anything changes, removed once the layout is back. */
static void recover_layout(void);
static void restore_layout(void);

static void state_path(char *out, size_t len) {
    const char *dir = getenv("XDG_STATE_HOME");
    if (dir != NULL && dir[0] != 0) {
        snprintf(out, len, "%s/eclipse-kde-layout", dir);
    } else {
        snprintf(out, len, "%s/.local/state/eclipse-kde-layout", getenv("HOME") != NULL ? getenv("HOME") : "/tmp");
    }
}

/* ---- protocol listeners ---------------------------------------------------------------------------------------- */

static void m_size(void *p, struct kde_output_device_mode_v2 *o, int32_t w, int32_t h) {
    (void) o;
    ((kmode_t *) p)->w = w;
    ((kmode_t *) p)->h = h;
}
static void m_i(void *p, struct kde_output_device_mode_v2 *o, int32_t v) { (void) p; (void) o; (void) v; }
static void m_0(void *p, struct kde_output_device_mode_v2 *o) { (void) p; (void) o; }
static void m_u(void *p, struct kde_output_device_mode_v2 *o, uint32_t v) { (void) p; (void) o; (void) v; }
static const struct kde_output_device_mode_v2_listener mode_listener = {
        .size = m_size, .refresh = m_i, .preferred = m_0, .removed = m_0, .flags = m_u};

#define L0(n) static void n(void *p, struct kde_output_device_v2 *o) { (void) p; (void) o; }
#define LU(n) static void n(void *p, struct kde_output_device_v2 *o, uint32_t v) { (void) p; (void) o; (void) v; }
#define LS(n) static void n(void *p, struct kde_output_device_v2 *o, const char *v) { (void) p; (void) o; (void) v; }
static void o_geometry(void *p, struct kde_output_device_v2 *o, int32_t x, int32_t y, int32_t pw, int32_t ph,
                       int32_t sub, const char *make, const char *model, int32_t transform) {
    (void) o; (void) pw; (void) ph; (void) sub; (void) make; (void) model; (void) transform;
    ((kout_t *) p)->x = x;
    ((kout_t *) p)->y = y;
}
static void o_current(void *p, struct kde_output_device_v2 *o, struct kde_output_device_mode_v2 *m) {
    (void) o;
    ((kout_t *) p)->cur = m;
}
static void o_mode(void *p, struct kde_output_device_v2 *o, struct kde_output_device_mode_v2 *m) {
    (void) p; (void) o;
    if (nmodes < MAX_MODES) {
        modes[nmodes].obj = m;
        kde_output_device_mode_v2_add_listener(m, &mode_listener, &modes[nmodes]);
        nmodes++;
    }
}
static void o_scale(void *p, struct kde_output_device_v2 *o, wl_fixed_t s) { (void) o; ((kout_t *) p)->scale = s; }
static void o_enabled(void *p, struct kde_output_device_v2 *o, int32_t e) { (void) o; ((kout_t *) p)->enabled = e; }
static void o_name(void *p, struct kde_output_device_v2 *o, const char *n) {
    (void) o;
    snprintf(((kout_t *) p)->name, sizeof(((kout_t *) p)->name), "%s", n);
}
static void o_priority(void *p, struct kde_output_device_v2 *o, uint32_t v) { (void) o; ((kout_t *) p)->priority = v; }
static void o_removed(void *p, struct kde_output_device_v2 *o) { (void) o; ((kout_t *) p)->alive = 0; }
static void o_bm(void *p, struct kde_output_device_v2 *o, uint32_t a, uint32_t b, uint32_t c) {
    (void) p; (void) o; (void) a; (void) b; (void) c;
}
static void o_bo(void *p, struct kde_output_device_v2 *o, int32_t a, int32_t b, int32_t c) {
    (void) p; (void) o; (void) a; (void) b; (void) c;
}
static void o_range(void *p, struct kde_output_device_v2 *o, uint32_t a, uint32_t b) { (void) p; (void) o; (void) a; (void) b; }
L0(o_done) LS(o_edid) LS(o_uuid) LS(o_serial) LS(o_eisa) LU(o_caps) LU(o_overscan) LU(o_vrr) LU(o_rgb) LU(o_hdr)
LU(o_sdrb) LU(o_wcg) LU(o_arp) LS(o_icc) LU(o_sgw) LU(o_cps) LU(o_bright) LU(o_cpt) LU(o_dim) LS(o_repl) LU(o_ddc)
LU(o_mbpc) LU(o_ambpc) LU(o_edr) LU(o_sharp) LU(o_ab)
static const struct kde_output_device_v2_listener output_listener = {
        .geometry = o_geometry, .current_mode = o_current, .mode = o_mode, .done = o_done, .scale = o_scale,
        .edid = o_edid, .enabled = o_enabled, .uuid = o_uuid, .serial_number = o_serial, .eisa_id = o_eisa,
        .capabilities = o_caps, .overscan = o_overscan, .vrr_policy = o_vrr, .rgb_range = o_rgb, .name = o_name,
        .high_dynamic_range = o_hdr, .sdr_brightness = o_sdrb, .wide_color_gamut = o_wcg, .auto_rotate_policy = o_arp,
        .icc_profile_path = o_icc, .brightness_metadata = o_bm, .brightness_overrides = o_bo,
        .sdr_gamut_wideness = o_sgw, .color_profile_source = o_cps, .brightness = o_bright,
        .color_power_tradeoff = o_cpt, .dimming = o_dim, .replication_source = o_repl, .ddc_ci_allowed = o_ddc,
        .max_bits_per_color = o_mbpc, .max_bits_per_color_range = o_range, .automatic_max_bits_per_color_limit = o_ambpc,
        .edr_policy = o_edr, .sharpness = o_sharp, .priority = o_priority, .auto_brightness = o_ab,
        .removed = o_removed};

static void add_output(struct kde_output_device_v2 *obj) {
    if (nouts >= MAX_OUT) {
        return;
    }
    kout_t *o = &outs[nouts++];
    memset(o, 0, sizeof(*o));
    o->obj = obj;
    o->alive = 1;
    o->scale = wl_fixed_from_int(1);
    kde_output_device_v2_add_listener(obj, &output_listener, o);
}
static void dr_output(void *p, struct kde_output_device_registry_v2 *r, struct kde_output_device_v2 *o) {
    (void) p; (void) r;
    add_output(o);
}
static void dr_finished(void *p, struct kde_output_device_registry_v2 *r) { (void) p; (void) r; }
static const struct kde_output_device_registry_v2_listener devreg_listener = {.finished = dr_finished, .output = dr_output};

static void global(void *p, struct wl_registry *r, uint32_t id, const char *iface, uint32_t v) {
    (void) p;
    if (strcmp(iface, "zkde_screencast_unstable_v1") == 0 && v >= 2) {
        sc = wl_registry_bind(r, id, &zkde_screencast_unstable_v1_interface, v < 4 ? v : 4);
    } else if (strcmp(iface, "kde_output_management_v2") == 0) {
        mgmt = wl_registry_bind(r, id, &kde_output_management_v2_interface, v < 19 ? v : 19);
    } else if (strcmp(iface, "kde_output_device_registry_v2") == 0 && v >= 21 && devreg == NULL) {
        /* KWin 6.7 announces outputs only through this registry */
        devreg = wl_registry_bind(r, id, &kde_output_device_registry_v2_interface, 21);
        kde_output_device_registry_v2_add_listener(devreg, &devreg_listener, NULL);
    } else if (strcmp(iface, "kde_output_device_v2") == 0) {
        /* KWin 6.6: one global per output, the virtual one included once it exists */
        if (bound_initial) {
            if (devreg == NULL) {
                add_output(wl_registry_bind(r, id, &kde_output_device_v2_interface, v < 20 ? v : 20));
            }
        } else if (nlegacy < MAX_OUT) {
            legacy_id[nlegacy] = id;
            legacy_v[nlegacy++] = v < 20 ? v : 20;
        }
    } else if (strcmp(iface, "org_kde_kwin_fake_input") == 0) {
        fake = wl_registry_bind(r, id, &org_kde_kwin_fake_input_interface, v < 4 ? v : 4);
    }
}
static void global_remove(void *p, struct wl_registry *r, uint32_t id) { (void) p; (void) r; (void) id; }
static const struct wl_registry_listener registry_listener = {global, global_remove};

static void s_closed(void *p, struct zkde_screencast_stream_unstable_v1 *s) {
    (void) p; (void) s;
    stream_failed = true;
}
static void s_created(void *p, struct zkde_screencast_stream_unstable_v1 *s, uint32_t n) {
    (void) p; (void) s;
    node = n;
}
static void s_failed(void *p, struct zkde_screencast_stream_unstable_v1 *s, const char *e) {
    (void) p; (void) s;
    stream_failed = true;
    if (plog != NULL) plog("[pane-kde] KWin refused the virtual output: %s", e);
}
static const struct zkde_screencast_stream_unstable_v1_listener stream_listener = {s_closed, s_created, s_failed};

static void c_applied(void *p, struct kde_output_configuration_v2 *c) { (void) p; (void) c; applied = 1; }
static void c_failed(void *p, struct kde_output_configuration_v2 *c) { (void) p; (void) c; applied = -1; }
static void c_reason(void *p, struct kde_output_configuration_v2 *c, const char *r) {
    (void) p; (void) c;
    if (plog != NULL) plog("[pane-kde] layout refused: %s", r);
}
static const struct kde_output_configuration_v2_listener config_listener = {
        .applied = c_applied, .failed = c_failed, .failure_reason = c_reason};

/* ---- connection ---------------------------------------------------------------------------------------------- */

static void disconnect_all(void) {
    if (stream != NULL) {
        zkde_screencast_stream_unstable_v1_close(stream);
        stream = NULL;
    }
    if (dpy != NULL) {
        wl_display_flush(dpy);
        wl_display_disconnect(dpy);     /* frees every proxy with it; KWin drops the output if the close was lost */
    }
    dpy = NULL;
    reg = NULL;
    sc = NULL;
    mgmt = NULL;
    devreg = NULL;
    fake = NULL;
    nouts = nmodes = nlegacy = 0;
    bound_initial = false;
    node = NO_NODE;
    stream_failed = false;
}

static bool connect_all(void) {
    dpy = wl_display_connect(NULL);
    if (dpy == NULL) {
        return false;
    }
    reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &registry_listener, NULL);
    wl_display_roundtrip(dpy);
    if (devreg == NULL) {
        for (int i = 0; i < nlegacy; i++) {
            add_output(wl_registry_bind(reg, legacy_id[i], &kde_output_device_v2_interface, legacy_v[i]));
        }
    }
    bound_initial = true;
    wl_display_roundtrip(dpy);
    wl_display_roundtrip(dpy);
    return true;
}

bool local_pane_kde_available(void) {
    if (dpy != NULL) {
        return true;    /* a session is running */
    }
    /* asked whenever the co-op dialog or the host list redraws: one Wayland connection per 5 s at most */
    static Uint32 asked_at;
    static bool answer;
    if (asked_at != 0 && SDL_GetTicks() - asked_at < 5000) {
        return answer;
    }
    asked_at = SDL_GetTicks();
    if (asked_at == 0) asked_at = 1;
    answer = false;
    if (!connect_all()) {
        return false;
    }
    answer = sc != NULL && mgmt != NULL && fake != NULL && nouts > 0;
    static bool recovered;
    if (answer && !recovered) {
        recovered = true;
        recover_layout();
    }
    disconnect_all();
    return answer;
}

static void logical_size(const kout_t *o, int *w, int *h) {
    int pw = 0, ph = 0;
    for (int i = 0; i < nmodes; i++) {
        if (modes[i].obj == o->cur) {
            pw = modes[i].w;
            ph = modes[i].h;
        }
    }
    const double s = wl_fixed_to_double(o->scale) > 0 ? wl_fixed_to_double(o->scale) : 1.0;
    *w = (int) (pw / s + 0.5);
    *h = (int) (ph / s + 0.5);
}

static kout_t *virtual_output(void) {
    for (int i = 0; i < nouts; i++) {
        if (outs[i].alive && strstr(outs[i].name, OUTPUT_NAME) != NULL) {
            return &outs[i];
        }
    }
    return NULL;
}

/* Every recorded output back where it was (only those that moved); the state file goes once that is done. */
static void restore_layout(void) {
    if (nmoved > 0 && mgmt != NULL) {
        for (int tries = 0; tries < 30 && virtual_output() != NULL; tries++) {   /* after the virtual output is gone */
            wl_display_roundtrip(dpy);
            SDL_Delay(30);
        }
        struct kde_output_configuration_v2 *c = kde_output_management_v2_create_configuration(mgmt);
        kde_output_configuration_v2_add_listener(c, &config_listener, NULL);
        applied = 0;
        int n = 0;
        for (int i = 0; i < nouts; i++) {
            for (int j = 0; j < nmoved; j++) {
                if (outs[i].alive && outs[i].enabled && strcmp(outs[i].name, moved[j].name) == 0 &&
                    (outs[i].x != moved[j].x || outs[i].y != moved[j].y)) {
                    kde_output_configuration_v2_position(c, outs[i].obj, moved[j].x, moved[j].y);
                    n++;
                }
            }
        }
        if (n > 0) {
            kde_output_configuration_v2_apply(c);
            for (int i = 0; i < 100 && applied == 0; i++) wl_display_roundtrip(dpy);
            if (plog != NULL) plog("[pane-kde] %d output(s) put back where they were: %s", n, applied == 1 ? "done" : "REFUSED");
        }
        kde_output_configuration_v2_destroy(c);
        if (n == 0 || applied == 1) {
            char path[512];
            state_path(path, sizeof(path));
            remove(path);
        }
        nmoved = 0;
    }
}

static void save_state(void) {
    char path[512], tmp[560];
    state_path(path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (f == NULL) return;
    for (int j = 0; j < nmoved; j++) fprintf(f, "%s %d %d\n", moved[j].name, moved[j].x, moved[j].y);
    if (fclose(f) == 0) rename(tmp, path); else remove(tmp);
}

/* A previous session left its layout (state file present, no virtual output of ours): put it back. */
static void recover_layout(void) {
    char path[512];
    state_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (f == NULL) return;
    nmoved = 0;
    char name[128];
    int x, y;
    while (nmoved < MAX_OUT && fscanf(f, "%127s %d %d", name, &x, &y) == 3) {
        snprintf(moved[nmoved].name, sizeof(moved[nmoved].name), "%s", name);
        moved[nmoved].x = x;
        moved[nmoved].y = y;
        nmoved++;
    }
    fclose(f);
    if (plog != NULL) plog("[pane-kde] an earlier session did not end cleanly: restoring %d output position(s)", nmoved);
    restore_layout();
}

static int SDLCALL pump_main(void *data) {
    (void) data;
    /* Keeps the connection read: KWin's events (output changes, the stream's state) must not pile up unread. */
    while (!SDL_AtomicGet(&pump_stop)) {
        while (wl_display_prepare_read(dpy) != 0) {
            wl_display_dispatch_pending(dpy);
        }
        wl_display_flush(dpy);
        struct pollfd pfd = {wl_display_get_fd(dpy), POLLIN, 0};
        if (poll(&pfd, 1, 100) > 0 && (pfd.revents & POLLIN)) {
            wl_display_read_events(dpy);
        } else {
            wl_display_cancel_read(dpy);
        }
        wl_display_dispatch_pending(dpy);
    }
    return 0;
}

bool local_pane_kde_start(int width, int height, local_pane_side_t side, uint32_t *out_node, SDL_Rect *virtual_rect,
                          SDL_Rect *client_rect, double *client_scale, void (*log)(const char *fmt, ...)) {
    plog = log;
    if (dpy != NULL || !connect_all()) {
        return false;
    }
    if (sc == NULL || mgmt == NULL || fake == NULL) {
        plog("[pane-kde] KWin does not offer its virtual outputs to Eclipse (screencast %d, output management %d, "
             "fake input %d): the desktop file's X-KDE-Wayland-Interfaces is missing", sc != NULL, mgmt != NULL,
             fake != NULL);
        disconnect_all();
        return false;
    }
    /* this window's output: the primary one (priority 1) before the virtual output exists, else the first enabled */
    const kout_t *client = NULL;
    for (int i = 0; i < nouts; i++) {
        if (outs[i].alive && outs[i].enabled && (client == NULL || outs[i].priority == 1)) {
            client = &outs[i];
        }
    }
    if (client == NULL) {
        disconnect_all();
        return false;
    }
    int cw, ch;
    logical_size(client, &cw, &ch);
    geo_client = (SDL_Rect) {client->x, client->y, cw, ch};
    geo_client_scale = wl_fixed_to_double(client->scale) > 0 ? wl_fixed_to_double(client->scale) : 1.0;
    char client_name[128];
    snprintf(client_name, sizeof(client_name), "%s", client->name);
    /* THE ORIGINAL LAYOUT, recorded before the virtual output exists, is what the end puts back. MEASURED 2026-11-15:
     * KWin remembers a layout per set of outputs, so when the virtual output appears it may re-apply the shift a
     * previous session made (HDMI-A-2 jumped to 960,0 on its own); positions read before it appears are stale. */
    recover_layout();
    nmoved = 0;
    for (int i = 0; i < nouts && nmoved < MAX_OUT; i++) {
        if (!outs[i].alive || !outs[i].enabled) continue;
        snprintf(moved[nmoved].name, sizeof(moved[nmoved].name), "%s", outs[i].name);
        moved[nmoved].x = outs[i].x;
        moved[nmoved].y = outs[i].y;
        nmoved++;
    }
    save_state();

    /* cursor mode 2 = embedded: the view shows this PC's pointer like the screen would */
    stream = zkde_screencast_unstable_v1_stream_virtual_output(sc, OUTPUT_NAME, width, height, wl_fixed_from_int(1), 2);
    zkde_screencast_stream_unstable_v1_add_listener(stream, &stream_listener, NULL);
    for (int i = 0; i < 100 && node == NO_NODE && !stream_failed; i++) {
        wl_display_roundtrip(dpy);
        if (node == NO_NODE) SDL_Delay(20);
    }
    kout_t *v = NULL;
    for (int i = 0; i < 50 && v == NULL && node != NO_NODE; i++) {   /* its output object follows the node */
        wl_display_roundtrip(dpy);
        v = virtual_output();
        if (v == NULL) SDL_Delay(20);
    }
    if (node == NO_NODE || v == NULL) {
        plog("[pane-kde] no virtual output (node %s, listed %s)", node == NO_NODE ? "missing" : "ok", v ? "yes" : "no");
        disconnect_all();
        return false;
    }
    wl_display_roundtrip(dpy);       /* let KWin finish its own placement first */
    wl_display_roundtrip(dpy);
    int minx = 1 << 30, miny = 1 << 30, maxx = -(1 << 30), maxy = -(1 << 30);
    for (int i = 0; i < nouts; i++) {
        if (!outs[i].alive || !outs[i].enabled || &outs[i] == v) continue;
        int w, h;
        logical_size(&outs[i], &w, &h);
        if (outs[i].x < minx) minx = outs[i].x;
        if (outs[i].y < miny) miny = outs[i].y;
        if (outs[i].x + w > maxx) maxx = outs[i].x + w;
        if (outs[i].y + h > maxy) maxy = outs[i].y + h;
        if (strcmp(outs[i].name, client_name) == 0) {
            geo_client.x = outs[i].x;
            geo_client.y = outs[i].y;
        }
    }
    SDL_Rect vr = {0, 0, width, height};
    switch (side) {
        case LOCAL_PANE_SIDE_LEFT: vr.x = minx - width; vr.y = geo_client.y; break;
        case LOCAL_PANE_SIDE_RIGHT: vr.x = maxx; vr.y = geo_client.y; break;
        case LOCAL_PANE_SIDE_TOP: vr.x = geo_client.x; vr.y = miny - height; break;
        default: vr.x = geo_client.x; vr.y = maxy; break;
    }
    /* MEASURED 2026-11-15 (KWin 6.6.6): "Position of enabled output ... is negative (-960, 0)" -- the layout must
     * start at 0,0, so a virtual output on the left or top shifts the others, and they go back at the end. */
    const int ox = vr.x < minx ? vr.x : minx, oy = vr.y < miny ? vr.y : miny;
    struct kde_output_configuration_v2 *c = kde_output_management_v2_create_configuration(mgmt);
    kde_output_configuration_v2_add_listener(c, &config_listener, NULL);
    applied = 0;
    if (ox < 0 || oy < 0) {
        const int dx = ox < 0 ? -ox : 0, dy = oy < 0 ? -oy : 0;
        for (int i = 0; i < nouts; i++) {
            if (!outs[i].alive || !outs[i].enabled || &outs[i] == v) continue;
            kde_output_configuration_v2_position(c, outs[i].obj, outs[i].x + dx, outs[i].y + dy);
        }
        vr.x += dx;
        vr.y += dy;
    }
    kde_output_configuration_v2_position(c, v->obj, vr.x, vr.y);
    kde_output_configuration_v2_scale(c, v->obj, wl_fixed_from_int(1));
    if (kde_output_management_v2_get_version(mgmt) >= 3) {
        kde_output_configuration_v2_set_priority(c, v->obj, 1);
    }
    kde_output_configuration_v2_apply(c);
    for (int i = 0; i < 100 && applied == 0; i++) {
        wl_display_roundtrip(dpy);
    }
    kde_output_configuration_v2_destroy(c);
    wl_display_roundtrip(dpy);
    wl_display_roundtrip(dpy);
    /* KWin may normalise the layout: read back where everything is now */
    v = virtual_output();
    for (int i = 0; i < nouts; i++) {
        if (outs[i].alive && strcmp(outs[i].name, client_name) == 0) {
            geo_client.x = outs[i].x;
            geo_client.y = outs[i].y;
        }
    }
    if (applied != 1 || v == NULL) {
        plog("[pane-kde] the virtual output could not be placed");
        disconnect_all();
        return false;
    }
    int vw, vh;
    logical_size(v, &vw, &vh);
    *virtual_rect = (SDL_Rect) {v->x, v->y, vw > 0 ? vw : width, vh > 0 ? vh : height};
    *client_rect = geo_client;
    *client_scale = geo_client_scale;
    *out_node = node;
    org_kde_kwin_fake_input_authenticate(fake, "Eclipse", "Moves the pointer between this PC's half and the stream");
    wl_display_flush(dpy);
    plog("[pane-kde] layout: virtual output %s %dx%d at %d,%d (priority 1, scale 1); this window's output %s at %d,%d "
         "%dx%d scale %.2f", v->name, virtual_rect->w, virtual_rect->h, virtual_rect->x, virtual_rect->y, client_name,
         geo_client.x, geo_client.y, geo_client.w, geo_client.h, geo_client_scale);
    SDL_AtomicSet(&pump_stop, 0);
    pump = SDL_CreateThread(pump_main, "PaneKde", NULL);
    return true;
}

bool local_pane_kde_pointer(double x, double y) {
    if (dpy == NULL || fake == NULL) {
        return false;
    }
    org_kde_kwin_fake_input_pointer_motion_absolute(fake, wl_fixed_from_double(x), wl_fixed_from_double(y));
    wl_display_flush(dpy);
    return true;
}

void local_pane_kde_stop(void) {
    if (dpy == NULL) {
        return;
    }
    /* off the virtual output before it goes (GNOME crashed when the pointer was on a removed output) */
    local_pane_kde_pointer(geo_client.x + geo_client.w / 2.0, geo_client.y + geo_client.h / 2.0);
    SDL_Delay(150);
    if (pump != NULL) {
        SDL_AtomicSet(&pump_stop, 1);
        SDL_WaitThread(pump, NULL);
        pump = NULL;
    }
    if (stream != NULL) {
        zkde_screencast_stream_unstable_v1_close(stream);
        stream = NULL;
    }
    restore_layout();
    disconnect_all();
}
