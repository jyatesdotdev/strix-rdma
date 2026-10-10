# strix-rdma

General RDMA transport between two Strix Halo hosts over Thunderbolt/USB4 NHI
DMA rings: a default-off native software-verbs provider (`kernel/verbs/`,
`providers/strix_nhi/`) plus the USB4STREAM backport it builds on. Not tied to
any one workload; tensor-parallel GPU exchange is the shape the zero-copy ring
contract in `docs/TP_TRANSPORT_CONTRACT.md` was measured against.

Master plan: docs/PLAN.md. Kernel backport strategy: kernel/README.md.
Native verbs design and limits: docs/NATIVE_VERBS.md.
Zero-copy data plane: docs/ZEROCOPY_VERBS.md. Benchmarks: bench/README.md.
Host-side stream bring-up and teardown rules: tools/systemd/tbstream-lifecycle.md.

Numbers quoted in the docs are measurements taken on the lab host pair on the
date stated next to them. There is no committed results tree: re-measure on
your own hosts before relying on any figure.

## Log
@LOG.md
