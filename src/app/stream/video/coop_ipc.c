#include "coop_ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static bool fill_addr(struct sockaddr_un *addr, const char *path) {
#ifdef ECLIPSE_RUNTIME_ACTIVE
    memset(addr, 0, sizeof(*addr));
    addr->sun_family = AF_UNIX;
    return eclipse_runtime_socket_path(path, addr->sun_path, sizeof(addr->sun_path)) == 0;
#else
    if (strlen(path) >= sizeof(addr->sun_path)) {
        return false;
    }
    memset(addr, 0, sizeof(*addr));
    addr->sun_family = AF_UNIX;
    strncpy(addr->sun_path, path, sizeof(addr->sun_path) - 1);
    return true;
#endif
}

intptr_t coop_ipc_listen(const char *path) {
    struct sockaddr_un addr;
    if (!fill_addr(&addr, path)) {
        return -1;
    }
    unlink(path);   /* a stale socket file would make bind fail */
    intptr_t fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, 1) != 0) {
        close(fd);
        unlink(path);
        return -1;
    }
    return fd;
}

intptr_t coop_ipc_accept(intptr_t listen_fd) {
    intptr_t fd;
    do {
        fd = accept(listen_fd, NULL, NULL);
    } while (fd < 0 && errno == EINTR);
    return fd;
}

