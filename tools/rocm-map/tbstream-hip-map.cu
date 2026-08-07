// SPDX-License-Identifier: MIT
/* Prove GPU access to a thunderbolt-stream zero-copy TX pool. */

#include <hip/hip_runtime.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../pingpong/thunderbolt-stream.h"

static __global__ void fill_bytes(unsigned char *buf, size_t len,
                                  unsigned char seed)
{
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x;
    size_t stride = (size_t)blockDim.x * gridDim.x;

    for (; i < len; i += stride)
        buf[i] = (unsigned char)(seed + i);
}

static __global__ void check_bytes(const unsigned char *buf, size_t len,
                                   unsigned char seed,
                                   unsigned long long *bad)
{
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x;
    size_t stride = (size_t)blockDim.x * gridDim.x;
    unsigned long long local_bad = 0;

    for (; i < len; i += stride) {
        if (buf[i] != (unsigned char)(seed + i))
            local_bad++;
    }
    if (local_bad)
        atomicAdd(bad, local_bad);
}

static void usage(const char *prog)
{
    fprintf(stderr, "usage: %s [-d DEVICE] [-g GPU] [-s SIZE]\n", prog);
}

static size_t parse_size(const char *text)
{
    char *end = NULL;
    unsigned long long value;
    unsigned long long multiplier = 1;

    errno = 0;
    value = strtoull(text, &end, 10);
    if (end == text || errno == ERANGE)
        return 0;
    if ((*end == 'k' || *end == 'K') && end[1] == '\0')
        multiplier = 1024ull;
    else if ((*end == 'm' || *end == 'M') && end[1] == '\0')
        multiplier = 1024ull * 1024ull;
    else if (*end != '\0')
        return 0;
    if (value > ULLONG_MAX / multiplier)
        return 0;
    value *= multiplier;
    if (!value || value > SIZE_MAX)
        return 0;
    return (size_t)value;
}

static int hip_ok(hipError_t result, const char *operation)
{
    if (result == hipSuccess)
        return 1;
    fprintf(stderr, "%s: %s\n", operation, hipGetErrorString(result));
    return 0;
}

