// SPDX-License-Identifier: MIT
/*
 * Bidirectional zero-copy gate using the request/response geometry produced
 * by DS4 distributed inference. This intentionally avoids model and GPU
 * dependencies so transport delivery and ring ownership can be tested alone.
 */

#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#ifdef __linux__
#include "../pingpong/thunderbolt-stream.h"
#else
/* Let the offline self-test compile on non-Linux development machines. */
#define TBSTREAM_ZC_FRAME_SIZE 4096u

typedef uint64_t tbstream_aligned_u64 __attribute__((aligned(8)));

struct tbstream_zc_info {
	uint32_t ring_size;
	uint32_t frame_size;
	uint64_t tx_pool_offset;
	uint64_t rx_pool_offset;
};

enum tbstream_zc_event_type {
	TBSTREAM_ZC_EV_RX = 0,
	TBSTREAM_ZC_EV_TX_DONE = 1,
	TBSTREAM_ZC_EV_CLOSE = 2,
};

struct tbstream_zc_event {
	uint32_t type;
	uint32_t first;
	uint32_t nframes;
	uint32_t bytes;
};

struct tbstream_zc_tx {
	uint32_t nframes;
	uint32_t last_len;
	uint32_t first;
	uint32_t reserved;
};

struct tbstream_zc_reap {
	uint32_t max;
	uint32_t flags;
	uint64_t events;
};

struct tbstream_zc_rx {
	uint32_t nframes;
	uint32_t flags;
};

struct tbstream_zc_ring_stats {
	tbstream_aligned_u64 descriptors_posted;
	tbstream_aligned_u64 descriptors_completed;
	tbstream_aligned_u64 interrupts;
	tbstream_aligned_u64 work_runs;
	tbstream_aligned_u64 kick_requests;
	tbstream_aligned_u64 kick_pending;
	uint32_t flags;
	int32_t hop;
	int32_t irq;
	uint32_t vector;
	uint32_t size;
	uint32_t sw_head;
	uint32_t sw_tail;
	uint32_t hw_posted;
	uint32_t hw_completed;
	uint32_t queued;
	uint32_t in_flight;
	uint32_t tail_flags;
	uint32_t tail_length;
	uint32_t tail_eof;
	uint32_t tail_sof;
	uint32_t options;
	uint32_t interval_nsec;
	uint32_t reserved[2];
};

struct tbstream_zc_stats {
	uint32_t version;
	uint32_t struct_size;
	tbstream_aligned_u64 tx_submit_calls;
	tbstream_aligned_u64 tx_submit_frames;
	tbstream_aligned_u64 tx_callbacks;
	tbstream_aligned_u64 tx_terminal_callbacks;
	tbstream_aligned_u64 tx_events;
	tbstream_aligned_u64 tx_enqueue_errors;
	tbstream_aligned_u64 rx_callbacks;
	tbstream_aligned_u64 rx_data_more;
	tbstream_aligned_u64 rx_data;
	tbstream_aligned_u64 rx_close;
	tbstream_aligned_u64 rx_events;
	tbstream_aligned_u64 rx_repost_calls;
	tbstream_aligned_u64 rx_repost_frames;
	tbstream_aligned_u64 rx_repost_errors;
	tbstream_aligned_u64 reap_calls;
	tbstream_aligned_u64 reaped_events;
	tbstream_aligned_u64 event_drops;
	tbstream_aligned_u64 crc_errors;
	tbstream_aligned_u64 overrun_errors;
	tbstream_aligned_u64 canceled_callbacks;
	tbstream_aligned_u64 failures;
	tbstream_aligned_u64 tx_prod;
	tbstream_aligned_u64 tx_cons;
	tbstream_aligned_u64 rx_prod;
	tbstream_aligned_u64 rx_cons;
	uint32_t tx_pending;
	uint32_t tx_done;
	uint32_t rx_partial_frames;
	uint32_t rx_partial_bytes;
	uint32_t fifo_len;
	uint32_t fifo_avail;
	uint32_t flags;
	int32_t in_hopid;
	int32_t out_hopid;
	uint32_t throttling;
	uint32_t last_error;
	uint32_t reserved[4];
	struct tbstream_zc_ring_stats tx;
	struct tbstream_zc_ring_stats rx;
};

struct tbstream_zc_kick {
	uint32_t rings;
	uint32_t reserved;
};

#define TBSTREAM_ZC_RX_F_INTERRUPT_BOUNDARIES 0x1u
#define TBSTREAM_ZC_STATS_VERSION 1u
#define TBSTREAM_ZC_ERROR_NONE 0u
#define TBSTREAM_ZC_ERROR_RX_CRC 1u
#define TBSTREAM_ZC_ERROR_RX_OVERRUN 2u
#define TBSTREAM_ZC_ERROR_EVENT_DROP 3u
#define TBSTREAM_ZC_ERROR_TX_PARTIAL 4u
#define TBSTREAM_ZC_ERROR_RX_PARTIAL 5u
#define TBSTREAM_ZC_RING_F_HW_VALID 0x20u
#define TBSTREAM_ZC_KICK_TX 0x1u
#define TBSTREAM_ZC_KICK_RX 0x2u
#define TBSTREAM_ZC_MAGIC 0xb4
#define TBSTREAM_ZC_ENABLE _IO(TBSTREAM_ZC_MAGIC, 0x00)
#define TBSTREAM_ZC_GET_INFO \
	_IOR(TBSTREAM_ZC_MAGIC, 0x01, struct tbstream_zc_info)
#define TBSTREAM_ZC_SUBMIT_TX \
	_IOWR(TBSTREAM_ZC_MAGIC, 0x02, struct tbstream_zc_tx)
#define TBSTREAM_ZC_POST_RX _IOW(TBSTREAM_ZC_MAGIC, 0x03, uint32_t)
#define TBSTREAM_ZC_REAP \
	_IOWR(TBSTREAM_ZC_MAGIC, 0x04, struct tbstream_zc_reap)
#define TBSTREAM_ZC_POST_RX_FLAGS \
	_IOW(TBSTREAM_ZC_MAGIC, 0x05, struct tbstream_zc_rx)
#define TBSTREAM_ZC_GET_STATS \
	_IOR(TBSTREAM_ZC_MAGIC, 0x06, struct tbstream_zc_stats)
#define TBSTREAM_ZC_KICK \
	_IOW(TBSTREAM_ZC_MAGIC, 0x07, struct tbstream_zc_kick)
#endif

_Static_assert(sizeof(struct tbstream_zc_ring_stats) == 128,
	       "tbstream diagnostic ring ABI size changed");
_Static_assert(offsetof(struct tbstream_zc_stats, tx) == 272,
	       "tbstream diagnostic TX offset changed");
_Static_assert(offsetof(struct tbstream_zc_stats, rx) == 400,
	       "tbstream diagnostic RX offset changed");
