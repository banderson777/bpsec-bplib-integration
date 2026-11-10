#include "bplib_tcp_cla.h"

#include <stdio.h>
#include <pthread.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/** Send one length-prefixed bundle over loopback and compare the bytes received. */
static int test_stcp(void)
{
    BPLib_TCPCLA_t sender = {0}, receiver = {0};
    const uint8_t sent[] = {0x82, 0x01, 0x02, 0x03};
    uint8_t received[sizeof(sent)] = {0};
    size_t received_size = 0;
    int sockets[2];
    int result = socketpair(AF_UNIX, SOCK_STREAM, 0, sockets);
    if (result != 0 || BPLib_TCPCLA_Init(&sender, 17000, 0) != BPLIB_SUCCESS ||
        BPLib_TCPCLA_Init(&receiver, 17000, 0) != BPLIB_SUCCESS ||
        BPLib_TCPCLA_Attach(&sender, sockets[0]) != BPLIB_SUCCESS ||
        BPLib_TCPCLA_Attach(&receiver, sockets[1]) != BPLIB_SUCCESS ||
        BPLib_TCPCLA_Send(&sender, sent, sizeof(sent), 1000) != BPLIB_SUCCESS ||
        BPLib_TCPCLA_Receive(&receiver, received, sizeof(received), &received_size, 1000) != BPLIB_SUCCESS ||
        received_size != sizeof(sent) || memcmp(sent, received, sizeof(sent)) != 0)
    {
        fprintf(stderr, "STCP smoke test failed\n");
        return 1;
    }
    BPLib_TCPCLA_Close(&sender); BPLib_TCPCLA_Close(&receiver);
    return 0;
}

typedef struct
{
    int fd;
    int status;
} TCPCLThreadArgs_t;

/** Connect as the active TCPCLv4 peer, negotiate, send the fixture, and record the result. */
static void *tcpclv4_active_thread(void *arg)
{
    TCPCLThreadArgs_t *args = arg;
    BPLib_TCPCLA_t sender = {0};
    const uint8_t sent[] = "TCPCLv4 segment transfer";
    args->status = BPLib_TCPCLA_InitConfig(&sender, 17000, 0, BPLib_TCPCLA_TCPCLV4,
                                           "ipn:1.0", 4, 17000, 0);
    if (args->status == BPLIB_SUCCESS) args->status = BPLib_TCPCLA_Attach(&sender, args->fd);
    if (args->status == BPLIB_SUCCESS) args->status = BPLib_TCPCLA_SessionStart(&sender, true, 2000);
    if (args->status == BPLIB_SUCCESS) args->status = BPLib_TCPCLA_Send(&sender, sent, sizeof(sent), 2000);
    BPLib_TCPCLA_Close(&sender);
    return NULL;
}

/** Run the passive peer against the active worker and check delivery. */
static int test_tcpclv4(void)
{
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) return 1;
    TCPCLThreadArgs_t thread_args = {.fd = sockets[0], .status = BPLIB_ERROR};
    pthread_t thread;
    if (pthread_create(&thread, NULL, tcpclv4_active_thread, &thread_args) != 0) return 1;
    BPLib_TCPCLA_t receiver = {0};
    uint8_t received[64] = {0};
    size_t received_size = 0;
    BPLib_Status_t status = BPLib_TCPCLA_InitConfig(&receiver, 17000, 0, BPLib_TCPCLA_TCPCLV4,
                                                    "ipn:2.0", 4, 17000, 0);
    if (status == BPLIB_SUCCESS) status = BPLib_TCPCLA_Attach(&receiver, sockets[1]);
    if (status == BPLIB_SUCCESS) status = BPLib_TCPCLA_SessionStart(&receiver, false, 2000);
    if (status == BPLIB_SUCCESS) status = BPLib_TCPCLA_Receive(&receiver, received, sizeof(received), &received_size, 2000);
    BPLib_TCPCLA_Close(&receiver);
    (void)pthread_join(thread, NULL);
    const uint8_t expected[] = "TCPCLv4 segment transfer";
    if (status != BPLIB_SUCCESS || thread_args.status != BPLIB_SUCCESS ||
        received_size != sizeof(expected) || memcmp(received, expected, sizeof(expected)) != 0)
    {
        fprintf(stderr, "TCPCLv4 smoke test failed (receiver=%d active=%d)\n", status, thread_args.status);
        return 1;
    }
    return 0;
}

/** Run both protocol round trips and return a failure if either check fails. */
int main(void)
{
    if (test_stcp() != 0 || test_tcpclv4() != 0) return 1;
    puts("TCP CLA STCP and TCPCLv4 round trips succeeded");
    return 0;
}
