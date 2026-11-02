# Isolate SQLite store statements between instances — 2026-10-30

This follows the [custody admission review](custody-admission-review-2026-10-30.md).
The serialized-size limit check already existed in the transaction path. Review
of that path instead exposed shared prepared-statement ownership: metadata and
blob insert statements were process-global while storage locks were per instance.

## Fix

Each `BPLib_SQL_Store` invocation now owns a zero-initialized statement context.
It prepares its metadata and blob statements into that context, passes them
explicitly through the store helpers, and finalizes only those statements on
success or failure. Another instance can no longer replace or finalize them.
The two global prepared-statement pointers are removed. Immutable query text
continues to be shared; no additional global writer mutex is introduced.

Previously, instance A could prepare its statements and pause during encoding.
Instance B could then overwrite the globals with its own database statements,
complete and finalize them, and clear the pointers. When A resumed it used the
cleared handles, failed its transaction and lost ownership of its original
prepared statements. Actual overlapping execution could also use a handle for
the wrong database or a finalized handle. The deterministic regression reproduces
the logical ownership failure with ordered steps rather than relying on undefined
racy access to fail consistently.

The `BPLib_SQL_Store` and public `BPLib_STOR_*` signatures are unchanged. Private
helpers in `bplib_stor_sql_store.h` now take explicit statements/context. Direct
consumers of that internal source header must recompile and adapt; the removed
globals are no longer an internal integration interface. No database schema,
wire format, cache structure layout or custody commit boundary changes.

Same-instance operations still require the existing storage serialization.
Separate SQLite connections are required for independent instance operations;
this change does not authorize unsynchronized sharing of one connection or
concurrent mutation of global node configuration. It addresses statement
ownership, not every global in the BPA or all possible SQLite busy schedules.

## Regression

The new `storage_parallel_regression` compiles the actual SQL initialization and
store implementation and uses two real SQLite in-memory databases. Encoding and
clock/configuration boundaries are controlled; the encoder emits distinct small
opaque byte patterns for each instance. This fixture tests SQL storage, not CBOR
wire validity; existing recovery and native tests exercise actual wire encoding.

- Deterministic overlap pauses A inside encoding after statement preparation,
  lets B finish, then resumes A. Both must commit into their own database.
- Two threads perform 40 additional stores each. Identical bundle IDs in separate
  databases must remain independent, with distinct stored payload patterns.
- A second controlled overlap makes B's blob-statement preparation fail through
  SQLite's authorizer. A must still commit, B must leave no new row or counts,
  and both connections must return to autocommit.
- Final checks require exactly 42 metadata/blob/recovery rows in A and 41 in B,
  correct payloads, no orphan statements, and successful close of both databases.

Pool/custody signalling is not stubbed into this test because it calls the lower
SQL transaction boundary directly. The caller holds each instance's storage
mutex as required. Condition-variable coordination makes the original regression
reproducible; the parallel loop adds interleavings without claiming exhaustive
schedule coverage.

The original source fails when A resumes after B completes. The fixed focused
regression passes, including failure isolation and statement cleanup. A missing
private-header include in the initial fixture was corrected before final runs;
final fixture builds use the proper function declaration.

## Validation

- Full integration CTest: **34/34 passed**, 4.33 seconds.
- ASan/UBSan with leak detection: **13/13 passed**, 0.85 seconds.
- Deterministic before/after ownership reproduction: failed before, passed after.
- ThreadSanitizer: build succeeded, but the host runtime exited before the test
  with `FATAL: ThreadSanitizer: unexpected memory mapping`. **No completed TSan
  result is claimed.** Host layout/security settings were not changed.

Native validation: **5/5 passed** on the final pinned library. Independent checks
cover all 25 runtimes, module/host/startup identities, endpoint-only keys,
source/output hashes, custody/restart checkpoints and preserved receipt hashes.
No receipt-clock or BPLib initialization errors were found.

| Scenario | Result | Evidence |
| --- | --- | --- |
| Original-ID duplicate replay | Passed, 115.51 s | Nine original IDs recovered; all five live hosts drained; immutable output/completion; 41 unique IVs |
| Baseline transfer | Passed, 40.38 s | Three matching segments; custody drained |
| Incomplete-receipt receiver restart | Passed, 78.08 s | Saved receipt unchanged; completion after current readiness; three matching segments |
| Dropped custody acknowledgement | Passed, 40.62 s | Drop fired; transfer recovered; matching outputs and custody drained |
| Storage failure | Passed, 25.52 s | No ground output/completion; source retains seven pending bundles, refusing relay retains none |

These native processes each contain one instance; they validate integration and
recovery after the refactor. Concurrent instances in one process are exercised
by the deterministic SQL fixture. Older matrices are not recounted as fresh runs.

The [evidence directory](validation-parallel-2026-10-30/README.md) preserves the
failed reproduction, final test/build logs, TSan startup failure and exact
source/binary fingerprints. Earlier reports retain their original snapshots.

## Reproduction and limits

Use the configured dependencies in [build and test](build-and-test.md):

```sh
cmake --build INTEGRATION_BUILD --target storage_parallel_regression -j4
ctest --test-dir INTEGRATION_BUILD -R '^storage_parallel_regression$' --output-on-failure
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
ctest --test-dir ASAN_BUILD -R '^storage_parallel_regression$' --output-on-failure
```

A TSan-capable host can build the same target with `-fsanitize=thread` and rerun
it; this host did not provide an executable TSan result. Tests do not establish
all-process thread safety, same-file multi-connection fairness, physical power-loss
behavior, or Windows-native execution.

Preserved records include the [pre-fix overlap failure](validation-parallel-2026-10-30/before-test.log),
[full integration results](validation-parallel-2026-10-30/integration-tests.log),
[ASan/UBSan results](validation-parallel-2026-10-30/sanitizer-tests.log),
[TSan runtime failure](validation-parallel-2026-10-30/tsan-tests.log),
[native commands/results](validation-parallel-2026-10-30/results.json),
[independent evidence](validation-parallel-2026-10-30/native-evidence.json), and
[verification log](validation-parallel-2026-10-30/verify-native.log).
The [private helper header](../bplib/bpa/stor/src/bplib_stor_sql_store.h) records the
explicit statement/context arguments. Exact source/binary fingerprints and the
production diff are preserved alongside the logs.

The later [SQLite bindings review](sqlite-bindings-review-2026-10-31.md) fixes
borrowed wire-buffer lifetime and binding/error cleanup. This parallel-storage
report retains its original source/binary fingerprints.
