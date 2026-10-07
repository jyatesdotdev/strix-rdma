/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include "../verbs/native_wire.h"
#include "../verbs/native_platform.h"

static struct sn_header header(void)
{
	struct sn_header h = {0};

	h.opcode = SN_SEND; h.src_qpn = 1; h.dst_qpn = 2;
	h.src_epoch = 3; h.dst_epoch = 4;
	h.src_generation = 5; h.dst_generation = 6;
	h.sequence = 0xffffff;
	return h;
}
static void repair_crc(sn_u8 *p, size_t n)
{
	sn_put32(p + 96, sn_crc(p, n));
}
static void test_codec(void)
{
	sn_u8 p[SN_FRAME], saved[SN_FRAME];
	struct sn_header h = header(), got;
	const unsigned sizes[] = {0, 1, SN_PAYLOAD, SN_MESSAGE};
	size_t i, j, n;

	for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		h.total = sizes[i]; h.offset = 0;
		do {
			h.length = h.total - h.offset;
			if (h.length > SN_PAYLOAD) h.length = SN_PAYLOAD;
			memset(p, 0xa5, sizeof(p));
			n = SN_HEADER + h.length;
			assert(sn_encode(p, n - 1, &h));
			assert(!sn_encode(p, n, &h));
			assert(!sn_decode(&got, p, n));
			assert(!memcmp(&got, &h, sizeof(h)));
			h.offset += h.length;
		} while (h.offset < h.total);
	}
	puts("ok 1 - roundtrip segmentation at 0, 1, 3984 and 2 MiB");
	h = header(); h.length = h.total = 1;
	assert(!sn_encode(p, sizeof(p), &h));
	n = SN_HEADER + h.length;
	memcpy(saved, p, n);
	for (i = 0; i < n; i++) {
		for (j = 0; j < 8; j++) {
			memcpy(p, saved, n); p[i] ^= 1U << j;
			assert(sn_decode(&got, p, n));
		}
	}
	memcpy(p, saved, n);
	assert(sn_decode(&got, p, n - 1));
	assert(sn_decode(&got, p, n + 1));
	for (i = 0; i < SN_HEADER; i++) assert(sn_decode(&got, p, i));
	assert(sn_decode(&got, p, SN_FRAME + 1));
	puts("ok 2 - truncation, excess data and every single-bit corruption rejected");
	for (i = 0; i < SN_HEADER; i++) {
		if (!(i == 5 || i == 6 || (i >= 9 && i <= 11) || i >= 100)) continue;
		memcpy(p, saved, n); p[i] = 1; repair_crc(p, n);
		assert(sn_decode(&got, p, n));
	}
	for (i = 0; i < 256; i++) {
		if (i >= SN_SEND && i <= SN_CREDIT) continue;
		memcpy(p, saved, n); p[8] = i; repair_crc(p, n);
		assert(sn_decode(&got, p, n));
	}
	memcpy(p, saved, n); sn_put32(p + 12, SN_MESSAGE + 1); repair_crc(p, n);
	assert(sn_decode(&got, p, n));
	memcpy(p, saved, n); sn_put32(p + 72, ~0U); repair_crc(p, n);
	assert(sn_decode(&got, p, n));
	puts("ok 3 - valid-CRC reserved fields, opcodes and overflow rejected");
}
static void test_admission(void)
{
	struct sn_header h = header();
	struct sn_binding b = {4, 3, 6, 5, 2, 1}, changed;
	struct sn_readiness r = {4, 3, 3, 4, 1, 1, 1, 1, 1}, stale;

	assert(sn_admit(&b, &h));
#define STALE(field) do { changed = b; changed.field++; assert(!sn_admit(&changed, &h)); } while (0)
	STALE(local_epoch); STALE(peer_epoch); STALE(local_generation);
	STALE(peer_generation); STALE(local_qpn); STALE(peer_qpn);
#undef STALE
	assert(sn_ready(&r));
#define NOT_READY(field) do { stale = r; stale.field = 0; assert(!sn_ready(&stale)); } while (0)
	NOT_READY(local_epoch); NOT_READY(peer_epoch); NOT_READY(local_echo);
	NOT_READY(peer_echo); NOT_READY(rx_primed); NOT_READY(rings_started);
	NOT_READY(paths_active); NOT_READY(peer_ready); NOT_READY(carrier);
#undef NOT_READY
	stale = r; stale.local_epoch++; assert(!sn_ready(&stale));
	stale = r; stale.peer_epoch++; assert(!sn_ready(&stale));
	puts("ok 4 - all binding fields and reciprocal activated-path readiness required");
	assert(sn_range(4096, 8192, 4096, 8192));
	assert(sn_range(4096, 8192, 12288, 0));
	assert(!sn_range(4096, 8192, 4095, 1));
	assert(!sn_range(4096, 8192, 12288, 1));
	assert(!sn_range(4096, 8192, 4096, ~(sn_u64)0));
	assert(!sn_range(~(sn_u64)0 - 2, 4, ~(sn_u64)0 - 1, 1));
	assert(!sn_range(1, 1, ~(sn_u64)0, 1));
	puts("ok 5 - MR range arithmetic rejects underflow and overflow");
}
static void test_opcodes(void)
{
	struct sn_header h = header();
	sn_u8 p[SN_FRAME];
	struct sn_header got;
	unsigned int opcode;

	for (opcode = SN_SEND; opcode <= SN_CREDIT; opcode++) {
		h = header(); h.opcode = opcode;
		if (opcode == SN_WRITE || opcode == SN_READ) {
			h.rkey = 77; h.address = 0x123456789abcdef0ULL;
		}
		if (opcode == SN_ACK || opcode == SN_READ_ACK) {
			h.ack_sequence = h.sequence; h.sequence = 0;
		}
		assert(!sn_encode(p, sizeof(p), &h));
		assert(!sn_decode(&got, p, SN_HEADER));
		assert(!memcmp(&got, &h, sizeof(h)));
	}
	h = header(); h.opcode = SN_READ; h.rkey = 1;
	h.address = ~(sn_u64)0; h.total = 1; assert(!sn_header_valid(&h));
	h = header(); h.opcode = SN_CREDIT; h.total = 5; assert(!sn_header_valid(&h));
	h = header(); h.opcode = SN_ACK; h.sequence = 0;
	h.status = SN_PROTOCOL + 1; assert(!sn_header_valid(&h));
	h = header(); h.rkey = 1; assert(!sn_header_valid(&h));
	h = header(); h.total = 1; assert(!sn_header_valid(&h));
	puts("ok 6 - all opcodes roundtrip with canonical operation-specific fields");
}
int main(void)
{
	unsigned int id;

	puts("1..7");
	test_codec(); test_admission(); test_opcodes();
	assert(sn_crc((const sn_u8 *)"123456789", 9) == 0xe3069283U);
	for (id = 0; id <= 0xffff; id++) {
		assert(sn_supported_nhi(0x1022, id) == (id == 0x158d || id == 0x158e));
		assert(sn_supported_nhi(id, 0x158e) == (id == 0x1022));
	}
	puts("ok 7 - CRC32C reference vector and exhaustive admitted PCI identity predicate");
	return 0;
}
