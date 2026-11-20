#include "bplib_bsl_security.h"
#include <bsl/BPSecLib_Private.h>
#include <bsl/dynamic/PublicInterfaceImpl.h>
#include <bsl/dynamic/SecurityActionSet.h>
#include <bsl/dynamic/AbsSecBlock.h>
#include <bsl/dynamic/CBOR.h>
#include <bsl/default_sc/DefaultSecContext.h>
#include <bsl/mock_bpa/MockBPA.h>
#include <bsl/mock_bpa/ctr.h>
#include <bsl/mock_bpa/agent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static BSL_LibCtx_t library;
static bool initialized;
static bool integrity_enabled;
static bool creating_bib_bcb;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static const char key_name[] = "five-node-video";
static const char integrity_name[] = "five-node-integrity";

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

/** Create a security block with the lab flags before BSL computes its AAD. */
static int create_security_block(BSL_BundleRef_t *ref, uint64_t type, uint64_t *number)
{
    int status = MockBPA_CreateBlock(ref, type, number);
    if (!status && creating_bib_bcb && type == BSL_SECBLOCKTYPE_BCB)
    {
        MockBPA_Bundle_t *bundle = ref->data;
        (*MockBPA_BlockByNum_get(bundle->blocks_num, *number))->flags = 0;
    }
    return status;
}

/** Refresh the number index because the mock list can move surviving entries. */
static int remove_security_block(BSL_BundleRef_t *ref, uint64_t number)
{
    int status = MockBPA_RemoveBlock(ref, number);
    if (!status)
    {
        MockBPA_Bundle_t *bundle = ref->data;
        MockBPA_BlockByNum_reset(bundle->blocks_num);
        MockBPA_BlockList_it_t iterator;
        for (MockBPA_BlockList_it(iterator, bundle->blocks); !MockBPA_BlockList_end_p(iterator);
             MockBPA_BlockList_next(iterator))
        {
            MockBPA_CanonicalBlock_t *block = MockBPA_BlockList_ref(iterator);
            MockBPA_BlockByNum_set_at(bundle->blocks_num, block->blk_num, block);
        }
    }
    return status;
}

/** Load a dedicated key and release the temporary handle on every path. */
static int load_key(const uint8_t *key, size_t size, const char *label)
{
    BSL_Crypto_KeyHandle_t handle = NULL;
    BSL_Data_t name = BSL_DATA_INIT_VIEW((uint8_t *)label, strlen(label));
    int result = BSL_Crypto_LoadKey(key, size, &handle);
    if (!result) result = MockBPA_KeyStore_AddKey(&name, handle);
    BSL_Crypto_ReleaseKeyHandle(handle);
    return result;
}

int BPLib_BSL_SecurityInit(const uint8_t key[32])
{
    return BPLib_BSL_SecurityInitWithIntegrity(key, NULL);
}

