/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef STRIX_NHI_NATIVE_WIRE_H
#define STRIX_NHI_NATIVE_WIRE_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/string.h>
typedef u8 sn_u8;
typedef u32 sn_u32;
typedef u64 sn_u64;
#else
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef uint8_t sn_u8;
typedef uint32_t sn_u32;
typedef uint64_t sn_u64;
#endif

#define SN_MAGIC 0x534e4831U /* SNH1; not compatible with USB4STREAM or TVD1 */
#define SN_VERSION 1U
#define SN_HEADER 112U
#define SN_FRAME 4096U
#define SN_PAYLOAD (SN_FRAME - SN_HEADER)
#define SN_MESSAGE (2U * 1024U * 1024U)
#define SN_QUEUE_DEPTH 4U
#define SN_MAX_CQE 64U
#define SN_MAX_MRS 4U
#define SN_MR_BYTES (8U * 1024U * 1024U)
#define SN_PIN_BYTES (32U * 1024U * 1024U)
#define SN_PSN_MAX 0xffffffU

/* Values and byte offsets are a wire contract, never a C structure layout. */
enum sn_opcode {
	SN_SEND = 1, SN_WRITE = 2, SN_READ = 3, SN_READ_REPLY = 4,
	SN_ACK = 5, SN_READ_ACK = 6, SN_CREDIT = 7,
	SN_HELLO = 8, SN_HELLO_ACK = 9, SN_BIND = 10, SN_BIND_ACK = 11,
	SN_ZC_SEND = 12, SN_ZC_READY = 13
};
/* HELLO/HELLO_ACK capability bits in total: v1 is SEND/WRITE/READ. */
#define SN_CAPS_V1 0x7U
#define SN_CAP_ZDATA 0x8U
#define SN_CAPS_V2 0xfU
#define SN_CAPS_KNOWN SN_CAPS_V2
/* Zero-copy burst states (sn_qp.zc_state). */
#define SN_ZC_NONE	0
#define SN_ZC_WAIT	1
#define SN_ZC_SENDING	2
#define SN_ZC_SENT	3
/* SENDs larger than this use the header-less data-ring path when the peer
 * advertised SN_CAP_ZDATA. */
#define SN_ZC_THRESHOLD	65536
enum sn_status {
	SN_OK = 0, SN_RNR = 1, SN_ACCESS = 2, SN_LENGTH = 3,
	SN_PROTOCOL = 4
};
struct sn_header {
	sn_u32 opcode, total, src_qpn, dst_qpn;
	sn_u64 src_generation, dst_generation, src_epoch, dst_epoch;
	sn_u64 sequence, ack_sequence;
	sn_u32 offset, length;
	sn_u64 address;
	sn_u32 rkey, status;
};
struct sn_binding {
	sn_u64 local_epoch, peer_epoch, local_generation, peer_generation;
	sn_u32 local_qpn, peer_qpn;
};
struct sn_readiness {
	sn_u64 local_epoch, peer_epoch, local_echo, peer_echo;
	unsigned int rx_primed, rings_started, paths_active, peer_ready, carrier;
};

static inline sn_u32 sn_get32(const sn_u8 *p)
{
	return (sn_u32)p[0] << 24 | (sn_u32)p[1] << 16 |
	       (sn_u32)p[2] << 8 | p[3];
}
static inline sn_u64 sn_get64(const sn_u8 *p)
{
	return (sn_u64)sn_get32(p) << 32 | sn_get32(p + 4);
}
static inline void sn_put32(sn_u8 *p, sn_u32 v)
{
	p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}
