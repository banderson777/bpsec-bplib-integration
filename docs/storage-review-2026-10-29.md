# Storage transaction follow-up — 2026-10-29

Historical snapshots below are superseded by the [storage completion report](storage-completion-2026-10-29.md), including its recovery format and durable custody semantics.

This pass continues the [earlier review and fixes](review-2026-10-29.md), focusing
on cleanup, egress marking, and custody SQL updates. Findings 7–9 below are
fixed in source and reproduced with real SQLite before the fixes. The earlier
[11-case deployment matrix](deployment-validation-2026-10-29.md) records the
previous binary snapshot; the validation below applies to these follow-up changes.
This is a review of these transaction paths, not an exhaustive dependency audit.

## 7. P2 — Failed cleanup leaves partial deletes and premature custody removal

Locations: `bplib/bpa/stor/src/bplib_stor_sql.c`, expiration and egress cleanup;
`bplib/bpa/stor/src/bplib_stor.c`, the garbage-collection caller.

A failed DELETE returned directly without rolling back the transaction. Both
cleanup variants also removed in-memory custody entries while merely selecting
rows, before any deletion committed. A failed COMMIT could be hidden by a
successful ROLLBACK, and byte/discard counters could reflect rolled-back work.
Second-statement preparation failure leaked the already prepared selection.

Before-fix evidence for both expired and egressed cleanup, using two custodial
rows and a trigger rejecting the second DELETE:

```text
status=-293 count=0 rows=1 CT-deletes=2 open-txn=1
```

Fix: share an atomic cleanup helper. Select the bounded batch, stage custody
IDs, delete it, commit, and only then remove custody entries and reduce stored
bytes. Every bind/step/COMMIT failure rolls back the owned transaction, preserves
the original error, reports zero discarded rows, and releases temporary memory
and statements. Allocation failure leaves the database/custody/accounting intact.
Empty cleanup succeeds without leaving a transaction open. Invalid byte totals
and ignored DELETEs fail without decrementing counters. A caller-owned outer
transaction is preserved when this helper cannot begin its own transaction.

The garbage-collection caller also used to overwrite a failed cleanup status
with a subsequent successful operation/size query. A separate regression caught
this after the SQL helper was fixed. The caller now retains its first failure
while still allowing independent maintenance/size checks to run. Its before-fix
transcript is `before-gc-status-fix.log` under the artifact root.

Custody IDs use temporary heap storage, allocated only when a custodial row is
selected, bounded by `BPLIB_STOR_DISCARDBATCHSIZE * sizeof(uint32_t)` (100,000
bytes at the default 25,000-row limit). This keeps that batch off the flight task
stack. The allocation failure path is covered by a linker-wrapped allocator in
the regression; SQLite itself still uses its real allocator and transactions.

## 8. P2 — Egress marking can commit a partial failed batch

Location: `bplib/bpa/stor/src/bplib_stor_sql_load.c`,
`BPLib_SQL_MarkBatchEgressed` and its implementation.

If marking a row failed, the old code still ran the retransmit-timer loop, whose
successful steps overwrote the original error. It could commit the earlier
partial marking and return success. Timer/COMMIT failure could also be hidden
by successful rollback. An empty remote batch could leave BEGIN uncommitted.

Before-fix evidence with the second egress-flag update rejected:

```text
mark failure: status=0 flags=1 not-egressed=1
```

Fix: stop immediately on the first failure, roll back the entire batch, and
preserve the error. Egress counts change only after commit. Prepared statements
are released on every preparation/execution path. Empty batches return without
starting a transaction; null/oversized batch arguments fail before database use.
Both local-delivery and remote-contact paths retain their valid behavior.

## 9. P2 — Custody SQL batches can hide failures and corrupt accounting

Location: `bplib/bpa/stor/src/bplib_stor_sql_cust.c`,
`BPLib_SQL_UpdateCustodialBundles` and its implementation.

The old first phase could fail marking custody release, then the timer phase
could succeed and overwrite that failure. A rollback could similarly hide the
failed update/COMMIT. Uncommitted egress counts could then decrement the cached
not-egressed count. Preparation failures leaked statements from earlier stages.

Before-fix evidence for rejected custody marking followed by a timer update:

```text
custody mark failure: status=0 timestamps=1100
```

The original sum of timestamps was 200; one operation changed despite the
failed batch. The function reported success.

Fix: execute batch operations in input order with immediate failure handling.
A failed operation/COMMIT rolls back the batch, zeros its egress count, and
returns the original failure. Accounting changes only on committed success.
Preparation cleanup releases all statements; invalid operation codes reject
the batch. A repeated successful delete-mark batch does not decrement twice.

## Regression coverage

`integration/storage_error_regression.c` compiles the real SQL initializer,
cleanup, garbage-collection caller, egress, and custody implementations. Four CTest modes exercise expired
cleanup, egressed cleanup, egress marking, and custody updates. Real SQLite
triggers fail individual writes and deferred foreign keys fail COMMIT. An
authorizer denies preparation after an earlier statement was prepared. Tests
check table contents, byte/egress counters, custody callbacks, autocommit,
absence of outstanding statements, and successful retry after removing faults.
Cleanup allocation failure, insufficient byte accounting, ignored DELETEs,
caller-owned transactions, empty batches, retransmit-stop values, and invalid
custody opcodes are covered as well. Time and
custody callbacks are controlled; custody callbacks assert they run after commit.

