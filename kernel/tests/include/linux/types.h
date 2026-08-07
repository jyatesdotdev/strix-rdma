#ifndef TBSTREAM_TEST_LINUX_TYPES_H
#define TBSTREAM_TEST_LINUX_TYPES_H

/*
 * Minimal UAPI-only type model. A 32-bit ABI commonly aligns plain 64-bit
 * integers to four bytes, while __aligned_u64 must remain eight-byte aligned.
 * The layout test builds once with each plain-u64 alignment.
 */
#ifndef TBSTREAM_TEST_U64_ALIGNMENT
#error "TBSTREAM_TEST_U64_ALIGNMENT must be 4 or 8"
#endif

typedef unsigned int __u32;
typedef signed int __s32;

#if TBSTREAM_TEST_U64_ALIGNMENT == 4
typedef struct {
	__u32 low;
	__u32 high;
} __u64;
#elif TBSTREAM_TEST_U64_ALIGNMENT == 8
typedef struct {
	__u32 low;
	__u32 high;
} __attribute__((aligned(8))) __u64;
#else
#error "unsupported TBSTREAM_TEST_U64_ALIGNMENT"
#endif

typedef struct {
	__u32 low;
	__u32 high;
} __attribute__((aligned(8))) __aligned_u64;

#endif /* TBSTREAM_TEST_LINUX_TYPES_H */
