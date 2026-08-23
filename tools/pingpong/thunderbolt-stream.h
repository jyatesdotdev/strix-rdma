/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Zero-copy slot interface for Thunderbolt/USB4 stream devices
 *
 * A stream device switched into zero-copy mode exposes its fixed,
 * DMA-mapped TX and RX frame pools directly to userspace through
 * mmap(). Messages are submitted and reaped by frame-pool index, so
 * the payload is never copied by the kernel.
 *
 * Frame pools are rings of ring_size frames of TBSTREAM_ZC_FRAME_SIZE
 * bytes each, consumed strictly in order:
 *
 *  - TX: userspace writes payload into the next free frames (its own
 *    cursor, starting at index 0 and wrapping at ring_size) and calls
 *    TBSTREAM_ZC_SUBMIT_TX. Frames become free again when the matching
 *    TBSTREAM_ZC_EV_TX_DONE event is reaped. At most ring_size - 1
 *    frames may be in flight.
 *  - RX: all frames start posted to the hardware. A received message
 *    is reported as a TBSTREAM_ZC_EV_RX event covering nframes frames
 *    starting at index first. After consuming the payload userspace
 *    returns the frames with TBSTREAM_ZC_POST_RX. Frames are reposted
 *    in consumption order.
 *
 * A message spans one or more frames; every frame except the last
 * carries a full TBSTREAM_ZC_FRAME_SIZE bytes of payload. Both sides
 * of the tunnel must be in zero-copy mode; this is negotiated out of
 * band by the application.
 */

#ifndef _UAPI_LINUX_THUNDERBOLT_STREAM_H
#define _UAPI_LINUX_THUNDERBOLT_STREAM_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define TBSTREAM_ZC_FRAME_SIZE	4096

/**
 * struct tbstream_zc_info - Geometry of the zero-copy pools
 * @ring_size: Number of frames in each pool
 * @frame_size: Bytes per frame (%TBSTREAM_ZC_FRAME_SIZE)
 * @tx_pool_offset: mmap() file offset of the first TX frame
 * @rx_pool_offset: mmap() file offset of the first RX frame
 *
 * The whole mapping is 2 * @ring_size * @frame_size bytes starting at
 * file offset 0.
 */
struct tbstream_zc_info {
	__u32 ring_size;
	__u32 frame_size;
	__u64 tx_pool_offset;
	__u64 rx_pool_offset;
};

/**
 * enum tbstream_zc_event_type - Completion event types
 * @TBSTREAM_ZC_EV_RX: Message received into the RX pool
 * @TBSTREAM_ZC_EV_TX_DONE: Previously submitted message left the ring
 * @TBSTREAM_ZC_EV_CLOSE: Peer closed the stream
 */
enum tbstream_zc_event_type {
	TBSTREAM_ZC_EV_RX = 0,
	TBSTREAM_ZC_EV_TX_DONE = 1,
	TBSTREAM_ZC_EV_CLOSE = 2,
};

/**
 * struct tbstream_zc_event - One completion event
 * @type: One of &enum tbstream_zc_event_type
 * @first: Pool index of the first frame of the message
 * @nframes: Number of frames the message occupies
 * @bytes: Total payload bytes (RX only, 0 otherwise)
 */
struct tbstream_zc_event {
	__u32 type;
	__u32 first;
	__u32 nframes;
	__u32 bytes;
};

/**
 * struct tbstream_zc_tx - TBSTREAM_ZC_SUBMIT_TX argument
 * @nframes: Frames making up the message (>= 1)
 * @last_len: Payload bytes in the final frame (1..frame_size)
 * @first: Filled by the kernel: pool index of the first frame used
 * @reserved: Must be 0
 */
struct tbstream_zc_tx {
	__u32 nframes;
	__u32 last_len;
	__u32 first;
	__u32 reserved;
};

