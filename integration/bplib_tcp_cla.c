#include "bplib_tcp_cla.h"

#ifndef _WIN32
#include <arpa/inet.h>
#endif
#ifndef _WIN32
#include <errno.h>
#endif
#ifndef _WIN32
#include <fcntl.h>
#endif
#ifndef _WIN32
#include <netdb.h>
#endif
#ifndef _WIN32
#include <poll.h>
#else
#include <io.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <time.h>
#ifndef _WIN32
#include <sys/socket.h>
#include <unistd.h>
#endif

#define CONTACT_LEN 6u
#define INIT_FIXED_LEN 21u
#define SEGMENT 1u
#define ACK 2u
#define KEEPALIVE 4u
#define INIT 7u
#define END_FLAG 0x01u
#define START_FLAG 0x02u
#define DEFAULT_SEGMENT_MRU 16384u
#define MAX_PENDING 4u

struct BPLib_TCPCLA_Pending
{
    uint8_t *data;
    size_t size;
    struct BPLib_TCPCLA_Pending *next;
};

static bool try_lock(BPLib_TCPCLA_t *cla)
{
#ifdef _WIN32
    return TryAcquireSRWLockExclusive(&cla->IoLock) != 0;
#else
    return pthread_mutex_trylock(&cla->IoLock) == 0;
#endif
}

static void unlock(BPLib_TCPCLA_t *cla)
{
#ifdef _WIN32
    ReleaseSRWLockExclusive(&cla->IoLock);
#else
    pthread_mutex_unlock(&cla->IoLock);
#endif
}

static void lock(BPLib_TCPCLA_t *cla)
{
#ifdef _WIN32
    AcquireSRWLockExclusive(&cla->IoLock);
#else
    pthread_mutex_lock(&cla->IoLock);
#endif
}

static uint64_t now_ms(void)
{
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
#endif
}

static void begin_io(BPLib_TCPCLA_t *cla, uint32_t timeout_ms)
{
    cla->IoProgress = 0;
    cla->Deadline = timeout_ms ? now_ms() + timeout_ms : 0;
}

static void close_unlocked(BPLib_TCPCLA_t *cla);

static BPLib_Status_t finish_io(BPLib_TCPCLA_t *cla, BPLib_Status_t status)
{
    /* Only an idle timeout is resumable. Never reuse a partly consumed frame. */
    if (status != BPLIB_SUCCESS &&
        (status != BPLIB_CLA_TIMEOUT || cla->IoProgress || cla->RxData))
        close_unlocked(cla);
    return status;
}

static bool nonblocking(bplib_socket_t fd)
{
#ifdef _WIN32
    u_long value = 1;
    return ioctlsocket(fd, FIONBIO, &value) == 0;
#else
    int flags = fcntl(fd, F_GETFL, 0);
#ifdef SO_NOSIGPIPE
    int one = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)) != 0) return false;
#endif
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

static const uint8_t contact[CONTACT_LEN] = {0x64, 0x74, 0x6e, 0x21, 0x04, 0x00};

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void put64(uint8_t *p, uint64_t v) { for (unsigned i = 0; i < 8; ++i) p[i] = (uint8_t)(v >> (56u - 8u * i)); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }
static uint32_t get32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint64_t get64(const uint8_t *p) { uint64_t v = 0; for (unsigned i = 0; i < 8; ++i) v = (v << 8) | p[i]; return v; }

static BPLib_Status_t wait_fd(intptr_t fd, short events, uint32_t timeout_ms)
{
    struct pollfd pfd = {.fd = (bplib_socket_t)fd, .events = events, .revents = 0};
    int result;
    uint64_t deadline = timeout_ms ? now_ms() + timeout_ms : 0;
    for (;;)
    {
        int remaining = -1;
        if (deadline)
        {
            uint64_t now = now_ms();
            if (now >= deadline) return BPLIB_CLA_TIMEOUT;
            uint64_t left = deadline - now;
            remaining = left > INT_MAX ? INT_MAX : (int)left;
        }
#ifdef _WIN32
        result = WSAPoll(&pfd, 1, remaining);
#else
        result = poll(&pfd, 1, remaining);
#endif
        /* Recompute the finite deadline after EINTR or a clamped poll wait. */
        if (result < 0 && bplib_socket_interrupted(bplib_socket_last_error())) continue;
        if (result == 0 && deadline) continue;
        break;
    }
    if (result == 0) return BPLIB_CLA_TIMEOUT;
    if (result < 0 || (pfd.revents & (POLLERR | POLLNVAL)) ||
        !(pfd.revents & events)) return BPLIB_OS_ERROR;
    return BPLIB_SUCCESS;
}

