# Correct database-size telemetry — 2026-11-01

Following the [SQL ownership review](sql-ownership-review-2026-10-31.md), this pass
checks the database-size helper used by storage maintenance. Two confirmed errors
could publish an inaccurate `StorageSize`: assuming 4096-byte pages regardless of
database geometry, and reporting success when the page-count step failed.

## Reproductions and impact

A real SQLite database with two 512-byte pages occupied 1024 bytes. The original
helper returned success and reported 8192 bytes. For larger pages the same
constant can underreport instead. The [geometry failure log](validation-db-size-2026-11-01/confirmed-before-db-size.log)
compares reported size against both SQLite geometry and the actual file length.

A statement-trace callback arms SQLite's progress interrupt after preparation,
so the page-count execution fails. The old helper ignored that step result and
returned success with zero bytes, as recorded in the
[step failure log](validation-db-size-2026-11-01/confirmed-before-db-size-failure.log).
Storage maintenance assigns `StorageSize` only on success, so this false success
could replace its last telemetry value with zero. A failure during preparation
already returned an error; that control is not claimed as a newly fixed bug.

These are allocated database-page telemetry defects. Bundle-byte budgets use
separate accounting; this pass does not claim a reproduced custody-acceptance or
storage-capacity bypass through this helper.

## Fix

`BPLib_SQL_GetDbSize` reads both `PRAGMA page_count` and `PRAGMA page_size` using
locally owned statements. Each read requires one nonnegative integer row, normal
statement completion, and successful finalization. The result is published only
after both reads succeed, the page size is positive, and multiplication fits
`size_t`. Any query failure leaves the output zero and returns failure, so the
existing maintenance caller preserves its previous `StorageSize`.

Null instance/output arguments return the standard null-pointer error. A missing
database handle returns failure. No public signatures, schemas, wire formats or
custody behavior changed. The only new production helper is file-local.

The result represents allocated pages in the main database, including free pages.
It does not include WAL/journal/SHM sidecars or filesystem allocation overhead.
Normal per-instance serialization remains required; this is not a transactional
snapshot across arbitrary external schema/page-size changes.

## Tests

The existing `sql_ownership_regression` retains its 25 ownership overlap rounds
and adds real temporary-file checks for all eight power-of-two page sizes from
512 through 65536 bytes. Each size must match `page_count * page_size` and the
actual file length. Files are closed and removed after testing.

Statement tracing and progress interrupts cover failed execution of each PRAGMA.
Authorizer denials cover preparation failure of each, including failure after the
first query has completed. Tests require zero output and no leaked statements,
then retry successfully after removing the faults. Null instance/output and absent
database-handle cases are also checked.

A preliminary interruption was armed before preparation and correctly failed on
the old helper. The final reproducer targets execution explicitly, as described
above. Only the preserved final logs support the false-success claim.

## Validation

- Expanded ownership/size regression: **1/1 passed**, including the 25 overlap rounds.
- Focused ASan/UBSan with leak detection: **1/1 passed**, 0.22 seconds.
- Full integration suite: **37/37 passed**, 11.17 seconds.
- Rebuilt native BPLib module and fresh five-node baseline: **1/1 passed**, 41.54 seconds.
- Independent verification passed for all five native runtimes: pinned module/host/
  startup identities, matching output hashes, custody drain and endpoint-only keys.
  No receipt-clock or BPLib initialization errors were found.

The [detailed test output](validation-db-size-2026-11-01/size-test-detail.log)
records all eight page geometries and both rejected interrupted queries. See the
[integration results](validation-db-size-2026-11-01/integration-tests.log),
[sanitizer results](validation-db-size-2026-11-01/asan-tests.log),
[native results](validation-db-size-2026-11-01/results.json),
[independent evidence](validation-db-size-2026-11-01/native-evidence.json) and
[verification log](validation-db-size-2026-11-01/verify-native.log).

The suite remains 37 entries because an existing regression was expanded. This
pass uses focused sanitizer coverage of the changed helper and a fresh native
baseline; earlier 16-test sanitizer and five-scenario native matrices retain their
own source snapshots and are not counted as executions for this fix.

## Reproduction and limits

Follow [build and test](build-and-test.md), then run:

```sh
cmake --build INTEGRATION_BUILD --target sql_ownership_regression -j4
ctest --test-dir INTEGRATION_BUILD -R '^sql_ownership_regression$' --output-on-failure
```

`sql_ownership_regression db-size` isolates geometry and error checks;
`sql_ownership_regression db-size-failure` isolates error checks. Normal no-argument
execution includes all ownership and size cases. These are test-executable modes.

Overflow guards are present, but no 32-bit build or synthetic oversized database
is claimed. Hardware power loss, Windows-native behavior and a new TSan execution
remain untested. The [evidence folder](validation-db-size-2026-11-01/README.md)
contains the production diff, pre-fix failures, passing logs and pinned identities.

Later validation: the [egress-selection review](selection-review-2026-11-01.md) prevents partial batch publication after selection errors. Results above retain their original pinned snapshot.
