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

/* ---- TP step: produce -> send -> receive -> consume (exchange --tp) ----
 * docs/ZEROCOPY_VERBS.md, round nineteen. The payload word j of iteration it
 * from side s; the consumer checks the peer's (!s) words for the same it. */
#define TP_STOP 0xffffffffu
/* Control words in coherent host memory, each on its own 128-byte line. */
#define TP_GO 0
#define TP_READY 32
#define TP_RECVD 64
#define TP_SLOT 96
#define TP_DONE 128
#define TP_DEBUG 160	/* first mismatch: it, word, found, expected */

__device__ static unsigned tp_word(unsigned it, unsigned side, unsigned j)
{
	return (it * 2654435761u) ^ (j * 40503u) ^ (side ? 0x5a5a5a5au : 0u);
}

__device__ static unsigned tp_poll(unsigned *p)
{
	return __hip_atomic_load(p, __ATOMIC_ACQUIRE, __HIP_MEMORY_SCOPE_SYSTEM);
}

__device__ static void tp_signal(unsigned *p, unsigned v)
{
	__hip_atomic_store(p, v, __ATOMIC_RELEASE, __HIP_MEMORY_SCOPE_SYSTEM);
}

/* Read a word the receive path wrote, with no kernel boundary since the last
 * read of the same slot. TP_LOAD selects how (compile with -DTP_LOAD=n):
 *   0  system-scope atomic load (glc: past L0/L1, through L2) -- the default
 *   1  plain load
 *   2  glc slc dlc, past every GPU cache: reads only zeros here, because the
 *      staged receive's CPU stores are visible to the GPU through L2, not
 *      from DRAM directly. */
#ifndef TP_LOAD
#define TP_LOAD 0
#endif
__device__ static unsigned tp_load_received(const unsigned *p)
{
#if TP_LOAD == 1
	return *(const volatile unsigned *)p;
#elif TP_LOAD == 2
	unsigned v;

	asm volatile("global_load_b32 %0, %1, off glc slc dlc\n\ts_waitcnt vmcnt(0)"
		     : "=v"(v) : "v"(p) : "memory");
	return v;
#else
	return __hip_atomic_load(p, __ATOMIC_RELAXED, __HIP_MEMORY_SCOPE_SYSTEM);
#endif
}

/* Write this iteration's activations and release them at system scope (G4). */
__device__ static void tp_produce(unsigned *tx, unsigned words, unsigned it, unsigned side)
{
	for (unsigned j = threadIdx.x; j < words; j += blockDim.x)
		tx[j] = tp_word(it, side, j);
	__threadfence_system();
}

extern "C" __global__ void sn_tp_produce(unsigned *tx, unsigned words, unsigned it, unsigned side)
{
	tp_produce(tx, words, it, side);
}

/* Launch-based consumer: a kernel boundary precedes the read. */
extern "C" __global__ void sn_tp_consume(const unsigned *rx, unsigned words, unsigned it,
					 unsigned side, unsigned *bad)
{
	unsigned wrong = 0;

	for (unsigned j = threadIdx.x; j < words; j += blockDim.x)
		wrong += rx[j] != tp_word(it, side, j);
	if (wrong)
		atomicAdd(bad, wrong);
}

/* Resident producer and consumer, driven through the control words. Every
 * thread polls: on gfx1151 a __syncthreads() after a loop that only thread 0
 * runs deadlocks. The host writes each word only after the previous step's
 * signal, so all threads leave a poll with the same value. */
extern "C" __global__ void sn_tp_persistent(unsigned *tx, const unsigned char *rx,
					    unsigned slot_bytes, unsigned words,
					    unsigned side, unsigned *ctl, unsigned *bad)
{
	unsigned last = 0;

	for (;;) {
		unsigned it, slot, wrong = 0;
		const unsigned *in;

		while ((it = tp_poll(&ctl[TP_GO])) == last)
			;
		if (it == TP_STOP)
			return;
		tp_produce(tx, words, it, side);
		__syncthreads();
		if (threadIdx.x == 0)
			tp_signal(&ctl[TP_READY], it);
		while (tp_poll(&ctl[TP_RECVD]) != it)
			if (tp_poll(&ctl[TP_GO]) == TP_STOP) /* host gave up mid-step */
				return;
		slot = tp_poll(&ctl[TP_SLOT]);
		in = (const unsigned *)(rx + (size_t)slot * slot_bytes);
		for (unsigned j = threadIdx.x; j < words; j += blockDim.x) {
			unsigned got = tp_load_received(&in[j]), want = tp_word(it, !side, j);

			if (got != want) {
				wrong++;
				if (atomicCAS(&ctl[TP_DEBUG], 0u, it) == 0u) {
					tp_signal(&ctl[TP_DEBUG + 1], j);
					tp_signal(&ctl[TP_DEBUG + 2], got);
					tp_signal(&ctl[TP_DEBUG + 3], want);
				}
			}
		}
		if (wrong)
			atomicAdd(bad, wrong);
		__syncthreads();
		if (threadIdx.x == 0)
			tp_signal(&ctl[TP_DONE], it);
		last = it;
	}
}

/* --gap-gpu: keep the GPU busy for the gap, as layer compute would, so the
 * launch-based hand-offs are not measured against a GPU that has gone idle.
 * wall_clock64() ticks at 100 MHz on AMD GPUs. */
extern "C" __global__ void sn_spin(unsigned long long ticks)
{
	unsigned long long until = wall_clock64() + ticks;

	while (wall_clock64() < until)
		;
}
