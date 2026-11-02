# Source provenance and licenses

The root Apache-2.0 LICENSE applies to local integration additions. Each upstream
component retains its own copyright headers, licenses and notices. Retain those
files when redistributing source or binary packages.

| Component | Original upstream | Local publication |
| --- | --- | --- |
| BPLib | https://github.com/nasa/bplib | banderson777/bplib_and_BSL, codex/integration |
| BPNode | https://github.com/nasa/bp | banderson777/bp, codex/integration |
| BSL | https://github.com/NASA-AMMOS/BSL | banderson777/BSL, codex/integration |
| cFS bundle | https://github.com/nasa/cFS | banderson777/cFS, codex/integration |
| cFE | https://github.com/nasa/cFE | banderson777/cFE, codex/integration |

The cFE change provides per-process RAM mount selection for the multi-node native
lab. The BSL change provides sample policy-parser compatibility with older Jansson.
BPLib, BPNode and integration fixes are documented in the review reports in docs/.

BSL recursively includes QCBOR, m-lib and Unity. The cFS bundle recursively includes
OSAL, PSP, scheduler and other NASA components and tools. Their license/notice
files remain in their respective source directories. DEPENDENCIES.lock.json records
the exact source commits; .gitmodules records clone locations.

The optional unmodified BSL-docs, external topology references and standalone OSAL
working copy are not required for the published native integration. Their original
upstreams and local base commits are retained in the publication inventory.
