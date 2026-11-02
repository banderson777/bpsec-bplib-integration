# Startup accounting validation and publication — 2026-11-01

The [review](../accounting-review-2026-11-01.md) describes fixes, compatibility,
tests and limits. `before-negative.log`, `before-flag.log`, `before-prepare.log`
and `before-step.log` record the original invalid-counter and partial-publication
failures; `before-results.json` records exits. `accounting-fix.patch` records the
production change. `accounting-test-detail.log` extracts the startup regression's
actual output from the full-suite CTest LastTest log, without another run.

Integration results cover 37 tests; sanitizer results cover startup, ownership
and eight storage error modes. Build logs and source/binary fingerprints record
the corrected snapshot. `cases.json`/`run_matrix.py` describe a fresh baseline
and original-ID duplicate replay through relay shutdown/restart, which exercises
recovery with retained custody records. `native-build.log` records the incremental
BPLib build installed in the existing native mission; other mission artifacts are
reused and fingerprinted. The collector/verifier independently check runtimes,
outputs, custody/replay checkpoints and endpoint-only keys. They require the
recorded temporary paths; archived logs and JSON remain readable afterward.
Keys, databases and payloads are excluded. Focused tests inject malformed metadata
and query faults; native runs check valid integration and recovery.

Final results: 37/37 integration tests, 10/10 ASan/UBSan checks and 2/2 fresh
native scenarios passed. Independent verification passed for all ten runtimes,
including nine original custody IDs recovered after relay restart. No native test
hosts remained after completion.

Later validation: the [standalone scalar review](../scalar-accounting-review-2026-11-01.md) closes the direct-helper validation gap noted above. Earlier results retain their original pinned snapshot.
