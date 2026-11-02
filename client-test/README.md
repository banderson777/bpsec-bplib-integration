# Three-node lunar video client test

## Setup and scope

From the workspace root, install packages into your Python 3 environment:

```sh
python3 -m pip install -r client-test/requirements.txt
python3 client-test/test_security.py
```

Use `python` instead of `python3` if that is your Python 3 executable on Windows.
FFmpeg is needed for segment generation and persistent viewer remuxing; ffplay
is optional for headless transfers. Participants run as threads in one process
on loopback. These CBOR maps are a demo format, not BPv7/BPSec wire bundles;
the scripts do not launch BPNode. See [code guide](../docs/code-guide.md).

| Script | Default relay / ground UDP ports | Output |
| --- | --- | --- |
| `bpnode_video_demo.py` | 14556 / 24556 | Console, optional `--output-video` |
| `continuous_video_demo.py` | 14560 / 24560 | `--output-dir` (`ground-segments`) and optional player |
| `custody_video_demo.py` | 14570 / 24570 | `--output-dir` (`ground-segments`), durable `--spool` (`relay-spool-v2`) |

Pass `--help` to each script for all options. Use separate segment/output
directories per recording: matching names can be replaced, and generation may
leave stale trailing segments from a longer previous input. Keep custody keys
with their spool and ground receipts.

Run:

```sh
python3 client-test/bpnode_video_demo.py --frames 5 --frame-bytes 256
```

To transmit an actual video file, choose a bundle chunk size (900 bytes keeps
UDP datagrams comfortably below the MTU), then reassemble it at ground:

```sh
python3 client-test/bpnode_video_demo.py \
  --input-video moon-8s.mp4 --output-video ground-received.mp4 \
  --frame-bytes 900
ffplay ground-received.mp4       # or open it with VLC/mpv
```

For progressive output, use a stream-friendly container such as MPEG-TS (or a
fragmented MP4 produced with `-movflags +frag_keyframe+empty_moov`) and enable
live writes:

```sh
python3 client-test/bpnode_video_demo.py --input-video moon-8s.ts \
  --output-video ground-live.ts --frame-bytes 900 --live-output
ffplay ground-live.ts
```

`--live-output` decrypts, reorders, writes, and flushes each contiguous chunk as
it arrives. A conventional MP4 may still wait for its final index; use MPEG-TS,
fragmented MP4, or send independently decodable JPEG frames for true live
display.

The continuous demo preserves timestamps across MPEG-TS segments so one
persistent player can consume them. Older segments generated with reset
timestamps should be preserved separately; use fresh segment/output directories
for another run.

The input is treated as an opaque video byte stream; encryption and
authentication happen per bundle chunk, and the ground node writes chunks in
sequence order before playback. The simulator does not transcode or decode
video on the relay.

## Continuous live feed

For a continuous source, use the segment-aware pipeline. It creates one-second
keyframe-aligned MPEG-TS segments, encrypts each segment's bundles, and feeds
completed segments directly to `ffplay` at ground. Parts are reordered within
each segment; segments are published/played in completion order. This variant
does not retry lost UDP packets:

```sh
python3 client-test/continuous_video_demo.py \
  --input-video moon-8s.mp4 --player ffplay --chunk-bytes 900
```

The authenticated EOF declares the expected segment count. Ground fails the run
if any segment or part is missing, including when an entire segment was lost.
Already published segments remain on disk after a failed transfer. Empty input
segments produce empty output files. Continuous sender/receiver versions must
both support the `segments` EOF field; older EOF records are rejected.

Omit `--player ffplay` to run headless; decrypted segments are saved under
`ground-segments/`. All demo participants authenticate the complete demo record.
A small ground jitter buffer can
be added by delaying the `proc.stdin.write()` call until several complete
segments are queued. For a real BPNode deployment, replace the UDP send/receive
functions with BPLib payload-agent/CLA calls while retaining segment metadata,
sequence ordering, custody/retransmission, and BPSec processing.

## Custody-transfer simulation

Run the custody-aware variant against pre-segmented MPEG-TS files:

```sh
python3 client-test/custody_video_demo.py \
  --segment-dir moon-segments --spool relay-spool-v2 --chunk-bytes 900
```

The relay writes each encrypted bundle to `relay-spool-v2/` before sending a
custody acknowledgement to the moon. It retains the file until the ground node
acknowledges receipt, then deletes it. Any `.cbor` files already in the spool
are replayed when the relay starts, simulating restart recovery.

The v2 demo format authenticates payload, metadata, EOF, and acknowledgements as
one AES-GCM record, using fresh random nonces. The simple/continuous demos share
a random key and session ID for each run; `--key-file` optionally retains a key.
The custody demo retains its key at `SPOOL/demo.key` (created with private POSIX
permissions) so it can replay stored records after restart. Preserve that key
with the spool. These are trusted-participant simulations: the relay also has
the demo key. This is not the BPSec wire format or a security-transparent relay.

Old `.cbor` files use an incompatible, unauthenticated format and are rejected;
use the new spool directory without deleting the old artifacts. Ground keeps
durable per-session chunk receipts in `OUTPUT/.custody/`, acknowledges only
after persistence, and publishes segments only after every part is present.
Duplicate packets are idempotent; unacknowledged relay records are retried.
Receipts are retained for recovery and deduplication; archive them with the
recording after the corresponding relay spool has drained. `--output-dir`
selects the destination. A storage failure makes the run fail rather than
reporting successful custody.

## Start the viewer first

The viewer can wait indefinitely for the moon feed. Start it in one terminal:

```sh
python3 client-test/segment_viewer.py --dir ground-segments
```

To move each segment after it is displayed:

```sh
python3 client-test/segment_viewer.py \
  --dir ground-segments --archive-dir displayed-segments
```

Then start the ground/relay pipeline in another terminal. The viewer displays
contiguous segments as `ground-000000.ts`,
`ground-000001.ts`, etc. If `ffplay` is unavailable, install FFmpeg or pass a
compatible player executable with `--player`.

By default the viewer launches `ffplay` separately for each segment. With
`--persistent`, it inserts a long-lived FFmpeg remuxer before `ffplay` to
normalize MPEG-TS continuity and discard malformed boundary packets, keeping a
single player window open. It buffers three segments by default; pass
`--buffer-segments 1` for short recordings. The viewer waits for new filenames
and has no EOF signal, so stop it explicitly after playback.

The source (“moon”) generates deterministic video frames, encrypts each
record with AES-GCM, and sends authenticated CBOR demo records over UDP. The relay
authenticates records before forwarding them. The ground node authenticates/decrypts
each payload and displays a console luminance bar and checksum.

This is a standalone client test for the BPNode UDP convergence-layer shape.
To run with real BPNode instances, configure three BPNode/cFS targets with UDP
ports `14556` (moon→relay) and `24556` (relay→ground), and replace the source
and sink application sockets with BPNode payload-agent calls. The BSL adapter
belongs at the BPNode ingress/egress security callbacks; the relay remains
security-transparent.