static BPLib_Status_t transfer(BPLib_TCPCLA_t *cla, void *buffer, size_t length,
                               bool writing, uint32_t timeout_ms)
{
    uint8_t *cursor = buffer;
    size_t done = 0;
    while (done < length)
    {
        if (cla->Deadline)
        {
            uint64_t now = now_ms();
            if (now >= cla->Deadline) return BPLIB_CLA_TIMEOUT;
            uint64_t left = cla->Deadline - now;
            timeout_ms = left > INT_MAX ? INT_MAX : (uint32_t)left;
        }
        BPLib_Status_t status = wait_fd(cla->SocketFd, writing ? POLLOUT : POLLIN, timeout_ms);
        if (status != BPLIB_SUCCESS) return status;
        bplib_socket_count_t count;
#ifdef _WIN32
        count = writing ? send((SOCKET)cla->SocketFd, (const char *)(cursor + done),
                               (int)(length - done), 0)
                        : recv((SOCKET)cla->SocketFd, (char *)(cursor + done),
                               (int)(length - done), 0);
#else
        count = writing ? send((int)cla->SocketFd, cursor + done, length - done,
#ifdef MSG_NOSIGNAL
                               MSG_NOSIGNAL
#else
                               0
#endif
                               )
                        : recv((int)cla->SocketFd, cursor + done, length - done, 0);
#endif
        if (count == 0) return BPLIB_OS_ERROR;
        if (count < 0)
        {
            if (bplib_socket_would_block(bplib_socket_last_error()) ||
                bplib_socket_interrupted(bplib_socket_last_error())) continue;
            return BPLIB_OS_ERROR;
        }
        done += (size_t)count;
        cla->IoProgress += (size_t)count;
    }
    return BPLIB_SUCCESS;
}

static BPLib_Status_t send_init(BPLib_TCPCLA_t *cla, uint32_t timeout_ms)
{
    size_t node_len = strlen(cla->NodeId);
    size_t total = INIT_FIXED_LEN + node_len + 4u;
    if (node_len > UINT16_MAX) return BPLIB_INVALID_CONFIG_ERR;
    uint8_t *frame = calloc(1, total);
    if (frame == NULL) return BPLIB_OS_ERROR;
    frame[0] = INIT;
    put16(frame + 1, cla->KeepaliveInterval);
    put64(frame + 3, cla->SegmentMru);
    put64(frame + 11, cla->TransferMru);
    put16(frame + 19, (uint16_t)node_len);
    memcpy(frame + INIT_FIXED_LEN, cla->NodeId, node_len);
    /* No session extensions. */
    BPLib_Status_t status = transfer(cla, frame, total, true, timeout_ms);
    free(frame);
    return status;
}

static BPLib_Status_t receive_init(BPLib_TCPCLA_t *cla, uint32_t timeout_ms)
{
    uint8_t fixed[INIT_FIXED_LEN];
    BPLib_Status_t status = transfer(cla, fixed, sizeof(fixed), false, timeout_ms);
    if (status != BPLIB_SUCCESS || fixed[0] != INIT) return BPLIB_OS_ERROR;
    uint16_t node_len = get16(fixed + 19);
    uint8_t discard[256];
    size_t remaining = node_len;
    while (remaining > 0)
    {
        size_t chunk = remaining < sizeof(discard) ? remaining : sizeof(discard);
        status = transfer(cla, discard, chunk, false, timeout_ms);
        if (status != BPLIB_SUCCESS) return status;
        remaining -= chunk;
    }
    uint8_t ext_wire[4];
    status = transfer(cla, ext_wire, sizeof(ext_wire), false, timeout_ms);
    if (status != BPLIB_SUCCESS) return status;
    uint32_t ext_len = get32(ext_wire);
    if (ext_len != 0) return BPLIB_UNIMPLEMENTED;
    uint64_t peer_segment = get64(fixed + 3);
    uint64_t peer_transfer = get64(fixed + 11);
    if (peer_segment == 0 || peer_transfer == 0) return BPLIB_INVALID_CONFIG_ERR;
    if (peer_segment < cla->SegmentMru) cla->SegmentMru = peer_segment;
    if (peer_transfer < cla->TransferMru) cla->TransferMru = peer_transfer;
    uint16_t peer_keepalive = get16(fixed + 1);
    if (peer_keepalive < cla->KeepaliveInterval) cla->KeepaliveInterval = peer_keepalive;
    return BPLIB_SUCCESS;
}

