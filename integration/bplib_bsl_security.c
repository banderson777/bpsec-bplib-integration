#include "bplib_bsl_security.h"
#include <bsl/BPSecLib_Private.h>
#include <bsl/dynamic/PublicInterfaceImpl.h>
#include <bsl/dynamic/SecurityActionSet.h>
#include <bsl/dynamic/AbsSecBlock.h>
#include <bsl/dynamic/CBOR.h>
#include <bsl/default_sc/DefaultSecContext.h>
#include <bsl/mock_bpa/MockBPA.h>
#include <bsl/mock_bpa/ctr.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static BSL_LibCtx_t library;
static bool initialized;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static const char key_name[] = "five-node-video";

/** Mock BPA callback that constructs the fixed security-source EID for this endpoint policy. */
static int security_source(void *unused, BSL_HostEID_t *eid)
{
    (void)unused;
    return mock_bpa_eid_from_text(eid, "ipn:10.0", NULL);
}

/** Match the decoded endpoint against a zero-authority IPN node and service pair. */
static bool ipn(const BSL_HostEID_t *eid, uint64_t node, uint64_t service)
{
    const bsl_mock_eid_t *value = eid->handle;
    return value && value->scheme == BSL_MOCK_EID_IPN &&
        value->ssp.as_ipn.auth_num == 0 && value->ssp.as_ipn.node_num == node &&
        value->ssp.as_ipn.svc_num == service;
}

/** Install Mock BPA descriptors, load the endpoint key, and register the AES-GCM context before
 * workers start. Release partially initialized state on failure. */
int BPLib_BSL_SecurityInit(const uint8_t key[32])
{
    if (!key || initialized) return -1;
    BSL_HostDescriptors_t descriptors = MockBPA_Agent_Descriptors(NULL);
    descriptors.get_sec_src_eid_fn = security_source;
    if (BSL_HostDescriptors_Set(descriptors)) return -1;
    MockBPA_KeyStore_Init();
    BSL_Crypto_KeyHandle_t handle = NULL;
    BSL_Data_t name = BSL_DATA_INIT_VIEW((uint8_t *)key_name, sizeof(key_name) - 1);
    int result = BSL_Crypto_LoadKey(key, 32, &handle);
    if (!result) result = MockBPA_KeyStore_AddKey(&name, handle);
    BSL_Crypto_ReleaseKeyHandle(handle);
    if (!result) result = BSL_API_InitLib(&library);
    if (result) { MockBPA_KeyStore_Deinit(); BSL_HostDescriptors_Clear(); return -1; }
    BSL_SecCtxDesc_t context = {.execute = BSLX_BCB_Execute, .validate = BSLX_BCB_Validate};
    if (BSL_API_RegisterSecurityContext(&library, RFC9173_CONTEXTID_BCB_AES_GCM, context))
    {
        int cleanup_status = BSL_API_DeinitLib(&library); (void)cleanup_status; MockBPA_KeyStore_Deinit(); BSL_HostDescriptors_Clear();
        return -1;
    }
    initialized = true;
    return 0;
}

/** Release the process-local context, key store, and host descriptors once workers have
 * stopped. */
void BPLib_BSL_SecurityClose(void)
{
    if (!initialized) return;
    int cleanup_status = BSL_API_DeinitLib(&library); (void)cleanup_status;
    MockBPA_KeyStore_Deinit();
    BSL_HostDescriptors_Clear();
    initialized = false;
}

/** Check that a required abstract security block parameter exists and has the expected integer
 * value. */
static bool parameter(const BSL_AbsSecBlock_t *asb, int64_t id, int64_t expected)
{
    BSLB_VariantPtr_t *const *entry = BSLB_VariantPtrMap_cget(asb->params, id);
    int64_t value;
    return entry && !BSL_Variant_GetAsInt64(BSLB_VariantPtr_cref(*entry), &value) && value == expected;
}

/** Decode a BCB and validate its declared security source, context, scope, and single payload
 * target before attempting authentication. */
