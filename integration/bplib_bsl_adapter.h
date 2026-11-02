#ifndef BPLIB_BSL_ADAPTER_H
#define BPLIB_BSL_ADAPTER_H

#include "bsl/BPSecLib_Public.h"
#include "bplib_bblocks.h"

/** @file
 * Selected metadata bridge, not a complete BSL host service. Bundle references
 * borrow a live BPLib_Bundle_t through their data member. No callback mutates it.
 */

/** Initialize out and copy primary scalar fields plus the payload block number.
 * Endpoint IDs and extension block numbers are not populated. On success,
 * out owns a BSL-allocated block_numbers array; release it with
 * BSL_PrimaryBlock_deinit(). Pass fresh output storage, not an owning object.
 * Returns 0 on success, -1 for missing input/output, -2 for allocation failure.
 * On allocation failure out has been initialized and may be deinitialized.
 */
int BPLib_BSL_BundleMetadata(const BSL_BundleRef_t *, BSL_PrimaryBlock_t *);
/** Copy payload canonical metadata into caller-owned out (no allocations).
 * Returns 0 on success, -1 for missing input/output, -2 for a non-payload number.
 * With valid pointers, out is cleared before the block number is checked.
 */
int BPLib_BSL_BlockMetadata(const BSL_BundleRef_t *, uint64_t, BSL_CanonicalBlock_t *);

/** Decode/encode a CBOR bundle with the Mock BPA; no security processing occurs.
 * wire_in borrows nonempty bytes; wire_out must be a separate initialized empty
 * BSL_Data_t. Release its owned output with BSL_Data_Deinit(). Returns -1 for
 * invalid arguments, -2 for Mock BPA initialization failure, or codec status
 * (0 on success). The encoded representation need not be byte-identical.
 */
int BPLib_BSL_WireRoundTrip(const BSL_Data_t *wire_in, BSL_Data_t *wire_out);

#endif
