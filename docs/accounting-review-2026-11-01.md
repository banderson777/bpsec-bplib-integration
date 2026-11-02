# Validate and atomically restore storage accounting — 2026-11-01

Following the [egress retry review](egress-retry-review-2026-11-01.md), this pass
checks accounting restored from existing SQLite storage. Initialization converted
unchecked SQL aggregate values to unsigned counters and assigned counters before
all recovery queries succeeded.

## Confirmed failures

A database containing `bundle_bytes=-1` initialized successfully and set
`BytesStorageInUse` to 18446744073709551615 on this 64-bit host. A row with
`egress_attempted=2` likewise initialized successfully and wrapped the pending
bundle count to that value. See the [negative-byte reproduction](validation-accounting-2026-11-01/before-negative.log)
and [invalid-flag reproduction](validation-accounting-2026-11-01/before-flag.log).
The existing schema allows these values; ordinary writes are expected to supply
valid metadata, but recovery did not reject invalid persisted accounting fields.

Two direct recovery calls also demonstrated partial publication on error. Denying
read access to `bundle_bytes` returned an authorization error after stored/pending
counters changed from sentinels 91/92 to 2/1; the byte counter remained 93. An
interrupt armed after a result row returned an error after the stored count
changed to 2. The [preparation failure](validation-accounting-2026-11-01/before-prepare.log)
and [execution failure](validation-accounting-2026-11-01/before-step.log) preserve
those observations. These calls already reported errors; their newly fixed
behavior is retaining the original counters together.

The tests establish invalid startup accounting and partial recovery state. They
do not demonstrate a remote exploit, accepted corrupt payload, or custody bypass.

## Fix and compatibility

`BPLib_SQL_InitTable` now reads all accounting inputs in one SELECT: total rows,
rows marked egressed, total declared bytes and a count of malformed accounting
rows. This observes one accounting snapshot and avoids mixing separate reads.
The query counts egressed rows rather than summing an unchecked flag.

Every persisted `bundle_bytes` must be an integer at least zero. Every
`egress_attempted` must be integer 0 or 1. NULL, fractional and nonnumeric text
values fail recovery. Empty storage is valid and restores zero counters. Integer
SUM overflow also fails without publishing counters. Aggregate values must be
nonnegative integers, egressed count cannot exceed stored count, stored count must
fit `uint32_t`, and declared bytes must fit `size_t`.

Only normal query completion and successful finalization publish the stored,
pending and byte counters together. Any error retains their previous values.
The outer `BPLib_SQL_Init` continues to close the failed database handle and
return initialization failure. Existing startup/instance serialization remains
required. Schema creation still follows its existing behavior;
this change does not make the entire schema-creation sequence transactional.

Valid accounting metadata retains its existing meaning. Databases with malformed
fields now fail initialization instead of being silently coerced. This is validation,
not repair: stored rows are not rewritten. Public signatures, SQL schema, wire
formats and portable recovery records are unchanged. Startup no longer uses the
three separate scalar query helpers; those standalone helpers retain their prior
behavior and require a separate review for direct callers.

## Tests

`storage_init_regression` retains its transient-monitor-lock, bounded-contention,
failed-handle cleanup and interrupted scalar-helper controls. New accounting cases
use the real SQLite schema and initialization code:

- Ten invalid database cases: negative, NULL, fractional and text byte sizes;
  flags 2, -1, NULL, fractional and text; and overflowing integer byte SUM.
- Direct recovery failures during byte-column preparation and after a result row.
  All counters must retain sentinels, statements must close, autocommit must remain
  intact, and retry after clearing the fault must restore 2 stored/1 pending/30 bytes.
- Valid empty storage, mixed rows including zero declared bytes, and a 3-billion-
  byte declaration on this 64-bit host. Successful recovery must publish exact values.

Invalid persisted metadata must make the public initializer fail, close the handle
and retain prior counters. The fixture's metadata-only rows test accounting; payload
integrity remains the responsibility of the existing loading/recovery checks.

## Validation

- Full integration suite: **37/37 passed**, 9.38 seconds.
- Startup, ownership and eight storage error modes under ASan/UBSan with leak
  detection: **10/10 passed**, 2.52 seconds.
- Fresh native baseline and retained-custody restart: **2/2 passed**.
- Independent verification passed for all ten runtimes: module/host/startup
  identities, output hashes, custody/restart checkpoints, immutable replay,
  endpoint-only keys and pinned source/binary identities. No receipt-clock or
  BPLib initialization errors were found.

| Native scenario | Result | Evidence |
| --- | --- | --- |
| Baseline | Passed, 42.11 s | Three matching segments; custody drained |
| Original-ID replay with retained-custody restart | Passed, 125.98 s | Nine original IDs recovered; all five live hosts drained; output unchanged; 47 unique IVs |

See the [native results](validation-accounting-2026-11-01/results.json),
[independent evidence](validation-accounting-2026-11-01/native-evidence.json) and
[verification log](validation-accounting-2026-11-01/verify-native.log).

The suite remains 37 entries because an existing regression was expanded. The
[captured startup-test output](validation-accounting-2026-11-01/accounting-test-detail.log)
records the invalid and valid cases. See the
[integration results](validation-accounting-2026-11-01/integration-tests.log) and
[sanitizer results](validation-accounting-2026-11-01/asan-tests.log).

## Reproduction and limits

Follow [build and test](build-and-test.md), then run:

```sh
cmake --build INTEGRATION_BUILD --target storage_init_regression -j4
ctest --test-dir INTEGRATION_BUILD -R '^storage_init_regression$' --output-on-failure
```

Optional test arguments `negative`, `flag`, `prepare` and `step` isolate the four
original failures. No-argument execution runs the accounting and existing lock
checks. These are test modes, not product fault switches.

Range guards are present, but no database with more than `UINT32_MAX` records or
32-bit build is claimed. Recovery validates accounting fields, not arbitrary
payload/schema authenticity or all historical database corruption. Hardware power
loss, Windows-native behavior and a new TSan execution remain untested. Older
matrices retain their snapshots and are not counted as current runs. The
[evidence directory](validation-accounting-2026-11-01/README.md) preserves failures,
the production diff, test/build logs and pinned source/binary identities.

Later validation: the [standalone scalar review](scalar-accounting-review-2026-11-01.md) closes the direct-helper validation gap noted above. Earlier results retain their original pinned snapshot.
