# Custody SQL ownership and atomic scheduling — 2026-10-31

This follows the [SQL completeness review](storage-completeness-review-2026-10-31.md).
The custody SQL module shared statement handles and a mutable query buffer between
instances. Per-instance storage locks cannot protect those process-wide objects.
A separate failure also allowed a failed retransmission update to leave changed rows.

## Reproduced failures

A deterministic two-database test pauses instance A inside an UPDATE trigger while
instance B completes its own operation. Before the fix, a custody batch failed on
A after B replaced/finalized the shared handle. Filtered scheduling also leaked A's
statement. These results are preserved in the [batch overlap log](validation-custody-transactions-2026-10-31/before-0.log)
and [scheduling overlap log](validation-custody-transactions-2026-10-31/before-3.log).

A real SQLite trigger raising FAIL on the second row reproduced partial scheduling:
the function returned error and reported zero updates, but one row remained changed.
The [failure log](validation-custody-transactions-2026-10-31/before-filter-failure.log)
records that result. A failed scheduling operation must leave no partial changes.

## Fixes and contracts

Custody batches now own their three prepared statements in a local context. The
filtered scheduler owns its statement and builds its query with `sqlite3_mprintf`;
it frees the query after finalizing the statement on every prepared exit. This
removes the shared mutable query buffer without adding another large stack buffer.

Filtered scheduling now begins an explicit transaction. Binding, stepping and
COMMIT errors roll back its changes and leave the published update count zero.
Only successful COMMIT publishes the row count. If BEGIN fails because a caller
already owns a transaction, the function returns failure without rolling back
that caller's work. Existing custody-batch transaction semantics remain in place.

Public wrapper signatures, the SQL schema and wire formats are unchanged. Private
implementation helpers now receive the locally owned statements. Callers must
continue to serialize access to each instance/connection with the storage lock.
Independent connections no longer share custody statement/query state.

## Tests and validation

`integration/custody_parallel_regression.c` links the real SQL initialization,
filter construction and custody implementation against SQLite and pthreads.
Each instance contains two matching rows and one unrelated sentinel. Twelve
controlled overlap rounds exercise deletion, retransmission start, retransmission
stop and filtered scheduling, each with success, a second-row FAIL trigger, and
an authorizer-denied preparation. Assertions check instance-specific filters,
committed counters, unchanged sentinel rows, rollback, autocommit restoration and
zero leaked prepared statements. Separate cases cover a deferred-constraint
COMMIT failure followed by a successful retry, and preservation of an outer
transaction rejected by BEGIN.

- Full integration suite: **36/36 passed**, 7.44 seconds.
- Focused ASan/UBSan suite with leak detection: **15/15 passed**, 1.98 seconds.
- Native mission rebuilt and installed with the corrected SQL module.

An intermediate header edit accidentally changed a wrapper declaration; compilation
caught it and it was corrected before the passing builds. Public signatures remain
unchanged. The evidence retains the final passing build/test records.

Native validation: **5/5 passed**. Independent verification checked all 25
runtimes against pinned module, host and startup identities; endpoint-only keys;
source/output hashes; custody and receiver-restart checkpoints; and immutable
output after duplicate replay. No receipt-clock or BPLib initialization errors
were found.

| Scenario | Result | Evidence |
| --- | --- | --- |
| Original-ID duplicate replay | Passed, 141.08 s | Nine original IDs recovered; all five hosts drained; output unchanged; 44 unique IVs |
| Baseline transfer | Passed, 31.69 s | Three matching segments; custody drained |
| Incomplete-receipt receiver restart | Passed, 65.00 s | Preserved receipts; completion after readiness; three matching segments |
| Dropped custody acknowledgement | Passed, 62.25 s | Fault fired; matching outputs; custody drained |
| Storage failure | Passed, 25.97 s | No ground output/completion; source retains seven pending bundles; refusing relay retains none |

The [integration log](validation-custody-transactions-2026-10-31/integration-tests.log),
[sanitizer log](validation-custody-transactions-2026-10-31/asan-tests.log),
[native results](validation-custody-transactions-2026-10-31/results.json),
[independent evidence](validation-custody-transactions-2026-10-31/native-evidence.json)
and [verification log](validation-custody-transactions-2026-10-31/verify-native.log)
preserve the final results. Trigger faults are exercised in focused tests; the
native cases validate integration and recovery after the production changes.

## Reproduction and remaining limits

Follow [build and test](build-and-test.md) for dependencies, then run:

```sh
cmake --build INTEGRATION_BUILD --target custody_parallel_regression -j4
ctest --test-dir INTEGRATION_BUILD -R '^custody_parallel_regression$' --output-on-failure
```

No-argument execution runs all cases. Test arguments `0`, `3` and `filter-failure`
isolate the historical batch-overlap, scheduling-overlap and partial-update cases.
These are test controls, not product runtime options.

This pass covers the custody SQL module. Shared statements in SQL loading and
maintenance helpers remain a separate review item; this is not a claim that every
storage helper supports concurrent instances. Tests use separate in-memory SQLite
connections with normal instance locking; they do not establish same-file writer
fairness or exhaustive thread schedules. No new TSan run is claimed because the
previously documented host runtime cannot start it. Hardware power loss and
Windows-native execution remain untested.

See the [evidence directory](validation-custody-transactions-2026-10-31/README.md)
for preserved failure logs, passing checks, production diff and pinned identities.

Later validation: the [SQL loading and maintenance review](sql-ownership-review-2026-10-31.md) addresses the shared statement/query state left for follow-up above. Earlier results retain their original snapshot.
