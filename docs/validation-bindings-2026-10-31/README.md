# SQLite binding lifetime evidence — 2026-10-31

See the [review](../sqlite-bindings-review-2026-10-31.md) for the defect, fix and
limits. `before-test.log` reproduces retained bindings after the chunk writer
returns. `after-tests.log` records the fixed focused pair. The existing size-limit
integration test already rolled back before the fix; its passing negative-control
log is preserved as `existing-size-limit-protection.log`. It is not a failed
custody-acknowledgement reproduction.

Build, full integration and sanitizer logs preserve actual results. The production
diff is `binding-fix.patch`. `cases.json` and `run_matrix.py` contain the native
commands and runner; `results.json` contains exits/timings. `native-evidence.json`
adds read-only custody SQL counts, module/host/startup identities, endpoint key
placement, source/output hashes, receipt hashes, readiness and duplicate replay
checkpoints. `verify-native.py` invokes the collector and checks runtime files
and pinned source/binary identities. It needs the original temporary paths;
archived logs and JSON remain readable after those directories are removed.
Keys, database files and payloads are excluded from this evidence folder.

These are five targeted native reruns. Historical matrices retain their original
snapshots. The earlier TSan runtime startup limitation remains; this pass does
not claim a new TSan result.

The later [SQL completeness pass](../storage-completeness-review-2026-10-31.md)
changes the live source/build snapshot. This directory remains historical and its
verifier expects its original pinned files.
