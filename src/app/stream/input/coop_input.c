/*
 * Co-op split-screen: controller routing. See coop_input.h for the design and
 * the rules this path obeys.
 */
#include "coop_input.h"

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "app.h"
#include "app_launch.h"
#include "coop_gamepad_map.h"
#include "coop_pointer.h"
#include "stream/coop_msg_channel.h"
#include "stream/video/coop_secondary.h"
#include "input/input_gamepad.h"

/*
 * Coordinator state. Every field below (including the pending queue and
 * counters) is protected by coord_state_lock. When both locks are needed, the
 * order is ALWAYS coord_state_lock -> coop_msg_channel_t::lock. This prevents
 * the session worker's stop path from detaching or closing a descriptor while
 * the UI/input thread is accepting, flushing, or sending on it.
 */
static bool coord_active = false;
static intptr_t coord_listen_fd = -1;
static intptr_t coord_peer_fd = -1;
static coop_msg_channel_t coord_channel = COOP_MSG_CHANNEL_INITIALIZER;
static pthread_mutex_t coord_state_lock = PTHREAD_MUTEX_INITIALIZER;
/* Kept from coord_start: rumble arrives from the peer and must reach the real pad. */
static app_input_t *coord_input = NULL;
/* Pointer state is UI-owned. Session start/stop only publish these requests;
 * coop_input_coord_pump() applies them before dispatching the next SDL event. */
static bool coord_pointer_start_pending = false;
static bool coord_pointer_stop_pending = false;

/* One transport packet may already be retained by coop_msg_channel on Windows.
 * This bounded FIFO holds later events until POLLOUT. Full-state samples can be
 * superseded, but edge/ordering events are never coalesced. */
#define COOP_INPUT_PENDING_CAPACITY 128u
typedef struct {
    coop_input_msg_t message;
    bool coalescible;
} coord_pending_entry_t;
static coord_pending_entry_t coord_pending[COOP_INPUT_PENDING_CAPACITY];
static size_t coord_pending_count = 0;
/* A controller packet is complete state, but changing a button bit is still an
 * edge. Track the last submitted bitset so only axes/trigger-only updates are
 * eligible to supersede one another. */
static uint16_t coord_last_buttons_lo[sizeof(uint16_t) * CHAR_BIT];
static uint16_t coord_last_buttons_hi[sizeof(uint16_t) * CHAR_BIT];
static uint16_t coord_last_controller_mask[sizeof(uint16_t) * CHAR_BIT];
static bool coord_last_buttons_valid[sizeof(uint16_t) * CHAR_BIT];

/* Secondary state */
static intptr_t sec_fd = -1;
static atomic_bool sec_run = ATOMIC_VAR_INIT(false);
static bool sec_callbacks_enabled = false;
static bool sec_thread_started = false;
static pthread_t sec_thread;
static coop_msg_channel_t sec_channel = COOP_MSG_CHANNEL_INITIALIZER;
static pthread_mutex_t sec_state_lock = PTHREAD_MUTEX_INITIALIZER;

static unsigned stat_sent = 0, stat_dropped = 0, stat_rumbles = 0;