The before-fix transcript and copies of the original source files are in
`/tmp/bpsec-storage-followup-20261029/before-fixes.log` and `before/`.
The four modes all failed before the fixes and all pass afterward.

## Validation

- Fresh integration build: passed.
- Full CTest suite: **20/20 passed** in 6.26 seconds with local sockets allowed.
- Four new storage modes: **4/4 passed** under ASan/UBSan with leak detection.
- Native cFS BPLib rebuild: passed with the current source.
Three fresh five-process follow-up scenarios passed with the final native module:

| Check | Result | Seconds | Outcome |
| --- | --- | ---: | --- |
| `baseline-final` | PASS | 39.58 | Three segments match source; all five stores drained. |
| `relay-restart-final` | PASS | 86.76 | Nine records retained through 3→4 outage; relay 3 restarted with source stopped; matching segment and live-node custody drained. |
| `storage-failure-final` | PASS | 23.78 | Node 2 withheld custody; node 1 retained seven pending records; no output or completion. |

The evidence check verified the updated `bplib.so` hash at all 15 node runtimes,
keys only at endpoints 1 and 5, matching output hashes, and the expected pending
custody state. Positive runs use the existing six-second drain stability window.
The stopped source in the relay-outage case is excluded from the drain check.
Final BPLib SHA-256: `444f936fbfb633829d88ed850ee466c0947d2976eaa9cc2231beba0cd761b243`.

This follow-up reran these three scenarios; the complete 11-scenario matrix in
the earlier deployment report remains a record of the earlier module snapshot.
The new four-mode SQL fault regression provides detailed failure coverage for
this pass. It also invokes the real garbage-collection caller to verify that
later successful housekeeping does not hide an earlier failure.

Artifacts remain under `/tmp/bpsec-storage-followup-20261029`: `before-fixes.log`,
`before-gc-status-fix.log`, original sources in `before/`, native build logs,
`source-hashes.json`, `binary-hash.json`, `deployment-results.json`,
`deployment-evidence.json`, the sequential runner `run_deployment.py`, per-node
logs, and fresh runtime databases. The initial deployment checks before the
last caller-status fix are preserved separately; final run names end in `-final`.

Reproduce from the workspace root:

```sh
cmake -S integration -B /tmp/bpsec-storage-followup-20261029 \
  -DCFS_BUILD=/tmp/bpsec-deployment-20261029/build/cfs
cmake --build /tmp/bpsec-storage-followup-20261029 -j4
ctest --test-dir /tmp/bpsec-storage-followup-20261029 --output-on-failure --timeout 60

cmake -S integration -B /tmp/bpsec-storage-followup-20261029-asan \
  -DCMAKE_C_FLAGS='-g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'
cmake --build /tmp/bpsec-storage-followup-20261029-asan \
  --target storage_error_regression -j4
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir /tmp/bpsec-storage-followup-20261029-asan \
  -R '^storage_error_' --output-on-failure --timeout 20
```

The CFS build override must name a configured build with usable generated
headers. Socket tests and LeakSanitizer require appropriate host permissions.
No existing recording, custody database, key, or prior validated installation
was changed. Fresh runtime copies and a separately installed module snapshot
are used for deployment checks. Native Windows, hardware power-loss behavior,
and the complete deployed process under sanitizers are outside this pass.

## Stored-bundle recovery follow-up

A subsequent loader review fixed the following defects in
`bplib/bpa/stor/src/bplib_stor_sql_load.c`:

- Check a null instance before dereferencing it and initialize the caller's
  output to NULL before any validation, SQL operation, or allocation. Partial
  loads free their pool blocks without leaving a stale caller pointer.
- Bind SQLite row IDs with `sqlite3_bind_int64`, preserving IDs above INT32_MAX.
- Classify preparation/read/corruption errors as load errors before considering
  the missing-bundle result; keep allocation failures distinct.
- Finalize and clear the global query statement on every wrapper exit after
  preparation, and explicitly order stored chunks by their insertion IDs.
- Reject a metadata-only record and empty or oversized wire chunks.
- Reject metadata containing a non-null `AdminRecordPayload`. The legacy format
  stores a raw process address; returning it can cause invalid access or free
  after recovery. Rejection leaves the database record intact. This is a safety
  restriction: administrative records with such payloads cannot currently be
  recovered through this loader. Complete support needs reconstruction from
  encoded bytes or a versioned pointer-free storage format.

The new `storage_error_load_regression` uses real SQLite and an allocator that
poisons recycled memory, injects allocation failures, and counts live blocks.
It covers null/invalid arguments, row ID 2147483648, successful loading, failure
at both allocations, oversized/empty chunks, metadata-only records, persisted
administrative pointers, wrong metadata size, missing rows, and failed query
preparation. Each failed load must return NULL, release all allocated blocks,
and leave no statement or transaction open. The allocator boundary is stubbed;
this test does not claim coverage of the native pool implementation.

