# Custody-signal sender and duplicate recovery — 2026-10-30

This follows the [receiver validation review](ccs-review-2026-10-30.md).
It addresses outgoing span compatibility, shutdown-retained duplicate recovery,
and a complete native matrix on one final build. Validation results are recorded
in the accompanying [evidence directory](validation-sender-2026-10-30/README.md).

## Sender fixes

Outgoing compressed custody signals now obey the receiver's
`BPLIB_CT_MAX_CCS_SEQUENCE_SPAN` per-collection limit. Included runs and excluded
gaps both count. The default is 10,000 positions; builds reject a zero limit or
one exceeding `UINT32_MAX`. Peers must configure compatible limits: no limit
negotiation was added, and a sender's larger local limit cannot establish what
a remote peer accepts.

Insertion normalizes a bounded set of inclusive intervals. It merges adjacency,
reordered arrivals, backfill, and duplicates, then emits alternating positive
included and excluded lengths. It checks inclusive endpoints and size changes
before publishing the result. A singleton at `UINT64_MAX` is valid; wrapping
from that value to zero cannot join the same collection.

Before an insertion would exceed the span or range-array capacity, the sender
flushes the previous signal and places the triggering acknowledgement in a new
signal. The previous size/range flushing paths could lose that acknowledgement.
Contact/peer/sequence identity changes also flush the old signal. Reusing a full
open-signal table now initializes the new sequence identifier consistently,
including slots holding only refusals. Locally generated dispositions remain
acceptance and refusal; unsupported dispositions fail before indexing storage.

The last-sequence field is explicitly 64 bits. A transmitting contact reserves
a fresh sequence identifier before its counter would wrap, starting the new
stream at zero. Identifier allocation skips reserved value 100 rather than
recycling old identifiers at 100. Exhaustion and callback failure retain the
contact's old state and leave the bundle unmodified for retry. The default
allocator remains process-local; missions requiring uniqueness across restart
must install a durable allocator, as the native demo already does. Allocation
and contact updates remain under the existing custody mutex.

These changes preserve the existing ARP submission contract. They do not make
CCS queue submission transactional or guarantee delivery when its downstream
queue fails; missing acknowledgements still require custody retransmission.
No wire-format or SQL schema migration is introduced.

## Duplicate replay fix and test

The new native scenario holds upstream CCS traffic at node 2 until a release
file exists. With source ADU feeding gated, the source retains its nine original
stored bundle IDs after ground delivery. The controller restarts the source
without re-feeding input, takes node 3 offline, and waits for node 2 to retain
exactly those IDs. It stops node 2, verifies their durable retention, releases
CCS, and restarts the receiver, relay, and source. A current-launch readiness
marker must report recovery of all nine records.

The first run found that repeated authenticated data chunks reconstructed and
renamed an already published ground segment. Bytes matched, but the segment's
modification time changed. Ground delivery now validates every retained chunk
against an existing output, checks its exact length, and accepts a byte-for-byte
match without replacing or rewriting the output. Conflicting or unreadable
output fails closed. Missing output is still rebuilt from synced receipts.
Completion already compared and reused its existing durable record.

The test requires a stable six-second zero-custody checkpoint on all five live
hosts, unchanged ground/completion bytes and modification times, exactly one
completion publication, unchanged source key, and unique 96-bit source IVs
across restarts. Post-stop SQL counts are recorded separately: shutdown can
retain newly committed in-flight duplicates after a live zero checkpoint.
No SQL rows are deleted by the controller to obtain a passing result.
The hold fault is opt-in through `BPNODE_DEMO_HOLD_CCS_UNTIL` in demo mode.

## Receiver restart checkpoint correction

The first full matrix's node 5 restart controller returned success, but the
independent verifier found completion before the second readiness marker.
Receipt polling alone had raced the fast transfer and restarted an already
completed receiver. This run is retained as a rejected checkpoint, not counted
as evidence of incomplete receipt recovery.

The controller now enables the existing source file gate and feeds one record
per interval. After the first durable source row it closes that gate, waits for
a receipt, stops node 5, and requires a nonempty, incomplete receipt set with no
completion. It saves receipt hashes before restart. Only after the new receiver
is ready does it release the remaining source input. Final completion must occur
after that readiness marker, and the independent verifier rechecks the retained
receipt hashes. The scenario is rerun with unchanged host/modules; other matrix
scenarios use their original controller snapshot. Both controller hashes and
the rejected run are recorded so a passing controller exit cannot hide a missed
checkpoint. This change is confined to `--restart-node5-after-receipt`.

## Regression coverage

`storage_ccs_sender_regression` uses the actual CCS builder, encoder, and decoder.
It validates every captured outgoing signal against the span policy and checks
reordering, gap merging, duplicate arrivals, both dispositions, exact and excess
span, wide backfill, maximum 64-bit sequence values, identity transitions,
range/size flushing, full-table eviction, and unchanged state on invalid input.
The same test runs in an eight-position override build.

`custody_regression` executes the actual assignment and bundle-update functions
with controlled database entries and allocator failure. It checks counter
exhaustion, durable callback success/failure, reserved-ID skipping, allocator
exhaustion, and existing lock ownership behavior. The upstream CT unit fixture's nominal
reserved-ID expectation is updated from recycling identifier 1 to advancing to
101. That upstream coverage runner is not enabled in this mission build; the
passing integration regression executes the actual allocator directly instead.
The reported test counts refer to the integration suite, not all upstream
component coverage targets.