/**
 * struct tbstream_zc_reap - TBSTREAM_ZC_REAP argument
 * @max: Capacity of the @events array
 * @flags: %TBSTREAM_ZC_REAP_NONBLOCK or 0
 * @events: Userspace pointer to struct tbstream_zc_event[]
 *
 * Blocks until at least one event is available unless
 * %TBSTREAM_ZC_REAP_NONBLOCK is set or the file is O_NONBLOCK.
 * Returns the number of events written.
 */
struct tbstream_zc_reap {
	__u32 max;
	__u32 flags;
	__u64 events;
};

#define TBSTREAM_ZC_REAP_NONBLOCK	0x1

/**
 * struct tbstream_zc_rx - Flagged RX repost request
 * @nframes: Frames to return to the RX ring
 * @flags: %TBSTREAM_ZC_RX_F_INTERRUPT_BOUNDARIES or 0
 *
 * INTERRUPT_BOUNDARIES is intended for fixed-size message streams. It
 * suppresses interrupts on every reposted frame except the first and last.
 * The final interrupt reports a complete message; the first ensures a
 * following one-frame CLOSE is observed. Applications with variable message
 * geometry should use the legacy unflagged POST_RX operation.
 */
struct tbstream_zc_rx {
	__u32 nframes;
	__u32 flags;
};

#define TBSTREAM_ZC_RX_F_INTERRUPT_BOUNDARIES	0x1
#define TBSTREAM_ZC_RX_F_INTERRUPT_LAST \
	TBSTREAM_ZC_RX_F_INTERRUPT_BOUNDARIES /* source compatibility */

/**
 * struct tbstream_zc_ring_stats - Diagnostic snapshot of one NHI ring
 * @descriptors_posted: Descriptors posted to the NHI since ring allocation
 * @descriptors_completed: Completed descriptors harvested by ring work
 * @interrupts: Interrupts observed for this ring
 * @work_runs: Ring work executions
 * @kick_requests: Explicit diagnostic kick requests
 * @kick_pending: Kicks that found a completed descriptor awaiting harvest
 * @flags: %TBSTREAM_ZC_RING_F_* flags
 * @hop: Local NHI ring HopID
 * @irq: Linux MSI-X IRQ, or 0 when the shared MSI path is used
 * @vector: MSI-X vector index, or 0 for shared MSI
 * @size: Descriptor count
 * @sw_head: Next descriptor software will post
 * @sw_tail: Next descriptor software will complete
 * @hw_posted: NHI-visible posted-descriptor index
 * @hw_completed: NHI-visible completed-descriptor index
 * @queued: Frames waiting for descriptor space
 * @in_flight: Frames owned by the NHI
 * @tail_flags: Current software-tail descriptor flags
 * @tail_length: Current software-tail descriptor length
 * @tail_eof: Current software-tail descriptor EOF/PDF value
 * @tail_sof: Current software-tail descriptor SOF/PDF value
 * @options: Raw NHI ring options register
 * @interval_nsec: Configured interrupt-throttling interval
 * @reserved: Must be ignored
 *
 * Hardware and software can advance while this structure is copied. Fields
 * are one lock-consistent ring snapshot, not a cross-ring transaction.
 */
struct tbstream_zc_ring_stats {
	__aligned_u64 descriptors_posted;
	__aligned_u64 descriptors_completed;
	__aligned_u64 interrupts;
	__aligned_u64 work_runs;
	__aligned_u64 kick_requests;
	__aligned_u64 kick_pending;
	__u32 flags;
	__s32 hop;
	__s32 irq;
	__u32 vector;
	__u32 size;
	__u32 sw_head;
	__u32 sw_tail;
	__u32 hw_posted;
	__u32 hw_completed;
	__u32 queued;
	__u32 in_flight;
	__u32 tail_flags;
	__u32 tail_length;
	__u32 tail_eof;
	__u32 tail_sof;
	__u32 options;
	__u32 interval_nsec;
	__u32 reserved[2];
};

