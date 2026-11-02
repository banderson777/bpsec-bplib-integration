# Custody-signal input validation — 2026-10-30

This pass completes the custody-signal changes left awaiting final validation
after the [CRC follow-up](crc-review-2026-10-29.md). It covers compressed custody
signals (CCS), their disposition codes, and their sequence ranges.

## Fixes

The CBOR disposition-map parser now requires an integer label that is nonzero
and exactly representable in `BPLib_CT_DispositionCode_t`. Previously it narrowed
the 64-bit wire label before validation. On the validated enum representation,
`-4294967295` could narrow to positive `1`, changing a refusal into acceptance.
The parser now rejects that conversion. Ordinary positive/negative values and
other representable nonzero values retain their sign and behavior. Text labels
and oversized integer labels fail with `BPLIB_CBOR_DEC_INV_DISP_CODE_ERR`.

The parser also validates its count-output pointer, resets the count to zero
before parsing, and publishes a nonzero count only after the complete map has
passed validation. Array storage may be partially populated on a parse error;
callers must honor the returned error and zero count.

`BPLib_CT_ProcessBundleSeqCollection` now validates null arguments, disposition
zero, array length, cumulative work, and sequence arithmetic before taking the
CT mutex or looking up/changing any custody entry. It uses 64-bit sequence
positions instead of platform-sized `size_t`. Iteration uses a bounded offset
rather than a potentially overflowing exclusive endpoint. A one-element range
starting at `UINT64_MAX` is supported; a range extending past it is rejected.
A lookup that reports success but supplies no entry now produces a not-found
error rather than silently reporting success in the tested missing-entry case.

## Processing limit and compatibility

`BPLIB_CT_MAX_CCS_SEQUENCE_SPAN` bounds the total sequence positions covered by
one incoming collection, including excluded gaps. Its default is
`BPLIB_CT_DB_MAX_ENTRIES`, currently **10,000**. It is a compile-time mission
setting, guarded by `#ifndef` in `bplib/bpa/ct/inc/bplib_ct.h`; for example,
`-DBPLIB_CT_MAX_CCS_SEQUENCE_SPAN=20000` changes it for a mission build.

A collection exceeding the configured limit fails with `BPLIB_STOR_PARAM_ERR`
before any of its operations are applied. Custody is retained and the peer must
split a larger span into separate messages or use a compatible mission limit.
This is a deliberate compatibility restriction: a previously accepted sparse
range with a very large gap can now be rejected. Use a positive limit that fits
the 32-bit statistics fields and revalidate the mission's processing budget when
changing it. No wire-format version or database migration is introduced.

Preflight and the work limit apply per collection, not atomically to every
collection in an entire CCS. The current decoder permits up to five received
collections per CCS. Processing cost still scales with the limit, database
operations, and incoming message rate; this bound is not a packet-rate limiter.
The existing synchronous SQL commit boundary continues to determine when a
custody operation has succeeded durably.

## Tests

`storage_ccs_parse_regression` compiles the real QCBOR and BPLib parser. It checks
acceptance/refusal and another representable positive code, zero, sign-changing
and oversized integer labels, text labels, null count output, and zero count on
errors. It does not simulate custody transfer through a parser stub.

`storage_error_ccs_ranges_regression` compiles the real collection processor and
SQLite storage code, with controlled CT lookup/accounting/event boundaries. It
checks invalid pointers, oversized arrays, arithmetic overflow, individual and
cumulative work-limit violations, zero lookups before invalid input is rejected,
a success-with-null lookup result, the valid maximum 64-bit sequence, and the
exact 10,000-position boundary. The valid maximum-sequence case must update the
real SQL egress mark and CT state/count. The exact-limit case uses already
transferred entries so it checks iteration without 10,000 redundant commits.

The existing custody test also covers 548 distinct updates submitted by four
concurrent producers and SQL failure/retry behavior. The real administrative
recovery, crash-durability, and constructed-bundle tests run in the focused
sanitizer subset.

## Validation

Fresh build directories were required because the previous `/tmp` builds had
been removed. The current integration build uses the checkout's existing cFS
header configuration; a separate fresh native mission is built by
`demo/five_node.py`. No existing recording or runtime is reused.

- Integration CTest: **29/29 passed**, 11.76 seconds.
- ASan/UBSan with leak detection: **6/6 passed**, 5.57 seconds.
- Compile-time override to eight positions: **1/1 boundary test passed**,
  0.02 seconds; this is a separate build of the real collection processor.