static void coop_input_log(const char *fmt, ...) {
    FILE *f = fopen(AURORA_DIAG_LOG, "a");
    if (f == NULL) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static bool fill_addr(struct sockaddr_un *addr) {
    memset(addr, 0, sizeof(*addr));
    addr->sun_family = AF_UNIX;
#ifdef ECLIPSE_RUNTIME_ACTIVE
    return eclipse_runtime_socket_path(COOP_INPUT_SOCKET, addr->sun_path, sizeof(addr->sun_path)) == 0;
#else
    if (strlen(COOP_INPUT_SOCKET) >= sizeof(addr->sun_path)) {
        return false;
    }
    strcpy(addr->sun_path, COOP_INPUT_SOCKET);
    return true;
#endif
}

/* ── routing ──────────────────────────────────────────────────────────────── */

int coop_input_route(int gs_id) {
    /* Outside co-op every pad is ours, so the ordinary single-host path never
     * consults the config and never changes behaviour. */
    if (!app_launch_coop_requested() || app_configuration == NULL) {
        return COOP_PAD_PRIMARY;
    }
    if (gs_id < 0 || gs_id >= 4) {
        return COOP_PAD_PRIMARY;
    }
    const int v = app_configuration->coop_pad_pane[gs_id];
    return (v >= COOP_PAD_PRIMARY && v <= COOP_PAD_BOTH) ? v : COOP_PAD_PRIMARY;
}

bool coop_input_wants_primary(int gs_id) {
    const int r = coop_input_route(gs_id);
    return r == COOP_PAD_PRIMARY || r == COOP_PAD_BOTH;
}

static bool wants_peer(int gs_id) {
    const int r = coop_input_route(gs_id);
    return r == COOP_PAD_SECONDARY || r == COOP_PAD_BOTH;
}

static uint16_t routed_mask(bool primary) {
    uint16_t mask = 0;
    for (int source = 0; source < 16; ++source) {
        const bool included = primary ? coop_input_wants_primary(source) : wants_peer(source);
        if (included) {
            mask |= (uint16_t) (1u << source);
        }
    }
    return mask;
}

int coop_input_primary_slot(int gs_id) {
    return coop_gamepad_compact_slot(routed_mask(true), gs_id);
}

int coop_input_primary_source_slot(int primary_slot) {
    return coop_gamepad_source_slot(routed_mask(true), primary_slot);
}

int coop_input_peer_slot(int gs_id) {
    /* 0-based index AMONG THE PADS ROUTED TO THE PEER, so that host sees a normal
     * 1-based controller set rather than a gap where its player 1 should be. */
    return coop_gamepad_compact_slot(routed_mask(false), gs_id);
}

uint16_t coop_input_primary_mask(uint16_t active_mask) {
    return coop_gamepad_compact_mask(routed_mask(true), active_mask);
}

uint16_t coop_input_peer_mask(uint16_t active_mask) {
    return coop_gamepad_compact_mask(routed_mask(false), active_mask);
}

static void coord_reset_pending_locked(void) {
    coord_pending_count = 0;
    memset(coord_last_buttons_valid, 0, sizeof(coord_last_buttons_valid));
}

/* ── coordinator ──────────────────────────────────────────────────────────── */

void coop_input_coord_start(app_input_t *input) {
    if (!app_launch_coop_requested() || coop_secondary_mode() != NULL) {
        return;
    }

    pthread_mutex_lock(&coord_state_lock);
    if (coord_active) {
        pthread_mutex_unlock(&coord_state_lock);
        return;
    }
    struct sockaddr_un addr;
    if (!fill_addr(&addr)) {
        pthread_mutex_unlock(&coord_state_lock);
        return;
    }
    unlink(COOP_INPUT_SOCKET);
    intptr_t fd = coop_msg_socket();
    if (fd < 0) {
        pthread_mutex_unlock(&coord_state_lock);
        return;
    }
    if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) != 0 || listen(fd, 1) != 0) {
        close(fd);
        unlink(COOP_INPUT_SOCKET);
        pthread_mutex_unlock(&coord_state_lock);
        return;
    }
    if (!coop_msg_set_nonblocking(fd)) {
        close(fd);
        unlink(COOP_INPUT_SOCKET);
        pthread_mutex_unlock(&coord_state_lock);
        return;
    }
    coord_listen_fd = fd;
    coord_input = input;
    coord_active = true;
    coord_reset_pending_locked();
    stat_sent = stat_dropped = stat_rumbles = 0;
    coord_pointer_stop_pending = false;
    coord_pointer_start_pending = true;
    pthread_mutex_unlock(&coord_state_lock);
    coop_input_log("[COOP-INPUT] coordinator listening on %s", COOP_INPUT_SOCKET);
}

void coop_input_coord_stop(void) {
    pthread_mutex_lock(&coord_state_lock);
    /* Disable new sends before taking ownership of the descriptors. */
    coord_active = false;
    coord_pointer_start_pending = false;
    coord_pointer_stop_pending = true;

    const intptr_t peer_fd = coord_peer_fd;
    coord_peer_fd = -1;
    if (peer_fd >= 0) {
        /* State lock -> channel lock is the sole nesting order in this file. */
        coop_msg_channel_detach(&coord_channel);
    }
    const intptr_t listen_fd = coord_listen_fd;
    coord_listen_fd = -1;
    coord_input = NULL;
    coord_reset_pending_locked();

    /* There is no coordinator worker to join. Once detached under the lifetime
     * lock, each descriptor is closed exactly once and cannot be reused by a
     * concurrent pump/send before close. */
    if (peer_fd >= 0) {
        close(peer_fd);
    }
    if (listen_fd >= 0) {
        close(listen_fd);
    }
    unlink(COOP_INPUT_SOCKET);
    pthread_mutex_unlock(&coord_state_lock);
}