#define TBSTREAM_ZC_RING_F_TX			0x1
#define TBSTREAM_ZC_RING_F_RUNNING		0x2
#define TBSTREAM_ZC_RING_F_MSIX			0x4
#define TBSTREAM_ZC_RING_F_INTERRUPT_ENABLED	0x8
#define TBSTREAM_ZC_RING_F_TAIL_COMPLETED	0x10
#define TBSTREAM_ZC_RING_F_HW_VALID		0x20

/**
 * struct tbstream_zc_stats - Zero-copy and NHI progress snapshot
 * @version: %TBSTREAM_ZC_STATS_VERSION
 * @struct_size: Size of this structure in bytes
 * @tx_submit_calls: Successful or failed TX submit attempts
 * @tx_submit_frames: TX frames requested by submit calls
 * @tx_callbacks: Non-canceled TX frame callbacks
 * @tx_terminal_callbacks: TX callbacks carrying a message-ending PDF
 * @tx_events: TX completion events queued
 * @tx_enqueue_errors: TX frame enqueue failures
 * @rx_callbacks: Non-canceled RX frame callbacks
 * @rx_data_more: RX callbacks carrying a continuation PDF
 * @rx_data: RX callbacks carrying a message-ending DATA PDF
 * @rx_close: RX callbacks carrying CLOSE
 * @rx_events: RX or CLOSE events queued
 * @rx_repost_calls: RX repost attempts
 * @rx_repost_frames: RX frames requested by repost calls
 * @rx_repost_errors: RX frame enqueue failures during repost
 * @reap_calls: Event reap attempts
 * @reaped_events: Events successfully copied and dequeued
 * @event_drops: Events that could not be queued
 * @crc_errors: RX descriptors reporting CRC errors
 * @overrun_errors: RX descriptors reporting buffer overrun
 * @canceled_callbacks: Ring callbacks canceled during shutdown
 * @failures: Times zero-copy mode entered its terminal error state
 * @tx_prod: Stream TX producer cursor
 * @tx_cons: Stream TX consumer cursor
 * @rx_prod: Stream RX producer cursor
 * @rx_cons: Stream RX consumer cursor
 * @tx_pending: Frames accumulated toward the next TX completion event
 * @tx_done: Frames represented by TX completion events
 * @rx_partial_frames: Frames accumulated toward the next RX message event
 * @rx_partial_bytes: Bytes accumulated toward the next RX message event
 * @fifo_len: Events currently queued
 * @fifo_avail: Unused event slots
 * @flags: %TBSTREAM_ZC_STATS_F_* flags
 * @in_hopid: Stream input path HopID
 * @out_hopid: Stream output path HopID
 * @throttling: Configured interrupt-throttling interval in ns
 * @last_error: First terminal zero-copy error, a %TBSTREAM_ZC_ERROR_* value
 * @reserved: Must be ignored
 * @tx: Transmit NHI ring snapshot
 * @rx: Receive NHI ring snapshot
 */
struct tbstream_zc_stats {
	__u32 version;
	__u32 struct_size;
	__aligned_u64 tx_submit_calls;
	__aligned_u64 tx_submit_frames;
	__aligned_u64 tx_callbacks;
	__aligned_u64 tx_terminal_callbacks;
	__aligned_u64 tx_events;
	__aligned_u64 tx_enqueue_errors;
	__aligned_u64 rx_callbacks;
	__aligned_u64 rx_data_more;
	__aligned_u64 rx_data;
	__aligned_u64 rx_close;
	__aligned_u64 rx_events;
	__aligned_u64 rx_repost_calls;
	__aligned_u64 rx_repost_frames;
	__aligned_u64 rx_repost_errors;
	__aligned_u64 reap_calls;
	__aligned_u64 reaped_events;
	__aligned_u64 event_drops;
	__aligned_u64 crc_errors;
	__aligned_u64 overrun_errors;
	__aligned_u64 canceled_callbacks;
	__aligned_u64 failures;
	__aligned_u64 tx_prod;
	__aligned_u64 tx_cons;
	__aligned_u64 rx_prod;
	__aligned_u64 rx_cons;
	__u32 tx_pending;
	__u32 tx_done;
	__u32 rx_partial_frames;
	__u32 rx_partial_bytes;
	__u32 fifo_len;
	__u32 fifo_avail;
	__u32 flags;
	__s32 in_hopid;
	__s32 out_hopid;
	__u32 throttling;
	__u32 last_error;
	__u32 reserved[4];
	struct tbstream_zc_ring_stats tx;
	struct tbstream_zc_ring_stats rx;
};

