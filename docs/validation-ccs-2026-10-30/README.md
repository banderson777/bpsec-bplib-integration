# Custody-signal validation records

These files support the [custody-signal review](../ccs-review-2026-10-30.md).
They contain generated test results and hashes, without keys or bundle payloads.

- `integration-tests.log`: fresh 29-entry CTest suite.
- `sanitizer-tests.log`: six focused ASan/UBSan/leak checks.
- `source-hashes.json`: changed production and regression sources.
- Configure logs identify the generated-header and sanitizer build options.

- `build-options.json`: compiler, generated-header, and sanitizer settings.
- `cap8-tests.log`: one boundary test with the compile-time limit set to eight.
- `readiness-tests.log`: two host/application readiness checks.
- `native-result.json` and `native-test.log`: the successful controller run.
- `native-evidence.json`: matching outputs, module identities, endpoint keys,
  live custody assertion, post-stop SQL counts, and dropped-CCS marker.
- `binary-hashes.json`: native core, BPNode, BPLib, and security library hashes.
- `verify_native.py`: independent runtime/source checks used after the test.
- Build logs and the initial startup failure preserve validation history.

Run `python3 docs/validation-ccs-2026-10-30/verify_native.py` from the project to
re-read the original runtime paths in `native-result.json`. It requires matching
source files and the `/tmp` runtime; it does not overwrite a runtime or inspect
key contents. Archived JSON/logs remain readable without those temporary files.
Raw runtimes remain under `/tmp/bpsec-ccs-20261030`; endpoint keys, databases, and
bundle payloads are not copied into this validation directory.
