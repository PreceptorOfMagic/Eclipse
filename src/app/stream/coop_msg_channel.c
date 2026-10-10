#include "coop_msg_channel.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define COOP_MSG_FRAME_MAGIC 0x434D4731u /* "CMG1" */
#define COOP_MSG_FRAME_BYTES (sizeof(uint32_t) * 2u)

static void channel_clear_locked(coop_msg_channel_t *channel) {
    free(channel->tx_buf);
    free(channel->rx_buf);
    channel->fd = -1;
    channel->framed = false;
    channel->max_message = 0;
    channel->tx_buf = NULL;
    channel->tx_len = 0;
    channel->tx_off = 0;
    channel->rx_buf = NULL;
    channel->rx_have = 0;
    channel->rx_need = 0;
}

intptr_t coop_msg_socket(void) {
#ifdef _WIN32
    return socket(AF_UNIX, SOCK_STREAM, 0);
#else
    return socket(AF_UNIX, SOCK_SEQPACKET, 0);
#endif
}

bool coop_msg_set_nonblocking(intptr_t fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool coop_msg_channel_attach(coop_msg_channel_t *channel, intptr_t fd, size_t max_message) {
    if (channel == NULL || fd < 0 || max_message == 0 ||
        max_message > UINT32_MAX || max_message > SIZE_MAX - COOP_MSG_FRAME_BYTES) {
        errno = EINVAL;
        return false;
    }

    int socket_type = 0;
    socklen_t socket_type_len = sizeof(socket_type);
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &socket_type, &socket_type_len) != 0) {
        return false;
    }

    pthread_mutex_lock(&channel->lock);
    channel_clear_locked(channel);
    channel->fd = fd;
    channel->framed = socket_type == SOCK_STREAM;
    channel->max_message = max_message;

    if (channel->framed) {
        const size_t framed_capacity = COOP_MSG_FRAME_BYTES + max_message;
        channel->tx_buf = malloc(framed_capacity);
        channel->rx_buf = malloc(framed_capacity);
        if (channel->tx_buf == NULL || channel->rx_buf == NULL) {
            channel_clear_locked(channel);
            pthread_mutex_unlock(&channel->lock);
            errno = ENOMEM;
            return false;
        }
        channel->rx_need = COOP_MSG_FRAME_BYTES;
    }
    pthread_mutex_unlock(&channel->lock);
    return true;
}

void coop_msg_channel_detach(coop_msg_channel_t *channel) {
    if (channel == NULL) {
        return;
    }
    pthread_mutex_lock(&channel->lock);
    channel_clear_locked(channel);
    pthread_mutex_unlock(&channel->lock);
}

static int flush_locked(coop_msg_channel_t *channel) {
    while (channel->tx_off < channel->tx_len) {
        ssize_t n = send(channel->fd, channel->tx_buf + channel->tx_off,
                         channel->tx_len - channel->tx_off,
                         MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n > 0) {
            channel->tx_off += (size_t) n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return COOP_MSG_WOULD_BLOCK;
        }
        if (n == 0) {
            errno = EPIPE;
        }
        return COOP_MSG_ERROR;
    }
    channel->tx_len = 0;
    channel->tx_off = 0;
    return COOP_MSG_ACCEPTED;
}

int coop_msg_channel_flush(coop_msg_channel_t *channel) {
    if (channel == NULL) {
        errno = EINVAL;
        return COOP_MSG_ERROR;
    }
    pthread_mutex_lock(&channel->lock);
    int result = channel->fd < 0 ? COOP_MSG_ERROR : flush_locked(channel);
    pthread_mutex_unlock(&channel->lock);
    return result;
}

bool coop_msg_channel_has_pending(coop_msg_channel_t *channel) {
    if (channel == NULL) {
        return false;
    }
    pthread_mutex_lock(&channel->lock);
    bool pending = channel->tx_off < channel->tx_len;
    pthread_mutex_unlock(&channel->lock);
    return pending;
}

