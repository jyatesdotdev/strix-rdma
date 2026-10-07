/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef STRIX_NHI_NATIVE_ORDER_H
#define STRIX_NHI_NATIVE_ORDER_H
#include "native_wire.h"

/* One ordered operation per direction. Storage/copies and MR ownership are
 * deliberately supplied by the caller; progress is committed after copying. */
struct sn_assembly {
	struct sn_header operation, replay;
	sn_u64 expected;
	sn_u32 received, replay_status;
	unsigned int active, have_replay;
};
enum sn_fragment_result {
	SN_FRAGMENT_DROP, SN_FRAGMENT_START, SN_FRAGMENT_APPEND,
	SN_FRAGMENT_DUPLICATE, SN_FRAGMENT_REPLAY, SN_FRAGMENT_INVALID
};
static inline int sn_same_operation(const struct sn_header *a,
				     const struct sn_header *b)
{
	return a->opcode == b->opcode && a->sequence == b->sequence &&
	       a->total == b->total && a->address == b->address && a->rkey == b->rkey;
}
/* Binding and wire validation MUST precede this operation-level check. */
static inline enum sn_fragment_result
sn_fragment(const struct sn_assembly *a, const struct sn_header *h)
{
	if (a->have_replay && h->sequence == a->replay.sequence)
		return sn_same_operation(&a->replay, h) ?
			SN_FRAGMENT_REPLAY : SN_FRAGMENT_INVALID;
	if (h->sequence != a->expected || a->expected == ~(sn_u64)0)
		return SN_FRAGMENT_DROP;
	if (!a->active)
		return h->offset ? SN_FRAGMENT_DROP : SN_FRAGMENT_START;
	if (!sn_same_operation(&a->operation, h))
		return SN_FRAGMENT_INVALID;
	if (h->offset < a->received)
		return h->length <= a->received - h->offset ?
			SN_FRAGMENT_DUPLICATE : SN_FRAGMENT_INVALID;
	return h->offset == a->received ? SN_FRAGMENT_APPEND : SN_FRAGMENT_DROP;
}
static inline void sn_fragment_commit(struct sn_assembly *a,
				       const struct sn_header *h)
{
	if (!a->active) {
		a->operation = *h;
		a->received = 0;
		a->active = 1;
	}
	a->received += h->length;
}
static inline void sn_operation_commit(struct sn_assembly *a, sn_u32 status)
{
	a->replay = a->operation;
	a->replay_status = status;
	a->have_replay = 1;
	a->active = 0;
	a->received = 0;
	a->expected++;
}
/* Verbs timeout unit is 4.096 us * 2^timeout; 0 is unbounded and rejected.
 * Return nanoseconds so timer rounding cannot shorten the accepted interval. */
static inline sn_u64 sn_ack_timeout_ns(unsigned int timeout)
{
	return timeout && timeout <= 31 ? (sn_u64)4096 << timeout : 0;
}
static inline sn_u64 sn_rnr_timeout_ns(unsigned int timer)
{
	static const sn_u32 usec[32] = {
		655360, 10, 20, 30, 40, 60, 80, 120,
		160, 240, 320, 480, 640, 960, 1280, 1920,
		2560, 3840, 5120, 7680, 10240, 15360, 20480, 30720,
		40960, 61440, 81920, 122880, 163840, 245760, 327680, 491520
	};
	return timer < 32 ? (sn_u64)usec[timer] * 1000 : 0;
}
/* Limit counts retransmissions after the initial attempt. */
static inline int sn_retry_take(unsigned int *used, unsigned int limit)
{
	if (limit > 6 || *used >= limit)
		return 0;
	++*used;
	return 1;
}
#endif
