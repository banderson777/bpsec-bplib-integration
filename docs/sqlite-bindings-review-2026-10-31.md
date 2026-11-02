# Release SQLite blob bindings before freeing encoded data — 2026-10-31

This follows the [parallel-storage review](storage-parallel-review-2026-10-30.md).
Review of the now-local statement lifecycle found a remaining buffer-lifetime
violation in the chunk writer.

## Defect and fix

The writer bound encoded data with `SQLITE_STATIC`, stepped the insert, and left
the parameters attached to the statement. `StoreBundle` subsequently freed the
encoded wire buffer before that statement was finalized or rebound. SQLite's
[binding contract](https://www.sqlite.org/c3ref/bind_blob.html) requires borrowed
storage to remain valid through that interval. Merely
[resetting a statement](https://www.sqlite.org/c3ref/reset.html) retains its
bindings; [clearing bindings](https://www.sqlite.org/c3ref/clear_bindings.html)
releases those parameter references.

`BPLib_SQL_StoreChunk` now resets and clears parameters before reuse, checks each
bind result, executes only after successful binding, and resets/clears before
return. Thus `StoreBundle` can free its wire allocation without SQLite retaining
its address. It keeps `SQLITE_STATIC` only for the duration of the call; no extra
blob copy is introduced. Success requires a completed insert and successful
cleanup. A failed bind or step retains its original failure result through cleanup.

A null statement or input pointer is rejected, and a chunk larger than `INT_MAX`
is rejected before narrowing the SQLite length argument. A non-null pointer with
zero length still represents a valid empty blob at this private helper boundary;
normal encoded BP bundles remain nonempty. Retry after a failed step works with
fresh parameters. Batch diagnostics now print the returned failure code rather
than a connection error value that cleanup may have changed.

Public entry points, private helper signatures, wire formats and SQL schema are
unchanged. The private header documents the new parameter-release guarantee.
Same-instance storage serialization and per-invocation statement ownership remain
as described in the previous review.

## What the reproduction establishes

The direct regression inspects `sqlite3_expanded_sql` while the caller's buffer
is still alive. Before the fix it finds the blob and row ID still bound after
return; after the fix all parameters are NULL. This avoids deliberately reading
freed memory to demonstrate the lifetime mismatch.

An initial end-to-end test reduced SQLite's blob-size limit and checked whether
an ignored bind error could lead to a NULL blob being committed and custody
acknowledged. On this SQLite build, the existing path already rolled back and
refused custody. **No pre-fix false acknowledgement is claimed for that case.**
The passing negative control is preserved, and the case remains a regression
guard. Explicit bind-result checks strengthen the helper independently of that
SQLite behavior.

## Tests

The new `storage_binding_regression` links the actual chunk writer with real
SQLite. It verifies:

- No retained blob or row-ID parameters on success, size error, constraint error,
  invalid input and retry.
- Persisted data remains unchanged when the caller reuses its source buffer.
- Real SQLite blob-limit errors, a wrong parameter layout, null arguments and
  lengths above `INT_MAX` return errors without inserting a new row.
- Valid empty blobs, successful reuse after a constraint failure, and 64 further
  writes with cleared parameters after every call.
- Exactly 68 expected rows, final statement cleanup and successful database close.

The existing `storage_commit_regression` now also exercises a real blob-size
limit with larger encoded output. It requires a rolled-back batch, zero accepted
custody, zero queued egress, both refusals outside the storage mutex, and complete
allocation cleanup. Prior insert/COMMIT, capacity and pending-batch checks remain.
Encoding is controlled in that transaction fixture; actual wire codecs and native
transfers remain covered separately.

## Validation

- Integration CTest: **35/35 passed**, 20.41 seconds.
- ASan/UBSan with leak detection: **14/14 passed**, 8.27 seconds.
- Focused binding/transaction pair: **2/2 passed**.
- Retained-binding reproduction: failed before the fix, passed afterwards.
- Pre-fix size-limit integration control: already passed, and remains covered.

Native validation: **5/5 passed** on the final pinned build. Independent checks
cover all 25 runtimes, module/host/startup identities, endpoint-only keys,
source/output hashes, custody/restart checkpoints and unchanged receipt hashes.
No receipt-clock or BPLib initialization errors were found.

| Scenario | Result | Evidence |
| --- | --- | --- |
| Original-ID duplicate replay | Passed, 121.28 s | Nine original IDs recovered; all five live hosts drained; immutable output/completion; 41 unique IVs |
| Baseline transfer | Passed, 43.11 s | Three matching segments; custody drained |
| Incomplete-receipt receiver restart | Passed, 61.14 s | Saved receipt unchanged; completion after current readiness; three matching segments |
| Dropped custody acknowledgement | Passed, 72.65 s | Drop fired; transfer recovered; matching outputs and custody drained |
| Storage failure | Passed, 26.07 s | No ground output/completion; source retains seven pending bundles, refusing relay retains none |

These are targeted native reruns for the changed writer, not a recount of any
historical matrix. Real binding/error cases are exercised in the focused tests;
native runs check transfer, replay and recovery with actual wire encodings.

The [evidence directory](validation-bindings-2026-10-31/README.md) preserves source
and binary fingerprints, build/test logs, both kinds of reproduction records,
and fresh native commands/results. Automatic permission review timed out before
starting the first full-suite request; its authorized retry executed successfully.
This was not an application test failure.

## Reproduction and limits

Use the configured dependencies in [build and test](build-and-test.md):

```sh
cmake --build INTEGRATION_BUILD --target storage_binding_regression storage_commit_regression -j4
ctest --test-dir INTEGRATION_BUILD -R '^storage_(binding|commit)_regression$' --output-on-failure
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
ctest --test-dir ASAN_BUILD -R '^storage_(binding|commit)_regression$' --output-on-failure
```

The helper expects a valid, owned insert statement with serialized access. It
does not make concurrently finalized pointers safe or guarantee behavior for
arbitrary statement corruption. The direct fixture tests SQLite binding semantics,
not CBOR correctness. The previous host TSan startup failure is not recounted as
a new check. Exhaustive scheduling, physical power-loss behavior, filesystem
quotas and Windows-native execution remain outside this validation.

Preserved records include the [retained-binding failure](validation-bindings-2026-10-31/before-test.log),
[already-protected size-limit control](validation-bindings-2026-10-31/existing-size-limit-protection.log),
[focused passing pair](validation-bindings-2026-10-31/after-tests.log),
[integration results](validation-bindings-2026-10-31/integration-tests.log),
[sanitizer results](validation-bindings-2026-10-31/sanitizer-tests.log),
[native commands/results](validation-bindings-2026-10-31/results.json),
[independent evidence](validation-bindings-2026-10-31/native-evidence.json), and
[verification log](validation-bindings-2026-10-31/verify-native.log).
The production diff and exact source/binary identities are preserved alongside them.

The subsequent [SQL completeness review](storage-completeness-review-2026-10-31.md)
fixes false duplicate classification and suppressed required writes. This binding
report retains its earlier source/binary fingerprints.
