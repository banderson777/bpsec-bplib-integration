# Completion validation evidence — 2026-10-30

See the [review](../completion-validation-review-2026-10-30.md) for behavior,
compatibility and limits. `before-test.log` reproduces successful EOF acceptance
of damaged output with the previous source; `zero-before-test.log` separately
reproduces the zero-segment bypass; `final-test.log` records the corrected
regression. Build logs, `integration-tests.log`, and `sanitizer-tests.log` preserve
suite results. `completion-fix.patch` records the production-code diff.

`cases.json` and `run_matrix.py` contain exact native commands and runner setup.
`results.json` records exits and timings. `native-evidence.json` contains read-only
SQL custody counts, output/manifest hashes, current-launch restart markers,
retained-receipt hashes, module/host/startup matches, key placement, and duplicate
replay checkpoints. `verify-native.py` invokes the collector and checks runtime
files against the pinned `source-hashes.json` and `binary-fingerprints.json`.
It requires the original temporary runtimes/builds; archived JSON/logs remain
readable after those directories are removed. Keys, databases, and payloads are
excluded from this evidence folder. `symbol-check.log` checks that fault injection
is absent from the installed module.

These are three targeted native reruns. The earlier twelve-case sender matrix
and publication-durability evidence retain their own distinct snapshots.

The three initial native scenarios ran before the zero-segment guard was added.
They remain in `results-intermediate.json` with `intermediate-*` source/binary
fingerprints and build/test logs. Final results rerun all three scenarios on the
final source/module snapshot in separate `final-*` temporary runtime directories.

The later [file-read pass](../file-read-error-review-2026-10-30.md) changes the live
source/build snapshot. This evidence remains historical and its verifier expects
the originally pinned files.
