#include "bplib_mem.h"
#undef NDEBUG
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static int allocations, fail_after = -1;
BPLib_Status_t BPLib_MEM_PoolImplInit(BPLib_MEM_PoolImpl_t *p, void *mem, size_t size, size_t block)
{ (void)p; (void)mem; (void)size; (void)block; return BPLIB_SUCCESS; }
void BPLib_MEM_PoolImplDestroy(BPLib_MEM_PoolImpl_t *p) { (void)p; }
void *BPLib_MEM_PoolImplAlloc(BPLib_MEM_PoolImpl_t *p, size_t size)
{
    (void)p;
    if (fail_after == 0) return NULL;
    if (fail_after > 0) --fail_after;
    void *ptr = malloc(size);
    if (ptr) ++allocations;
    return ptr;
}
void BPLib_MEM_PoolImplFree(BPLib_MEM_PoolImpl_t *p, void *ptr)
{ (void)p; free(ptr); --allocations; }
size_t BPLib_MEM_GetBytesInUseImpl(BPLib_MEM_PoolImpl_t *p) { (void)p; return allocations * 1024; }
size_t BPLib_MEM_GetBytesFreeImpl(BPLib_MEM_PoolImpl_t *p) { (void)p; return 0; }
static void bounds(BPLib_MEM_Pool_t *pool)
{
    unsigned char source[BPLIB_MEM_BIG_BLK_DATA_SIZE + 3];
    for (size_t i = 0; i < sizeof(source); ++i) source[i] = (unsigned char)i;
    BPLib_Bundle_t *bundle = BPLib_MEM_BundleAlloc(pool, source, sizeof(source));
    assert(bundle);
    /* Make allocator slack deterministic, as when a pool block is reused. */
    BPLib_MEM_Block_t *last = bundle->blob;
    while (last->next) last = last->next;
    memset(last->user_data.BigData + last->used_len, 0xCC,
           BPLIB_MEM_BIG_BLK_DATA_SIZE - last->used_len);
    unsigned char output[8];
    memset(output, 0xAA, sizeof(output));
    assert(BPLib_MEM_CopyOutFromOffset(bundle, sizeof(source) - 1, 2, output, sizeof(output)) != BPLIB_SUCCESS);
    for (size_t i = 0; i < sizeof(output); ++i) assert(output[i] == 0xAA);
    assert(BPLib_MEM_CopyOutFromOffset(bundle, sizeof(source) + 1, 1, output, sizeof(output)) != BPLIB_SUCCESS);
    assert(BPLib_MEM_CopyOutFromOffset(bundle, UINT64_MAX, 1, output, sizeof(output)) != BPLIB_SUCCESS);
    assert(BPLib_MEM_CopyOutFromOffset(bundle, sizeof(source), 0, output, sizeof(output)) == BPLIB_SUCCESS);
    assert(BPLib_MEM_CopyOutFromOffset(bundle, BPLIB_MEM_BIG_BLK_DATA_SIZE - 2, 5, output, sizeof(output)) == BPLIB_SUCCESS);
    assert(memcmp(output, source + BPLIB_MEM_BIG_BLK_DATA_SIZE - 2, 5) == 0);
    assert(BPLib_MEM_CopyOutFromOffset(bundle, 0, 9, output, sizeof(output)) == BPLIB_BUF_LEN_ERROR);
    /* Readers must also reject metadata whose range exceeds the blob. */
    bundle->blocks.PayloadHeader.BlockNum = 1;
    bundle->blocks.PayloadHeader.DataOffsetStart = sizeof(source) - 1;
    bundle->blocks.PayloadHeader.DataSize = 2;
    BPLib_BlockReader_t reader; size_t got = 99;
    assert(BPLib_BlockReaderOpen(bundle, 1, &reader) == BPLIB_SUCCESS);
    assert(BPLib_BlockReaderRead(&reader, output, sizeof(output), &got) != BPLIB_SUCCESS && got == 0);
    BPLib_MEM_BundleFree(pool, bundle);
    puts("PASS blob bounds, unchanged output on failure, cross-block reads, reader bounds");
}

static void pool_bounds(void)
{
    BPLib_MEM_Pool_t pool;
    unsigned char memory[32];
    memset(memory, 0xAB, sizeof(memory));
    assert(BPLib_MEM_PoolInit(&pool, memory, 1) == BPLIB_SUCCESS);
    assert(memory[0] == 0);
    for (size_t i = 1; i < sizeof(memory); ++i) assert(memory[i] == 0xAB);
    BPLib_MEM_PoolDestroy(&pool);
    assert(BPLib_MEM_PoolInit(&pool, memory, sizeof(memory)) == BPLIB_SUCCESS);
    for (size_t i = 0; i < sizeof(memory); ++i) assert(memory[i] == 0);
    BPLib_MEM_PoolDestroy(&pool);
    puts("PASS pool initialization respects supplied buffer length");
}

int main(void)
{
    pool_bounds();
    BPLib_MEM_Pool_t pool; char memory[16], payload[4096] = {0};
    assert(BPLib_MEM_PoolInit(&pool, memory, sizeof(memory)) == BPLIB_SUCCESS);
    assert(BPLib_MEM_BundleAlloc(&pool, payload, 0) == NULL && allocations == 0);
    int allocation_success = 0;
    for (int allocation_limit = 0; allocation_limit < 32; ++allocation_limit)
    {
        fail_after = allocation_limit;
        BPLib_Bundle_t *injected = BPLib_MEM_BundleAlloc(&pool, payload, sizeof(payload));
        if (injected)
        {
            BPLib_MEM_BundleFree(&pool, injected);
            fail_after = -1;
            allocation_success = 1;
            break;
        }
        assert(allocations == 0);
    }
    assert(allocation_success);
    fail_after = -1;
    BPLib_Bundle_t *bundle = BPLib_MEM_BundleAlloc(&pool, payload, sizeof(payload));
    assert(bundle); BPLib_MEM_BundleFree(&pool, bundle); assert(allocations == 0);
    bounds(&pool); assert(allocations == 0);
    BPLib_MEM_PoolDestroy(&pool);
    puts("PASS zero-length and injected allocation failures: valid frees, no outstanding allocations");
    return 0;
}