static BPLib_Status_t send_ack(BPLib_TCPCLA_t *cla, uint64_t id, uint8_t flags,
                               uint64_t acknowledged, uint32_t timeout_ms)
{
    uint8_t frame[18] = {ACK, flags};
    put64(frame + 2, id); put64(frame + 10, acknowledged);
    return transfer(cla, frame, sizeof(frame), true, timeout_ms);
}

/* Called only by the current I/O owner. Incoming bundles are dispatched even
 * while Send waits for its ACK, so simultaneous bidirectional sends work. */
static BPLib_Status_t dispatch(BPLib_TCPCLA_t *cla, bool expecting_ack,
                               uint64_t id, uint64_t acknowledged,
                               bool *acked, uint32_t timeout_ms)
{
    uint8_t type;
    *acked = false;
    BPLib_Status_t status = transfer(cla, &type, 1, false, timeout_ms);
    if (status != BPLIB_SUCCESS) return status;
    if (type == KEEPALIVE) return BPLIB_SUCCESS;
    if (type == ACK)
    {
        uint8_t rest[17];
        status = transfer(cla, rest, sizeof(rest), false, timeout_ms);
        if (status != BPLIB_SUCCESS) return status;
        if (!expecting_ack || get64(rest + 1) != id || get64(rest + 9) != acknowledged ||
            (rest[0] & ~(START_FLAG | END_FLAG))) return BPLIB_OS_ERROR;
        *acked = true;
        return BPLIB_SUCCESS;
    }
    if (type != SEGMENT) return BPLIB_OS_ERROR;
    uint8_t header[9], length[8];
    status = transfer(cla, header, sizeof(header), false, timeout_ms);
    if (status != BPLIB_SUCCESS) return status;
    uint8_t flags = header[0];
    uint64_t segment_id = get64(header + 1);
    if (flags & ~(START_FLAG | END_FLAG)) return BPLIB_OS_ERROR;
    if (flags & START_FLAG)
    {
        uint8_t extensions[4];
        if (cla->RxData || cla->PendingCount >= MAX_PENDING) return BPLIB_BUF_LEN_ERROR;
        status = transfer(cla, extensions, sizeof(extensions), false, timeout_ms);
        if (status != BPLIB_SUCCESS) return status;
        if (get32(extensions)) return BPLIB_UNIMPLEMENTED;
        cla->RxData = malloc(cla->MaxBundleSize);
        if (!cla->RxData) return BPLIB_NO_STOR_ERR;
        cla->RxSize = 0;
        cla->RxId = segment_id;
    }
    else if (!cla->RxData || segment_id != cla->RxId) return BPLIB_OS_ERROR;
    status = transfer(cla, length, sizeof(length), false, timeout_ms);
    if (status != BPLIB_SUCCESS) return status;
    uint64_t len = get64(length);
    if (len == 0 || len > cla->SegmentMru ||
        cla->RxSize > cla->TransferMru || len > cla->TransferMru - cla->RxSize ||
        len > cla->MaxBundleSize - cla->RxSize) return BPLIB_BUF_LEN_ERROR;
    status = transfer(cla, cla->RxData + cla->RxSize, (size_t)len, false, timeout_ms);
    if (status != BPLIB_SUCCESS) return status;
    cla->RxSize += (size_t)len;
    if (flags & END_FLAG)
    {
        BPLib_TCPCLA_Pending_t *pending = malloc(sizeof(*pending));
        if (!pending) return BPLIB_NO_STOR_ERR;
        pending->data = cla->RxData;
        pending->size = cla->RxSize;
        pending->next = NULL;
        if (cla->PendingTail) cla->PendingTail->next = pending;
        else cla->PendingHead = pending;
        cla->PendingTail = pending;
        cla->PendingCount++;
        cla->RxData = NULL;
    }
    return send_ack(cla, segment_id, flags, cla->RxSize, timeout_ms);
}

static BPLib_Status_t receive_ack(BPLib_TCPCLA_t *cla, uint64_t id, uint64_t acknowledged,
                                  uint32_t timeout_ms)
{
    bool acked = false;
    while (!acked)
    {
        BPLib_Status_t status = dispatch(cla, true, id, acknowledged, &acked, timeout_ms);
        if (status != BPLIB_SUCCESS) return status;
    }
    return BPLIB_SUCCESS;
}

