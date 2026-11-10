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

## Run the five-node mode in separate terminal sessions

The optional five-node lab is run as five independent native Linux processes, one
for each node. Use a dedicated terminal for each node and keep them all in the same
WSL2 Linux environment. Keep the checkout in WSL's Linux filesystem (for example,
`$HOME/src/bpsec-bplib-integration`); CMake's many-file mission build can be very
slow from a Windows-mounted OneDrive path such as `/mnt/c/Users/...`.

```bash
set -e

# In every terminal, use the same workspace and run directory
export BPSEC_WORKSPACE="/mnt/c/Users/bande/OneDrive/Documents/source/bpsec-bplib-integration"
export BPSEC_BUILD="$BPSEC_WORKSPACE/build/native"
export BPSEC_RUN="$BPSEC_WORKSPACE/build/runs/five-node-demo-1"
export BPSEC_SEGMENTS="$HOME/bpsec-video-segments"
cd "$BPSEC_WORKSPACE"
```

Build the mission once and wait for the command to finish successfully before
continuing. Do not interrupt CMake; configuration and compilation can take a while.
If repeating a lab, change the `five-node-demo-1` suffix in `BPSEC_RUN` to an unused
name and use that same path in every terminal.

```bash
set -e
python3 demo/five_node.py build --build-dir "$BPSEC_BUILD"
```

Prepare the video segments and run directory only after the build succeeds. The
`prepare` command creates `BPSEC_RUN`, so do not create that directory yourself.

```bash
set -e
mkdir -p "$BPSEC_SEGMENTS"
ffmpeg -hide_banner -y -f lavfi -i testsrc2=size=320x240:rate=10 \
  -t 12 -an -c:v mpeg2video -g 10 -f segment -segment_time 2 \
  "$BPSEC_SEGMENTS/segment-%06d.ts"
python3 demo/five_node.py prepare \
  --build-dir "$BPSEC_BUILD" \
  --run-dir "$BPSEC_RUN" \
  --segment-dir "$BPSEC_SEGMENTS"
```

Start nodes 5, 4, 3, and 2 in separate terminals. Each node uses its own isolated
runtime directory under `$BPSEC_RUN/nodeN` and the same `--port-base` for the full
lab. In a sixth terminal, start the live viewer after node 5 is running; it waits
for the first authenticated segment. Then start node 1 to begin the transfer. The
viewer opens with `ffplay` and plays each complete segment as node 5 publishes it.

```bash
# Terminal 5: MOC / sink / decrypt
python3 demo/five_node.py node --run-dir "$BPSEC_RUN" --node 5 --port-base 34000

# Terminal 4: relay / cloud node
python3 demo/five_node.py node --run-dir "$BPSEC_RUN" --node 4 --port-base 34000

# Terminal 3: relay / ground node
python3 demo/five_node.py node --run-dir "$BPSEC_RUN" --node 3 --port-base 34000

# Terminal 2: relay / relay node
python3 demo/five_node.py node --run-dir "$BPSEC_RUN" --node 2 --port-base 34000

# Terminal 6: live video viewer (start before node 1)
python3 demo/five_node.py view --run-dir "$BPSEC_RUN"

# Terminal 1: source / encrypt node
python3 demo/five_node.py node --run-dir "$BPSEC_RUN" --node 1 --port-base 34000
```

### Watch a folder for video files

For continuous intake, create a fresh streaming run instead of using `prepare`.
The watcher waits until files stop changing, orders them by oldest modification
time (filename breaks ties), transcodes each to MPEG-TS, and queues its bundles
to node 1. After node 1 accepts all bundles for a file, the watcher renames it
from `name.mp4` to `name.mp4.sent`. See the
[five-node video guide](docs/five-node-video-test.md#watch-a-folder)
for the full seven-terminal startup and shutdown sequence. Node 5 automatically
plays received segments in FIFO order and saves one replayable TS file per video
under `$BPSEC_RUN/node5/output/videos/`; the run manifest maps archive numbers
to input names.

```bash
export BPSEC_RUN="$BPSEC_WORKSPACE/build/runs/five-node-watch-1"
export BPSEC_WATCH="/home/banderson777/incoming-video"
mkdir -p "$BPSEC_WATCH"
python3 demo/five_node.py prepare-stream --build-dir "$BPSEC_BUILD" --run-dir "$BPSEC_RUN"
```

Start nodes 5, 4, 3, and 2, then the viewer in its own terminal. Start node 1,
then run the watcher in a seventh terminal. Keep both long-running commands in
separate terminals:

```bash
# Terminal 6: live playback; waits for node 5 and then plays the growing queue
python3 demo/five_node.py view --run-dir "$BPSEC_RUN" --startup-timeout 0
```

```bash
# Terminal 7: continuously poll for completed MP4 files
python3 demo/five_node.py watch --run-dir "$BPSEC_RUN" \
  --watch-dir "$BPSEC_WATCH" --pattern '*.mp4' \
  --poll-interval 1 --stable-seconds 2 --segment-duration 2
```

The watcher stays active until Ctrl+C. Stop it while it is waiting between files;
it queues the end marker so node 5 can finish and the viewer can close. Use a new
run directory for each streaming session.

If you are running the automated deterministic transfer instead of a video clip,
use the single-controller command below from a normal terminal:

```bash
python3 demo/test_five_node.py --build-dir "$BPSEC_BUILD" --run-dir "$BPSEC_RUN" --timeout 90
```

This command launches the same five node processes internally as a controller,
verifies the transfer byte-for-byte, waits for custody to drain, and preserves
logs and runtime state under the selected run directory. The manual five-terminal
flow is intended when you want to observe each node in its own terminal window.

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
