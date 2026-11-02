# Validate ground completion against retained receipts — 2026-10-30

This follows the [publication durability review](publication-durability-review-2026-10-30.md).
The next delivery review found that completion used a weaker integrity check than
data replay: EOF processing only required readable ground filenames. A changed,
truncated, or extended segment could therefore pass completion even though a
replayed data chunk would reject the same output.

## Fix

Data delivery and EOF now share `persist_segment`. It validates each referenced
receipt's record tag, session, segment/part index, part count, total segment count,
and declared payload size. Existing ground data must exactly match the retained
payloads and their combined length. Missing output can be reconstructed from a
complete, consistent receipt set through the existing file-sync, rename, and
directory-sync path. A conflicting existing output is rejected without rewriting.

EOF obtains each segment's part count from its first receipt, validates that
receipt's header and bounded nonzero count, then requires every part. Missing
receipts fail completion even when an output file is readable. Before either
creating a completion record or accepting an identical replay, all referenced
segments must pass those checks. EOF also syncs the receipts directory, retaining
the durability boundary for its retained reference files. The existing output
and completion directory-sync retries remain in place.

The record header now requires at least one segment. A zero-segment EOF formerly
skipped every validation loop and could create completion immediately. The demo
producer already rejects an empty input file set; the receiver now enforces that
same contract. A zero-byte input file remains a valid one-segment recording.

The helper receives the caller's existing 8 KiB scratch buffer. It does not
recursively invoke delivery or add another chunk-sized buffer to the native
CLA task's stack. Partial data arrivals still succeed once their receipt is
stored; they do not publish a segment until all its parts are available. Valid
zero-byte segments remain supported.

## Compatibility and limits

Retain the demo's receipt directory through completion and replay. Ground output
without its matching receipts can no longer establish completion, even if a
previous completion record exists. No wire format or database migration changes.
This intentionally strengthens the optional native demo's replay checks.

Completion checks now read the referenced receipts and output bytes. Work scales
with the session's stored data and runs under the existing file mutex. This is
integrity validation, not a packet-rate limiter or a claim about real-time I/O
latency. Receipt metadata counts retain the existing limits: 1–100,000
segments and 65,536 parts per segment.

The retained receipt files were authenticated on arrival; this comparison does
not cryptographically authenticate local storage again. Coordinated alteration
of both receipts and output is outside the check. A detected conflict withholds
successful delivery; it does not overwrite the conflicting output or revoke a
completion already observed by a consumer. Physical power loss, hostile local
filesystem replacement, and Windows-native execution remain outside this host
validation.

## Regression

The actual demo implementation is compiled in `demo_delivery_regression`, with
real filesystem reads, writes, renames, and sync calls. The pre-fix regression
fails because EOF returns success for changed output. The corrected regression
checks:

- Changed, truncated and extended output rejected on both initial EOF and
  completion replay, with no output replacement or completion publication.
- Missing output reconstructed from complete receipts, preserving existing
  completion identity and modification time.
- Missing first or later receipts, inconsistent segment metadata, zero/oversized
  part counts, and a wrong session rejected despite readable ground output.
- Unsupported zero-segment EOF rejected without creating completion; valid
  zero-byte segment publication and completion retained.
- Existing immutable data/EOF replay and conflicting data-record checks.
- All six previous post-rename directory-sync failures/retries, plus one receipt
  directory-sync failure during EOF. The test wrapper can skip the segment sync
  to target the later completion-record sync precisely.

The linker fault injector remains test-only. Installed-module symbols confirm
ordinary `fsync` usage without the wrapper. All original assertions about
unchanged file inode, size and modification time remain.

## Validation

- Integration CTest: **31/31 passed**, 6.14 seconds.
- ASan/UBSan with leak detection: **9/9 passed**, 1.36 seconds.
- Direct before/after reproductions: damaged-output EOF and zero-segment EOF
  false successes confirmed, then fixed.

Final native validation: **3/3 passed** on the final source/module snapshot,
including the zero-segment guard. Independent checks cover all 15 runtimes,
module/host/startup identities, endpoint-only keys, output hashes, custody and
restart checkpoints, and unchanged retained receipts. No receipt-clock or
BPLib initialization errors were found.

| Scenario | Result | Evidence |
| --- | --- | --- |
| Original-ID duplicate replay | Passed, 116.05 s | Nine original IDs recovered; all five live hosts drained; immutable output/completion; 38 unique IVs |
| Baseline transfer | Passed, 40.08 s | Three matching segments; custody drained |
| Incomplete-receipt receiver restart | Passed, 72.21 s | Saved receipt preserved; completion after new readiness; three matching segments |

The first native set preceded the final zero-segment guard and is retained as
intermediate evidence. All three scenarios were repeated in fresh `final-*`
runtime directories for the final results above. Neither set is counted as a
new full twelve-scenario matrix.

The [evidence directory](validation-completion-2026-10-30/README.md) contains the
logs, exact source/binary identities and fresh native runtime evidence. Older
counts remain evidence for their previously recorded snapshots.

## Reproduction

Use the configured headers and dependencies in [build and test](build-and-test.md):

```sh
cmake --build INTEGRATION_BUILD --target demo_delivery_regression -j4
ctest --test-dir INTEGRATION_BUILD -R '^demo_delivery_regression$' --output-on-failure
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
ctest --test-dir ASAN_BUILD -R '^demo_delivery_regression$' --output-on-failure
```

The native commands use fresh run directories and ports, recorded in
`validation-completion-2026-10-30/cases.json`. The cFS rebuild/install uses
`GIT_DIR=/dev/null` for the existing version-description fallback, and restores
the generated demo startup script after installation.

Preserved records include the [damaged-output reproduction](validation-completion-2026-10-30/before-test.log),
[zero-segment reproduction](validation-completion-2026-10-30/zero-before-test.log),
[integration results](validation-completion-2026-10-30/integration-tests.log),
[sanitizer results](validation-completion-2026-10-30/sanitizer-tests.log),
[final native commands/results](validation-completion-2026-10-30/results.json),
[independent native evidence](validation-completion-2026-10-30/native-evidence.json),
and [verification log](validation-completion-2026-10-30/verify-native.log).
The production patch, exact source/binary hashes and intermediate records are
kept alongside these logs.

The subsequent [file-read error review](file-read-error-review-2026-10-30.md)
fixes EOF-probe error propagation in the shared loader. This completion report
and its evidence retain their earlier source/binary identities.
