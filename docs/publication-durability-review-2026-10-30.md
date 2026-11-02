# Publication durability on replay — 2026-10-30

This follows the [sender and duplicate-recovery review](ccs-sender-review-2026-10-30.md).
Reviewing its immutable ground-output path exposed an error-retry gap in the
optional native demo's receipt and publication code.

## Fixes

A successful `rename` does not establish directory-entry durability. The demo
already syncs each newly written file before rename, then syncs its directory.
If that final directory `fsync` failed, the file could remain visible and the
call correctly returned an error. A later duplicate, however, could compare and
accept the visible file without retrying the failed directory sync. Visibility
was therefore sufficient to report success on retry even though the durability
boundary had never succeeded.

`deliver` now retries the directory sync after validating an existing receipt,
a matching published segment, or a matching completion record. A repeated sync
error continues to fail delivery, so `BPNode_DemoWire` returns its I/O error
before the bundle can enter successful custody acceptance. A later successful
sync permits retry to complete. Existing data is compared as before and is not
rewritten or renamed: inode, bytes, and modification time remain unchanged.
New files retain their file-sync, rename, and directory-sync ordering.

The segment temporary-path buffer is sized for the entire output-path buffer
plus `.tmp`. This removes the compiler's possible-truncation warning while
preserving the existing filenames and length checks.

The change is confined to demo delivery. It adds directory sync operations on
valid duplicate receipt/output/completion paths, under the existing file mutex.
It does not add a production fault-injection environment variable, wire-format
change, database migration, or new protocol acknowledgement behavior.

## Regression and evidence

`demo_delivery_regression` compiles the real demo source. A GNU linker wrapper
around `fsync` fails only a selected private test directory; ordinary file sync,
read/write, rename, and all other filesystem operations remain real. For each
of receipts, completed segments, and completion records it:

1. Fails directory sync after the file has actually been renamed into place.
2. Replays the same data and fails that directory sync again; the replay must
   still return an error and consume the injected failure.
3. Removes the injected failure, requires success, and checks the file's inode,
   size, and modification time are unchanged.

There are six injected failures in total. The existing conflicting-receipt,
changed/truncated/extended output, immutable replay, and missing-output recovery
checks remain. The pre-fix implementation fails the incomplete-receipt replay
assertion; the corrected implementation passes. The wrapper applies only to
this test executable and is absent from the installed BPNode module.

- Integration CTest: **31/31 passed**, 6.26 seconds.
- ASan/UBSan with leak detection: **9/9 passed**, 1.11 seconds.
- Focused before/after regression: reproduced the failure, then passed with all
  six failure injections consumed.
- Final incremental integration, sanitizer, and native build logs contain no
  compiler warnings or errors.

Native validation on the newly pinned build: **3/3 passed**. Independent
verification covers all 15 node runtimes, module/host/startup identities,
endpoint-only keys, matching segment hashes, custody/restart checkpoints, and
unchanged retained receipt hashes. No receipt-clock or BPLib initialization
errors were found.

| Native scenario | Result | Detail |
| --- | --- | --- |
| Original-ID duplicate replay | Passed, 122.18 s | Nine original IDs recovered; stable zero custody on all five hosts; unchanged output/completion; 41 unique IVs |
| Baseline transfer | Passed, 41.19 s | Three byte-identical segments and custody drained |
| Incomplete-receipt receiver restart | Passed, 85.70 s | Nonempty incomplete receipts preserved; completion after new readiness; three matching segments |

These are targeted reruns for the changed delivery paths. The previous twelve-case
matrix remains tied to its earlier recorded build and is not counted again.

The [evidence directory](validation-publication-2026-10-30/README.md) preserves
test/build logs and exact source/binary identities. Historical sender evidence
continues to describe its earlier recorded source and module snapshot.

## Reproduction and limits

Use the dependencies in [build and test](build-and-test.md). Build the configured
integration `demo_delivery_regression` target and run:

```sh
ctest --test-dir INTEGRATION_BUILD -R '^demo_delivery_regression$' --output-on-failure
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
ctest --test-dir ASAN_BUILD -R '^demo_delivery_regression$' --output-on-failure
```

The linker wrapper requires the existing Linux/GNU host-test toolchain. The
native demo must use a filesystem that supports directory `fsync`; errors remain
errors rather than being ignored to accommodate an unsupported filesystem.
Replays can consume additional storage I/O. These tests establish correct
error propagation and retry ordering on the host filesystem, not physical
power-loss behavior or Windows-native durability. The existing completion log
is best-effort observability; after an injected post-rename failure, success on
an already-existing completion record does not re-emit a creation message.

The [before-fix failure](validation-publication-2026-10-30/before-test.log),
[final integration log](validation-publication-2026-10-30/integration-tests.log),
[sanitizer log](validation-publication-2026-10-30/sanitizer-tests.log),
[native results](validation-publication-2026-10-30/results.json),
[independent evidence](validation-publication-2026-10-30/native-evidence.json), and
[verification log](validation-publication-2026-10-30/verify-native.log) are preserved
with the source/module fingerprints and production patch.

The subsequent [completion-validation review](completion-validation-review-2026-10-30.md)
strengthens EOF integrity checks and shares segment validation with data replay.
The evidence in this publication report retains its earlier source/binary hashes.
