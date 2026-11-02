# Sender and duplicate-recovery validation evidence

See [the review](../ccs-sender-review-2026-10-30.md) for fixes, limits, and reproduction.
The logs and JSON in this directory preserve the final integration, sanitizer,
policy override, and native matrix results. Native databases, keys and payloads
are deliberately excluded. Runtime paths in JSON refer to temporary host files;
archived results remain readable after those files are removed.

`source-hashes.json` pins the affected production, test and controller files;
`binary-fingerprints.json` pins the installed host, BPLib, BPNode, security library
and startup script. `results.json` contains commands, runtime paths and exit
codes; `native-evidence.json` adds independent SQL counts, manifest/output hashes,
module matches, endpoint-key placement, recovery logs and replay checkpoints.
`verify-native.py` invokes the read-only collector and checks those live files
against the pinned source/modules. Run it before temporary runtimes are removed.
The archived `run_matrix.py` and `cases.json` describe the host runner; adjust
build/run paths and choose unused directories/ports when reproducing.

The pre-fix duplicate test intentionally failed on changed publication timestamps.
Its logs are named `duplicate-before-fix*`; final successful evidence is separate.
Integration/sanitizer/override logs record the focused host checks. Build logs
record the final mission compilation and installation; cFS's unavailable-Git
fallback (`GIT_DIR=/dev/null`) avoids slow mounted-checkout version scans.

`integration-sandbox-attempt.log` records the socket-restricted first attempt;
the final host-access run is `integration-tests.log`. The intermediate ASan
build did not yet have the newly added target in its generated build files;
`asan-config-final.log` records regeneration and successful delivery-target
compilation, followed by the passing nine-case `sanitizer-tests.log`.

The initial node 5 restart scenario completed before restart, so its successful
controller exit was rejected by independent verification. `results-initial.json`,
`verify-before-checkpoint-fix.log` and `node5-restart-missed-checkpoint*` preserve
that attempt. Final results substitute a fresh gated incomplete-receipt run on
the same binaries. `controller-snapshots.json` identifies the two controller
versions; per-case metadata records which version ran. The rerun's exact wall
duration was not recorded. No successful runtime or database was overwritten.

The later [publication durability pass](../publication-durability-review-2026-10-30.md)
changes the live source/build snapshot. This directory remains historical; its
verification helper expects the original pinned files, not the later build.