_Static_assert(offsetof(struct tbstream_zc_stats, last_error) == 248,
	       "tbstream diagnostic error offset changed");
_Static_assert(sizeof(struct tbstream_zc_stats) == 528,
	       "tbstream diagnostic stats ABI size changed");
_Static_assert(sizeof(struct tbstream_zc_kick) == 8,
	       "tbstream diagnostic kick ABI size changed");
#ifdef __linux__
_Static_assert(_IOC_NR(TBSTREAM_ZC_GET_STATS) == 0x06,
	       "tbstream diagnostic stats ioctl number changed");
_Static_assert(_IOC_SIZE(TBSTREAM_ZC_GET_STATS) == 528,
	       "tbstream diagnostic stats ioctl size changed");
_Static_assert(_IOC_NR(TBSTREAM_ZC_KICK) == 0x07,
	       "tbstream diagnostic kick ioctl number changed");
_Static_assert(_IOC_SIZE(TBSTREAM_ZC_KICK) == 8,
	       "tbstream diagnostic kick ioctl size changed");
#endif

#define GATE_MAGIC 0x44345347u /* D4SG */
#define GATE_VERSION 1u
#define GATE_ENVELOPE_BYTES 64u
#define GATE_DEFAULT_EXCHANGES 32u
#define GATE_DEFAULT_TIMEOUT_MS 30000u
#define GATE_KICK_SETTLE_MS 10u
#define GATE_MAX_DELAY_MS 3600000u
#define GATE_REQUEST_LAST_LEN 64u
#define GATE_RESPONSE_FRAMES 127u
#define GATE_RESPONSE_LAST_LEN 1088u

enum message_kind {
	MESSAGE_REQUEST = 1,
	MESSAGE_RESPONSE = 2,
};

static const uint32_t request_frames[] = {17u, 17u, 33u, 65u};

struct zc_state {
	int fd;
	void *mapping;
	size_t mapping_bytes;
	unsigned char *tx;
	unsigned char *rx;
	size_t pool_bytes;
	uint32_t frame_size;
	uint32_t ring_size;
	uint32_t tx_next;
	uint32_t rx_next;
	bool tx_outstanding;
	uint32_t tx_first;
	uint32_t tx_frames;
	bool pending_rx;
	struct tbstream_zc_event pending_rx_event;
	uint32_t timeout_ms;
	const char *role;
	const char *phase;
	uint32_t exchanges;
	uint32_t sequence;
	bool exchange_active;
	bool hold_on_timeout;
	bool kick_on_timeout;
	bool timed_out;
	uint64_t wait_count;
	uint64_t tx_submitted_frames;
	uint64_t tx_completed_frames;
	uint64_t rx_received_frames;
	uint64_t rx_reposted_frames;
	struct timespec started_at;
};

static volatile sig_atomic_t release_timeout_hold;

static uint64_t elapsed_us(const struct zc_state *z)
{
	struct timespec now;
	int64_t seconds;
	int64_t nanoseconds;

	if (clock_gettime(CLOCK_MONOTONIC, &now))
		return 0;
	seconds = (int64_t)now.tv_sec - (int64_t)z->started_at.tv_sec;
	nanoseconds = (int64_t)now.tv_nsec - (int64_t)z->started_at.tv_nsec;
	if (nanoseconds < 0) {
		seconds--;
		nanoseconds += 1000000000L;
	}
	if (seconds < 0)
		return 0;
	return (uint64_t)seconds * 1000000u + (uint64_t)nanoseconds / 1000u;
}

static void gate_log(const struct zc_state *z, const char *event,
		     const char *format, ...)
{
	va_list arguments;

	fprintf(stderr, "GATE role=%s exchange=",
		z->role ? z->role : "unknown");
	if (z->exchange_active)
		fprintf(stderr, "%u/%u sequence=%u", z->sequence + 1u,
			z->exchanges, z->sequence);
	else
		fprintf(stderr, "0/%u sequence=-", z->exchanges);
	fprintf(stderr,
		" phase=%s event=%s elapsed_us=%" PRIu64
		" wait_count=%" PRIu64
		" tx_submitted_frames=%" PRIu64
		" tx_completed_frames=%" PRIu64
		" rx_received_frames=%" PRIu64
		" rx_reposted_frames=%" PRIu64
		" tx_next=%u rx_next=%u",
		z->phase ? z->phase : "setup", event, elapsed_us(z),
		z->wait_count, z->tx_submitted_frames, z->tx_completed_frames,
		z->rx_received_frames, z->rx_reposted_frames, z->tx_next,
		z->rx_next);
	if (format && *format) {
		fputc(' ', stderr);
		va_start(arguments, format);
		vfprintf(stderr, format, arguments);
		va_end(arguments);
	}
	fputc('\n', stderr);
	fflush(stderr);
}

static void enter_phase(struct zc_state *z, uint32_t sequence,
			const char *phase)
{
	z->sequence = sequence;
	z->exchange_active = true;
	z->phase = phase;
	gate_log(z, "phase-enter", NULL);
}

static int add_frames(uint64_t *total, uint32_t frames)
{
	if (UINT64_MAX - *total < frames) {
		errno = EOVERFLOW;
		return -1;
	}
	*total += frames;
	return 0;
}

static const char *event_type_name(uint32_t type)
{
	switch (type) {
	case TBSTREAM_ZC_EV_RX:
		return "rx";
	case TBSTREAM_ZC_EV_TX_DONE:
		return "tx-done";
	case TBSTREAM_ZC_EV_CLOSE:
		return "close";
	default:
		return "unknown";
	}
}

static const char *error_type_name(uint32_t error)
{
	switch (error) {
	case TBSTREAM_ZC_ERROR_NONE:
		return "none";
	case TBSTREAM_ZC_ERROR_RX_CRC:
		return "rx-crc";
	case TBSTREAM_ZC_ERROR_RX_OVERRUN:
		return "rx-overrun";
	case TBSTREAM_ZC_ERROR_EVENT_DROP:
		return "event-drop";
	case TBSTREAM_ZC_ERROR_TX_PARTIAL:
		return "tx-partial";
	case TBSTREAM_ZC_ERROR_RX_PARTIAL:
		return "rx-partial";
	default:
		return "unknown";
	}
}

static void put_u32(unsigned char *p, uint32_t value)
{
	value = htonl(value);
	memcpy(p, &value, sizeof(value));
}

static uint32_t message_seed(enum message_kind kind, uint32_t sequence)
{
	return 0x6d2b79f5u ^ ((uint32_t)kind * 0x9e3779b9u) ^
	       (sequence * 0x85ebca6bu);
}

static size_t message_bytes(uint32_t frame_size, uint32_t nframes,
			    uint32_t last_len)
{
	return (size_t)(nframes - 1u) * frame_size + last_len;
}

