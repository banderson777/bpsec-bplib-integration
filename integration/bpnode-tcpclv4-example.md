# BPNode mixed TCPCLv4/STCP three-node example

Use three BPNode/cFS instances with these roles and TCP contacts:

| Node | Contact | Role | Address | Port | Node ID |
|---|---:|---|---|---:|---|
| Moon | 0 | active connector to relay | relay host | 4601 | `ipn:10.0` |
| Relay | 0 | passive listener from moon | `0.0.0.0` | 4601 | `ipn:20.0` |
| Relay | 1 | active connector to ground | ground host | 4602 | `ipn:20.0` |
| Ground/Hardy | — | passive STCP listener from relay | `0.0.0.0` | 4602 | — |

For an active contact, set `CLAType = BPLib_TCP_CLA`, `ClaOutAddr`, and
`ClaOutPort`; leave `ClaInAddr` empty. For a passive contact, set `CLAType`,
`ClaInAddr`, and `ClaInPort`; leave `ClaOutAddr` empty. The BPNode integration
selects the role from whether `ClaOutAddr` is populated.

Use this relay mission override to keep the moon link on TCPCLv4 and use
Hardy's four-byte length-prefixed STCP stream on contact 1:

```c
#define BPNODE_TCPCLA_PROTOCOL(ContactId) \\
    ((ContactId) == 1 ? BPLib_TCPCLA_STCP : BPLib_TCPCLA_TCPCLV4)
```

The relay therefore needs two contacts. Route moon-destined bundles into
relay contact 0 and ground-destined bundles into relay contact 1 using the
normal BPLib destination EID patterns. The relay does not decrypt or rewrite
the bundle while forwarding it. The current metadata adapter does not perform
BPSec processing; production ingress/egress security callbacks still need wiring.

Hardy must be listening with its STCP `mtcp-cla` on port 4602. No TCPCLv4
session handshake is sent on the relay-to-Hardy contact.

Set the mission override below independently for each executable:

```c
#define BPNODE_TCPCLA_NODE_ID "ipn:10.0" /* moon */
/* or ipn:20.0 for relay, ipn:30.0 for ground */
```

Start the passive ground and relay contacts before starting the active moon
and relay contacts. Once the contacts are in `BPLIB_CLA_STARTED`, the existing
BPNode CLA child tasks call TCPCLv4 receive/send and the relay's normal BPLib
forwarding logic moves bundles between its two contacts.

This example requires a cFS bundle because BPNode is a cFE application. The
workspace includes `cfs-bundle/` and a generated `cfs-build/` tree. BPNode's
`add_cfe_app()` target must run within a configured cFS environment; those copied
build files do not by themselves establish a portable build. See
[build and validation](../docs/build-and-test.md).
