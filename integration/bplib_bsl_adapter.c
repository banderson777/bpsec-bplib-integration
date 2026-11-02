#include "bplib_bsl_adapter.h"
#include "bplib_mem.h"
#include "bsl/front/BSLMemory.h"
#include "bsl/mock_bpa/bundle.h"
#include "bsl/mock_bpa/decode.h"
#include "bsl/mock_bpa/encode.h"
#include "bsl/dynamic/CBOR.h"
#include <qcbor/qcbor_decode.h>
#include <string.h>

static BPLib_Bundle_t *bundle(const BSL_BundleRef_t *r)
{
    return (r && r->data) ? (BPLib_Bundle_t *)r->data : NULL;
}

int BPLib_BSL_BundleMetadata(const BSL_BundleRef_t *r, BSL_PrimaryBlock_t *out)
{
    BPLib_Bundle_t *b = bundle(r);
    if (!b || !out) return -1;
    BSL_PrimaryBlock_Init(out);
    out->field_version = BPLIB_BUNDLE_PROTOCOL_VERSION;
    out->field_flags = b->blocks.PrimaryBlock.BundleProcFlags;
    out->field_crc_type = b->blocks.PrimaryBlock.CrcType;
    out->field_bundle_creation_time = b->blocks.PrimaryBlock.Timestamp.CreateTime;
    out->field_seq_num = b->blocks.PrimaryBlock.Timestamp.SequenceNumber;
    out->field_lifetime = b->blocks.PrimaryBlock.Lifetime;
    out->field_frag_offset = b->blocks.PrimaryBlock.FragmentOffset;
    out->field_adu_length = b->blocks.PrimaryBlock.TotalAduLength;
    out->block_count = 1;
    out->block_numbers = BSL_calloc(1, sizeof(*out->block_numbers));
    if (!out->block_numbers) return -2;
    out->block_numbers[0] = b->blocks.PayloadHeader.BlockNum;
    return 0;
}

int BPLib_BSL_BlockMetadata(const BSL_BundleRef_t *r, uint64_t n, BSL_CanonicalBlock_t *out)
{
    BPLib_Bundle_t *b = bundle(r);
    if (!b || !out) return -1;
    memset(out, 0, sizeof(*out));
    if (n != b->blocks.PayloadHeader.BlockNum) return -2;
    out->block_num = n;
    out->type_code = b->blocks.PayloadHeader.BlockType;
    out->flags = b->blocks.PayloadHeader.BlockProcFlags;
    out->crc_type = b->blocks.PayloadHeader.CrcType;
    out->btsd_len = b->blocks.PayloadHeader.DataSize;
    return 0;
}

int BPLib_BSL_WireRoundTrip(const BSL_Data_t *wire_in, BSL_Data_t *wire_out)
{
    if (!wire_in || !wire_out || !wire_in->ptr || !wire_in->len) return -1;
    MockBPA_Bundle_t b;
    if (MockBPA_Bundle_Init(&b)) return -2;
    QCBORDecodeContext dec;
    QCBORDecode_Init(&dec, (UsefulBufC){wire_in->ptr, wire_in->len}, QCBOR_DECODE_MODE_NORMAL);
    int rc = bsl_mock_decode_bundle(&dec, &b);
    if (!rc) rc = BSL_CBOR_Encode_Twopass(wire_out, (BSL_CBOR_Encode_f)bsl_mock_encode_bundle, &b);
    MockBPA_Bundle_Deinit(&b);
    return rc;
}
