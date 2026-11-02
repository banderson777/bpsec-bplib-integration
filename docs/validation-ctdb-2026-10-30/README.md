# Custody index rollback validation — 2026-10-30

See the [review](../custody-index-review-2026-10-30.md) for fixes, contracts,
tests and limits. `before-test.log` reproduces the real-tree duplicate insertion
leak/count error. `after-test.log` records the focused fixed result. Integration,
sanitizer and build logs preserve the expanded suites and actual compilation.
The production patches cover both the database helpers and bundle egress.

`cases.json`/`run_matrix.py` record native commands and setup. `results.json`
records final exits/timings; `native-evidence.json` adds read-only SQL counts,
module/host/startup identities, output hashes, endpoint key placement, retained
receipt hashes, current-launch readiness and duplicate replay checkpoints.
`verify-native.py` invokes the collector and checks those runtime files and the
pinned source/binary hashes. It requires the original temporary build/runtime
paths. Archived JSON/logs remain readable afterwards. Keys, databases and
payloads are not copied into this folder.

This is a five-scenario targeted native validation for custody index changes.
It does not recount the earlier twelve-scenario sender matrix or later three-case
file-delivery snapshots as new runs.

The later [custody-admission pass](../custody-admission-review-2026-10-30.md) changes
the live source/build snapshot. This directory remains historical; its verifier
expects its original pinned files.
