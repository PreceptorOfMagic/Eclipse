/* HOST-AND-CLIENT CO-OP ON LINUX: this PC's own display. See local_pane_linux.h for the why; the measurements behind
 * every step are in docs/design-host-and-client-linux.md (section D). */
#include "local_pane_linux.h"
#include "local_pane_kde.h"

#include <gio/gio.h>
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/buffers.h>
#include <spa/buffer/meta.h>
#include <spa/utils/result.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MUTTER_SC "org.gnome.Mutter.ScreenCast"
#define MUTTER_RD "org.gnome.Mutter.RemoteDesktop"
#define MUTTER_DC "org.gnome.Mutter.DisplayConfig"

static void (*log_fn)(const char *line);

static void plog(const char *fmt, ...) {
    char line[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (log_fn != NULL) {
        log_fn(line);
    }
}

/* ---- D-Bus ------------------------------------------------------------------------------------------------- */

static GDBusConnection *bus;
static char *rd_path, *sc_path, *vstream_path, *pstream_path;
static guint32 pw_node = 0;
static bool node_seen = false;
static bool running = false;

static GVariant *dcall(const char *name, const char *path, const char *iface, const char *method, GVariant *args,
                       const char *reply, GError **err) {
    return g_dbus_connection_call_sync(bus, name, path, iface, method, args,
                                       reply != NULL ? G_VARIANT_TYPE(reply) : NULL, G_DBUS_CALL_FLAGS_NONE, 5000,
                                       NULL, err);
}

static bool name_has_owner(const char *name) {
    GVariant *r = dcall("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "NameHasOwner",
                        g_variant_new("(s)", name), "(b)", NULL);
    if (r == NULL) {
        return false;
    }
    gboolean has = FALSE;
    g_variant_get(r, "(b)", &has);
    g_variant_unref(r);
    return has;
}

bool local_pane_linux_available(void) {
    /* Mutter makes virtual outputs only as a Wayland compositor; on "GNOME on Xorg" the same bus names answer but
     * RecordVirtual cannot work, and co-op must stay the ordinary kind (this PC streamed to itself). */
    const char *session_type = getenv("XDG_SESSION_TYPE");
    if (getenv("WAYLAND_DISPLAY") == NULL && (session_type == NULL || strcmp(session_type, "wayland") != 0)) {
        return false;
    }
    if (bus == NULL) {
        bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    }
    if (bus != NULL && name_has_owner(MUTTER_SC) && name_has_owner(MUTTER_RD) && name_has_owner(MUTTER_DC)) {
        return true;                       /* GNOME */
    }
    return local_pane_kde_available();     /* KDE Plasma (local_pane_kde.c) */
}

static bool on_kde;                        /* this session's virtual output is KWin's, not Mutter's */

static void on_stream_added(GDBusConnection *c, const char *sender, const char *path, const char *iface,
                            const char *signal, GVariant *params, gpointer data) {
    (void) c; (void) sender; (void) path; (void) iface; (void) signal; (void) data;
    g_variant_get(params, "(u)", &pw_node);
    node_seen = true;
}

/* ---- layout (DisplayConfig) ------------------------------------------------------------------------------- */

typedef struct {
    char connector[64];
    char mode[64];
    int x, y, w, h;          /* logical */
    double scale;
    guint transform;
    bool primary;
} lmon_t;

#define MAX_MON 8
static lmon_t saved[MAX_MON];
static int saved_n;
static SDL_Rect geo_virtual, geo_client;
static double geo_scale_client = 1.0;    /* the client's output: its pixels per logical unit (RecordMonitor pixels) */

/** Read the layout: one entry per logical monitor (its first monitor). @return count, -1 on error. */
static int read_layout(lmon_t *out, int max, guint32 *serial) {
    GError *err = NULL;
    GVariant *st = dcall(MUTTER_DC, "/org/gnome/Mutter/DisplayConfig", MUTTER_DC, "GetCurrentState", NULL,
                         "(ua((ssss)a(siiddada{sv})a{sv})a(iiduba(ssss)a{sv})a{sv})", &err);
    if (st == NULL) {
        plog("[pane-linux] DisplayConfig.GetCurrentState failed: %s", err ? err->message : "?");
        g_clear_error(&err);
        return -1;
    }
    GVariant *monitors = NULL, *logical = NULL, *props = NULL;
    g_variant_get(st, "(u@a((ssss)a(siiddada{sv})a{sv})@a(iiduba(ssss)a{sv})@a{sv})", serial, &monitors, &logical,
                  &props);
    int n = 0;
    GVariantIter lit;
    g_variant_iter_init(&lit, logical);
    gint32 x, y;
    gdouble scale;
    guint32 transform;
    gboolean primary;
    GVariant *specs, *lprops;
    while (n < max && g_variant_iter_next(&lit, "(iidub@a(ssss)@a{sv})", &x, &y, &scale, &transform, &primary, &specs,
                                          &lprops)) {
        lmon_t *m = &out[n];
        memset(m, 0, sizeof(*m));
        m->x = x;
        m->y = y;
        m->scale = scale;
        m->transform = transform;
        m->primary = primary;
        if (g_variant_n_children(specs) > 0) {
            const char *conn = NULL;
            g_variant_get_child(specs, 0, "(&sss&s)", &conn, NULL, NULL, NULL);
            g_strlcpy(m->connector, conn, sizeof(m->connector));
            n++;
        }
        g_variant_unref(specs);
        g_variant_unref(lprops);
    }
    /* each logical monitor's current mode: its id and size */
    GVariantIter mit;
    g_variant_iter_init(&mit, monitors);
    GVariant *spec, *modes, *mprops;
    while (g_variant_iter_next(&mit, "(@(ssss)@a(siiddada{sv})@a{sv})", &spec, &modes, &mprops)) {
        const char *conn = NULL;
        g_variant_get(spec, "(&sss&s)", &conn, NULL, NULL, NULL);
        for (int i = 0; i < n; i++) {
            if (strcmp(out[i].connector, conn) != 0) {
                continue;
            }
            GVariantIter mdi;
            g_variant_iter_init(&mdi, modes);
            const char *id;
            gint32 w, h;
            gdouble rate, pref;
            GVariant *scales, *mdp;
            while (g_variant_iter_next(&mdi, "(&siidd@ad@a{sv})", &id, &w, &h, &rate, &pref, &scales, &mdp)) {
                gboolean cur = FALSE;
                g_variant_lookup(mdp, "is-current", "b", &cur);
                if (cur) {
                    g_strlcpy(out[i].mode, id, sizeof(out[i].mode));
                    out[i].w = (int) (w / (out[i].scale > 0 ? out[i].scale : 1));
                    out[i].h = (int) (h / (out[i].scale > 0 ? out[i].scale : 1));
                }
                g_variant_unref(scales);
                g_variant_unref(mdp);
            }
        }
        g_variant_unref(spec);
        g_variant_unref(modes);
        g_variant_unref(mprops);
    }
    g_variant_unref(monitors);
    g_variant_unref(logical);
    g_variant_unref(props);
    g_variant_unref(st);
    return n;
}

static bool apply_layout(guint32 serial, const lmon_t *mons, int n) {
    GVariantBuilder lms;
    g_variant_builder_init(&lms, G_VARIANT_TYPE("a(iiduba(ssa{sv}))"));
    for (int i = 0; i < n; i++) {
        GVariantBuilder ml;
        g_variant_builder_init(&ml, G_VARIANT_TYPE("a(ssa{sv})"));
        g_variant_builder_add(&ml, "(ss@a{sv})", mons[i].connector, mons[i].mode,
                              g_variant_new_array(G_VARIANT_TYPE("{sv}"), NULL, 0));
        g_variant_builder_add(&lms, "(iidub@a(ssa{sv}))", mons[i].x, mons[i].y, mons[i].scale, mons[i].transform,
                              mons[i].primary, g_variant_builder_end(&ml));
    }
    GError *err = NULL;
    GVariant *r = dcall(MUTTER_DC, "/org/gnome/Mutter/DisplayConfig", MUTTER_DC, "ApplyMonitorsConfig",
                        g_variant_new("(uu@a(iiduba(ssa{sv}))@a{sv})", serial, 1u, g_variant_builder_end(&lms),
                                      g_variant_new_array(G_VARIANT_TYPE("{sv}"), NULL, 0)),
                        NULL, &err);
    if (r == NULL) {
        plog("[pane-linux] ApplyMonitorsConfig failed: %s", err ? err->message : "?");
        g_clear_error(&err);
        return false;
    }
    g_variant_unref(r);
    return true;
}

/** Put the virtual output beyond the layout on @p side, primary, scale 1; remember the layout for the end. */
static bool place_virtual(local_pane_side_t side) {
    lmon_t cur[MAX_MON];
    guint32 serial = 0;
    int n = -1;
    int vi = -1;
    for (int tries = 0; tries < 40 && vi < 0; tries++) {       /* the output appears once the format is agreed */
        n = read_layout(cur, MAX_MON, &serial);
        for (int i = 0; i < n; i++) {
            bool known = false;
            for (int j = 0; j < saved_n; j++) {
                known |= strcmp(saved[j].connector, cur[i].connector) == 0;
            }
            if (!known && strncmp(cur[i].connector, "Meta-", 5) == 0) {
                vi = i;
            }
        }
        if (vi < 0) {
            g_usleep(100 * 1000);
        }
    }
    if (vi < 0) {
        plog("[pane-linux] the virtual output never appeared in the layout");
        return false;
    }
    lmon_t *v = &cur[vi];
    v->w = (int) (v->w * v->scale);     /* to scale 1 */
    v->h = (int) (v->h * v->scale);
    v->scale = 1.0;
    int minx = 1 << 30, miny = 1 << 30, maxx = -(1 << 30), maxy = -(1 << 30);
    const lmon_t *client = NULL;
    for (int i = 0; i < n; i++) {
        if (i == vi) {
            continue;
        }
        if (cur[i].x < minx) minx = cur[i].x;
        if (cur[i].y < miny) miny = cur[i].y;
        if (cur[i].x + cur[i].w > maxx) maxx = cur[i].x + cur[i].w;
        if (cur[i].y + cur[i].h > maxy) maxy = cur[i].y + cur[i].h;
        if (client == NULL || cur[i].primary) {
            client = &cur[i];
        }
    }
    if (client == NULL) {
        return false;
    }
    switch (side) {
        case LOCAL_PANE_SIDE_LEFT: v->x = minx - v->w; v->y = client->y; break;
        case LOCAL_PANE_SIDE_RIGHT: v->x = maxx; v->y = client->y; break;
        case LOCAL_PANE_SIDE_TOP: v->x = client->x; v->y = miny - v->h; break;
        default: v->x = client->x; v->y = maxy; break;
    }
    /* Mutter wants the layout to start at 0,0 */
    int ox = v->x < minx ? v->x : minx, oy = v->y < miny ? v->y : miny;
    for (int i = 0; i < n; i++) {
        cur[i].x -= ox;
        cur[i].y -= oy;
        cur[i].primary = (i == vi);
    }
    geo_client = (SDL_Rect) {client->x, client->y, client->w, client->h};
    geo_scale_client = client->scale > 0 ? client->scale : 1.0;
    geo_virtual = (SDL_Rect) {v->x, v->y, v->w, v->h};
    if (!apply_layout(serial, cur, n)) {
        return false;
    }
    plog("[pane-linux] layout: virtual output %s %dx%d at %d,%d (primary, scale 1); this window's output %s at %d,%d %dx%d",
         v->connector, v->w, v->h, v->x, v->y, client->connector, client->x, client->y, client->w, client->h);
    return true;
}

static void restore_layout(void) {
    if (saved_n <= 0) {
        return;
    }
    lmon_t cur[MAX_MON];
    guint32 serial = 0;
    for (int tries = 0; tries < 30; tries++) {           /* wait for the virtual output to be gone */
        int n = read_layout(cur, MAX_MON, &serial);
        bool virt = false;
        for (int i = 0; i < n; i++) {
            virt |= strncmp(cur[i].connector, "Meta-", 5) == 0;
        }
        if (n >= 0 && !virt) {
            bool same = n == saved_n;
            for (int i = 0; same && i < n; i++) {
                same = strcmp(cur[i].connector, saved[i].connector) == 0 && cur[i].x == saved[i].x &&
                       cur[i].y == saved[i].y && cur[i].primary == saved[i].primary;
            }
            if (!same) {
                plog("[pane-linux] putting the previous layout back");
                apply_layout(serial, saved, saved_n);
            }
            saved_n = 0;
            return;
        }
        g_usleep(100 * 1000);
    }
    plog("[pane-linux] the virtual output is still in the layout; previous layout not re-applied");
    saved_n = 0;
}

/* ---- PipeWire ------------------------------------------------------------------------------------------------ */

static struct pw_thread_loop *pw_loop;
static struct pw_context *pw_ctx;
static struct pw_core *pw_core_;
static struct pw_stream *pw_stream_;
static struct spa_hook stream_hook;
static int want_w, want_h, want_hz;
static int fmt_w, fmt_h;
static bool fmt_ok;

static SDL_mutex *frame_lock;
static uint8_t *frame;          /* latest picture, tightly packed BGRx */
static int frame_w, frame_h;
static uint64_t frame_seq, frames_total;

/* GNOME's cursor, sent beside the picture (cursor-mode metadata). MEASURED 2026-11-15 (VEGA, GNOME 50.1): with the
 * cursor embedded, Mutter sent 0.0 pictures/s of the virtual output while only the pointer moved over it, so this PC's
 * half showed a frozen cursor and the user lost the mouse. Metadata arrives on every movement; we draw it ourselves. */
#define CURSOR_MAX 1024      /* the meta slot GNOME 50 offers is a fixed 384x384 one (589872 bytes); the range must reach it */
static bool cursor_meta;                /* asked for (GNOME); KWin's stream keeps its own cursor in the picture */
static bool cursor_shown;
static int cursor_x, cursor_y, cursor_hx, cursor_hy;
static uint8_t *cursor_px;              /* cursor_w x cursor_h, 4 bytes each, in cursor_fmt */
static int cursor_w, cursor_h;
static uint32_t cursor_fmt;
static uint64_t cursor_seq, cursor_updates;

static unsigned cursor_logged;

static void take_cursor(struct spa_buffer *buf) {
    struct spa_meta_cursor *c = spa_buffer_find_meta_data(buf, SPA_META_Cursor, sizeof(*c));
    if (cursor_logged < 3 || (c != NULL && cursor_logged < 12)) {
        cursor_logged++;
        if (c == NULL) {
            plog("[pane-linux] cursor: buffer without cursor metadata (%u metas)", buf->n_metas);
        } else {
            const struct spa_meta_bitmap *bm =
                    c->bitmap_offset >= sizeof(*c) ? SPA_PTROFF(c, c->bitmap_offset, struct spa_meta_bitmap) : NULL;
            plog("[pane-linux] cursor: id %u at %d,%d hotspot %d,%d bitmap %s %ux%u format %u", c->id, c->position.x,
                 c->position.y, c->hotspot.x, c->hotspot.y, bm ? "yes" : "no", bm ? bm->size.width : 0,
                 bm ? bm->size.height : 0, bm ? bm->format : 0);
        }
    }
    if (c == NULL) {
        return;
    }
    if (!spa_meta_cursor_is_valid(c)) {      /* id 0: no cursor on this output */
        if (cursor_shown) {
            cursor_shown = false;
            cursor_seq++;
        }
        return;
    }
    cursor_x = c->position.x;
    cursor_y = c->position.y;
    cursor_hx = c->hotspot.x;
    cursor_hy = c->hotspot.y;
    cursor_shown = true;
    if (c->bitmap_offset >= sizeof(*c)) {
        const struct spa_meta_bitmap *bm = SPA_PTROFF(c, c->bitmap_offset, struct spa_meta_bitmap);
        const int w = (int) bm->size.width, h = (int) bm->size.height;
        if (w > 0 && h > 0 && w <= CURSOR_MAX && h <= CURSOR_MAX && bm->offset >= sizeof(*bm) &&
            (bm->format == SPA_VIDEO_FORMAT_RGBA || bm->format == SPA_VIDEO_FORMAT_BGRA)) {
            const uint8_t *src = SPA_PTROFF(bm, bm->offset, uint8_t);
            const int stride = bm->stride > 0 ? bm->stride : w * 4;
            uint8_t *px = malloc((size_t) w * h * 4);
            if (px != NULL) {
                for (int y = 0; y < h; y++) {
                    memcpy(px + (size_t) y * w * 4, src + (size_t) y * stride, (size_t) w * 4);
                }
                free(cursor_px);
                cursor_px = px;
                cursor_w = w;
                cursor_h = h;
                cursor_fmt = bm->format;
            }
        }
    }
    cursor_seq++;
    cursor_updates++;
}

static void on_param_changed(void *data, uint32_t id, const struct spa_pod *param) {
    (void) data;
    if (param == NULL || id != SPA_PARAM_Format) {
        return;
    }
    struct spa_video_info_raw info;
    if (spa_format_video_raw_parse(param, &info) < 0) {
        return;
    }
    fmt_w = (int) info.size.width;
    fmt_h = (int) info.size.height;
    fmt_ok = true;
    plog("[pane-linux] picture format agreed: %dx%d, format %u, %u/%u fps", fmt_w, fmt_h, info.format,
         info.max_framerate.num, info.max_framerate.denom);
    uint8_t buf[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    const struct spa_pod *params[2];
    params[0] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
                                           SPA_PARAM_BUFFERS_dataType,
                                           SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr)));
