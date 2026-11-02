# CRC callback portability fix — 2026-10-29

This pass addresses the incompatible callback casts left visible in the
[storage completion report](storage-completion-2026-10-29.md).

`BPLib_CRC_DigestFunc_t` returns a 64-bit `BPLib_CRC_Val_t` and accepts a
`const void *`. The CRC implementation cast functions returning `uint16_t` and
`uint32_t` with `const uint8_t *` parameters to that callback type. Calling
through an incompatible function type is undefined behavior in C. There was no
observed incorrect checksum on the previously validated native Linux build;
this correction removes the ABI dependency rather than claiming a reproduced
checksum mismatch there.

`bplib/ci/crc/src/bplib_crc.c` now uses two small adapters with exactly the generic
callback signature. Each calls its original digest implementation normally and
widens the result through an ordinary return conversion. CRC polynomials,
reflection, lookup tables, final XOR, and public API behavior are unchanged.

`integration/crc_regression.c` compiles the real CRC implementation with
`-Werror=cast-function-type`. The original source failed this build at both
callback assignments; the corrected source builds without those diagnostics.
The runtime test checks the `123456789` results (CRC-16/X.25 `0x906e`, CRC-32C
`0xe3069283`) and performs 712 comparisons against an independent reflected,
bit-at-a-time implementation. Coverage includes empty input, unaligned buffers,
block-size boundaries through 4097 bytes, every single-byte value, deterministic
random lengths, zero high bits in the public 64-bit result, repeated
initialization, and the existing unsupported-type result.

Validation:

- Full integration CTest suite: **27/27 passed**, 5.52 seconds.
- ASan/UBSan with leak detection: **4/4 passed**, 0.38 seconds. This includes the
  CRC test and the real CBOR/CRC storage recovery, process-crash durability, and
  constructed-bundle encoder tests.
- Native `bplib` module rebuilt successfully without the CRC callback warnings.
- Native five-node baseline: **passed**. Three segments match source byte for
  byte, all five nodes have zero pending custody after shutdown, keys appear only
  at nodes 1 and 5, and all five deployed BPLib modules match the new hash.
  See the [native evidence](validation-crc-2026-10-29/native-evidence.json). No
  native test hosts remain running.

The [validation records](validation-crc-2026-10-29/hashes.json),
[integration log](validation-crc-2026-10-29/integration-tests.log), and
[sanitizer log](validation-crc-2026-10-29/sanitizer-tests.log) are preserved in the
project. The original CRC source and native runtime remain under
`/tmp/bpsec-crc-20261029`. The earlier full eleven-scenario security/recovery
matrix remains evidence for its recorded pre-adapter binary; it was not rerun
for this narrowly scoped callback correction. Initialization remains a required
caller lifecycle step, and this pass makes no new claim that concurrent CRC
initialization is supported.

Commands:

```sh
cmake --build /tmp/bpsec-storage-followup-20261029 \
  --target crc_regression storage_recovery_regression -j4
ctest --test-dir /tmp/bpsec-storage-followup-20261029 --output-on-failure --timeout 60
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir /tmp/bpsec-storage-followup-20261029-asan \
  -R '^(crc_regression|storage_(recovery|durability|constructed)_regression)$' \
  --output-on-failure --timeout 30
python3 docs/check_documentation.py
```

The subsequent [custody-signal review](ccs-review-2026-10-30.md) records later
input-validation fixes and a separately identified validation build.
