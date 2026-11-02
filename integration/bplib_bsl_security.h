#ifndef BPLIB_BSL_SECURITY_H
#define BPLIB_BSL_SECURITY_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Serialized process-local BSL wire bridge for the five-node lab policy.
 * Initialize once, before workers; close after they stop. Only endpoints load
 * the 32-byte key. BSL's Mock BPA codec supplies block creation/removal; input
 * and output are BPv7, not the Python demo record envelope.
 */
int BPLib_BSL_SecurityInit(const uint8_t key[32]);
void BPLib_BSL_SecurityClose(void);
/* Protect or accept payload block 1 of ipn:10.1 -> ipn:50.1. Acceptance requires
 * exactly one AES-256-GCM BCB from ipn:10.0, scope 7, targeting payload alone.
 * Output is written only on success; caller retains ownership of all buffers.
 */
int BPLib_BSL_SecurityTransform(bool protect, const void *input, size_t input_size,
                              void *output, size_t capacity, size_t *output_size);
#endif