#define CURSOR_META_SIZE(w, h) (sizeof(struct spa_meta_cursor) + sizeof(struct spa_meta_bitmap) + (w) * (h) * 4)
    params[1] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
                                           SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Cursor),
                                           SPA_PARAM_META_size,
                                           SPA_POD_CHOICE_RANGE_Int(CURSOR_META_SIZE(64, 64), CURSOR_META_SIZE(1, 1),
                                                                    CURSOR_META_SIZE(CURSOR_MAX, CURSOR_MAX)));
    pw_stream_update_params(pw_stream_, params, cursor_meta ? 2 : 1);
}

static void on_process(void *data) {
    (void) data;
    struct pw_buffer *b = pw_stream_dequeue_buffer(pw_stream_);
    if (b == NULL) {
        return;
    }
    struct spa_data *d = &b->buffer->datas[0];
    if (cursor_meta) {
        SDL_LockMutex(frame_lock);
        take_cursor(b->buffer);
        SDL_UnlockMutex(frame_lock);
    }
    /* a cursor-only buffer carries no picture: empty, or flagged corrupted */
    if (d->data != NULL && d->chunk != NULL && d->chunk->size > 0 && !(d->chunk->flags & SPA_CHUNK_FLAG_CORRUPTED) &&
        fmt_ok) {
        const int stride = d->chunk->stride > 0 ? d->chunk->stride : fmt_w * 4;
        SDL_LockMutex(frame_lock);
        if (frame == NULL || frame_w != fmt_w || frame_h != fmt_h) {
            free(frame);
            frame = malloc((size_t) fmt_w * fmt_h * 4);
            frame_w = fmt_w;
            frame_h = fmt_h;
        }
        if (frame != NULL) {
            const uint8_t *src = (const uint8_t *) d->data + d->chunk->offset;
            for (int y = 0; y < fmt_h; y++) {
                memcpy(frame + (size_t) y * fmt_w * 4, src + (size_t) y * stride, (size_t) fmt_w * 4);
            }
            frame_seq++;
            frames_total++;
        }
        SDL_UnlockMutex(frame_lock);
    }
    pw_stream_queue_buffer(pw_stream_, b);
}