int BPLib_BSL_SecurityInitWithIntegrity(const uint8_t key[32], const uint8_t integrity_key[64])
{
    if (!key || initialized) return -1;
    BSL_HostDescriptors_t descriptors = MockBPA_Agent_Descriptors(NULL);
    descriptors.get_sec_src_eid_fn = security_source;
    descriptors.block_create_fn = create_security_block;
    descriptors.block_remove_fn = remove_security_block;
    if (BSL_HostDescriptors_Set(descriptors)) return -1;
    MockBPA_KeyStore_Init();
    int result = load_key(key, 32, key_name);
    if (!result && integrity_key) result = load_key(integrity_key, 64, integrity_name);
    if (!result) result = BSL_API_InitLib(&library);
    if (!result)
    {
        BSL_SecCtxDesc_t bcb = {.execute = BSLX_BCB_Execute, .validate = BSLX_BCB_Validate};
        BSL_SecCtxDesc_t bib = {.execute = BSLX_BIB_Execute, .validate = BSLX_BIB_Validate};
        result = BSL_API_RegisterSecurityContext(&library, RFC9173_CONTEXTID_BCB_AES_GCM, bcb);
        if (!result && integrity_key)
            result = BSL_API_RegisterSecurityContext(&library, RFC9173_CONTEXTID_BIB_HMAC_SHA2, bib);
        if (result) { int cleanup = BSL_API_DeinitLib(&library); (void)cleanup; }
    }
    if (result) { MockBPA_KeyStore_Deinit(); BSL_HostDescriptors_Clear(); return -1; }
    integrity_enabled = integrity_key != NULL;
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
    integrity_enabled = false;
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

/** Validate the context, source, and single target, and extract the GCM nonce. */
static bool accept_bcb(const MockBPA_CanonicalBlock_t *block, uint64_t *number, uint8_t iv[12])
{
    if (!block) return false;
    BSL_AbsSecBlock_t asb;
    BSL_AbsSecBlock_Init(&asb);
    BSL_Data_t data = BSL_DATA_INIT_VIEW(block->btsd, block->btsd_len);
    BSL_Data_t nonce = BSL_DATA_INIT_NULL;
    bool valid = !BSL_CBOR_Decode(&data, (BSL_CBOR_Decode_f)BSL_AbsSecBlock_Decode, &asb);
    valid = valid && asb.sec_context_id == RFC9173_CONTEXTID_BCB_AES_GCM &&
        ipn(&asb.source_eid, 10, 0) && BSL_AbsSecBlock_TargetList_size(asb.target_results) == 1 &&
        parameter(&asb, RFC9173_BCB_SECPARAM_AESVARIANT, RFC9173_BCB_AES_VARIANT_A256GCM) &&
        parameter(&asb, RFC9173_BCB_SECPARAM_AADSCOPE, 7);
    if (valid)
    {
        BSL_AbsSecBlock_TargetPtr_t *const *target = BSL_AbsSecBlock_TargetList_cget(asb.target_results, 0);
        BSLB_VariantPtr_t *const *entry = BSLB_VariantPtrMap_cget(asb.params, RFC9173_BCB_SECPARAM_IV);
        valid = entry && !BSL_Variant_GetAsBytestr(BSLB_VariantPtr_cref(*entry), &nonce) &&
            nonce.len == 12;
        if (valid)
        {
            *number = BSL_AbsSecBlock_TargetPtr_cref(*target)->target_block_num;
            memcpy(iv, nonce.ptr, 12);
        }
    }
    BSL_Data_Deinit(&nonce);
    BSL_AbsSecBlock_Deinit(&asb);
    return valid;
}

/** Check the decrypted BIB against the endpoint's full-scope payload policy. */
static bool accept_bib(const MockBPA_CanonicalBlock_t *block)
{
    if (!block) return false;
    BSL_AbsSecBlock_t asb;
    BSL_AbsSecBlock_Init(&asb);
    BSL_Data_t data = BSL_DATA_INIT_VIEW(block->btsd, block->btsd_len);
    bool valid = !BSL_CBOR_Decode(&data, (BSL_CBOR_Decode_f)BSL_AbsSecBlock_Decode, &asb);
    valid = valid && asb.sec_context_id == RFC9173_CONTEXTID_BIB_HMAC_SHA2 &&
        ipn(&asb.source_eid, 10, 0) && BSL_AbsSecBlock_TargetList_size(asb.target_results) == 1 &&
        parameter(&asb, RFC9173_BIB_PARAMID_SHA_VARIANT, RFC9173_BIB_SHA_HMAC512) &&
        parameter(&asb, RFC9173_BIB_PARAMID_INTEG_SCOPE_FLAG, 7) &&
        !BSLB_VariantPtrMap_cget(asb.params, RFC9173_BIB_PARAMID_WRAPPED_KEY);
    if (valid)
    {
        BSL_AbsSecBlock_TargetPtr_t *const *target = BSL_AbsSecBlock_TargetList_cget(asb.target_results, 0);
        valid = BSL_AbsSecBlock_TargetPtr_cref(*target)->target_block_num == 1;
    }
    BSL_AbsSecBlock_Deinit(&asb);
    return valid;
}

/** Execute one security operation and require its individual conclusion. */
static bool apply_security(mock_bpa_ctr_t *container, bool protect, bool integrity,
                           uint64_t target, uint64_t security)
{
    BSL_SecOper_t operation;
    BSL_SecurityAction_t action;
    BSL_SecurityActionSet_t actions;
    BSL_SecOper_Init(&operation);
    BSL_SecurityAction_Init(&action);
    BSL_SecurityActionSet_Init(&actions);
    BSL_SecOper_Populate(&operation,
        integrity ? RFC9173_CONTEXTID_BIB_HMAC_SHA2 : RFC9173_CONTEXTID_BCB_AES_GCM,
        target, security, integrity ? BSL_SECBLOCKTYPE_BIB : BSL_SECBLOCKTYPE_BCB,
        protect ? BSL_SECROLE_SOURCE : BSL_SECROLE_ACCEPTOR, BSL_POLICYACTION_DROP_BUNDLE);
    if (integrity)
    {
        BSL_Variant_SetTextstr(BSL_SecOper_AddOption(&operation, BSLX_BIB_OPT_KEY_ID), integrity_name);
        BSL_Variant_SetInt64(BSL_SecOper_AddOption(&operation, BSLX_BIB_OPT_SCOPE), 7);
        BSL_Variant_SetInt64(BSL_SecOper_AddOption(&operation, BSLX_BIB_OPT_SHA_VARIANT), RFC9173_BIB_SHA_HMAC512);
        BSL_Variant_SetInt64(BSL_SecOper_AddOption(&operation, BSLX_BIB_OPT_USE_KEY_WRAP), 0);
    }
    else
    {
        BSL_Variant_SetTextstr(BSL_SecOper_AddOption(&operation, BSLX_BCB_OPT_KEY_ID), key_name);
        BSL_Variant_SetInt64(BSL_SecOper_AddOption(&operation, BSLX_BCB_OPT_SCOPE), 7);
        BSL_Variant_SetInt64(BSL_SecOper_AddOption(&operation, BSLX_BCB_OPT_AES_VARIANT), RFC9173_BCB_AES_VARIANT_A256GCM);
        BSL_Variant_SetInt64(BSL_SecOper_AddOption(&operation, BSLX_BCB_OPT_USE_KEY_WRAP), 0);
    }
    int appended = BSL_SecurityAction_AppendSecOper(&action, &operation);
    /* AppendSecOper moves ownership into the action. */
    if (!appended) BSL_SecOper_Init(&operation);
    bool success = false;
    if (!appended && !BSL_SecurityActionSet_AppendAction(&actions, &action))
    {
        creating_bib_bcb = protect && !integrity && target != 1;
        int status = BSL_SecCtx_ExecutePolicyActionSet(&library, &container->bundle_ref, &actions);
        creating_bib_bcb = false;
        if (!status)
        {
            const BSL_SecurityAction_t *executed = BSL_SecurityActionSet_GetActionAtIndex(&actions, 0);
            success = BSL_SecOper_GetConclusion(BSL_SecurityAction_GetSecOperAtIndex(executed, 0)) ==
                BSL_SECOP_CONCLUSION_SUCCESS;
        }
    }
    BSL_SecurityActionSet_Deinit(&actions);
    BSL_SecurityAction_Deinit(&action);
    BSL_SecOper_Deinit(&operation);
    return success;
}

/** Find the block by its validated bundle-local number. */
static MockBPA_CanonicalBlock_t *find_block(MockBPA_Bundle_t *bundle, uint64_t number)
{
    MockBPA_CanonicalBlock_t **block = MockBPA_BlockByNum_get(bundle->blocks_num, number);
    return block ? *block : NULL;
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

/** Order the three security blocks for the mixed Rust/legacy wire profile.
 * The mock encoder already places payload last. Reordering happens after the
 * final security operation, so the number index is no longer consulted. */
static bool order_combined_blocks(MockBPA_Bundle_t *bundle, uint64_t bib_number)
{
    MockBPA_CanonicalBlock_t *slots[3];
    unsigned count = 0;
    MockBPA_BlockList_it_t iterator;
    for (MockBPA_BlockList_it(iterator, bundle->blocks); !MockBPA_BlockList_end_p(iterator);
         MockBPA_BlockList_next(iterator))
    {
        MockBPA_CanonicalBlock_t *block = MockBPA_BlockList_ref(iterator);
        if (block->blk_num >= bib_number && block->blk_num <= bib_number + 2)
        {
            if (count == 3) return false;
            slots[count++] = block;
        }
    }
    if (count != 3 || slots[0]->blk_num != bib_number + 2 ||
        slots[1]->blk_num != bib_number + 1 || slots[2]->blk_num != bib_number) return false;
    MockBPA_CanonicalBlock_t encrypted_bib_bcb = *slots[0];
    *slots[0] = *slots[2];
    *slots[2] = encrypted_bib_bcb;
    return true;
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
    uint64_t next_number = 2, bib_number = 0;
    uint64_t bcb_numbers[2], bcb_targets[2];
    uint8_t nonces[2][12];
    unsigned payloads = 0, bcbs = 0;
    MockBPA_BlockList_it_t iterator;
    for (MockBPA_BlockList_it(iterator, bundle->blocks); !MockBPA_BlockList_end_p(iterator); MockBPA_BlockList_next(iterator))
    {
        const MockBPA_CanonicalBlock_t *block = MockBPA_BlockList_ref(iterator);
        if (find_block(bundle, block->blk_num) != block) goto done;
        if (block->blk_num >= next_number)
        {
            if (block->blk_num == UINT64_MAX) goto done;
            next_number = block->blk_num + 1;
        }
        if (block->blk_type == 1 && block->blk_num == 1) payloads++;
        if (block->blk_type == 11)
        {
            if (protect || !integrity_enabled || bib_number || block->blk_num < 2) goto done;
            bib_number = block->blk_num;
        }
        if (block->blk_type == 12)
        {
            if (protect || bcbs == 2 || !accept_bcb(block, &bcb_targets[bcbs], nonces[bcbs])) goto done;
            bcb_numbers[bcbs++] = block->blk_num;
        }
    }
    if (payloads != 1) goto done;
    if (protect)
    {
        if (integrity_enabled)
        {
            if (next_number > UINT64_MAX - 2) goto done;
            bib_number = next_number++;
            if (!apply_security(&container, true, true, 1, bib_number)) goto done;
            /* RFC 9173 removes checksums from encrypted targets. The legacy
             * single-BCB path keeps its existing wire behavior. */
            MockBPA_CanonicalBlock_t *payload = find_block(bundle, 1);
            if (!payload) goto done;
            payload->crc_type = 0;
            if (!apply_security(&container, true, false, 1, next_number) ||
                !apply_security(&container, true, false, bib_number, next_number + 1)) goto done;
            uint64_t target;
            if (!accept_bcb(find_block(bundle, next_number), &target, nonces[0]) ||
                !accept_bcb(find_block(bundle, next_number + 1), &target, nonces[1]) ||
                !memcmp(nonces[0], nonces[1], sizeof(nonces[0]))) goto done;
        }
        else if (!apply_security(&container, true, false, 1, next_number)) goto done;
    }
    else if (integrity_enabled)
    {
        MockBPA_CanonicalBlock_t *payload = find_block(bundle, 1);
        MockBPA_CanonicalBlock_t *bib = find_block(bundle, bib_number);
        if (!payload || payload->crc_type != 0 || !bib || bib->crc_type != 0) goto done;
        if (!bib_number || bcbs != 2 || bcb_numbers[0] == bcb_numbers[1] ||
            !memcmp(nonces[0], nonces[1], sizeof(nonces[0]))) goto done;
        unsigned payload_bcb = bcb_targets[0] == 1 ? 0 : 1;
        unsigned bib_bcb = 1 - payload_bcb;
        if (bcb_targets[payload_bcb] != 1 || bcb_targets[bib_bcb] != bib_number ||
            !apply_security(&container, false, false, bib_number, bcb_numbers[bib_bcb]) ||
            !apply_security(&container, false, false, 1, bcb_numbers[payload_bcb]) ||
            !accept_bib(find_block(bundle, bib_number)) ||
            !apply_security(&container, false, true, 1, bib_number)) goto done;
    }
    else if (bcbs != 1 || bcb_targets[0] != 1 ||
        !apply_security(&container, false, false, 1, bcb_numbers[0])) goto done;

    mock_bpa_ctr_sort_blocks(&container);
    if (protect && integrity_enabled && !order_combined_blocks(bundle, bib_number)) goto done;
    if ((!protect || !audit_source_iv(bundle)) && !mock_bpa_ctr_encode(&container) &&
        container.encoded.len <= capacity)
    {
        memcpy(output, container.encoded.ptr, container.encoded.len);
        *output_size = container.encoded.len;
        result = 0;
    }
done:
    mock_bpa_ctr_deinit(&container);
    pthread_mutex_unlock(&mutex);
    return result;
}
