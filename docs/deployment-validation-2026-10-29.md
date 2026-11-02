# Five-node deployment validation — 2026-10-29

For the later portable-storage build and full matrix rerun, see the [storage completion report](storage-completion-2026-10-29.md). This report retains the original deployment snapshot.

This run rebuilds and installs the native Linux five-node cFS mission from the
current workspace, including the three [memory/decoder fixes](review-2026-10-29.md).
All tests use fresh runtime directories under `/tmp/bpsec-deployment-20261029`.
Existing recordings, custody databases, keys, and installed builds are preserved.

**Snapshot note:** the [storage follow-up](storage-review-2026-10-29.md) records
later cleanup/update fixes and validation. The fingerprints and matrix below
apply to the earlier findings 1–6 snapshot.

## Failures found and fixed

The first baseline passed, but the 2→3 relay-restart scenario timed out.
The transfer used a 10 ms queue-poll timeout for TCP frame/ACK I/O, while peer
tasks wake on a 200 ms demo timer. Repeated partial-transfer timeouts caused
reconnects and blocked completion. BPNode now uses the configurable
`BPNODE_TCPCLA_IO_TIMEOUT_MSEC` (default 1000 ms) for TCP send/receive.
See [finding 4](review-2026-10-29.md) for the fix and targeted coverage.
The first retry passed baseline and all three link outages. Its lost-CCS case
then exposed a separate startup failure: node 1's BPNode application exited
storage initialization while its cFS host remained alive. A deterministic
read-only-lock regression reproduced the SQLite startup vulnerability; storage
now waits within a 1000 ms busy budget, reports SQLite errors, and cleans failed
handles and interrupted initialization statements. See finding 5 in the fix
record. The controller now checks application readiness before querying SQLite.

The storage-failure case exposed another correctness bug: successful rollback
replaced the original insert/COMMIT error with success. Node 2 accepted custody
without a persisted row and forwarded its memory copy; node 1 released custody.
The fix preserves the write/commit error and resets uncommitted counters so the
finalizer refuses custody and never forwards a failed batch. The new real-SQLite
insert/COMMIT regression failed before the change and passes afterward, including
sanitizer/leak checks. See finding 6 in the fix record.

The final matrix uses freshly installed binaries after all three fixes and new
runtime directories with a `-final` suffix. Initial and intermediate failure
artifacts are preserved in `results-initial.json`, `results-timeout-fix.json`,
`evidence-initial.json`, and `evidence-timeout-fix.json`. The later storage-failure
artifacts and results are preserved as `results-before-commit-fix.json` and
`evidence-before-commit-fix.json`. The original failed
run directories retain their logs and databases.

## Test harness changes

- `demo/test_five_node.py --wrong-key` changes only the fresh node 5 key and
  requires authentication rejection, no segment/completion output, live nodes,
  and retained node 4 custody. It is mutually exclusive with other fault modes.
- Wrong-key and missing-BCB checks reject any published video segment. The
  ciphertext test checks that segment zero, which contains the corrupted chunk,
  is never published. Other independently authenticated segments may be written;
  completion must remain absent and node 4 must retain the rejected record.
- `demo/test_five_node_faults.py` now waits for custody to stay drained for six
  seconds on every running node after verifying output hashes. The source is
  deliberately stopped at the outage checkpoint; this assertion excludes its stopped database. It establishes
  that recovery completes from the relay's persisted data without a live source.
  The baseline/restart/lost-CCS tests use the same six-second stability window.
  An intermediate run showed that a single zero-count sample could be followed
  by a late duplicate, so the check no longer stops at that transient sample.
- Controllers check for current-process BPNode readiness, fail promptly on
  application initialization errors, and check host-process liveness. Restart
  readiness checks skip earlier log bytes. Fault-runner logs are line-buffered.

## Build and results

The fresh native cFS mission build and install succeeded. BPNode and BPLib
were rebuilt with the bounds and TCP timeout fixes. After the SQLite startup
and commit-error fixes, the native BPLib target was rebuilt and its module
installed into the isolated mission tree before the final run. Build transcripts
are `build.log`, `rebuild-timeout.log`, `rebuild-storage-final.log`, and
`rebuild-commit.log` under the artifact root.

- **16/16 integration CTest entries passed** in 6.30 seconds, built against the
  fresh cFS headers, including the new startup-health, SQLite startup, and
  failed-commit custody regressions.
- Documentation checks passed: 29 inventory entries, 32 public Python
  classes/callables, and 44 relative links.
- **11/11 five-process deployment scenarios passed** with the final modules.
  All successful transfers matched source bytes and emitted completion once.
- Targeted storage startup and commit regressions passed ASan/UBSan and leak
  checks. The earlier memory/decoder sanitizer results remain recorded in the
  fix report. This does not claim instrumentation of the entire deployed cFS
  process or all staged dependencies.

