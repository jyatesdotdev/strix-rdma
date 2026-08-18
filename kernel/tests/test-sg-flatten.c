/*
 * Host-independent tests for the DMA segment validation and frame
 * flattening rules shared by the thunderbolt-stream DMA-BUF probe and
 * the imported-pool mode. Compiled against the patched kernel tree's
 * drivers/thunderbolt/stream-sg.h in its userspace mode.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* Userspace type shim required by stream-sg.h outside __KERNEL__. */
typedef uint64_t u64;
typedef uint32_t u32;
#define U32_MAX UINT32_MAX
#define U64_MAX UINT64_MAX

#include "stream-sg.h"

static int failures;
static int checks;

#define CHECK(cond, name) do { \
		checks++; \
		if (!(cond)) { \
			failures++; \
			fprintf(stderr, "not ok %d - %s\n", checks, name); \
		} \
	} while (0)

struct seg {
	u64 addr;
	u64 len;
};

/* Feed segments through one query; returns the last segment's status. */
static int run(struct tbstream_sg_query *q, const struct seg *segs, int n)
{
	int i, ret = 0;

	for (i = 0; i < n; i++) {
		ret = tbstream_sg_segment(q, segs[i].addr, segs[i].len);
		if (ret)
			return ret;
	}
	return ret;
}

#define K4 4096ULL
#define K64 (16 * K4)
#define M1 (256 * K4)

static void test_init_rules(void)
{
	struct tbstream_sg_query q;

	CHECK(tbstream_sg_query_init(&q, 0, 0, NULL, 0) == -EINVAL,
	      "init rejects zero length");
	CHECK(tbstream_sg_query_init(&q, K4, K4, NULL, 0) == 0,
	      "init accepts aligned offset");
	CHECK(tbstream_sg_query_init(&q, 512, K4, NULL, 0) == -EINVAL,
	      "init rejects unaligned offset");
	CHECK(tbstream_sg_query_init(&q, 0, K4 + 512, NULL, 0) == -EINVAL,
	      "init rejects unaligned length");
	CHECK(tbstream_sg_query_init(&q, 0xFFFFFFFFFFFFF000ULL, 2 * K4, NULL, 0)
	      == -EOVERFLOW,
	      "init rejects wrapping range");
}

static void test_single_segment(void)
{
	static const struct seg segs[] = { { 0x100000000ULL, K64 } };
	u64 frames[16];
	struct tbstream_sg_query q;
	int i;

	CHECK(tbstream_sg_query_init(&q, 0, K64, frames, 16) == 0,
	      "single: init");
	CHECK(run(&q, segs, 1) == 0, "single: accepted");
	CHECK(q.covered == K64, "single: full coverage");
	CHECK(q.table_bytes == K64, "single: table bytes");
	CHECK(q.frames == 16, "single: frame count");
	CHECK(q.mapped_entries == 1, "single: entry count");
	CHECK(q.largest_segment == K64, "single: largest segment");
	CHECK(q.min_align == K64, "single: alignment is segment-sized");
	for (i = 0; i < 16; i++)
		if (frames[i] != 0x100000000ULL + (u64)i * K4)
			break;
	CHECK(i == 16, "single: emitted addresses walk the segment");
}

static void test_many_segments(void)
{
	static const struct seg segs[] = {
		{ 0x200000000ULL, K4 },
		{ 0x300000000ULL, 2 * K4 },
		{ 0x400000000ULL, K4 },
	};
	u64 frames[4];
	struct tbstream_sg_query q;

	CHECK(tbstream_sg_query_init(&q, 0, 4 * K4, frames, 4) == 0,
	      "many: init");
	CHECK(run(&q, segs, 3) == 0, "many: accepted");
	CHECK(q.frames == 4 && q.covered == 4 * K4, "many: coverage");
	CHECK(frames[0] == 0x200000000ULL &&
	      frames[1] == 0x300000000ULL &&
	      frames[2] == 0x300000000ULL + K4 &&
	      frames[3] == 0x400000000ULL,
	      "many: no frame crosses a segment boundary");
	CHECK(q.largest_segment == 2 * K4, "many: largest segment");
}

