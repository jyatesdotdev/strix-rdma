# Zero-copy slot mode vs stock USB4STREAM — 2026-08-02

First numbers for the zero-copy UAPI (kernel/zerocopy/ series: mmap'd frame
pools + SUBMIT_TX/POST_RX/REAP + RING_FRAME_NO_INTERRUPT). Both hosts on the
patched driver, stream `ds4`, ring 4096. Payloads verified (`-V`) in the 4 KiB
smoke run. Raw CSVs: `usb4stream-zc.csv` (throttling 8192),
`usb4stream-zc-thr0.csv` (throttling 0).

## QD1 echo round-trip (p50 µs, throttling 0)

| msg size | rw mode (tuned) | zero-copy | delta |
|--:|--:|--:|--:|
| 4 KiB  | 24.0 | 23.9 | ~0 |
| 16 KiB | 47.4 | 55.6 | +17% |
| 64 KiB | 141.0 | 152.9 | +8% |
| 256 KiB | 511.7 | 523.0 | +2% |
| 1 MiB  | 1884.0 | 2033.1 | +8% |
| 4 MiB  | 7108.7 | 8062.9 | +13% |

## Echo-side cost, 1000+ x 1 MiB echoes (the design targets)

| metric | rw mode | zero-copy | delta |
|--|--:|--:|--:|
| process CPU (user+sys) | 0.473 s (0.467 sys) | 0.245 s (0.206 sys) | **-48%** |
| total interrupts (300 msgs) | 83.5k (~269/msg) | 43.1k (~139/msg) | **-48%** |
| throughput | 1095 MB/s | 1026 MB/s | -6% |

## Read

- **CPU and interrupts halve; RTT does not improve.** The RTT deficit is a
  benchmark artifact, not transport cost: rw-mode `pong` gets woken per frame
  and starts copying/echoing while later frames are still arriving, i.e. it
  pipelines *within* a message. Zero-copy REAP fires once per complete
  message, so the echo host is store-and-forward — it eats a full message
  serialization (~890 µs at 1 MiB) that rw hides. DS4's real pattern is the
  zc one: the receiver consumes the completed tensor in place (ideally the
  GPU reads the slot directly), and overlap comes from pipelining tensor
  production/transfer/consumption at the DS4 level, not from splitting one
  tensor's arrival.
- **CPU is the currency that matters for DS4**: 464 µs → 240 µs of echo-side
  CPU per 1 MiB message is CPU the token loop gets back.
- Remaining interrupt load (~139/msg) is RX-side per-frame completions
  (coalesced under load). v2: since DS4 negotiates fixed message sizes,
  POST_RX can take a flag to request an interrupt only on every k-th frame
  (message boundary) using the same RING_FRAME_NO_INTERRUPT plumbing —
  should take ~139/msg to ~2/msg and shave RX-side wake latency.
- 4 KiB single-frame: zc ≈ rw (23.9 vs 24.0) — no copies to save, and one
  extra ioctl round trip per message cancels the gain.

## Next measurements

- One-way zc streaming mode (ztx/zrx) for a clean bandwidth + one-way
  latency comparison without the echo artifact.
- Step 4 of the plan: `hipHostRegister()` the mmap'd pools and measure
  GPU-direct access latency.