static BPLib_Status_t receive_v4(BPLib_TCPCLA_t *cla, void *bundle, size_t capacity,
                                size_t *size, uint32_t timeout_ms)
{
    if (!cla->SessionReady) return BPLIB_APP_STATE_ERR;
    while (!cla->PendingHead)
    {
        bool ignored;
        BPLib_Status_t status = dispatch(cla, false, 0, 0, &ignored, timeout_ms);
        if (status != BPLIB_SUCCESS) return status;
    }
    BPLib_TCPCLA_Pending_t *pending = cla->PendingHead;
    if (pending->size > capacity) return BPLIB_BUF_LEN_ERROR;
    memcpy(bundle, pending->data, pending->size);
    *size = pending->size;
    cla->PendingHead = pending->next;
    if (!cla->PendingHead) cla->PendingTail = NULL;
    cla->PendingCount--;
    free(pending->data);
    free(pending);
    return BPLIB_SUCCESS;
}

static BPLib_Status_t receive_stcp(BPLib_TCPCLA_t *cla, void *bundle, size_t capacity,
                                  size_t *size, uint32_t timeout_ms)
{
    uint32_t wire_size;
    BPLib_Status_t status = transfer(cla, &wire_size, sizeof(wire_size), false, timeout_ms);
    if (status != BPLIB_SUCCESS) return status;
    size_t len = ntohl(wire_size);
    if (!len || len > cla->MaxBundleSize || len > capacity)
        return BPLIB_BUF_LEN_ERROR;
    status = transfer(cla, bundle, len, false, timeout_ms);
    if (status == BPLIB_SUCCESS) *size = len;
    return status;
}

static BPLib_Status_t init_lock(BPLib_TCPCLA_t *cla)
{
#ifdef _WIN32
    InitializeSRWLock(&cla->IoLock);
#else
    if (pthread_mutex_init(&cla->IoLock, NULL) != 0)
        return BPLIB_OS_ERROR;
#endif
    return BPLIB_SUCCESS;
}

static BPLib_Status_t send_v4(BPLib_TCPCLA_t *cla, const uint8_t *bundle,
                              size_t size, uint32_t timeout_ms)
{
    if (!cla->SessionReady) return BPLIB_APP_STATE_ERR;
    if ((uint64_t)size > cla->TransferMru) return BPLIB_BUF_LEN_ERROR;
    uint64_t id = cla->NextTransferId++;
    size_t offset = 0;
    do
    {
        size_t chunk = size - offset;
        if ((uint64_t)chunk > cla->SegmentMru) chunk = (size_t)cla->SegmentMru;
        bool first = offset == 0;
        bool last = offset + chunk == size;
        /* type + flags + transfer ID + (first-segment extension length) +
         * payload length. */
        size_t header_len = first ? 22u : 18u;
        uint8_t *frame = malloc(header_len + chunk);
        if (frame == NULL) return BPLIB_OS_ERROR;
        frame[0] = SEGMENT;
        frame[1] = (uint8_t)((first ? START_FLAG : 0) | (last ? END_FLAG : 0));
        put64(frame + 2, id);
        size_t length_offset = 10;
        if (first) { put32(frame + 10, 0); length_offset += 4; }
        put64(frame + length_offset, chunk);
        memcpy(frame + length_offset + 8, bundle + offset, chunk);
        BPLib_Status_t status = transfer(cla, frame, header_len + chunk, true, timeout_ms);
        free(frame);
        if (status != BPLIB_SUCCESS) return status;
        offset += chunk;
        status = receive_ack(cla, id, offset, timeout_ms);
        if (status != BPLIB_SUCCESS) return status;
    } while (offset < size);
    return BPLIB_SUCCESS;
}

const char *BPLib_TCPCLA_ProtocolName(BPLib_TCPCLA_Protocol_t protocol)
{
    return protocol == BPLib_TCPCLA_TCPCLV4 ? "tcpclv4" : "stcp";
}

