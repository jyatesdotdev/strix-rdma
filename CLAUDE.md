# strix-rdma

General RDMA transport between two Strix Halo hosts over Thunderbolt/USB4 NHI
DMA rings: a default-off native software-verbs provider (`kernel/verbs/`,
`providers/strix_nhi/`) plus the USB4STREAM backport it builds on. Not specific
to any one workload — tensor-parallel inference was the first consumer and
remains the reference workload behind the historical captures in
`bench/results/`.

Master plan: docs/PLAN.md. Kernel backport strategy: kernel/README.md.
Native verbs design and limits: docs/NATIVE_VERBS.md.
Zero-copy data plane: docs/ZEROCOPY_VERBS.md. Benchmarks: bench/README.md.

Treat `bench/results/` and `LOG.md` as an immutable dated evidence record: they
name the workloads they actually measured and must not be rewritten to match
current framing.

## Log
@LOG.md
