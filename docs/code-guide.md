# Code guide

## Architecture and boundaries

The local C path is `BPNode contact tasks -> BPLib CLA queues -> TCP adapter ->
peer`. BPLib owns decoded bundles, routing, and storage. The adapter transports
encoded bundle bytes, using either four-byte length-prefixed STCP or a limited
TCPCLv4 session implementation. BPNode integrates it per contact in
`bpnode/fsw/src/bpnode_cla_in.c` and `bpnode_cla_out.c`; contact state lives in
`bpnode_app.h`. The optional video sink extracts payload bytes from a decoded
bundle and appends them to an OSAL file.

The BSL metadata bridge is separate. `BSL_BundleRef_t.data` borrows a
`BPLib_Bundle_t`; callbacks copy selected metadata into BSL structures.
Endpoint IDs and extension-block enumeration are not translated by this bridge.
The Mock BPA wire round trip decodes and re-encodes a fixture through BSL; it
does not apply or verify BIB/BCB security blocks. The separate
`bplib_bsl_security` endpoint bridge uses Mock BPA host services plus BSL
AES-GCM to protect/accept complete wire bundles for the optional five-node lab.

The Python path is independent: source, relay, and ground threads exchange
AES-GCM-protected CBOR maps over loopback UDP. A `bp: 7` map field does not make
these records BPv7 bundles. All three participants possess the same key, so the
relay can authenticate/decrypt records. These client-test simulations do not call the C adapter or cFS. The separate
`demo/five_node.py` controller builds and launches five actual cFS processes.

## Local source inventory

This inventory covers project-owned integration code, demos, scripts, and
mission configuration.
Dependency modules retain their own source comments and guides linked from the
[root README](../README.md). Generated builds, staging trees, recordings, and
databases are outside this inventory.