static void encode_envelope(unsigned char *header, enum message_kind kind,
			    uint32_t sequence, uint32_t nframes,
			    uint32_t last_len, uint32_t frame_size)
{
	const uint32_t total =
		(uint32_t)message_bytes(frame_size, nframes, last_len);
	const uint32_t payload = total - GATE_ENVELOPE_BYTES;
	const uint32_t seed = message_seed(kind, sequence);
	uint32_t check = 0xa5a55a5au;

	memset(header, 0, GATE_ENVELOPE_BYTES);
	put_u32(header + 0, GATE_MAGIC);
	put_u32(header + 4, GATE_VERSION);
	put_u32(header + 8, (uint32_t)kind);
	put_u32(header + 12, sequence);
	put_u32(header + 16, nframes);
	put_u32(header + 20, last_len);
	put_u32(header + 24, frame_size);
	put_u32(header + 28, total);
	put_u32(header + 32, payload);
	put_u32(header + 36, GATE_ENVELOPE_BYTES);
	put_u32(header + 40, seed);
	put_u32(header + 44, ~sequence);
	put_u32(header + 48, ~nframes);
	put_u32(header + 52, 0x44533400u | (uint32_t)kind);
	put_u32(header + 56, sequence ^ (nframes << 16) ^ last_len);
	check ^= GATE_MAGIC ^ GATE_VERSION ^ (uint32_t)kind ^ sequence;
	check ^= nframes ^ last_len ^ frame_size ^ total ^ payload ^ seed;
	put_u32(header + 60, check);
}

static unsigned char expected_payload_byte(enum message_kind kind,
					   uint32_t sequence,
					   size_t payload_offset)
{
	const size_t mixed = (size_t)message_seed(kind, sequence) +
		131u * payload_offset + (payload_offset >> 7) +
		(payload_offset >> 17);

	return (unsigned char)mixed;
}

static int validate_message_geometry(uint32_t frame_size, uint32_t ring_size,
				     uint32_t nframes, uint32_t last_len)
{
	if (!nframes || nframes >= ring_size || !last_len ||
	    last_len > frame_size) {
		fprintf(stderr,
			"invalid message geometry: frames=%u last_len=%u "
			"ring=%u frame_size=%u\n",
			nframes, last_len, ring_size, frame_size);
		return -1;
	}
	if (message_bytes(frame_size, nframes, last_len) <
	    GATE_ENVELOPE_BYTES) {
		fprintf(stderr, "message is smaller than the gate envelope\n");
		return -1;
	}
	if (message_bytes(frame_size, nframes, last_len) > UINT32_MAX) {
		fprintf(stderr, "message byte count exceeds the event ABI\n");
		return -1;
	}
	return 0;
}

static int fill_message(unsigned char *pool, uint32_t ring_size,
			uint32_t frame_size, uint32_t first,
			enum message_kind kind, uint32_t sequence,
			uint32_t nframes, uint32_t last_len)
{
	size_t payload_offset = 0;

	if (validate_message_geometry(frame_size, ring_size, nframes, last_len))
		return -1;
	encode_envelope(pool + (size_t)first * frame_size, kind, sequence,
			nframes, last_len, frame_size);

	for (uint32_t frame = 0; frame < nframes; frame++) {
		const uint32_t index = (first + frame) % ring_size;
		const size_t length = frame == nframes - 1u ?
			last_len : frame_size;
		const size_t begin = frame == 0 ? GATE_ENVELOPE_BYTES : 0;
		unsigned char *page = pool + (size_t)index * frame_size;

		for (size_t offset = begin; offset < length; offset++)
			page[offset] = expected_payload_byte(kind, sequence,
							 payload_offset++);
	}
	return 0;
}

static int verify_message(const unsigned char *pool, uint32_t ring_size,
			  uint32_t frame_size, uint32_t first,
			  enum message_kind kind, uint32_t sequence,
			  uint32_t nframes, uint32_t last_len)
{
	unsigned char expected_header[GATE_ENVELOPE_BYTES];
	const unsigned char *header = pool + (size_t)first * frame_size;
	size_t payload_offset = 0;

	if (validate_message_geometry(frame_size, ring_size, nframes, last_len))
		return -1;
	encode_envelope(expected_header, kind, sequence, nframes, last_len,
			frame_size);
	if (memcmp(header, expected_header, sizeof(expected_header))) {
		for (size_t offset = 0; offset < sizeof(expected_header); offset++) {
			if (header[offset] != expected_header[offset]) {
				fprintf(stderr,
					"envelope mismatch: kind=%u seq=%u "
					"offset=%zu got=0x%02x expected=0x%02x\n",
					(unsigned)kind, sequence, offset,
					header[offset], expected_header[offset]);
				break;
			}
		}
		return -1;
	}

	for (uint32_t frame = 0; frame < nframes; frame++) {
		const uint32_t index = (first + frame) % ring_size;
		const size_t length = frame == nframes - 1u ?
			last_len : frame_size;
		const size_t begin = frame == 0 ? GATE_ENVELOPE_BYTES : 0;
		const unsigned char *page =
			pool + (size_t)index * frame_size;

		for (size_t offset = begin; offset < length; offset++) {
			const unsigned char expected = expected_payload_byte(
				kind, sequence, payload_offset);

			if (page[offset] != expected) {
				fprintf(stderr,
					"payload mismatch: kind=%u seq=%u "
					"payload_offset=%zu got=0x%02x "
					"expected=0x%02x\n",
					(unsigned)kind, sequence, payload_offset,
					page[offset], expected);
				return -1;
			}
			payload_offset++;
		}
	}
	return 0;
}

static void zc_close(struct zc_state *z)
{
	if (z->mapping != MAP_FAILED)
		munmap(z->mapping, z->mapping_bytes);
	if (z->fd >= 0)
		close(z->fd);
	z->mapping = MAP_FAILED;
	z->fd = -1;
}

