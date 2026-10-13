// SPDX-License-Identifier: GPL-2.0-only
/* GPU-side producer and consumer for strix-verbs-smoke --gpu-kernels (the G4
 * stale-cache gate, docs/GPU_DIRECT_VERBS.md). Built once into a code object,
 *   hipcc --genco --offload-arch=gfx1151 -O2 strix-verbs-gpu-kernels.hip \
 *         -o strix-verbs-gpu-kernels.hsaco
 * and loaded by the C smoke tool at runtime. The pattern must match the tool's
 * pattern() exactly. */
#include <hip/hip_runtime.h>

#define WINDOW 4u
#define CHUNK (2u * 1024u * 1024u)

__device__ static unsigned char pattern(unsigned long long nonce, unsigned window,
					unsigned slot, unsigned byte)
{
	return (unsigned char)((nonce >> ((byte % 8) * 8)) ^ (byte * 131u) ^
		(byte >> 11) ^ (window * 29u) ^ (slot * 53u));
}

/* Write this side's pattern for iteration `it` into every slot's transfer range. */
extern "C" __global__ void sn_fill(unsigned char *buf, unsigned long long nonce,
				   unsigned it, unsigned len, unsigned off)
{
	size_t stride = (size_t)gridDim.x * blockDim.x;

	for (size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x;
	     i < (size_t)WINDOW * len; i += stride) {
		unsigned slot = (unsigned)(i / len), j = (unsigned)(i % len);

		buf[(size_t)slot * CHUNK + off + j] = pattern(nonce, it, slot, j);
	}
}

/* Count bytes that differ from what must be there. full != 0: the whole window,
 * the peer's pattern inside each slot's transfer range and zero outside it.
 * full == 0 ("hot"): the transfer ranges only, so the lines this check reads stay
 * resident in the GPU L2 for the next iteration's check -- the condition under
 * which a missing system-scope acquire would return stale data. Every byte of
 * the pattern changes between iterations, so a stale line always mismatches. */
extern "C" __global__ void sn_check(const unsigned char *buf, unsigned long long nonce,
				    unsigned it, unsigned len, unsigned off, unsigned *bad,
				    unsigned full)
{
	size_t stride = (size_t)gridDim.x * blockDim.x;
	size_t total = full ? (size_t)WINDOW * CHUNK : (size_t)WINDOW * len;
	unsigned local = 0;

	for (size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x; i < total; i += stride) {
		unsigned slot, j;
		unsigned char want;

		if (full) {
			slot = (unsigned)(i / CHUNK); j = (unsigned)(i % CHUNK);
			want = j >= off && j - off < len ? pattern(nonce, it, slot, j - off) : 0;
			local += buf[i] != want;
		} else {
			slot = (unsigned)(i / len); j = (unsigned)(i % len);
			local += buf[(size_t)slot * CHUNK + off + j] != pattern(nonce, it, slot, j);
		}
	}
	if (local)
		atomicAdd(bad, local);
}
