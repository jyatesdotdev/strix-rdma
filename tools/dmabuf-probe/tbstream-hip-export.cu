// SPDX-License-Identifier: MIT
/*
 * tbstream-hip-export - export a native HIP allocation as a DMA-BUF and
 * hand the inherited fd to tbstream-dmabuf-probe.
 *
 * Gate-4 companion for the no-traffic import probe. It:
 *
 *   1. allocates one dedicated HIP allocation of the requested memory
 *      type (coarse hipMalloc, hipDeviceMallocUncached, or
 *      hipDeviceMallocFinegrained with a verified coherency attribute);
 *   2. fills it from a GPU kernel and synchronizes the device so the
 *      GPU is quiesced before export;
 *   3. exports the allocation base and full size with
 *      hipMemGetHandleForAddressRange(hipMemRangeHandleTypeDmaBufFd);
 *   4. exec's tbstream-dmabuf-probe with the fd inherited, keeping the
 *      HIP allocation alive for the probe's whole lifetime; and
 *   5. frees the allocation only after the probe has exited.
 *
 * It never posts traffic and never passes a raw pointer to the kernel;
 * only the DMA-BUF fd crosses the process boundary.
 */

#include <hip/hip_runtime.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define FRAME_SIZE 4096ULL

static __global__ void fill_bytes(unsigned char *buf, size_t len,
                                  unsigned char seed)
{
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x;
    size_t stride = (size_t)blockDim.x * gridDim.x;

    for (; i < len; i += stride)
        buf[i] = (unsigned char)(seed + i);
}

static void usage(FILE *out, const char *prog)
{
    fprintf(out,
            "usage: %s --type coarse|uncached|finegrain --direction tx|rx\n"
            "       [--size BYTES] [--gpu N] [--device PATH] [--probe PATH]\n"
            "\n"
            "Allocate native HIP memory, export it as a DMA-BUF, and run\n"
            "tbstream-dmabuf-probe against the inherited fd. Size accepts\n"
            "K/M/G suffixes and defaults to 16M; it must be a 4 KiB\n"
            "multiple.\n",
            prog);
}

static uint64_t parse_size(const char *arg, const char *what)
{
    uint64_t mult = 1;
    char *end = NULL;
    unsigned long long v;

    errno = 0;
    v = strtoull(arg, &end, 0);
    if (errno || end == arg)
        goto bad;
    if (*end) {
        if (end[1])
            goto bad;
        switch (*end) {
        case 'K': case 'k': mult = 1ULL << 10; break;
        case 'M': case 'm': mult = 1ULL << 20; break;
        case 'G': case 'g': mult = 1ULL << 30; break;
        default:
            goto bad;
        }
    }
    if (v > UINT64_MAX / mult)
        goto bad;
    return v * mult;
bad:
    fprintf(stderr, "invalid %s: %s\n", what, arg);
    exit(2);
}

#define HIP_CHECK(what, call)                                             \
    do {                                                                  \
        hipError_t err_ = (call);                                         \
        if (err_ != hipSuccess) {                                         \
            fprintf(stderr, "%s: %s\n", (what), hipGetErrorString(err_)); \
            exit(1);                                                      \
        }                                                                 \
    } while (0)

