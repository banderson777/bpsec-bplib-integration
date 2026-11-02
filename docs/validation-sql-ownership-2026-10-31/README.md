# SQL loading and maintenance ownership — 2026-10-31

The [review](../sql-ownership-review-2026-10-31.md) describes fixes, contracts and limits.
`before-results.json` records six isolated original-implementation failures;
`before-0.log`, `before-2.log`, `before-4.log`, `before-6.log`, `before-7.log` and
`before-8.log` preserve their diagnostics (the SIGSEGV case has no text output).
The regression was expanded afterward with later-preparation failures and stronger
rollback assertions. `sql-ownership-fix.patch` records all four production files.

Passing build, integration and sanitizer logs accompany pinned source/binary hashes.
`cases.json`/`run_matrix.py` record the fresh native commands and bounded runner;
`results.json` records exits/timings, and `native-evidence.json` records independent
output/custody/restart/module checks. The collector and verifier require the original
temporary runtime paths; archived logs and JSON remain readable without those paths.
Keys, databases and payloads are excluded. Focused SQLite tests inject the SQL faults;
native cases validate integration and recovery. No new TSan result is claimed.

Native validation resumed on 2026-11-01 after temporary build files were lost.
`interrupted-cases.json` preserves the earlier planned commands; no result is
claimed for that unfinished build. Current `cases.json` targets the fresh build.

Final validation on 2026-11-01: the fresh native mission built and installed,
all five scenarios passed, and independent verification passed for 25 runtimes.
`native-build.log` is the complete fresh build log. The 37-test integration and
16-test sanitizer logs preserve the already completed unchanged source snapshot.

Later validation: the [database-size telemetry review](../db-size-review-2026-11-01.md) fixes page geometry and failed size-query reporting. The results above retain their original pinned snapshot.