Validation after these loader changes: **21/21 integration CTests passed** in
5.99 seconds; **5/5 storage failure modes passed under ASan/UBSan with leak
checks** in 0.08 seconds. Builds are in the same integration directories shown
above; their previous source hashes and native-deployment evidence describe the
preceding snapshot. The native five-node deployment has **not** been rerun for
this loader follow-up.

Further review remains warranted for restoration of transient bundle metadata,
portable administrative-record persistence, and retry/locking behavior of the
higher-level custody update queue. These are not covered by the loader fixes or
a claim of exhaustive security validation.

## Recovery metadata and custody queue follow-up

The next pass closes two further correctness gaps:

1. The loader now joins the parent bundle row when reading chunks and restores
   `TotalBytes`, `IsCustodial`, and `RetransmitTime`. Previously these fields
   contained recycled pool bytes. It zero-initializes the bundle object, validates
   persisted integer types/ranges, and checks that metadata-plus-wire byte count
   matches `bundle_bytes`. Missing parents, invalid custody flags, invalid timers,
   and inconsistent byte counts fail without returning an object. The legacy
   database does not store contact IDs or local-origin metadata: recovered
   `IngressID` and `EgressID` are UINT32_MAX, and `LocalBundle` defaults to false.
   The egress caller assigns its actual output contact. Unknown ingress cannot
   accidentally route a custody acknowledgement to contact zero.
2. Custody update batches are cleared only after successful SQL commit. Failed
   batches remain in memory and are retried during storage maintenance, including
   maintenance cycles where traffic suppresses garbage collection. Capacity checks,
   retry, and append now share the storage mutex. A full failed batch cannot be
   overwritten by a concurrent producer. Invalid operations are rejected before
   enqueueing.

`BPLib_STOR_AddToCustodialUpdateBatch` now returns `BPLib_Status_t` (public header
and generated test stub updated). Success means storage owns the request,
including retry responsibility after a failed automatic flush. Failure means no
new request was accepted. Existing source callers that discard a return value
remain valid C, but callers should inspect it; dependent native modules must be
rebuilt together. Custody signal processing now propagates rejection, preserves
custody state/count for an unqueued transfer, and stops that sequence collection.
A replay or subsequent retransmission can retry unaccepted requests. After
releasing the CT lock to queue an accepted transfer, processing looks the entry
up again before changing it, avoiding use of a pointer that concurrent cleanup
may have invalidated. The two locks are never held together in this path.

`storage_error_custody_queue_regression` exercises a full failed queue, rejection
of the next request, retained explicit-flush failure, failed maintenance retry,
successful maintenance retry, and four concurrent producers submitting 548
requests. It compiles the real custody sequence processor and verifies that
queue rejection preserves custody, while a later successful retry transfers it.
The CT database lookup and accounting/event boundaries are controlled; SQLite,
the storage queue, mutexes, maintenance, and sequence processing are real.
The load mode additionally checks restored fields from poisoned memory, invalid
custody flags, and inconsistent byte accounting.

Validation for this snapshot: **22/22 integration tests passed** (4.99 seconds),
**6/6 storage modes passed ASan/UBSan with leak detection** (0.17 seconds), and
the native `bplib` module rebuilt successfully. Existing unused-parameter compiler
warnings remain in storage table validation and retransmit configuration.
All three native scenarios also passed with the rebuilt module:

| Scenario | Seconds | Verified result |
| --- | ---: | --- |
| Baseline | 38.95 | Three source-matching segments; custody drained on all five nodes |
| Relay restart, 3–4 outage | 86.09 | Nine retained records; relay 3 restarts with source stopped; output matches and live-node custody drains |
| Node 2 storage failure | 24.23 | No completed output; source retains seven records; node 2 accepts no custody |

Evidence is preserved under `/tmp/bpsec-recovery-20261029`: per-scenario logs and
runtimes, `run_deployment.py`, `deployment-results.json`,
`deployment-evidence.json`, `collect_deployment.py`, `hashes.json`,
`binary-hash.json`, `integration-tests.log`, and `sanitizer-tests.log`.
The collector checked module hashes in all 15 node runtimes, output sizes and
SHA-256 hashes, custody rows, and keys present only at endpoints 1 and 5.
The source's retained rows during the relay outage are intentional because that
source is stopped. Earlier deployments and their evidence remain separate.
These three scenarios validate this snapshot; the earlier eleven-case security
matrix has not been rerun in full.

Remaining limits: administrative payload pointers still fail closed as described
above; portable reconstruction and persistence of original ingress/local-origin
metadata require a versioned storage format. Pending custody updates survive SQL
failures in memory, not a process crash before commit. A full queue under a
persistent database failure returns an error rather than growing without bound;
upstream custody retains ownership and must retry. The tests do not establish
crash-atomic persistence of the in-memory CT database, exhaustive concurrency
correctness, or complete security assurance.