static int zc_open(struct zc_state *z, const char *device,
		   uint32_t timeout_ms, const char *role, uint32_t exchanges,
		   bool hold_on_timeout, bool kick_on_timeout)
{
	struct tbstream_zc_info info;

	memset(z, 0, sizeof(*z));
	z->fd = -1;
	z->mapping = MAP_FAILED;
	z->timeout_ms = timeout_ms;
	z->role = role;
	z->phase = "setup";
	z->exchanges = exchanges;
	z->hold_on_timeout = hold_on_timeout;
	z->kick_on_timeout = kick_on_timeout;
	if (clock_gettime(CLOCK_MONOTONIC, &z->started_at)) {
		perror("clock_gettime");
		return -1;
	}

	z->fd = open(device, O_RDWR | O_CLOEXEC);
	if (z->fd < 0) {
		fprintf(stderr, "open %s: %s\n", device, strerror(errno));
		return -1;
	}
	if (ioctl(z->fd, TBSTREAM_ZC_ENABLE)) {
		perror("TBSTREAM_ZC_ENABLE");
		return -1;
	}
	memset(&info, 0, sizeof(info));
	if (ioctl(z->fd, TBSTREAM_ZC_GET_INFO, &info)) {
		perror("TBSTREAM_ZC_GET_INFO");
		return -1;
	}
	if (!info.ring_size || !info.frame_size ||
	    info.ring_size > SIZE_MAX / info.frame_size) {
		fprintf(stderr, "invalid zero-copy pool geometry\n");
		return -1;
	}
	z->ring_size = info.ring_size;
	z->frame_size = info.frame_size;
	z->pool_bytes = (size_t)info.ring_size * info.frame_size;
	if (z->pool_bytes > SIZE_MAX / 2u) {
		fprintf(stderr, "zero-copy mapping is too large\n");
		return -1;
	}
	z->mapping_bytes = 2u * z->pool_bytes;
	if (info.tx_pool_offset > z->mapping_bytes - z->pool_bytes ||
	    info.rx_pool_offset > z->mapping_bytes - z->pool_bytes) {
		fprintf(stderr, "invalid zero-copy pool offsets\n");
		return -1;
	}
	z->mapping = mmap(NULL, z->mapping_bytes, PROT_READ | PROT_WRITE,
			  MAP_SHARED, z->fd, 0);
	if (z->mapping == MAP_FAILED) {
		perror("mmap");
		return -1;
	}
	z->tx = (unsigned char *)z->mapping + (size_t)info.tx_pool_offset;
	z->rx = (unsigned char *)z->mapping + (size_t)info.rx_pool_offset;

	if (z->frame_size != TBSTREAM_ZC_FRAME_SIZE ||
	    z->ring_size <= GATE_RESPONSE_FRAMES) {
		fprintf(stderr,
			"gate needs >%u frames of %u bytes; device has %u of %u\n",
			GATE_RESPONSE_FRAMES, (unsigned)TBSTREAM_ZC_FRAME_SIZE,
			z->ring_size, z->frame_size);
		return -1;
	}
	fprintf(stderr, "zc: ring_size=%u frame_size=%u timeout_ms=%u\n",
		z->ring_size, z->frame_size, z->timeout_ms);
	gate_log(z, "zc-open",
		 "device=%s ring_size=%u frame_size=%u hold_on_timeout=%u "
		 "kick_on_timeout=%u",
		 device, z->ring_size, z->frame_size, z->hold_on_timeout,
		 z->kick_on_timeout);
	return 0;
}

static void diagnostic_context(const struct zc_state *z, const char *record,
			       const char *label)
{
	fprintf(stderr, "%s role=%s exchange=", record, z->role);
	if (z->exchange_active)
		fprintf(stderr, "%u/%u sequence=%u", z->sequence + 1u,
			z->exchanges, z->sequence);
	else
		fprintf(stderr, "0/%u sequence=-", z->exchanges);
	fprintf(stderr, " phase=%s label=%s elapsed_us=%" PRIu64,
		z->phase, label, elapsed_us(z));
}

static void dump_ring_stats(const struct zc_state *z, const char *label,
			    const char *ring_name,
			    const struct tbstream_zc_ring_stats *ring)
{
	diagnostic_context(z, "GATE_RING_STATS", label);
	fprintf(stderr,
		" ring=%s flags=0x%x hop=%d irq=%d vector=%u size=%u"
		" descriptors_posted=%" PRIu64
		" descriptors_completed=%" PRIu64
		" interrupts=%" PRIu64 " work_runs=%" PRIu64
		" kick_requests=%" PRIu64 " kick_pending=%" PRIu64,
		ring_name, ring->flags, ring->hop, ring->irq, ring->vector,
		ring->size, (uint64_t)ring->descriptors_posted,
		(uint64_t)ring->descriptors_completed, (uint64_t)ring->interrupts,
		(uint64_t)ring->work_runs, (uint64_t)ring->kick_requests,
		(uint64_t)ring->kick_pending);
	fprintf(stderr,
		" sw_head=%u sw_tail=%u hw_valid=%u"
		" hw_posted=%u hw_completed=%u"
		" queued=%u in_flight=%u tail_flags=0x%x tail_length=%u"
		" tail_eof=%u tail_sof=%u options=0x%x interval_nsec=%u\n",
		ring->sw_head, ring->sw_tail,
		!!(ring->flags & TBSTREAM_ZC_RING_F_HW_VALID),
		ring->hw_posted, ring->hw_completed, ring->queued, ring->in_flight,
		ring->tail_flags,
		ring->tail_length, ring->tail_eof, ring->tail_sof,
		ring->options, ring->interval_nsec);
}

