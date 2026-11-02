# Database-size telemetry — 2026-11-01

See the [review](../db-size-review-2026-11-01.md) for changes, contracts and limits.
`confirmed-before-db-size.log` records incorrect page geometry accounting;
`confirmed-before-db-size-failure.log` records success after interrupted execution.
`confirmed-before-results.json` records their failing exits. `db-size-fix.patch`
preserves the production change. Test/build logs and source/binary hashes record
the corrected snapshot.

`cases.json` and `run_matrix.py` describe one fresh native baseline using the
rebuilt BPLib module installed in the existing native mission. `native-build.log`
records the incremental native library build. Host, BPNode, security and startup
artifacts are reused and fingerprinted. `results.json` records the baseline exit
and timing; the collector/verifier independently check all five runtimes, matching
outputs, custody drain and endpoint keys. Verification requires the recorded
temporary paths; archived logs/JSON remain readable after those paths are removed.
Keys, databases and payloads are excluded from this evidence folder.

Focused SQLite tests inject query faults and check geometry against file lengths.
The native baseline checks integration after the fix. Historical five-case matrices
are not counted as new executions in this pass.

Final results: 37/37 integration tests, 1/1 focused ASan/UBSan test and 1/1
fresh native baseline passed. `size-test-detail.log` extracts this regression's
actual captured output from CTest's full-suite LastTest log, without another run.
It records all page sizes and the two rejected interrupted query steps.

Later validation: the [egress-selection review](../selection-review-2026-11-01.md) prevents partial batch publication after selection errors. Results above retain their original pinned snapshot.
