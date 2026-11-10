#include "bplib_tcp_cla.h"
#undef NDEBUG
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/** Create and attach an isolated pair of connected test sockets, optionally with TCPCLv4 state. */
static void pair(BPLib_TCPCLA_t *a, BPLib_TCPCLA_t *b, bool v4)
{
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    assert(BPLib_TCPCLA_InitConfig(a, 64, 0, v4 ? BPLib_TCPCLA_TCPCLV4 : BPLib_TCPCLA_STCP,
                                   "ipn:1.0", 4, 64, 0) == BPLIB_SUCCESS);
    assert(BPLib_TCPCLA_InitConfig(b, 64, 0, a->Protocol, "ipn:2.0", 4, 64, 0) == BPLIB_SUCCESS);
    assert(BPLib_TCPCLA_Attach(a, sockets[0]) == BPLIB_SUCCESS);
    assert(BPLib_TCPCLA_Attach(b, sockets[1]) == BPLIB_SUCCESS);
    a->SessionReady = b->SessionReady = v4;
}

/** Write an exact test frame directly to the peer socket to exercise parser edge cases. */
static void raw(BPLib_TCPCLA_t *c, const void *data, size_t size)
{
    assert(write(c->SocketFd, data, size) == (ssize_t)size);
}

static volatile sig_atomic_t interruptions;
/** Handle the periodic test signal and bound the number of injected interrupts. */
static void interrupt_wait(int signo)
{
    (void)signo;
    /* Bound the old implementation's retry loop so failure is prompt. */
    if (++interruptions == 50)
    {
        struct itimerval stopped = {0};
        setitimer(ITIMER_REAL, &stopped, NULL);
    }
}

