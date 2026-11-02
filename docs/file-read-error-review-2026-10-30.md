# Propagate file-read errors from the EOF probe — 2026-10-30

This follows the [completion validation review](completion-validation-review-2026-10-30.md).
The shared demo file loader had an error-ordering bug: after `fread`, it checked
`ferror` before calling `fgetc` to require end of file. If that probe returned
`EOF` because of a new I/O error, the earlier error check was already complete,
and the function could return success.

## Fix and scope

`read_file` now preserves a failure from the main read and checks the stream's
error flag again after the EOF probe. A clean EOF still succeeds. Data beyond
the supplied capacity, read errors during either phase, and close errors all
return failure. Every opened stream is still closed once, including error paths.
The output count/buffer can contain partial data on failure; callers must honor
the returned status.

All call sites in `bpnode_demo.c` already check the status before using the
result: session/key loading, durable custody-sequence recovery, source ADU
feeding, retained receipt checks, and completion replay. Exact-sized startup
files (including the eight-byte sequence counter) therefore no longer treat
an EOF-probe I/O error as a successful load. A scan of local BPNode/integration
C sources found no other instance of the same check-ordering pattern; the
published-segment comparison already checks `ferror` after its probe.

Valid file formats, filenames, capacity checks, wire messages, database layout,
and the normal replay/directory-sync contract are unchanged. The change is in
the optional native demo's file helper.

## Regression

`demo_delivery_regression` compiles the actual implementation. A test-only GNU
linker wrapper for `fopen` substitutes a GNU `fopencookie` stream for one reserved
test path. Its reader supplies bytes normally, then either a clean EOF or an
`EIO` error. All other opens use the real filesystem. This exercises real stdio
buffering/error flags rather than stubbing the loader's return value.

The focused reproducer supplies exactly eight bytes to an eight-byte buffer,
then fails the next read. Before the fix, the function incorrectly succeeds;
afterwards it fails and closes the stream. A 40-case matrix checks lengths zero
through nine against an eight-byte capacity, with clean operation, a read error,
a close error, or both. It requires the expected status, byte count, preserved
bytes, and exactly one close in every case.

All existing delivery assertions remain: seven directory-sync failures/retries,
validated initial/replayed EOF, rejected zero-segment EOF, valid zero-byte files,
missing/invalid receipts, damaged-output rejection, missing-output recovery,
and immutable publication. Both `fopen` and `fsync` wrappers are confined to this
test executable; installed-module symbols confirm they are absent there.

## Validation

- Before/after regression: reproduced false success, then passed.
- Integration CTest: **31/31 passed**, 6.40 seconds.
- ASan/UBSan with leak detection: **9/9 passed**, 1.41 seconds.

Native validation: **3/3 passed** on the final pinned build. Independent
verification checks all 15 node runtimes, module/host/startup identities,
endpoint-only keys, output hashes, custody and restart checkpoints, and preserved
receipt hashes. No receipt-clock or BPLib initialization errors were found.

| Scenario | Result | Evidence |
| --- | --- | --- |
| Original-ID duplicate replay | Passed, 152.42 s | Nine original IDs retained/recovered; stable zero custody on all five hosts; immutable output/completion; 75 unique IVs |
| Baseline transfer | Passed, 43.57 s | Three matching segments; custody drained |
| Incomplete-receipt receiver restart | Passed, 71.09 s | Saved receipt unchanged; completion after new readiness; three matching segments |

These are targeted reruns for the changed file loader, including normal startup,
source sequence recovery, receipt reads and completion replay. They do not
recount the earlier twelve-scenario matrix as a new run.

The [evidence directory](validation-reads-2026-10-30/README.md) preserves exact
source/binary hashes, build logs, and test results. Earlier evidence remains
tied to its original snapshots.

## Reproduction and limits

Use the configured dependencies in [build and test](build-and-test.md):

```sh
cmake --build INTEGRATION_BUILD --target demo_delivery_regression -j4
ctest --test-dir INTEGRATION_BUILD -R '^demo_delivery_regression$' --output-on-failure
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
ctest --test-dir ASAN_BUILD -R '^demo_delivery_regression$' --output-on-failure
```

The stream fault test requires the existing Linux/GNU toolchain (`fopencookie`
and linker wrapping). Production reads still use ordinary stdio. Tests establish
error propagation and cleanup, not hardware fault probabilities, physical
power-loss guarantees, or Windows-native behavior. They do not add cryptographic
authentication to on-disk files.

Preserved records include the [pre-fix failure](validation-reads-2026-10-30/before-test.log),
[final regression](validation-reads-2026-10-30/final-test.log),
[integration results](validation-reads-2026-10-30/integration-tests.log),
[sanitizer results](validation-reads-2026-10-30/sanitizer-tests.log),
[native commands/results](validation-reads-2026-10-30/results.json),
[independent native evidence](validation-reads-2026-10-30/native-evidence.json), and
[verification log](validation-reads-2026-10-30/verify-native.log).

The later [custody-index review](custody-index-review-2026-10-30.md) fixes in-memory
index insertion rollback and bundle sequence publication. This file-read report
and its evidence retain their earlier source/binary identities.