intptr_t coop_ipc_connect(const char *path) {
    struct sockaddr_un addr;
    if (!fill_addr(&addr, path)) {
        return -1;
    }
    intptr_t fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    if (connect(fd, (struct sockaddr *) &addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

bool coop_ipc_set_nonblocking(intptr_t fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

/**
 * Write exactly @p len bytes, retrying short writes.
 *
 * WAITS ON poll(), NEVER ON A SPIN. The socket is non-blocking and a partial access unit
 * cannot be abandoned -- doing so desynchronises the framing for the rest of the session --
 * so this has to wait for the coordinator to drain. It used to do that by calling write() in
 * a tight loop on EAGAIN, which pins a core on the SECONDARY'S VIDEO RECEIVE THREAD for as
 * long as the drain is behind. That is the opposite of what coop_secondary_init() sets the
 * socket non-blocking FOR ("a stalled coordinator must never back-pressure this session's
 * network thread"): a spinning thread does not drop a pane, it starves the thread that was
 * meant to keep receiving. Both Eclipse processes share the TV's CPU, so it starves the
 * primary's control stream too -- measured 2026-08-01, a session ending at 3m26s with
 * "Control stream received unexpected disconnect event" while the host logged only
 * "CLIENT DISCONNECTED", and 13 IPC link drops in the 40s before it.
 *
 * poll() waits for exactly the same condition and costs nothing while it does.  The absolute
 * whole-message deadline below turns a peer that never resumes into a link reset instead of a
 * permanently occupied video callback.
 */
static uint32_t mono_ms32(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t) ((uint64_t) ts.tv_sec * 1000ull + (uint64_t) (ts.tv_nsec / 1000000ull));
}

static bool io_deadline_expired(uint32_t started_ms) {
    return mono_ms32() - started_ms >= COOP_IPC_IO_DEADLINE_MS;
}

static int io_poll_timeout_ms(uint32_t started_ms) {
    const uint32_t elapsed = mono_ms32() - started_ms;
    if (elapsed >= COOP_IPC_IO_DEADLINE_MS) {
        return 0;
    }
    const uint32_t remaining = COOP_IPC_IO_DEADLINE_MS - elapsed;
    return (int) (remaining < 100u ? remaining : 100u);
}

static bool write_all(intptr_t fd, const unsigned char *p, size_t len,
                      uint32_t message_started_ms) {
    while (len > 0) {
        if (io_deadline_expired(message_started_ms)) {
            errno = ETIMEDOUT;
            return false;
        }
        ssize_t n = write(fd, p, len);
        if (n > 0) {
            p += n;
            len -= (size_t) n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pfd = {.fd = fd, .events = POLLOUT, .revents = 0};
            int r = poll(&pfd, 1, io_poll_timeout_ms(message_started_ms));
            if (r < 0 && errno != EINTR) {
                return false;
            }
            if (r > 0 && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                errno = (pfd.revents & POLLNVAL) != 0 ? EBADF : EPIPE;
                return false;
            }
            continue;
        }
        if (n == 0) {
            errno = EPIPE;
        }
        return false;
    }
    return true;
}

/* HOW LONG THE PREVIOUS SEND BLOCKED. Passover 07 §2C identified that write_all() retries a
 * non-blocking socket under poll(), so a delayed writer could sit outside the printed latency
 * metrics. This is that measurement, and it is deliberately
 * carried on the NEXT message rather than reported locally: the secondary has no log the
 * coordinator reads per-picture, and adding one would put a file write on the receive thread
 * this whole path exists to keep unblocked. Cost is one AU of reporting lag. */
static uint32_t coop_ipc_prev_send_ms = 0;
uint32_t coop_ipc_last_send_ms(void) { return coop_ipc_prev_send_ms; }

bool coop_ipc_send_au(intptr_t fd, uint32_t frame, bool is_idr,
                      const unsigned char *data, size_t len,
                      const coop_ipc_meta_t *meta) {
    if (len > UINT32_MAX || (len > 0 && data == NULL)) {
        errno = len > UINT32_MAX ? EMSGSIZE : EINVAL;
        return false;
    }
    const uint32_t enter = mono_ms32();
    coop_ipc_hdr_t hdr = {
            .magic = COOP_IPC_MAGIC,
            .length = (uint32_t) len,
            .frame = frame,
            .is_idr = is_idr ? 1u : 0u,
            .flags = meta != NULL ? meta->flags : 0u,
            .host_10us = meta != NULL ? meta->host_10us : 0u,
            .pts_ms = meta != NULL ? meta->pts_ms : 0u,
            .recv_ms = meta != NULL ? meta->recv_ms : 0u,
            .reasm_ms = meta != NULL ? meta->reasm_ms : 0u,
            .send_enter_mono_ms = enter,
            .prev_send_ms = coop_ipc_prev_send_ms,
    };
    const bool ok = write_all(fd, (const unsigned char *) &hdr, sizeof(hdr), enter) &&
                    write_all(fd, data, len, enter);
    coop_ipc_prev_send_ms = mono_ms32() - enter;
    return ok;
}

bool coop_ipc_send_ctl(intptr_t fd, uint8_t code) {
    return coop_ipc_send_au(fd, COOP_IPC_CTL_FRAME, false, &code, 1, NULL);
}

/**
 * Read exactly @p len bytes. Returns 1 on success, 0 if nothing has arrived yet
 * and nothing was consumed, -1 on error/EOF. Once a partial read has happened we
 * must block for the remainder, otherwise the framing desynchronises.
 *
 * @p idle_ok says whether "nothing pending" is a legitimate answer AT THIS CALL SITE, and
 * getting that wrong was a real bug (fixed 2026-08-03). The `got == 0` test is per CALL, but
 * the framing invariant is per MESSAGE: once the header has been consumed the payload MUST be
 * read, so a body read that finds no bytes yet is NOT idle -- it is mid-message. It used to
 * return 0 there, the caller checked `!= 1` and turned that into -1, and the coordinator
 * closed the socket. A header and its payload arriving in separate reads is the ordinary case
 * for a stream socket under load, so this tore the IPC link down constantly: measured 21-23
 * teardowns in ONE session with both processes alive, errno=0 and no failing syscall to blame.
 * Each teardown cost a burst of consecutive peer frames (e.g. 700-704), which breaks the
 * peer's prediction chain and corrupts its pane until the next IDR -- i.e. this is a direct
 * cause of the "secondary pane is broken" fault, and it fires ONLY on peer panes because only
 * they cross the IPC link, which is why the fault followed the ROLE and not the machine.
 */
int coop_ipc_last_errno = 0;
const char *coop_ipc_last_stage = "none";

static int read_all(intptr_t fd, unsigned char *p, size_t len, bool idle_ok,
                    uint32_t message_started_ms) {
    size_t got = 0;
    while (got < len) {
        if (io_deadline_expired(message_started_ms)) {
            errno = ETIMEDOUT;
            coop_ipc_last_errno = ETIMEDOUT;
            coop_ipc_last_stage = "message-timeout";
            return -1;
        }
        ssize_t n = read(fd, p + got, len - got);
        if (n > 0) {
            got += (size_t) n;
            continue;
        }
        if (n == 0) {
            /* Distinguish an orderly close BEFORE a message from one that arrives
             * mid-message: the latter means the peer died holding a partial write, the
             * former that it closed cleanly between units. They have different causes. */
            coop_ipc_last_errno = 0;
            coop_ipc_last_stage = (got == 0) ? "eof-idle" : "eof-midmsg";
            return -1;   /* peer closed */
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (got == 0 && idle_ok) {
                return 0;   /* clean "nothing pending", and legitimate here */
            }
            /* Mid-message: the remainder MUST be read or the framing desynchronises. Wait
             * on poll() rather than re-reading in a tight loop -- an access unit is tens of
             * kilobytes and arrives in several chunks, so this path runs on every pane, and
             * spinning it burns the coordinator's core for the whole gap. Same reasoning as
             * write_all() above. */
            struct pollfd pfd = {.fd = fd, .events = POLLIN, .revents = 0};
            int r = poll(&pfd, 1, io_poll_timeout_ms(message_started_ms));
            if (r < 0 && errno != EINTR) {
                coop_ipc_last_errno = errno;
                coop_ipc_last_stage = "poll-error";
                return -1;
            }
            if (r > 0 && (pfd.revents & (POLLERR | POLLNVAL)) != 0) {
                coop_ipc_last_errno = pfd.revents;
                coop_ipc_last_stage = "poll-revents";
                return -1;
            }
            continue;
        }
        coop_ipc_last_errno = errno;
        coop_ipc_last_stage = "read-error";
        return -1;
    }
    return 1;
}

long coop_ipc_recv_au(intptr_t fd, unsigned char *buf, size_t cap,
                      uint32_t *frame_out, bool *is_idr_out) {
    return coop_ipc_recv_au_meta(fd, buf, cap, frame_out, is_idr_out, NULL);
}

long coop_ipc_recv_au_meta(intptr_t fd, unsigned char *buf, size_t cap,
                           uint32_t *frame_out, bool *is_idr_out,
                           coop_ipc_hdr_t *hdr_out) {
    coop_ipc_hdr_t hdr;
    const uint32_t message_started_ms = mono_ms32();
    /* The ONLY call that may legitimately find nothing: between access units. */
    int r = read_all(fd, (unsigned char *) &hdr, sizeof(hdr), true, message_started_ms);
    if (r <= 0) {
        return r;
    }
    if (hdr.magic != COOP_IPC_MAGIC) {
        errno = EBADMSG;
        coop_ipc_last_errno = EBADMSG;
        coop_ipc_last_stage = "bad-magic";
        return -2;   /* framing desync (distinct from an I/O error, so the two
                      * causes can be told apart in the log) */
    }
    if (hdr.length > cap) {
        /* Oversized units must NOT kill the connection. At 4K an IDR exceeded the
         * 4MB cap and this returned -1, which closed the socket and made the
         * secondary exit after ~21 frames -- leaving the composite stuck on a
         * single pane. Drain the payload so framing stays intact and skip just
         * this one access unit. */
        if (cap == 0) {
            errno = EMSGSIZE;
            coop_ipc_last_errno = EMSGSIZE;
            coop_ipc_last_stage = "oversize-no-drain-buffer";
            return -1;
        }
        size_t remaining = hdr.length;
        while (remaining > 0) {
            size_t chunk = remaining < cap ? remaining : cap;
            if (read_all(fd, buf, chunk, false, message_started_ms) != 1) {
                return -1;
            }
            remaining -= chunk;
        }
        return 0;
    }
    if (read_all(fd, buf, hdr.length, false, message_started_ms) != 1) {
        return -1;
    }
    if (frame_out != NULL) {
        *frame_out = hdr.frame;
    }
    if (is_idr_out != NULL) {
        *is_idr_out = hdr.is_idr != 0;
    }
    if (hdr_out != NULL) {
        *hdr_out = hdr;
    }
    return (long) hdr.length;
}