| Source | Responsibility |
| --- | --- |
| `integration/bplib_bsl_adapter.h` | Public metadata/codec contracts |
| `integration/bplib_bsl_adapter.c` | Translate primary/payload metadata; Mock BPA decode/encode |
| `integration/bplib_bsl_security.h` | Endpoint wire-security API and ownership contract |
| `integration/bplib_bsl_security.c` | BSL BCB AES-256-GCM protection/acceptance with fixed demo endpoint policy |
| `integration/bplib_tcp_cla.h` | Protocol configuration, context lifetime, public transport API |
| `integration/bplib_tcp_cla.c` | Socket setup, handshake, STCP framing, TCPCLv4 dispatcher/ACKs |
| `integration/bplib_socket_compat.h` | POSIX/Winsock handle types, errors, polling, socket close |
| `integration/bplib_tcp_cla_bplib.c` | One-bundle ingress/egress queue bridge with temporary buffers |
| `integration/main.c` | BSL metadata/codec/data smoke executable |
| `integration/tcp_cla_smoke.c` | Basic adapter socket smoke executable |
| `integration/tcpclv4_chain_smoke.c` | Three-node local TCPCLv4 smoke topology |
| `integration/tcpclv4_stcp_chain_smoke.c` | Local mixed-protocol relay smoke topology |
| `integration/tcpclv4_stcp_hdtn_chain_smoke.c` | Extended local mixed-protocol smoke topology |
| `integration/tcp_cla_regression.c` | Framing, concurrent dispatch, failure, size, lifecycle regressions |
| `integration/memory_regression.c` | Allocation failure cleanup, pool buffer bounds, and blob/reader range regressions |
| `integration/demo_delivery_regression.c` | Real demo data/EOF delivery, read/close/sync error injection, receipt/output consistency, immutable replay, and missing-output reconstruction |
| `integration/storage_recovery_regression.c` | Real SQL/CBOR/CRC recovery, origin metadata, crash durability, constructed ADUs, encoder boundaries, CCS disposition labels, and real outgoing CCS span/identity handling |
| `integration/storage_error_regression.c` | Real SQLite cleanup, egress, custody failure/rollback/accounting and stored-bundle recovery, concurrent custody updates, and CCS sequence bounds |
| `integration/storage_binding_regression.c` | Actual SQLite chunk bindings: buffer release, bind/step errors, invalid lengths, retry and statement cleanup |
| `integration/sql_ownership_regression.c` | Real SQLite selection, local/remote egress, discard and scalar counts: instance overlap, rollback and cleanup; database-size geometry/query failures and failed-selection rollback |
| `integration/custody_parallel_regression.c` | Real SQLite custody operations on independent instances: deterministic overlap, preparation/update/COMMIT failures, statement cleanup and outer-transaction preservation |
| `integration/storage_parallel_regression.c` | Real SQLite stores across instances: deterministic overlap, parallel payload isolation, preparation failure and statement cleanup |
| `integration/storage_commit_regression.c` | Real two-bundle insert/COMMIT failures; observe custody refusal, no forwarding, and successful post-commit acceptance |
| `integration/storage_init_regression.c` | Real SQLite startup contention, bounded failure, and statement/handle cleanup |
| `integration/crc_regression.c` | Known CRC vectors and 712 independent bitwise comparisons; strict callback type checking |
| `integration/cbor_bounds_regression.c` | Actual canonical decoder: excess extension rejection and final valid extension/payload slots |
| `integration/custody_admission_regression.c` | Actual custody admission and indexes; duplicates under resource pressure, byte limits, pending commit protection and storage-mutex contention |
| `integration/custody_db_regression.c` | Actual custody indexes and red-black trees; allocation/duplicate rollback, sequence retry, counts, and mixed-order deletion |
| `integration/custody_regression.c` | Durable allocator failures and CCS mutex ownership |
| `integration/bpnode_ingress_regression.c` | Ingress task regression with framework stubs |
| `integration/video_sink_regression.c` | Sink regression with OSAL/decoder stubs |
| `integration/CMakeLists.txt` | Adapter object library, smoke/regression targets, conditional test registration |
| `client-test/secure_records.py` | Shared record codec, validation, atomic persistence, ground receipts |
| `client-test/bpnode_video_demo.py` | Synthetic frames or opaque file chunks; ordered reassembly |
| `client-test/continuous_video_demo.py` | FFmpeg segment creation and completion-driven UDP playback |
| `client-test/custody_video_demo.py` | Durable relay spool, custody ACKs, retransmission and replay |
| `client-test/segment_viewer.py` | Poll contiguous segment filenames; optional remux/player pipeline |
| `client-test/test_security.py` | Authentication/storage/CLI regression suite |
| `client-test/requirements.txt` | Python package requirements |
| `demo/five_node.py` | Native mission build, runtime preparation, input queue, process launch, live playback, and output verification |
| `demo/test_five_node.py` | Baseline transfer, custody drain, and optional security, storage, and endpoint restart scenarios |
| `demo/test_five_node_faults.py` | Link outage, relay restart, and retained duplicate replay scenarios |
| `demo/test_five_node_stream.py` | Streaming record encoding, growing playlist, and archive verification tests |
| `demo/test_node_health.py` | Host process and BPNode application readiness tests |
| `demo/test_security.py` | Endpoint wire security tests against the compiled BSL bridge |
| `scripts/bootstrap.sh` | Validate prerequisites, build dependencies and native targets, then run the selected checks |
| `scripts/package_source.py` | Export committed source and pinned submodules with a file hash manifest |
| `docs/check_documentation.py` | Check source inventory, public Python docstrings, and relative links |
| `cfs-buildconfig/targets.cmake` | Mission identity, CPU assignments, and application selection |
| `cfs-buildconfig/global_build_options.cmake` | Shared compiler diagnostics and optional sanitizer settings |
| `cfs-buildconfig/mission_build_custom.cmake`, `cfs-buildconfig/arch_build_custom.cmake` | Mission and architecture customization hooks |
| `cfs-buildconfig/install_custom.cmake`, `cfs-buildconfig/extra_install.cmake` | Mission install hooks and extra runtime files |
| `cfs-buildconfig/default_osconfig.cmake`, `cfs-buildconfig/native_osconfig.cmake` | OSAL defaults and native platform overrides |
| `cfs-buildconfig/cfe_es_startup.scr`, `cfs-buildconfig/cfe_perfids.h` | Application startup entries and performance ID assignments |
| `cfs-buildconfig/tables/sch_lab_table.c`, `cfs-buildconfig/tables/to_lab_sub.c` | Scheduler and telemetry subscription tables |
| `cfs-buildconfig/inc/ci_lab_topicids.h`, `cfs-buildconfig/inc/to_lab_topicids.h` | Command-ingest and telemetry-output topic IDs |