static int dump_kernel_stats(struct zc_state *z, const char *label)
{
	struct tbstream_zc_stats stats;

	memset(&stats, 0, sizeof(stats));
	if (ioctl(z->fd, TBSTREAM_ZC_GET_STATS, &stats)) {
		const int error = errno;

		gate_log(z, error == ENOTTY ? "stats-unavailable" : "stats-error",
			 "label=%s ioctl=TBSTREAM_ZC_GET_STATS errno=%d error=%s%s",
			 label, error, strerror(error),
			 error == ENOTTY ?
			 " reason=kernel_does_not_support_diagnostic_UAPI" : "");
		errno = error;
		return -1;
	}
	if (stats.version != TBSTREAM_ZC_STATS_VERSION ||
	    stats.struct_size != sizeof(stats)) {
		gate_log(z, "stats-abi-mismatch",
			 "label=%s version=%u expected_version=%u struct_size=%u "
			 "expected_size=%zu",
			 label, stats.version, TBSTREAM_ZC_STATS_VERSION,
			 stats.struct_size, sizeof(stats));
		errno = EPROTO;
		return -1;
	}

	diagnostic_context(z, "GATE_STREAM_STATS", label);
	fprintf(stderr,
		" version=%u struct_size=%u flags=0x%x in_hopid=%d out_hopid=%d"
		" throttling=%u last_error=%u last_error_name=%s"
		" tx_submit_calls=%" PRIu64
		" tx_submit_frames=%" PRIu64 " tx_callbacks=%" PRIu64
		" tx_terminal_callbacks=%" PRIu64 " tx_events=%" PRIu64
		" tx_enqueue_errors=%" PRIu64,
		stats.version, stats.struct_size, stats.flags, stats.in_hopid,
		stats.out_hopid, stats.throttling, stats.last_error,
		error_type_name(stats.last_error), (uint64_t)stats.tx_submit_calls,
		(uint64_t)stats.tx_submit_frames, (uint64_t)stats.tx_callbacks,
		(uint64_t)stats.tx_terminal_callbacks, (uint64_t)stats.tx_events,
		(uint64_t)stats.tx_enqueue_errors);
	fprintf(stderr,
		" rx_callbacks=%" PRIu64 " rx_data_more=%" PRIu64
		" rx_data=%" PRIu64 " rx_close=%" PRIu64
		" rx_events=%" PRIu64 " rx_repost_calls=%" PRIu64
		" rx_repost_frames=%" PRIu64 " rx_repost_errors=%" PRIu64,
		(uint64_t)stats.rx_callbacks, (uint64_t)stats.rx_data_more,
		(uint64_t)stats.rx_data, (uint64_t)stats.rx_close,
		(uint64_t)stats.rx_events, (uint64_t)stats.rx_repost_calls,
		(uint64_t)stats.rx_repost_frames,
		(uint64_t)stats.rx_repost_errors);
	fprintf(stderr,
		" reap_calls=%" PRIu64 " reaped_events=%" PRIu64
		" event_drops=%" PRIu64 " crc_errors=%" PRIu64
		" overrun_errors=%" PRIu64 " canceled_callbacks=%" PRIu64
		" failures=%" PRIu64,
		(uint64_t)stats.reap_calls, (uint64_t)stats.reaped_events,
		(uint64_t)stats.event_drops, (uint64_t)stats.crc_errors,
		(uint64_t)stats.overrun_errors,
		(uint64_t)stats.canceled_callbacks, (uint64_t)stats.failures);
	fprintf(stderr,
		" tx_prod=%" PRIu64 " tx_cons=%" PRIu64
		" rx_prod=%" PRIu64 " rx_cons=%" PRIu64
		" tx_pending=%u tx_done=%u rx_partial_frames=%u"
		" rx_partial_bytes=%u fifo_len=%u fifo_avail=%u\n",
		(uint64_t)stats.tx_prod, (uint64_t)stats.tx_cons,
		(uint64_t)stats.rx_prod, (uint64_t)stats.rx_cons,
		stats.tx_pending, stats.tx_done, stats.rx_partial_frames,
		stats.rx_partial_bytes, stats.fifo_len, stats.fifo_avail);
	dump_ring_stats(z, label, "tx", &stats.tx);
	dump_ring_stats(z, label, "rx", &stats.rx);
	fflush(stderr);
	return 0;
}

static const char *kick_ring_name(uint32_t ring)
{
	return ring == TBSTREAM_ZC_KICK_TX ? "tx" : "rx";
}

static const char *wait_target_name(uint32_t ring)
{
	return ring == TBSTREAM_ZC_KICK_TX ? "tx-done" : "rx-or-close";
}

static const char *kick_error_reason(int error)
{
	switch (error) {
	case ENOTTY:
		return "kernel_does_not_support_diagnostic_UAPI";
	case EACCES:
		return "kernel_parameter_zc_diagnostic_kick_is_disabled";
	case EPERM:
		return "requires_CAP_SYS_RAWIO";
	default:
		return "unspecified";
	}
}

static int kick_kernel_ring(struct zc_state *z, uint32_t ring)
{
	struct tbstream_zc_kick kick = {
		.rings = ring,
	};

	if (ioctl(z->fd, TBSTREAM_ZC_KICK, &kick)) {
		const int error = errno;
		const bool denied = error == EACCES || error == EPERM;

		gate_log(z, error == ENOTTY ? "kick-unavailable" :
			 denied ? "kick-denied" : "kick-error",
			 "ioctl=TBSTREAM_ZC_KICK ring=%s mask=0x%x errno=%d "
			 "error=%s reason=%s",
			 kick_ring_name(kick.rings), kick.rings, error,
			 strerror(error), kick_error_reason(error));
		errno = error;
		return -1;
	}
	gate_log(z, "kick-issued", "ioctl=TBSTREAM_ZC_KICK ring=%s mask=0x%x",
		 kick_ring_name(kick.rings), kick.rings);
	return 0;
}

static void settle_after_kick(struct zc_state *z)
{
	struct timespec remaining = {
		.tv_nsec = (long)GATE_KICK_SETTLE_MS * 1000000L,
	};

	while (nanosleep(&remaining, &remaining)) {
		if (errno != EINTR) {
			gate_log(z, "kick-settle-error", "errno=%d error=%s",
				 errno, strerror(errno));
			return;
		}
	}
	gate_log(z, "kick-settled", "settle_ms=%u", GATE_KICK_SETTLE_MS);
}

static void diagnose_timeout(struct zc_state *z, uint32_t ring)
{
	gate_log(z, "kick-selection",
		 "expected=%s selected_ring=%s mask=0x%x enabled=%u",
		 wait_target_name(ring), kick_ring_name(ring), ring,
		 z->kick_on_timeout);
	dump_kernel_stats(z, "timeout-pre-kick");
	if (!z->kick_on_timeout)
		return;
	if (!kick_kernel_ring(z, ring))
		settle_after_kick(z);
	dump_kernel_stats(z, "timeout-post-kick");
}

static int wait_readable(struct zc_state *z, uint32_t ring)
{
	struct pollfd pfd = {
		.fd = z->fd,
		.events = POLLIN,
	};

	for (;;) {
		z->wait_count++;
		gate_log(z, "wait-begin", "expected=%s timeout_ms=%u",
			 wait_target_name(ring), z->timeout_ms);
		int rc = poll(&pfd, 1, (int)z->timeout_ms);

		if (rc < 0 && errno == EINTR)
			continue;
		if (rc < 0) {
			perror("poll");
			return -1;
		}
		if (!rc) {
			z->timed_out = true;
			gate_log(z, "timeout", "expected=%s timeout_ms=%u",
				 wait_target_name(ring), z->timeout_ms);
			diagnose_timeout(z, ring);
			errno = ETIMEDOUT;
			return -1;
		}
		if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
			gate_log(z, "poll-error", "revents=0x%x",
				 (unsigned)pfd.revents);
			dump_kernel_stats(z, "poll-error");
			errno = EIO;
			return -1;
		}
		if (pfd.revents & POLLIN) {
			gate_log(z, "wait-ready", "revents=0x%x",
				 (unsigned)pfd.revents);
			return 0;
		}
	}
}

static int reap_one(struct zc_state *z, struct tbstream_zc_event *event,
		    uint32_t ring)
{
	struct tbstream_zc_reap reap = {
		.max = 1,
		.events = (uint64_t)(uintptr_t)event,
	};

	if (wait_readable(z, ring))
		return -1;
	for (;;) {
		int count = ioctl(z->fd, TBSTREAM_ZC_REAP, &reap);

		if (count < 0 && errno == EINTR)
			continue;
		if (count < 0) {
			perror("TBSTREAM_ZC_REAP");
			return -1;
		}
		if (count != 1) {
			fprintf(stderr, "TBSTREAM_ZC_REAP returned %d, expected 1\n",
				count);
			errno = EPROTO;
			return -1;
		}
		if (event->type == TBSTREAM_ZC_EV_RX &&
		    add_frames(&z->rx_received_frames, event->nframes)) {
			fprintf(stderr, "RX frame counter overflow\n");
			return -1;
		}
		gate_log(z, "reap",
			 "type=%s type_id=%u first=%u frames=%u bytes=%u",
			 event_type_name(event->type), event->type, event->first,
			 event->nframes, event->bytes);
		return 0;
	}
}