/* THE OUTPUT CAN BE TAKEN AWAY UNDER US. GNOME's screen-sharing indicator (top bar, orange) stops the session when
 * clicked. MEASURED 2026-11-15 on VEGA (GNOME Shell 50.1): "Removed virtual monitor Meta-0", this stream went to
 * "paused" and stayed there, and the session carried on with a frozen half the pointer could no longer reach. A
 * stream that has streamed and then sits out of "streaming" is reported by local_pane_linux_lost(). */
static volatile bool pw_streamed;
static volatile Uint32 pw_out_since;     /* SDL ticks when it left "streaming" after having streamed; 0 = streaming */

static void on_state(void *data, enum pw_stream_state old, enum pw_stream_state state, const char *error) {
    (void) data; (void) old;
    plog("[pane-linux] picture stream %s%s%s", pw_stream_state_as_string(state), error ? ": " : "", error ? error : "");
    if (state == PW_STREAM_STATE_STREAMING) {
        pw_streamed = true;
        pw_out_since = 0;
    } else if (pw_streamed && pw_out_since == 0) {
        const Uint32 now = SDL_GetTicks();
        pw_out_since = now != 0 ? now : 1;
    }
}

bool local_pane_linux_lost(void) {
    if (!running || on_kde) {
        return false;
    }
    const Uint32 since = pw_out_since;
    /* a pause while the format is renegotiated comes back within a frame or two */
    return since != 0 && SDL_TICKS_PASSED(SDL_GetTicks(), since + 1500);
}

