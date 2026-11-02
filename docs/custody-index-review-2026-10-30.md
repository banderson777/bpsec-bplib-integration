# Custody index rollback and sequence publication — 2026-10-30

This follows the [file-read review](file-read-error-review-2026-10-30.md).
Review of allocation and custody state failures found that in-memory custody
index operations published counts or transmitted state before insertion succeeded.

## Fixes

`BPLib_CT_InitEntry` now increments `CurrDbSize` only after the bundle-ID index
accepts its entry. Failed duplicate insertion frees the newly allocated block.
Previously the count was incremented first and the allocation was leaked on
failure. Both creation helpers now reject counts at or above the configured
capacity before allocating; a count already above the limit cannot bypass it.

`BPLib_CT_AddToCtdb` zero-initializes reused pool storage and creates a defined
state instead of inheriting stale link pointers and state. It inserts the ID
first, then the sequence. A sequence collision rolls back the ID insertion and
frees the attempted entry; an ID collision never leaves a sequence-only orphan.
Only successful insertion into both indexes publishes the count and transmitted
state. This internal helper does not accept custody or increment
`BundleCountInCustody`; custody-acceptance accounting remains the caller's
responsibility, as documented in its header. Current production egress uses
`InitEntry` followed by `UpdateEntry`, not this convenience helper.

`BPLib_CT_UpdateEntry` now changes transmitted state only after sequence insertion
succeeds. On failure it restores the prior sequence fields and leaves the prior
state intact. An already-indexed entry is rejected by the RBT insertion API
without changing its link; restoring its sequence fields preserves its existing
search keys. A previously unindexed entry can retry after a sequence collision.

`BPLib_CT_UpdateBundle` reserves the next sequence in locals and writes its CTEB
only after indexing succeeds. Failure leaves its prior CTEB, administrative EID,
encode flag and custody state untouched for retry. The failed attempt consumes
a counter position rather than recycling it; the resulting gap is intentional.
Success-with-null lookup output now fails with not-found instead of dereferencing
null. The unfinalized-custody diagnostic now prints state and the actual sequence
number with matching variadic argument types; its old format mislabeled state
as a sequence and supplied an unused argument.

Index lookups clear their output on failure and validate null arguments. Creation,
update and removal also reject null inputs. API signatures, wire formats and SQL
schema remain unchanged. These are volatile CTDB changes, separate from the
existing synchronous SQL custody commit boundary.

## Locking and failure limits

Callers must hold `Ct.Lock` or exclude concurrent use while manipulating/searching
these indexes. The existing production lock ownership is unchanged. Sequence
fields are transiently assigned for RBT insertion, with failure restoration before
return; concurrent unlocked readers are not supported.

Allocation/duplicate rollback assumes valid red-black trees. If rollback extraction
itself fails because a tree is already corrupt, `AddToCtdb` retains and accounts
for the initialized allocation rather than freeing memory still referenced by
an index, then reports the rollback failure. This is defensive containment, not
repair or an atomicity guarantee for arbitrary preexisting tree corruption.
The direct regression does not inject structural tree corruption.

## Tests

The new `custody_db_regression` compiles real CTDB and red-black-tree API and
implementation sources. Only pool allocation/free is controlled. Newly allocated
entry bytes are poisoned to simulate reused storage. Tests cover:

- Duplicate initialization with unchanged count, original lookup and live allocation.
- Allocation failure and refusal at/above capacity before allocating.
- Two-index success, ID collision, and sequence collision with successful rollback.
- Failed sequence update preserving state/fields, subsequent successful retry,
  and rejection of an already-indexed update without corrupting its lookup.
- Missing lookup output reset, null argument handling and complete removal.
- A 64-entry tree with repeated ID/sequence collisions, checking counts/live
  allocations after each insertion and mixed-order deletion. Both roots and
  all entry/custody/allocation counts must end at zero.

The existing `custody_regression` now injects sequence-index failure at the bundle
update boundary and verifies unchanged CTEB/encode state, consumed counter gap,
successful retry and success-with-null lookup handling. Prior allocator exhaustion,
rollover and mutex ownership checks remain. The database regression uses actual
indexes rather than a stubbed successful insertion response.

The pre-fix implementation fails the duplicate initialization count/allocation
assertion. The fixed focused pair passes, and the new test participates in both
full integration and focused sanitizer runs.

## Validation

- Full integration CTest: **32/32 passed**, 9.15 seconds.
- ASan/UBSan with leak detection: **10/10 passed**, 2.59 seconds.
- Focused real-index and egress tests: **2/2 passed**.

Native validation: **5/5 passed** on the pinned final build. Independent checks
cover all 25 runtimes, module/host/startup identities, endpoint-only keys,
source/output hashes, custody/restart checkpoints and saved receipt hashes.
No receipt-clock or BPLib initialization errors were found.

| Scenario | Result | Evidence |
| --- | --- | --- |
| Original-ID duplicate replay | Passed, 116.87 s | Nine original IDs recovered; all five live hosts drained; immutable output/completion; 40 unique IVs |
| Baseline transfer | Passed, 53.77 s | Three matching segments; custody drained |
| Incomplete-receipt receiver restart | Passed, 63.17 s | Saved receipt unchanged; completion after current readiness; three matching segments |
| Dropped custody acknowledgement | Passed, 67.55 s | Drop hook fired; transfer recovered; matching outputs and custody drained |
| Storage failure | Passed, 26.78 s | No ground output or completion; source retains seven pending bundles, refusing relay retains none |

These are targeted native runs for the changed custody library, not a recount
of the historical twelve-scenario matrix.

The [evidence directory](validation-ctdb-2026-10-30/README.md) preserves exact
source/binary identities, the failed reproduction, build logs and test results.
Prior reports retain their originally recorded snapshots.

## Reproduction

Use the generated cFS headers and dependencies in [build and test](build-and-test.md):

```sh
cmake -S integration -B INTEGRATION_BUILD -DCFS_BUILD=/path/to/cfs-build
cmake --build INTEGRATION_BUILD --target custody_db_regression custody_regression -j4
ctest --test-dir INTEGRATION_BUILD -R '^custody(_db)?_regression$' --output-on-failure
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
ctest --test-dir ASAN_BUILD -R '^custody(_db)?_regression$' --output-on-failure
```

The host tests use the configured POSIX/GNU toolchain. They establish rollback,
cleanup and state publication in the tested scenarios, not exhaustive thread
schedules, hardware power-loss behavior or Windows-native execution. The native
build continues to use cFS's unavailable-Git-description fallback and pins its
actual artifacts; exact scenario commands are preserved with the evidence.

Preserved records include the [failed real-tree reproduction](validation-ctdb-2026-10-30/before-test.log),
[focused passing pair](validation-ctdb-2026-10-30/after-test.log),
[full integration results](validation-ctdb-2026-10-30/integration-tests.log),
[sanitizer results](validation-ctdb-2026-10-30/sanitizer-tests.log),
[native commands/results](validation-ctdb-2026-10-30/results.json),
[independent native evidence](validation-ctdb-2026-10-30/native-evidence.json), and
[verification log](validation-ctdb-2026-10-30/verify-native.log).
The two production patches, API/test sources and actual RBT dependencies are
identified by the recorded source hashes; built artifacts have separate hashes.

The later [custody-admission review](custody-admission-review-2026-10-30.md) fixes
resource-pressure duplicate handling, overflow-safe capacity checks and rejected
pending-custody cleanup. This index report retains its original source/binary hashes.
