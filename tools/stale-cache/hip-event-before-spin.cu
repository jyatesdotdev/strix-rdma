// SPDX-License-Identifier: MIT
/*
 * Verify the exact ROCm ordering used by the production TP gate:
 *
 *   producer -> hipEventRecord(release-to-system) -> later spin kernel
 *                     |
 *                     +-> service thread hipEventSynchronize()
 *
 * The event wait must return while the subsequently enqueued spin kernel
 * is still running.  Waiting for the stream tail instead would deadlock
 * bilateral TP: each host would wait for its RX spin before submitting TX.
 *
 * This is a HIP/CLR implementation gate, not a thunderbolt-stream test.
 */
#include <hip/hip_runtime.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>

struct control {
	uint32_t spin_started;
	uint32_t release_spin;
};

static __global__ void producer(uint32_t *out)
{
	if (threadIdx.x == 0)
		__hip_atomic_store(out, 0x51a9c0deu, __ATOMIC_RELEASE,
				   __HIP_MEMORY_SCOPE_SYSTEM);
}

static __global__ void later_spin(struct control *ctl)
{
	if (threadIdx.x != 0)
		return;

	__hip_atomic_store(&ctl->spin_started, 1u, __ATOMIC_RELEASE,
			   __HIP_MEMORY_SCOPE_SYSTEM);
	while (!__hip_atomic_load(&ctl->release_spin, __ATOMIC_ACQUIRE,
				  __HIP_MEMORY_SCOPE_SYSTEM))
		__builtin_amdgcn_s_sleep(1);
}

#define HIP_CHECK(call)                                                    \
	do {                                                                \
		hipError_t err_ = (call);                                   \
		if (err_ != hipSuccess) {                                   \
			std::fprintf(stderr, "%s: %s\n", #call,              \
				     hipGetErrorString(err_));                 \
			return 1;                                           \
		}                                                           \
	} while (0)

int main(void)
{
	struct control *ctl = nullptr;
	uint32_t *out = nullptr;
	hipEvent_t ready = nullptr;

	HIP_CHECK(hipSetDevice(0));
	HIP_CHECK(hipHostMalloc(reinterpret_cast<void **>(&ctl), sizeof(*ctl),
			       0));
	ctl->spin_started = 0;
	ctl->release_spin = 0;
	HIP_CHECK(hipMalloc(reinterpret_cast<void **>(&out), sizeof(*out)));
	HIP_CHECK(hipMemset(out, 0, sizeof(*out)));
	/* Timing remains enabled; do not add hipEventDisableTiming. */
	HIP_CHECK(hipEventCreateWithFlags(&ready, hipEventReleaseToSystem));

	hipLaunchKernelGGL(producer, dim3(1), dim3(1), 0, 0, out);
	HIP_CHECK(hipEventRecord(ready, 0));
	hipLaunchKernelGGL(later_spin, dim3(1), dim3(64), 0, 0, ctl);
	HIP_CHECK(hipGetLastError());

	std::atomic<int> sync_result{-1};
	std::thread waiter([&] {
		hipError_t err = hipSetDevice(0);

		if (err == hipSuccess)
			err = hipEventSynchronize(ready);
		sync_result.store(static_cast<int>(err),
				  std::memory_order_release);
	});

	auto deadline = std::chrono::steady_clock::now() +
			std::chrono::seconds(5);
	while (!ctl->spin_started && std::chrono::steady_clock::now() < deadline)
		std::this_thread::yield();
	if (!ctl->spin_started) {
		std::fprintf(stderr, "FAIL: later spin kernel never started\n");
		ctl->release_spin = 1;
		waiter.join();
		return 1;
	}

	/* Give the waiter up to two seconds while the spin remains blocked. */
	deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (sync_result.load(std::memory_order_acquire) < 0 &&
	       std::chrono::steady_clock::now() < deadline)
		std::this_thread::yield();
	int before_release = sync_result.load(std::memory_order_acquire);

	ctl->release_spin = 1;
	waiter.join();
	HIP_CHECK(hipDeviceSynchronize());
	uint32_t got = 0;
	HIP_CHECK(hipMemcpy(&got, out, sizeof(got), hipMemcpyDeviceToHost));

	bool pass = before_release == static_cast<int>(hipSuccess) &&
		    got == 0x51a9c0deu;
	std::printf("event_sync_while_later_spin_active=%s rc=%d "
		    "producer=%#x\n",
		    pass ? "PASS" : "FAIL", before_release, got);

	HIP_CHECK(hipEventDestroy(ready));
	HIP_CHECK(hipFree(out));
	HIP_CHECK(hipHostFree(ctl));
	return pass ? 0 : 1;
}