static const struct pw_stream_events stream_events = {
        PW_VERSION_STREAM_EVENTS,
        .state_changed = on_state,
        .param_changed = on_param_changed,
        .process = on_process,
};

static bool pw_start(uint32_t node) {
    pw_streamed = false;
    pw_out_since = 0;
    pw_init(NULL, NULL);
    pw_loop = pw_thread_loop_new("eclipse-pane", NULL);
    if (pw_loop == NULL) {
        return false;
    }
    pw_ctx = pw_context_new(pw_thread_loop_get_loop(pw_loop), NULL, 0);
    if (pw_ctx == NULL || pw_thread_loop_start(pw_loop) < 0) {
        return false;
    }
    pw_thread_loop_lock(pw_loop);
    pw_core_ = pw_context_connect(pw_ctx, NULL, 0);
    if (pw_core_ == NULL) {
        pw_thread_loop_unlock(pw_loop);
        plog("[pane-linux] cannot reach PipeWire");
        return false;
    }
    pw_stream_ = pw_stream_new(pw_core_, "eclipse-local-pane",
                               pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture",
                                                 PW_KEY_MEDIA_ROLE, "Screen", NULL));
    pw_stream_add_listener(pw_stream_, &stream_hook, &stream_events, NULL);
    uint8_t buf[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    struct spa_rectangle size = SPA_RECTANGLE(want_w, want_h);
    struct spa_fraction rate = SPA_FRACTION(want_hz, 1), rmin = SPA_FRACTION(0, 1), rmax = SPA_FRACTION(1000, 1);
    const struct spa_pod *params[1];
    params[0] = spa_pod_builder_add_object(
            &b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
            SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
            SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
            SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(3, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRx,
                                                           SPA_VIDEO_FORMAT_BGRA),
            SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&size),
            SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&SPA_FRACTION(0, 1)),
            SPA_FORMAT_VIDEO_maxFramerate, SPA_POD_CHOICE_RANGE_Fraction(&rate, &rmin, &rmax));
    const int rc = pw_stream_connect(pw_stream_, PW_DIRECTION_INPUT, node,
                                     PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS, params, 1);
    pw_thread_loop_unlock(pw_loop);
    if (rc < 0) {
        plog("[pane-linux] PipeWire connect to node %u failed: %s", node, spa_strerror(rc));
        return false;
    }
    return true;
}