static void test_coalesced(void)
{
	static const struct seg segs[] = { { 0x500000000ULL, M1 } };
	u64 frames[256];
	struct tbstream_sg_query q;

	CHECK(tbstream_sg_query_init(&q, 0, M1, frames, 256) == 0,
	      "coalesced: init");
	CHECK(run(&q, segs, 1) == 0, "coalesced: accepted");
	CHECK(q.frames == 256, "coalesced: 256 frames from one entry");
	CHECK(frames[255] == 0x500000000ULL + 255 * K4,
	      "coalesced: last frame inside the segment");
}

static void test_exact_boundaries(void)
{
	static const struct seg segs[] = {
		{ 0x600000000ULL, 2 * K4 },
		{ 0x700000000ULL, K4 },
	};
	u64 frames[2];
	struct tbstream_sg_query q;

	/* Range ends exactly at the first segment's end. */
	CHECK(tbstream_sg_query_init(&q, 0, 2 * K4, frames, 2) == 0,
	      "boundary: init");
	CHECK(run(&q, segs, 2) == 0, "boundary: accepted");
	CHECK(q.covered == 2 * K4 && q.frames == 2,
	      "boundary: trailing segment excluded");
	CHECK(q.table_bytes == 3 * K4 && q.mapped_entries == 2,
	      "boundary: whole table still measured");
}

static void test_bad_segments(void)
{
	static const struct seg zero[] = { { 0x1000, 0 } };
	static const struct seg badaddr[] = { { 0x1200, K4 } };
	static const struct seg badlen[] = { { 0x1000, K4 + 512 } };
	struct tbstream_sg_query q;

	CHECK(tbstream_sg_query_init(&q, 0, K4, NULL, 0) == 0 &&
	      run(&q, zero, 1) == -EINVAL,
	      "reject zero-length segment");
	CHECK(tbstream_sg_query_init(&q, 0, K4, NULL, 0) == 0 &&
	      run(&q, badaddr, 1) == -EINVAL,
	      "reject unaligned segment address");
	CHECK(tbstream_sg_query_init(&q, 0, 2 * K4, NULL, 0) == 0 &&
	      run(&q, badlen, 1) == -EINVAL,
	      "reject unaligned segment length");
}

static void test_overflow(void)
{
	/* addr + len wraps. */
	static const struct seg wrap[] = { { 0xFFFFFFFFFFFFF000ULL, 2 * K4 } };
	/* Cumulative length wraps on the second segment. */
	static const struct seg sum[] = {
		{ 0ULL, 0xFFFFFFFFFFFFF000ULL },
		{ 0x1000000000ULL, 2 * K4 },
	};
	struct tbstream_sg_query q;

	CHECK(tbstream_sg_query_init(&q, 0, K4, NULL, 0) == 0 &&
	      run(&q, wrap, 1) == -EOVERFLOW,
	      "reject wrapping segment address arithmetic");
	CHECK(tbstream_sg_query_init(&q, 0, K4, NULL, 0) == 0 &&
	      run(&q, sum, 2) == -EOVERFLOW,
	      "reject wrapping cumulative length");

	/* Excessive frame count. */
	CHECK(tbstream_sg_query_init(&q, 0, K64, NULL, 0) == 0,
	      "frame overflow: init");
	q.frames = U32_MAX;
	CHECK(tbstream_sg_segment(&q, 0x800000000ULL, K4) == -EOVERFLOW,
	      "reject frame counter overflow");
}

static void test_coverage_reporting(void)
{
	/* Table shorter than the requested range: partial coverage is a
	 * reportable result, not an error. */
	static const struct seg shortt[] = {
		{ 0x100000000ULL, 8 * K4 },
		{ 0x200000000ULL, 8 * K4 },
	};
	/* Table longer than the requested range. */
	static const struct seg over[] = { { 0x100000000ULL, K64 } };
	struct tbstream_sg_query q;

	CHECK(tbstream_sg_query_init(&q, 0, 32 * K4, NULL, 0) == 0 &&
	      run(&q, shortt, 2) == 0,
	      "short: accepted and measured");
	CHECK(q.covered == 16 * K4 && q.frames == 16,
	      "short: partial coverage reported");

	CHECK(tbstream_sg_query_init(&q, 0, 4 * K4, NULL, 0) == 0 &&
	      run(&q, over, 1) == 0,
	      "overlong: accepted and measured");
	CHECK(q.covered == 4 * K4 && q.table_bytes == K64,
	      "overlong: range covered, larger table recorded");
}

