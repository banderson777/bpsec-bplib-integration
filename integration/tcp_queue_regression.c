/* Exercise both BPLib queue boundaries over a real local STCP connection. */
#include "bplib_tcp_cla.h"
#include "bplib_inst.h"
#undef NDEBUG
#include <assert.h>
#include <string.h>
#include <sys/socket.h>

static BPLib_Instance_t instance;
static const char payload[] = "queue transfer";
static unsigned enqueued, dequeued;
static BPLib_Status_t queue_status;

#ifdef TEST_ALLOC_FAILURE
static bool fail_allocation;
void *__real_malloc(size_t size);
/** Fail the next bridge allocation on demand; forward every other request. */
void *__wrap_malloc(size_t size)
{
    if (!fail_allocation) return __real_malloc(size);
    fail_allocation = false;
    return NULL;
}
#endif

/** Supply one encoded bundle and verify the bridge forwards queue parameters. */
BPLib_Status_t BPLib_CLA_Egress(BPLib_Instance_t *inst, uint32_t contact, void *buffer,
                                size_t *size, size_t capacity, uint32_t timeout)
{
    assert(inst == &instance && contact == 0 && timeout == 1000 && capacity == 64);
    ++dequeued;
    if (queue_status == BPLIB_SUCCESS)
    {
        memcpy(buffer, payload, sizeof(payload));
        *size = sizeof(payload);
    }
    return queue_status;
}

/** Verify the destination queue receives the exact bytes, contact and timeout. */
BPLib_Status_t BPLib_CLA_Ingress(BPLib_Instance_t *inst, uint32_t contact, const void *buffer,
                                 size_t size, uint32_t timeout)
{
    assert(inst == &instance && contact == 1 && timeout == 1000);
    assert(size == sizeof(payload) && memcmp(buffer, payload, size) == 0);
    ++enqueued;
    return queue_status;
}

/** Check success, rejected inputs, storage failures, and closed transports. */
int main(void)
{
    BPLib_TCPCLA_t source, sink;
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    assert(BPLib_TCPCLA_Init(&source, 64, 0) == BPLIB_SUCCESS);
    assert(BPLib_TCPCLA_Init(&sink, 64, 1) == BPLIB_SUCCESS);
    assert(BPLib_TCPCLA_Attach(&source, sockets[0]) == BPLIB_SUCCESS);
    assert(BPLib_TCPCLA_Attach(&sink, sockets[1]) == BPLIB_SUCCESS);
    assert(BPLib_TCPCLA_Ingress(NULL, &instance, 1000) == BPLIB_NULL_PTR_ERROR);
    assert(BPLib_TCPCLA_Ingress(&sink, NULL, 1000) == BPLIB_NULL_PTR_ERROR);
    assert(BPLib_TCPCLA_Egress(NULL, &instance, 1000) == BPLIB_NULL_PTR_ERROR);
    assert(BPLib_TCPCLA_Egress(&source, NULL, 1000) == BPLIB_NULL_PTR_ERROR);
#ifdef TEST_ALLOC_FAILURE
    fail_allocation = true;
    assert(BPLib_TCPCLA_Ingress(&sink, &instance, 1000) == BPLIB_NO_STOR_ERR);
    fail_allocation = true;
    assert(BPLib_TCPCLA_Egress(&source, &instance, 1000) == BPLIB_NO_STOR_ERR);
#endif
    queue_status = BPLIB_ERROR;
    assert(BPLib_TCPCLA_Egress(&source, &instance, 1000) == BPLIB_ERROR);
    queue_status = BPLIB_SUCCESS;
    assert(BPLib_TCPCLA_Egress(&source, &instance, 1000) == BPLIB_SUCCESS);
    assert(BPLib_TCPCLA_Ingress(&sink, &instance, 1000) == BPLIB_SUCCESS);
    assert(BPLib_TCPCLA_Egress(&source, &instance, 1000) == BPLIB_SUCCESS);
    queue_status = BPLIB_ERROR;
    assert(BPLib_TCPCLA_Ingress(&sink, &instance, 1000) == BPLIB_ERROR);
    BPLib_TCPCLA_Close(&sink);
    assert(BPLib_TCPCLA_Ingress(&sink, &instance, 1000) == BPLIB_APP_STATE_ERR);
    BPLib_TCPCLA_Close(&source);
    queue_status = BPLIB_SUCCESS;
    assert(BPLib_TCPCLA_Egress(&source, &instance, 1000) == BPLIB_APP_STATE_ERR);
    assert(enqueued == 2 && dequeued == 4);
    BPLib_TCPCLA_Destroy(&source);
    BPLib_TCPCLA_Destroy(&sink);
    return 0;
}
