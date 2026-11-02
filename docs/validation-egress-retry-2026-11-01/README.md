# Retain consumed egress batches on marking failure — 2026-11-01

See the [review](../egress-retry-review-2026-11-01.md) for fixes, tests and limits.
Four `before-retry_*.log` files and `before-results.json` record the original
remote-egress failures. `egress-retry-fix.patch` records the production change.
`fixture-name-build.log` preserves an initial fixture naming mistake, corrected
before reproduction. `egress-retry-test-detail.log` extracts actual captured
marking-test output from the full-suite CTest LastTest log, without another run.

Integration results cover 37 tests; sanitizer results cover eight storage error
modes including all seven new caller-level failure/retry cases. Build logs and
source/binary hashes preserve the corrected snapshot.

`cases.json`/`run_matrix.py` record one fresh five-node baseline using the rebuilt
BPLib in the existing native mission. The host, BPNode, security and startup
artifacts are reused and fingerprinted. `native-build.log` records the incremental
library build. The collector/verifier check output hashes, five runtime identities,
custody drain and endpoint-only keys. They require the recorded temporary paths;
archived logs and JSON remain readable afterward. Keys, databases and payloads
are excluded. Focused tests inject SQL faults; native execution validates normal
integration. Older matrices are not counted as executions for this fix.

Later validation: the [startup accounting review](../accounting-review-2026-11-01.md) validates persisted accounting and publishes recovered counters together. Earlier results retain their original snapshot.
