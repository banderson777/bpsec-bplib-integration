#include "bplib_tcp_cla.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

typedef struct { BPLib_TCPCLA_t *cla; bool active; BPLib_Status_t status; } SessionArgs_t;
typedef struct {
    BPLib_TCPCLA_t *cla;
    uint8_t *buffer;
    size_t capacity;
    size_t size;
    BPLib_Status_t status;
} ReceiveArgs_t;

/** Perform one side of session negotiation and save the status for the joining thread. */
static void *session_worker(void *arg)
{
    SessionArgs_t *args = arg;
    args->status = BPLib_TCPCLA_SessionStart(args->cla, args->active, 2000);
    return NULL;
}

/** Receive one encoded bundle into caller-provided storage and save its size and status. */
static void *receive_worker(void *arg)
{
    ReceiveArgs_t *args = arg;
    args->status = BPLib_TCPCLA_Receive(args->cla, args->buffer, args->capacity,
                                        &args->size, 2000);
    return NULL;
}

/** Negotiate the TCPCLv4 hop concurrently on the active and passive peers. */
static int start_tcpclv4(BPLib_TCPCLA_t *active, BPLib_TCPCLA_t *passive)
{
    SessionArgs_t active_args = {.cla = active, .active = true, .status = BPLIB_ERROR};
    SessionArgs_t passive_args = {.cla = passive, .active = false, .status = BPLIB_ERROR};
    pthread_t active_thread, passive_thread;
    if (pthread_create(&active_thread, NULL, session_worker, &active_args) != 0 ||
        pthread_create(&passive_thread, NULL, session_worker, &passive_args) != 0)
        return 1;
    (void)pthread_join(active_thread, NULL);
    (void)pthread_join(passive_thread, NULL);
    return active_args.status == BPLIB_SUCCESS && passive_args.status == BPLIB_SUCCESS ? 0 : 1;
}

/** Start a receive worker before sending so the peer can acknowledge TCPCLv4 segments. */
static int send_and_receive(BPLib_TCPCLA_t *sender, BPLib_TCPCLA_t *receiver,
                            const uint8_t *input, size_t input_size,
                            uint8_t *output, size_t output_capacity, size_t *output_size)
{
    ReceiveArgs_t args = {.cla = receiver, .buffer = output, .capacity = output_capacity,
                          .size = 0, .status = BPLIB_ERROR};
    pthread_t thread;
    if (pthread_create(&thread, NULL, receive_worker, &args) != 0)
        return 1;
    BPLib_Status_t send_status = BPLib_TCPCLA_Send(sender, input, input_size, 2000);
    (void)pthread_join(thread, NULL);
    *output_size = args.size;
    return send_status == BPLIB_SUCCESS && args.status == BPLIB_SUCCESS ? 0 : 1;
}

/** Exercise a mixed-protocol chain with simulated relay, ground, and HDTN peers. */
int main(void)
{
    BPLib_TCPCLA_t moon = {0}, relay_moon = {0}, relay_hardy = {0};
    BPLib_TCPCLA_t hardy_ground = {0}, hardy_hdtn = {0}, hdtn = {0};
    int moon_link[2] = {-1, -1}, hardy_link[2] = {-1, -1}, hdtn_link[2] = {-1, -1};
    const uint8_t bundle[] = {0x82, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    uint8_t relay_buffer[1024] = {0}, hardy_buffer[1024] = {0}, hdtn_buffer[1024] = {0};
    size_t relay_size = 0, hardy_size = 0, hdtn_size = 0;
    int status = 1;

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, moon_link) != 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, hardy_link) != 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, hdtn_link) != 0 ||
        BPLib_TCPCLA_InitConfig(&moon, 1024, 0, BPLib_TCPCLA_TCPCLV4, "ipn:10.0", 4, 1024, 0) != BPLIB_SUCCESS ||
        BPLib_TCPCLA_InitConfig(&relay_moon, 1024, 0, BPLib_TCPCLA_TCPCLV4, "ipn:20.0", 4, 1024, 0) != BPLIB_SUCCESS ||
        BPLib_TCPCLA_InitConfig(&relay_hardy, 1024, 1, BPLib_TCPCLA_STCP, NULL, 4, 1024, 0) != BPLIB_SUCCESS ||
        BPLib_TCPCLA_InitConfig(&hardy_ground, 1024, 0, BPLib_TCPCLA_STCP, NULL, 4, 1024, 0) != BPLIB_SUCCESS ||
        BPLib_TCPCLA_InitConfig(&hardy_hdtn, 1024, 1, BPLib_TCPCLA_TCPCLV4, "ipn:40.0", 4, 1024, 0) != BPLIB_SUCCESS ||
        BPLib_TCPCLA_InitConfig(&hdtn, 1024, 0, BPLib_TCPCLA_TCPCLV4, "ipn:50.0", 4, 1024, 0) != BPLIB_SUCCESS)
        goto done;

    if (BPLib_TCPCLA_Attach(&moon, moon_link[0]) != BPLIB_SUCCESS ||
        BPLib_TCPCLA_Attach(&relay_moon, moon_link[1]) != BPLIB_SUCCESS ||
        BPLib_TCPCLA_Attach(&relay_hardy, hardy_link[0]) != BPLIB_SUCCESS ||
        BPLib_TCPCLA_Attach(&hardy_ground, hardy_link[1]) != BPLIB_SUCCESS ||
        BPLib_TCPCLA_Attach(&hardy_hdtn, hdtn_link[0]) != BPLIB_SUCCESS ||
        BPLib_TCPCLA_Attach(&hdtn, hdtn_link[1]) != BPLIB_SUCCESS)
        goto done;

    if (start_tcpclv4(&moon, &relay_moon) != 0)
    {
        fprintf(stderr, "moon-relay TCPCLv4 session failed\n");
        goto done;
    }
    if (start_tcpclv4(&hardy_hdtn, &hdtn) != 0)
    {
        fprintf(stderr, "Hardy-HDTN TCPCLv4 session failed\n");
        goto done;
    }

    if (send_and_receive(&moon, &relay_moon, bundle, sizeof(bundle),
                         relay_buffer, sizeof(relay_buffer), &relay_size) != 0)
    {
        fprintf(stderr, "moon TCPCLv4 transfer failed\n");
        goto done;
    }
    if (send_and_receive(&relay_hardy, &hardy_ground, relay_buffer, relay_size,
                         hardy_buffer, sizeof(hardy_buffer), &hardy_size) != 0)
    {
        fprintf(stderr, "relay-to-Hardy STCP transfer failed\n");
        goto done;
    }
    if (send_and_receive(&hardy_hdtn, &hdtn, hardy_buffer, hardy_size,
                         hdtn_buffer, sizeof(hdtn_buffer), &hdtn_size) != 0)
    {
        fprintf(stderr, "Hardy-to-HDTN TCPCLv4 transfer failed\n");
        goto done;
    }

    if (hdtn_size == sizeof(bundle) && memcmp(bundle, hdtn_buffer, sizeof(bundle)) == 0)
    {
        puts("TCPCLv4 moon-relay, STCP Hardy, TCPCLv4 HDTN forwarding succeeded");
        status = 0;
    }

done:
    BPLib_TCPCLA_Close(&moon); BPLib_TCPCLA_Close(&relay_moon);
    BPLib_TCPCLA_Close(&relay_hardy); BPLib_TCPCLA_Close(&hardy_ground);
    BPLib_TCPCLA_Close(&hardy_hdtn); BPLib_TCPCLA_Close(&hdtn);
    return status;
}
