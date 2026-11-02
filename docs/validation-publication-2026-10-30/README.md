# Publication durability validation — 2026-10-30

See the [review](../publication-durability-review-2026-10-30.md) for the fixes
and their limits. `before-test.log` demonstrates the replay bug with real
filesystem operations and an injected directory `fsync` error. The before/after
build logs and `after-test.log` preserve reproduction and the corrected result.
`integration-tests.log` and `sanitizer-tests.log` contain final suite results.

`cases.json` and `run_matrix.py` record the native commands and host runner;
`results.json` records exits and timings. All use fresh temporary runtimes.
`native-evidence.json` adds read-only SQL counts, module/host/startup matches,
output hashes, current-launch readiness, endpoint key placement, and replay
checkpoints. `verify-native.py` invokes `collect_evidence.py` and rechecks those
runtime files and the pinned `source-hashes.json` and `binary-fingerprints.json`.
Run it before the temporary runtime/build directories are removed. Logs and
archived JSON remain readable afterwards. Keys, database contents and payloads
are not copied into this evidence directory.

This pass runs three native scenarios against its new BPNode binary. The earlier
full twelve-case sender matrix remains evidence for its own recorded binary;
its counts are not claimed as new executions in this pass.

`publication-fix.patch` preserves the production-code change; `symbol-check.log`
confirms the installed module has the ordinary fsync import and no test wrapper.
`build-options.txt` records the toolchain flags and execution setup.

The [completion-validation pass](../completion-validation-review-2026-10-30.md)
changes the live source and build snapshot. This directory remains historical;
its verifier expects its original pinned files.
