# Preserve consumed batches until egress marking succeeds — 2026-11-01

Following the [selection rollback review](selection-review-2026-11-01.md), this
pass checks the storage caller after all IDs in a load batch have been consumed.
The SQL marking helper already rolls back failed updates. Its caller nevertheless
cleared the consumed batch on error, discarding the state needed to retry marking.

## Confirmed failures

Four isolated runs of the real `BPLib_STOR_EgressForID` for remote egress inject:

- Denied UPDATE preparation.
- Failure on the second marking update.
- Deferred-constraint failure at COMMIT.
- Failure on the second custodial retransmission-timer update.

Each returned `BPLIB_STOR_SQL_MARK_EGRESSED_ERR` and zero newly egressed bundles.
SQLite rolled back and the row flags stayed unchanged, but the previously consumed
batch changed from size/reader 2/2 to 0/0. See the
[recorded exits](validation-egress-retry-2026-11-01/before-results.json) and
[preparation](validation-egress-retry-2026-11-01/before-retry_prepare.log),
[update](validation-egress-retry-2026-11-01/before-retry_step.log),
[COMMIT](validation-egress-retry-2026-11-01/before-retry_commit.log) and
[timer](validation-egress-retry-2026-11-01/before-retry_timer.log) logs.

With an empty batch, the next eligible storage poll performs a new selection
rather than retrying the marking operation. Still-eligible bundles could therefore
be reloaded for delivery. The reproduction establishes loss of consumed-batch
retry state; it does not observe a duplicate native transport delivery or bundle
loss, and does not claim that SQL rollback was missing.

## Fix

`BPLib_STOR_EgressForID` clears a consumed load batch only after
`BPLib_SQL_MarkBatchEgressed` succeeds. On failure, IDs, size and read index remain
available. The next eligible poll sees the same consumed batch and retries the
database marking path, rather than selecting or loading bundles again. Normal
ingress-idle and pending-bundle scheduling conditions still apply.

The existing error is returned, the newly egressed count remains zero, and the
storage mutex is released. The SQL helper retains its existing transaction and
post-COMMIT accounting behavior. Successful marking still clears the batch.
No public signature, SQL schema, wire format or lock ordering changed. This fix
adds no batch copy, allocation or new synchronization primitive.

## Tests

`storage_error_regression mark_egress` retains its SQL-level checks and now runs
seven caller-level cases: preparation, second-update and COMMIT failures for both
local and remote noncustodial egress, plus a remote custodial timer-update failure.
Each starts with a consumed batch representing bundles already delivered to the
queue. The fixture links the actual storage caller, SQL marking and batch code
against SQLite. Queue and memory boundaries are instrumented stubs.

On error, tests require retained IDs/size/reader, zero newly egressed bundles,
rolled-back row flags/timers, unchanged storage accounting and last-active time,
no active SQL statements, autocommit restored, and an unlocked storage mutex.
After removing the fault, the next call must mark successfully and clear the
batch, with the correct counters/timers. Both calls must avoid bundle allocation
and queue inspection/push. A deferred COMMIT fault must also leave no child rows.

An initial test fixture used the wrong public function name. The linker caught
that mistake; it was corrected to `BPLib_STOR_EgressForID` before the recorded
reproductions. The [initial build diagnostic](validation-egress-retry-2026-11-01/fixture-name-build.log)
is preserved separately from passing builds; this was not a product API change.

## Validation

- Full integration suite: **37/37 passed**, 10.40 seconds.
- Eight storage error modes under ASan/UBSan with leak detection: **8/8 passed**,
  0.54 seconds; the marking mode contains all seven new caller cases.
- Rebuilt native BPLib and fresh five-node baseline: **1/1 passed**, 42.73 seconds.
- Independent native verification checks all five runtime identities, matching
  outputs, custody drain, endpoint keys and source/binary fingerprints.

The [captured marking-test output](validation-egress-retry-2026-11-01/egress-retry-test-detail.log)
records the seven retained 2/2 batches. See the
[integration results](validation-egress-retry-2026-11-01/integration-tests.log),
[sanitizer results](validation-egress-retry-2026-11-01/asan-tests.log),
[native results](validation-egress-retry-2026-11-01/results.json),
[independent evidence](validation-egress-retry-2026-11-01/native-evidence.json) and
[verification log](validation-egress-retry-2026-11-01/verify-native.log).
The suite stays at 37 entries because an existing regression mode was expanded.

## Reproduction and limits

Use the dependencies in [build and test](build-and-test.md), then run:

```sh
cmake --build INTEGRATION_BUILD --target storage_error_regression -j4
ctest --test-dir INTEGRATION_BUILD -R '^storage_error_mark_egress_regression$' --output-on-failure
```

`storage_error_regression egress_retry` runs only the seven caller cases.
`retry_prepare`, `retry_step`, `retry_commit` and `retry_timer` isolate fault kinds.
The regular `mark_egress` mode includes SQL-level and caller-level checks. These
are test modes, not native product fault settings.

The native baseline validates normal integration on the corrected module; it does
not inject the new SQL faults. Focused tests seed a consumed batch rather than
first sending live native bundles. No exactly-once delivery, hardware power-loss,
Windows-native or new TSan result is claimed. Historical matrices retain their
own snapshots and are not counted as executions for this fix. The
[evidence directory](validation-egress-retry-2026-11-01/README.md) preserves the
production diff, failures, passing logs and pinned build/source identities.

Later validation: the [startup accounting review](accounting-review-2026-11-01.md) validates persisted accounting and publishes recovered counters together. Earlier results retain their original snapshot.
