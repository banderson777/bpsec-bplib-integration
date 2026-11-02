# Custody SQL ownership and transactions — 2026-10-31

See the [review](../custody-transactions-review-2026-10-31.md) for fixes, tests and limits.
`before-0.log`, `before-3.log` and `before-filter-failure.log` record the original
batch overlap, leaked scheduling statement and partially applied failed update.
`custody-fix.patch` records the production implementation diff. Passing build,
integration and sanitizer logs accompany exact source/binary fingerprints.

`cases.json` and `run_matrix.py` record fresh native commands. `results.json`
records their exits/timings; `native-evidence.json` records independent output,
custody, restart, module/host/startup and endpoint-key checks. `verify-native.py`
runs the collector and rechecks runtime files and pinned source/binary identities.
It requires the original temporary runtime paths. Archived logs/JSON remain readable
if those paths are removed. Keys, databases and payloads are excluded.

Focused SQLite triggers and authorizers inject faults. Native scenarios validate
integration and recovery; they do not inject the new SQL trigger faults. Prior
matrices retain their original snapshots. No new TSan result is claimed.

Later validation: the [SQL loading and maintenance review](../sql-ownership-review-2026-10-31.md) addresses the shared statement/query state left for follow-up above. Earlier results retain their original snapshot.