`demo_delivery_regression` includes the real demo implementation and executes
its receipt/publication path in a private temporary directory. It checks incomplete
assembly, complete output, byte-for-byte replay without inode/mtime changes,
idempotent completion, conflicting duplicate receipts, changed/truncated/extended
published output rejection without replacement, and reconstruction of missing
output. Unused framework functions are discarded at link time; delivery and its
filesystem operations are not stubbed. This check also runs under ASan/UBSan.

## Validation

- Final integration suite: **31/31 passed** (5.65 seconds).
- ASan/UBSan with leak detection: **9/9 passed** (1.51 seconds).
- Retained-duplicate native restart: **passed** (160.26 seconds), nine original
  IDs retained/recovered, all five live custody counts stable at zero, immutable
  output/completion, and 44 unique source IVs.
- Eight-position policy override: **2/2 passed** (sender and receiver bounds).

Final native validation: **12/12 scenarios passed** on the same pinned host,
BPLib, BPNode and security binaries. Independent verification covers all 60
node runtimes, both module identities, host/startup hashes, endpoint-only keys,
output hashes, custody checkpoints and restart ordering. No receipt-clock or
BPLib initialization errors were found in the final runs.

| Scenario | Result | Evidence |
| --- | --- | --- |
| Original duplicate IDs across relay restart | Passed | 9 original IDs; immutable output; 44 unique IVs |
| 2→3 outage / relay restart | Passed | 1 matching segments |
| Storage write failure | Passed | No completion; upstream retains custody |
| Source restart / IV audit | Passed | 3 matching segments; 15 unique IVs |
| Baseline transfer | Passed | 3 matching segments |
| 3→4 outage / relay restart | Passed | 1 matching segments |
| 4→5 outage / relay restart | Passed | 1 matching segments |
| One dropped CCS | Passed | 3 matching segments |
| Incomplete ground receipts / restart | Passed | 1 retained receipt; completion after new readiness |
| Wrong endpoint key | Passed | No completion; upstream retains custody |
| Altered ciphertext with valid BP CRC | Passed | No completion; upstream retains custody |
| Missing BCB | Passed | No completion; upstream retains custody |

The ordinary outage cases preserve source rows after intentionally stopping the
source. After teardown the 2→3 run retained three additional relay duplicates,
and the 4→5 run retained one at node 2; their live checkpoints were zero on all
running relays. The dedicated duplicate-replay run reached zero on all five live
hosts and also had zero custodial rows after shutdown. These observations are
recorded separately rather than interpreting teardown snapshots as live drain.

Native IV counts vary with retransmission scheduling. The independent source
restart verifier requires at least one pre-restart protection plus the manifest
record count, two source readiness markers, the controller's successful audit,
and unique valid 96-bit IVs. It does not require reproducing the previous
snapshot's incidental count of 20; this run recorded 15 in that scenario.

The first unprivileged integration attempt could not open loopback sockets;
rerunning with host socket access passed. This was an execution restriction,
not an application failure. The initial native duplicate attempt exposed the
republishing bug described above; its failure log and ground log are retained.

## Reproduction

Use the configuration and dependencies described in [build and test](build-and-test.md).
After building the integration suite, run `ctest --test-dir BUILD --output-on-failure`.
For sanitizer checks, build `storage_recovery_regression`,
`storage_error_regression`, `custody_regression`, and `demo_delivery_regression` with ASan/UBSan and run:

```sh
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
ctest --test-dir ASAN_BUILD --output-on-failure \
  -R '^(custody_regression|demo_delivery_regression|storage_(error_(ccs_ranges|custody_queue)_regression|(ccs_parse|ccs_sender|recovery|durability|constructed)_regression))$'
```

For the alternate policy, build with
`-DCMAKE_C_FLAGS=-DBPLIB_CT_MAX_CCS_SEQUENCE_SPAN=8` and run the sender and receiver
range tests. The native reproduction is:

```sh
GIT_DIR=/dev/null python3 demo/five_node.py build --build-dir /tmp/bpsec-native-build
python3 demo/test_five_node_faults.py --build-dir /tmp/bpsec-native-build \
  --run-dir /tmp/bpsec-duplicate-fresh --edge 2-3 \
  --replay-retained-duplicates --port-base 26300 --timeout 90
```

Use a new run directory. Native tests need local TCP sockets and supported POSIX
host services. Evidence preserves exact matrix commands, source/module hashes,
failed-attempt logs, and independent runtime checks. Hardware power loss,
Windows-native execution, packet-rate abuse, and exhaustive scheduling are
outside this host validation.

## Preserved records

The [evidence index](validation-sender-2026-10-30/README.md) links the recorded
builds and test setup. Key records are the
[integration results](validation-sender-2026-10-30/integration-tests.log),
[sanitizer results](validation-sender-2026-10-30/sanitizer-tests.log),
[override results](validation-sender-2026-10-30/cap8-tests.log),
[native case commands/results](validation-sender-2026-10-30/results.json),
[independent native evidence](validation-sender-2026-10-30/native-evidence.json),
[verification log](validation-sender-2026-10-30/verify-native.log),
[source hashes](validation-sender-2026-10-30/source-hashes.json), and
[binary fingerprints](validation-sender-2026-10-30/binary-fingerprints.json).
Failed and rejected attempts remain separate from final successful runs.
All three follow-up priorities are complete for this host validation scope.

The later [publication durability follow-up](publication-durability-review-2026-10-30.md)
fixes directory-sync retry for existing receipt/output/completion files. This
report and its twelve-case evidence retain their earlier binary/source hashes.
