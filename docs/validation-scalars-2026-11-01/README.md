# Standalone scalar accounting reads — 2026-11-01

The [review](../scalar-accounting-review-2026-11-01.md) describes fixes, compatibility,
contracts and limits. Four `before-scalar-*.log` files and `before-results.json`
record the original failures. The count limit is tested with a connection-local
aggregate, not a physical billion-row database. `scalar-fix.patch` records the
production change. `raw-sql-compatibility.log` confirms the original exported raw
SQL definitions are retained; validated helpers use private immutable queries.

`scalar-test-detail.log` extracts actual startup/scalar output from the full-suite
CTest LastTest log, without another run. Integration and sanitizer logs record
the final private-query implementation. Build logs and source/binary hashes pin
the final snapshot after the raw SQL compatibility adjustment.

`cases.json`/`run_matrix.py` record a fresh baseline in `baseline-final`, using the
rebuilt BPLib in the existing mission. An earlier passing baseline in `baseline`
preceded that compatibility adjustment and is not counted as final validation.
The host, BPNode, security and startup artifacts are reused and fingerprinted.
The collector/verifier check five runtimes, output hashes, custody and endpoint
keys. They require the recorded temporary paths; archived logs/JSON remain readable
without them. Keys, databases and payloads are excluded. Focused tests inject the
faults; native execution checks normal integration. Historical matrices are not
counted as executions for this fix.

Final private-query implementation: 37/37 integration tests, 10/10 ASan/UBSan
checks and 1/1 fresh native baseline passed. Independent verification passed for
all five runtimes and the final pinned source/binary snapshot. No cFS test hosts
remained after completion.
