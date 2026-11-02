#include "bplib_tcp_cla.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

typedef struct
{
    BPLib_TCPCLA_t *cla;
    bool active;
    BPLib_Status_t status;
} SessionArgs_t;

typedef struct
{
    BPLib_TCPCLA_t *cla;
    const uint8_t *input;
    uint8_t *output;
    size_t input_size;
    size_t output_size;
    BPLib_Status_t status;
} TransferArgs_t;

static void *session_worker(void *arg)
{
    SessionArgs_t *args = arg;
    args->status = BPLib_TCPCLA_SessionStart(args->cla, args->active, 2000);
    return NULL;
}

static void *receive_worker(void *arg)
{
    TransferArgs_t *args = arg;
    args->status = BPLib_TCPCLA_Receive(args->cla, args->output, args->output_size,
                                        &args->output_size, 2000);
    return NULL;
}

static int start_sessions(BPLib_TCPCLA_t *active, BPLib_TCPCLA_t *passive)
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

static int forward_bundle(BPLib_TCPCLA_t *source, BPLib_TCPCLA_t *relay_in,
                          BPLib_TCPCLA_t *relay_out, BPLib_TCPCLA_t *ground,
                          const uint8_t *bundle, size_t size)
{
    uint8_t relay_buffer[1024] = {0};
    uint8_t ground_buffer[1024] = {0};
    TransferArgs_t relay_rx = {.cla = relay_in, .output = relay_buffer,
                               .output_size = sizeof(relay_buffer), .status = BPLIB_ERROR};
    pthread_t relay_rx_thread;
    if (pthread_create(&relay_rx_thread, NULL, receive_worker, &relay_rx) != 0) return 1;
    BPLib_Status_t moon_status = BPLib_TCPCLA_Send(source, bundle, size, 2000);
    (void)pthread_join(relay_rx_thread, NULL);
    if (moon_status != BPLIB_SUCCESS || relay_rx.status != BPLIB_SUCCESS) return 1;

    TransferArgs_t ground_rx = {.cla = ground, .output = ground_buffer,
                                .output_size = sizeof(ground_buffer), .status = BPLIB_ERROR};
    pthread_t ground_rx_thread;
    if (pthread_create(&ground_rx_thread, NULL, receive_worker, &ground_rx) != 0) return 1;
    BPLib_Status_t relay_status = BPLib_TCPCLA_Send(relay_out, relay_buffer, relay_rx.output_size, 2000);
    (void)pthread_join(ground_rx_thread, NULL);
    return relay_status == BPLIB_SUCCESS && ground_rx.status == BPLIB_SUCCESS &&
           ground_rx.output_size == size && memcmp(bundle, ground_buffer, size) == 0 ? 0 : 1;
}

int main(void)
{
    BPLib_TCPCLA_t moon = {0}, relay_in = {0}, relay_out = {0}, ground = {0};
    int moon_link[2], ground_link[2];
    const uint8_t bundle[] = {0x82, 0x01, 0x02, 0x03, 0x04, 0x05};
    int status = 1;

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, moon_link) != 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, ground_link) != 0 ||
        BPLib_TCPCLA_InitConfig(&moon, 1024, 0, BPLib_TCPCLA_TCPCLV4, "ipn:10.0", 4, 1024, 0) != BPLIB_SUCCESS ||
        BPLib_TCPCLA_InitConfig(&relay_in, 1024, 0, BPLib_TCPCLA_TCPCLV4, "ipn:20.0", 4, 1024, 0) != BPLIB_SUCCESS ||
        BPLib_TCPCLA_InitConfig(&relay_out, 1024, 0, BPLib_TCPCLA_TCPCLV4, "ipn:20.0", 4, 1024, 0) != BPLIB_SUCCESS ||
        BPLib_TCPCLA_InitConfig(&ground, 1024, 0, BPLib_TCPCLA_TCPCLV4, "ipn:30.0", 4, 1024, 0) != BPLIB_SUCCESS)
        goto done;
    if (BPLib_TCPCLA_Attach(&moon, moon_link[0]) != BPLIB_SUCCESS ||
        BPLib_TCPCLA_Attach(&relay_in, moon_link[1]) != BPLIB_SUCCESS ||
        BPLib_TCPCLA_Attach(&relay_out, ground_link[0]) != BPLIB_SUCCESS ||
        BPLib_TCPCLA_Attach(&ground, ground_link[1]) != BPLIB_SUCCESS)
        goto done;
    if (start_sessions(&moon, &relay_in) != 0 || start_sessions(&relay_out, &ground) != 0)
        goto done;
    status = forward_bundle(&moon, &relay_in, &relay_out, &ground, bundle, sizeof(bundle));
    if (status == 0) puts("TCPCLv4 moon-relay-ground forwarding succeeded");

done:
    BPLib_TCPCLA_Close(&moon); BPLib_TCPCLA_Close(&relay_in);
    BPLib_TCPCLA_Close(&relay_out); BPLib_TCPCLA_Close(&ground);
    return status;
}