static int account_tx_done(struct zc_state *z,
			   const struct tbstream_zc_event *event)
{
	if (!z->tx_outstanding) {
		fprintf(stderr, "unexpected TX_DONE with no outstanding TX\n");
		errno = EPROTO;
		return -1;
	}
	if (event->first != z->tx_first || event->nframes != z->tx_frames ||
	    event->bytes != 0) {
		fprintf(stderr,
			"TX_DONE mismatch: first=%u/%u frames=%u/%u bytes=%u/0\n",
			event->first, z->tx_first, event->nframes, z->tx_frames,
			event->bytes);
		errno = EPROTO;
		return -1;
	}
	z->tx_outstanding = false;
	if (add_frames(&z->tx_completed_frames, event->nframes)) {
		fprintf(stderr, "TX completion frame counter overflow\n");
		return -1;
	}
	gate_log(z, "tx-complete", "first=%u frames=%u", event->first,
		 event->nframes);
	return 0;
}

static int wait_tx_done(struct zc_state *z)
{
	while (z->tx_outstanding) {
		struct tbstream_zc_event event;

		memset(&event, 0, sizeof(event));
		if (reap_one(z, &event, TBSTREAM_ZC_KICK_TX))
			return -1;
		switch (event.type) {
		case TBSTREAM_ZC_EV_TX_DONE:
			if (account_tx_done(z, &event))
				return -1;
			break;
		case TBSTREAM_ZC_EV_RX:
			if (z->pending_rx) {
				fprintf(stderr, "more than one RX arrived before TX_DONE\n");
				errno = EPROTO;
				return -1;
			}
			z->pending_rx_event = event;
			z->pending_rx = true;
			break;
		case TBSTREAM_ZC_EV_CLOSE:
			fprintf(stderr, "peer closed before TX_DONE\n");
			errno = ECONNRESET;
			return -1;
		default:
			fprintf(stderr, "unknown NHI event type %u\n", event.type);
			errno = EPROTO;
			return -1;
		}
	}
	return 0;
}

static int receive_event(struct zc_state *z, struct tbstream_zc_event *rx)
{
	if (z->pending_rx) {
		*rx = z->pending_rx_event;
		z->pending_rx = false;
		return 0;
	}
	for (;;) {
		struct tbstream_zc_event event;

		memset(&event, 0, sizeof(event));
		if (reap_one(z, &event, TBSTREAM_ZC_KICK_RX))
			return -1;
		switch (event.type) {
		case TBSTREAM_ZC_EV_RX:
			*rx = event;
			return 0;
		case TBSTREAM_ZC_EV_TX_DONE:
			if (account_tx_done(z, &event))
				return -1;
			break;
		case TBSTREAM_ZC_EV_CLOSE:
			fprintf(stderr, "peer closed while waiting for RX\n");
			errno = ECONNRESET;
			return -1;
		default:
			fprintf(stderr, "unknown NHI event type %u\n", event.type);
			errno = EPROTO;
			return -1;
		}
	}
}

static int post_rx(struct zc_state *z, uint32_t nframes)
{
	if (ioctl(z->fd, TBSTREAM_ZC_POST_RX, &nframes)) {
		perror("TBSTREAM_ZC_POST_RX");
		return -1;
	}
	if (add_frames(&z->rx_reposted_frames, nframes)) {
		fprintf(stderr, "RX repost frame counter overflow\n");
		return -1;
	}
	return 0;
}

static int send_message(struct zc_state *z, enum message_kind kind,
			uint32_t sequence, uint32_t nframes,
			uint32_t last_len)
{
	struct tbstream_zc_tx tx = {
		.nframes = nframes,
		.last_len = last_len,
	};
	const uint32_t expected_first = z->tx_next;

	if (z->tx_outstanding) {
		fprintf(stderr, "attempted a second TX before exact TX_DONE\n");
		errno = EBUSY;
		return -1;
	}
	if (fill_message(z->tx, z->ring_size, z->frame_size, expected_first,
			 kind, sequence, nframes, last_len))
		return -1;
	atomic_thread_fence(memory_order_release);
	if (ioctl(z->fd, TBSTREAM_ZC_SUBMIT_TX, &tx)) {
		perror("TBSTREAM_ZC_SUBMIT_TX");
		return -1;
	}
	if (tx.first != expected_first) {
		fprintf(stderr, "TX cursor mismatch: kernel=%u expected=%u\n",
			tx.first, expected_first);
		errno = EPROTO;
		return -1;
	}
	z->tx_first = tx.first;
	z->tx_frames = nframes;
	z->tx_outstanding = true;
	z->tx_next = (z->tx_next + nframes) % z->ring_size;
	if (add_frames(&z->tx_submitted_frames, nframes)) {
		fprintf(stderr, "TX submission frame counter overflow\n");
		return -1;
	}
	gate_log(z, "tx-submit",
		 "kind=%s first=%u frames=%u last_len=%u bytes=%zu",
		 kind == MESSAGE_REQUEST ? "request" : "response", tx.first,
		 nframes, last_len,
		 message_bytes(z->frame_size, nframes, last_len));
	return 0;
}

static int receive_message(struct zc_state *z, enum message_kind kind,
			   uint32_t sequence, uint32_t nframes,
			   uint32_t last_len)
{
	struct tbstream_zc_event event;
	const uint32_t bytes =
		(uint32_t)message_bytes(z->frame_size, nframes, last_len);

	if (receive_event(z, &event))
		return -1;
	if (event.first != z->rx_next || event.nframes != nframes ||
	    event.bytes != bytes) {
		fprintf(stderr,
			"RX geometry mismatch: kind=%u seq=%u first=%u/%u "
			"frames=%u/%u bytes=%u/%u\n",
			(unsigned)kind, sequence, event.first, z->rx_next,
			event.nframes, nframes, event.bytes, bytes);
		errno = EPROTO;
		return -1;
	}
	atomic_thread_fence(memory_order_acquire);
	if (verify_message(z->rx, z->ring_size, z->frame_size, event.first,
			   kind, sequence, nframes, last_len)) {
		errno = EILSEQ;
		return -1;
	}
	gate_log(z, "rx-verified",
		 "kind=%s first=%u frames=%u last_len=%u bytes=%u",
		 kind == MESSAGE_REQUEST ? "request" : "response", event.first,
		 event.nframes, last_len, event.bytes);
	if (post_rx(z, event.nframes))
		return -1;
	z->rx_next = (z->rx_next + event.nframes) % z->ring_size;
	gate_log(z, "rx-repost", "first=%u frames=%u", event.first,
		 event.nframes);
	return 0;
}

