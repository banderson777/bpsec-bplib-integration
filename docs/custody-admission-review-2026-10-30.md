# Preserve custody during resource pressure — 2026-10-30

This follows the [custody index review](custody-index-review-2026-10-30.md).
Admission review found that resource refusal could precede duplicate detection
and remove the original bundle's custody index. Capacity additions could also
wrap, and storage capacity refusal failed to release a pending custody entry.

## Fixes

`BPLib_CT_ProcessNewBundle` now resolves an existing bundle ID before checking
resources for a new entry. A finalized duplicate is acknowledged and discarded
without changing the original entry, custody count or allocation. A duplicate
whose original remains `Initialized` is discarded without acknowledging it or
removing that pending original. Both rules apply even when storage or CTDB
counters are full. A new entry still observes CTDB capacity and policy.

Previously, the full-resource branch signalled refusal first; refusal cleanup
looked up the same bundle ID and could remove the already-owned original.
Existing duplicates do not need another storage allocation and must not take
that path. This change preserves the prior duplicate return code and the rule
that new acceptance follows durable commit, not preliminary admission.

Custody and storage admission now use subtraction after checking current usage,
so `used + incoming` cannot wrap to a small value and bypass refusal. Exact-limit
admission remains refused, matching the prior `>=` policy. Remaining-byte
diagnostics clamp to zero above the limit and use the correct `size_t` format.

Storage admission also reserves the declared byte sizes of queued, uncommitted
insert-batch members under the storage mutex. A later bundle cannot spend the
same remaining budget again. Refusing the later bundle preserves earlier batch
members so they can still commit.

If `BPLib_STOR_StoreBundle` rejects a custodial bundle for capacity, it now signals
refusal before freeing it. This releases the CTDB entry created during preliminary
admission, allowing a later retransmission to retry. Signalling and freeing occur
after unlocking storage, avoiding a nested storage-to-custody lock acquisition.
Noncustodial rejection still frees the bundle without a custody signal.

## Locking and budget contract

The early custody-stage byte check is advisory. It holds `Ct.Lock` and uses
`pthread_mutex_trylock` to read a consistent storage usage snapshot. It must not
block on storage, whose cleanup can need custody locking. If storage is busy,
the usage portion is deferred to storage admission; a bundle whose own declared
size reaches the total limit is still refused. Unexpected mutex errors fail
closed without creating a new custody entry.

`BPLib_STOR_StoreBundle` performs its definitive admission check under the storage
mutex, including pending batch sizes. Preliminary CTDB allocation is not an
acknowledgement: actual custody acceptance still follows the existing synchronous
SQL commit boundary. Library contexts require normal mutex initialization.

The configured budget accounts for declared `Meta.TotalBytes` and recorded
committed usage. It is not a physical disk quota and does not account for SQLite
indexes, WAL or filesystem overhead. It relies on the caller's valid size
metadata; this pass does not introduce a serialized-size reservation system or
change SQL transaction formats. No wire-format or schema migration is added.

## Regression

The new `custody_admission_regression` compiles the real admission/signalling,
CTDB and red-black-tree sources. Pool allocation, ID hashing, policy and CCS
submission are controlled boundaries. It verifies:

- A finalized duplicate retains its original entry, allocation, state and count
  when storage, CTDB, both or neither are full.
- An uncommitted original remains indexed without an acknowledgement under the
  same pressure combinations.
- New admission rejects wrapping inputs near `SIZE_MAX`, usage at/above the
  configured limit, and exact-fit input, while accepting a valid below-limit
  candidate as initialized with no acknowledgement before commit.
- A busy storage mutex does not deadlock preliminary admission or acknowledge
  the resulting pending entry.

`storage_commit_regression` now checks the same byte boundaries at the storage
stage. Refusal must happen outside the storage mutex, free once, leave no queued
new record or SQL row, and issue the proper custodial refusal. A pending first
bundle survives rejection of a second exact-budget candidate and commits through
real SQLite with a post-commit acknowledgement. Noncustodial refusal emits no
custody signal. Prior real insert/COMMIT failure and rollback tests remain.

The original code fails both the full-resource duplicate reproduction and the
wrapped storage-admission reproduction. The focused fixed pair passes.

## Validation

- Integration CTest: **33/33 passed**, 7.88 seconds.
- ASan/UBSan with leak detection: **12/12 passed**, 2.57 seconds.
- Focused admission/storage pair: **2/2 passed**.

Native validation: **5/5 passed** on the final pinned build. Independent checks
cover all 25 runtimes, module/host/startup identities, endpoint-only keys,
source/output hashes, custody/restart checkpoints and saved receipt hashes.
No receipt-clock or BPLib initialization errors were found.

| Scenario | Result | Evidence |
| --- | --- | --- |
| Original-ID duplicate replay | Passed, 114.12 s | Nine original IDs recovered; all five live hosts drained; immutable output/completion; 38 unique IVs |
| Baseline transfer | Passed, 40.83 s | Three matching segments; custody drained |
| Incomplete-receipt receiver restart | Passed, 74.64 s | Saved receipt unchanged; completion after current readiness; three matching segments |
| Dropped custody acknowledgement | Passed, 62.47 s | Drop hook fired; transfer recovered; matching outputs and custody drained |
| Storage failure | Passed, 25.67 s | No ground output/completion; source retains seven pending bundles and the refusing relay retains none |

Full-capacity counters and pending-batch boundaries are controlled in the focused
tests; the native fault scenario injects storage failure rather than filling an
8 GB runtime. These five scenarios do not recount older matrices as new runs.

The [evidence directory](validation-admission-2026-10-30/README.md) preserves
reproduction failures, build/test logs and exact source/binary fingerprints.
Earlier counts retain their original snapshots.

## Reproduction and limits

Use the headers/dependencies in [build and test](build-and-test.md):

```sh
cmake -S integration -B INTEGRATION_BUILD -DCFS_BUILD=/path/to/cfs-build
cmake --build INTEGRATION_BUILD --target custody_admission_regression storage_commit_regression -j4
ctest --test-dir INTEGRATION_BUILD -R '^(custody_admission_regression|storage_commit_regression)$' --output-on-failure
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
ctest --test-dir ASAN_BUILD -R '^(custody_admission_regression|storage_commit_regression)$' --output-on-failure
```

The targeted tests use the existing POSIX host toolchain. Mutex contention is
controlled in the focused test; it is not exhaustive schedule exploration or a
ThreadSanitizer run. Corrupt existing tree structure, coordinated local-storage
tampering, hardware power loss and Windows-native behavior remain outside this
validation.

Preserved records include the [duplicate-pressure failure](validation-admission-2026-10-30/before-test.log),
[wrapped-storage failure](validation-admission-2026-10-30/storage-before-test.log),
[full integration results](validation-admission-2026-10-30/integration-tests.log),
[sanitizer results](validation-admission-2026-10-30/sanitizer-tests.log),
[native commands/results](validation-admission-2026-10-30/results.json),
[independent evidence](validation-admission-2026-10-30/native-evidence.json), and
[verification log](validation-admission-2026-10-30/verify-native.log).
The two production patches and source/binary fingerprints are preserved alongside them.

The subsequent [parallel-storage review](storage-parallel-review-2026-10-30.md)
fixes prepared-statement ownership between instances. This admission report
retains its originally recorded source/binary identities.