#define TBSTREAM_ZC_STATS_VERSION	1
#define TBSTREAM_ZC_STATS_F_FAILED	0x1
#define TBSTREAM_ZC_STATS_F_CLOSED	0x2
#define TBSTREAM_ZC_STATS_F_REMOVED	0x4
#define TBSTREAM_ZC_STATS_F_TX_IMPORTED	0x8
#define TBSTREAM_ZC_STATS_F_RX_IMPORTED	0x10

#define TBSTREAM_ZC_ERROR_NONE		0
#define TBSTREAM_ZC_ERROR_RX_CRC	1
#define TBSTREAM_ZC_ERROR_RX_OVERRUN	2
#define TBSTREAM_ZC_ERROR_EVENT_DROP	3
#define TBSTREAM_ZC_ERROR_TX_PARTIAL	4
#define TBSTREAM_ZC_ERROR_RX_PARTIAL	5

/**
 * struct tbstream_zc_kick - Explicitly schedule normal NHI ring work
 * @rings: %TBSTREAM_ZC_KICK_* mask
 * @reserved: Must be 0
 *
 * This is a diagnostic operation. It does not alter descriptors, reset a
 * path, or bypass normal ring completion processing.
 */
struct tbstream_zc_kick {
	__u32 rings;
	__u32 reserved;
};

#define TBSTREAM_ZC_KICK_TX	0x1
#define TBSTREAM_ZC_KICK_RX	0x2

#define TBSTREAM_ZC_DMABUF_TX	1
#define TBSTREAM_ZC_DMABUF_RX	2

#define TBSTREAM_ZC_DMABUF_PROBE_VERSION	1

/**
 * struct tbstream_zc_dmabuf_probe - TBSTREAM_ZC_DMABUF_PROBE argument
 * @version: Must be %TBSTREAM_ZC_DMABUF_PROBE_VERSION
 * @flags: Must be 0
 * @fd: DMA-BUF file descriptor to probe
 * @direction: %TBSTREAM_ZC_DMABUF_TX or %TBSTREAM_ZC_DMABUF_RX
 * @offset: First byte to measure, aligned to %TBSTREAM_ZC_FRAME_SIZE
 * @length: Bytes to measure, a nonzero multiple of %TBSTREAM_ZC_FRAME_SIZE
 * @covered: Mapped bytes overlapping [@offset, @offset + @length)
 * @min_alignment: Tightest power-of-two alignment of any mapped segment
 *	start or length in bytes, or 0 when nothing was mapped
 * @largest_segment: Largest single DMA-mapped segment in bytes
 * @orig_entries: SG entries published by the exporter
 * @mapped_entries: DMA-mapped SG entries for the NHI device
 * @reserved: Must be 0
 *
 * Privileged no-traffic diagnostic. The stream attaches @fd to its NHI
 * DMA device in @direction, pins and maps it, validates that every
 * mapped segment is frame-aligned, measures coverage of the requested
 * range, and tears the mapping down. Aggregate geometry is reported
 * only after the teardown completes; no ring descriptor is ever
 * programmed and no DMA address is exposed. Requires CAP_SYS_RAWIO
 * and the thunderbolt_stream.zc_diagnostic_dmabuf module parameter.
 */
struct tbstream_zc_dmabuf_probe {
	__u32 version;
	__u32 flags;
	__s32 fd;
	__u32 direction;
	__aligned_u64 offset;
	__aligned_u64 length;
	__aligned_u64 covered;
	__aligned_u64 min_alignment;
	__aligned_u64 largest_segment;
	__u32 orig_entries;
	__u32 mapped_entries;
	__aligned_u64 reserved[4];
};

