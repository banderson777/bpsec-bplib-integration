# Complete SQL bundle writes — 2026-10-31

See the [review](../storage-completeness-review-2026-10-31.md) for changes, contracts
and limits. `before-unique.log`, `before-blob.log` and `before-recovery.log` preserve
three real-SQLite cases that committed partial batches and attempted two custody
acceptances. Focused passing, full integration, sanitizer and build logs preserve
the corrected results. `completeness-fix.patch` records the production changes.

`cases.json`/`run_matrix.py` record the five native commands and runner;
`results.json` records exits/timings. `native-evidence.json` adds read-only SQL
custody counts, module/host/startup identities, endpoint key placement, source/
output hashes, receipt hashes, readiness and duplicate replay checkpoints.
`verify-native.py` invokes the collector and rechecks runtime files and pinned
source/binary identities. It requires the original temporary paths. Archived
logs/JSON remain readable after those paths are removed. Keys, databases and
payloads are excluded from this evidence folder.

These native scenarios check integration and recovery. Trigger/constraint fault
cases and genuine duplicate boundaries are exercised in focused real-SQLite tests.
Older matrices retain their own snapshots. No new TSan result is claimed.

Later validation: the [custody SQL review](../custody-transactions-review-2026-10-31.md) adds instance isolation and atomic retransmission scheduling. Results above describe the earlier pinned build.
