# Build and validation

Run commands from the workspace root unless a block explicitly changes directory.
The complete integration smoke project currently assumes Linux/POSIX dependency
layouts, GCC/Clang flags, and pthreads. On Windows, use a Linux environment for
that workflow and consult [Windows host notes](windows-build.md) for native host
work. Do not reuse Linux CMake caches on Windows or move caches between machines.

For five separate node sessions, follow the [five-node runbook](five-node-video-test.md)
after the dependencies below. It includes the dedicated mission build and launch
commands for Linux terminals or Windows Terminal with WSL2. The Linux automated
baseline transfer and custody drain have passed; fault-injection and restart
scenarios are listed separately in that runbook.

## Dependencies

The integration project requires CMake 3.16 or newer; BSL requires 3.23 or newer.
Use a C99 compiler, pthreads, and the BSL dependencies described in its
[README](../BSL/README.md), including OpenSSL, Jansson, QCBOR, and m-lib.
BPLib's OSAL build also needs a configured and installed NASA OSAL package.
The integration CMake file expects BSL dependency staging under `BSL/testroot`
and OSAL staging under `bplib/osal-staging`; these directories must contain real
headers/libraries, not just a copied cache.

Build BSL using its wrapper in a complete checkout with dependencies available:

```sh
cd BSL
git submodule update --init --recursive
./build.sh deps
./build.sh prep
./build.sh
./build.sh check
cd ..
```

The default integration path is `BSL/build/default`. For BPLib, follow the
[OSAL build/install instructions](../bplib/README.md), then configure the library
with the staged package directory for your installation:

```sh
cmake -S bplib -B bplib/build -DBPLIB_OS_LAYER=OSAL \
  -DNasaOsal_DIR="$PWD/bplib/osal-staging/usr/local/lib/cmake"
cmake --build bplib/build
ctest --test-dir bplib/build --output-on-failure
```

The OSAL package location depends on its install prefix; locate `NasaOsalConfig.cmake`
and pass its containing directory if the example differs from your staging tree.
BPLib's unit tests and BSL's tests are separate from the integration tests.

Python demos and the integration security regression need Python 3 and:

```sh
python3 -m pip install -r client-test/requirements.txt
python3 client-test/test_security.py
```

FFmpeg is needed only to create video segments or remux persistent playback;
ffplay is optional for headless transfers. Demo imports require `cbor2` and
`cryptography`. Inspect a script's `--help` for its complete CLI.

## Integration project

```sh
cmake -S integration -B integration/build \
  -DBSL_BUILD="$PWD/BSL/build/default" -DBPLIB_BUILD="$PWD/bplib/build"
cmake --build integration/build
ctest --test-dir integration/build -N
ctest --test-dir integration/build --output-on-failure --timeout 60
```

Absolute overrides prevent confusion about relative cache paths.

| CMake cache option | Default relative to this workspace | Purpose |
| --- | --- | --- |
| `BSL_ROOT` | `BSL` | BSL source headers and CBOR fixture |
| `BPLIB_ROOT` | `bplib` | BPLib headers and memory regression source |
| `BSL_BUILD` | `BSL/build/default` | Generated BSL headers and libraries |
| `BPLIB_BUILD` | `bplib/build` | BPLib library search location |
| `CFS_BUILD` | `cfs-build` | Generated includes for BPNode regression targets |

`bplib_tcp_cla` is an **object library**, compiling both the socket adapter and
the BPLib queue bridge. A final host executable supplies BPLib and framework
dependencies. BPNode's CMake target also compiles those two adapter sources.
The metadata smoke executable links BSL libraries, QCBOR, and SQLite; it is not
a complete host BPA with OSAL startup.

## What each test establishes