/** Read monotonic milliseconds for assertions about I/O elapsed time. */
static uint64_t monotonic_ms(void)
{
    struct timespec ts;
    assert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/** Check that signals do not extend finite receive and accept deadlines. */
static void interrupted_timeouts(void)
{
    struct sigaction handler = {0}, previous;
    handler.sa_handler = interrupt_wait;
    sigemptyset(&handler.sa_mask);
    assert(sigaction(SIGALRM, &handler, &previous) == 0);
    for (unsigned accepting = 0; accepting < 2; ++accepting)
    {
        BPLib_TCPCLA_t a, b;
        if (accepting)
        {
            assert(BPLib_TCPCLA_Init(&a, 64, 0) == BPLIB_SUCCESS);
            assert(BPLib_TCPCLA_Listen(&a, "127.0.0.1", 0, 1) == BPLIB_SUCCESS);
        }
        else pair(&a, &b, false);
        struct itimerval timer = {{0, 10000}, {0, 10000}}, stopped = {0};
        interruptions = 0;
        assert(setitimer(ITIMER_REAL, &timer, NULL) == 0);
        uint64_t start = monotonic_ms();
        char data[64]; size_t size;
        BPLib_Status_t status = accepting ? BPLib_TCPCLA_Accept(&a, 40) :
            BPLib_TCPCLA_Receive(&a, data, sizeof(data), &size, 40);
        uint64_t elapsed = monotonic_ms() - start;
        assert(setitimer(ITIMER_REAL, &stopped, NULL) == 0);
        assert(status == BPLIB_CLA_TIMEOUT && interruptions > 0);
        /* Wide scheduling allowance, still below the old ~540 ms behavior. */
        assert(elapsed >= 40 && elapsed < 300);
        if (!accepting)
        {
            assert(a.SocketFd != BPLIB_INVALID_SOCKET);
            raw(&b, "\0\0\0\1x", 5);
            assert(BPLib_TCPCLA_Receive(&a, data, sizeof(data), &size, 0) == BPLIB_SUCCESS);
            assert(size == 1 && data[0] == 'x');
            BPLib_TCPCLA_Destroy(&b);
        }
        else assert(a.ListenFd == BPLIB_INVALID_SOCKET);
        BPLib_TCPCLA_Destroy(&a);
    }
    assert(sigaction(SIGALRM, &previous, NULL) == 0);
    puts("PASS interrupted receive/accept preserve deadlines and zero-timeout receive works");
}

/** Distinguish resumable idle timeouts from partial transfers that close a session. */
static void timeouts(void)
{
    BPLib_TCPCLA_t a, b;
    char out[64]; size_t n;
    pair(&a, &b, false);
    assert(BPLib_TCPCLA_Receive(&b, out, sizeof(out), &n, 10) == BPLIB_CLA_TIMEOUT);
    assert(b.SocketFd != BPLIB_INVALID_SOCKET);
    raw(&a, "\0\0", 2);
    assert(BPLib_TCPCLA_Receive(&b, out, sizeof(out), &n, 10) == BPLIB_CLA_TIMEOUT);
    assert(b.SocketFd == BPLIB_INVALID_SOCKET);
    assert(BPLib_TCPCLA_Receive(&b, out, sizeof(out), &n, 10) == BPLIB_APP_STATE_ERR);
    BPLib_TCPCLA_Destroy(&a); BPLib_TCPCLA_Destroy(&b);
    pair(&a, &b, false);
    raw(&a, "\0\0\0\4AB", 6);
    assert(BPLib_TCPCLA_Receive(&b, out, sizeof(out), &n, 10) == BPLIB_CLA_TIMEOUT);
    assert(b.SocketFd == BPLIB_INVALID_SOCKET);
    BPLib_TCPCLA_Destroy(&a); BPLib_TCPCLA_Destroy(&b);
    unsigned char segment[26] = {1, 3}; segment[9] = 1; segment[21] = 4;
    for (size_t prefix = 1; prefix < sizeof(segment); ++prefix)
    {
        pair(&a, &b, true); raw(&a, segment, prefix);
        assert(BPLib_TCPCLA_Receive(&b, out, sizeof(out), &n, 10) != BPLIB_SUCCESS);
        assert(b.SocketFd == BPLIB_INVALID_SOCKET && !b.SessionReady);
        BPLib_TCPCLA_Destroy(&a); BPLib_TCPCLA_Destroy(&b);
    }
    puts("PASS idle timeout preserves stream; every partial frame closes stream");
}

/** Exercise transfer size limits and disconnected peer handling. */
static void limits_and_disconnect(void)
{
    BPLib_TCPCLA_t a, b; char out[64]; size_t n;
    pair(&a, &b, true); b.TransferMru = 4;
    unsigned char first[26] = {1,2}, last[22] = {1,1};
    first[9] = last[9] = 1; first[21] = last[17] = 4;
    raw(&a, first, sizeof(first)); raw(&a, last, sizeof(last));
    assert(BPLib_TCPCLA_Receive(&b, out, sizeof(out), &n, 100) == BPLIB_BUF_LEN_ERROR);
    assert(n == 0 && b.SocketFd == BPLIB_INVALID_SOCKET);
    BPLib_TCPCLA_Destroy(&a); BPLib_TCPCLA_Destroy(&b);
    pair(&a, &b, false); BPLib_TCPCLA_Destroy(&b);
    signal(SIGPIPE, SIG_DFL);
    assert(BPLib_TCPCLA_Send(&a, "x", 1, 100) != BPLIB_SUCCESS);
    assert(a.SocketFd == BPLIB_INVALID_SOCKET);
    BPLib_TCPCLA_Destroy(&a);
    puts("PASS cumulative transfer MRU; closed peer returns error without SIGPIPE");
}

typedef struct { BPLib_TCPCLA_t *cla; bool sending; int status; char data[16]; } Worker;
/** Run the selected send or receive role. Retry busy or idle receive timeouts,
 * verify delivered bytes, and save the final transport status. */
static void *worker(void *arg)
{
    Worker *w = arg; size_t n = 0;
    if (w->sending) w->status = BPLib_TCPCLA_Send(w->cla, "abcdefgh", 8, 2000);
    else
    {
        for (unsigned i = 0; i < 4000; ++i)
        {
            w->status = BPLib_TCPCLA_Receive(w->cla, w->data, sizeof(w->data), &n, 20);
            if (w->status != BPLIB_CLA_TIMEOUT) break;
            struct timespec pause = {0, 1000000}; nanosleep(&pause, NULL);
        }
        if (w->status == BPLIB_SUCCESS) assert(n == 8 && memcmp(w->data, "abcdefgh", 8) == 0);
    }
    return NULL;
}

/** Exercise dispatch while both peers send and acknowledge transfers. */
static void concurrent(void)
{
    for (unsigned run = 0; run < 50; ++run)
    {
        BPLib_TCPCLA_t a, b; pair(&a, &b, true);
        Worker workers[4] = {{.cla=&a,.sending=true}, {.cla=&b,.sending=true},
                             {.cla=&a}, {.cla=&b}};
        pthread_t threads[4];
        for (unsigned i = 0; i < 4; ++i) assert(pthread_create(&threads[i], NULL, worker, &workers[i]) == 0);
        for (unsigned i = 0; i < 4; ++i)
        { assert(pthread_join(threads[i], NULL) == 0); assert(workers[i].status == BPLIB_SUCCESS); }
        BPLib_TCPCLA_Destroy(&a); BPLib_TCPCLA_Destroy(&b);
    }
    puts("PASS 50 simultaneous bidirectional transfers with concurrent ingress/egress");
}

/** Delay the receive worker to model a peer serviced on a later wakeup. */
static void *delayed_receiver(void *arg)
{
    struct timespec pause = {0, 60000000};
    nanosleep(&pause, NULL);
    return worker(arg);
}

/** Verify the sender can complete when its peer dispatches the ACK later. */
static void delayed_ack(void)
{
    BPLib_TCPCLA_t a, b; pair(&a, &b, true);
    Worker receiver = {.cla = &b};
    pthread_t thread;
    assert(pthread_create(&thread, NULL, delayed_receiver, &receiver) == 0);
    /* Peer dispatch may start on a later task wakeup, after a queue poll budget. */
    assert(BPLib_TCPCLA_Send(&a, "abcdefgh", 8, 1000) == BPLIB_SUCCESS);
    assert(pthread_join(thread, NULL) == 0 && receiver.status == BPLIB_SUCCESS);
    BPLib_TCPCLA_Destroy(&a); BPLib_TCPCLA_Destroy(&b);
    puts("PASS TCPCL ACK may arrive after a later peer task wakeup");
}

/** Exercise the adapter lifecycle through setup errors and cleanup. */
static void startup_cleanup(void)
{
    BPLib_TCPCLA_t a, b;
    assert(BPLib_TCPCLA_Init(&a, 64, 0) == BPLIB_SUCCESS);
    assert(BPLib_TCPCLA_Listen(&a, "127.0.0.1", 0, 1) == BPLIB_SUCCESS);
    assert(BPLib_TCPCLA_Accept(&a, 10) == BPLIB_CLA_TIMEOUT);
    assert(a.ListenFd == BPLIB_INVALID_SOCKET);
    assert(BPLib_TCPCLA_Listen(&a, "127.0.0.1", 0, 1) == BPLIB_SUCCESS);
    BPLib_TCPCLA_Destroy(&a);
    pair(&a, &b, true); raw(&b, "bad!!!", 6);
    assert(BPLib_TCPCLA_SessionStart(&a, false, 100) != BPLIB_SUCCESS);
    assert(a.SocketFd == BPLIB_INVALID_SOCKET && !a.SessionReady);
    a.SegmentMru = 1; a.TransferMru = 2; a.NextTransferId = 99;
    BPLib_TCPCLA_Close(&a);
    assert(a.SegmentMru == 4 && a.TransferMru == 64 && a.NextTransferId == 1);
    assert(BPLib_TCPCLA_Listen(&a, "127.0.0.1", 0, 1) == BPLIB_SUCCESS);
    BPLib_TCPCLA_Destroy(&a); BPLib_TCPCLA_Destroy(&b);
    puts("PASS failed accept/handshake clean up, allow retry, and restore configured limits");
}

/** Run timeout, bounds, dispatch, and connection lifecycle regressions. */
int main(void)
{
    interrupted_timeouts(); timeouts(); limits_and_disconnect(); concurrent(); delayed_ack(); startup_cleanup();
    return 0;
}
