# NASA BPSec (BSL) + BPLib integration

This workspace combines NASA AMMOS BSL, NASA BPLib, and the cFS BPNode app
with a host TCP convergence-layer adapter and Python video simulations.
The optional five-node lab adds BSL payload security to BPNode at its endpoints,
with keyless relays and isolated cFS runtimes. Its automated five-process
transfer, custody drain, link/relay recovery, endpoint restart, security rejection,
storage-failure, and IV-restart checks pass on native Linux. The separate Python simulations
use their own authenticated record format and shared keys.

## Download and run

See the [beginner quick start](docs/quickstart.md) for Ubuntu 24.04 / Windows WSL2.
Clone with `--recurse-submodules`, then run `bash scripts/bootstrap.sh --native-test`.
The [publication guide](docs/publishing.md) explains how all component changes are
saved to GitHub and how complete-source release archives include their dependencies.

## Start here

- [Code guide](docs/code-guide.md): architecture, source inventory, ownership,
  failure behavior, demo formats, and documentation maintenance.
- [Build and validation](docs/build-and-test.md): dependency setup, CMake options,
  test coverage, and troubleshooting.
- [Integration guide](integration/README.md): TCP contacts and the optional video sink.
- [Video demos](client-test/README.md): setup, commands, playback, and recovery.
- [Five-node video test](docs/five-node-video-test.md): endpoint-only BPSec,
  hop custody, build steps, five Linux or Windows/WSL terminal sessions, and
  verified transfer, fault-injection, and restart results.
- [Windows host notes](docs/windows-build.md): platform requirements and limits.
- [Prior security validation](docs/security-fixes-validation.md): recorded Linux
  results and their limitations; these are historical results, not a fresh test run.
- [Scalar accounting validation](docs/scalar-accounting-review-2026-11-01.md): reject invalid or oversized query results and preserve outputs on failure.
- [Startup accounting validation](docs/accounting-review-2026-11-01.md): reject invalid persisted accounting and restore counters together.
- [Egress marking retries](docs/egress-retry-review-2026-11-01.md): retain consumed batches until database marking succeeds.
- [Failed egress-selection rollback](docs/selection-review-2026-11-01.md): preserve batch contents and reader position when selection fails.
- [Database-size telemetry](docs/db-size-review-2026-11-01.md): use actual SQLite page sizes and reject failed size queries.
- [SQL loading and maintenance ownership](docs/sql-ownership-review-2026-10-31.md): isolate selection, egress, discard and count statements across instances.
- [Custody SQL ownership and transactions](docs/custody-transactions-review-2026-10-31.md): isolate concurrent instances and roll back failed scheduling updates.
- [Complete SQL bundle writes](docs/storage-completeness-review-2026-10-31.md): reject partial writes and distinguish genuine duplicates from other constraint failures.
- [SQLite blob-binding lifetime](docs/sqlite-bindings-review-2026-10-31.md): release borrowed buffers and preserve binding/step errors.
- [Concurrent SQLite writers](docs/storage-parallel-review-2026-10-30.md): isolate prepared statements between storage instances and verify failure cleanup.
- [Custody admission under pressure](docs/custody-admission-review-2026-10-30.md): preserve duplicate custody, reject wrapped byte counts, and release rejected pending entries.
- [Custody index rollback](docs/custody-index-review-2026-10-30.md): failed insertion cleanup, state preservation, and real-tree regression coverage.
- [File-read error handling](docs/file-read-error-review-2026-10-30.md): reject EOF-probe I/O errors and verify stream cleanup.
- [Completion integrity validation](docs/completion-validation-review-2026-10-30.md): EOF validates output against retained receipts and rejects damaged files.
- [Publication durability on replay](docs/publication-durability-review-2026-10-30.md): directory-sync failure recovery and fault-injection evidence.
- [Custody-signal sender and duplicate recovery](docs/ccs-sender-review-2026-10-30.md): bounded outgoing signals, sequence rollover, and restart evidence.
- [Custody-signal validation](docs/ccs-review-2026-10-30.md): disposition-code checks, bounded sequence processing, and current test evidence.
- [CRC portability fix](docs/crc-review-2026-10-29.md): typed digest callbacks and independent checksum regressions.
- [Storage recovery completion](docs/storage-completion-2026-10-29.md): portable recovery, origin metadata, durable custody updates, and current validation.
- [Storage transaction follow-up](docs/storage-review-2026-10-29.md): cleanup, egress, custody queue retry, and recovery metadata fixes with real SQLite regressions.
- [Deployment validation](docs/deployment-validation-2026-10-29.md): fresh native build, 11 recovery/security scenarios, and deployment fixes.
- [Code review and fixes](docs/review-2026-10-29.md): bounds, transport, SQLite startup, and durable custody fixes with regressions.
- [Follow-up review](docs/review-2026-10-06.md): two findings, reproduction
  evidence, and fixes completed on 2026-10-07.

## Folder map

| Folder | Contents and documentation |
| --- | --- |
| `integration/` | Local C adapter, socket tests, regression tests, and CMake project |
| `client-test/` | Python source/relay/ground simulations and authentication tests |
| `bplib/` | Bundle Protocol library; [README](bplib/README.md), [API guide instructions](bplib/docs/README.md) |
| `bpnode/` | cFS application and mission configuration; [README](bpnode/README.md) |
| `BSL/` | BPSec library and Mock BPA; [README](BSL/README.md) |
| `BSL-docs/` | BSL product/user guide sources; [README](BSL-docs/README.md) |
| `cfs-bundle/`, `cfs-buildconfig/` | cFS sources and local mission build configuration |
| `external/` | External implementations used as topology references; see their own READMEs |
| `cfs-build/`, nested build/staging folders | Generated output tied to its original build environment |

Root video files, segment folders, relay spool files, and `bplib-storage.db*`
are runtime artifacts. Preserve recordings and custody state when preparing
another run. Use separate output directories to avoid mixing sessions.

The local guides document integration behavior and local additions to BPLib
and BPNode. Dependency internals retain their upstream documentation. This
workspace is not a certification of every bundled dependency or flight target.
