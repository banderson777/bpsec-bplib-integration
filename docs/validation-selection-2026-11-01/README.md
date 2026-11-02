# Failed egress-selection rollback — 2026-11-01

The [review](../selection-review-2026-11-01.md) describes fixes, contracts and limits.
`before-selection-failure.log` and `before-selection-capacity.log` record partial
batch publication on error. `before-results.json` records their failed assertions.
`selection-fix.patch` records the production change. `selection-test-detail.log`
extracts this regression's output from the full-suite CTest LastTest log, without
another test run. The directory includes complete integration/sanitizer results.

`cases.json`/`run_matrix.py` describe one fresh five-node baseline using the rebuilt
BPLib module in the existing mission. `native-build.log` records that incremental
library build; the host, BPNode, security and startup artifacts are reused and
fingerprinted. `results.json` records the baseline exit/timing. The collector and
verifier check output hashes, runtime identities, custody and endpoint-only keys.
They require the original temporary paths; archived logs/JSON remain readable
after those paths disappear. Keys, databases and payloads are excluded.

The focused regression injects selection faults. Native execution checks normal
integration, without injecting those faults. Historical matrices are not counted
as executions for the current fix.

Final results: 37/37 integration tests, 1/1 focused ASan/UBSan regression and
1/1 fresh native baseline passed. Independent verification passed for all five
runtimes, outputs, custody state and source/binary identities. No native test hosts
remained after completion.

Later validation: the [egress marking retry review](../egress-retry-review-2026-11-01.md) retains consumed batches after SQL marking errors. Results above retain their original pinned snapshot.