| Scenario | Result | Seconds | Observed result |
| --- | --- | ---: | --- |
| `baseline` | PASS | 42.26 | Three segments match source; completion once; all five stores drained. |
| `outage-2-3` | PASS | 72.19 | Relay 2 retained nine records and recovered with source stopped; matching segment; live nodes drained. |
| `outage-3-4` | PASS | 91.63 | Relay 3 retained nine records and recovered with source stopped; matching segment; live nodes drained. |
| `outage-4-5` | PASS | 64.71 | Relay 4 retained nine records and recovered with source stopped; matching segment; live nodes drained. |
| `lost-ccs` | PASS | 43.23 | Node 2 drop hook fired; all three segments match; custody drained. |
| `node5-restart` | PASS | 51.56 | Node 5 initialized twice; completion followed the second initialization exactly once; matching segments and drained custody. |
| `wrong-key` | PASS | 18.25 | No segments or completion; node 4 retained seven pending custodial records. |
| `tampered-ciphertext` | PASS | 18.03 | Corrupted segment 0 blocked; no completion; valid segments 1 and 2 published; node 4 retained seven pending records. |
| `missing-bcb` | PASS | 19.20 | No segments or completion; node 4 retained seven pending custodial records. |
| `storage-failure` | PASS | 23.92 | Node 2 committed no custodial rows and produced no output; source retained seven pending records. |
| `source-iv-restart` | PASS | 40.01 | 20 unique 96-bit IVs across source restart with retained key; matching segments; custody drained. |

Durations include startup/shutdown and observation windows. The runner uses
90-second phase budgets; outage checkpoint, recovery, and custody-drain phases
have separate deadlines, so total elapsed time can exceed 90 seconds.

After process shutdown, the evidence checker verified that all 55 runtime
BPNode/BPLib module pairs matched the installed final modules, and that keys
existed only at nodes 1 and 5 in all 11 runs. Every positive run had matching
output hashes, one completion, and zero pending custody on the nodes that were
running. The intentionally stopped source may retain custody rows after an
outage; it is explicitly excluded from that drain assertion. Negative cases
had no completion and retained upstream pending custody. Node 5's restart log
confirmed completion occurred after the second readiness marker.

Final installed module SHA-256 fingerprints:

| Module | SHA-256 |
| --- | --- |
| `bplib.so` | `c8e7f212d45e8677768e37ae08d7fcd8b549edd14348da15dd549a58e20fcf89` |
| `bpnode.so` | `8c553e469031e8e41cfc66c483e13a227b7087e98e45e6a2b562dbf61e39e0b7` |

The full commands, timing, final SQLite observations, hashes, key-presence checks,
module comparisons, and restart/IV evidence are in `results.json` and
`evidence.json`. Source fingerprints are in `source-fingerprints-final.json`;
all installed binary hashes are in `binary-fingerprints.json`.

## Reproduction

Run from the workspace root on Linux where local sockets, native cFS processes,
and their shared-memory runtime are permitted. Select a new build/run prefix;
prepare/test commands refuse to overwrite existing run directories. Run scenarios
sequentially because the five CPU IDs and cFS runtime names are reused.

```sh
BPSEC_RUN=$(mktemp -d /tmp/bpsec-deployment-replay-XXXXXX)
BPSEC_BUILD="$BPSEC_RUN/build"
python3 demo/five_node.py build --build-dir "$BPSEC_BUILD"
python3 demo/test_five_node.py --build-dir "$BPSEC_BUILD" \
  --run-dir "$BPSEC_RUN/baseline" --port-base 24600
for edge in 2-3 3-4 4-5; do
  python3 demo/test_five_node_faults.py --build-dir "$BPSEC_BUILD" \
    --run-dir "$BPSEC_RUN/outage-$edge" --edge "$edge" --port-base 24620
done
python3 demo/test_five_node.py --build-dir "$BPSEC_BUILD" \
  --run-dir "$BPSEC_RUN/lost-ccs" --drop-ccs-node 2 --port-base 24680
python3 demo/test_five_node.py --build-dir "$BPSEC_BUILD" \
  --run-dir "$BPSEC_RUN/node5-restart" --restart-node5-after-receipt --port-base 24700
python3 demo/test_five_node.py --build-dir "$BPSEC_BUILD" \
  --run-dir "$BPSEC_RUN/wrong-key" --wrong-key --port-base 24720
python3 demo/test_five_node.py --build-dir "$BPSEC_BUILD" \
  --run-dir "$BPSEC_RUN/tampered-ciphertext" --tamper-ciphertext --port-base 24740
python3 demo/test_five_node.py --build-dir "$BPSEC_BUILD" \
  --run-dir "$BPSEC_RUN/missing-bcb" --omit-bcb --port-base 24760
python3 demo/test_five_node.py --build-dir "$BPSEC_BUILD" \
  --run-dir "$BPSEC_RUN/storage-failure" --storage-fail-node 2 --port-base 24780
python3 demo/test_five_node.py --build-dir "$BPSEC_BUILD" \
  --run-dir "$BPSEC_RUN/source-iv-restart" --audit-ivs-across-node1-restart --port-base 24800
```

The actual sequential runner and exact per-case commands are retained in
`/tmp/bpsec-deployment-20261029/run_matrix.py` and `results.json`.
The source and build fingerprints, full build log, per-case controller logs,
per-node runtime logs, and SQLite stores remain under that same root. Temporary
artifacts may be removed by the operating system; this report preserves the
validation conclusions. Keep runtime keys private and with their custody state.

## Startup diagnostics

Some native runs log cFS's one-second soft startup-sync warning while BPNode
finishes initialization shortly afterward. The cFS source documents that this
threshold logs a warning without stopping the application. The controller now
requires an explicit current-process BPNode readiness marker and fails on an
initialization error; a live host alone is never counted as successful startup.
The deployment does not claim every application starts within one second.

## Limits

These are native Linux process and loopback tests using deterministic bytes in
segment files. They do not prove video playback quality, real network outages,
external Hardy/HDTN interop, native Windows support, hardware power-loss durability,
or production flight-system qualification. Restart tests use process termination;
individual security failures are observed for a finite window. IV uniqueness is
an observation over the exercised source restart, not a proof of RNG quality.