/**
 * struct tbstream_zc_import_range - One directional pool import range
 * @fd: DMA-BUF file descriptor, or -1 to keep the kernel page-backed
 *	pool for this direction
 * @flags: Must be 0
 * @offset: First byte of the pool inside the DMA-BUF, aligned to
 *	%TBSTREAM_ZC_FRAME_SIZE; must be 0 when @fd is -1
 * @length: Pool bytes; must equal ring_size * %TBSTREAM_ZC_FRAME_SIZE
 *	exactly, or 0 when @fd is -1
 */
struct tbstream_zc_import_range {
	__s32 fd;
	__u32 flags;
	__aligned_u64 offset;
	__aligned_u64 length;
};

#define TBSTREAM_ZC_IMPORT_VERSION	1

/**
 * struct tbstream_zc_import - TBSTREAM_ZC_IMPORT argument
 * @version: Must be %TBSTREAM_ZC_IMPORT_VERSION
 * @flags: Must be 0
 * @tx: TX pool source
 * @rx: RX pool source
 * @reserved: Must be 0
 *
 * Atomically selects DMA-BUF backed frame pools for the zero-copy
 * session that a following TBSTREAM_ZC_ENABLE will start. The ioctl
 * must run on an exclusively opened, not yet activated stream: before
 * any read, write, poll, mmap or enable has started the rings or
 * enabled the DMA paths. Each imported pool is attached to the NHI DMA
 * device, pinned, mapped in its fixed direction, and validated to
 * cover the whole pool with frame-aligned segments; on any failure
 * every acquired object is released in reverse order and the device is
 * left unchanged. Imported pools have no CPU mapping: mmap() leaves
 * holes for imported halves and ordinary read()/write() are rejected
 * while an import is configured. The import lasts until the final
 * close of the device. Requires CAP_SYS_RAWIO and the
 * thunderbolt_stream.zc_diagnostic_dmabuf module parameter.
 */
struct tbstream_zc_import {
	__u32 version;
	__u32 flags;
	struct tbstream_zc_import_range tx;
	struct tbstream_zc_import_range rx;
	__aligned_u64 reserved[4];
};

#define TBSTREAM_ZC_MAGIC	0xb4

#define TBSTREAM_ZC_ENABLE	_IO(TBSTREAM_ZC_MAGIC, 0x00)
#define TBSTREAM_ZC_GET_INFO	_IOR(TBSTREAM_ZC_MAGIC, 0x01, struct tbstream_zc_info)
#define TBSTREAM_ZC_SUBMIT_TX	_IOWR(TBSTREAM_ZC_MAGIC, 0x02, struct tbstream_zc_tx)
#define TBSTREAM_ZC_POST_RX	_IOW(TBSTREAM_ZC_MAGIC, 0x03, __u32)
#define TBSTREAM_ZC_REAP	_IOWR(TBSTREAM_ZC_MAGIC, 0x04, struct tbstream_zc_reap)
#define TBSTREAM_ZC_POST_RX_FLAGS \
	_IOW(TBSTREAM_ZC_MAGIC, 0x05, struct tbstream_zc_rx)
#define TBSTREAM_ZC_GET_STATS \
	_IOR(TBSTREAM_ZC_MAGIC, 0x06, struct tbstream_zc_stats)
#define TBSTREAM_ZC_KICK \
	_IOW(TBSTREAM_ZC_MAGIC, 0x07, struct tbstream_zc_kick)
#define TBSTREAM_ZC_DMABUF_PROBE \
	_IOWR(TBSTREAM_ZC_MAGIC, 0x08, struct tbstream_zc_dmabuf_probe)
#define TBSTREAM_ZC_IMPORT \
	_IOW(TBSTREAM_ZC_MAGIC, 0x09, struct tbstream_zc_import)

#endif /* _UAPI_LINUX_THUNDERBOLT_STREAM_H */