- Readiness helper tests: **2/2 passed** after the controller startup adjustment.
- Fresh native dropped-acknowledgement scenario: **passed**, 42.54 seconds.
  Three segments match source byte for byte, exactly one CCS was dropped at
  node 2, and the controller verified all five nodes released accepted custody.
  Post-stop SQL checks also show zero pending custody at every node in this run.
  Both BPLib and BPNode hashes match at all five runtimes, keys exist only at
  endpoints 1 and 5, and no undefined receipt-clock delta was logged.

The native builder's optional `git describe --dirty` version scans were slow on
the mounted checkout. The initial builder was stopped and restarted with
`GIT_DIR=/dev/null`, using cFS's existing unavailable-Git-description fallback.
The source code and protocol behavior are unaffected; exact source/binary
hashes identify this validation build. Both build logs are retained.

Reproduction commands (the native build may take several minutes):

```sh
cmake -S integration -B /tmp/bpsec-ccs-20261030/integration \
  -DCFS_BUILD=/mnt/c/Users/bande/OneDrive/Documents/source/bpsec-bplib-integration/cfs-build
cmake --build /tmp/bpsec-ccs-20261030/integration -j4
ctest --test-dir /tmp/bpsec-ccs-20261030/integration --output-on-failure --timeout 60
cmake -S integration -B /tmp/bpsec-ccs-20261030/asan \
  '-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie' \
  '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined -no-pie'
cmake --build /tmp/bpsec-ccs-20261030/asan \
  --target storage_error_regression storage_recovery_regression -j4
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir /tmp/bpsec-ccs-20261030/asan \
  -R '^storage_(error_(ccs_ranges|custody_queue)_regression|(ccs_parse|recovery|durability|constructed)_regression)$' \
  --output-on-failure --timeout 30
GIT_DIR=/dev/null python3 demo/five_node.py build \
  --build-dir /tmp/bpsec-ccs-20261030/build
python3 demo/test_five_node.py --build-dir /tmp/bpsec-ccs-20261030/build \
  --run-dir /tmp/bpsec-ccs-20261030/lost-ccs-final --port-base 25400 \
  --timeout 90 --drop-ccs-node 2
python3 docs/check_documentation.py
```

The earlier full eleven-scenario matrix and CRC baseline remain evidence for
their respective recorded binaries. They are not claimed as fresh runs against
this custody-signal build. Hardware power loss, Windows-native behavior, and
exhaustive thread schedules remain outside this validation.

The first native launch aborted before node 1 initialized because simultaneous
hosts raced OSAL's shared RAM-directory `stat`/`mkdir` sequence and received
`File exists`. The controllers now wait for the first host's current readiness
marker before launching the others. The same ordering applies to both normal
and outage controllers; it does not change custody processing or OSAL itself.
The first failed runtime/logs remain in `lost-ccs` and the final retry uses
`lost-ccs-final`. No existing shared RAM directory was deleted to force a result.

## Preserved evidence

The [validation directory](validation-ccs-2026-10-30/README.md) contains
[integration results](validation-ccs-2026-10-30/integration-tests.log),
[sanitizer results](validation-ccs-2026-10-30/sanitizer-tests.log),
[override results](validation-ccs-2026-10-30/cap8-tests.log),
[native evidence](validation-ccs-2026-10-30/native-evidence.json),
[source hashes](validation-ccs-2026-10-30/source-hashes.json), and
[binary hashes](validation-ccs-2026-10-30/binary-hashes.json).
Build logs, selected build options, the initial startup failure, readiness tests,
and the independent native verification helper are preserved alongside them.
The helper rechecks original `/tmp` runtime files and current source hashes;
archived JSON/logs remain readable if those temporary runtimes are removed.
Use an unused run directory when repeating a native test.

To check the alternate bound:

```sh
cmake -S integration -B /tmp/bpsec-ccs-20261030/cap8   '-DCMAKE_C_FLAGS=-DBPLIB_CT_MAX_CCS_SEQUENCE_SPAN=8'
cmake --build /tmp/bpsec-ccs-20261030/cap8 --target storage_error_regression -j4
ctest --test-dir /tmp/bpsec-ccs-20261030/cap8   -R '^storage_error_ccs_ranges_regression$' --output-on-failure --timeout 20
```

These checks validate input types, bounds, and processing behavior; they do not
authenticate a custody-signal sender or replace mission-level traffic policy.

The later [sender and duplicate-recovery review](ccs-sender-review-2026-10-30.md)
completes the outgoing-limit and restart priorities, with an expanded suite and
a fresh full native matrix. Counts in this report describe its earlier snapshot.
