# Validate standalone storage accounting reads — 2026-11-01

The [startup accounting review](accounting-review-2026-11-01.md) moved recovery to
a validated combined snapshot. The three standalone scalar helpers still used
unchecked conversions and published results before checking statement completion.
This pass closes that direct-call gap.

## Reproduced behavior

`BPLib_SQL_GetTotalBundleBytes` returned success and converted a negative declared
byte total into 18446744073709551615 on this host. `BPLib_SQL_GetNumEgressed`
returned success with value 2 for a single row whose egress flag was 2. The
[byte failure](validation-scalars-2026-11-01/before-scalar-bytes.log) and
[flag failure](validation-scalars-2026-11-01/before-scalar-flag.log) record these.

A connection-local test aggregate returned `UINT32_MAX + 1` for COUNT(*).
`BPLib_SQL_GetNumStoredBundles` narrowed it through `sqlite3_column_int`, reported
success and returned zero. The [count-boundary failure](validation-scalars-2026-11-01/before-scalar-wide.log)
is synthetic: no database with billions of records was created or claimed.
The valid `UINT32_MAX` boundary is also checked.

The helpers returned as soon as their first result row arrived. In the
[completion-path test](validation-scalars-2026-11-01/before-scalar-completion.log),
a row callback armed a subsequent progress interrupt; the old stored-count helper
reported success and published 1 without stepping to the terminal result. The
corrected helper observes that injected interrupt during completion and retains
the caller's original value. This establishes the missing completion check;
it does not claim the old helper received and ignored an interrupt return code.

## Fix and API contract

All three helpers now use a private validated reader. A result must contain the
expected integer value and malformed-row count, have a nonnegative value and zero
malformed rows, and fit the destination type. The statement must then finish
normally and finalize successfully before the output is assigned. On every failure,
the caller's previous output remains unchanged.

- Stored count is read as a 64-bit integer and bounded by `UINT32_MAX`.
- Egressed count counts rows with integer flag 1 and rejects flags outside integer
  0/1, including NULL, fractional and nonnumeric text values. It must fit `size_t`.
- Declared bytes require nonnegative integer metadata and return a `uint64_t`
  total. NULL, fractional and nonnumeric values are errors; integer SUM overflow
  remains an SQLite error. Empty totals remain valid zero results.

Invalid result/input types return `SQLITE_CORRUPT`; out-of-range counts return
`SQLITE_TOOBIG`. Actual preparation/execution errors retain their SQLite status.
Null output or database arguments return `SQLITE_MISUSE`. Statement ownership
remains local and every exit finalizes its statement.

Public function signatures, schemas and wire formats are unchanged. The existing
exported raw SQL definitions retain their exact text and result shape, confirmed
by the [compatibility check](validation-scalars-2026-11-01/raw-sql-compatibility.log).
The validated helpers use private immutable query arrays. An initial implementation
changed the raw query shape; compatibility review corrected that before the final
build and validation recorded here.

Each helper validates its own accounting field in one query; it does not validate
all payload/database metadata or provide one snapshot across separate helper calls.
Startup continues to use its combined validated snapshot. Callers retain normal
connection/instance serialization.

## Tests

`storage_init_regression` retains startup, lock/contention and cleanup coverage,
and now exercises the standalone helpers directly after creating real SQLite rows:

- Negative, NULL, fractional and text bytes; invalid, negative, NULL, fractional
  and text egress flags; and integer byte-SUM overflow.
- Empty results, mixed valid rows, a 3-billion-byte total and `INT64_MAX` bytes.
  These are metadata declarations, not allocations of equally large payloads.
- Completion interruption for each helper, preserving sentinel output, followed
  by successful retry after removing the fault.
- Denied SELECT preparation for each, unchanged output, cleanup and retry.
- Null database/output arguments, no active statements and preserved autocommit.
- Connection-local COUNT(*) overrides for the valid `UINT32_MAX` boundary,
  oversized `UINT32_MAX + 1`, and a negative synthetic count.

The synthetic aggregate is confined to its test connection, which is then closed.
The production code has no aggregate overrides or fault switches.

## Validation

- Full integration suite: **37/37 passed**, 9.51 seconds.
- Startup/scalar, ownership and eight storage error modes under ASan/UBSan with
  leak detection: **10/10 passed**, 3.96 seconds.
- Rebuilt final native BPLib and fresh five-node baseline: **1/1 passed**,
  42.88 seconds.
- Independent verification passed for all five runtimes: module/host/startup
  identities, output hashes, custody drain, endpoint keys and final source/binary
  fingerprints. No receipt-clock or BPLib initialization errors were found.

See the [native results](validation-scalars-2026-11-01/results.json),
[independent evidence](validation-scalars-2026-11-01/native-evidence.json) and
[verification log](validation-scalars-2026-11-01/verify-native.log).

The suite remains 37 entries because an existing regression was expanded. The
[captured test output](validation-scalars-2026-11-01/scalar-test-detail.log) records
scalar and retained startup checks. See the
[integration results](validation-scalars-2026-11-01/integration-tests.log) and
[sanitizer results](validation-scalars-2026-11-01/asan-tests.log).

## Reproduction and limits

Follow [build and test](build-and-test.md), then run:

```sh
cmake --build INTEGRATION_BUILD --target storage_init_regression -j4
ctest --test-dir INTEGRATION_BUILD -R '^storage_init_regression$' --output-on-failure
```

Test modes `scalar-bytes`, `scalar-flag`, `scalar-completion` and `scalar-wide`
isolate the original failures. No-argument execution runs scalar, startup and
contention checks. Fault injection is test-only. Physical billion-row databases,
32-bit execution, hardware power loss, Windows-native behavior and a new TSan
run are outside this validation. The native baseline checks normal integration;
focused SQLite tests inject invalid fields and query faults. Older native matrices
retain their own snapshots and are not counted as current runs. The
[evidence directory](validation-scalars-2026-11-01/README.md) preserves the production
diff, failures, build/test logs and pinned source/binary identities.
