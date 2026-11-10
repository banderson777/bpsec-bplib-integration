/* Exercise real demo receipt/publication code without the cFS task runtime. */
#include "../bpnode/fsw/src/bpnode_demo.c"
#undef NDEBUG
#include <assert.h>

/* Inject directory-sync errors after a real successful rename; file writes,
 * renames, reads and all other syncs still use the host filesystem. */
static ino_t failing_directory;
static unsigned failures_left, injected_failures, skip_matching_syncs;
int __real_fsync(int fd);
int __wrap_fsync(int fd)
{
    struct stat value;
    if (failures_left && fstat(fd, &value) == 0 && S_ISDIR(value.st_mode) &&
        value.st_ino == failing_directory)
    {
        if (skip_matching_syncs) skip_matching_syncs--;
        else { failures_left--; injected_failures++; errno = EIO; return -1; }
    }
    return __real_fsync(fd);
}
static void fail_directory(const char *path)
{
    struct stat value;
    assert(stat(path, &value) == 0 && S_ISDIR(value.st_mode));
    failing_directory = value.st_ino; failures_left = 1; skip_matching_syncs = 0;
}
/* A real stdio stream whose reader can fail only after the requested bytes.
 * Other fopen calls still use the real filesystem. This distinguishes an EOF
 * read error from a clean EOF, including the exact-capacity boundary. */
