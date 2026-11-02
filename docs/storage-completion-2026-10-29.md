# Storage recovery and durability completion — 2026-10-29

This report supersedes the remaining-work statements in the earlier
[storage review](storage-review-2026-10-29.md). Earlier test counts and hashes
remain historical snapshots. This pass implements administrative-record recovery,
persistent origin metadata, durable custody updates, additional SQL checks, and a
fresh full native security/recovery matrix.

## Portable recovery format

New writes use an additive `bundle_recovery` table keyed by `bundle_data.id`:

| Field | Meaning |
| --- | --- |
| `format` | Explicit version 2; unsupported versions fail closed |
| `ingress_id` | Original unsigned 32-bit ingress contact/channel identifier |
| `local_bundle` | Original local-origin boolean |
| `wire_bytes` | Exact length of the complete encoded bundle |
| `receipt_time`, `receipt_era` | Original monotonic receipt/creation time and boot era needed by age calculations |

The row, parent metadata, and ordered `bundle_blobs` chunks commit in the same
transaction. Foreign-key cascade removes recovery metadata with its bundle.
Version 2 blobs contain a complete BP encoding, with no C struct layout, padding,
address, or serialized pointer. The existing parent table keeps bundle ID,
custody status, retransmit trigger, routing/expiration indexes, and storage byte
accounting. `bundle_bytes` accounts for encoded length plus the writer's decoded
block overhead; it is an accounting value, not the portable encoding length.
Recovery computes its in-memory `TotalBytes` using its own block overhead.

On load, scalar types and ranges are validated before allocating a bounded wire
buffer. Chunk lengths must sum exactly to `wire_bytes`; missing, empty, extra,
non-blob, or oversized-total data is rejected. Total wire size cannot exceed the
configured maximum bundle length. The actual CBOR decoder and CRC checks rebuild
the primary, extension, and administrative payload objects. Bundle ID and origin
metadata and the receipt clock are restored separately because they are not encoded protocol fields.
Egress ID starts invalid until the output route selects a real contact. Decode
errors free all owned pool blocks, including a partially parsed administrative
record, and return NULL.

Storage explicitly encodes the bundle before persisting it. A locally created
bundle may own only an ADU blob and dirty decoded headers; its blob is not yet a
full encoding. Storage encodes a snapshot of those headers, supplying valid local
previous-node/custody-source EIDs when the local block factory has left them
unset until first egress. Actual custody sequence assignment and forwarding
updates remain in the normal egress path. Insert accounting is checked again
against the storage limit using the encoded size, and failed writes restore the
original byte count on the input object. Snapshot encoding avoids changing the
input's decoded CRC fields while preparing the persistent record.

Both store and load use temporary heap buffers bounded by the configured maximum
bundle length, in addition to the existing memory pool. Synchronous encoding and
reconstruction add CPU and memory cost; the native matrix validates the demo
workload, not every flight mission's throughput or memory budget.

## Legacy compatibility

A parent row without a recovery row is version 1. Existing databases gain the new
table without rewriting or deleting stored bundles. The legacy loader retains
its exact native metadata-size and byte-accounting checks. When legacy metadata
contains an administrative pointer, the pointer is never dereferenced: the
loader clears it and reconstructs the object from the owned encoded bytes.
Malformed wire data fails without returning an object. Legacy administrative
records whose chunks never contained a complete encoding cannot recover a
missing pointed-to object; those remain preserved and fail closed. A real legacy fixture
with a deliberately invalid saved address exercises this path.

The old format did not store original ingress or local-origin information. That
information cannot be recovered retroactively: legacy records use invalid
contact sentinels and `LocalBundle=false`. New records preserve it exactly.
Legacy records also remain dependent on their original native metadata layout;
only version 2 is independent of that layout. Old program versions do not
understand version 2; do not downgrade a runtime while it has version 2 records.
There is no destructive bulk migration or automatic database reset.

## Custody commit boundary

`BPLib_STOR_AddToCustodialUpdateBatch` keeps its compatibility name and status
return, but now commits each accepted operation synchronously under the storage
mutex. Success means SQLite committed with WAL and `synchronous=FULL`. Failure
means the caller still owns the operation and must retain/retry custody. There
is no interval in which a successfully accepted operation exists only in a
volatile queue. This replaces the earlier in-memory full-batch/retry design and
trades batching throughput for an explicit durable ownership boundary.