bool coop_input_coord_active(void) {
    pthread_mutex_lock(&coord_state_lock);
    const bool active = coord_active;
    pthread_mutex_unlock(&coord_state_lock);
    return active;
}

/** Drop the attached peer. coord_state_lock must be held. */
static void coord_disconnect_locked(void) {
    if (coord_peer_fd >= 0) {
        const intptr_t fd = coord_peer_fd;
        coord_peer_fd = -1;
        coop_msg_channel_detach(&coord_channel);
        coord_reset_pending_locked();
        close(fd);
        coop_input_log("[COOP-INPUT] secondary disconnected");
    }
}

/** Accept the secondary if it is waiting. coord_state_lock must be held. */
static void coord_accept_locked(void) {
    if (!coord_active || coord_peer_fd >= 0 || coord_listen_fd < 0) {
        return;
    }
    intptr_t fd = accept(coord_listen_fd, NULL, NULL);
    if (fd < 0) {
        return;
    }
    if (!coop_msg_set_nonblocking(fd) ||
        !coop_msg_channel_attach(&coord_channel, fd, sizeof(coop_input_msg_t))) {
        close(fd);
        return;
    }
    coord_peer_fd = fd;
    coord_reset_pending_locked();
    coop_input_log("[COOP-INPUT] secondary connected");
}

/* ACCEPT BEFORE ANSWERING, or the pointer can never reach the peer at all.
 *
 * coord_peer_fd is only ever set by coord_accept_locked(), reached from this gate,
 * coord_send(), and the UI pump. The pointer path
 * (coop_pointer_position/button/scroll) checks this function BEFORE it would call
 * coord_send(), so a plain "is the fd set" answer is circular: routing needs an
 * attached peer, and the routing gate itself must be able to attach it. The result
 * used to be a pointer permanently stuck on the primary unless an unrelated pad event
 * happened to accept the secondary first.
 *
 * accept() here is non-blocking and coord_accept_locked() no-ops once attached, so this
 * costs one guarded syscall per event while the secondary is still connecting and nothing
 * afterwards. tests/coop/test_coop_pointer.c STUBS this function, which is exactly why
 * case 7 passed while the live path deadlocked -- the stub replaced the interaction that
 * was broken. */
bool coop_input_peer_attached(void) {
    pthread_mutex_lock(&coord_state_lock);
    coord_accept_locked();
    const bool attached = coord_active && coord_peer_fd >= 0;
    pthread_mutex_unlock(&coord_state_lock);
    return attached;
}

/** Classify and remember one submitted message. coord_state_lock must be held. */
static bool coord_msg_coalescible_locked(const coop_input_msg_t *m) {
    const size_t slot_count = sizeof(coord_last_buttons_valid) /
                              sizeof(coord_last_buttons_valid[0]);
    if (m->type == COOP_INPUT_MSG_CONTROLLER) {
        if ((size_t) m->slot >= slot_count) {
            return false;
        }
        const bool same_buttons = coord_last_buttons_valid[m->slot] &&
                                  coord_last_buttons_lo[m->slot] == m->buttons_lo &&
                                  coord_last_buttons_hi[m->slot] == m->buttons_hi &&
                                  coord_last_controller_mask[m->slot] == m->mask;
        coord_last_buttons_lo[m->slot] = m->buttons_lo;
        coord_last_buttons_hi[m->slot] = m->buttons_hi;
        coord_last_controller_mask[m->slot] = m->mask;
        coord_last_buttons_valid[m->slot] = true;
        return same_buttons;
    }
    if ((m->type == COOP_INPUT_MSG_ARRIVE || m->type == COOP_INPUT_MSG_REMOVE) &&
        (size_t) m->slot < slot_count) {
        coord_last_buttons_valid[m->slot] = false;
    }
    return m->type == COOP_INPUT_MSG_MOUSE_POS ||
           m->type == COOP_INPUT_MSG_SENSOR;
}

static bool coord_same_state(const coop_input_msg_t *a, const coop_input_msg_t *b) {
    if (a->type != b->type) {
        return false;
    }
    if (a->type == COOP_INPUT_MSG_MOUSE_POS) {
        return true;
    }
    if (a->type == COOP_INPUT_MSG_SENSOR) {
        return a->slot == b->slot && a->sub == b->sub;
    }
    return a->slot == b->slot;
}