static void pw_stop(void) {
    if (pw_loop != NULL) {
        pw_thread_loop_lock(pw_loop);
        if (pw_stream_ != NULL) {
            pw_stream_disconnect(pw_stream_);
            pw_stream_destroy(pw_stream_);
            pw_stream_ = NULL;
        }
        if (pw_core_ != NULL) {
            pw_core_disconnect(pw_core_);
            pw_core_ = NULL;
        }
        pw_thread_loop_unlock(pw_loop);
        pw_thread_loop_stop(pw_loop);
        if (pw_ctx != NULL) {
            pw_context_destroy(pw_ctx);
            pw_ctx = NULL;
        }
        pw_thread_loop_destroy(pw_loop);
        pw_loop = NULL;
    }
    fmt_ok = false;
}

/* ---- session ------------------------------------------------------------------------------------------------- */

static char *dup_path(GVariant *r) {
    const char *p = NULL;
    g_variant_get(r, "(&o)", &p);
    char *out = g_strdup(p);
    g_variant_unref(r);
    return out;
}

bool local_pane_linux_start(int width, int height, int hz, local_pane_side_t side, void (*log)(const char *line)) {
    log_fn = log;
    if (running) {
        return true;
    }
    if (!local_pane_linux_available()) {
        plog("[pane-linux] GNOME's ScreenCast/RemoteDesktop/DisplayConfig are not on the session bus");
        return false;
    }
    if (frame_lock == NULL) {
        frame_lock = SDL_CreateMutex();
    }
    want_w = width;
    want_h = height;
    want_hz = hz > 0 ? hz : 60;
    on_kde = !(name_has_owner(MUTTER_SC) && name_has_owner(MUTTER_RD) && name_has_owner(MUTTER_DC));
    cursor_meta = !on_kde;
    cursor_shown = false;
    cursor_updates = 0;
    cursor_logged = 0;
    if (on_kde) {
        uint32_t kde_node = 0;
        if (!local_pane_kde_start(width, height, side, &kde_node, &geo_virtual, &geo_client, &geo_scale_client, plog)) {
            return false;
        }
        running = true;
        plog("[pane-linux] virtual output session up (KDE): PipeWire node %u, %dx%d", kde_node, width, height);
        if (!pw_start(kde_node)) {
            local_pane_linux_stop();
            return false;
        }
        return true;
    }
    guint32 serial;
    saved_n = read_layout(saved, MAX_MON, &serial);
    if (saved_n < 0) {
        saved_n = 0;
    }
    GError *err = NULL;
    GMainContext *ctx = g_main_context_new();
    g_main_context_push_thread_default(ctx);
    GVariant *r = dcall(MUTTER_RD, "/org/gnome/Mutter/RemoteDesktop", MUTTER_RD, "CreateSession", NULL, "(o)", &err);
    if (r == NULL) {
        goto fail;
    }
    rd_path = dup_path(r);
    r = dcall(MUTTER_RD, rd_path, "org.freedesktop.DBus.Properties", "Get",
              g_variant_new("(ss)", MUTTER_RD ".Session", "SessionId"), "(v)", &err);
    if (r == NULL) {
        goto fail;
    }
    GVariant *idv = NULL;
    g_variant_get(r, "(v)", &idv);
    const char *session_id = g_variant_get_string(idv, NULL);
    GVariantBuilder opts;
    g_variant_builder_init(&opts, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&opts, "{sv}", "remote-desktop-session-id", g_variant_new_string(session_id));
    GVariant *r2 = dcall(MUTTER_SC, "/org/gnome/Mutter/ScreenCast", MUTTER_SC, "CreateSession",
                         g_variant_new("(a{sv})", &opts), "(o)", &err);
    g_variant_unref(idv);
    g_variant_unref(r);
    if (r2 == NULL) {
        goto fail;
    }
    sc_path = dup_path(r2);
    g_variant_builder_init(&opts, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&opts, "{sv}", "cursor-mode", g_variant_new_uint32(2));   /* metadata: see cursor_meta */
    g_variant_builder_add(&opts, "{sv}", "is-platform", g_variant_new_boolean(TRUE));
    r = dcall(MUTTER_SC, sc_path, MUTTER_SC ".Session", "RecordVirtual", g_variant_new("(a{sv})", &opts), "(o)", &err);
    if (r == NULL) {
        goto fail;
    }
    vstream_path = dup_path(r);
    g_variant_builder_init(&opts, G_VARIANT_TYPE("a{sv}"));
    r = dcall(MUTTER_SC, sc_path, MUTTER_SC ".Session", "RecordMonitor", g_variant_new("(sa{sv})", "", &opts), "(o)",
              &err);
    if (r == NULL) {
        goto fail;
    }
    pstream_path = dup_path(r);
    node_seen = false;
    const guint sub = g_dbus_connection_signal_subscribe(bus, MUTTER_SC, MUTTER_SC ".Stream", "PipeWireStreamAdded",
                                                         vstream_path, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
                                                         on_stream_added, NULL, NULL);
    r = dcall(MUTTER_RD, rd_path, MUTTER_RD ".Session", "Start", NULL, NULL, &err);
    if (r == NULL) {
        g_dbus_connection_signal_unsubscribe(bus, sub);
        goto fail;
    }
    g_variant_unref(r);
    const gint64 deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
    while (!node_seen && g_get_monotonic_time() < deadline) {
        g_main_context_iteration(ctx, FALSE);
        g_usleep(10 * 1000);
    }
    g_dbus_connection_signal_unsubscribe(bus, sub);
    g_main_context_pop_thread_default(ctx);
    g_main_context_unref(ctx);
    ctx = NULL;
    if (!node_seen) {
        plog("[pane-linux] no PipeWire node for the virtual output within 5 s");
        goto fail_stop;
    }
    running = true;
    plog("[pane-linux] virtual output session up: PipeWire node %u, %dx%d at %d Hz asked", pw_node, width, height,
         want_hz);
    if (!pw_start(pw_node)) {
        goto fail_stop;
    }
    if (!place_virtual(side)) {
        goto fail_stop;
    }
    return true;

fail:
    plog("[pane-linux] Mutter session failed: %s", err ? err->message : "?");
    g_clear_error(&err);
    if (ctx != NULL) {
        g_main_context_pop_thread_default(ctx);
        g_main_context_unref(ctx);
    }
fail_stop:
    running = true;
    local_pane_linux_stop();
    return false;
}

