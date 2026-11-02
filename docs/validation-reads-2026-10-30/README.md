# File-read error validation — 2026-10-30

See the [review](../file-read-error-review-2026-10-30.md) for the bug, fix, tests
and limits. `before-test.log` reproduces successful return on an exact-fit read
followed by an EOF-probe error. `final-test.log` contains the corrected result;
`read-fix.patch` contains the production diff. Integration/sanitizer and build
logs preserve their actual results. `symbol-check.log` checks that fault-injection
wrappers are absent from the installed module.

`cases.json` and `run_matrix.py` contain native commands and runner setup;
`results.json` contains exits and timings. `native-evidence.json` adds read-only
SQL counts, module/host/startup matches, output hashes, endpoint key placement,
retained receipt hashes, restart ordering, and duplicate replay checkpoints.
`verify-native.py` invokes the collector and checks runtime files and the pinned
source/binary hashes. It needs the original temporary build/runtime paths;
archived logs and JSON remain readable after those directories are removed.
Keys, databases, and payloads are not copied into this evidence directory.

These are three targeted native reruns, separate from the earlier twelve-case
sender matrix and later completion/publication snapshots.

The later [custody-index pass](../custody-index-review-2026-10-30.md) changes the
live source/build snapshot. This directory remains historical; its verifier
expects its originally pinned files.
