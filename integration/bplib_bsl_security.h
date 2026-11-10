#ifndef BPLIB_BSL_SECURITY_H
#define BPLIB_BSL_SECURITY_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/** @file
 * Process-local BSL wire bridge for the five-node endpoint policy.
 * Transform calls serialize internally. The caller must initialize before
 * starting workers and close only after all calls have returned.
 */

/** Load a 32-byte AES key into the endpoint's BSL key store.
 * The caller retains the key buffer. Returns 0 on success and -1 on invalid
 * input, repeated initialization, or a BSL setup failure.
 */
int BPLib_BSL_SecurityInit(const uint8_t key[32]);

/** Release the library and its key store. Safe to call when uninitialized. */
void BPLib_BSL_SecurityClose(void);

/** Encrypt (protect=true) or authenticate and decrypt (protect=false) payload
 * block 1 of a nonfragmented BPv7 bundle from ipn:10.1 to ipn:50.1.
 * Acceptance requires exactly one AES-256-GCM BCB from ipn:10.0 with scope 7,
 * targeting only the payload. Caller owns the input and output buffers.
 * Input must contain 1–17000 bytes; capacity is the writable output size.
 * Returns 0 and sets output_size to the encoded length on success. Returns
 * -1 for invalid input, failed policy or codec operations, or insufficient
 * capacity; output bytes are untouched and a nonnull output_size is set to 0.
 */
int BPLib_BSL_SecurityTransform(bool protect, const void *input, size_t input_size,
                              void *output, size_t capacity, size_t *output_size);
#endif