static bool accept_policy(const MockBPA_CanonicalBlock_t *block)
{
    BSL_AbsSecBlock_t asb;
    BSL_AbsSecBlock_Init(&asb);
    BSL_Data_t data = BSL_DATA_INIT_VIEW(block->btsd, block->btsd_len);
    bool valid = !BSL_CBOR_Decode(&data, (BSL_CBOR_Decode_f)BSL_AbsSecBlock_Decode, &asb);
    valid = valid && asb.sec_context_id == RFC9173_CONTEXTID_BCB_AES_GCM &&
        ipn(&asb.source_eid, 10, 0) && BSL_AbsSecBlock_TargetList_size(asb.target_results) == 1 &&
        parameter(&asb, RFC9173_BCB_SECPARAM_AESVARIANT, RFC9173_BCB_AES_VARIANT_A256GCM) &&
        parameter(&asb, RFC9173_BCB_SECPARAM_AADSCOPE, 7);
    if (valid)
    {
        BSL_AbsSecBlock_TargetPtr_t *const *target = BSL_AbsSecBlock_TargetList_cget(asb.target_results, 0);
        valid = BSL_AbsSecBlock_TargetPtr_cref(*target)->target_block_num == 1;
    }
    BSL_AbsSecBlock_Deinit(&asb);
    return valid;
}

/* Optional demo-only audit trail used to compare source IVs across restarts. */
static int audit_source_iv(const MockBPA_Bundle_t *bundle)
{
    const char *path = getenv("BPNODE_DEMO_IV_AUDIT");
    if (!path || !path[0]) return 0;
    MockBPA_BlockList_it_t iterator;
    for (MockBPA_BlockList_it(iterator, bundle->blocks); !MockBPA_BlockList_end_p(iterator); MockBPA_BlockList_next(iterator))
    {
        const MockBPA_CanonicalBlock_t *block = MockBPA_BlockList_ref(iterator);
        if (block->blk_type != 12) continue;
        BSL_AbsSecBlock_t asb;
        BSL_AbsSecBlock_Init(&asb);
        BSL_Data_t data = BSL_DATA_INIT_VIEW(block->btsd, block->btsd_len);
        int status = BSL_CBOR_Decode(&data, (BSL_CBOR_Decode_f)BSL_AbsSecBlock_Decode, &asb);
        BSLB_VariantPtr_t *const *entry = status ? NULL : BSLB_VariantPtrMap_cget(asb.params, 1);
        BSL_Data_t iv = BSL_DATA_INIT_NULL;
        if (!entry || BSL_Variant_GetAsBytestr(BSLB_VariantPtr_cref(*entry), &iv) || iv.len != 12)
        {
            BSL_AbsSecBlock_Deinit(&asb);
            return -1;
        }
        FILE *stream = fopen(path, "a");
        if (!stream) { BSL_Data_Deinit(&iv); BSL_AbsSecBlock_Deinit(&asb); return -1; }
        for (size_t i = 0; i < iv.len; ++i) fprintf(stream, "%02x", iv.ptr[i]);
        fputc('\n', stream);
        int failed = ferror(stream);
        if (fclose(stream)) failed = 1;
        BSL_Data_Deinit(&iv);
        BSL_AbsSecBlock_Deinit(&asb);
        return failed ? -1 : 0;
    }
    return -1;
}

/** Serialize one protect or accept operation under the bridge mutex. Publish encoded bytes only
 * after policy, crypto, and capacity checks pass. */
