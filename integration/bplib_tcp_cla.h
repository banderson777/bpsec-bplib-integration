/*
 * Host-side TCP convergence-layer adapter for BPLib.
 *
 * Supports the legacy four-byte length-prefixed STCP-style stream and a
 * negotiated TCPCLv4 stream. Select the wire protocol with InitConfig().
 */
#ifndef BPLIB_TCP_CLA_H
#define BPLIB_TCP_CLA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bplib_cla.h"
#include "bplib_socket_compat.h"
#ifndef _WIN32
#include <pthread.h>
#endif

typedef struct BPLib_TCPCLA_Pending BPLib_TCPCLA_Pending_t;

typedef enum
{
    BPLib_TCPCLA_STCP = 0,
    BPLib_TCPCLA_TCPCLV4 = 1
} BPLib_TCPCLA_Protocol_t;

/** Owned connection state; keep at a stable address and do not copy or mutate
 * fields during I/O. Initialize once, Close for reuse, Destroy to end lifetime.
 * Setup/handshake require exclusive ownership. Send calls serialize; Receive
 * returns CLA_TIMEOUT immediately if the shared dispatcher lock is busy.
 */
typedef struct
{
    intptr_t ListenFd;
    intptr_t SocketFd;
    size_t   MaxBundleSize;
    uint32_t ContactId;
    BPLib_TCPCLA_Protocol_t Protocol;
    bool     SessionReady;
    uint64_t NextTransferId;
    uint64_t SegmentMru;
    uint64_t TransferMru;
    uint16_t KeepaliveInterval;
    uint64_t ConfigSegmentMru, ConfigTransferMru;
    uint16_t ConfigKeepaliveInterval;
    char     NodeId[128];
#ifdef _WIN32
    SRWLOCK IoLock;
#else
    pthread_mutex_t IoLock;
#endif
    size_t IoProgress;
    uint64_t Deadline;
    uint8_t *RxData;
    size_t RxSize;
    uint64_t RxId;
    BPLib_TCPCLA_Pending_t *PendingHead, *PendingTail;
    unsigned PendingCount;
} BPLib_TCPCLA_t;

/** Initialize STCP with default size limits. Returns INVALID_CONFIG_ERR for
 * invalid size/contact/context; OS_ERROR if synchronization setup fails.
 * For all timeout parameters, zero means indefinite socket wait. Lock waits and
 * DNS resolution are outside socket deadlines. See docs/code-guide.md.
 */
BPLib_Status_t BPLib_TCPCLA_Init(BPLib_TCPCLA_t *Cla, size_t MaxBundleSize,
                                 uint32_t ContactId);
/** Initialize protocol and local receive limits (bytes), contact index, and
 * keepalive interval (seconds). TCPCLv4 requires a nonempty node ID <128 bytes.
 * MaxBundleSize is 1..UINT32_MAX. Zero TransferMru uses MaxBundleSize; zero
 * SegmentMru uses min(TransferMru, 16384). Require SegmentMru <= TransferMru <=
 * MaxBundleSize. NodeId is copied. No autonomous keepalive timer is provided.
 * Initialize only fresh storage; a failed initialization is not a usable context.
 */
BPLib_Status_t BPLib_TCPCLA_InitConfig(BPLib_TCPCLA_t *Cla, size_t MaxBundleSize,
                                       uint32_t ContactId, BPLib_TCPCLA_Protocol_t Protocol,
                                       const char *NodeId, uint64_t SegmentMru,
                                       uint64_t TransferMru, uint16_t KeepaliveInterval);
/** Return static protocol label; caller must not free it. */
const char *BPLib_TCPCLA_ProtocolName(BPLib_TCPCLA_Protocol_t Protocol);
/** Adopt a connected socket and make it nonblocking. Ownership transfers only
 * on success; caller retains ownership on error. Context must have no socket.
 */