static int delay_ms(uint32_t milliseconds)
{
	struct timespec remaining = {
		.tv_sec = milliseconds / 1000u,
		.tv_nsec = (long)(milliseconds % 1000u) * 1000000L,
	};

	while (nanosleep(&remaining, &remaining)) {
		if (errno != EINTR) {
			perror("nanosleep");
			return -1;
		}
	}
	return 0;
}

static void timeout_hold_signal(int signal_number)
{
	(void)signal_number;
	release_timeout_hold = 1;
}

static void hold_after_timeout(struct zc_state *z)
{
	struct sigaction action;
	struct sigaction old_interrupt;
	struct sigaction old_terminate;
	bool interrupt_installed = false;
	bool terminate_installed = false;

	memset(&action, 0, sizeof(action));
	action.sa_handler = timeout_hold_signal;
	sigemptyset(&action.sa_mask);
	release_timeout_hold = 0;
	if (!sigaction(SIGINT, &action, &old_interrupt))
		interrupt_installed = true;
	else
		perror("sigaction(SIGINT)");
	if (!sigaction(SIGTERM, &action, &old_terminate))
		terminate_installed = true;
	else
		perror("sigaction(SIGTERM)");

	gate_log(z, "timeout-hold-begin",
		 "pid=%ld release_with=SIGINT_or_SIGTERM", (long)getpid());
	while (!release_timeout_hold) {
		struct timespec interval = {
			.tv_sec = 1,
		};

		nanosleep(&interval, NULL);
	}
	gate_log(z, "timeout-hold-end", "pid=%ld", (long)getpid());

	if (interrupt_installed)
		sigaction(SIGINT, &old_interrupt, NULL);
	if (terminate_installed)
		sigaction(SIGTERM, &old_terminate, NULL);
}

static int run_initiator(const char *device, uint32_t exchanges,
			 uint32_t turnaround_delay_ms, uint32_t timeout_ms,
			 bool hold_on_timeout, bool kick_on_timeout)
{
	struct zc_state z;
	int failure_errno = 0;
	int rc = 1;

	if (zc_open(&z, device, timeout_ms, "initiator", exchanges,
		    hold_on_timeout, kick_on_timeout))
		goto out;
	for (uint32_t sequence = 0; sequence < exchanges; sequence++) {
		const uint32_t frames =
			request_frames[sequence %
				       (sizeof(request_frames) /
					sizeof(request_frames[0]))];

		enter_phase(&z, sequence, "submit-request");
		if (send_message(&z, MESSAGE_REQUEST, sequence, frames,
				 GATE_REQUEST_LAST_LEN))
			goto out;
		enter_phase(&z, sequence, "wait-request-tx-done");
		if (wait_tx_done(&z))
			goto out;
		enter_phase(&z, sequence, "wait-response-rx");
		if (receive_message(&z, MESSAGE_RESPONSE, sequence,
				    GATE_RESPONSE_FRAMES,
				    GATE_RESPONSE_LAST_LEN))
			goto out;
		fprintf(stderr,
			"exchange %u/%u: request=%u frames response=%u frames "
			"verified\n",
			sequence + 1u, exchanges, frames, GATE_RESPONSE_FRAMES);
		enter_phase(&z, sequence, "exchange-complete");
		if (sequence + 1u < exchanges && turnaround_delay_ms) {
			enter_phase(&z, sequence, "turnaround-delay");
			if (delay_ms(turnaround_delay_ms))
				goto out;
		}
	}
	z.phase = "complete";
	gate_log(&z, "pass", NULL);
	printf("PASS: initiator completed %u DS4-shaped zero-copy exchanges\n",
	       exchanges);
	rc = 0;

out:
	failure_errno = errno;
	if (rc && z.hold_on_timeout && z.timed_out)
		hold_after_timeout(&z);
	if (rc)
		gate_log(&z, "fail", "errno=%d error=%s", failure_errno,
			 failure_errno ? strerror(failure_errno) : "unspecified");
	zc_close(&z);
	errno = failure_errno;
	return rc;
}

static int run_responder(const char *device, uint32_t exchanges,
			 uint32_t compute_delay_ms, uint32_t timeout_ms,
			 bool hold_on_timeout, bool kick_on_timeout)
{
	struct zc_state z;
	int failure_errno = 0;
	int rc = 1;

	if (zc_open(&z, device, timeout_ms, "responder", exchanges,
		    hold_on_timeout, kick_on_timeout))
		goto out;
	printf("READY: responder armed for %u DS4-shaped exchanges\n",
	       exchanges);
	fflush(stdout);
	for (uint32_t sequence = 0; sequence < exchanges; sequence++) {
		const uint32_t frames =
			request_frames[sequence %
				       (sizeof(request_frames) /
					sizeof(request_frames[0]))];

		enter_phase(&z, sequence, "wait-request-rx");
		if (receive_message(&z, MESSAGE_REQUEST, sequence, frames,
				    GATE_REQUEST_LAST_LEN))
			goto out;
		if (compute_delay_ms) {
			enter_phase(&z, sequence, "compute-delay");
			if (delay_ms(compute_delay_ms))
				goto out;
		}
		enter_phase(&z, sequence, "submit-response");
		if (send_message(&z, MESSAGE_RESPONSE, sequence,
				 GATE_RESPONSE_FRAMES,
				 GATE_RESPONSE_LAST_LEN))
			goto out;
		enter_phase(&z, sequence, "wait-response-tx-done");
		if (wait_tx_done(&z))
			goto out;
		fprintf(stderr,
			"exchange %u/%u: request=%u frames response=%u frames "
			"verified\n",
			sequence + 1u, exchanges, frames, GATE_RESPONSE_FRAMES);
		enter_phase(&z, sequence, "exchange-complete");
	}
	z.phase = "complete";
	gate_log(&z, "pass", NULL);
	printf("PASS: responder completed %u DS4-shaped zero-copy exchanges\n",
	       exchanges);
	rc = 0;

out:
	failure_errno = errno;
	if (rc && z.hold_on_timeout && z.timed_out)
		hold_after_timeout(&z);
	if (rc)
		gate_log(&z, "fail", "errno=%d error=%s", failure_errno,
			 failure_errno ? strerror(failure_errno) : "unspecified");
	zc_close(&z);
	errno = failure_errno;
	return rc;
}

