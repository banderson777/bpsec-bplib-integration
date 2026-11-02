#include "bpnode_video_sink.h"
#include "bplib_cbor.h"
#include "osapi-file.h"
#include "osapi-error.h"
#undef NDEBUG
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char output_path[128];
static BPLib_Instance_t instance;
typedef struct { BPLib_Bundle_t bundle; size_t size; uint8_t bytes[2048]; } TestBundle;

int32 OS_OpenCreate(osal_id_t *id, const char *path, int32 flags, int32 mode)
{
    (void)path; (void)mode;
    int fd = open(output_path, O_CREAT | O_WRONLY | ((flags & OS_FILE_FLAG_TRUNCATE) ? O_TRUNC : 0), 0600);
    if (fd < 0) return -1;
    *id = (osal_id_t)fd; return OS_SUCCESS;
}
int32 OS_close(osal_id_t id) { return close((int)id); }
int32 OS_lseek(osal_id_t id, osal_offset_t offset, uint32 whence) { return lseek((int)id, offset, whence); }
int32 OS_write(osal_id_t id, const void *buffer, size_t size)
{
    /* Force short writes to test completion, too. */
    return write((int)id, buffer, size > 37 ? 37 : size);
}
BPLib_Bundle_t *BPLib_MEM_BundleAlloc(BPLib_MEM_Pool_t *pool, const void *data, size_t size)
{
    (void)pool; assert(size <= 2048);
    TestBundle *b = calloc(1, sizeof(*b)); assert(b);
    memcpy(b->bytes, data, size); b->size = size; return &b->bundle;
}
void BPLib_MEM_BundleFree(BPLib_MEM_Pool_t *pool, BPLib_Bundle_t *bundle) { (void)pool; free(bundle); }
BPLib_Status_t BPLib_CBOR_DecodeBundle(BPLib_Instance_t *inst, const void *data, size_t size, BPLib_Bundle_t *bundle)
{ (void)inst; (void)data; (void)size; bundle->blocks.PayloadHeader.BlockNum = 1; return BPLIB_SUCCESS; }
BPLib_Status_t BPLib_BlockReaderOpen(BPLib_Bundle_t *bundle, uint64_t num, BPLib_BlockReader_t *reader)
{ (void)num; reader->bundle = bundle; reader->offset = 0; reader->remaining = ((TestBundle *)bundle)->size; return BPLIB_SUCCESS; }
BPLib_Status_t BPLib_BlockReaderRead(BPLib_BlockReader_t *reader, void *buf, size_t cap, size_t *got)
{
    *got = cap < reader->remaining ? cap : reader->remaining;
    memcpy(buf, ((TestBundle *)reader->bundle)->bytes + reader->offset, *got);
    reader->offset += *got; reader->remaining -= *got; return BPLIB_SUCCESS;
}
static void *write_bundle(void *arg)
{
    uint8_t bytes[2048]; memset(bytes, *(char *)arg, sizeof(bytes));
    assert(BPNode_VideoSink_Write(&instance, bytes, sizeof(bytes)) == BPLIB_SUCCESS);
    return NULL;
}
int main(void)
{
    strcpy(output_path, "/tmp/bpnode-video-test-XXXXXX"); int fd = mkstemp(output_path); assert(fd >= 0); close(fd);
    char a = 'A', b = 'B'; write_bundle(&a); BPNode_VideoSink_Close(); write_bundle(&b); BPNode_VideoSink_Close();
    pthread_t threads[2];
    assert(pthread_create(&threads[0], NULL, write_bundle, &a) == 0);
    assert(pthread_create(&threads[1], NULL, write_bundle, &b) == 0);
    pthread_join(threads[0], NULL); pthread_join(threads[1], NULL); BPNode_VideoSink_Close();
    fd = open(output_path, O_RDONLY); assert(fd >= 0);
    char bytes[8193]; ssize_t size = read(fd, bytes, sizeof(bytes)); assert(size == 8192);
    for (size_t offset = 0; offset < 8192; offset += 2048)
        for (size_t i = offset; i < offset + 2048; ++i) assert(bytes[i] == bytes[offset]);
    assert(bytes[0] == 'A' && bytes[2048] == 'B' && bytes[4096] != bytes[6144]);
    close(fd); assert(unlink(output_path) == 0);
    puts("PASS actual video sink preserves reconnect output, serializes whole bundles, and completes short writes");
    return 0;
}
