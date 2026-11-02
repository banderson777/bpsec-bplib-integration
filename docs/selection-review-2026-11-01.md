# Do not publish failed egress selections — 2026-11-01

This follows the [database-size review](db-size-review-2026-11-01.md).
A failed SQL egress selection could leave newly appended bundle IDs visible in
its load batch. The next storage poll could consume those IDs without completing
a successful selection.

## Confirmed failures

The real SQLite regression interrupts a SELECT after its first result row. The
original `BPLib_SQL_FindForEIDs` returned `BPLIB_STOR_SQL_LOAD_IDS_ERR`, but the
previously empty batch had size 1 and reader index 0. This is recorded in the
[interruption log](validation-selection-2026-11-01/before-selection-failure.log).
Storage's next poll checks whether the batch is empty before selecting again;
that partial batch could therefore enter the bundle-loading branch.

A second test starts with 59999 entries in the configured 60000-entry batch and
selects two matching IDs. The first append succeeds, the second fails because the
batch is full, and the wrapper reports an error while leaving size 60000. The
[capacity log](validation-selection-2026-11-01/before-selection-capacity.log)
records the changed size and preserved reader. This direct-helper case checks
append rollback; normal storage polling selects only into empty batches.

These cases do not show invalid destination filtering, database mutation, bundle
loss or a custody-acceptance bypass. They demonstrate that selection errors did
not prevent partial IDs from becoming visible through the batch's logical bounds.

## Fix and contracts

The public selection wrapper saves the batch's size and read index before query
execution. It restores both if preparation, binding, stepping, append or finalizing
fails. Successful finalization is now required before reporting success. Existing
entries remain intact because additions only write after the original prefix.

This provides logical rollback without copying the configurable batch onto the
stack. Appended slots beyond the restored size may retain old bytes; they are
outside the valid batch range and cannot be consumed through the batch API.
Callers must retain normal storage serialization while selection executes.

A successful query still appends to the existing batch and preserves its reader.
Capacity exhaustion remains an error; this pass does not change the query limit
or promise partial success when a caller provides too little free space. Invalid
input and `dtn:none` handling remain unchanged. Public signatures, SQL schema,
wire formats and database transaction behavior are unchanged. The private
implementation still appends provisionally; the public wrapper owns rollback.

## Tests

The existing `sql_ownership_regression` keeps its 25 overlap rounds and database-
size checks. Six new cases exercise local and remote selection:

- Interruption after a row, into an empty batch.
- Interruption after a row, with an existing consumed prefix.
- Capacity exhaustion after one append to a nearly full batch.

Each failure must preserve the original size, read index and all existing prefix
IDs, leave no prepared statements, and preserve SQLite autocommit state. Removing
the interrupt must allow a successful retry, appending both correct IDs while
preserving the original reader. These tests link the actual selection, filter,
batch and SQLite implementations.

## Validation

- Full integration suite: **37/37 passed**, 8.15 seconds.
- Focused ASan/UBSan with leak detection: **1/1 passed**, 0.26 seconds.
- Rebuilt native BPLib and fresh five-node baseline: **1/1 passed**, 40.89 seconds.
- Independent checks verified all five runtimes against pinned module/host/startup
  identities, source/output hashes, drained custody and endpoint-only key placement.
  No receipt-clock or BPLib initialization errors were found.

See the [native results](validation-selection-2026-11-01/results.json),
[independent evidence](validation-selection-2026-11-01/native-evidence.json) and
[verification log](validation-selection-2026-11-01/verify-native.log).

The suite count remains 37 because an existing regression was expanded. The
[detailed captured test output](validation-selection-2026-11-01/selection-test-detail.log)
records all six failures and the retained geometry checks. See the
[integration log](validation-selection-2026-11-01/integration-tests.log) and
[sanitizer log](validation-selection-2026-11-01/asan-tests.log).

## Reproduction and limits

Follow [build and test](build-and-test.md), then run:

```sh
cmake --build INTEGRATION_BUILD --target sql_ownership_regression -j4
ctest --test-dir INTEGRATION_BUILD -R '^sql_ownership_regression$' --output-on-failure
```

`sql_ownership_regression selection-failure` isolates interruption and retry;
`sql_ownership_regression selection-capacity` isolates buffer exhaustion.
No-argument execution runs these alongside the prior ownership and geometry cases.

The focused tests inject failures through the actual SQL helper and batch API.
They do not force a failing selection through a live native transport or claim
exactly-once delivery. Hardware power loss, Windows-native behavior and a new TSan
execution remain untested. Earlier fault matrices retain their own pinned builds;
they are not counted as new executions for this fix. The
[evidence directory](validation-selection-2026-11-01/README.md) preserves failures,
the production patch, test/build logs and source/binary identities.

Later validation: the [egress marking retry review](egress-retry-review-2026-11-01.md) retains consumed batches after SQL marking errors. Results above retain their original pinned snapshot.
