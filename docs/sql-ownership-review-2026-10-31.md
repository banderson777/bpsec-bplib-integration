# SQL loading and maintenance ownership — 2026-10-31

This completes the shared-statement follow-up identified in the
[custody SQL review](custody-transactions-review-2026-10-31.md).
The remaining SQL loading and maintenance helpers used process-global prepared
statements, although their callers lock only an individual storage instance.
Egress selection also constructed SQL in a shared mutable buffer.

## Confirmed failures

The new regression pauses instance A inside SQLite while instance B completes the
same helper on an independent connection. On the original implementation, six
isolated runs failed: local egress marking, expired discard, local selection,
stored count, egressed count and total-byte count. Egressed count terminated with
SIGSEGV; the other runs stopped on correctness or statement-cleanup assertions.
The [recorded exits](validation-sql-ownership-2026-10-31/before-results.json)
and individual `before-N.log` files preserve these observations. The crash log
is empty because the process terminated before an assertion diagnostic.

## Fixes and contracts

Each scalar count helper now prepares, uses and finalizes its own statement.
Discard calls own both their selection and deletion statements across collection,
transaction, rollback and cleanup. The existing expired/egressed implementation
entry points retain their signatures and now prepare their own statement pair,
instead of depending on handles set up by another function.

Egress selection owns its prepared statement and constructs query text with
`sqlite3_mprintf`, freeing that allocation after finalization. Local and remote
filter layouts therefore cannot replace each other's buffers or bindings. The
local/remote egress marker owns both its marking and retransmission statements.
Legacy bundle loading also owns its blob-selection statement; portable recovery
already used local statement handles. Obsolete statement externs are removed from
the SQL headers, including declarations for previously absent legacy globals.

Public operation signatures, SQL schema and wire formats remain unchanged.
Private loading implementation helpers receive the statements explicitly; direct
internal users of those helpers must update their arguments. The removed globals
are no longer an extension interface. Normal per-instance storage serialization
remains required. No process-wide mutex was added.

Existing transaction behavior is preserved: failed egress/discard batches roll
back, counters and custody effects follow successful COMMIT, and failure to begin
an already active transaction does not roll back the caller's work. This pass
fixes ownership, rather than claiming these rollback guarantees were first added
here. No file-scope prepared-statement pointer or mutable generated query buffer
remains in the storage SQL source modules.

## Regression coverage

`integration/sql_ownership_regression.c` links actual SQL initialization, loading,
filter generation, egress and discard code against SQLite and pthreads. SQLite
row tracing gates read queries; trigger callbacks gate updates/deletes. Instance
locks remain held throughout each helper, demonstrating why those locks cannot
protect shared process-wide handles. Custody deletion is stubbed for this isolated
fixture; existing custody/storage regressions and native scenarios cover its
integration.

Twenty-five deterministic overlap rounds cover:

- Local and remote egress marking, with success, second-row FAIL, denied initial
  preparation, and (remote only) denied retransmission-statement preparation.
- Expired and egressed discard, with success, second-delete FAIL, denied selection
  preparation, and denied deletion preparation after selection was prepared.
- Local and remote destination selection, stored count, egressed count and total
  bytes, each with success and denied preparation on the second instance.

Assertions check both databases, destination-filter results, unrelated sentinel
rows, rollback of changed rows, per-instance byte/counter accounting, autocommit
restoration and absence of leaked prepared statements. Existing full-suite tests
continue to exercise legacy/portable load behavior; the new overlap fixture does
not construct a legacy bundle for concurrent decoding.

## Validation

- Full integration suite: **37/37 passed**, 7.82 seconds.
- Focused ASan/UBSan with leak detection: **16/16 passed**, 2.45 seconds.
- Fresh native build and matrix completed on **2026-11-01**: **5/5 passed**.
  The temporary build directory was lost between sessions; the earlier unfinished
  build produced no native result. Current evidence comes from a new build.

Independent verification checked all 25 runtimes against pinned module, host and
startup identities, endpoint-only key placement, output/source hashes, custody
state, preserved receiver receipts and duplicate replay checkpoints. No receipt-
clock or BPLib initialization errors were found.

| Scenario | Result | Evidence |
| --- | --- | --- |
| Original-ID duplicate replay | Passed, 145.11 s | Nine original IDs recovered; all five hosts drained; immutable output; 33 unique IVs |
| Baseline transfer | Passed, 75.49 s | Three matching segments; custody drained |
| Incomplete-receipt receiver restart | Passed, 88.43 s | Receipts preserved; completion after readiness; three matching segments |
| Dropped custody acknowledgement | Passed, 48.48 s | Fault fired; matching outputs; custody drained |
| Storage failure | Passed, 33.02 s | No ground output or completion; source retains seven pending bundles; refusing relay retains none |

See the [integration results](validation-sql-ownership-2026-10-31/integration-tests.log),
[sanitizer results](validation-sql-ownership-2026-10-31/asan-tests.log),
[native commands/results](validation-sql-ownership-2026-10-31/results.json),
[independent evidence](validation-sql-ownership-2026-10-31/native-evidence.json),
[verification log](validation-sql-ownership-2026-10-31/verify-native.log) and
[build log](validation-sql-ownership-2026-10-31/native-build.log).
The focused SQLite tests inject the new overlap/failure conditions. Native cases
exercise integration and recovery on the corrected build.


## Reproduction and limits

Use the dependencies described in [build and test](build-and-test.md), then run:

```sh
cmake --build INTEGRATION_BUILD --target sql_ownership_regression -j4
ctest --test-dir INTEGRATION_BUILD -R '^sql_ownership_regression$' --output-on-failure
```

No-argument execution runs all 25 rounds. An integer test argument isolates a
success overlap: 0 local egress, 1 remote egress, 2 expired discard, 3 egressed
discard, 4 local selection, 5 remote selection, 6 stored count, 7 egressed count,
8 total bytes. These are test controls, not product runtime fault settings.

The tests cover independent connections with normal storage locking. They do not
prove all library operations thread-safe, same-file multi-writer fairness, or all
possible schedules. No new TSan execution is claimed because of the previously
recorded host startup limitation. Hardware power loss and Windows-native behavior
remain outside this validation. Other reviews' historical validation results and
pinned snapshots remain intact.

The [evidence directory](validation-sql-ownership-2026-10-31/README.md) preserves
failure logs, the production patch, current tests and pinned build/source evidence.

Later validation: the [database-size telemetry review](db-size-review-2026-11-01.md) fixes page geometry and failed size-query reporting. The results above retain their original pinned snapshot.