| CTest entry | Behavior covered |
| --- | --- |
| `bpsec_bplib_smoke` | Primary metadata, Mock BPA CBOR round trip, BSL data allocation/cleanup |
| `bplib_tcp_cla_smoke` | Host stream framing and socket transfer |
| `tcpclv4_chain_smoke` | Local three-node TCPCLv4 topology |
| `tcpclv4_stcp_chain_smoke` | Local mixed TCPCLv4/STCP topology |
| `tcpclv4_stcp_hdtn_chain_smoke` | Longer local mixed-protocol topology |
| `tcp_cla_regression` | Concurrent dispatch, framing failures, MRU limits, session reuse, SIGPIPE, interrupted receive/accept deadlines, delayed peer ACK dispatch |
| `memory_regression` | Allocation failure cleanup, pool initialization bounds, exact blob bounds, cross-block reads, reader failure propagation |
| `crc_regression` | Known CRC vectors, 712 differential checks, empty/unaligned inputs, reinitialization, and callback cast rejection at compile time |
| `cbor_bounds_regression` | Actual canonical decoder rejects excess extensions before writes; final valid extension and payload slots remain usable |
| `endpoint_security_regression` | Compiled BSL endpoint round trips, fail-closed policy, and mutable relay blocks |
| `custody_admission_regression` | Actual custody admission, full-resource duplicate preservation, byte limits and storage mutex contention |
| `custody_db_regression` | Real CTDB/RBT insertion rollback, allocation/accounting, collision retry, poisoned pool contents, and tree cleanup |
| `custody_regression` | Sequence counter/identifier exhaustion, allocator/index failure, CTEB publication, and CCS mutex ownership |
| `storage_binding_regression` | Actual SQLite parameter lifetimes, bind/step errors, length guards, buffer reuse and statement cleanup |
| `sql_ownership_regression` | Real SQLite loading/egress/discard/count overlap and rollback; size telemetry across eight page sizes; failed-selection rollback and retry |
| `custody_parallel_regression` | Independent-instance custody SQL overlap, preparation and step failures, deferred COMMIT failure, statement cleanup and outer-transaction preservation |
| `storage_parallel_regression` | Real two-instance SQLite stores, controlled overlap, parallel payload isolation, preparation failure and statement cleanup |
| `storage_commit_regression` | Real SQLite rollback, required-row checks, duplicate classification, admission budgets, unlocked refusal, and post-commit acceptance |
| `storage_error_*_regression` (eight entries) | Real SQLite cleanup, egress, custody rollback, and bundle recovery; accounting, preparation cleanup, allocation failures, corrupt records, concurrent custody producers, synchronous commit/rejection, SQL EID/configuration validation, and custody sequence range limits |
| `demo_delivery_regression` | Real data/EOF delivery, stream read/close errors, sync retry, receipt/output consistency, and missing-output rebuilding |
| `storage_recovery_regression`, `storage_durability_regression`, `storage_constructed_regression`, `storage_ccs_parse_regression`, `storage_ccs_sender_regression` | Real administrative decoding and persistence, restored origins, allocation cleanup, legacy recovery, crashes around commit, local ADU encoding, exact-fit guards, custody disposition map parsing, and bounded outgoing CCS construction |
| `storage_init_regression` | Real SQLite transient reader-lock recovery, bounded permanent-lock failure, statement and failed-handle cleanup |
| `deployment_health_regression` | Live native host with failed BPNode, current-process restart readiness, startup timeout, host exit |
| `five_node_stream_regression` | Streaming record encoding, contiguous HLS playlist publication, and archive byte verification |
| `video_security_regression` | Authentication, keys/nonces, custody persistence/replay, Python CLI transfers, continuous EOF completeness |
| `bpnode_ingress_regression` | Actual ingress task exits on error and passes the TCP-specific timeout; framework boundaries stubbed |
| `video_sink_regression` | Actual sink append and serialized short writes; decoder/framework stubbed |

The BPNode ingress, video sink, and custody entries appear only on POSIX when
`CFS_BUILD/native/default/apps/bpnode/CMakeFiles/bpnode.dir/flags.make` exists.
That detection requires a configured Makefile-based cFS build with usable
generated headers. The Python tests appear only if CMake finds an interpreter;
finding Python does not install its packages. Inspect `ctest -N` to avoid mistaking
an omitted test for a pass. POSIX socket tests are excluded on Windows.

Chain tests use this adapter at every simulated node. Their names do not prove
interop with external Hardy/HDTN. Sink/ingress tests do not validate a complete
flight deployment, and endpoint tests alone do not establish the five-process forwarding/custody path.
See [prior security validation](security-fixes-validation.md) for the recorded
Linux/sanitizer run and reproduction commands.

## Troubleshooting

| Symptom | Check |
| --- | --- |
| Missing BSL generated header or library | Build BSL and verify `BSL_BUILD`; staged dependency paths are Linux-oriented |
| OSAL package not found | Set `NasaOsal_DIR` to the actual installed package directory |
| Missing `m-lib`, QCBOR, or SQLite | Inspect BSL dependency staging and installed development packages |
| Two BPNode regressions absent | Check the exact `flags.make` path above and `CFS_BUILD` |
| Python import error | Install requirements into the interpreter CMake selected |
| Socket tests fail with access denied | Run where local socket creation and transfer are permitted |
| Demo bind failure | Use free ports and stop any earlier demo workers |
| Viewer waits forever | Check contiguous names from `ground-000000.ts`; use a smaller `--buffer-segments` for short persistent runs |
| Custody spool fails to load | Preserve its matching key; old unauthenticated records are incompatible |

After comment/documentation edits, run `python3 docs/check_documentation.py`.
For behavior changes, run the relevant regression and then the applicable suite.

## Memory and decoder fixes (2026-10-29)

[Review and fix record](review-2026-10-29.md) documents three bounds fixes,
regression coverage, validation commands, and remaining limits.
`cbor_bounds_regression` compiles the current BPLib canonical decoder and uses
QCBOR directly; host services, CRC validation, and unrelated field helpers are
stubbed. It runs independently of the optional generated cFS headers.

