// SPDX-License-Identifier: MIT
/*
 * Exercise the DS4 GPU-to-NHI handoff without loading a model.
 *
 * Start "rx" on the peer first.  The "tx" side writes a 64-byte envelope
 * with the CPU, copies the remaining payload from VRAM into the registered
 * TX pool with hipMemcpy(DeviceToHost), synchronizes the GPU, and submits the
 * exact 17, 17, 33, 65 frame sequence seen in distributed inference.
 */

#include <hip/hip_runtime.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../pingpong/thunderbolt-stream.h"

#define TEST_MAGIC 0x44344754u /* D4GT */
#define TEST_VERSION 1u
#define TEST_ENVELOPE_BYTES 64u
#define TEST_TIMEOUT_MS 30000

static const uint32_t test_frames[] = {17u, 17u, 33u, 65u};

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
};

static __global__ void fill_payload(unsigned char *buf, size_t len,
                                    unsigned char seed)
{
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x;
    size_t stride = (size_t)blockDim.x * gridDim.x;

    for (; i < len; i += stride)
        buf[i] = (unsigned char)(seed + 131u * i + (i >> 8));
}

static unsigned char expected_byte(size_t offset, unsigned char seed)
{
    return (unsigned char)(seed + 131u * offset + (offset >> 8));
}

static int hip_ok(hipError_t result, const char *operation)
{
    if (result == hipSuccess)
        return 1;
    fprintf(stderr, "%s: %s\n", operation, hipGetErrorString(result));
    return 0;
}

static void put_u32(unsigned char *p, uint32_t value)
{
    value = htonl(value);
    memcpy(p, &value, sizeof(value));
}

static uint32_t get_u32(const unsigned char *p)
{
    uint32_t value;
    memcpy(&value, p, sizeof(value));
    return ntohl(value);
}

static unsigned char message_seed(uint32_t sequence)
{
    return (unsigned char)(0x31u + 29u * sequence);
}

static void encode_envelope(unsigned char *p, uint32_t sequence,
                            uint32_t nframes, uint32_t payload_bytes)
{
    memset(p, 0, TEST_ENVELOPE_BYTES);
    put_u32(p + 0, TEST_MAGIC);
    put_u32(p + 4, TEST_VERSION);
    put_u32(p + 8, sequence);
    put_u32(p + 12, nframes);
    put_u32(p + 16, payload_bytes);
    put_u32(p + 20, message_seed(sequence));
    put_u32(p + 24, TEST_ENVELOPE_BYTES);
}

