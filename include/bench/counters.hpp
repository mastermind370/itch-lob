// perf-style counters around a region of code.
//
// Linux: hardware counters through perf_event_open (cycles, instructions,
// branch misses, L1d read misses, last-level-cache misses), user space only.
// Unavailable inside many VMs or when /proc/sys/kernel/perf_event_paranoid is
// too strict; then hw is false and only the software counters below are filled.
//
// Everywhere: getrusage deltas, i.e. page faults and voluntary/involuntary
// context switches. On macOS this is all that's available without root (the
// PMU is only reachable through Instruments or the private kperf framework), and
// involuntary context switches are what explain rare multi-microsecond latency
// spikes.
#pragma once

#include <sys/resource.h>

#include <cstdint>
#include <cstring>
#include <string>

#if defined(__linux__)
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace bench {

struct CounterSample {
    bool hw = false;
    std::uint64_t cycles = 0, instructions = 0, branch_misses = 0, l1d_misses = 0, llc_misses = 0;
    long minor_faults = 0, major_faults = 0, voluntary_cs = 0, involuntary_cs = 0;

    [[nodiscard]] double ipc() const { return cycles ? static_cast<double>(instructions) / cycles : 0; }

    // Per-unit values (e.g. per message) as a JSON object.
    [[nodiscard]] std::string json(double per) const {
        auto f = [per](double v) { return std::to_string(per > 0 ? v / per : v); };
        std::string s = "{\"hw\": ";
        s += hw ? "true" : "false";
        if (hw) {
            s += ", \"ipc\": " + std::to_string(ipc());
            s += ", \"cycles_per\": " + f(static_cast<double>(cycles));
            s += ", \"instructions_per\": " + f(static_cast<double>(instructions));
            s += ", \"branch_misses_per\": " + f(static_cast<double>(branch_misses));
            s += ", \"l1d_misses_per\": " + f(static_cast<double>(l1d_misses));
            s += ", \"llc_misses_per\": " + f(static_cast<double>(llc_misses));
        }
        s += ", \"minor_faults\": " + std::to_string(minor_faults);
        s += ", \"major_faults\": " + std::to_string(major_faults);
        s += ", \"voluntary_cs\": " + std::to_string(voluntary_cs);
        s += ", \"involuntary_cs\": " + std::to_string(involuntary_cs) + "}";
        return s;
    }
};

class Counters {
public:
    Counters() {
#if defined(__linux__)
        fds_[0] = open(PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES);
        fds_[1] = open(PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS);
        fds_[2] = open(PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_MISSES);
        fds_[3] = open(PERF_TYPE_HW_CACHE, PERF_COUNT_HW_CACHE_L1D | (PERF_COUNT_HW_CACHE_OP_READ << 8) |
                                               (PERF_COUNT_HW_CACHE_RESULT_MISS << 16));
        fds_[4] = open(PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_MISSES);
        hw_ = fds_[0] >= 0 && fds_[1] >= 0;
#endif
    }

    Counters(const Counters&) = delete;
    Counters& operator=(const Counters&) = delete;

    ~Counters() {
#if defined(__linux__)
        for (int fd : fds_)
            if (fd >= 0) ::close(fd);
#endif
    }

    [[nodiscard]] bool hardware_available() const { return hw_; }

    void start() {
        ::getrusage(RUSAGE_SELF, &ru0_);
#if defined(__linux__)
        for (int fd : fds_)
            if (fd >= 0) {
                ::ioctl(fd, PERF_EVENT_IOC_RESET, 0);
                ::ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);
            }
#endif
    }

    CounterSample stop() {
        CounterSample s;
#if defined(__linux__)
        std::uint64_t v[5] = {};
        for (int i = 0; i < 5; ++i)
            if (fds_[i] >= 0) {
                ::ioctl(fds_[i], PERF_EVENT_IOC_DISABLE, 0);
                if (::read(fds_[i], &v[i], sizeof v[i]) != sizeof v[i]) v[i] = 0;
            }
        s.hw = hw_;
        s.cycles = v[0];
        s.instructions = v[1];
        s.branch_misses = v[2];
        s.l1d_misses = v[3];
        s.llc_misses = v[4];
#endif
        struct rusage ru1 {};
        ::getrusage(RUSAGE_SELF, &ru1);
        s.minor_faults = ru1.ru_minflt - ru0_.ru_minflt;
        s.major_faults = ru1.ru_majflt - ru0_.ru_majflt;
        s.voluntary_cs = ru1.ru_nvcsw - ru0_.ru_nvcsw;
        s.involuntary_cs = ru1.ru_nivcsw - ru0_.ru_nivcsw;
        return s;
    }

private:
#if defined(__linux__)
    static int open(std::uint32_t type, std::uint64_t config) {
        perf_event_attr a;
        std::memset(&a, 0, sizeof a);
        a.size = sizeof a;
        a.type = type;
        a.config = config;
        a.disabled = 1;
        a.exclude_kernel = 1;
        a.exclude_hv = 1;
        a.inherit = 1;  // count threads created after start(), e.g. the pipeline consumer
        return static_cast<int>(::syscall(SYS_perf_event_open, &a, 0, -1, -1, 0));
    }
    int fds_[5] = {-1, -1, -1, -1, -1};
#endif
    bool hw_ = false;
    struct rusage ru0_ {};
};

}  // namespace bench