/* Do not move a state sample across a same-device lifecycle edge, or a pointer
 * position across a click/scroll generated at the earlier position. */
static bool coord_state_barrier(const coop_input_msg_t *state,
                                const coord_pending_entry_t *queued) {
    if (state->type == COOP_INPUT_MSG_MOUSE_POS) {
        return queued->message.type == COOP_INPUT_MSG_MOUSE_BUTTON ||
               queued->message.type == COOP_INPUT_MSG_MOUSE_SCROLL ||
               queued->message.type == COOP_INPUT_MSG_KEYBOARD;
    }
    if (state->type == COOP_INPUT_MSG_CONTROLLER && !queued->coalescible &&
        queued->message.type == COOP_INPUT_MSG_CONTROLLER &&
        queued->message.slot == state->slot) {
        return true; /* a button or active-mask edge */
    }
    return (queued->message.type == COOP_INPUT_MSG_ARRIVE ||
            queued->message.type == COOP_INPUT_MSG_REMOVE) &&
           queued->message.slot == state->slot;
}

static void coord_pending_remove_locked(size_t index) {
    if (index + 1 < coord_pending_count) {
        memmove(&coord_pending[index], &coord_pending[index + 1],
                (coord_pending_count - index - 1) * sizeof(coord_pending[0]));
    }
    coord_pending_count--;
}

/** Retain an event for POLLOUT. coord_state_lock must be held. */
static bool coord_pending_push_locked(const coop_input_msg_t *m, bool coalescible) {
    if (coalescible) {
        /* Replace the newest equivalent full-state sample, but never cross an
         * ordering edge for the same input domain. Controller packets only reach
         * this branch when their button bits and active mask did not change. */
        for (size_t i = coord_pending_count; i > 0; --i) {
            coord_pending_entry_t *queued = &coord_pending[i - 1];
            if (coord_state_barrier(m, queued)) {
                break;
            }
            if (queued->coalescible && coord_same_state(m, &queued->message)) {
                queued->message = *m;
                stat_dropped++; /* superseded intentionally rather than lost */
                return true;
            }
        }
    }

    if (coord_pending_count == COOP_INPUT_PENDING_CAPACITY) {
        if (!coalescible) {
            /* Preserve the transition by sacrificing the oldest replaceable
             * sample. Transitions never merge with or overwrite one another. */
            size_t victim = 0;
            while (victim < coord_pending_count &&
                   !coord_pending[victim].coalescible) {
                victim++;
            }
            if (victim < coord_pending_count) {
                coord_pending_remove_locked(victim);
                stat_dropped++;
            }
        } else {
            /* Keep the newest replaceable state when no exact coalescing
             * candidate existed. */
            size_t victim = 0;
            while (victim < coord_pending_count &&
                   !coord_pending[victim].coalescible) {
                victim++;
            }
            if (victim < coord_pending_count) {
                coord_pending_remove_locked(victim);
                stat_dropped++;
            }
        }
        if (coord_pending_count == COOP_INPUT_PENDING_CAPACITY) {
            stat_dropped++;
            return false;
        }
    }

    coord_pending[coord_pending_count++] = (coord_pending_entry_t) {
        .message = *m,
        .coalescible = coalescible,
    };
    return true;
}

/* Rumble arrives on the same socket, travelling the other way. Drained here
 * from the UI pump; coord_state_lock must be held. */
static void coord_drain_rumble_locked(void) {
    if (coord_peer_fd < 0) {
        return;
    }
    coop_input_msg_t m;
    for (int i = 0; i < 8; ++i) {
        size_t n = 0;
        int result = coop_msg_channel_receive(&coord_channel, &m, sizeof(m), &n);
        if (result == COOP_MSG_RECV_IDLE) {
            return;
        }
        if (result == COOP_MSG_RECV_TOO_LARGE) {
            stat_dropped++;
            continue;
        }
        if (result == COOP_MSG_RECV_ERROR) {
            coord_disconnect_locked();
            return;
        }
        if (n != sizeof(m) || m.magic != COOP_INPUT_MAGIC) {
            continue;
        }
        /* The peer names its own compact slot; map it back to the physical pad. */
        int gs_id = coop_gamepad_source_slot(routed_mask(false), (int) m.slot);
        if (gs_id < 0) {
            continue;
        }
        if (m.type == COOP_INPUT_MSG_RUMBLE) {
            stat_rumbles++;
            if (coord_input != NULL) {
                app_input_gamepad_rumble(coord_input, (unsigned short) gs_id,
                                         m.motor_lo, m.motor_hi);
            }
        } else if (m.type == COOP_INPUT_MSG_RUMBLE_TRIGGERS) {
            stat_rumbles++;
            if (coord_input != NULL) {
                app_input_gamepad_rumble_triggers(coord_input, (unsigned short) gs_id,
                                                  m.motor_lo, m.motor_hi);
            }
        }
    }
}