void local_pane_linux_stop(void) {
    if (!running) {
        return;
    }
    if (on_kde) {
        running = false;
        pw_stop();
        local_pane_kde_stop();             /* pointer off the output first, then the output removed */
        plog("[pane-linux] virtual output session stopped after %llu pictures", (unsigned long long) frames_total);
        return;
    }
    /* THE POINTER MUST NOT BE ON THE VIRTUAL OUTPUT WHEN IT GOES. MEASURED 2026-11-14 on VEGA (GNOME Shell 50.1):
     * stopping the session with the pointer on it crashed gnome-shell 7 s later (core dump: meta_monitor_manager_
     * ensure_configured -> ... -> meta_logical_monitor_get_layout on a monitor that was gone), taking every window
     * of the session with it. The same stop with the pointer on the real output was clean. Move it first. */
    if (pstream_path != NULL && geo_client.w > 0) {
        GVariant *r = dcall(MUTTER_RD, rd_path, MUTTER_RD ".Session", "NotifyPointerMotionAbsolute",
                            g_variant_new("(sdd)", pstream_path, geo_client.w * geo_scale_client / 2.0,
                                          geo_client.h * geo_scale_client / 2.0), NULL, NULL);
        if (r != NULL) {
            g_variant_unref(r);
        }
        g_usleep(150 * 1000);
    }
    running = false;
    pw_stop();
    if (rd_path != NULL) {
        GVariant *r = dcall(MUTTER_RD, rd_path, MUTTER_RD ".Session", "Stop", NULL, NULL, NULL);
        if (r != NULL) {
            g_variant_unref(r);
        }
    }
    g_clear_pointer(&rd_path, g_free);
    g_clear_pointer(&sc_path, g_free);
    g_clear_pointer(&vstream_path, g_free);
    g_clear_pointer(&pstream_path, g_free);
    restore_layout();
    plog("[pane-linux] virtual output session stopped after %llu pictures, %llu cursor updates",
         (unsigned long long) frames_total, (unsigned long long) cursor_updates);
}