The [2026-10-29 deployment report](deployment-validation-2026-10-29.md) records
a fresh mission build and the recovery/security matrix, including the
TCP-specific I/O timeout correction found during the deployment run.

The [storage transaction follow-up](storage-review-2026-10-29.md) records later
cleanup/update fixes and the expanded 22-entry suite (including stored-bundle recovery and custody queue retries). Prior deployment counts
refer to their documented binary snapshot.

The [storage completion report](storage-completion-2026-10-29.md) supersedes prior
remaining-work statements and documents version 2 recovery, synchronous custody
commit, compatibility, and the latest full native matrix.

The current completion snapshot has 26 CTest entries. Its sanitizer subset covers
11 storage modes, including abrupt exits, distinct concurrent custody updates,
and exact encoder boundaries. Historical counts above refer to earlier snapshots.

The subsequent [CRC review](crc-review-2026-10-29.md) expands the suite to 27
entries; earlier counts remain tied to their recorded snapshots.

The [custody-signal review](ccs-review-2026-10-30.md) completes the input-validation
pass and records the current 29-entry suite, six focused sanitizer modes,
mission work-limit behavior, and native validation.

The [sender and duplicate-recovery follow-up](ccs-sender-review-2026-10-30.md) adds
the outgoing signal regression (31 CTest entries), sequence exhaustion checks,
and a native retained-duplicate restart scenario. Historical counts above refer
to their recorded snapshots.

The [publication durability review](publication-durability-review-2026-10-30.md)
adds six directory-sync failures to `demo_delivery_regression`, retaining the
31-entry suite. Its linker wrapper is test-only.

The [completion-validation review](completion-validation-review-2026-10-30.md)
extends the same delivery regression to initial/replayed EOF integrity, receipt
metadata failures, missing receipt/output recovery and zero-byte segments. The
suite remains 31 CTest entries.

The [file-read follow-up](file-read-error-review-2026-10-30.md) adds a controlled
stdio stream matrix for EOF-probe, main-read and close failures to the existing
delivery regression. Its additional `fopen` wrapper remains test-only; the suite
still has 31 entries.

The [custody-index review](custody-index-review-2026-10-30.md) adds the real-tree
regression: **32 integration entries** and ten focused sanitizer checks. Historical
counts above remain tied to their recorded snapshots.

The [custody-admission review](custody-admission-review-2026-10-30.md) expands the
suite to **33 entries**, with twelve focused sanitizer checks covering admission,
indexes, storage transactions and prior delivery/parser/recovery paths.

The [parallel-storage review](storage-parallel-review-2026-10-30.md) adds the
concurrent SQLite fixture: **34 entries** and thirteen focused ASan/UBSan checks.
A TSan attempt failed at runtime startup on this host; it is recorded separately.

The [binding-lifetime review](sqlite-bindings-review-2026-10-31.md) expands the
suite to **35 entries** and fourteen focused ASan/UBSan checks. The existing
transaction regression also retains the real blob-size-limit rollback case.

The [SQL completeness review](storage-completeness-review-2026-10-31.md) expands
the existing transaction/parallel tests to partial-write and false-duplicate
failures. The suite remains **35 entries** with fourteen focused sanitizer checks.

The [custody SQL review](custody-transactions-review-2026-10-31.md) adds
`custody_parallel_regression`, bringing the current suite to **36 entries** and
fifteen focused ASan/UBSan checks. Custody statement ownership and failed-update
rollback are checked against real SQLite.

The [SQL loading and maintenance ownership review](sql-ownership-review-2026-10-31.md)
adds `sql_ownership_regression`, bringing the current suite to **37 entries** and
sixteen focused sanitizer checks. It checks 25 controlled instance-overlap rounds
against real SQLite, including failures after an earlier statement was prepared.

The [database-size review](db-size-review-2026-11-01.md) adds geometry and failure
cases to `sql_ownership_regression`; the suite remains **37 entries**. Its focused
sanitizer run exercises the changed size helper and retained overlap tests.

The [selection rollback review](selection-review-2026-11-01.md) expands
`sql_ownership_regression` with local/remote interruption, append-capacity and retry
cases. The suite remains **37 entries** with focused sanitizer coverage.

The [egress marking retry review](egress-retry-review-2026-11-01.md) adds seven
caller-level fault/retry cases to `storage_error_mark_egress_regression`. The suite
remains **37 entries**; eight storage error modes were checked with ASan/UBSan.

The [startup accounting review](accounting-review-2026-11-01.md) extends
`storage_init_regression` with ten invalid databases, two failed-read/retry cases
and valid empty/mixed/wide accounting. The suite remains **37 entries**; startup,
ownership and eight storage error modes were checked with ASan/UBSan.

The [standalone scalar review](scalar-accounting-review-2026-11-01.md) expands
`storage_init_regression` with malformed-field, boundary, completion and retry
checks. The suite remains **37 entries**; ten related modes passed ASan/UBSan.