static void test_frame_capacity(void)
{
	static const struct seg segs[] = { { 0x100000000ULL, 2 * K4 } };
	u64 frames[2];
	struct tbstream_sg_query q;

	CHECK(tbstream_sg_query_init(&q, 0, 2 * K4, frames, 1) == 0 &&
	      run(&q, segs, 1) == -ENOSPC,
	      "reject frame destination overflow");
	CHECK(tbstream_sg_query_init(&q, 0, 2 * K4, frames, 2) == 0 &&
	      run(&q, segs, 1) == 0 && q.frames == 2,
	      "exact frame capacity accepted");
}

static void test_final_partial_segment(void)
{
	/* Two large segments followed by a small final one. */
	static const struct seg segs[] = {
		{ 0x100000000ULL, K64 },
		{ 0x200000000ULL, K64 },
		{ 0x300000000ULL, K4 },
	};
	u64 frames[33];
	struct tbstream_sg_query q;

	CHECK(tbstream_sg_query_init(&q, 0, 33 * K4, frames, 33) == 0 &&
	      run(&q, segs, 3) == 0,
	      "final: accepted");
	CHECK(q.frames == 33 && q.covered == 33 * K4,
	      "final: full coverage");
	CHECK(frames[32] == 0x300000000ULL,
	      "final: last frame is the small segment's base");
	CHECK(q.largest_segment == K64, "final: largest segment");
}

static void test_subrange(void)
{
	/* Offset and length land inside one coalesced segment. */
	static const struct seg segs[] = {
		{ 0x100000000ULL, K64 },
		{ 0x200000000ULL, K64 },
	};
	u64 frames[4];
	struct tbstream_sg_query q;

	CHECK(tbstream_sg_query_init(&q, 4 * K4, 4 * K4, frames, 4) == 0 &&
	      run(&q, segs, 2) == 0,
	      "subrange: accepted");
	CHECK(q.covered == 4 * K4 && q.frames == 4,
	      "subrange: only the overlap is measured");
	CHECK(frames[0] == 0x100000000ULL + 4 * K4,
	      "subrange: first frame carries the offset");

	/* Range skipping whole leading segments. */
	CHECK(tbstream_sg_query_init(&q, K64, K4, frames, 1) == 0 &&
	      run(&q, segs, 2) == 0,
	      "subrange: skip accepted");
	CHECK(q.frames == 1 && frames[0] == 0x200000000ULL,
	      "subrange: leading segment contributes nothing");
}

static void test_empty_table(void)
{
	struct tbstream_sg_query q;

	CHECK(tbstream_sg_query_init(&q, 0, K4, NULL, 0) == 0 &&
	      run(&q, NULL, 0) == 0,
	      "empty: accepted");
	CHECK(q.mapped_entries == 0 && q.covered == 0 && q.frames == 0,
	      "empty: nothing mapped, nothing covered");
	CHECK(q.min_align == U64_MAX,
	      "empty: alignment stays at the init sentinel");
}

static void test_min_alignment(void)
{
	static const struct seg segs[] = {
		{ 0x100000000ULL, K64 },	/* 64 KiB aligned */
		{ 0x100010000ULL, K4 },		/* 4 KiB aligned */
	};
	struct tbstream_sg_query q;

	CHECK(tbstream_sg_query_init(&q, 0, K64 + K4, NULL, 0) == 0 &&
	      run(&q, segs, 2) == 0,
	      "align: accepted");
	CHECK(q.min_align == K4,
	      "align: tightest segment alignment reported");
}

int main(void)
{
	test_init_rules();
	test_single_segment();
	test_many_segments();
	test_coalesced();
	test_exact_boundaries();
	test_bad_segments();
	test_overflow();
	test_coverage_reporting();
	test_frame_capacity();
	test_final_partial_segment();
	test_subrange();
	test_empty_table();
	test_min_alignment();

	if (failures) {
		fprintf(stderr, "FAIL: %d/%d checks failed\n", failures, checks);
		return 1;
	}
	printf("ok - %d SG flatten checks passed\n", checks);
	return 0;
}