int BPLib_BSL_SecurityTransform(bool protect, const void *input, size_t input_size,
                              void *output, size_t capacity, size_t *output_size)
{
    if (output_size) *output_size = 0;
    if (!initialized || !input || !output || !output_size || !input_size || input_size > 17000) return -1;
    pthread_mutex_lock(&mutex);
    int result = -1;
    mock_bpa_ctr_t container;
    mock_bpa_ctr_init(&container);
    if (BSL_Data_CopyFrom(&container.encoded, input_size, input) || mock_bpa_ctr_decode(&container)) goto done;
    MockBPA_Bundle_t *bundle = container.bundle;
    if (!ipn(&bundle->primary_block.src_node_id, 10, 1) ||
        !ipn(&bundle->primary_block.dest_eid, 50, 1) || bundle->primary_block.flags & 3) goto done;
    uint64_t security_number = 0, next_number = 2;
    unsigned payloads = 0, security_blocks = 0;
    MockBPA_BlockList_it_t iterator;
    for (MockBPA_BlockList_it(iterator, bundle->blocks); !MockBPA_BlockList_end_p(iterator); MockBPA_BlockList_next(iterator))
    {
        MockBPA_CanonicalBlock_t *block = MockBPA_BlockList_ref(iterator);
        if (block->blk_num >= next_number)
        {
            if (block->blk_num == UINT64_MAX) goto done;
            next_number = block->blk_num + 1;
        }
        if (block->blk_type == 1 && block->blk_num == 1) payloads++;
        if (block->blk_type == 11) goto done; /* This lab policy is BCB-only. */
        if (block->blk_type == 12)
        {
            security_blocks++; security_number = block->blk_num;
            if (protect || !accept_policy(block)) goto done;
        }
    }
    if (payloads != 1 || (!protect && security_blocks != 1)) goto done;
    BSL_SecOper_t operation;
    BSL_SecurityAction_t action;
    BSL_SecurityActionSet_t actions;
    BSL_SecOper_Init(&operation);
    BSL_SecurityAction_Init(&action);
    BSL_SecurityActionSet_Init(&actions);
    BSL_SecOper_Populate(&operation, RFC9173_CONTEXTID_BCB_AES_GCM, 1,
        protect ? next_number : security_number, BSL_SECBLOCKTYPE_BCB,
        protect ? BSL_SECROLE_SOURCE : BSL_SECROLE_ACCEPTOR, BSL_POLICYACTION_DROP_BUNDLE);
    BSL_Variant_SetTextstr(BSL_SecOper_AddOption(&operation, BSLX_BCB_OPT_KEY_ID), key_name);
    BSL_Variant_SetInt64(BSL_SecOper_AddOption(&operation, BSLX_BCB_OPT_SCOPE), 7);
    BSL_Variant_SetInt64(BSL_SecOper_AddOption(&operation, BSLX_BCB_OPT_AES_VARIANT), RFC9173_BCB_AES_VARIANT_A256GCM);
    BSL_Variant_SetInt64(BSL_SecOper_AddOption(&operation, BSLX_BCB_OPT_USE_KEY_WRAP), 0);
    int appended = BSL_SecurityAction_AppendSecOper(&action, &operation);
    /* AppendSecOper moves ownership; do not deinitialize the moved value. */
    if (!appended) BSL_SecOper_Init(&operation);
    if (!appended && !BSL_SecurityActionSet_AppendAction(&actions, &action) &&
        !BSL_SecCtx_ExecutePolicyActionSet(&library, &container.bundle_ref, &actions))
    {
        const BSL_SecurityAction_t *executed = BSL_SecurityActionSet_GetActionAtIndex(&actions, 0);
        /* ExecutePolicyActionSet returns success even when an operation fails. */
        if (BSL_SecOper_GetConclusion(BSL_SecurityAction_GetSecOperAtIndex(executed, 0)) == BSL_SECOP_CONCLUSION_SUCCESS)
        {
            mock_bpa_ctr_sort_blocks(&container);
            if ((!protect || !audit_source_iv(bundle)) && !mock_bpa_ctr_encode(&container) && container.encoded.len <= capacity)
            {
                memcpy(output, container.encoded.ptr, container.encoded.len);
                *output_size = container.encoded.len;
                result = 0;
            }
        }
    }
    BSL_SecurityActionSet_Deinit(&actions);
    BSL_SecurityAction_Deinit(&action);
    BSL_SecOper_Deinit(&operation);
done:
    mock_bpa_ctr_deinit(&container);
    pthread_mutex_unlock(&mutex);
    return result;
}
