# Custody/storage admission evidence — 2026-10-30

See the [review](../custody-admission-review-2026-10-30.md) for behavior, locking,
limits and reproduction. `before-test.log` reproduces refusal of an existing
duplicate under pressure; `storage-before-test.log` reproduces wrapped storage
admission. Focused and full test logs and build logs preserve actual results.
Production diffs are stored as `custody-fix.patch` and `storage-fix.patch`.

`cases.json`/`run_matrix.py` contain the native commands and runner. `results.json`
contains exits/timings. `native-evidence.json` adds read-only custody SQL counts,
module/host/startup identities, endpoint key placement, source/output hashes,
receipt hashes, current-launch readiness and duplicate replay checkpoints.
`verify-native.py` invokes the collector and checks those files against pinned
source/binary hashes. It requires the original temporary build/runtime paths;
archived JSON and logs remain readable after those directories are removed.
Keys, databases and payloads are excluded from the evidence folder.

These are five targeted native reruns, separate from older recorded matrices.

The later [parallel-storage pass](../storage-parallel-review-2026-10-30.md) changes
the live source/build snapshot. This directory remains historical; its verifier
expects the originally pinned files.