Local dependency additions include the block-access API in
`bplib/ci/mem/inc/bplib_mem.h` and `bplib/ci/mem/src/bplib_mem.c`, the BPNode TCP
contact changes above, and `bpnode/fsw/src/bpnode_video_sink.c`/`.h`.
`bpnode/CMakeLists.txt` includes the adapter and sink in the cFS application.
`cfs-buildconfig/` contains mission-specific headers and tables; it must agree
with contact addresses, destination routes, telemetry topic IDs, and the target's
file-system mapping. See [contact configuration](../integration/README.md) and
the [mixed-protocol example](../integration/bpnode-tcpclv4-example.md).

## Five-node streaming records

The controller queues application data units in `node1/input` as numbered
`.adu` files. Each record starts with a four-byte ASCII tag and the 16-byte
run session ID. The remaining integers are unsigned 32-bit values in network
byte order. These are demo payloads carried by BPv7, not BPv7 block formats.

| Tag | Fields after the session ID | Consumer |
| --- | --- | --- |
| `BPD1` | Segment index, part index, part count, total segment count, payload byte count, then payload bytes | Node 5 retains the authenticated parts and publishes a segment when all parts are present. Streaming records use zero for the unknown total. |
| `BPE2` | One-based video number, first segment index, segment count | Node 5 concatenates the published segment range into one replay archive. |
| `BPF1` | One-based video number | Node 1 advances `file-ack` after accepting the preceding records. This marker never goes on the wire. |
| `BPE1` | Final segment count, zero, zero, final segment count, zero | Node 5 validates the full segment set and publishes `output/complete`. An idle stream can close with zero segments. |