int main(int argc, char **argv)
{
    const char *device = "/dev/tbstream0";
    size_t requested = 0;
    int gpu = 0;
    int fd = -1;
    int opt;
    int rc = 1;
    bool registered = false;
    void *mapping = MAP_FAILED;
    unsigned char *tx = NULL;
    unsigned char *gpu_tx = NULL;
    unsigned long long *gpu_bad = NULL;
    unsigned long long bad = 0;
    struct tbstream_zc_info info = {};
    size_t map_len = 0;
    size_t tx_len = 0;
    size_t test_len = 0;
    hipDeviceProp_t props = {};
    const unsigned threads = 256;
    unsigned blocks = 0;

    while ((opt = getopt(argc, argv, "d:g:s:h")) != -1) {
        switch (opt) {
        case 'd':
            device = optarg;
            break;
        case 'g': {
            char *end = NULL;
            long parsed = strtol(optarg, &end, 10);
            if (end == optarg || *end || parsed < 0 || parsed > INT32_MAX) {
                usage(argv[0]);
                return 2;
            }
            gpu = (int)parsed;
            break;
        }
        case 's':
            requested = parse_size(optarg);
            if (!requested) {
                usage(argv[0]);
                return 2;
            }
            break;
        default:
            usage(argv[0]);
            return opt == 'h' ? 0 : 2;
        }
    }

    fd = open(device, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "open %s: %s\n", device, strerror(errno));
        goto out;
    }
    if (ioctl(fd, TBSTREAM_ZC_ENABLE)) {
        perror("TBSTREAM_ZC_ENABLE");
        goto out;
    }
    if (ioctl(fd, TBSTREAM_ZC_GET_INFO, &info)) {
        perror("TBSTREAM_ZC_GET_INFO");
        goto out;
    }
    if (!info.ring_size || !info.frame_size ||
        info.ring_size > SIZE_MAX / info.frame_size) {
        fprintf(stderr, "invalid zero-copy pool geometry\n");
        goto out;
    }
    tx_len = (size_t)info.ring_size * info.frame_size;
    if (tx_len > SIZE_MAX / 2) {
        fprintf(stderr, "invalid zero-copy pool offsets\n");
        goto out;
    }
    map_len = 2 * tx_len;
    if (info.tx_pool_offset > map_len - tx_len ||
        info.rx_pool_offset > map_len - tx_len) {
        fprintf(stderr, "invalid zero-copy pool offsets\n");
        goto out;
    }
    mapping = mmap(NULL, map_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED) {
        perror("mmap");
        goto out;
    }
    tx = (unsigned char *)mapping + info.tx_pool_offset;
    test_len = requested ? requested : tx_len;
    if (test_len > tx_len) {
        fprintf(stderr, "requested size %zu exceeds TX pool size %zu\n",
                test_len, tx_len);
        goto out;
    }

    if (!hip_ok(hipSetDeviceFlags(hipDeviceMapHost), "hipSetDeviceFlags") ||
        !hip_ok(hipSetDevice(gpu), "hipSetDevice") ||
        !hip_ok(hipGetDeviceProperties(&props, gpu), "hipGetDeviceProperties") ||
        !hip_ok(hipHostRegister(tx, tx_len, hipHostRegisterMapped),
                "hipHostRegister(TX pool)"))
        goto out;
    registered = true;
    if (!hip_ok(hipHostGetDevicePointer((void **)&gpu_tx, tx, 0),
                "hipHostGetDevicePointer") ||
        !hip_ok(hipMalloc((void **)&gpu_bad, sizeof(*gpu_bad)), "hipMalloc"))
        goto out;

    blocks = (unsigned)((test_len + threads - 1) / threads);
    if (blocks > 4096)
        blocks = 4096;

    fill_bytes<<<blocks, threads>>>(gpu_tx, test_len, 0x31);
    if (!hip_ok(hipGetLastError(), "GPU write kernel launch") ||
        !hip_ok(hipDeviceSynchronize(), "GPU write synchronization"))
        goto out;
    for (size_t i = 0; i < test_len; i++) {
        if (tx[i] != (unsigned char)(0x31 + i)) {
            fprintf(stderr, "GPU-to-pool mismatch at byte %zu\n", i);
            goto out;
        }
    }

    for (size_t i = 0; i < test_len; i++)
        tx[i] = (unsigned char)(0xa7 + i);
    if (!hip_ok(hipMemset(gpu_bad, 0, sizeof(*gpu_bad)), "hipMemset"))
        goto out;
    check_bytes<<<blocks, threads>>>(gpu_tx, test_len, 0xa7, gpu_bad);
    if (!hip_ok(hipGetLastError(), "pool read kernel launch") ||
        !hip_ok(hipMemcpy(&bad, gpu_bad, sizeof(bad), hipMemcpyDeviceToHost),
                "hipMemcpy mismatch count"))
        goto out;
    if (bad) {
        fprintf(stderr, "pool-to-GPU verification found %" PRIu64
                        " mismatched bytes\n", (uint64_t)bad);
        goto out;
    }

    printf("PASS: GPU %d (%s) mapped and verified %zu/%zu TX-pool bytes\n",
           gpu, props.name, test_len, tx_len);
    rc = 0;

out:
    if (gpu_bad && !hip_ok(hipFree(gpu_bad), "hipFree cleanup"))
        rc = 1;
    if (registered &&
        !hip_ok(hipHostUnregister(tx), "hipHostUnregister cleanup"))
        rc = 1;
    if (mapping != MAP_FAILED)
        munmap(mapping, map_len);
    if (fd >= 0)
        close(fd);
    return rc;
}
