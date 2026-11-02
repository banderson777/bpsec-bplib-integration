# Require complete SQL writes before custody acceptance — 2026-10-31

This follows the [SQLite bindings review](sqlite-bindings-review-2026-10-31.md).
Transaction review reproduced three cases where a partial batch committed and
storage attempted custody acceptance for bundles that were not fully stored.

## Reproduced failures

The transaction loop treated any `SQLITE_CONSTRAINT_UNIQUE` result as evidence
of an existing bundle. A UNIQUE failure from an unrelated insert trigger therefore
became a reported duplicate instead of a batch failure. The reproduction committed
one metadata row but attempted two custody acceptances and two egress pushes.

Separately, `SQLITE_DONE` was treated as sufficient proof of an insert. A trigger
using `RAISE(IGNORE)` could suppress a blob or recovery-record insert while the
batch still committed two metadata rows and attempted both custody acceptances.
The logs preserve these outcomes rather than inferring the failure from mocks.
This is a confirmed false-success regression, distinct from the earlier binding
size-limit control that already rolled back correctly.

## Fixes

The writer now looks up an existing bundle ID inside the transaction before any
new write. Only an ID already present at that point is counted as redundant.
This also recognizes duplicates earlier in the same batch. Duplicate-only work
adds no stored bytes, so existing IDs can be skipped even at a full byte budget.
Any later write error, including an unrelated UNIQUE failure, rolls back the batch.

Metadata, recovery-record and blob inserts must each affect exactly one row
before their success is published. SQLite's
[change-count contract](https://www.sqlite.org/c3ref/changes.html) excludes auxiliary
trigger changes, so a suppressed primary insert cannot borrow a trigger's row
count to look successful. The blob helper still releases borrowed bindings and
preserves failure through cleanup. A failed completeness check follows the
existing batch rollback/refusal path: zero committed counters, no egress, and
no custody acceptance.

The early declared-size transaction check now uses the same bounded subtraction
pattern as the existing post-encoding check. The serialized-size check was
already present and remains; this pass does not claim a previously demonstrated
committed-byte overflow through that final check.

The duplicate lookup is prepared once per `BPLib_SQL_Store` invocation and owned
by its local statement context alongside metadata and blob statements. Its read
cursor is reset and cleared before writes/commit; all three handles are finalized
on every store exit. A lookup-preparation error fails closed without affecting
another instance's in-progress transaction.

Public store signatures, wire formats and SQL schema are unchanged. The private
statement context now has a lookup handle; internal consumers that manually
construct a context for `StoreImpl` must provide the new prepared query or use
the unchanged `BPLib_SQL_Store` entry point. Normal storage serialization remains
required for each instance/connection.

## Tests

`storage_commit_regression` uses actual SQLite and the real transaction/finalization
code. It now covers:

- An unrelated UNIQUE failure from a metadata insert trigger: the whole batch
  must roll back instead of being treated as a duplicate.
- Blob, recovery-record and metadata inserts suppressed by `RAISE(IGNORE)`:
  no partial rows, custody acceptance or egress may remain.
- Genuine previously stored IDs at a full byte budget: two duplicates, no new
  stored bytes or custodial rows, original data retained.
- A repeated new ID within one batch: one new record and one duplicate.
- Prior bind/COMMIT failures, capacity checks and pending-batch preservation.

Before the fix, the first three fault modes all returned success with two custody
acceptances. After the fix, all roll back with both refusals outside the storage
mutex and complete memory cleanup. The metadata no-op case is also covered as
an invariant check; no unobserved pre-fix false acceptance is claimed for it.

`storage_parallel_regression` adds deterministic overlap while the second writer
is denied read access for lookup preparation. The first writer must still commit;
the failing writer must leave no row and no open statements. Alongside the prior
blob-preparation failure and 80 parallel writes, the final expected database sizes
are now 43 rows in A and 41 in B for metadata, blobs and recovery sidecars, with
the correct instance-specific payloads. Earlier reports' 42/41 counts describe
their old fixture snapshot.

## Validation

- Integration CTest: **35/35 passed**, 7.66 seconds.
- ASan/UBSan with leak detection: **14/14 passed**, 2.01 seconds.
- Focused transaction/binding/parallel checks: **3/3 passed**.
- Three independent pre-fix false-success modes reproduced with real SQLite.

Native validation: **5/5 passed** on the final pinned build. Independent checks
cover all 25 runtimes, module/host/startup identities, endpoint-only keys,
source/output hashes, custody/restart checkpoints and preserved receipt hashes.
No receipt-clock or BPLib initialization errors were found.

| Scenario | Result | Evidence |
| --- | --- | --- |
| Original-ID duplicate replay | Passed, 122.25 s | Nine original IDs recovered; all five live hosts drained; immutable output/completion; 42 unique IVs |
| Baseline transfer | Passed, 40.91 s | Three matching segments; custody drained |
| Incomplete-receipt receiver restart | Passed, 65.80 s | Saved receipt unchanged; completion after current readiness; three matching segments |
| Dropped custody acknowledgement | Passed, 63.00 s | Drop fired; transfer recovered; matching outputs and custody drained |
| Storage failure | Passed, 26.58 s | No ground output/completion; source retains seven pending bundles, refusing relay retains none |

The trigger and unrelated-constraint faults are exercised in focused real-SQLite
tests. Native runs validate integration/recovery after the change; they do not
recount historical matrices as new executions.

The [evidence directory](validation-completeness-2026-10-31/README.md) preserves
failure logs, exact source/binary fingerprints, build/test logs and fresh native
scenario commands/results. The suite count stays 35 because existing regression
executables were expanded.

## Reproduction and limits

Use the configured dependencies in [build and test](build-and-test.md):

```sh
cmake --build INTEGRATION_BUILD --target storage_commit_regression storage_binding_regression storage_parallel_regression -j4
ctest --test-dir INTEGRATION_BUILD -R '^storage_(commit|binding|parallel)_regression$' --output-on-failure
```

To isolate a fault path, run `storage_commit_regression unique`, `blob` or
`recovery`; no-argument execution runs all cases. These are test-executable
arguments, not product fault controls.

The duplicate lookup trusts previously stored bundle IDs, as before. It does not
revalidate or repair historical corrupt/partial records. Row-count checks detect
suppressed required writes; they do not authenticate arbitrary trigger-rewritten
values or hostile local schema/storage changes. The tests use independent database
connections and require normal per-instance locking; they do not prove same-file
multi-connection fairness or exhaustive schedules. Hardware power loss,
Windows-native behavior and a new TSan execution remain outside this validation.

Preserved records include the [unrelated UNIQUE failure](validation-completeness-2026-10-31/before-unique.log),
[skipped blob failure](validation-completeness-2026-10-31/before-blob.log),
[skipped recovery failure](validation-completeness-2026-10-31/before-recovery.log),
[focused passing tests](validation-completeness-2026-10-31/focused-tests.log),
[integration results](validation-completeness-2026-10-31/integration-tests.log),
[sanitizer results](validation-completeness-2026-10-31/sanitizer-tests.log),
[native commands/results](validation-completeness-2026-10-31/results.json),
[independent evidence](validation-completeness-2026-10-31/native-evidence.json), and
[verification log](validation-completeness-2026-10-31/verify-native.log).
The production diff and exact source/binary identities are preserved alongside them.

Later validation: the [custody SQL review](custody-transactions-review-2026-10-31.md) adds instance isolation and atomic retransmission scheduling. Results above describe the earlier pinned build.