/** Move queued messages into the channel while writable. Lock must be held. */
static void coord_flush_pending_locked(void) {
    if (coord_peer_fd < 0) {
        return;
    }

    int result = coop_msg_channel_flush(&coord_channel);
    if (result == COOP_MSG_ERROR) {
        coord_disconnect_locked();
        return;
    }
    if (result == COOP_MSG_WOULD_BLOCK) {
        return;
    }

    while (coord_pending_count > 0 && coord_peer_fd >= 0) {
        result = coop_msg_channel_send(&coord_channel, &coord_pending[0].message,
                                       sizeof(coord_pending[0].message));
        if (result == COOP_MSG_ACCEPTED) {
            coord_pending_remove_locked(0);
            stat_sent++;
            continue;
        }
        if (result == COOP_MSG_ERROR) {
            coord_disconnect_locked();
        }
        return;
    }
}

void coop_input_coord_pump(void) {
    pthread_mutex_lock(&coord_state_lock);
    /* The session worker publishes pointer lifecycle changes, but only this UI
     * pump mutates coop_pointer.c's state. That keeps its split/held/pane fields
     * on the same thread as every pointer callback. */
    if (coord_pointer_stop_pending) {
        coord_pointer_stop_pending = false;
        coop_pointer_stop();
    }
    if (coord_pointer_start_pending && coord_active) {
        coord_pointer_start_pending = false;
        coop_pointer_start();
    }
    if (!coord_active) {
        pthread_mutex_unlock(&coord_state_lock);
        return;
    }

    coord_accept_locked();
    if (coord_peer_fd >= 0) {
        short events = POLLIN;
        if (coord_pending_count > 0 || coop_msg_channel_has_pending(&coord_channel)) {
            events |= POLLOUT;
        }
        struct pollfd pfd = {.fd = coord_peer_fd, .events = events, .revents = 0};
        int ready;
        do {
            ready = poll(&pfd, 1, 0);
        } while (ready < 0 && errno == EINTR);
        if (ready < 0 || (ready > 0 &&
                          (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)) {
            coord_disconnect_locked();
        } else if (ready > 0) {
            if ((pfd.revents & POLLIN) != 0) {
                coord_drain_rumble_locked();
            }
            if (coord_peer_fd >= 0 && (pfd.revents & POLLOUT) != 0) {
                coord_flush_pending_locked();
            }
        }
    }
    pthread_mutex_unlock(&coord_state_lock);
}

static void coord_send(const coop_input_msg_t *m) {
    pthread_mutex_lock(&coord_state_lock);
    if (!coord_active) {
        pthread_mutex_unlock(&coord_state_lock);
        return;
    }
    coord_accept_locked();
    if (coord_peer_fd < 0) {
        stat_dropped++;
        pthread_mutex_unlock(&coord_state_lock);
        return;
    }
    const bool coalescible = coord_msg_coalescible_locked(m);

    /* Never overtake an older queued transition. Full-state messages can be
     * coalesced in the bounded queue; edge events retain FIFO order. */
    if (coord_pending_count > 0) {
        (void) coord_pending_push_locked(m, coalescible);
        pthread_mutex_unlock(&coord_state_lock);
        return;
    }

    const int result = coop_msg_channel_send(&coord_channel, m, sizeof(*m));
    if (result == COOP_MSG_ACCEPTED) {
        stat_sent++;
    } else if (result == COOP_MSG_WOULD_BLOCK) {
        (void) coord_pending_push_locked(m, coalescible);
    } else {
        stat_dropped++;
        coord_disconnect_locked();
    }
    pthread_mutex_unlock(&coord_state_lock);
}

static void msg_init(coop_input_msg_t *m, uint8_t type, int gs_id, uint16_t active_mask) {
    memset(m, 0, sizeof(*m));
    m->magic = COOP_INPUT_MAGIC;
    m->type = type;
    m->slot = (uint8_t) coop_input_peer_slot(gs_id);
    m->mask = coop_input_peer_mask(active_mask);
}

void coop_input_send_controller(int gs_id, uint16_t active_mask,
                                short buttons, unsigned char left_trigger,
                                unsigned char right_trigger, short lx, short ly,
                                short rx, short ry) {
    if (!wants_peer(gs_id)) {
        return;
    }
    coop_input_msg_t m;
    msg_init(&m, COOP_INPUT_MSG_CONTROLLER, gs_id, active_mask);
    m.buttons_lo = (uint16_t) ((unsigned) buttons & 0xFFFFu);
    m.left_trigger = left_trigger;
    m.right_trigger = right_trigger;
    m.lx = lx; m.ly = ly; m.rx = rx; m.ry = ry;
    coord_send(&m);
}

void coop_input_send_arrive(int gs_id, uint16_t active_mask,
                            uint8_t type, uint16_t capabilities,
                            uint32_t supported_buttons) {
    if (!wants_peer(gs_id)) {
        return;
    }
    coop_input_msg_t m;
    msg_init(&m, COOP_INPUT_MSG_ARRIVE, gs_id, active_mask);
    m.pad_type = type;
    m.capabilities = capabilities;
    m.supported_buttons = supported_buttons;
    coord_send(&m);
}

void coop_input_send_remove(int gs_id, uint16_t active_mask) {
    if (!wants_peer(gs_id)) {
        return;
    }
    coop_input_msg_t m;
    msg_init(&m, COOP_INPUT_MSG_REMOVE, gs_id, active_mask);
    coord_send(&m);
}

void coop_input_send_sensor(int gs_id, uint16_t active_mask,
                            uint8_t sensor_type, uint32_t timestamp,
                            float x, float y, float z) {
    if (!wants_peer(gs_id)) {
        return;
    }
    coop_input_msg_t m;
    msg_init(&m, COOP_INPUT_MSG_SENSOR, gs_id, active_mask);
    m.sub = sensor_type;
    m.timestamp = timestamp;
    m.f0 = x; m.f1 = y; m.f2 = z;
    coord_send(&m);
}

void coop_input_send_touchpad(int gs_id, uint16_t active_mask,
                              uint8_t event_type, uint32_t pointer_id,
                              float x, float y, float pressure) {
    if (!wants_peer(gs_id)) {
        return;
    }
    coop_input_msg_t m;
    msg_init(&m, COOP_INPUT_MSG_TOUCHPAD, gs_id, active_mask);
    m.sub = event_type;
    m.pointer_id = (uint8_t) pointer_id;
    m.f0 = x; m.f1 = y; m.f2 = pressure;
    coord_send(&m);
}

/* The pointer has no gs_id: it belongs to whichever pane it is over, so slot and mask
 * mean nothing here and stay zero. */
static void mouse_msg_init(coop_input_msg_t *m, uint8_t type) {
    memset(m, 0, sizeof(*m));
    m->magic = COOP_INPUT_MAGIC;
    m->type = type;
}

void coop_input_send_mouse_position(int x, int y, int ref_w, int ref_h) {
    coop_input_msg_t m;
    mouse_msg_init(&m, COOP_INPUT_MSG_MOUSE_POS);
    m.lx = (int16_t) x;
    m.ly = (int16_t) y;
    m.rx = (int16_t) ref_w;
    m.ry = (int16_t) ref_h;
    coord_send(&m);
}

void coop_input_send_mouse_button(int action, int button) {
    coop_input_msg_t m;
    mouse_msg_init(&m, COOP_INPUT_MSG_MOUSE_BUTTON);
    m.sub = (uint8_t) action;
    m.buttons_lo = (uint16_t) button;
    coord_send(&m);
}

void coop_input_send_mouse_scroll(int vertical, int horizontal) {
    if (vertical == 0 && horizontal == 0) {
        return;
    }
    coop_input_msg_t m;
    mouse_msg_init(&m, COOP_INPUT_MSG_MOUSE_SCROLL);
    m.lx = (int16_t) vertical;
    m.ly = (int16_t) horizontal;
    coord_send(&m);
}

void coop_input_send_keyboard(short key_code, char action, char modifiers) {
    coop_input_msg_t m;
    mouse_msg_init(&m, COOP_INPUT_MSG_KEYBOARD);   /* no slot or mask: a key belongs to a pane */
    m.lx = key_code;
    m.sub = (uint8_t) action;
    m.pad_type = (uint8_t) modifiers;
    coord_send(&m);
}

unsigned coop_input_sent(void) {
    pthread_mutex_lock(&coord_state_lock);
    const unsigned value = stat_sent;
    pthread_mutex_unlock(&coord_state_lock);
    return value;
}

unsigned coop_input_dropped(void) {
    pthread_mutex_lock(&coord_state_lock);
    const unsigned value = stat_dropped;
    pthread_mutex_unlock(&coord_state_lock);
    return value;
}

unsigned coop_input_rumbles(void) {
    pthread_mutex_lock(&coord_state_lock);
    const unsigned value = stat_rumbles;
    pthread_mutex_unlock(&coord_state_lock);
    return value;
}

/* ── secondary ────────────────────────────────────────────────────────────── */

static void secondary_apply(const coop_input_msg_t *m) {
    switch (m->type) {
        case COOP_INPUT_MSG_CONTROLLER:
            LiSendMultiControllerEvent(m->slot, m->mask, (short) m->buttons_lo,
                                       m->left_trigger, m->right_trigger,
                                       m->lx, m->ly, m->rx, m->ry);
            break;
        case COOP_INPUT_MSG_ARRIVE:
            LiSendControllerArrivalEvent(m->slot, m->mask, m->pad_type,
                                         m->supported_buttons, m->capabilities);
            break;
        case COOP_INPUT_MSG_REMOVE:
            LiSendMultiControllerEvent(m->slot, m->mask, 0, 0, 0, 0, 0, 0, 0);
            break;
        case COOP_INPUT_MSG_SENSOR:
            LiSendControllerMotionEvent(m->slot, m->sub, m->f0, m->f1, m->f2);
            break;
        case COOP_INPUT_MSG_TOUCHPAD:
            LiSendControllerTouchEvent(m->slot, m->sub, m->pointer_id,
                                       m->f0, m->f1, m->f2);
            break;
        /* Already pane-local: the reference size travelled with the position, so this
         * side does no geometry at all and cannot disagree with the coordinator. */
        case COOP_INPUT_MSG_MOUSE_POS:
            LiSendMousePositionEvent(m->lx, m->ly, m->rx, m->ry);
            break;
        case COOP_INPUT_MSG_MOUSE_BUTTON:
            LiSendMouseButtonEvent((char) m->sub, (int) m->buttons_lo);
            break;
        case COOP_INPUT_MSG_KEYBOARD:
            LiSendKeyboardEvent(m->lx, (char) m->sub, (char) m->pad_type);
            break;
        case COOP_INPUT_MSG_MOUSE_SCROLL:
            if (m->lx != 0) {
                LiSendScrollEvent((signed char) m->lx);
            }
            if (m->ly != 0) {
                LiSendHScrollEvent((signed char) m->ly);
            }
            break;
        default:
            break;
    }
}

static void *secondary_worker(void *arg) {
    const intptr_t worker_fd = (intptr_t) (uintptr_t) arg;
    coop_input_msg_t m;
    while (atomic_load_explicit(&sec_run, memory_order_acquire)) {
        size_t n = 0;
        int result = coop_msg_channel_receive(&sec_channel, &m, sizeof(m), &n);
        if (result == COOP_MSG_RECV_ERROR) {
            break;
        }
        if (result == COOP_MSG_RECV_MESSAGE && n == sizeof(m) &&
            m.magic == COOP_INPUT_MAGIC) {
            if (atomic_load_explicit(&sec_run, memory_order_acquire)) {
                secondary_apply(&m);
            }
            continue;
        }
        if (result == COOP_MSG_RECV_TOO_LARGE) {
            continue;
        }

        /* Both transports are deliberately non-blocking. Sleep in poll rather
         * than spinning while a stream frame is still arriving. POLLOUT also
         * gives a partially-written rumble a chance to finish even if no later
         * rumble event happens to flush it. */
        short events = POLLIN;
        if (coop_msg_channel_has_pending(&sec_channel)) {
            events |= POLLOUT;
        }
        struct pollfd pfd = {.fd = worker_fd, .events = events, .revents = 0};
        int ready;
        do {
            ready = poll(&pfd, 1, 100);
        } while (ready < 0 && errno == EINTR &&
                 atomic_load_explicit(&sec_run, memory_order_acquire));
        if (ready > 0 && (pfd.revents & POLLOUT) != 0 &&
            coop_msg_channel_flush(&sec_channel) == COOP_MSG_ERROR) {
            break;
        }
        if (ready > 0 && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            break;
        }
    }
    atomic_store_explicit(&sec_run, false, memory_order_release);
    return NULL;
}

bool coop_input_secondary_start(void) {
    pthread_mutex_lock(&sec_state_lock);
    if (sec_fd >= 0) {
        const bool running = atomic_load_explicit(&sec_run, memory_order_acquire);
        pthread_mutex_unlock(&sec_state_lock);
        return running;
    }
    struct sockaddr_un addr;
    if (!fill_addr(&addr)) {
        pthread_mutex_unlock(&sec_state_lock);
        return false;
    }
    intptr_t fd = coop_msg_socket();
    if (fd < 0) {
        pthread_mutex_unlock(&sec_state_lock);
        return false;
    }
    if (connect(fd, (struct sockaddr *) &addr, sizeof(addr)) != 0) {
        close(fd);
        pthread_mutex_unlock(&sec_state_lock);
        return false;
    }
    if (!coop_msg_set_nonblocking(fd) ||
        !coop_msg_channel_attach(&sec_channel, fd, sizeof(coop_input_msg_t))) {
        close(fd);
        pthread_mutex_unlock(&sec_state_lock);
        return false;
    }
    sec_fd = fd;
    atomic_store_explicit(&sec_run, true, memory_order_release);
    sec_callbacks_enabled = true;
    if (pthread_create(&sec_thread, NULL, secondary_worker,
                       (void *) (uintptr_t) fd) != 0) {
        sec_callbacks_enabled = false;
        atomic_store_explicit(&sec_run, false, memory_order_release);
        coop_msg_channel_detach(&sec_channel);
        close(fd);
        sec_fd = -1;
        pthread_mutex_unlock(&sec_state_lock);
        return false;
    }
    sec_thread_started = true;
    pthread_mutex_unlock(&sec_state_lock);
    coop_input_log("[COOP-INPUT] secondary: input link up");
    return true;
}

void coop_input_secondary_stop(void) {
    pthread_mutex_lock(&sec_state_lock);
    if (sec_fd < 0) {
        pthread_mutex_unlock(&sec_state_lock);
        return;
    }

    /* Connection callbacks may still arrive until LiStopConnection(). Disable
     * them first, then wake and join the receive worker before detaching. */
    sec_callbacks_enabled = false;
    atomic_store_explicit(&sec_run, false, memory_order_release);
    const intptr_t fd = sec_fd;
    shutdown(fd, SHUT_RDWR);
    if (sec_thread_started) {
        pthread_join(sec_thread, NULL);
        sec_thread_started = false;
    }
    coop_msg_channel_detach(&sec_channel);
    sec_fd = -1;
    close(fd);
    pthread_mutex_unlock(&sec_state_lock);
}

static void secondary_send_rumble(uint8_t type, uint16_t controller, uint16_t a, uint16_t b) {
    coop_input_msg_t m;
    memset(&m, 0, sizeof(m));
    m.magic = COOP_INPUT_MAGIC;
    m.type = type;
    m.slot = (uint8_t) controller;
    m.motor_lo = a;
    m.motor_hi = b;

    pthread_mutex_lock(&sec_state_lock);
    if (sec_callbacks_enabled && sec_fd >= 0 &&
        atomic_load_explicit(&sec_run, memory_order_acquire)) {
        const int result = coop_msg_channel_send(&sec_channel, &m, sizeof(m));
        if (result == COOP_MSG_ERROR) {
            /* Leave ownership with secondary_stop(), but stop all further
             * callbacks and wake the worker immediately. */
            sec_callbacks_enabled = false;
            atomic_store_explicit(&sec_run, false, memory_order_release);
            shutdown(sec_fd, SHUT_RDWR);
        }
    }
    pthread_mutex_unlock(&sec_state_lock);
}

void coop_input_secondary_rumble(uint16_t controller, uint16_t low, uint16_t high) {
    secondary_send_rumble(COOP_INPUT_MSG_RUMBLE, controller, low, high);
}

void coop_input_secondary_rumble_triggers(uint16_t controller, uint16_t left, uint16_t right) {
    secondary_send_rumble(COOP_INPUT_MSG_RUMBLE_TRIGGERS, controller, left, right);
}