BPLib_Status_t BPLib_TCPCLA_Attach(BPLib_TCPCLA_t *Cla, intptr_t SocketFd);
/** Close socket/listener, discard pending data, and restore configured limits.
 * Preserves the lock for reconnect; NULL is allowed. Requires initialized state.
 * Waits for the I/O lock, so it does not immediately cancel a blocked send.
 */
void BPLib_TCPCLA_Close(BPLib_TCPCLA_t *Cla);
/* Release synchronization resources after all callers have stopped. InitConfig
 * must be called once per lifetime; Close permits reconnect, Destroy ends it.
 * Connect/listen/attach/session setup require exclusive lifecycle ownership.
 * Send/Receive are concurrent-safe; a busy dispatcher returns CLA_TIMEOUT. */
void BPLib_TCPCLA_Destroy(BPLib_TCPCLA_t *Cla);

/** Connect to a resolved peer. Does not perform TCPCLv4 handshake. Each candidate
 * address gets its own timeout wait; DNS resolution is not timed here.
 */
BPLib_Status_t BPLib_TCPCLA_Connect(BPLib_TCPCLA_t *Cla, const char *Host,
                                    uint16_t Port, uint32_t TimeoutMs);
/** Bind a local address and listen with Backlog >0; no socket/listener may exist.
 * Host must be non-NULL (e.g. "0.0.0.0" for wildcard IPv4).
 */
BPLib_Status_t BPLib_TCPCLA_Listen(BPLib_TCPCLA_t *Cla, const char *Host,
                                   uint16_t Port, int Backlog);
/** Accept one peer from an existing listener. Failure closes the context,
 * including the listener; call Listen again before retrying. No handshake yet.
 */
BPLib_Status_t BPLib_TCPCLA_Accept(BPLib_TCPCLA_t *Cla, uint32_t TimeoutMs);
/** Perform TCPCLv4 contact/session negotiation on a connected socket. Active
 * sends first; passive receives first. Failure closes sockets and resets state.
 * STCP must skip this call. TLS and nonzero extensions are unsupported.
 */
BPLib_Status_t BPLib_TCPCLA_SessionStart(BPLib_TCPCLA_t *Cla, bool Active,
                                          uint32_t TimeoutMs);

/** Send one nonempty encoded bundle from borrowed bytes. TCPCLv4 requires a
 * ready session and waits for transfer ACKs, dispatching inbound segments too.
 * ACKs do not establish BP custody. Size/MRU violations return BUF_LEN_ERROR.
 * Socket/protocol errors or partial-transfer timeouts close the connection.
 */
BPLib_Status_t BPLib_TCPCLA_Send(BPLib_TCPCLA_t *Cla, const void *Bundle,
                                 size_t Size, uint32_t TimeoutMs);
/** Copy one completed bundle into caller storage and return its byte count.
 * With valid pointers, *Size starts at zero; use output only on success.
 * Insufficient capacity returns BUF_LEN_ERROR and may close the session.
 * Idle timeout preserves framing; a partial-frame error closes the session.
 */
BPLib_Status_t BPLib_TCPCLA_Receive(BPLib_TCPCLA_t *Cla, void *Bundle,
                                    size_t BufferSize, size_t *Size,
                                    uint32_t TimeoutMs);

/** Dequeue and send one bundle for ContactId. Allocates/frees a MaxBundleSize
 * buffer. Queue wait and send each get TimeoutMs; no automatic retry/requeue.
 * Propagates queue/send status; NO_STOR_ERR indicates buffer allocation failure.
 */
BPLib_Status_t BPLib_TCPCLA_Egress(BPLib_TCPCLA_t *Cla, BPLib_Instance_t *Inst,
                                   uint32_t TimeoutMs);
/** Receive and enqueue one bundle for ContactId using a temporary buffer.
 * Receive and queue insertion each get TimeoutMs. Propagates their status;
 * a queue failure after receive does not replay the consumed transport bytes.
 */
BPLib_Status_t BPLib_TCPCLA_Ingress(BPLib_TCPCLA_t *Cla, BPLib_Instance_t *Inst,
                                    uint32_t TimeoutMs);

#endif
