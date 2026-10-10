/*
 * Small-message transport for the co-op process links.
 *
 * Linux/webOS have AF_UNIX SOCK_SEQPACKET, which already preserves message
 * boundaries and remains the preferred transport. Windows AF_UNIX implements
 * SOCK_STREAM only, so that transport needs explicit framing and persistent
 * partial-I/O state. Callers use the same non-blocking API for both.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <pthread.h>

typedef struct {
    intptr_t fd;
    bool framed;
    size_t max_message;

    unsigned char *tx_buf;
    size_t tx_len;
    size_t tx_off;

    unsigned char *rx_buf;
    size_t rx_have;
    size_t rx_need;

    pthread_mutex_t lock;
} coop_msg_channel_t;

#define COOP_MSG_CHANNEL_INITIALIZER { \
        .fd = -1, .lock = PTHREAD_MUTEX_INITIALIZER \
    }

enum {
    COOP_MSG_ERROR = -1,
    COOP_MSG_WOULD_BLOCK = 0,
    COOP_MSG_ACCEPTED = 1,
};

enum {
    COOP_MSG_RECV_ERROR = -1,
    COOP_MSG_RECV_TOO_LARGE = -2,
    COOP_MSG_RECV_IDLE = 0,
    COOP_MSG_RECV_MESSAGE = 1,
};

/** Create the platform's preferred local message socket. */
intptr_t coop_msg_socket(void);

/** Put a socket in non-blocking mode. */
bool coop_msg_set_nonblocking(intptr_t fd);

/**
 * Attach a connected socket and allocate framing state for messages up to
 * @p max_message bytes. The socket remains owned by the caller.
 */
bool coop_msg_channel_attach(coop_msg_channel_t *channel, intptr_t fd, size_t max_message);

/** Drop buffered state without closing the attached socket. */
void coop_msg_channel_detach(coop_msg_channel_t *channel);

/**
 * Send one complete message without blocking.
 *
 * ACCEPTED means the complete packet was handed to SOCK_SEQPACKET, or that a
 * framed stream message was either written or safely retained for a later
 * flush. WOULD_BLOCK means an older stream message is still pending and this
 * new message was not accepted. ERROR means the link should be closed.
 */
int coop_msg_channel_send(coop_msg_channel_t *channel, const void *message, size_t length);

/** Try to finish a retained stream write without blocking. */
int coop_msg_channel_flush(coop_msg_channel_t *channel);

/** True when a framed stream message still has bytes waiting to be written. */
bool coop_msg_channel_has_pending(coop_msg_channel_t *channel);

/**
 * Receive at most one complete message without blocking. Partial stream data
 * is retained until the rest arrives; it is never exposed to the caller.
 */
int coop_msg_channel_receive(coop_msg_channel_t *channel, void *message,
                             size_t capacity, size_t *length_out);
