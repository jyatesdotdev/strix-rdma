# ROCm zero-copy gates

Build both ROCm helpers on each Strix Halo host:

```sh
make -C tools/rocm-map
```

`tbstream-hip-map` checks GPU read/write access to the local mapped TX pool.
`tbstream-hip-zc` additionally hands GPU-written pages to the NHI and verifies
the bytes on the peer. It uses a CPU-written 64-byte envelope followed by a
synchronous VRAM-to-mapped-pool copy, matching DS4's mapped transport path.

Start the receiver first and wait for its `READY` line:

```sh
sudo tools/rocm-map/tbstream-hip-zc rx -d /dev/tbstream0 -n 1
```

Then start the transmitter on the other host:

```sh
sudo tools/rocm-map/tbstream-hip-zc tx -d /dev/tbstream0 -g 0 -n 1
```

Each repeat sends 17, 17, 33, and 65 frames. The 33-frame message begins at
frame 34 and crosses frame 64; payload bytes are verified before RX slots are
reposted. With a 4096-frame ring, `-n 31` exercises 4092 advancing frames
without wrapping. Both sides must use the same repeat count.