Run one watcher per run. Queue files and the manifest are replaced atomically
so live readers do not see partial writes. A `.sent` suffix means node 1
accepted the input; it does not establish BP custody or delivery to node 5.
Node 5 publishes authenticated output separately, and `verify` checks each
segment hash and archive byte range against the manifest and published segments.
Live playback serves the contiguous prefix of available segments through an HLS
event playlist on loopback. The playlist ends after the completion marker and
the last listed segment are both present. See the
[operator runbook](five-node-video-test.md#watch-a-folder) for startup and recovery.

## Transport lifetime, ownership, and errors

Keep a successfully initialized `BPLib_TCPCLA_t` at a stable address. It owns
its sockets, receive buffer, pending queue, and lock; do not copy it, clear it,
or initialize it again during that lifetime. Configuration/setup/handshake
require exclusive ownership before I/O workers start.

1. Initialize with `Init` (STCP defaults) or `InitConfig` (explicit protocol).
2. Connect actively, or listen then accept passively, or attach a connected socket.
3. For TCPCLv4, call `SessionStart` with the appropriate active/passive role.
   STCP starts transferring directly and must not call `SessionStart`.
4. Send/receive bundles, or use the queue bridge helpers.
5. Stop workers and call `Close` before reconnecting. `Destroy` releases the
   lock/Winsock lifetime after all users have stopped.

Successful `Attach` transfers socket ownership to the context; on failure the
caller retains it. Send/receive borrow the caller's buffer only for the call.
Do not change public context fields during I/O; they include internal state.

`TimeoutMs == 0` waits indefinitely. Send/receive/session I/O use monotonic
deadlines after beginning socket work. Interrupted polling recomputes the
remaining finite wait, including in connect/accept. Waiting for the send mutex and name
resolution are not bounded by that socket deadline; connect can try multiple
resolved addresses, each with its own wait. `Close` acquires the same mutex and
does not interrupt a blocked sender immediately.

BPNode TCP send/receive use `BPNODE_TCPCLA_IO_TIMEOUT_MSEC` (default 1000 ms),
independently of the shorter `BPNODE_DATA_TIMEOUT_MSEC` queue-poll budget. A peer
may dispatch acknowledgements on a later task wakeup. Missions should configure
TCP I/O timeouts for their scheduling and link latency.

Send calls serialize. Receive tries the lock and returns `BPLIB_CLA_TIMEOUT`
immediately if another operation owns it. TCPCLv4 send also dispatches inbound
segments while waiting for ACKs, queueing at most four completed bundles.
The bound prevents unlimited queue growth; overflow fails the session. Idle
receive timeout can preserve the socket; errors after partial framing close it
to prevent a later call from treating remaining bytes as a fresh frame.

| Status | Typical meaning |
| --- | --- |
| `BPLIB_SUCCESS` | Operation completed |
| `BPLIB_INVALID_CONFIG_ERR` | Invalid configuration or setup sequence |
| `BPLIB_NULL_PTR_ERROR` | Missing required pointer; send also uses it for zero length |
| `BPLIB_APP_STATE_ERR` | Socket unavailable or TCPCLv4 session not ready |
| `BPLIB_CLA_TIMEOUT` | Deadline elapsed or receive dispatcher busy |
| `BPLIB_BUF_LEN_ERROR` | Bundle/caller-buffer/MRU limit or pending-queue limit exceeded |
| `BPLIB_OS_ERROR` | Socket, allocation, or protocol processing failed in transport |
| `BPLIB_NO_STOR_ERR` | Queue bridge temporary buffer allocation failed |

Ingress/egress allocate one `MaxBundleSize` buffer per call and propagate queue
or transport status. Queue wait and socket transfer each receive `TimeoutMs`;
the helper's total duration can exceed it. An egress send failure does not
automatically requeue or retransmit the dequeued bundle. TCPCLv4 ACKs indicate
transport progress, not BP custody or durable ground receipt. The implementation
does not provide TLS, nonzero extensions, or an autonomous keepalive scheduler.

## Decoder bounds

BPLib stores at most `BPLIB_MAX_NUM_EXTENSION_BLOCKS` extension records. The
extra canonical slot is reserved for the payload. An extension at that index
is rejected with `BPLIB_CBOR_DEC_CANON_BLOCK_INDEX_ERR` before its metadata or
block-specific data is written. See [October 29 fixes](review-2026-10-29.md).

## Block and sink contracts

Block readers borrow a decoded BPLib bundle and its offset metadata. Keep the
bundle alive and unchanged throughout access. Readers need no close operation;
read until `remaining == 0`, or discard the reader to stop early.
Blob reads validate the complete requested range against each memory block's
`used_len` before copying. Out-of-range reads fail without modifying output;
a zero-byte read at the exact end of the blob succeeds. Allocation slack is
never part of the readable blob.
Writers allocate a staging buffer and accept only the original BTSD length.
After a successful open, call close exactly once: `false` discards staging;
`true` requires every byte and copies into the existing blob, marking it for
re-encoding. These APIs do not create/remove blocks, resize BTSD, or synchronize
concurrent mutations. Their offsets must refer to a valid decoded blob.

The video sink borrows encoded input bytes, allocates/decodes a temporary bundle
in the supplied instance pool, and releases it before returning. A mutex
serializes writes and close; open seeks to the existing end rather than
truncating. Errors can leave an appended payload prefix; there is no rollback,
frame decoding, decryption, reordering, or durable custody acknowledgement.
Disabled builds return success without writing. The enabled sink currently uses
pthreads in addition to OSAL file APIs. Rotate recordings while contacts are stopped.

## Storage startup and deployment health

SQLite connections use `sqlite3_busy_timeout` with a default 1000 ms contention
budget, configurable at compile time with `BPLIB_SQL_BUSY_TIMEOUT_MSEC`. This
allows transient read-only monitoring locks to clear during WAL/schema setup.
Batch storage preserves the original insert/COMMIT failure even when rollback
succeeds. Rolled-back byte/duplicate/custody counts are cleared, and the storage
finalizer refuses custody and frees the failed batch without forwarding it.
Successful custody acceptance and enqueueing occur only after commit.

Initialization errors include SQLite diagnostics and release the failed handle;
initialization count/size queries finalize statements on failed steps as well as
success.

Cleanup and update batches preserve the first SQL failure and roll back all
partial changes. Byte/egress accounting and custody-record removal follow
successful commit. Garbage collection preserves its first failure rather than
replacing it with a later successful size query. Cleanup stages custody IDs in a bounded temporary heap
allocation rather than on a task stack; failure preserves the original state.
The compatibility custody enqueue API commits synchronously under the storage
mutex; success means durable SQL commit and failure preserves caller ownership.
New storage records use version 2: complete encoded wire chunks and explicit
SQL origin metadata, with no serialized C structs or pointers. Loading rebuilds
owned objects with the real CBOR decoder and CRC checks. Legacy admin pointers
are ignored and rebuilt from wire; absent legacy origin metadata uses safe
unknown defaults. See the [completion report](storage-completion-2026-10-29.md)
for compatibility, memory/throughput costs, crash tests, and current validation.

The five-node controllers wait for the current BPNode process's recovery/ready
marker before reading live custody databases. `demo/five_node.py` provides
`check_nodes` and `wait_for_nodes`; the checks catch an application initialization
failure even when the cFS host stays alive. Restart checks ignore old log bytes.
`demo/test_node_health.py` covers these health and log-generation boundaries.
Successful transfer tests require custody to stay drained for six seconds; the
outage tests exclude the intentionally stopped source from that check.

## Python record and persistence contracts

`Codec.encode` CBOR-encodes the complete map and returns `12-byte random nonce ||
AES-GCM ciphertext-and-tag`, with fixed associated data `bpnode-video-demo-v2`.
Decode checks a 28..65507-byte envelope, authentication, map type, and optional
session binding. Authentication failure raises `InvalidTag`; invalid size/type/
session raises `ValueError`. Map schemas are validated by the consumers.
`load_key` creates/reuses exactly 32 raw key bytes or generates an ephemeral key.
POSIX creation uses mode 0600; Windows privacy depends on the directory ACL.

| Demo | Authenticated map fields |
| --- | --- |
| Simple data | `bp`, `src`, `dst`, `seq`, `total`, `payload`, `session` |
| Continuous data | `bp`, `src`, `dst`, `segment`, `part`, `count`, `payload`, `session` |
| Simple EOF | `eof: true`, `session` |
| Continuous EOF | `eof: true`, `segments` (nonnegative integer), `session` |
| Custody data | `kind: bundle`, 16-byte `session`, `segment`, `part`, `count`, `seq`, `payload` |
| Custody ACK | `kind: custody_ack` or `ground_ack`, `session`, `seq` |
| Custody end | `kind: eof` or `eof_ack`, `session` |

Custody sequence is `(segment << 32) | part`. Segment is unsigned 32-bit; part
is 0..65535; count is 1..65536; payload is bytes. The filename identity is
`session.hex()-seq`, validated before file operations. `RelayStore` authenticates
and validates every spool record on startup and verifies its filename.
Invalid or old records fail startup; they are not migrated.

Custody ordering is: persist relay record -> ACK source -> forward/retry ->
persist ground receipt -> ACK relay -> delete/synchronize relay record.
Ground publishes `ground-NNNNNN.ts` after all parts of that segment are present
and consistent. It retains encrypted receipts under `.custody/SESSION/SEGMENT/`
for duplicates and replay. Output names contain no session ID: use a distinct
output directory per recording to avoid overwriting an earlier recording.

`durable_write` creates a temporary file in the destination directory, flushes
and fsyncs it, then atomically replaces the destination. POSIX also fsyncs the
directory; Windows skips directory fsync. No hardware power-loss guarantee was
tested. `GroundStore.accept` expects `message` to be the decoded form of `blob`;
it validates and persists, but the caller must authenticate before calling it.

Simple ground reorders chunks by sequence, including live contiguous writes.
Continuous ground assembles parts in order but publishes/plays **segments in
completion order**; it has no retransmission. Both fail incomplete transfers at
EOF. Continuous EOF declares the exact segment count; ground requires all IDs
from zero through that count minus one and no partial segments. An empty segment
uses one empty part. Zero segments is a valid protocol-level empty transfer;
the CLI still requires at least one input file. Old continuous EOF records
without the count are rejected. Custody retries source exchanges up to 20 times with 0.5-second ACK windows;
the relay resends pending records approximately every 0.2 seconds until stopped.
The viewer waits for contiguous filenames starting at zero and does not infer EOF.

## Maintaining coverage

Run `python3 docs/check_documentation.py` after adding/changing local sources.
The checker verifies the source inventory, Python module/public callable
docstrings, and relative Markdown links in local guides. It parses Python without
importing third-party packages. A pass establishes those structural checks;
it cannot prove that prose is accurate or every dependency is fully documented.
Update API contracts and this guide whenever behavior changes, inspect CLI help,
and run relevant regressions using [build and validation](build-and-test.md).

The [CRC follow-up](crc-review-2026-10-29.md) removes incompatible digest callback
casts and records the subsequent 27-entry suite and focused native validation.

The [custody-signal review](ccs-review-2026-10-30.md) documents integer-label
validation, full-width sequence bounds, the per-collection mission work limit,
and the associated parser/SQLite regressions in the existing storage test files.

The [sender follow-up](ccs-sender-review-2026-10-30.md) documents outgoing span
limits, safe sequence exhaustion, durable duplicate replay, and idempotent
ground publication. The optional fault-controller flag
`--replay-retained-duplicates` exercises original stored IDs with ADU feeding
gated and acknowledgement release controlled by a demo-only file gate.

The [publication durability follow-up](publication-durability-review-2026-10-30.md)
requires directory sync on matching replay records and adds real post-rename
error injection to the existing delivery regression.

The [completion-validation follow-up](completion-validation-review-2026-10-30.md)
shares segment validation between data and EOF and requires retained receipts
for completion. The helper reuses the existing scratch buffer.

The [file-read review](file-read-error-review-2026-10-30.md) requires a clean EOF
probe before successful file loads and records stdio error/close coverage.

The [custody-index review](custody-index-review-2026-10-30.md) documents staged
index/count/state publication, failed insertion cleanup, and the new real-tree
regression. The caller continues to own CT locking and custody acceptance counts.

The [admission review](custody-admission-review-2026-10-30.md) documents duplicate
precedence, overflow-safe byte checks, pending batch reservations and rejection
cleanup outside the storage mutex. It adds the admission regression to the inventory.

The [concurrent-storage review](storage-parallel-review-2026-10-30.md) replaces
process-global prepared-statement ownership with a context owned by each store
invocation. It documents private helper changes and the new parallel regression.

The [SQLite binding review](sqlite-bindings-review-2026-10-31.md) releases borrowed
wire-buffer parameters before return and documents error/cleanup guarantees in
the private chunk writer. The new direct regression uses real SQLite bindings.

The [SQL completeness review](storage-completeness-review-2026-10-31.md) requires
one metadata row, recovery row and each blob row before success, and performs
transactional duplicate lookup before writing. Existing transaction and parallel
fixtures now exercise suppressed writes, unrelated UNIQUE failures and lookup
preparation failures without adding source files.

The [custody SQL review](custody-transactions-review-2026-10-31.md) removes shared
statement/query state from custody operations and makes filtered retransmission
scheduling transactional. Its regression checks overlap, rollback and ownership;
SQL loading/maintenance helpers remain a separate concurrency review item.

The [SQL ownership review](sql-ownership-review-2026-10-31.md) closes the previously
listed shared-statement work in loading and maintenance helpers. Calls own prepared
statements and generated selection queries; each instance still requires storage
serialization. The new regression checks 25 deterministic overlap/failure rounds.

The [database-size review](db-size-review-2026-11-01.md) expands the existing SQL
regression with real-file geometry, failed query execution/preparation, null-input
and cleanup checks. Size telemetry uses actual page geometry and fails closed.

The [selection rollback review](selection-review-2026-11-01.md) adds six cases
for interrupted queries and exhausted append capacity. Failed selections preserve
the published batch bounds and existing prefix, allowing a clean retry.

The [egress retry review](egress-retry-review-2026-11-01.md) expands
`integration/storage_error_regression.c` with actual storage-caller retries after
SQL marking failures. Consumed batches remain intact until marking succeeds; tests
check local/remote rollback, lock release and no repeated loading/queueing.

The [startup accounting review](accounting-review-2026-11-01.md) expands
`integration/storage_init_regression.c` with persisted-field validation, failed
recovery publication and retry cases. Counters publish from one validated snapshot
after successful query completion and cleanup.

The [scalar accounting review](scalar-accounting-review-2026-11-01.md) expands
`integration/storage_init_regression.c` with direct getter validation, completion
errors, count boundaries and output preservation. Exported raw SQL text stays
compatible; the validated helpers use private immutable queries.

Publication tooling lives in `scripts/bootstrap.sh` (dependency checks, native build
and integration/native tests), `scripts/package_source.py` (committed recursive
source export) and `.github/workflows/ci.yml` (clean Ubuntu validation/artifacts).
See [quick start](quickstart.md) and [publishing](publishing.md).
