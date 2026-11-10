/* Exercise the actual canonical decoder and QCBOR; stub unrelated host services. */
#include "bplib_cbor_internal.h"
#undef NDEBUG
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

BPLib_EID_t BPLIB_EID_INSTANCE;
/** Enter the canonical block array through QCBOR and translate its error status. */
BPLib_Status_t BPLib_QCBOR_EnterDefiniteArray(QCBORDecodeContext *ctx, QCBORItem *item)
{
    QCBORDecode_EnterArray(ctx, item);
    return QCBORDecode_GetError(ctx) == QCBOR_SUCCESS ? BPLIB_SUCCESS : BPLIB_ERROR;
}
/** Close the current QCBOR array and translate its error status. */
BPLib_Status_t BPLib_QCBOR_ExitArray(QCBORDecodeContext *ctx)
{
    QCBORDecode_ExitArray(ctx);
    return QCBORDecode_GetError(ctx) == QCBOR_SUCCESS ? BPLIB_SUCCESS : BPLIB_ERROR;
}
/** Read one canonical unsigned integer from the test decoder. */
BPLib_Status_t BPLib_QCBOR_UInt64ParserImpl(QCBORDecodeContext *ctx, uint64_t *value)
{
    QCBORDecode_GetUInt64(ctx, value);
    return QCBORDecode_GetError(ctx) == QCBOR_SUCCESS ? BPLIB_SUCCESS : BPLIB_ERROR;
}
/** Fail if the bounds fixture unexpectedly reaches endpoint parsing. */
BPLib_Status_t BPLib_QCBOR_EidDtnNoneParserImpl(QCBORDecodeContext *ctx, BPLib_EID_t *eid)
{ (void)ctx; (void)eid; assert(!"unexpected EID parsing"); return BPLIB_ERROR; }
/** Fail if the bounds fixture unexpectedly reaches administrative record parsing. */
BPLib_Status_t BPLib_QCBOR_AdminRecordContentParserImpl(QCBORDecodeContext *ctx, BPLib_ARP_AdminRecord_t *record)
{ (void)ctx; (void)record; assert(!"unexpected admin record parsing"); return BPLIB_ERROR; }
/** Accept the fixture's no-CRC field and report a zero checksum. */
BPLib_Status_t BPLib_QCBOR_CRCParserImpl(QCBORDecodeContext *ctx, uint64_t *value, uint64_t type)
{ (void)ctx; *value = 0; assert(type == 0); return BPLIB_SUCCESS; }
/** Assert that the canonical fixture requests no CRC validation. */
BPLib_Status_t BPLib_CBOR_ValidateBlockCrc(const void *wire, BPLib_CRC_Type_t type, BPLib_CRC_Val_t crc, size_t offset, size_t length)
{ (void)wire; (void)crc; (void)offset; (void)length; assert(type == 0); return BPLIB_SUCCESS; }
/** Stub endpoint matching; it is not part of the canonical block limit check. */
bool BPLib_EID_IsMatch(const BPLib_EID_t *a, const BPLib_EID_t *b)
{ (void)a; (void)b; return false; }
/** Fail if parsing unexpectedly tries to allocate an administrative record. */
BPLib_MEM_Block_t *BPLib_MEM_BlockAlloc(BPLib_MEM_Pool_t *pool, size_t size)
{ (void)pool; (void)size; assert(!"unexpected admin record allocation"); return NULL; }

/** Construct a minimal canonical record and decode it at the requested bundle slot. */
static BPLib_Status_t decode(unsigned type, unsigned index, BPLib_Bundle_t *bundle)
{
    /* An outer indefinite array containing [type, 6, 0, 0, h'01'].
     * The age block contains one integer. Payload bytes are opaque. */
    unsigned char wire[] = {0x9f, 0x85, (unsigned char)type, 6, 0, 0, 0x41, 1, 0xff};
    if (type == BPLib_BlockType_Payload) wire[3] = 1;
    QCBORDecodeContext ctx;
    QCBORDecode_Init(&ctx, (UsefulBufC){wire, sizeof(wire)}, QCBOR_DECODE_MODE_NORMAL);
    QCBORDecode_EnterArray(&ctx, NULL);
    return BPLib_CBOR_DecodeCanonical(NULL, &ctx, bundle, index, wire);
}
/** Check the last legal extension slot, payload slot, and rejection beyond the extension limit. */
int main(void)
{
    BPLib_Bundle_t *bundle = calloc(1, sizeof(*bundle)); assert(bundle);
    const unsigned types[] = {BPLib_BlockType_PrevNode, BPLib_BlockType_Age,
        BPLib_BlockType_HopCount, BPLib_BlockType_CTEB, 23};
    for (size_t i = 0; i < sizeof(types)/sizeof(types[0]); ++i)
    {
        assert(decode(types[i], BPLIB_MAX_NUM_EXTENSION_BLOCKS, bundle) == BPLIB_CBOR_DEC_CANON_BLOCK_INDEX_ERR);
        /* Rejection precedes any metadata writes. */
        const unsigned char *bytes = (const unsigned char *)bundle;
        for (size_t j = 0; j < sizeof(*bundle); ++j) assert(bytes[j] == 0);
    }
    assert(decode(BPLib_BlockType_Age, BPLIB_MAX_NUM_EXTENSION_BLOCKS - 1, bundle) == BPLIB_SUCCESS);
    assert(bundle->blocks.ExtBlocks[BPLIB_MAX_NUM_EXTENSION_BLOCKS - 1].BlockData.AgeBlockData.Age == 1);
    assert(decode(BPLib_BlockType_Payload, BPLIB_MAX_NUM_CANONICAL_BLOCKS, bundle) == BPLIB_CBOR_DEC_CANON_BLOCK_INDEX_ERR);
    assert(decode(BPLib_BlockType_Payload, BPLIB_MAX_NUM_EXTENSION_BLOCKS, bundle) == BPLIB_SUCCESS);
    free(bundle);
    puts("PASS excess extension blocks rejected before writes; last valid extension slot decodes");
    return 0;
}
