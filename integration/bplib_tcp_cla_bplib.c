#include "bplib_tcp_cla.h"

#include <stdlib.h>

BPLib_Status_t BPLib_TCPCLA_Egress(BPLib_TCPCLA_t *cla, BPLib_Instance_t *inst,
                                   uint32_t timeout_ms)
{
    if (cla == NULL || inst == NULL) return BPLIB_NULL_PTR_ERROR;
    uint8_t *buffer = malloc(cla->MaxBundleSize);
    if (buffer == NULL) return BPLIB_NO_STOR_ERR;
    size_t size = 0;
    BPLib_Status_t status = BPLib_CLA_Egress(inst, cla->ContactId, buffer, &size,
                                             cla->MaxBundleSize, timeout_ms);
    if (status == BPLIB_SUCCESS) status = BPLib_TCPCLA_Send(cla, buffer, size, timeout_ms);
    free(buffer);
    return status;
}

BPLib_Status_t BPLib_TCPCLA_Ingress(BPLib_TCPCLA_t *cla, BPLib_Instance_t *inst,
                                    uint32_t timeout_ms)
{
    if (cla == NULL || inst == NULL) return BPLIB_NULL_PTR_ERROR;
    uint8_t *buffer = malloc(cla->MaxBundleSize);
    if (buffer == NULL) return BPLIB_NO_STOR_ERR;
    size_t size = 0;
    BPLib_Status_t status = BPLib_TCPCLA_Receive(cla, buffer, cla->MaxBundleSize,
                                                 &size, timeout_ms);
    if (status == BPLIB_SUCCESS)
        status = BPLib_CLA_Ingress(inst, cla->ContactId, buffer, size, timeout_ms);
    free(buffer);
    return status;
}