The lower-level atomic batch API remains available. The maintenance/explicit
flush paths still preserve failed batches if a caller has populated that legacy
batch explicitly; new CCS enqueue calls do not populate it. Custody signal
processing changes CT state only after durable success, propagates errors, and
re-looks up entries after releasing/reacquiring the CT mutex. Storage and CT
mutexes are not held simultaneously in that path.

The abrupt-exit regression uses separate processes and a file-backed WAL database.
A SQLite trigger terminates a child inside an update before commit: reopening
must retain the unegressed bundle and its count. A second child exits immediately
after successful enqueue, before any in-memory custody transition: reopening
must show the committed deletion mark and zero pending count. The existing
native recovery code reconstructs custody from committed bundle rows before
publishing readiness. These tests establish process-crash behavior; they do not
simulate faulty storage hardware or prove arbitrary power-loss behavior.

## Additional fixes

- SQL EID lookup checks the instance before dereferencing it and clears finalized
  statement globals. Retransmit configuration validates null pointers, counts,
  and timer range before use and reports zero updated rows on errors.
- EID binding loops now test both maximum node and maximum service for the
  terminating `dtn:none` pattern. The old duplicated-node comparison stopped
  early for a zero-node/nonzero-service pattern, misaligning SQL parameters.
- WHERE-clause construction initializes its output, bounds pattern counts, and
  rejects reversed ranges and unsigned values that cannot fit SQLite integers.
- Bundle encoding reserves a byte for its final indefinite-array marker before
  handing capacity to subencoders, preventing an end-marker write past the
  buffer. Payload encoding checks zero capacity before writing, emits a
  five-element array when CRC is absent, and permits an exact payload fit.

## Regression coverage

`integration/storage_recovery_regression.c` compiles the actual SQL store/load,
custody update, memory wrapper, CBOR encoder/decoder, CRC, and EID sources. The
pool backend is replaced with counted, poisoned allocations and deterministic
allocation failures; event/time/configuration/accounting boundaries are controlled.
It covers a file-backed administrative-record round trip across close/reopen,
restored origin/bundle ID, failure at bundle/blob/admin allocations, malformed
admin data after allocation, inconsistent lengths, legacy saved-pointer recovery,
preserved receipt clocks and boot eras, abrupt exits on either side of commit, local ADUs spanning multiple chunks,
uninitialized local extension EIDs, and encoder capacity guards around exact fit.

`storage_error_regression.c` adds SQL lookup/configuration argument and sentinel
coverage. Its compatibility-named `custody_queue` mode now verifies synchronous
write failure and retry, retained CT ownership on failure, transfer after commit,
and four concurrent producers committing deletion marks for 548 distinct bundles
under the storage mutex. Every row must change and the pending counter must
reach zero, so dropped requests or incorrect accounting cannot hide behind
idempotent updates.
`storage_commit_regression.c` keeps its focused encoder boundary stub and tests
real transaction failures and post-commit custody/forwarding behavior.

## Validation and evidence

The final integration suite passed **26/26** tests in 5.13 seconds. The focused
storage sanitizer suite passed **11/11** modes under ASan/UBSan with leak detection
in 1.01 seconds. The final native matrix passed **11/11** scenarios; details follow below.
The documentation checker covers 31 inventory entries, 32 public Python
classes/callables, and linked guide paths. Commands:

```sh
cmake -S integration -B /tmp/bpsec-storage-followup-20261029 \
  -DCFS_BUILD=/tmp/bpsec-deployment-20261029/build/cfs
cmake --build /tmp/bpsec-storage-followup-20261029 -j4
ctest --test-dir /tmp/bpsec-storage-followup-20261029 --output-on-failure --timeout 60
cmake --build /tmp/bpsec-storage-followup-20261029-asan \
  --target storage_error_regression storage_recovery_regression storage_commit_regression -j4
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir /tmp/bpsec-storage-followup-20261029-asan \
  -R '^storage_(error_|recovery_|durability_|constructed_|commit_)' \
  --output-on-failure --timeout 30
python3 /tmp/bpsec-complete-20261029/run_matrix.py
python3 /tmp/bpsec-complete-20261029/collect_evidence.py
python3 docs/check_documentation.py
```

The sanitizer directory retains the previously configured address/undefined
sanitizer flags, leak detection, and non-PIE executables. Native evidence is
isolated under `/tmp/bpsec-complete-20261029`; older installations remain intact.
The first two native attempts found ADU-only persistence and uninitialized local
extension EIDs during source restart. Their `*-final` and `*-portable-final`
runtimes/logs and `results-before-*.json`, source hashes, and binary fingerprints
remain alongside the corrected `*-complete-final` run. Those failures are not
counted as passes for the final snapshot.

