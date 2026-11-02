# BPSec + BPLib integration smoke project

This adapter compiles against NASA AMMOS BSL (BPSec) and BPLib and provides
`bplib_bsl_adapter.c`, which translates BPLib primary/payload metadata into the
BSL host callback model. It also round-trips an RFC-9171 CBOR bundle through
BSL's BPA-neutral Mock BPA codec, which is the wire-level bridge needed when
BPLib's internal BTSD is not exposed. The smoke test exercises both paths and
BSL's payload data API. BPLib itself is built and tested in its own CMake target;
its OSAL BSP archive contains a `main()` entry point, so embedding that archive
requires the host BPA's OSAL startup wiring.

BPLib now exposes block readers and same-length staged writers in
`bplib_mem.h`. The BSL adapter still supplies only metadata and a Mock BPA wire
round trip; it does not register a complete BSL security service or apply/verify
BIB/BCB blocks in BPNode. Completing end-to-end BPSec processing requires wiring
those callbacks and supporting security-block creation/removal and resizing.

The adapter is intentionally narrow: BSL does not consume BPLib's internal
bundle object directly. A production BPA integration should implement the BSL
Service API callbacks by translating BPLib extension-block/payload views into
`BSL_Data_t` views, then write the resulting security blocks back through
BPLib's extension-block API.

## TCP CLA

`bplib_tcp_cla.h` and its companion sources provide a host-side TCP
convergence-layer adapter for BPLib. In STCP mode, each complete RFC-9171 bundle
is sent as a four-byte network-order length followed by the bundle bytes. This is the
STCP framing used by Hardy's interop `mtcp-cla`; it is not TCPCLv4 and does not
implement TCPCL session negotiation, TLS, custody signalling, or retransmission.
TCPCLv4 mode adds session negotiation, segmentation, and acknowledgements.

The adapter supports client connect, passive listen/accept, exact-length
send/receive, maximum bundle-size enforcement, and poll-based timeouts. The
`BPLib_TCPCLA_Egress()` and `BPLib_TCPCLA_Ingress()` helpers connect it to
BPLib's public CLA queue APIs. A production BPNode task should call those
helpers from its CLA task loop and use the contact table to select the contact
ID and peer address.

TCPCLv4 uses one serialized dispatcher per connection. While sending, it handles
incoming segments and queues up to four completed bundles while awaiting ACKs.
Concurrent receive calls return `BPLIB_CLA_TIMEOUT` when the dispatcher is busy.
Partial-transfer failures close the session; an idle receive timeout preserves
it. Call `Close` before reconnecting and `Destroy` after all callers have stopped
to end a context's lifetime. Setup/handshake must precede concurrent I/O. Sockets
are nonblocking, and each I/O operation uses a monotonic deadline. TCPCLv4 here
does not implement TLS or nonzero session/transfer extensions.

## BPNode CLA/task integration

BPNode now drives TCPCLv4 contacts from its existing CLA child tasks. The
TCPCL context is stored per contact and shared by that contact's ingress and
egress tasks. Contact setup initializes the adapter; contact start establishes
the stream and performs the TCPCLv4 handshake; the input task receives bundles
into BPLib and the output task drains BPLib onto the stream; contact stop closes
the stream.

BPNode uses `BPNODE_TCPCLA_IO_TIMEOUT_MSEC` (default 1000 ms) for TCP frame/ACK
I/O, independently of its short queue-poll timeout. Missions can override this
finite budget for their scheduling and link latency; TCP work may span wakeups.

For TCP contacts, configure either an outbound or inbound address:

* `ClaOutAddr`/`ClaOutPort` set: active contact; BPNode connects to that peer.
* `ClaInAddr`/`ClaInPort` set with no outbound address: passive contact; BPNode
  listens and accepts a peer.

This lets a relay use one passive TCP contact for moon ingress and a second
active TCP contact for ground egress. Override `BPNODE_TCPCLA_NODE_ID` in each
mission configuration so every BPNode has its own node ID. The default contact
table remains UDP/SB-oriented; a mission-specific contacts table must set
`CLAType = BPLib_TCP_CLA` and the TCP addresses/ports.

The stream protocol is independently selectable per TCP contact with
`BPNODE_TCPCLA_PROTOCOL(ContactId)`. It defaults to TCPCLv4. A relay using
Hardy's STCP `mtcp-cla` for ground can use this mission override:

```c
#define BPNODE_TCPCLA_PROTOCOL(ContactId) \\
    ((ContactId) == 1 ? BPLib_TCPCLA_STCP : BPLib_TCPCLA_TCPCLV4)
```

Contact 0 then remains TCPCLv4 for the moon, while contact 1 uses the
four-byte length-prefixed STCP stream for Hardy. STCP contacts connect or
listen normally but do not perform TCPCL session negotiation.

## MOC video sink

BPNode includes an opt-in MOC sink in `bpnode_video_sink.c`. Define
`BPNODE_VIDEO_SINK_ENABLE` in the MOC mission configuration to decode each
received BPv7 bundle, read its payload block through BPLib, and append the
payload bytes to `BPNODE_VIDEO_SINK_PATH` (default `/cf/moc-video.bin`). Keep
the option disabled on relay and flight nodes. The sink uses OSAL file APIs,
so the output path must be writable by the target's file-system mapping.
The sink appends across contact restarts and serializes complete bundle writes.
To begin a new recording, stop the contacts and explicitly rotate the output file.

The sink is intentionally payload-focused: it preserves bundle arrival order
and emits the normal BPNode counters/events for COSMOS. COSMOS can display
those transfer metrics and record telemetry; a player such as `ffplay` or VLC
should open the completed MPEG-TS/fragmented-MP4 output separately.

## Build and test

Build BSL and BPLib first using [build and validation](../docs/build-and-test.md), then:

```sh
cmake -S integration -B integration/build \
  -DBSL_BUILD="$PWD/BSL/build/default" -DBPLIB_BUILD="$PWD/bplib/build"
cmake --build integration/build
ctest --test-dir integration/build --output-on-failure
```

The chain smoke tests simulate every node using this adapter over local socket
pairs. Names mentioning Hardy/HDTN describe intended topology; they are not
interop tests against those external implementations. Python security tests need
`client-test/requirements.txt`. BPNode ingress/sink regressions additionally use
headers from a configured cFS build (`-DCFS_BUILD=/path/to/cfs-build`). They compile
the actual task/sink sources and stub framework/decoder boundaries.

The [2026-10-29 fix record](../docs/review-2026-10-29.md) documents pool
initialization bounds, populated-blob read limits, and excess-extension decoder
rejection. The integration suite includes regressions for all three fixes.

The [sender/replay follow-up](../docs/ccs-sender-review-2026-10-30.md) documents
CCS span splitting and sequence exhaustion. `storage_ccs_sender_regression`
checks real encoded/decoded outgoing signals; `demo_delivery_regression` executes
the actual filesystem delivery code, including immutable replay and conflicting
output rejection. These checks require the configured cFS headers above.