static inline void sn_put64(sn_u8 *p, sn_u64 v)
{
	sn_put32(p, v >> 32); sn_put32(p + 4, (sn_u32)v);
}
/* CRC field 96..99 is logically zero. No authentication is implied. */
static inline sn_u32 sn_crc(const sn_u8 *p, size_t n)
{
	sn_u32 crc = ~0U;
	size_t i;
	unsigned int bit;

	for (i = 0; i < n; i++) {
		crc ^= (i >= 96 && i < 100) ? 0 : p[i];
		for (bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^ (0x82f63b78U & (0U - (crc & 1)));
	}
	return ~crc;
}
static inline int sn_qp_fields_valid(const struct sn_header *h)
{
	return h->src_qpn && h->src_qpn <= SN_PSN_MAX && h->dst_qpn &&
	       h->dst_qpn <= SN_PSN_MAX && h->src_generation &&
	       h->dst_generation && h->src_epoch && h->dst_epoch;
}
static inline int sn_header_valid(const struct sn_header *h)
{
	if (h->total > SN_MESSAGE || h->length > SN_PAYLOAD ||
	    h->offset > h->total || h->length > h->total - h->offset)
		return 0;
	switch (h->opcode) {
	case SN_SEND:
	case SN_READ_REPLY:
	case SN_WRITE:
		if (!sn_qp_fields_valid(h)) return 0;
		if (h->opcode != SN_WRITE && (h->address || h->rkey))
			return 0;
		if (h->status || h->ack_sequence ||
		    (h->total && !h->length) ||
		    (!h->total && (h->length || h->offset)))
			return 0;
		if (h->opcode == SN_WRITE && (!h->rkey ||
		    h->address > ~(sn_u64)0 - h->total))
			return 0;
		return 1;
	case SN_READ:
		return sn_qp_fields_valid(h) && !h->length && !h->offset && !h->status &&
		       !h->ack_sequence && h->rkey &&
		       h->address <= ~(sn_u64)0 - h->total;
	case SN_ZC_SEND:
		/* Zero-copy burst descriptor: total carries the whole message; payload
		 * follows as header-less frames on the data ring. */
		return sn_qp_fields_valid(h) && h->total && h->sequence && !h->length &&
		       !h->offset && !h->address && !h->rkey && !h->status && !h->ack_sequence;
	case SN_ZC_READY:
		/* ack_sequence carries the burst operation serial. */
		return sn_qp_fields_valid(h) && !h->total && !h->length && !h->offset &&
		       !h->address && !h->rkey && !h->sequence && !h->status;
	case SN_ACK:
		return sn_qp_fields_valid(h) && !h->total && !h->length && !h->offset &&
		       !h->address && !h->rkey && !h->sequence && h->status <= SN_PROTOCOL;
	case SN_READ_ACK:
		return sn_qp_fields_valid(h) && !h->total && !h->length && !h->offset &&
		       !h->address && !h->rkey && !h->sequence && !h->status;
	case SN_CREDIT:
		/* Sequence is a monotonic credit revision, total is available RQ. */
		return sn_qp_fields_valid(h) && h->total <= SN_QUEUE_DEPTH && !h->length &&
		       !h->offset && !h->address && !h->rkey && !h->status && !h->ack_sequence;
	case SN_HELLO:
		/* dst_epoch is 0 until the peer is known; total carries capabilities. */
		return h->src_epoch && h->total && !(h->total & ~SN_CAPS_KNOWN) &&
		       !h->src_qpn && !h->dst_qpn && !h->src_generation &&
		       !h->dst_generation && !h->sequence && !h->ack_sequence &&
		       !h->offset && !h->length && !h->address && !h->rkey && !h->status;
	case SN_HELLO_ACK:
		return h->src_epoch && h->dst_epoch && h->total &&
		       !(h->total & ~SN_CAPS_KNOWN) &&
		       !h->src_qpn && !h->dst_qpn && !h->src_generation &&
		       !h->dst_generation && !h->sequence && !h->ack_sequence &&
		       !h->offset && !h->length && !h->address && !h->rkey && !h->status;
	case SN_BIND:
		/* dst_generation is 0; the ACK echoes it. sequence/ack are our PSNs. */
		return h->src_qpn && h->src_qpn <= SN_PSN_MAX && h->dst_qpn &&
		       h->dst_qpn <= SN_PSN_MAX && h->src_generation &&
		       !h->dst_generation && h->src_epoch && h->dst_epoch &&
		       h->sequence <= SN_PSN_MAX && h->ack_sequence <= SN_PSN_MAX &&
		       !h->total && !h->offset && !h->length && !h->address &&
		       !h->rkey && !h->status;
	case SN_BIND_ACK:
		return h->src_qpn && h->src_qpn <= SN_PSN_MAX && h->dst_qpn &&
		       h->dst_qpn <= SN_PSN_MAX && h->src_generation &&
		       h->dst_generation && h->src_epoch && h->dst_epoch &&
		       h->sequence <= SN_PSN_MAX && h->ack_sequence <= SN_PSN_MAX &&
		       !h->total && !h->offset && !h->length && !h->address &&
		       !h->rkey && !h->status;
	default:
		return 0;
	}
}
/* Caller supplies payload at frame + SN_HEADER before encoding. */
static inline int sn_encode(sn_u8 *frame, size_t capacity,
			    const struct sn_header *h)
{
	if (!sn_header_valid(h) || capacity < SN_HEADER + h->length)
		return -1;
	memset(frame, 0, SN_HEADER);
	sn_put32(frame, SN_MAGIC);
	frame[4] = SN_VERSION;
	frame[7] = SN_HEADER;
	frame[8] = h->opcode;
	sn_put32(frame + 12, h->total);
	sn_put32(frame + 16, h->src_qpn);
	sn_put32(frame + 20, h->dst_qpn);
	sn_put64(frame + 24, h->src_generation);
	sn_put64(frame + 32, h->dst_generation);
	sn_put64(frame + 40, h->src_epoch);
	sn_put64(frame + 48, h->dst_epoch);
	sn_put64(frame + 56, h->sequence);
	sn_put64(frame + 64, h->ack_sequence);
	sn_put32(frame + 72, h->offset);
	sn_put32(frame + 76, h->length);
	sn_put64(frame + 80, h->address);
	sn_put32(frame + 88, h->rkey);
	sn_put32(frame + 92, h->status);
	sn_put32(frame + 96, sn_crc(frame, SN_HEADER + h->length));
	return 0;
}
static inline int sn_decode(struct sn_header *h, const sn_u8 *p, size_t n)
{
	struct sn_header v;
	size_t i;

	if (n < SN_HEADER || n > SN_FRAME || sn_get32(p) != SN_MAGIC ||
	    p[4] != SN_VERSION || p[5] || p[6] || p[7] != SN_HEADER ||
	    p[9] || p[10] || p[11])
		return -1;
	for (i = 100; i < SN_HEADER; i++)
		if (p[i])
			return -1;
	if (sn_get32(p + 76) != n - SN_HEADER || sn_get32(p + 96) != sn_crc(p, n))
		return -1;
	memset(&v, 0, sizeof(v));
	v.opcode = p[8]; v.total = sn_get32(p + 12);
	v.src_qpn = sn_get32(p + 16); v.dst_qpn = sn_get32(p + 20);
	v.src_generation = sn_get64(p + 24); v.dst_generation = sn_get64(p + 32);
	v.src_epoch = sn_get64(p + 40); v.dst_epoch = sn_get64(p + 48);
	v.sequence = sn_get64(p + 56); v.ack_sequence = sn_get64(p + 64);
	v.offset = sn_get32(p + 72); v.length = sn_get32(p + 76);
	v.address = sn_get64(p + 80); v.rkey = sn_get32(p + 88);
	v.status = sn_get32(p + 92);
	if (!sn_header_valid(&v))
		return -1;
	*h = v;
	return 0;
}
static inline int sn_ready(const struct sn_readiness *r)
{
	return r->local_epoch && r->peer_epoch &&
	       r->local_echo == r->peer_epoch && r->peer_echo == r->local_epoch &&
	       r->rx_primed && r->rings_started && r->paths_active &&
	       r->peer_ready && r->carrier;
}
/* Local state sufficient to carry handshake frames; data still needs sn_ready. */
static inline int sn_local_ready(const struct sn_readiness *r)
{
	return r->local_epoch && r->rings_started && r->paths_active &&
	       r->rx_primed && r->carrier;
}
static inline int sn_admit(const struct sn_binding *b, const struct sn_header *h)
{
	return b->local_epoch && b->peer_epoch && b->local_generation &&
	       b->peer_generation && b->local_qpn && b->peer_qpn &&
	       h->src_epoch == b->peer_epoch && h->dst_epoch == b->local_epoch &&
	       h->src_generation == b->peer_generation &&
	       h->dst_generation == b->local_generation &&
	       h->src_qpn == b->peer_qpn && h->dst_qpn == b->local_qpn;
}
static inline int sn_range(sn_u64 base, sn_u64 size, sn_u64 address, sn_u64 length)
{
	return size <= ~(sn_u64)0 - base && address >= base &&
	       address - base <= size && length <= size - (address - base);
}
#endif