int main(int argc, char **argv)
{
    const char *type = NULL, *direction = NULL;
    const char *device = NULL;
    const char *probe = "./tbstream-dmabuf-probe";
    uint64_t size = 16ULL << 20;
    int gpu = 0;

    for (int i = 1; i < argc; i += 2) {
        const char *arg = argv[i];
        const char *val = (i + 1 < argc) ? argv[i + 1] : NULL;

        if (!strcmp(arg, "--help") || !strcmp(arg, "-h")) {
            usage(stdout, argv[0]);
            return 0;
        }
        if (!val) {
            usage(stderr, argv[0]);
            return 2;
        }
        if (!strcmp(arg, "--type"))
            type = val;
        else if (!strcmp(arg, "--direction"))
            direction = val;
        else if (!strcmp(arg, "--size"))
            size = parse_size(val, "size");
        else if (!strcmp(arg, "--gpu"))
            gpu = (int)parse_size(val, "gpu");
        else if (!strcmp(arg, "--device"))
            device = val;
        else if (!strcmp(arg, "--probe"))
            probe = val;
        else {
            usage(stderr, argv[0]);
            return 2;
        }
    }

    if (!type || !direction ||
        (strcmp(direction, "tx") && strcmp(direction, "rx"))) {
        usage(stderr, argv[0]);
        return 2;
    }
    if (!size || (size & (FRAME_SIZE - 1))) {
        fprintf(stderr, "--size must be a nonzero 4 KiB multiple\n");
        return 2;
    }

    HIP_CHECK("hipSetDevice", hipSetDevice(gpu));

    hipDeviceProp_t prop;
    HIP_CHECK("hipGetDeviceProperties", hipGetDeviceProperties(&prop, gpu));
    printf("gpu=%s integrated=%d size=%" PRIu64 " type=%s\n",
           prop.name, prop.integrated, size, type);

    void *ptr = NULL;
    if (!strcmp(type, "coarse")) {
        HIP_CHECK("hipMalloc", hipMalloc(&ptr, size));
    } else if (!strcmp(type, "uncached")) {
        HIP_CHECK("hipExtMallocWithFlags(uncached)",
                  hipExtMallocWithFlags(&ptr, size, hipDeviceMallocUncached));
    } else if (!strcmp(type, "finegrain")) {
        HIP_CHECK("hipExtMallocWithFlags(finegrain)",
                  hipExtMallocWithFlags(&ptr, size,
                                        hipDeviceMallocFinegrained));
    } else {
        fprintf(stderr, "invalid --type: %s\n", type);
        return 2;
    }

    /*
     * The exported fd names the backing BO, not a sliced range, so the
     * probe must see the allocation base and full size. Confirm the
     * runtime reports the same range we allocated.
     */
    {
        hipDeviceptr_t base = 0;
        size_t range = 0;

        HIP_CHECK("hipMemGetAddressRange",
                  hipMemGetAddressRange(&base, &range, (hipDeviceptr_t)ptr));
        if ((void *)base != ptr || range != size) {
            fprintf(stderr,
                    "allocation is not a dedicated base/full-size range "
                    "(base delta %lld, range %zu)\n",
                    (long long)((char *)ptr - (char *)base), range);
            return 1;
        }
    }

    if (!strcmp(type, "finegrain")) {
        /* Require the runtime to actually report fine-grain coherence. */
        uint32_t mode = 0;

        HIP_CHECK("hipMemRangeGetAttribute(coherency)",
                  hipMemRangeGetAttribute(&mode, sizeof(mode),
                                          hipMemRangeAttributeCoherencyMode,
                                          ptr, size));
        printf("coherency-mode=%u (%s)\n", mode,
               mode == hipMemRangeCoherencyModeFineGrain ? "fine-grain" :
               mode == hipMemRangeCoherencyModeCoarseGrain ? "coarse-grain" :
               "other");
        if (mode != hipMemRangeCoherencyModeFineGrain) {
            fprintf(stderr,
                    "FAIL: finegrain allocation does not report fine-grain "
                    "coherency\n");
            return 1;
        }
    }

    /* GPU-fill and quiesce the device before export. */
    fill_bytes<<<256, 256>>>((unsigned char *)ptr, size, 0x5a);
    HIP_CHECK("fill launch", hipGetLastError());
    HIP_CHECK("hipDeviceSynchronize", hipDeviceSynchronize());

    int fd = -1;
    HIP_CHECK("hipMemGetHandleForAddressRange",
              hipMemGetHandleForAddressRange(&fd, (hipDeviceptr_t)ptr, size,
                                             hipMemRangeHandleTypeDmaBufFd,
                                             0));
    if (fd < 0) {
        fprintf(stderr, "export returned invalid fd %d\n", fd);
        return 1;
    }

    struct stat st;
    if (fstat(fd, &st))
        st.st_size = -1;
    printf("export fd=%d st_size=%lld\n", fd, (long long)st.st_size);

    /* The fd must survive exec in the child. */
    int flags = fcntl(fd, F_GETFD);
    if (flags < 0 || fcntl(fd, F_SETFD, flags & ~FD_CLOEXEC)) {
        perror("fcntl");
        return 1;
    }

    char fd_arg[16], len_arg[32];
    snprintf(fd_arg, sizeof(fd_arg), "%d", fd);
    snprintf(len_arg, sizeof(len_arg), "%" PRIu64, size);

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }
    if (!pid) {
        const char *args[12];
        int n = 0;

        args[n++] = probe;
        args[n++] = "--fd";
        args[n++] = fd_arg;
        args[n++] = "--direction";
        args[n++] = direction;
        args[n++] = "--length";
        args[n++] = len_arg;
        if (device) {
            args[n++] = "--device";
            args[n++] = device;
        }
        args[n] = NULL;
        execv(probe, (char *const *)args);
        perror(probe);
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        return 1;
    }

    /*
     * Only free after the probe (and thus the kernel's transient
     * attach/map/unmap/detach) has completed.
     */
    close(fd);
    HIP_CHECK("hipFree", hipFree(ptr));

    if (WIFEXITED(status)) {
        printf("probe-exit=%d\n", WEXITSTATUS(status));
        return WEXITSTATUS(status);
    }
    fprintf(stderr, "probe terminated abnormally (status 0x%x)\n", status);
    return 1;
}