static struct { size_t length, position; bool read_error, close_error; unsigned closes; } probe;
FILE *__real_fopen(const char *path, const char *mode);
static ssize_t probe_read(void *cookie, char *buffer, size_t capacity)
{
    (void)cookie;
    if (probe.position == probe.length)
    {
        if (probe.read_error) { errno = EIO; return -1; }
        return 0;
    }
    size_t bytes = probe.length - probe.position;
    if (bytes > capacity) bytes = capacity;
    memset(buffer, 0xa5, bytes); probe.position += bytes;
    return bytes;
}
static int probe_close(void *cookie)
{
    (void)cookie; probe.closes++;
    if (probe.close_error) { errno = EIO; return -1; }
    return 0;
}
FILE *__wrap_fopen(const char *path, const char *mode)
{
    if (strcmp(path, "test-read-probe") == 0)
    {
        assert(strcmp(mode, "rb") == 0);
        cookie_io_functions_t callbacks = { .read = probe_read, .close = probe_close };
        return fopencookie(&probe, "r", callbacks);
    }
    return __real_fopen(path, mode);
}
static void read_failures(void)
{
    uint8_t bytes[8]; size_t length;
    /* Probe error after eight successfully read bytes must not be clean EOF. */
    memset(&probe, 0, sizeof(probe)); probe.length = 8; probe.read_error = true;
    assert(read_file("test-read-probe", bytes, sizeof(bytes), &length) != 0);
    assert(length == 8 && probe.closes == 1);
    for (size_t size = 0; size <= 9; ++size)
    {
        for (unsigned errors = 0; errors < 4; ++errors)
        {
            memset(&probe, 0, sizeof(probe)); probe.length = size;
            probe.read_error = errors & 1; probe.close_error = errors & 2;
            int result = read_file("test-read-probe", bytes, sizeof(bytes), &length);
            assert((result == 0) == (size <= sizeof(bytes) && errors == 0));
            assert(length == (size < sizeof(bytes) ? size : sizeof(bytes)));
            for (size_t i = 0; i < length; ++i) assert(bytes[i] == 0xa5);
            assert(probe.closes == 1);
        }
    }
}
static void put32(uint8_t *p, uint32_t value)
{
    p[0] = value >> 24; p[1] = value >> 16; p[2] = value >> 8; p[3] = value;
}
static void record(uint8_t *data, unsigned part)
{
    memset(data, 0, 44);
    memcpy(data, "BPD1", 4);
    put32(data + 24, part); put32(data + 28, 2);
    put32(data + 32, 1); put32(data + 36, 4);
    memcpy(data + 40, part ? "EFGH" : "ABCD", 4);
}
static struct stat stamp(const char *path)
{
    struct timespec times[2] = {{123456789, 0}, {123456789, 0}};
    struct stat value;
    assert(utimensat(AT_FDCWD, path, times, 0) == 0);
    assert(stat(path, &value) == 0);
    return value;
}
static void unchanged(const char *path, struct stat before)
{
    struct stat after;
    assert(stat(path, &after) == 0);
    assert(after.st_ino == before.st_ino && after.st_size == before.st_size);
    assert(after.st_mtim.tv_sec == before.st_mtim.tv_sec && after.st_mtim.tv_nsec == before.st_mtim.tv_nsec);
}
int main(void)
{
    read_failures();
    char directory[] = "/tmp/bpnode-demo-delivery-XXXXXX";
    int original = open(".", O_RDONLY | O_DIRECTORY); assert(original >= 0);
    assert(mkdtemp(directory) && chdir(directory) == 0);
    assert(mkdir("receipts", 0700) == 0 && mkdir("output", 0700) == 0);
    uint8_t first[44], second[44], end[40] = {0}, bytes[40]; size_t length;
    record(first, 0); record(second, 1);
    /* An empty stream can close, but only with a consistent EOF record. */
    uint8_t empty_session[40] = {0}; memcpy(empty_session, "BPE1", 4);
    put32(empty_session + 20, 1);
    assert(deliver(empty_session, sizeof(empty_session)) != 0);
    assert(access("output/complete", F_OK) != 0);
    put32(empty_session + 20, 0);
    assert(deliver(empty_session, sizeof(empty_session)) == 0);
    assert(read_file("output/complete", bytes, sizeof(bytes), &length) == 0 &&
           length == sizeof(empty_session) && memcmp(bytes, empty_session, length) == 0);
    assert(unlink("output/complete") == 0);
    /* Failed receipt-directory sync must fail again on duplicate replay, even
     * while the segment remains incomplete. The same inode must survive retry. */
    fail_directory("receipts");
    assert(deliver(first, sizeof(first)) != 0 && failures_left == 0);
    struct stat receipt = stamp("receipts/000000-00000.adu");
    fail_directory("receipts");
    assert(deliver(first, sizeof(first)) != 0 && failures_left == 0);
    assert(deliver(first, sizeof(first)) == 0 && access("output/ground-000000.ts", F_OK) != 0);
    unchanged("receipts/000000-00000.adu", receipt);
    /* Output exists after failed directory sync. Replay must retry the sync
     * without rewriting/renaming an otherwise identical published segment. */
    fail_directory("output");
    assert(deliver(second, sizeof(second)) != 0 && failures_left == 0);
    struct stat retry_output = stamp("output/ground-000000.ts");
    fail_directory("output");
    assert(deliver(second, sizeof(second)) != 0 && failures_left == 0);
    assert(deliver(second, sizeof(second)) == 0);
    unchanged("output/ground-000000.ts", retry_output);
    assert(read_file("output/ground-000000.ts", bytes, sizeof(bytes), &length) == 0);
    assert(length == 8 && memcmp(bytes, "ABCDEFGH", 8) == 0);
    struct stat published = stamp("output/ground-000000.ts");
    assert(deliver(first, sizeof(first)) == 0 && deliver(second, sizeof(second)) == 0);
    unchanged("output/ground-000000.ts", published);
    memcpy(end, "BPE1", 4); put32(end + 20, 1); put32(end + 32, 1);
    fail_directory("output");
    skip_matching_syncs = 1; /* Validate/sync segment, then fail completion sync. */
    assert(deliver(end, sizeof(end)) != 0 && failures_left == 0);
    struct stat completed = stamp("output/complete");
    fail_directory("output");
    skip_matching_syncs = 1; /* Validate/sync segment, then fail completion sync. */
    assert(deliver(end, sizeof(end)) != 0 && failures_left == 0);
    assert(deliver(end, sizeof(end)) == 0); unchanged("output/complete", completed);
    assert(injected_failures == 6);
    fail_directory("receipts");
    assert(deliver(end, sizeof(end)) != 0 && failures_left == 0);
    assert(deliver(end, sizeof(end)) == 0);
    assert(injected_failures == 7);
    first[40] ^= 1; assert(deliver(first, sizeof(first)) != 0); first[40] ^= 1;
    unchanged("output/ground-000000.ts", published);
    /* Existing content, truncation, and extra suffix must fail without replacement. */
    const char *bad[] = {"XBCDEFGH", "ABC", "ABCDEFGHI"};
    for (size_t i = 0; i < 3; ++i)
    {
        assert(write_file("output/ground-000000.ts", bad[i], strlen(bad[i]), "output") == 0);
        published = stamp("output/ground-000000.ts");
        assert(deliver(first, sizeof(first)) != 0);
        assert(deliver(end, sizeof(end)) != 0); /* Readable damaged output cannot complete. */
        unchanged("output/ground-000000.ts", published);
        unchanged("output/complete", completed);
        assert(unlink("output/complete") == 0);
        assert(deliver(end, sizeof(end)) != 0 && access("output/complete", F_OK) != 0);
        assert(write_file("output/complete", end, sizeof(end), "output") == 0);
        completed = stamp("output/complete");
    }
    assert(unlink("output/ground-000000.ts") == 0);
    assert(deliver(end, sizeof(end)) == 0); /* EOF can reconstruct missing output from receipts. */
    unchanged("output/complete", completed);
    assert(read_file("output/ground-000000.ts", bytes, sizeof(bytes), &length) == 0);
    assert(length == 8 && memcmp(bytes, "ABCDEFGH", 8) == 0);
    published = stamp("output/ground-000000.ts");
    /* A readable output cannot substitute for missing or inconsistent receipts. */
    assert(unlink("receipts/000000-00001.adu") == 0);
    assert(deliver(end, sizeof(end)) != 0);
    assert(write_file("receipts/000000-00001.adu", second, sizeof(second), "receipts") == 0);
    second[35] = 2; /* Authenticated-record metadata must agree with EOF. */
    assert(write_file("receipts/000000-00001.adu", second, sizeof(second), "receipts") == 0);
    assert(deliver(end, sizeof(end)) != 0);
    second[35] = 1;
    assert(write_file("receipts/000000-00001.adu", second, sizeof(second), "receipts") == 0);
    assert(deliver(end, sizeof(end)) == 0);
    /* Validate the first receipt's count/session rather than trusting its filename. */
    put32(first + 28, 0);
    assert(write_file("receipts/000000-00000.adu", first, sizeof(first), "receipts") == 0);
    assert(deliver(end, sizeof(end)) != 0);
    put32(first + 28, 65537);
    assert(write_file("receipts/000000-00000.adu", first, sizeof(first), "receipts") == 0);
    assert(deliver(end, sizeof(end)) != 0);
    put32(first + 28, 2); first[4] ^= 1;
    assert(write_file("receipts/000000-00000.adu", first, sizeof(first), "receipts") == 0);
    assert(deliver(end, sizeof(end)) != 0);
    first[4] ^= 1;
    assert(write_file("receipts/000000-00000.adu", first, sizeof(first), "receipts") == 0);
    assert(deliver(end, sizeof(end)) == 0);
    unchanged("output/ground-000000.ts", published);
    unchanged("output/complete", completed);
    assert(unlink("receipts/000000-00000.adu") == 0);
    assert(deliver(end, sizeof(end)) != 0);
    assert(unlink("receipts/000000-00001.adu") == 0);
    assert(unlink("output/ground-000000.ts") == 0 && unlink("output/complete") == 0);
    /* A valid zero-byte segment still has a receipt and a completed output. */
    record(first, 0); put32(first + 28, 1); put32(first + 36, 0);
    assert(deliver(first, 40) == 0 && deliver(end, sizeof(end)) == 0);
    assert(read_file("output/ground-000000.ts", bytes, sizeof(bytes), &length) == 0 && length == 0);
    assert(unlink("receipts/000000-00000.adu") == 0);
    assert(unlink("output/ground-000000.ts") == 0 && unlink("output/complete") == 0);
    assert(rmdir("receipts") == 0 && rmdir("output") == 0);
    assert(fchdir(original) == 0 && close(original) == 0 && rmdir(directory) == 0);
    puts("PASS: EOF/read/close error handling, directory-sync retry, validated completion, and immutable replay");
    return 0;
}