BPLib_Status_t BPLib_TCPCLA_InitConfig(BPLib_TCPCLA_t *cla, size_t max_bundle_size,
                                       uint32_t contact_id, BPLib_TCPCLA_Protocol_t protocol,
                                       const char *node_id, uint64_t segment_mru,
                                       uint64_t transfer_mru, uint16_t keepalive_interval)
{
    if (cla == NULL || max_bundle_size == 0 || max_bundle_size > UINT32_MAX ||
        contact_id >= BPLIB_MAX_NUM_CONTACTS || protocol > BPLib_TCPCLA_TCPCLV4)
        return BPLIB_INVALID_CONFIG_ERR;
    if (transfer_mru == 0) transfer_mru = max_bundle_size;
    if (segment_mru == 0) segment_mru = transfer_mru < DEFAULT_SEGMENT_MRU ? transfer_mru : DEFAULT_SEGMENT_MRU;
    if (segment_mru > transfer_mru || transfer_mru > max_bundle_size ||
        (protocol == BPLib_TCPCLA_TCPCLV4 && (node_id == NULL || node_id[0] == '\0')))
        return BPLIB_INVALID_CONFIG_ERR;
    if (node_id && strlen(node_id) >= sizeof(cla->NodeId)) return BPLIB_INVALID_CONFIG_ERR;
    memset(cla, 0, sizeof(*cla));
    if (init_lock(cla) != BPLIB_SUCCESS) return BPLIB_OS_ERROR;
#ifdef _WIN32
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) return BPLIB_OS_ERROR;
#endif
    cla->ListenFd = BPLIB_INVALID_SOCKET; cla->SocketFd = BPLIB_INVALID_SOCKET;
    cla->MaxBundleSize = max_bundle_size; cla->ContactId = contact_id;
    cla->Protocol = protocol; cla->SessionReady = false; cla->NextTransferId = 1;
    cla->SegmentMru = segment_mru; cla->TransferMru = transfer_mru;
    cla->ConfigSegmentMru = segment_mru; cla->ConfigTransferMru = transfer_mru;
    cla->ConfigKeepaliveInterval = keepalive_interval;
    cla->KeepaliveInterval = keepalive_interval; cla->NodeId[0] = '\0';
    if (node_id != NULL)
    {
        if (strlen(node_id) >= sizeof(cla->NodeId)) return BPLIB_INVALID_CONFIG_ERR;
        (void)strcpy(cla->NodeId, node_id);
    }
    return BPLIB_SUCCESS;
}

BPLib_Status_t BPLib_TCPCLA_Init(BPLib_TCPCLA_t *cla, size_t max_bundle_size,
                                 uint32_t contact_id)
{
    return BPLib_TCPCLA_InitConfig(cla, max_bundle_size, contact_id,
                                   BPLib_TCPCLA_STCP, NULL, 0, 0, 0);
}

static void close_unlocked(BPLib_TCPCLA_t *cla)
{
    if (cla == NULL) return;
    intptr_t socket_fd = cla->SocketFd;
    if (socket_fd != BPLIB_INVALID_SOCKET) bplib_socket_close(socket_fd);
    if (cla->ListenFd != BPLIB_INVALID_SOCKET && cla->ListenFd != socket_fd)
        bplib_socket_close(cla->ListenFd);
    cla->SocketFd = BPLIB_INVALID_SOCKET; cla->ListenFd = BPLIB_INVALID_SOCKET;
    cla->SessionReady = false;
    cla->SegmentMru = cla->ConfigSegmentMru;
    cla->TransferMru = cla->ConfigTransferMru;
    cla->KeepaliveInterval = cla->ConfigKeepaliveInterval;
    cla->NextTransferId = 1;
    free(cla->RxData); cla->RxData = NULL; cla->RxSize = 0;
    while (cla->PendingHead)
    {
        BPLib_TCPCLA_Pending_t *next = cla->PendingHead->next;
        free(cla->PendingHead->data); free(cla->PendingHead);
        cla->PendingHead = next;
    }
    cla->PendingTail = NULL; cla->PendingCount = 0;
}

void BPLib_TCPCLA_Close(BPLib_TCPCLA_t *cla)
{
    if (!cla) return;
#ifdef _WIN32
    AcquireSRWLockExclusive(&cla->IoLock);
#else
    pthread_mutex_lock(&cla->IoLock);
#endif
    close_unlocked(cla);
    unlock(cla);
}

void BPLib_TCPCLA_Destroy(BPLib_TCPCLA_t *cla)
{
    if (!cla) return;
    BPLib_TCPCLA_Close(cla);
#ifdef _WIN32
    WSACleanup();
#else
    pthread_mutex_destroy(&cla->IoLock);
#endif
}