int coop_msg_channel_send(coop_msg_channel_t *channel, const void *message, size_t length) {
    if (channel == NULL || message == NULL || length == 0) {
        errno = EINVAL;
        return COOP_MSG_ERROR;
    }

    pthread_mutex_lock(&channel->lock);
    if (channel->fd < 0 || length > channel->max_message) {
        pthread_mutex_unlock(&channel->lock);
        errno = length > channel->max_message ? EMSGSIZE : ENOTCONN;
        return COOP_MSG_ERROR;
    }

    if (!channel->framed) {
        ssize_t n;
        do {
            n = send(channel->fd, message, length, MSG_DONTWAIT | MSG_NOSIGNAL);
        } while (n < 0 && errno == EINTR);
        pthread_mutex_unlock(&channel->lock);
        if (n == (ssize_t) length) {
            return COOP_MSG_ACCEPTED;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return COOP_MSG_WOULD_BLOCK;
        }
        if (n >= 0) {
            errno = EIO; /* SOCK_SEQPACKET must accept the packet atomically. */
        }
        return COOP_MSG_ERROR;
    }

    if (channel->tx_off < channel->tx_len && flush_locked(channel) == COOP_MSG_ERROR) {
        pthread_mutex_unlock(&channel->lock);
        return COOP_MSG_ERROR;
    }
    if (channel->tx_off < channel->tx_len) {
        pthread_mutex_unlock(&channel->lock);
        return COOP_MSG_WOULD_BLOCK;
    }

    const uint32_t header[2] = {COOP_MSG_FRAME_MAGIC, (uint32_t) length};
    memcpy(channel->tx_buf, header, sizeof(header));
    memcpy(channel->tx_buf + sizeof(header), message, length);
    channel->tx_len = sizeof(header) + length;
    channel->tx_off = 0;

    /* Once copied into tx_buf this message is accepted. EAGAIN or a partial
     * write leaves the remainder intact for flush()/the next send(). */
    int result = flush_locked(channel);
    pthread_mutex_unlock(&channel->lock);
    return result == COOP_MSG_ERROR ? COOP_MSG_ERROR : COOP_MSG_ACCEPTED;
}

static int receive_packet_locked(coop_msg_channel_t *channel, void *message,
                                 size_t capacity, size_t *length_out) {
    int flags = MSG_DONTWAIT;
#ifdef MSG_TRUNC
    flags |= MSG_TRUNC;
#endif
    ssize_t n;
    do {
        n = recv(channel->fd, message, capacity, flags);
    } while (n < 0 && errno == EINTR);

    if (n > (ssize_t) capacity) {
        return COOP_MSG_RECV_TOO_LARGE;
    }
    if (n > 0) {
        if (length_out != NULL) {
            *length_out = (size_t) n;
        }
        return COOP_MSG_RECV_MESSAGE;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return COOP_MSG_RECV_IDLE;
    }
    if (n == 0) {
        errno = EPIPE;
    }
    return COOP_MSG_RECV_ERROR;
}

static int receive_stream_locked(coop_msg_channel_t *channel, void *message,
                                 size_t capacity, size_t *length_out) {
    for (;;) {
        while (channel->rx_have < channel->rx_need) {
            ssize_t n = recv(channel->fd, channel->rx_buf + channel->rx_have,
                             channel->rx_need - channel->rx_have, MSG_DONTWAIT);
            if (n > 0) {
                channel->rx_have += (size_t) n;
                continue;
            }
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return COOP_MSG_RECV_IDLE;
            }
            if (n == 0) {
                errno = EPIPE;
            }
            return COOP_MSG_RECV_ERROR;
        }

        if (channel->rx_need == COOP_MSG_FRAME_BYTES) {
            uint32_t header[2];
            memcpy(header, channel->rx_buf, sizeof(header));
            if (header[0] != COOP_MSG_FRAME_MAGIC || header[1] > channel->max_message) {
                errno = EBADMSG;
                return COOP_MSG_RECV_ERROR;
            }
            channel->rx_need = COOP_MSG_FRAME_BYTES + (size_t) header[1];
            if (channel->rx_have < channel->rx_need) {
                continue;
            }
        }

        const size_t length = channel->rx_need - COOP_MSG_FRAME_BYTES;
        int result = COOP_MSG_RECV_MESSAGE;
        if (length > capacity) {
            result = COOP_MSG_RECV_TOO_LARGE;
        } else {
            memcpy(message, channel->rx_buf + COOP_MSG_FRAME_BYTES, length);
            if (length_out != NULL) {
                *length_out = length;
            }
        }
        channel->rx_have = 0;
        channel->rx_need = COOP_MSG_FRAME_BYTES;
        return result;
    }
}

int coop_msg_channel_receive(coop_msg_channel_t *channel, void *message,
                             size_t capacity, size_t *length_out) {
    if (channel == NULL || message == NULL) {
        errno = EINVAL;
        return COOP_MSG_RECV_ERROR;
    }
    if (length_out != NULL) {
        *length_out = 0;
    }

    pthread_mutex_lock(&channel->lock);
    int result;
    if (channel->fd < 0) {
        errno = ENOTCONN;
        result = COOP_MSG_RECV_ERROR;
    } else if (channel->framed) {
        result = receive_stream_locked(channel, message, capacity, length_out);
    } else {
        result = receive_packet_locked(channel, message, capacity, length_out);
    }
    pthread_mutex_unlock(&channel->lock);
    return result;
}