A third full attempt passed all live scenario assertions, but the independent
post-run collector found three pending rows at one restarted relay. Investigation
also found `BPLIB_TIME_UNDEF_DELTA_ERROR`: decoding protocol bytes alone does not
restore the primary block's local receipt clock. Version 2 now persists its
monotonic time and boot era, and legacy administrative reconstruction preserves
them too. The outage controller stops upstream processes before downstream ones
to avoid deliberately creating another outage during teardown. The final
`*-clock-final` runs replace that attempt; `*-complete-final` logs/evidence and
`*-before-receipt-clock.json` manifests remain as failure history. Final evidence
records both pending rows after process exit and the live six-second drain.
These are different checkpoints: termination can finalize in-flight duplicate
work after the live observation. Such rows remain durably pending for the next
recovery; they are not erased to make a shutdown database appear empty. The
outage controller's live drain assertion is the success criterion. Persisted
post-shutdown counts are reported separately, including retained source rows
because the source is deliberately stopped during these scenarios.

For recovery, preserve the runtime's time-correlation/boot-era history and custody
sequence reservation state along with its database and endpoint key/IV state.
Persisting a receipt boot era does not manufacture a missing clock correlation;
the normal time provider still determines whether a cross-boot delta is valid.
The native tests retain these files across each tested restart.

The validation is on native Linux/WSL with the checked-in mission configuration.
It is not a Windows-native build, hardware power-loss test, cross-architecture
legacy-struct migration, exhaustive thread-schedule test, or mission throughput
qualification. Existing dependency compiler warnings (unused parameters and CRC
callback casts) remain outside this storage-format change.

## Final native results

All cases below use the receipt-clock build and `*-clock-final` runtimes.

| Scenario | Seconds | Result |
| --- | ---: | --- |
| outage-2-3 | 73.51 | One matching segment; live nodes drained; three duplicate records remain pending at relay 2 after shutdown |
| storage-failure | 24.51 | No output/completion; source retains seven records; relay 2 accepts no custody |
| source-iv-restart | 41.97 | Three matching segments; custody drained; 29 unique 96-bit source IVs across restart |
| baseline | 39.40 | Three matching segments; all nodes drained |
| outage-3-4 | 88.39 | One matching segment; live nodes drained after relay 3 restarts |
| outage-4-5 | 85.43 | One matching segment; live nodes drained after relay 4 restarts |
| lost-ccs | 63.75 | Three matching segments; custody drains despite one dropped CCS |
| node5-restart | 63.18 | Three matching segments; completion follows the second readiness marker; custody drained |
| wrong-key | 19.55 | No output/completion; ground rejects; relay 4 retains seven records |
| tampered-ciphertext | 20.76 | No segment 0 or completion; valid segments 1 and 2 match; relay 4 retains seven records |
| missing-bcb | 17.98 | No output/completion; ground rejects; relay 4 retains seven records |

The independent verifier checked both BPLib and BPNode module hashes in all
**55 node runtimes**, output lengths and SHA-256 hashes, keys only at endpoints
1 and 5, receipt-clock error logs, restart readiness/completion ordering, and
source IV uniqueness. No `BPLIB_TIME_UNDEF_DELTA_ERROR` occurred in these final
scenario logs. During outage cases, the deliberately stopped source can retain
rows. Relay 2 also retains three already-delivered duplicate IDs after teardown
in the 2–3 case; the live controller first observed zero custody for six seconds.
Those post-stop rows are explicitly retained in the evidence, not counted as a
zero post-stop snapshot and not deleted. Recovery is responsible for replaying
such interrupted duplicate work on the next run.

Durable records are checked into the [validation record directory](validation-storage-completion-2026-10-29/README.md):
[native evidence](validation-storage-completion-2026-10-29/native-evidence.json),
[source hashes](validation-storage-completion-2026-10-29/source-hashes.json),
[binary identities](validation-storage-completion-2026-10-29/binary-fingerprints.json),
[integration log](validation-storage-completion-2026-10-29/integration-tests.log), and
[sanitizer log](validation-storage-completion-2026-10-29/sanitizer-tests.log).
Raw runtime/log artifacts and all failed-attempt history remain in `/tmp`.
The final documentation check passes; no native test hosts remain running.

The later [CRC review](crc-review-2026-10-29.md) fixes the callback casts listed
above. Its validation uses a newer binary; this report retains its earlier
26-test/11-scenario snapshot.
