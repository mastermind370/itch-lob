// Small platform helpers for the pipeline threads.
#pragma once

#include <cstdio>

#if defined(__x86_64__)
#include <immintrin.h>
#endif
#if defined(__APPLE__)
#include <pthread.h>
#include <sys/qos.h>
#elif defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace pipeline {

// Spin-wait hint: lets the other hyperthread run and saves power while busy-waiting.
inline void cpu_relax() noexcept {
#if defined(__x86_64__)
    _mm_pause();
#elif defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#endif
}

// Best effort at keeping a hot thread on a fast, dedicated core.
// Linux: pin to `cpu`. macOS has no hard affinity on Apple Silicon, so ask for
// the user-interactive QoS class, which the scheduler keeps on performance cores.
// Returns a short description of what was done.
inline const char* place_thread(int cpu) {
#if defined(__APPLE__)
    (void)cpu;
    return pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0) == 0 ? "qos=user-interactive" : "qos failed";
#elif defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof set, &set) == 0 ? "pinned" : "pin failed";
#else
    (void)cpu;
    return "unplaced";
#endif
}

}  // namespace pipeline