BPLib_Status_t BPLib_TCPCLA_Attach(BPLib_TCPCLA_t *cla, intptr_t socket_fd)
{
    if (cla == NULL || socket_fd == BPLIB_INVALID_SOCKET || cla->SocketFd != BPLIB_INVALID_SOCKET)
        return BPLIB_INVALID_CONFIG_ERR;
    if (!nonblocking((bplib_socket_t)socket_fd)) return BPLIB_OS_ERROR;
    cla->SocketFd = socket_fd;
    return BPLIB_SUCCESS;
}

BPLib_Status_t BPLib_TCPCLA_Connect(BPLib_TCPCLA_t *cla, const char *host,
                                    uint16_t port, uint32_t timeout_ms)
{
    if (cla == NULL || host == NULL || cla->SocketFd != BPLIB_INVALID_SOCKET) return BPLIB_INVALID_CONFIG_ERR;
    char service[6]; (void)snprintf(service, sizeof(service), "%u", (unsigned)port);
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *addresses = NULL;
    if (getaddrinfo(host, service, &hints, &addresses) != 0) return BPLIB_OS_ERROR;
    BPLib_Status_t status = BPLIB_OS_ERROR;
    for (struct addrinfo *address = addresses; address != NULL; address = address->ai_next)
    {
        bplib_socket_t fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (fd == BPLIB_INVALID_SOCKET) continue;
        if (!nonblocking(fd)) { bplib_socket_close(fd); continue; }
        int result = connect(fd, address->ai_addr, address->ai_addrlen);
        int connect_error = bplib_socket_last_error();
        if (result == BPLIB_SOCKET_ERROR && !bplib_socket_would_block(connect_error))
        { bplib_socket_close(fd); continue; }
        status = result == 0 ? BPLIB_SUCCESS : wait_fd(fd, POLLOUT, timeout_ms);
        if (status == BPLIB_SUCCESS)
        {
            int error = 0;
#ifdef _WIN32
            int error_len = sizeof(error);
#else
            socklen_t error_len = sizeof(error);
#endif
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_len) == 0 && error == 0)
            { cla->SocketFd = fd; break; }
            status = BPLIB_OS_ERROR;
        }
        bplib_socket_close(fd);
    }
    freeaddrinfo(addresses);
    return cla->SocketFd != BPLIB_INVALID_SOCKET ? BPLIB_SUCCESS : status;
}

