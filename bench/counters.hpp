// SPDX-License-Identifier: Apache-2.0 OR MIT
#pragma once

// Hardware counters read through perf_event_open(2) directly, rather than by
// shelling out to perf(1).
//
// Two reasons. The perf binary is absent on plenty of hosts, including the WSL2
// setup this was developed on, while the syscall works there because a core PMU
// is exposed and perf_event_paranoid permits user-space counting. And scoping
// the count to the probe loop alone is more precise than wrapping the whole
// process, where startup and workload generation dominate.
//
// Requires /proc/sys/kernel/perf_event_paranoid <= 2 for user-space events.
//
// PERF_COUNT_HW_REF_CPU_CYCLES is here for one reason: it is the only way this
// harness can see frequency at all. CPU_CYCLES is immune to frequency scaling,
// which is exactly why the rest of the report trusts it, and exactly why it
// cannot answer the AVX-512 downclocking question. REF_CPU_CYCLES ticks at the
// invariant reference rate instead, so cycles/ref-cycles is the effective
// frequency as a multiple of nominal: above 1.0 the part is in turbo, below 1.0
// it has been clocked down. Comparing that ratio across kernels in one run is
// the downclock measurement.

#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace aparajita::bench {

class Counter {
public:
    Counter(std::uint64_t config, std::string name)
        : name_(std::move(name)) {
        perf_event_attr attr{};
        std::memset(&attr, 0, sizeof(attr));
        attr.type = PERF_TYPE_HARDWARE;
        attr.size = sizeof(attr);
        attr.config = config;
        attr.disabled = 1;
        attr.exclude_kernel = 1;
        attr.exclude_hv = 1;
        fd_ = static_cast<int>(::syscall(__NR_perf_event_open, &attr, 0, -1, -1, 0));
        if (fd_ < 0) {
            errno_ = errno;
        }
    }

    Counter(const Counter&) = delete;
    Counter& operator=(const Counter&) = delete;

    ~Counter() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    bool available() const noexcept { return fd_ >= 0; }
    int last_errno() const noexcept { return errno_; }
    const std::string& name() const noexcept { return name_; }

    void start() const noexcept {
        if (fd_ < 0) return;
        ::ioctl(fd_, PERF_EVENT_IOC_RESET, 0);
        ::ioctl(fd_, PERF_EVENT_IOC_ENABLE, 0);
    }

    std::uint64_t stop() const noexcept {
        if (fd_ < 0) return 0;
        ::ioctl(fd_, PERF_EVENT_IOC_DISABLE, 0);
        std::uint64_t value = 0;
        const ssize_t got = ::read(fd_, &value, sizeof(value));
        return got == static_cast<ssize_t>(sizeof(value)) ? value : 0;
    }

private:
    int fd_{-1};
    int errno_{0};
    std::string name_;
};

// Five events, and the count is deliberate. On Intel, instructions, cycles, and
// ref-cycles occupy the three fixed-function counters, leaving branches and
// branch-misses to two of the four general-purpose slots. Nothing multiplexes, so
// no reading is scaled by the kernel and the numbers are exact rather than
// extrapolated. A sixth event would change that and every value with it.
struct CounterSet {
    Counter cycles{PERF_COUNT_HW_CPU_CYCLES, "cycles"};
    Counter ref_cycles{PERF_COUNT_HW_REF_CPU_CYCLES, "ref-cycles"};
    Counter instructions{PERF_COUNT_HW_INSTRUCTIONS, "instructions"};
    Counter branches{PERF_COUNT_HW_BRANCH_INSTRUCTIONS, "branches"};
    Counter branch_misses{PERF_COUNT_HW_BRANCH_MISSES, "branch-misses"};

    bool all_available() const noexcept {
        return cycles.available() && ref_cycles.available() && instructions.available() &&
               branches.available() && branch_misses.available();
    }
};

struct Reading {
    std::uint64_t cycles{};
    std::uint64_t ref_cycles{};
    std::uint64_t instructions{};
    std::uint64_t branches{};
    std::uint64_t branch_misses{};
};

} // namespace aparajita::bench
