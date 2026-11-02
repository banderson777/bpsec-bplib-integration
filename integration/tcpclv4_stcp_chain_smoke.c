#include "bplib_tcp_cla.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

typedef struct
{
    BPLib_TCPCLA_t *cla;
    uint8_t *buffer;
    size_t capacity;
    size_t size;
    BPLib_Status_t status;
} ReceiveArgs_t;

typedef struct
{
    BPLib_TCPCLA_t *cla;
    bool active;
    BPLib_Status_t status;
} SessionArgs_t;

static void *receive_worker(void *arg)
{
    ReceiveArgs_t *args = arg;
    args->status = BPLib_TCPCLA_Receive(args->cla, args->buffer, args->capacity,
                                        &args->size, 2000);
    return NULL;
}

static void *session_worker(void *arg)
{
    SessionArgs_t *args = arg;
    args->status = BPLib_TCPCLA_SessionStart(args->cla, args->active, 2000);
    return NULL;
}

static int start_tcpclv4(BPLib_TCPCLA_t *active, BPLib_TCPCLA_t *passive)
{
    SessionArgs_t active_args = {.cla = active, .active = true, .status = BPLIB_ERROR};
    SessionArgs_t passive_args = {.cla = passive, .active = false, .status = BPLIB_ERROR};
    pthread_t active_thread;
    pthread_t passive_thread;

    if (pthread_create(&active_thread, NULL, session_worker, &active_args) != 0 ||
        pthread_create(&passive_thread, NULL, session_worker, &passive_args) != 0)
        return 1;
    (void)pthread_join(active_thread, NULL);
    (void)pthread_join(passive_thread, NULL);
    return active_args.status == BPLIB_SUCCESS && passive_args.status == BPLIB_SUCCESS ? 0 : 1;
}

int main(void)
{
    BPLib_TCPCLA_t moon = {0}, relay_in = {0}, relay_out = {0}, hardy = {0};
    int moon_link[2] = {-1, -1};
    int ground_link[2] = {-1, -1};
    const uint8_t bundle[] = {0x82, 0x01, 0x02, 0x03, 0x04, 0x05};
    uint8_t relay_buffer[1024] = {0};
    uint8_t hardy_buffer[1024] = {0};
    ReceiveArgs_t relay_rx = {0}, hardy_rx = {0};
    pthread_t relay_thread, hardy_thread;
    int status = 1;

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, moon_link) != 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, ground_link) != 0 ||
        BPLib_TCPCLA_InitConfig(&moon, 1024, 0, BPLib_TCPCLA_TCPCLV4,
                                "ipn:10.0", 4, 1024, 0) != BPLIB_SUCCESS ||
        BPLib_TCPCLA_InitConfig(&relay_in, 1024, 0, BPLib_TCPCLA_TCPCLV4,
                                "ipn:20.0", 4, 1024, 0) != BPLIB_SUCCESS ||
        BPLib_TCPCLA_InitConfig(&relay_out, 1024, 1, BPLib_TCPCLA_STCP,
                                NULL, 4, 1024, 0) != BPLIB_SUCCESS ||
        BPLib_TCPCLA_InitConfig(&hardy, 1024, 0, BPLib_TCPCLA_STCP,
                                NULL, 4, 1024, 0) != BPLIB_SUCCESS)
        goto done;

    if (BPLib_TCPCLA_Attach(&moon, moon_link[0]) != BPLIB_SUCCESS ||
        BPLib_TCPCLA_Attach(&relay_in, moon_link[1]) != BPLIB_SUCCESS ||
        BPLib_TCPCLA_Attach(&relay_out, ground_link[0]) != BPLIB_SUCCESS ||
        BPLib_TCPCLA_Attach(&hardy, ground_link[1]) != BPLIB_SUCCESS)
        goto done;

    if (start_tcpclv4(&moon, &relay_in) != 0)
        goto done;

    relay_rx.cla = &relay_in;
    relay_rx.buffer = relay_buffer;
    relay_rx.capacity = sizeof(relay_buffer);
    hardy_rx.cla = &hardy;
    hardy_rx.buffer = hardy_buffer;
    hardy_rx.capacity = sizeof(hardy_buffer);

    if (pthread_create(&relay_thread, NULL, receive_worker, &relay_rx) != 0)
        goto done;
    if (BPLib_TCPCLA_Send(&moon, bundle, sizeof(bundle), 2000) != BPLIB_SUCCESS)
        goto join_relay;
    (void)pthread_join(relay_thread, NULL);
    if (relay_rx.status != BPLIB_SUCCESS)
        goto done;

    if (pthread_create(&hardy_thread, NULL, receive_worker, &hardy_rx) != 0)
        goto done;
    if (BPLib_TCPCLA_Send(&relay_out, relay_buffer, relay_rx.size, 2000) != BPLIB_SUCCESS)
        goto join_hardy;
    (void)pthread_join(hardy_thread, NULL);
    if (hardy_rx.status == BPLIB_SUCCESS && hardy_rx.size == sizeof(bundle) &&
        memcmp(bundle, hardy_buffer, sizeof(bundle)) == 0)
    {
        puts("Mixed TCPCLv4 moon-relay to Hardy STCP ground forwarding succeeded");
        status = 0;
    }
    goto done;

join_hardy:
    (void)pthread_join(hardy_thread, NULL);
    goto done;
join_relay:
    (void)pthread_join(relay_thread, NULL);

done:
    BPLib_TCPCLA_Close(&moon);
    BPLib_TCPCLA_Close(&relay_in);
    BPLib_TCPCLA_Close(&relay_out);
    BPLib_TCPCLA_Close(&hardy);
    return status;
}