BPLib_Status_t BPLib_TCPCLA_Listen(BPLib_TCPCLA_t *cla, const char *host,
                                   uint16_t port, int backlog)
{
    if (cla == NULL || host == NULL || cla->SocketFd != BPLIB_INVALID_SOCKET ||
        cla->ListenFd != BPLIB_INVALID_SOCKET || backlog <= 0)
        return BPLIB_INVALID_CONFIG_ERR;
    char service[6]; (void)snprintf(service, sizeof(service), "%u", (unsigned)port);
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    struct addrinfo *addresses = NULL;
    if (getaddrinfo(host, service, &hints, &addresses) != 0) return BPLIB_OS_ERROR;
    BPLib_Status_t status = BPLIB_OS_ERROR;
    for (struct addrinfo *address = addresses; address != NULL; address = address->ai_next)
    {
        bplib_socket_t fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (fd == BPLIB_INVALID_SOCKET) continue;
        int reuse = 1; (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        if (bind(fd, address->ai_addr, address->ai_addrlen) == 0 && listen(fd, backlog) == 0)
        { cla->ListenFd = fd; status = BPLIB_SUCCESS; break; }
        bplib_socket_close(fd);
    }
    freeaddrinfo(addresses);
    return status;
}

BPLib_Status_t BPLib_TCPCLA_Accept(BPLib_TCPCLA_t *cla, uint32_t timeout_ms)
{
    if (cla == NULL || cla->ListenFd == BPLIB_INVALID_SOCKET ||
        cla->SocketFd != BPLIB_INVALID_SOCKET) return BPLIB_INVALID_CONFIG_ERR;
    BPLib_Status_t status = wait_fd(cla->ListenFd, POLLIN, timeout_ms);
    if (status != BPLIB_SUCCESS) { BPLib_TCPCLA_Close(cla); return status; }
    bplib_socket_t fd = accept((bplib_socket_t)cla->ListenFd, NULL, NULL);
    if (fd == BPLIB_INVALID_SOCKET || !nonblocking(fd))
    {
        if (fd != BPLIB_INVALID_SOCKET) bplib_socket_close(fd);
        BPLib_TCPCLA_Close(cla);
        return BPLIB_OS_ERROR;
    }
    cla->SocketFd = fd;
    return BPLIB_SUCCESS;
}

BPLib_Status_t BPLib_TCPCLA_SessionStart(BPLib_TCPCLA_t *cla, bool active, uint32_t timeout_ms)
{
    if (cla == NULL || cla->SocketFd == BPLIB_INVALID_SOCKET ||
        cla->Protocol != BPLib_TCPCLA_TCPCLV4)
        return BPLIB_INVALID_CONFIG_ERR;
    BPLib_Status_t status;
    begin_io(cla, timeout_ms);
    if (active)
    {
        uint8_t peer_contact[CONTACT_LEN];
        status = transfer(cla, (void *)contact, sizeof(contact), true, timeout_ms);
        if (status == BPLIB_SUCCESS) status = transfer(cla, peer_contact, sizeof(peer_contact), false, timeout_ms);
        if (status == BPLIB_SUCCESS && memcmp(peer_contact, contact, sizeof(contact)) != 0)
            status = BPLIB_OS_ERROR;
        if (status == BPLIB_SUCCESS) status = send_init(cla, timeout_ms);
        if (status == BPLIB_SUCCESS) status = receive_init(cla, timeout_ms);
    }
    else
    {
        uint8_t peer_contact[CONTACT_LEN];
        status = transfer(cla, peer_contact, sizeof(peer_contact), false, timeout_ms);
        if (status == BPLIB_SUCCESS && memcmp(peer_contact, contact, sizeof(contact)) != 0) status = BPLIB_OS_ERROR;
        if (status == BPLIB_SUCCESS) status = transfer(cla, (void *)contact, sizeof(contact), true, timeout_ms);
        if (status == BPLIB_SUCCESS) status = receive_init(cla, timeout_ms);
        if (status == BPLIB_SUCCESS) status = send_init(cla, timeout_ms);
    }
    cla->SessionReady = status == BPLIB_SUCCESS;
    if (status != BPLIB_SUCCESS) BPLib_TCPCLA_Close(cla);
    return status;
}

BPLib_Status_t BPLib_TCPCLA_Send(BPLib_TCPCLA_t *cla, const void *bundle,
                                 size_t size, uint32_t timeout_ms)
{
    if (cla == NULL || bundle == NULL || size == 0) return BPLIB_NULL_PTR_ERROR;
    lock(cla);
    if (cla->SocketFd == BPLIB_INVALID_SOCKET) { unlock(cla); return BPLIB_APP_STATE_ERR; }
    if (size > cla->MaxBundleSize || (cla->Protocol == BPLib_TCPCLA_TCPCLV4 && size > cla->TransferMru))
    { unlock(cla); return BPLIB_BUF_LEN_ERROR; }
    begin_io(cla, timeout_ms);
    BPLib_Status_t status;
    if (cla->Protocol == BPLib_TCPCLA_TCPCLV4) status = send_v4(cla, bundle, size, timeout_ms);
    else
    {
        uint32_t wire_size = htonl((uint32_t)size);
        status = transfer(cla, &wire_size, sizeof(wire_size), true, timeout_ms);
        if (status == BPLIB_SUCCESS) status = transfer(cla, (void *)bundle, size, true, timeout_ms);
    }
    status = finish_io(cla, status);
    unlock(cla);
    return status;
}

BPLib_Status_t BPLib_TCPCLA_Receive(BPLib_TCPCLA_t *cla, void *bundle,
                                    size_t buffer_size, size_t *size, uint32_t timeout_ms)
{
    if (cla == NULL || bundle == NULL || size == NULL) return BPLIB_NULL_PTR_ERROR;
    *size = 0;
    if (!try_lock(cla)) return BPLIB_CLA_TIMEOUT;
    if (cla->SocketFd == BPLIB_INVALID_SOCKET) { unlock(cla); return BPLIB_APP_STATE_ERR; }
    begin_io(cla, timeout_ms);
    BPLib_Status_t status = cla->Protocol == BPLib_TCPCLA_TCPCLV4 ?
        receive_v4(cla, bundle, buffer_size, size, timeout_ms) :
        receive_stcp(cla, bundle, buffer_size, size, timeout_ms);
    status = finish_io(cla, status);
    unlock(cla);
    return status;
}
