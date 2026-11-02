# Concurrent SQLite statement ownership evidence — 2026-10-30

See the [review](../storage-parallel-review-2026-10-30.md) for the fix, interfaces,
test boundaries and limitations. `before-test.log` reproduces the deterministic
inter-instance failure; `after-test.log` preserves the initial fixed result.
Final integration/sanitizer logs include the completed fixture declaration.
`statement-fix.patch` records the production change. Build logs preserve actual
compilation, including the earlier fixture prototype warning corrected in the
final target builds.

The TSan target built, but `tsan-tests.log` records a fatal unexpected-memory-
mapping error before the regression could run. It is not a passing race check.
No host memory-layout/security settings were changed to accommodate it.

`cases.json`/`run_matrix.py` contain the five native commands and setup;
`results.json` contains exits and timings. `native-evidence.json` adds read-only
SQL counts, module/host/startup identities, endpoint key placement, output hashes,
receipt hashes, readiness and original-ID duplicate replay checkpoints.
`verify-native.py` rechecks runtime files and source/binary fingerprints using
the collector. It requires the original temporary build/runtime paths; archived
JSON/logs remain readable after those directories are removed. Keys, databases
and payloads are excluded from this evidence directory.

The concurrency regression uses two connections in one process. Native scenarios
remain one instance per process and check integration/recovery, not concurrent
instances in a shared process. Historical matrices keep their own snapshots.

The later [binding-lifetime pass](../sqlite-bindings-review-2026-10-31.md) changes
the live source/build snapshot. This directory remains historical and its
verifier expects its original pinned files.
