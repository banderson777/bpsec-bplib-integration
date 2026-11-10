#include <stdio.h>
#include <string.h>
#include "bplib_version.h"
#include "bsl/front/Data.h"
#include "bsl/front/TextUtil.h"
#include "bplib_bsl_adapter.h"
#include "bplib_mem.h"
#include "bsl/BPSecLib_Public.h"

BSL_HostDescriptors_t MockBPA_Agent_Descriptors(void *agent);
#include <stdio.h>

/** Run the metadata bridge and Mock BPA wire round-trip smoke checks. Exit nonzero if setup,
 * conversion, or validation fails. */
int main(void)
{
    if (BSL_HostDescriptors_Set(MockBPA_Agent_Descriptors(NULL)) != 0) return 1;
    static const uint8_t payload[] = "BPLib payload -> BPSec adapter";
    BSL_Data_t input = BSL_DATA_INIT_VIEW(payload, sizeof(payload) - 1);
    BSL_Data_t encoded = BSL_DATA_INIT_NULL;
    BPLib_MEM_Block_t blob = {0};
    BPLib_Bundle_t bpl_bundle = {0};
    blob.user_data.BigData[0] = payload[0];
    bpl_bundle.blob = &blob;
    bpl_bundle.blocks.PrimaryBlock.BundleProcFlags = 0;
    bpl_bundle.blocks.PrimaryBlock.CrcType = BPLib_CRC_Type_None;
    bpl_bundle.blocks.PayloadHeader.BlockNum = 1;
    bpl_bundle.blocks.PayloadHeader.BlockType = BPLib_BlockType_Payload;
    bpl_bundle.blocks.PayloadHeader.DataSize = sizeof(payload) - 1;
    BSL_BundleRef_t ref = {.data = &bpl_bundle};
    BSL_PrimaryBlock_t metadata;
    if (BPLib_BSL_BundleMetadata(&ref, &metadata) != 0 || metadata.block_count != 1)
        return 1;
    BSL_PrimaryBlock_deinit(&metadata);
    FILE *fp = fopen(WIRE_FIXTURE, "rb");
    if (!fp) return 1;
    fseek(fp, 0, SEEK_END);
    long fixture_len = ftell(fp);
    rewind(fp);
    uint8_t *fixture = BSL_malloc((size_t)fixture_len);
    if (!fixture || fread(fixture, 1, (size_t)fixture_len, fp) != (size_t)fixture_len) return 1;
    fclose(fp);
    BSL_Data_t fixture_in = BSL_DATA_INIT_VIEW(fixture, (size_t)fixture_len);
    BSL_Data_t fixture_out = BSL_DATA_INIT_NULL;
    int wire_rc = BPLib_BSL_WireRoundTrip(&fixture_in, &fixture_out);
    BSL_free(fixture);
    if (wire_rc != 0 || fixture_out.len == 0) return 1;
    BSL_Data_Deinit(&fixture_out);
    if (BSL_TextUtil_Base16_Encode(&encoded, &input, true) != 0)
        return 1;
    printf("BPLib version: %u.%u.%u\n", BPLIB_MAJOR_VERSION,
           BPLIB_MINOR_VERSION, BPLIB_REVISION);
    printf("BPSec adapter encoded payload (%zu bytes): %.*s\n",
           encoded.len, (int)encoded.len, encoded.ptr);
    BSL_Data_Deinit(&encoded);
    return (encoded.ptr == NULL && encoded.len == 0) ? 0 : 1;
}
