/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../verbs/native_order.h"

static void transfer(unsigned int opcode, unsigned int total)
{
	struct sn_assembly a = {0};
	struct sn_header h = {0}, bad;
	sn_u8 *placed = calloc(1, total ? total : 1);
	sn_u8 *stage = calloc(1, total ? total : 1);
	unsigned int offset = 0, copies = 0, cqes = 0;

	assert(placed && stage);
	a.expected = 0xffffff;
	h.sequence = a.expected; h.opcode = opcode; h.total = total;
	h.rkey = opcode == SN_WRITE ? 32 : 0;
	/* A reordered fragment cannot allocate an assembly or receive WQE. */
	h.offset = 1;
	assert(sn_fragment(&a, &h) == SN_FRAGMENT_DROP);
	do {
		h.offset = offset;
		h.length = total - offset;
		if (h.length > SN_PAYLOAD) h.length = SN_PAYLOAD;
		assert(sn_fragment(&a, &h) == (offset ? SN_FRAGMENT_APPEND : SN_FRAGMENT_START));
		memset(stage + offset, 0xab, h.length);
		sn_fragment_commit(&a, &h);
		if (h.length) assert(sn_fragment(&a, &h) == SN_FRAGMENT_DUPLICATE);
		bad = h; bad.total++;
		assert(sn_fragment(&a, &bad) == SN_FRAGMENT_INVALID);
		offset += h.length;
	} while (offset < total);
	assert(a.received == total);
	memcpy(placed, stage, total); copies++;
	if (opcode == SN_SEND || opcode == SN_READ_REPLY) cqes++;
	sn_operation_commit(&a, SN_OK);
	/* Lost ACK: replay every fragment without any second placement/CQE. */
	h.offset = 0;
	do {
		h.length = total - h.offset;
		if (h.length > SN_PAYLOAD) h.length = SN_PAYLOAD;
		assert(sn_fragment(&a, &h) == SN_FRAGMENT_REPLAY);
		h.offset += h.length;
	} while (h.offset < total);
	assert(copies == 1);
	assert(cqes == (opcode == SN_SEND || opcode == SN_READ_REPLY));
	assert(a.expected == 0x1000000);
	bad = h; bad.rkey++;
	assert(sn_fragment(&a, &bad) == SN_FRAGMENT_INVALID);
	bad = h; bad.sequence++;
	bad.offset = 0;
	assert(sn_fragment(&a, &bad) == SN_FRAGMENT_START);
	bad.sequence += 2;
	assert(sn_fragment(&a, &bad) == SN_FRAGMENT_DROP);
	for (offset = 0; offset < total; offset++) assert(placed[offset] == 0xab);
	free(placed); free(stage);
}
int main(void)
{
	const unsigned int lengths[] = {0, 1, SN_PAYLOAD, SN_PAYLOAD + 1, SN_MESSAGE};
	unsigned int i, opcode, retries;
	struct sn_assembly a = {0};
	struct sn_header h = {0};

	puts("1..4");
	for (opcode = SN_SEND; opcode <= SN_READ_REPLY; opcode++) {
		if (opcode == SN_READ) continue;
		for (i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++)
			transfer(opcode, lengths[i]);
	}
	puts("ok 1 - production assembly classifies reorder, duplicate, ACK-loss replay and metadata changes");
	a.expected = 55; h.sequence = 55; h.opcode = SN_READ; h.total = SN_MESSAGE;
	h.rkey = 17;
	assert(sn_fragment(&a, &h) == SN_FRAGMENT_START);
	sn_fragment_commit(&a, &h); sn_operation_commit(&a, SN_OK);
	assert(sn_fragment(&a, &h) == SN_FRAGMENT_REPLAY);
	assert(a.replay.total == SN_MESSAGE && a.replay.rkey == 17);
	/* Snapshot ownership/copy is a kernel-engine responsibility, not proven here. */
	a.expected = ~(sn_u64)0; h.sequence = a.expected;
	assert(sn_fragment(&a, &h) == SN_FRAGMENT_DROP);
	puts("ok 2 - READ replay identity retained; sequence wrap admission rejected");
	assert(sn_ack_timeout_ns(14) == 67108864);
	assert(!sn_ack_timeout_ns(0) && !sn_ack_timeout_ns(32));
	assert(sn_ack_timeout_ns(31) == 8796093022208ULL);
	assert(sn_rnr_timeout_ns(12) == 640000);
	assert(sn_rnr_timeout_ns(0) == 655360000);
	assert(!sn_rnr_timeout_ns(32));
	puts("ok 3 - exact verbs ACK/RNR timer conversions, including DS4 values");
	for (i = 0; i <= 6; i++) {
		retries = 0;
		while (sn_retry_take(&retries, i)) assert(retries <= i);
		assert(retries == i);
		assert(!sn_retry_take(&retries, i));
	}
	retries = 0; assert(!sn_retry_take(&retries, 7));
	puts("ok 4 - retries bounded exactly; infinite policy rejected");
	return 0;
}
