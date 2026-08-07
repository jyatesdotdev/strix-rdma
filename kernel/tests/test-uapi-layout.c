#include <linux/thunderbolt-stream.h>

#define OFFSET_OF(type, member) __builtin_offsetof(type, member)

_Static_assert(_Alignof(__u64) == TBSTREAM_TEST_U64_ALIGNMENT,
	       "plain __u64 alignment model is wrong");
_Static_assert(_Alignof(__aligned_u64) == 8,
	       "__aligned_u64 must be eight-byte aligned");

_Static_assert(sizeof(struct tbstream_zc_info) == 24,
	       "tbstream_zc_info ABI changed");
_Static_assert(sizeof(struct tbstream_zc_event) == 16,
	       "tbstream_zc_event ABI changed");
_Static_assert(sizeof(struct tbstream_zc_tx) == 16,
	       "tbstream_zc_tx ABI changed");
_Static_assert(sizeof(struct tbstream_zc_reap) == 16,
	       "tbstream_zc_reap ABI changed");
_Static_assert(sizeof(struct tbstream_zc_rx) == 8,
	       "tbstream_zc_rx ABI changed");

_Static_assert(_Alignof(struct tbstream_zc_ring_stats) == 8,
	       "ring stats must have one layout on 32-bit and 64-bit ABIs");
_Static_assert(sizeof(struct tbstream_zc_ring_stats) == 128,
	       "tbstream_zc_ring_stats ABI changed");
_Static_assert(OFFSET_OF(struct tbstream_zc_ring_stats, flags) == 48,
	       "ring flags offset changed");
_Static_assert(OFFSET_OF(struct tbstream_zc_ring_stats, interval_nsec) == 112,
	       "ring interval offset changed");
_Static_assert(OFFSET_OF(struct tbstream_zc_ring_stats, reserved) == 116,
	       "ring reserved offset changed");

_Static_assert(_Alignof(struct tbstream_zc_stats) == 8,
	       "stats must have one layout on 32-bit and 64-bit ABIs");
_Static_assert(sizeof(struct tbstream_zc_stats) == 528,
	       "tbstream_zc_stats ABI changed");
_Static_assert(OFFSET_OF(struct tbstream_zc_stats, tx_submit_calls) == 8,
	       "stats counters offset changed");
_Static_assert(OFFSET_OF(struct tbstream_zc_stats, tx_prod) == 176,
	       "stats cursor offset changed");
_Static_assert(OFFSET_OF(struct tbstream_zc_stats, tx_pending) == 208,
	       "stats scalar offset changed");
_Static_assert(OFFSET_OF(struct tbstream_zc_stats, last_error) == 248,
	       "stats terminal-error offset changed");
_Static_assert(OFFSET_OF(struct tbstream_zc_stats, reserved) == 252,
	       "stats reserved offset changed");
_Static_assert(OFFSET_OF(struct tbstream_zc_stats, tx) == 272,
	       "TX ring snapshot offset changed");
_Static_assert(OFFSET_OF(struct tbstream_zc_stats, rx) == 400,
	       "RX ring snapshot offset changed");

_Static_assert(_IOC_TYPE(TBSTREAM_ZC_GET_STATS) == TBSTREAM_ZC_MAGIC,
	       "GET_STATS ioctl type changed");
_Static_assert(_IOC_NR(TBSTREAM_ZC_GET_STATS) == 0x06,
	       "GET_STATS ioctl number changed");
_Static_assert(_IOC_DIR(TBSTREAM_ZC_GET_STATS) == _IOC_READ,
	       "GET_STATS ioctl direction changed");
_Static_assert(_IOC_SIZE(TBSTREAM_ZC_GET_STATS) == 528,
	       "GET_STATS ioctl size changed");
_Static_assert(_IOC_TYPE(TBSTREAM_ZC_KICK) == TBSTREAM_ZC_MAGIC,
	       "KICK ioctl type changed");
_Static_assert(_IOC_NR(TBSTREAM_ZC_KICK) == 0x07,
	       "KICK ioctl number changed");
_Static_assert(_IOC_DIR(TBSTREAM_ZC_KICK) == _IOC_WRITE,
	       "KICK ioctl direction changed");
_Static_assert(_IOC_SIZE(TBSTREAM_ZC_KICK) == 8,
	       "KICK ioctl size changed");

int main(void)
{
	return 0;
}