bool local_pane_linux_running(void) { return running; }

uint64_t local_pane_linux_frames(void) { return frames_total; }

bool local_pane_linux_geometry(SDL_Rect *virtual_output, SDL_Rect *client_output) {
    if (!running) {
        return false;
    }
    if (virtual_output != NULL) *virtual_output = geo_virtual;
    if (client_output != NULL) *client_output = geo_client;
    return true;
}

static bool motion_abs(const char *stream, double x, double y) {
    if (!running || rd_path == NULL || stream == NULL) {
        return false;
    }
    g_dbus_connection_call(bus, MUTTER_RD, rd_path, MUTTER_RD ".Session", "NotifyPointerMotionAbsolute",
                           g_variant_new("(sdd)", stream, x, y), NULL, G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL, NULL);
    return true;
}

bool local_pane_linux_pointer_to_pane(double x, double y) {
    if (on_kde) {      /* KWin takes the whole layout's logical coordinates */
        return running && local_pane_kde_pointer(geo_virtual.x + x, geo_virtual.y + y);
    }
    return motion_abs(vstream_path, x, y);
}

bool local_pane_linux_pointer_to_client_output(double x, double y) {
    if (on_kde) {      /* @p x, @p y are that output's pixels, as Mutter's monitor stream takes them */
        return running && local_pane_kde_pointer(geo_client.x + x / geo_scale_client, geo_client.y + y / geo_scale_client);
    }
    return motion_abs(pstream_path, x, y);
}

