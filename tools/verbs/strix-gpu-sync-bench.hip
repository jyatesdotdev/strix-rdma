// SPDX-License-Identifier: GPL-2.0-only
/*
 * strix-gpu-sync-bench: the GPU-side cost of one tensor-parallel exchange, the
 * part the transport's latency figures leave out (docs/ZEROCOPY_VERBS.md,
 * round nineteen). Single host, no network.
 *
 * An exchange from GPU memory needs two hand-offs around the transport:
 *   producer  the kernel that wrote the activations has finished, its writes
 *             are released at system scope (the G4 gate showed this is
 *             required), and the CPU knows it, so it can post the send;
 *   consumer  once the receive completes, the GPU starts reading.
 * Each case times one way of doing those, with a kernel that writes 10 KiB
 * (one token at hidden size 2,560, fp32) and releases it.
 *
 * Build: hipcc --offload-arch=gfx1151 -O2 strix-gpu-sync-bench.hip -o strix-gpu-sync-bench
 * Run:   ./strix-gpu-sync-bench [--spin] [iterations]
 *        --spin sets hipDeviceScheduleSpin: host waits spin instead of yielding.
 */
#include <hip/hip_runtime.h>
#include <immintrin.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>

namespace {

constexpr unsigned kPayloadBytes = 10240;
constexpr unsigned kWords = kPayloadBytes / 4;
constexpr unsigned kThreads = 256;
constexpr unsigned kStop = 0xffffffffu;
constexpr unsigned kWarmup = 100;
constexpr int64_t kTimeoutNs = 1000000000;

/* Host-visible signalling words, each on its own 128-byte line. A
 * failing case releases the persistent kernel before exiting, or the runtime
 * would wait for it forever at exit. */
unsigned *g_go;
/* --flush: write the CPU's go store back to memory (clflush), for GPU readers
 * that do not see a line still dirty in the CPU's cache. */
bool g_flush;

void signal_gpu(unsigned *go, unsigned value)
{
	__atomic_store_n(go, value, __ATOMIC_RELEASE);
	if (g_flush) {
		_mm_clflush(go);
		_mm_sfence();
	}
}

#define CHECK(call)                                                            \
	do {                                                                   \
		hipError_t err_ = (call);                                      \
		if (err_ != hipSuccess) {                                      \
			std::fprintf(stderr, "FAIL: %s: %s\n", #call,          \
				     hipGetErrorString(err_));                 \
			std::exit(1);                                          \
		}                                                              \
	} while (0)

int64_t now_ns()
{
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

/* Spin until the host-visible flag reaches want, with a timeout so a broken
 * case fails instead of hanging. */
void wait_flag(const unsigned *flag, unsigned want, const char *what)
{
	int64_t deadline = now_ns() + kTimeoutNs;

	while (__atomic_load_n(flag, __ATOMIC_ACQUIRE) != want) {
		if (now_ns() > deadline) {
			std::fprintf(stderr, "FAIL: %s: flag never reached %u (flag %u, go %u, started %u)\n",
				     what, want, *flag, g_go[0], g_go[32]);
			signal_gpu(g_go, kStop);
			std::fflush(stdout);
			std::_Exit(1);
		}
	}
}

void report(const char *name, std::vector<int64_t> samples)
{
	samples.erase(samples.begin(), samples.begin() + std::min<size_t>(kWarmup, samples.size()));
	std::sort(samples.begin(), samples.end());
	auto pct = [&](double p) { return samples[(size_t)(p * (double)(samples.size() - 1))] / 1000.0; };
	double sum = 0;
	for (int64_t s : samples)
		sum += (double)s;
	std::printf("%-52s p50 %7.2f  p90 %7.2f  p99 %7.2f  p99.9 %7.2f  max %8.2f  avg %7.2f us\n",
		    name, pct(0.50), pct(0.90), pct(0.99), pct(0.999),
		    samples.back() / 1000.0, sum / samples.size() / 1000.0);
}

} // namespace

/* Write one token's activations, then release them at system scope. */
__device__ void write_payload(unsigned *buf, unsigned it)
{
	for (unsigned i = threadIdx.x; i < kWords; i += blockDim.x)
		buf[i] = it ^ (i * 2654435761u);
	__threadfence_system();
}

__global__ void empty_kernel() {}

__global__ void produce(unsigned *buf, unsigned it)
{
	write_payload(buf, it);
}

/* Produce, then tell the CPU from inside the kernel. */
__global__ void produce_flag(unsigned *buf, unsigned it, unsigned *done)
{
	write_payload(buf, it);
	__syncthreads();
	if (threadIdx.x == 0)
		__hip_atomic_store(done, it, __ATOMIC_RELEASE, __HIP_MEMORY_SCOPE_SYSTEM);
}

/* How the resident kernel reads the CPU's `go` word (--poll): a system-scope
 * atomic load (the default), a fetch-add of 0, or a load asking every GPU cache
 * level to be bypassed. All three see the CPU's store. */
enum PollMode : unsigned { kPollAtomicLoad, kPollFetchAdd, kPollUncachedAsm };

__device__ unsigned load_go(unsigned *go, unsigned mode)
{
	unsigned v;

	switch (mode) {
	case kPollFetchAdd:
		return __hip_atomic_fetch_add(go, 0u, __ATOMIC_ACQUIRE, __HIP_MEMORY_SCOPE_SYSTEM);
	case kPollUncachedAsm:
		asm volatile("global_load_b32 %0, %1, off glc slc dlc\n\ts_waitcnt vmcnt(0)"
			     : "=v"(v) : "v"(go) : "memory");
		return v;
	default:
		return __hip_atomic_load(go, __ATOMIC_ACQUIRE, __HIP_MEMORY_SCOPE_SYSTEM);
	}
}

/* Stays resident: waits for the CPU's go, produces, signals done.
 *
 * Every thread polls. A first version had only thread 0 poll and the rest wait
 * at __syncthreads(); on gfx1151 that deadlocked after the first go was seen,
 * so a barrier must not follow a loop only some threads run. The CPU writes the
 * next go only after it sees done, so all threads leave the poll with the same
 * value. */
__global__ void persistent(unsigned *buf, unsigned *go, unsigned *done,
			   unsigned *started, unsigned mode)
{
	unsigned last = 0;

	if (threadIdx.x == 0)
		__hip_atomic_store(started, 1u, __ATOMIC_RELEASE, __HIP_MEMORY_SCOPE_SYSTEM);
	for (;;) {
		unsigned it;

		while ((it = load_go(go, mode)) == last)
			;
		if (it == kStop)
			return;
		write_payload(buf, it);
		__syncthreads();
		if (threadIdx.x == 0)
			__hip_atomic_store(done, it, __ATOMIC_RELEASE, __HIP_MEMORY_SCOPE_SYSTEM);
		last = it;
	}
}

int main(int argc, char **argv)
{
	bool spin = false;
	unsigned iters = 20000, poll_mode = kPollAtomicLoad;

	for (int i = 1; i < argc; i++) {
		if (!std::strcmp(argv[i], "--spin"))
			spin = true;
		else if (!std::strcmp(argv[i], "--flush"))
			g_flush = true;
		else if (i + 1 < argc && !std::strcmp(argv[i], "--poll"))
			poll_mode = (unsigned)std::strtoul(argv[++i], nullptr, 10);
		else
			iters = (unsigned)std::strtoul(argv[i], nullptr, 10);
	}
	if (iters <= kWarmup) {
		std::fprintf(stderr, "usage: %s [--spin] [iterations > %u]\n", argv[0], kWarmup);
		return 2;
	}
	std::setvbuf(stdout, nullptr, _IOLBF, 0);
	if (spin)
		CHECK(hipSetDeviceFlags(hipDeviceScheduleSpin));

	hipStream_t stream;
	hipEvent_t event;
	unsigned *payload, *flags, *host_copy;
	CHECK(hipStreamCreateWithFlags(&stream, hipStreamNonBlocking));
	CHECK(hipEventCreateWithFlags(&event, hipEventDisableTiming));
	CHECK(hipMalloc(&payload, kPayloadBytes));
	CHECK(hipHostMalloc(&flags, 4096, hipHostMallocCoherent));
	CHECK(hipHostMalloc(&host_copy, kPayloadBytes, hipHostMallocDefault));
	unsigned *done = &flags[0], *go = &flags[32], *started = &flags[64];
	g_go = go;
	*started = 0;
	std::vector<int64_t> s(iters);

	std::printf("strix-gpu-sync-bench: %u iterations (first %u excluded), %u B payload, host waits %s\n",
		    iters, kWarmup, kPayloadBytes, spin ? "spin (hipDeviceScheduleSpin)" : "default");
	if (g_flush)
		std::printf("CPU stores to GPU-polled flags are flushed with clflush\n");

	for (unsigned i = 0; i < iters; i++) {
		int64_t t0 = now_ns();
		empty_kernel<<<1, 1, 0, stream>>>();
		CHECK(hipStreamSynchronize(stream));
		s[i] = now_ns() - t0;
	}
	report("launch empty kernel + hipStreamSynchronize", s);

	for (unsigned i = 0; i < iters; i++) {
		int64_t t0 = now_ns();
		produce<<<1, kThreads, 0, stream>>>(payload, i + 1);
		CHECK(hipStreamSynchronize(stream));
		s[i] = now_ns() - t0;
	}
	report("launch produce + hipStreamSynchronize", s);

	for (unsigned i = 0; i < iters; i++) {
		int64_t t0 = now_ns();
		produce<<<1, kThreads, 0, stream>>>(payload, i + 1);
		CHECK(hipEventRecord(event, stream));
		CHECK(hipEventSynchronize(event));
		s[i] = now_ns() - t0;
	}
	report("launch produce + hipEventSynchronize", s);

	__atomic_store_n(done, 0, __ATOMIC_RELEASE);
	for (unsigned i = 0; i < iters; i++) {
		int64_t t0 = now_ns();
		produce_flag<<<1, kThreads, 0, stream>>>(payload, i + 1, done);
		wait_flag(done, i + 1, "in-kernel flag");
		s[i] = now_ns() - t0;
		CHECK(hipStreamSynchronize(stream)); /* untimed: keep the queue empty */
	}
	report("launch produce, CPU spins on in-kernel flag", s);

	__atomic_store_n(done, 0, __ATOMIC_RELEASE);
	for (unsigned i = 0; i < iters; i++) {
		int64_t t0 = now_ns();
		produce<<<1, kThreads, 0, stream>>>(payload, i + 1);
		CHECK(hipStreamWriteValue32(stream, done, i + 1, 0));
		wait_flag(done, i + 1, "stream-written flag");
		s[i] = now_ns() - t0;
		CHECK(hipStreamSynchronize(stream));
	}
	report("launch produce, CPU spins on hipStreamWriteValue32", s);

	/* Consumer side: the next kernel is already queued behind a wait on a
	 * flag the CPU sets when the receive completes. */
	__atomic_store_n(done, 0, __ATOMIC_RELEASE);
	__atomic_store_n(go, 0, __ATOMIC_RELEASE);
	for (unsigned i = 0; i < iters; i++) {
		CHECK(hipStreamWaitValue32(stream, go, i + 1, hipStreamWaitValueEq, 0xffffffffu));
		produce_flag<<<1, kThreads, 0, stream>>>(payload, i + 1, done);
		int64_t park = now_ns() + 50000; /* let the GPU reach the wait */
		while (now_ns() < park)
			;
		int64_t t0 = now_ns();
		signal_gpu(go, i + 1);
		wait_flag(done, i + 1, "wait-value release");
		s[i] = now_ns() - t0;
		CHECK(hipStreamSynchronize(stream));
	}
	report("queued kernel: CPU flag -> hipStreamWaitValue32 -> done", s);

	/* No launches and no runtime: a resident kernel polls host memory. */
	__atomic_store_n(done, 0, __ATOMIC_RELEASE);
	__atomic_store_n(go, 0, __ATOMIC_RELEASE);
	persistent<<<1, kThreads, 0, stream>>>(payload, go, done, started, poll_mode);
	CHECK(hipGetLastError());
	for (unsigned i = 0; i < iters; i++) {
		int64_t t0 = now_ns();
		signal_gpu(go, i + 1);
		wait_flag(done, i + 1, "persistent kernel");
		s[i] = now_ns() - t0;
	}
	signal_gpu(go, kStop);
	CHECK(hipStreamSynchronize(stream));
	report(poll_mode == kPollUncachedAsm ? "persistent kernel (glc slc dlc poll): go -> done"
	       : poll_mode == kPollFetchAdd ? "persistent kernel (fetch-add poll): go -> done"
	       : "persistent kernel (atomic-load poll): go -> done", s);

	/* For comparison: staging through host memory instead of GPU-direct. */
	for (unsigned i = 0; i < iters; i++) {
		int64_t t0 = now_ns();
		CHECK(hipMemcpyAsync(host_copy, payload, kPayloadBytes, hipMemcpyDeviceToHost, stream));
		CHECK(hipStreamSynchronize(stream));
		s[i] = now_ns() - t0;
	}
	report("hipMemcpyAsync D2H 10 KiB + sync (host staging)", s);
	for (unsigned i = 0; i < iters; i++) {
		int64_t t0 = now_ns();
		CHECK(hipMemcpyAsync(payload, host_copy, kPayloadBytes, hipMemcpyHostToDevice, stream));
		CHECK(hipStreamSynchronize(stream));
		s[i] = now_ns() - t0;
	}
	report("hipMemcpyAsync H2D 10 KiB + sync (host staging)", s);

	CHECK(hipHostFree(host_copy));
	CHECK(hipHostFree(flags));
	CHECK(hipFree(payload));
	CHECK(hipEventDestroy(event));
	CHECK(hipStreamDestroy(stream));
	return 0;
}
