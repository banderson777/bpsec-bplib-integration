# Storage completion validation records

These records accompany [the completion report](../storage-completion-2026-10-29.md).
They contain test results and hashes, not keys, runtime databases, or bundle payloads.

- `integration-tests.log`: final 26-entry CTest run.
- `sanitizer-tests.log`: final 11-mode storage ASan/UBSan/leak run.
- `source-hashes.json`: source snapshot for production changes and regressions.
- `binary-fingerprints.json`: native module/core/security-library identities.
- `native-evidence.json`: scenario results, output hashes, live assertions and post-stop custody counts.
- `verify-native.py`: independent persisted evidence checks used after the matrix.

Full per-node logs and runtime files remain under `/tmp/bpsec-complete-20261029`.

`verify-native.py` and `collect_evidence.py` re-read original runtime files using
paths recorded in `results.json`; they require the `/tmp` scenario directories.
The archived JSON and test logs remain readable without those directories.
`live_custody_checkpoint` records the zero-count assertion reported by the live
outage controller; `pending_custody` is a separate post-termination SQL snapshot.