/* ---- view (UI thread) ---------------------------------------------------------------------------------------- */

static SDL_Texture *tex;
static SDL_Renderer *tex_renderer;
static int tex_w, tex_h;
static uint64_t tex_seq;
static SDL_Texture *cur_tex;
static SDL_Renderer *cur_tex_renderer;
static uint64_t cur_tex_seq, cur_drawn_seq;

void local_pane_linux_drop_texture(void) {
    if (tex != NULL) {
        SDL_DestroyTexture(tex);
        tex = NULL;
    }
    tex_seq = 0;
    if (cur_tex != NULL) {
        SDL_DestroyTexture(cur_tex);
        cur_tex = NULL;
    }
    cur_drawn_seq = 0;
}

/** GNOME only (cursor_meta): this PC's cursor over its half, at the position Mutter last reported. */
static void draw_cursor(SDL_Renderer *renderer, const SDL_Rect *dst) {
    if (!cursor_meta || frame_w <= 0 || frame_h <= 0) {
        return;
    }
    SDL_LockMutex(frame_lock);
    const bool shown = cursor_shown && cursor_px != NULL;
    if (shown && (cur_tex == NULL || cur_tex_renderer != renderer || cur_tex_seq != cursor_seq)) {
        if (cur_tex != NULL) {
            SDL_DestroyTexture(cur_tex);
        }
        /* bytes R,G,B,A = SDL's ABGR8888 on little-endian; B,G,R,A = ARGB8888 */
        cur_tex = SDL_CreateTexture(renderer, cursor_fmt == SPA_VIDEO_FORMAT_RGBA ? SDL_PIXELFORMAT_ABGR8888
                                                                                  : SDL_PIXELFORMAT_ARGB8888,
                                    SDL_TEXTUREACCESS_STATIC, cursor_w, cursor_h);
        if (cur_tex != NULL) {
            SDL_UpdateTexture(cur_tex, NULL, cursor_px, cursor_w * 4);
            SDL_SetTextureBlendMode(cur_tex, SDL_BLENDMODE_BLEND);
        }
        cur_tex_renderer = renderer;
        cur_tex_seq = cursor_seq;
    }
    const int x = cursor_x - cursor_hx, y = cursor_y - cursor_hy, w = cursor_w, h = cursor_h;
    cur_drawn_seq = cursor_seq;
    SDL_UnlockMutex(frame_lock);
    if (!shown || cur_tex == NULL) {
        return;
    }
    const SDL_Rect r = {dst->x + x * dst->w / frame_w, dst->y + y * dst->h / frame_h, w * dst->w / frame_w,
                        h * dst->h / frame_h};
    SDL_RenderCopy(renderer, cur_tex, NULL, &r);
}

bool local_pane_linux_draw(SDL_Renderer *renderer, const SDL_Rect *dst) {
    if (!running || frame_lock == NULL) {
        return false;
    }
    SDL_LockMutex(frame_lock);
    if (frame != NULL) {
        if (tex == NULL || tex_renderer != renderer || tex_w != frame_w || tex_h != frame_h) {
            local_pane_linux_drop_texture();
            tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, frame_w, frame_h);
            tex_renderer = renderer;
            tex_w = frame_w;
            tex_h = frame_h;
        }
        if (tex != NULL && tex_seq != frame_seq) {
            SDL_UpdateTexture(tex, NULL, frame, frame_w * 4);
            tex_seq = frame_seq;
        }
    }
    SDL_UnlockMutex(frame_lock);
    if (tex == NULL) {
        return false;
    }
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_NONE);
    const bool drawn = SDL_RenderCopy(renderer, tex, NULL, dst) == 0;
    draw_cursor(renderer, dst);
    return drawn;
}

bool local_pane_linux_frame_pending(void) {
    /* read without the lock: a stale answer costs one frame */
    return running && (frame_seq != tex_seq || (cursor_meta && cursor_seq != cur_drawn_seq));
}
