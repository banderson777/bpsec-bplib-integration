#include "bpnode_app.h"
#undef NDEBUG
#include <assert.h>
#include <stdio.h>

BPNode_AppData_t BPNode_AppData;
static unsigned calls;
static BPLib_Status_t receive_status;
/** Report a started contact so the ingress task reaches the receive path. */
BPLib_Status_t BPLib_CLA_GetContactRunState(uint32_t id, BPLib_CLA_ContactRunState_t *state)
{ (void)id; *state = BPLIB_CLA_STARTED; return BPLIB_SUCCESS; }
/** Check the TCP-specific receive timeout and return the fixture transport result. */
BPLib_Status_t BPLib_TCPCLA_Receive(BPLib_TCPCLA_t *cla, void *buf, size_t capacity, size_t *size, uint32_t timeout)
{
    (void)cla; (void)buf; (void)capacity;
    assert(timeout == BPNODE_TCPCLA_IO_TIMEOUT_MSEC);
    /* Bound the old infinite loop so the regression fails immediately. */
    assert(++calls == 1); *size = 0; return receive_status;
}
/** Stub bundle enqueueing; the failure cases exercise the transport before queue insertion. */
BPLib_Status_t BPLib_CLA_Ingress(BPLib_Instance_t *inst, uint32_t id, const void *buf, size_t len, uint32_t timeout)
{ (void)inst; (void)id; (void)buf; (void)len; (void)timeout; return BPLIB_SUCCESS; }
/** Accept the ingress performance entry without cFS instrumentation. */
BPLib_Status_t BPLib_PL_PerfLogEntry(uint32_t id) { (void)id; return BPLIB_SUCCESS; }
/** Accept the matching performance exit without cFS instrumentation. */
BPLib_Status_t BPLib_PL_PerfLogExit(uint32_t id) { (void)id; return BPLIB_SUCCESS; }
/** Accept test events without starting flight telemetry services. */
BPLib_Status_t BPLib_EM_SendEvent(uint16_t id, BPLib_EM_EventType_t type, const char *format, ...)
{ (void)id; (void)type; (void)format; return BPLIB_SUCCESS; }
/** Return a software-bus timeout with no received message. */
CFE_Status_t CFE_SB_ReceiveBuffer(CFE_SB_Buffer_t **buf, CFE_SB_PipeId_t id, int32 timeout)
{ (void)id; (void)timeout; *buf = NULL; return CFE_SB_TIME_OUT; }
/** Report a zero-length fixture message without decoding a cFS header. */
CFE_Status_t CFE_MSG_GetSize(const CFE_MSG_Message_t *msg, CFE_MSG_Size_t *size)
{ (void)msg; *size = 0; return CFE_SUCCESS; }
/** Treat the fixture message itself as its user data. */
void *CFE_SB_GetUserData(CFE_MSG_Message_t *msg) { return msg; }
#ifdef DEFAULT_UDP_CLA
/** Return a synthetic timeout when the UDP driver path is compiled in. */
int32 CFE_PSP_IODriver_Command(const CFE_PSP_IODriver_Location_t *location, uint32 cmd, CFE_PSP_IODriver_Arg_t arg)
{ (void)location; (void)cmd; (void)arg; return CFE_PSP_ERROR_TIMEOUT; }
#endif
/** Run the actual ingress task against instrumented transport and framework boundaries. */
int main(void)
{
    BPNode_AppData.BplibInst.ContCtxt[0].Config.CLAType = BPLib_TCP_CLA;
    BPNode_AppData.BplibInst.ContCtxt[0].Config.IngressBitsPerCycle = 100;
    BPLib_Status_t cases[] = {BPLIB_CLA_TIMEOUT, BPLIB_OS_ERROR, BPLIB_APP_STATE_ERR, BPLIB_BUF_LEN_ERROR};
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
    {
        calls = 0; receive_status = cases[i]; BPNode_ClaIn_TaskMain(0); assert(calls == 1);
    }
    puts("PASS actual BPNode ingress task returns after timeout, disconnect, closed stream, and malformed frame");
    return 0;
}