static int run_selftest(void)
{
	const uint32_t ring_size = 257u;
	const uint32_t frame_size = TBSTREAM_ZC_FRAME_SIZE;
	const size_t pool_bytes = (size_t)ring_size * frame_size;
	unsigned char *pool = calloc(1, pool_bytes);
	uint64_t request_frame_total = 0;
	uint64_t response_frame_total = 0;
	int rc = 1;

	if (!pool) {
		perror("calloc");
		return 1;
	}
	for (uint32_t sequence = 0; sequence < 8u; sequence++) {
		const uint32_t frames =
			request_frames[sequence %
				       (sizeof(request_frames) /
					sizeof(request_frames[0]))];
		const uint32_t first = ring_size - 3u + sequence;

		if (fill_message(pool, ring_size, frame_size, first % ring_size,
				 MESSAGE_REQUEST, sequence, frames,
				 GATE_REQUEST_LAST_LEN) ||
		    verify_message(pool, ring_size, frame_size,
				   first % ring_size, MESSAGE_REQUEST, sequence,
				   frames, GATE_REQUEST_LAST_LEN))
			goto out;
	}
	if (fill_message(pool, ring_size, frame_size, 200u, MESSAGE_RESPONSE,
			 19u, GATE_RESPONSE_FRAMES, GATE_RESPONSE_LAST_LEN) ||
	    verify_message(pool, ring_size, frame_size, 200u, MESSAGE_RESPONSE,
			   19u, GATE_RESPONSE_FRAMES,
			   GATE_RESPONSE_LAST_LEN))
		goto out;
	for (uint32_t exchange = 0; exchange < GATE_DEFAULT_EXCHANGES;
	     exchange++) {
		const uint32_t frames =
			request_frames[exchange %
				       (sizeof(request_frames) /
					sizeof(request_frames[0]))];

		if (add_frames(&request_frame_total, frames) ||
		    add_frames(&response_frame_total, GATE_RESPONSE_FRAMES))
			goto out;
	}
	if (request_frame_total != 1056u || response_frame_total != 4064u) {
		fprintf(stderr,
			"cumulative frame self-test failed: request=%" PRIu64
			"/1056 response=%" PRIu64 "/4064\n",
			request_frame_total, response_frame_total);
		goto out;
	}
	printf("PASS: deterministic envelope/payload wrap self-test\n");
	printf("PASS: deterministic cumulative frame counter self-test\n");
	rc = 0;

out:
	free(pool);
	return rc;
}

static uint32_t parse_u32(const char *text, const char *name,
			  uint32_t minimum, uint32_t maximum)
{
	char *end = NULL;
	unsigned long long value;

	errno = 0;
	value = strtoull(text, &end, 10);
	if (errno || end == text || *end || value < minimum ||
	    value > maximum) {
		fprintf(stderr, "invalid %s: %s (expected %u..%u)\n",
			name, text, minimum, maximum);
		exit(2);
	}
	return (uint32_t)value;
}

static void usage(const char *program)
{
	fprintf(stderr,
		"usage: %s initiator|responder [options]\n"
		"       %s selftest\n"
		"  -d, --device PATH              device (default /dev/tbstream0)\n"
		"  -n, --exchanges N              exchanges, at least 32 "
		"(default 32)\n"
		"  -c, --compute-delay-ms MS      responder delay before response\n"
		"  -a, --turnaround-delay-ms MS   initiator delay before next request\n"
		"  -t, --timeout-ms MS            per-event timeout (default 30000)\n"
		"      --hold-on-timeout          keep fd/mapping open until signal\n"
		"      --kick-on-timeout          kick the stalled ring work once\n",
		program, program);
}

int main(int argc, char **argv)
{
	enum {
		OPT_HOLD_ON_TIMEOUT = 1000,
		OPT_KICK_ON_TIMEOUT,
	};
	static const struct option options[] = {
		{"device", required_argument, NULL, 'd'},
		{"exchanges", required_argument, NULL, 'n'},
		{"compute-delay-ms", required_argument, NULL, 'c'},
		{"turnaround-delay-ms", required_argument, NULL, 'a'},
		{"timeout-ms", required_argument, NULL, 't'},
		{"hold-on-timeout", no_argument, NULL, OPT_HOLD_ON_TIMEOUT},
		{"kick-on-timeout", no_argument, NULL, OPT_KICK_ON_TIMEOUT},
		{"help", no_argument, NULL, 'h'},
		{NULL, 0, NULL, 0},
	};
	const char *device = "/dev/tbstream0";
	uint32_t exchanges = GATE_DEFAULT_EXCHANGES;
	uint32_t compute_delay_ms = 0;
	uint32_t turnaround_delay_ms = 0;
	uint32_t timeout_ms = GATE_DEFAULT_TIMEOUT_MS;
	bool hold_on_timeout = false;
	bool kick_on_timeout = false;
	bool initiator;
	int option;

	if (argc == 2 && !strcmp(argv[1], "selftest"))
		return run_selftest();
	if (argc < 2 || (strcmp(argv[1], "initiator") &&
			 strcmp(argv[1], "responder"))) {
		usage(argv[0]);
		return 2;
	}
	initiator = !strcmp(argv[1], "initiator");
	optind = 2;
	while ((option = getopt_long(argc, argv, "d:n:c:a:t:h", options,
				     NULL)) != -1) {
		switch (option) {
		case 'd':
			device = optarg;
			break;
		case 'n':
			exchanges = parse_u32(optarg, "exchange count",
					      GATE_DEFAULT_EXCHANGES,
					      UINT32_MAX);
			break;
		case 'c':
			compute_delay_ms = parse_u32(optarg, "compute delay", 0,
					     GATE_MAX_DELAY_MS);
			break;
		case 'a':
			turnaround_delay_ms = parse_u32(optarg,
						"turnaround delay", 0,
						GATE_MAX_DELAY_MS);
			break;
		case 't':
			timeout_ms = parse_u32(optarg, "timeout", 1, INT_MAX);
			break;
		case OPT_HOLD_ON_TIMEOUT:
			hold_on_timeout = true;
			break;
		case OPT_KICK_ON_TIMEOUT:
			kick_on_timeout = true;
			break;
		case 'h':
			usage(argv[0]);
			return 0;
		default:
			usage(argv[0]);
			return 2;
		}
	}
	if (optind != argc) {
		usage(argv[0]);
		return 2;
	}
	if (initiator && compute_delay_ms) {
		fprintf(stderr,
			"--compute-delay-ms applies only to the responder\n");
		return 2;
	}
	if (!initiator && turnaround_delay_ms) {
		fprintf(stderr,
			"--turnaround-delay-ms applies only to the initiator\n");
		return 2;
	}

	return initiator ?
		run_initiator(device, exchanges, turnaround_delay_ms, timeout_ms,
			      hold_on_timeout, kick_on_timeout) :
		run_responder(device, exchanges, compute_delay_ms, timeout_ms,
			      hold_on_timeout, kick_on_timeout);
}
