// SPDX-License-Identifier: MIT
/*
 * Shared generation-stamped payload pattern for the gate-5 stale-cache
 * harness. The CPU sender and the GPU digest kernel must compute
 * byte-identical expectations, so this mixer is the single source of
 * truth for both.
 */
#ifndef STALE_PATTERN_H
#define STALE_PATTERN_H

#include <stdint.h>

#define STALE_FRAME_SIZE	4096u
#define STALE_FRAME_WORDS	(STALE_FRAME_SIZE / 4u)

#ifdef __HIPCC__
#define STALE_FN __host__ __device__ static inline
#else
#define STALE_FN static inline
#endif

/*
 * One 32-bit payload word. msg is the monotonic message counter (so
 * every pass over a reused slot changes every word), frame is the
 * frame index within the message, word is the word index within the
 * frame.
 */
STALE_FN uint32_t stale_mix(uint32_t seed, uint32_t msg, uint32_t frame,
			    uint32_t word)
{
	uint32_t x = seed ^ (msg * 0x9e3779b9u) ^ (frame * 0x85ebca6bu) ^
		     (word * 0xc2b2ae35u);

	x ^= x >> 16;
	x *= 0x7feb352du;
	x ^= x >> 15;
	x *= 0x846ca68bu;
	x ^= x >> 16;
	return x;
}

#endif /* STALE_PATTERN_H */