static int check_envelope(const unsigned char *p, uint32_t sequence,
                          uint32_t nframes, uint32_t payload_bytes)
{
    if (get_u32(p + 0) != TEST_MAGIC ||
        get_u32(p + 4) != TEST_VERSION ||
        get_u32(p + 8) != sequence ||
        get_u32(p + 12) != nframes ||
        get_u32(p + 16) != payload_bytes ||
        get_u32(p + 20) != message_seed(sequence) ||
        get_u32(p + 24) != TEST_ENVELOPE_BYTES) {
        fprintf(stderr,
                "RX envelope mismatch: seq=%u magic=%08x version=%u "
                "wire_seq=%u nframes=%u payload=%u seed=%u header=%u\n",
                sequence, get_u32(p + 0), get_u32(p + 4),
                get_u32(p + 8), get_u32(p + 12), get_u32(p + 16),
                get_u32(p + 20), get_u32(p + 24));
        return -1;
    }
    for (size_t i = 28; i < TEST_ENVELOPE_BYTES; i++) {
        if (p[i] != 0) {
            fprintf(stderr,
                    "RX envelope reserved byte %zu is nonzero (0x%02x)\n",
                    i, p[i]);
            return -1;
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

static int zc_open(struct zc_state *z, const char *device)
{
    memset(z, 0, sizeof(*z));
    z->fd = -1;
    z->mapping = MAP_FAILED;

    z->fd = open(device, O_RDWR);
    if (z->fd < 0) {
        fprintf(stderr, "open %s: %s\n", device, strerror(errno));
        return -1;
    }
    if (ioctl(z->fd, TBSTREAM_ZC_ENABLE)) {
        perror("TBSTREAM_ZC_ENABLE");
        return -1;
    }

    struct tbstream_zc_info info;
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
    if (z->pool_bytes > SIZE_MAX / 2) {
        fprintf(stderr, "zero-copy mapping is too large\n");
        return -1;
    }
    z->mapping_bytes = 2 * z->pool_bytes;
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
    z->tx = (unsigned char *)z->mapping + info.tx_pool_offset;
    z->rx = (unsigned char *)z->mapping + info.rx_pool_offset;

    fprintf(stderr, "zc: ring_size=%u frame_size=%u\n",
            z->ring_size, z->frame_size);
    return 0;
}

static int wait_readable(int fd)
{
    struct pollfd pfd;
    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = fd;
    pfd.events = POLLIN;

    for (;;) {
        int rc = poll(&pfd, 1, TEST_TIMEOUT_MS);
        if (rc < 0 && errno == EINTR)
            continue;
        if (rc < 0) {
            perror("poll");
            return -1;
        }
        if (rc == 0) {
            fprintf(stderr, "timed out after %d ms waiting for NHI event\n",
                    TEST_TIMEOUT_MS);
            errno = ETIMEDOUT;
            return -1;
        }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            fprintf(stderr, "NHI poll failed (revents=0x%x)\n",
                    (unsigned)pfd.revents);
            errno = EIO;
            return -1;
        }
        if (pfd.revents & POLLIN)
            return 0;
    }
}

static int reap_one(struct zc_state *z, struct tbstream_zc_event *event)
{
    struct tbstream_zc_reap reap;
    memset(&reap, 0, sizeof(reap));
    reap.max = 1;
    reap.events = (uint64_t)(uintptr_t)event;

    if (wait_readable(z->fd) != 0)
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
        return 0;
    }
}

static int submit_tx(struct zc_state *z, uint32_t nframes,
                     uint32_t last_len, uint32_t *first)
{
    struct tbstream_zc_tx tx;
    memset(&tx, 0, sizeof(tx));
    tx.nframes = nframes;
    tx.last_len = last_len;

    if (ioctl(z->fd, TBSTREAM_ZC_SUBMIT_TX, &tx)) {
        perror("TBSTREAM_ZC_SUBMIT_TX");
        return -1;
    }
    if (tx.first != z->tx_next) {
        fprintf(stderr, "TX cursor mismatch: kernel=%u expected=%u\n",
                tx.first, z->tx_next);
        errno = EPROTO;
        return -1;
    }
    *first = tx.first;
    z->tx_next = (z->tx_next + nframes) % z->ring_size;
    return 0;
}

static int wait_tx_done(struct zc_state *z, uint32_t first,
                        uint32_t nframes)
{
    for (;;) {
        struct tbstream_zc_event event;
        memset(&event, 0, sizeof(event));
        if (reap_one(z, &event) != 0)
            return -1;
        if (event.type == TBSTREAM_ZC_EV_CLOSE) {
            fprintf(stderr, "peer closed before TX completion\n");
            errno = ECONNRESET;
            return -1;
        }
        if (event.type != TBSTREAM_ZC_EV_TX_DONE) {
            fprintf(stderr, "unexpected NHI event type %u while sending\n",
                    event.type);
            errno = EPROTO;
            return -1;
        }
        if (event.first != first || event.nframes != nframes ||
            event.bytes != 0) {
            fprintf(stderr,
                    "TX completion mismatch: first=%u/%u nframes=%u/%u "
                    "bytes=%u\n",
                    event.first, first, event.nframes, nframes, event.bytes);
            errno = EPROTO;
            return -1;
        }
        return 0;
    }
}

static int post_rx(struct zc_state *z, uint32_t nframes)
{
    struct tbstream_zc_rx rx;
    memset(&rx, 0, sizeof(rx));
    rx.nframes = nframes;
    rx.flags = TBSTREAM_ZC_RX_F_INTERRUPT_BOUNDARIES;
    if (!ioctl(z->fd, TBSTREAM_ZC_POST_RX_FLAGS, &rx))
        return 0;
    if (errno != ENOTTY) {
        perror("TBSTREAM_ZC_POST_RX_FLAGS");
        return -1;
    }
    if (ioctl(z->fd, TBSTREAM_ZC_POST_RX, &nframes)) {
        perror("TBSTREAM_ZC_POST_RX");
        return -1;
    }
    return 0;
}

static uint64_t frames_per_repeat(void)
{
    uint64_t total = 0;
    for (size_t i = 0; i < sizeof(test_frames) / sizeof(test_frames[0]); i++)
        total += test_frames[i];
    return total;
}

static int validate_run_geometry(const struct zc_state *z, uint32_t repeats)
{
    if (z->frame_size != TBSTREAM_ZC_FRAME_SIZE) {
        fprintf(stderr, "expected %u-byte NHI frames, got %u\n",
                (unsigned)TBSTREAM_ZC_FRAME_SIZE, z->frame_size);
        return -1;
    }
    uint64_t frames = frames_per_repeat() * repeats;
    if (frames >= z->ring_size) {
        fprintf(stderr,
                "%u repeats need %" PRIu64 " frames; ring %u permits at "
                "most %u without wrapping\n",
                repeats, frames, z->ring_size,
                (unsigned)((z->ring_size - 1u) / frames_per_repeat()));
        return -1;
    }
    return 0;
}

static int run_tx(const char *device, int gpu, uint32_t repeats)
{
    struct zc_state z;
    unsigned char *gpu_payload = NULL;
    size_t max_payload = 0;
    uint32_t sequence = 0;
    bool registered = false;
    int rc = 1;

    if (zc_open(&z, device) != 0)
        goto out;
    if (validate_run_geometry(&z, repeats) != 0)
        goto out;
    if (!hip_ok(hipSetDeviceFlags(hipDeviceMapHost), "hipSetDeviceFlags") ||
        !hip_ok(hipSetDevice(gpu), "hipSetDevice") ||
        !hip_ok(hipHostRegister(z.tx, z.pool_bytes, hipHostRegisterMapped),
                "hipHostRegister(TX pool)"))
        goto out;
    registered = true;

    max_payload = (size_t)(65u - 1u) * z.frame_size;
    if (!hip_ok(hipMalloc((void **)&gpu_payload, max_payload),
                "hipMalloc(payload)"))
        goto out;

    for (uint32_t repeat = 0; repeat < repeats; repeat++) {
        for (size_t geometry = 0;
             geometry < sizeof(test_frames) / sizeof(test_frames[0]);
             geometry++, sequence++) {
            const uint32_t nframes = test_frames[geometry];
            const uint32_t payload_bytes = (nframes - 1u) * z.frame_size;
            const uint32_t first = z.tx_next;
            unsigned char *message = z.tx + (size_t)first * z.frame_size;
            unsigned char *payload = message + TEST_ENVELOPE_BYTES;
            const unsigned threads = 256;
            unsigned blocks = (payload_bytes + threads - 1u) / threads;
            if (blocks > 4096u)
                blocks = 4096u;

            encode_envelope(message, sequence, nframes, payload_bytes);
            fill_payload<<<blocks, threads>>>(gpu_payload, payload_bytes,
                                              message_seed(sequence));
            if (!hip_ok(hipGetLastError(), "GPU payload fill launch") ||
                !hip_ok(hipMemcpy(payload, gpu_payload, payload_bytes,
                                 hipMemcpyDeviceToHost),
                        "hipMemcpy(DeviceToHost mapped TX pool)") ||
                !hip_ok(hipDeviceSynchronize(),
                        "hipDeviceSynchronize before NHI submit"))
                goto out;

            uint32_t submitted_first = 0;
            if (submit_tx(&z, nframes, TEST_ENVELOPE_BYTES,
                          &submitted_first) != 0 ||
                wait_tx_done(&z, submitted_first, nframes) != 0)
                goto out;
            fprintf(stderr,
                    "TX seq=%u first=%u nframes=%u payload=%u complete\n",
                    sequence, submitted_first, nframes, payload_bytes);
        }
    }

    printf("PASS: GPU %d sent and completed %u verified-pattern messages "
           "(%u repeats of 17,17,33,65 frames)\n",
           gpu, sequence, repeats);
    rc = 0;

out:
    if (gpu_payload && !hip_ok(hipFree(gpu_payload), "hipFree cleanup"))
        rc = 1;
    if (registered &&
        !hip_ok(hipHostUnregister(z.tx), "hipHostUnregister cleanup"))
        rc = 1;
    zc_close(&z);
    return rc;
}

static int verify_payload(const unsigned char *payload, size_t bytes,
                          uint32_t sequence)
{
    const unsigned char seed = message_seed(sequence);
    for (size_t i = 0; i < bytes; i++) {
        const unsigned char expected = expected_byte(i, seed);
        if (payload[i] != expected) {
            fprintf(stderr,
                    "RX payload mismatch: seq=%u offset=%zu got=0x%02x "
                    "expected=0x%02x\n",
                    sequence, i, payload[i], expected);
            return -1;
        }
    }
    return 0;
}

static int run_rx(const char *device, uint32_t repeats)
{
    struct zc_state z;
    uint32_t sequence = 0;
    uint32_t message_count = 0;
    int rc = 1;

    if (zc_open(&z, device) != 0)
        goto out;
    if (validate_run_geometry(&z, repeats) != 0)
        goto out;

    printf("READY: zero-copy RX armed; start the TX peer now\n");
    fflush(stdout);

    message_count =
        repeats * (uint32_t)(sizeof(test_frames) / sizeof(test_frames[0]));
    while (sequence < message_count) {
        struct tbstream_zc_event event;
        memset(&event, 0, sizeof(event));
        if (reap_one(&z, &event) != 0)
            goto out;
        if (event.type == TBSTREAM_ZC_EV_CLOSE) {
            fprintf(stderr, "peer closed after %u/%u messages\n",
                    sequence, message_count);
            errno = ECONNRESET;
            goto out;
        }
        if (event.type != TBSTREAM_ZC_EV_RX) {
            fprintf(stderr, "unexpected NHI event type %u while receiving\n",
                    event.type);
            errno = EPROTO;
            goto out;
        }

        const uint32_t expected_frames =
            test_frames[sequence %
                        (sizeof(test_frames) / sizeof(test_frames[0]))];
        const uint32_t payload_bytes =
            (expected_frames - 1u) * z.frame_size;
        const uint32_t expected_bytes = payload_bytes + TEST_ENVELOPE_BYTES;
        if (event.first != z.rx_next || event.nframes != expected_frames ||
            event.bytes != expected_bytes) {
            fprintf(stderr,
                    "RX geometry mismatch: seq=%u first=%u/%u "
                    "nframes=%u/%u bytes=%u/%u\n",
                    sequence, event.first, z.rx_next,
                    event.nframes, expected_frames,
                    event.bytes, expected_bytes);
            errno = EPROTO;
            goto out;
        }
        const size_t offset = (size_t)event.first * z.frame_size;
        if (offset > z.pool_bytes ||
            event.bytes > z.pool_bytes - offset) {
            fprintf(stderr, "RX message unexpectedly wraps the mapped pool\n");
            errno = EPROTO;
            goto out;
        }
        const unsigned char *message = z.rx + offset;
        if (check_envelope(message, sequence, expected_frames,
                           payload_bytes) != 0 ||
            verify_payload(message + TEST_ENVELOPE_BYTES, payload_bytes,
                           sequence) != 0)
            goto out;

        fprintf(stderr,
                "RX seq=%u first=%u nframes=%u payload=%u verified\n",
                sequence, event.first, event.nframes, payload_bytes);
        if (post_rx(&z, event.nframes) != 0)
            goto out;
        z.rx_next = (z.rx_next + event.nframes) % z.ring_size;
        sequence++;
    }

    printf("PASS: peer payload verified for %u messages "
           "(%u repeats of 17,17,33,65 frames)\n",
           message_count, repeats);
    rc = 0;

out:
    zc_close(&z);
    return rc;
}

static void usage(const char *program)
{
    fprintf(stderr,
            "usage: %s tx|rx [-d DEVICE] [-g GPU] [-n REPEATS]\n"
            "  Start rx first and wait for READY, then start tx.\n"
            "  -d DEVICE   stream device (default /dev/tbstream0)\n"
            "  -g GPU      GPU index for tx (default 0)\n"
            "  -n REPEATS  repeat 17,17,33,65 sequence without wrap "
            "(default 1)\n",
            program);
}

int main(int argc, char **argv)
{
    const char *device = "/dev/tbstream0";
    uint32_t repeats = 1;
    int gpu = 0;

    if (argc < 2 || (strcmp(argv[1], "tx") != 0 &&
                     strcmp(argv[1], "rx") != 0)) {
        usage(argv[0]);
        return 2;
    }
    const bool transmit = strcmp(argv[1], "tx") == 0;
    optind = 2;
    int option;
    while ((option = getopt(argc, argv, "d:g:n:h")) != -1) {
        char *end = NULL;
        unsigned long value;
        switch (option) {
        case 'd':
            device = optarg;
            break;
        case 'g':
            errno = 0;
            value = strtoul(optarg, &end, 10);
            if (errno || end == optarg || *end || value > INT32_MAX) {
                usage(argv[0]);
                return 2;
            }
            gpu = (int)value;
            break;
        case 'n':
            errno = 0;
            value = strtoul(optarg, &end, 10);
            if (errno || end == optarg || *end || value == 0 ||
                value > UINT32_MAX) {
                usage(argv[0]);
                return 2;
            }
            repeats = (uint32_t)value;
            break;
        default:
            usage(argv[0]);
            return option == 'h' ? 0 : 2;
        }
    }
    if (optind != argc) {
        usage(argv[0]);
        return 2;
    }

    return transmit ? run_tx(device, gpu, repeats)
                    : run_rx(device, repeats);
}
