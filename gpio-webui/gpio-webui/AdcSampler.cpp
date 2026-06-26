#include "AdcSampler.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <pthread.h>
#include <sched.h>
#include <sstream>
#include <time.h>
#include <utility>

namespace {
int64_t monotonicNs() {
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000ll + static_cast<int64_t>(ts.tv_nsec);
}

timespec nsToTimespec(int64_t ns) {
    timespec ts{};
    ts.tv_sec = static_cast<time_t>(ns / 1000000000ll);
    ts.tv_nsec = static_cast<long>(ns % 1000000000ll);
    if (ts.tv_nsec < 0) {
        ts.tv_sec--;
        ts.tv_nsec += 1000000000l;
    }
    return ts;
}

void cpuRelax() {
#if defined(__aarch64__)
    __asm__ __volatile__("yield" ::: "memory");
#else
    __asm__ __volatile__("" ::: "memory");
#endif
}

void sleepUntilMonotonicNs(int64_t target_ns, int64_t spin_ns, const std::atomic<bool>& running) {
    const int64_t sleep_target = target_ns - std::max<int64_t>(0, spin_ns);
    while (running.load(std::memory_order_relaxed)) {
        int64_t now = monotonicNs();
        if (now >= sleep_target) break;
        timespec ts = nsToTimespec(sleep_target);
        int rc;
        do {
            rc = ::clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr);
        } while (rc == EINTR && running.load(std::memory_order_relaxed));
        if (rc != 0 && rc != EINTR) break;
    }

    while (running.load(std::memory_order_relaxed) && monotonicNs() < target_ns) {
        cpuRelax();
    }
}
}

AdcSampler::AdcSampler() : AdcSampler(Config()) {}

AdcSampler::AdcSampler(Config config)
    : config_(std::move(config)) {
    const size_t n = std::max<size_t>(1, config_.history_samples);
    ring_[0].assign(n, 0);
    ring_[1].assign(n, 0);
}

AdcSampler::~AdcSampler() {
    stop();
}

void AdcSampler::start() {
    if (!config_.enabled || running_) return;
    running_ = true;
    worker_ = std::thread(&AdcSampler::worker, this);
}

void AdcSampler::stop() {
    running_ = false;
    if (worker_.joinable()) worker_.join();
    if (adc_) adc_->close();
}

bool AdcSampler::isEnabled() const {
    return config_.enabled;
}

uint64_t AdcSampler::ema(uint64_t old_value, uint64_t sample, uint32_t weight) {
    if (old_value == 0) return sample;
    return (old_value * weight + sample) / (weight + 1);
}

void AdcSampler::configureWorkerScheduling() {
    std::ostringstream status;
    bool ok = true;

    if (config_.cpu_affinity >= 0) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(config_.cpu_affinity, &cpuset);
        int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
        if (rc == 0) {
            status << "affinity=cpu" << config_.cpu_affinity << " ";
        } else {
            ok = false;
            status << "affinity failed: " << std::strerror(rc) << " ";
        }
    }

    if (config_.realtime) {
        sched_param sp{};
        sp.sched_priority = std::max(1, std::min(99, config_.realtime_priority));
        int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
        if (rc == 0) {
            realtime_active_ = true;
            status << "SCHED_FIFO priority " << sp.sched_priority;
        } else {
            ok = false;
            realtime_active_ = false;
            status << "SCHED_FIFO failed: " << std::strerror(rc);
        }
    } else if (status.str().empty()) {
        status << "normal scheduler";
    }

    std::lock_guard<std::mutex> lock(mtx_);
    scheduler_status_ = status.str();
    if (!ok && !last_error_.empty()) scheduler_status_ += "; last_error=" + last_error_;
}

void AdcSampler::worker() {
    configureWorkerScheduling();

    const int64_t period_ns = static_cast<int64_t>(1000000000ull / std::max<uint32_t>(1, config_.sample_rate_hz));
    // Sleep most of the interval with absolute CLOCK_MONOTONIC deadlines, then
    // spin briefly to avoid sub-millisecond scheduler oversleep stretching the
    // effective period. 80 us is conservative for a 277.8 us period at 3600 Hz
    // and still leaves most CPU time available between samples.
    const int64_t spin_ns = std::min<int64_t>(80000, std::max<int64_t>(0, period_ns / 3));
    int64_t next_sample_ns = monotonicNs();

    while (running_) {
        next_sample_ns += period_ns;
        sleepUntilMonotonicNs(next_sample_ns, spin_ns, running_);
        if (!running_) break;

        try {
            if (!adc_) adc_ = std::make_unique<MCP3202>(config_.adc);
            if (!adc_->isOpen()) adc_->open();

            const auto read_start = std::chrono::steady_clock::now();
            std::array<uint16_t, 2> raw = adc_->readBothChannels();
            const auto sample_time = std::chrono::steady_clock::now();
            const uint64_t read_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(sample_time - read_start).count());

            const int64_t after_read_ns = monotonicNs();
            const int64_t late_ns = after_read_ns - next_sample_ns;
            if (late_ns > period_ns) {
                std::lock_guard<std::mutex> lock(mtx_);
                overruns_++;
                max_overrun_us_ = std::max<uint64_t>(max_overrun_us_, static_cast<uint64_t>(late_ns / 1000));
            }

            const auto lock_wait_start = std::chrono::steady_clock::now();
            mtx_.lock();
            const auto lock_acquired = std::chrono::steady_clock::now();
            const uint64_t wait_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(lock_acquired - lock_wait_start).count());
            {
                if (total_frames_ == 0) {
                    first_sample_time_ = sample_time;
                    last_sample_time_ = sample_time;
                    rate_window_start_ = sample_time;
                    rate_window_frames_ = 0;
                    measured_sample_rate_hz_ = config_.sample_rate_hz;
                    lifetime_sample_rate_hz_ = config_.sample_rate_hz;
                } else {
                    last_sample_time_ = sample_time;
                }

                ring_[0][write_index_] = raw[0];
                ring_[1][write_index_] = raw[1];
                write_index_ = (write_index_ + 1) % ring_[0].size();
                valid_samples_ = std::min(valid_samples_ + 1, ring_[0].size());
                latest_raw_ = raw;
                total_frames_++;

                avg_frame_read_us_ = ema(avg_frame_read_us_, read_us);
                max_frame_read_us_ = std::max(max_frame_read_us_, read_us);
                avg_mutex_wait_us_ = ema(avg_mutex_wait_us_, wait_us);
                max_mutex_wait_us_ = std::max(max_mutex_wait_us_, wait_us);

                if (total_frames_ > 1) {
                    const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(last_sample_time_ - first_sample_time_).count();
                    if (elapsed_ns > 0) {
                        const uint64_t measured = ((total_frames_ - 1) * 1000000000ull + static_cast<uint64_t>(elapsed_ns / 2)) / static_cast<uint64_t>(elapsed_ns);
                        lifetime_sample_rate_hz_ = static_cast<uint32_t>(std::max<uint64_t>(1, measured));
                    }
                }

                const auto win_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(sample_time - rate_window_start_).count();
                if (win_ns >= 1000000000ll) {
                    const uint64_t frames = total_frames_ - rate_window_frames_;
                    const uint64_t recent = (frames * 1000000000ull + static_cast<uint64_t>(win_ns / 2)) / static_cast<uint64_t>(win_ns);
                    measured_sample_rate_hz_ = static_cast<uint32_t>(std::max<uint64_t>(1, recent));
                    rate_window_start_ = sample_time;
                    rate_window_frames_ = total_frames_;
                }

                healthy_ = true;
                last_error_.clear();
            }
            const auto unlock_time = std::chrono::steady_clock::now();
            const uint64_t hold_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(unlock_time - lock_acquired).count());
            avg_mutex_hold_us_ = ema(avg_mutex_hold_us_, hold_us);
            max_mutex_hold_us_ = std::max(max_mutex_hold_us_, hold_us);
            mtx_.unlock();

            // If a rare stall missed multiple periods, skip exactly the missed
            // deadlines and resume on the absolute schedule instead of stretching
            // every future interval. Small lateness keeps phase locked.
            const int64_t done_ns = monotonicNs();
            if (done_ns - next_sample_ns > period_ns * 4) {
                const int64_t missed = (done_ns - next_sample_ns) / period_ns;
                next_sample_ns += missed * period_ns;
            }
        } catch (const std::exception& e) {
            {
                std::lock_guard<std::mutex> lock(mtx_);
                dropped_reads_++;
                healthy_ = false;
                last_error_ = e.what();
            }
            if (adc_) adc_->close();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            next_sample_ns = monotonicNs();
        }
    }
}

std::vector<uint16_t> AdcSampler::copyRecentDecimatedLocked(int channel, size_t max_points) const {
    const auto& ring = ring_[channel];
    const size_t count = std::min(valid_samples_, ring.size());
    if (count == 0 || max_points == 0) return {};

    const size_t start = (write_index_ + ring.size() - count) % ring.size();
    if (count <= max_points) {
        std::vector<uint16_t> ordered;
        ordered.reserve(count);
        for (size_t i = 0; i < count; ++i) ordered.push_back(ring[(start + i) % ring.size()]);
        snapshot_samples_copied_ += ordered.size();
        return ordered;
    }

    // Direct min/max decimation from the ring: no full-history temporary vector.
    std::vector<uint16_t> decimated;
    decimated.reserve(max_points);
    const size_t buckets = std::max<size_t>(1, max_points / 2);
    for (size_t b = 0; b < buckets && decimated.size() < max_points; ++b) {
        const size_t begin = (b * count) / buckets;
        size_t end = ((b + 1) * count) / buckets;
        end = std::min(end, count);
        if (begin >= end) continue;
        uint16_t mn = ring[(start + begin) % ring.size()];
        uint16_t mx = mn;
        for (size_t i = begin + 1; i < end; ++i) {
            uint16_t v = ring[(start + i) % ring.size()];
            mn = std::min(mn, v);
            mx = std::max(mx, v);
        }
        decimated.push_back(mn);
        if (decimated.size() < max_points) decimated.push_back(mx);
    }
    snapshot_samples_copied_ += decimated.size();
    return decimated;
}

std::vector<uint16_t> AdcSampler::copyRecentExactLocked(int channel, size_t frames) const {
    const auto& ring = ring_[channel];
    const size_t count = std::min({frames, valid_samples_, ring.size()});
    if (count == 0) return {};

    std::vector<uint16_t> ordered;
    ordered.reserve(count);
    const size_t start = (write_index_ + ring.size() - count) % ring.size();
    for (size_t i = 0; i < count; ++i) {
        ordered.push_back(ring[(start + i) % ring.size()]);
    }
    snapshot_samples_copied_ += ordered.size();
    return ordered;
}

void AdcSampler::fillStatusLocked(AdcScopeData& data) const {
    data.enabled = config_.enabled;
    data.running = running_;
    data.healthy = healthy_;
    data.bitbang = config_.adc.bitbang;
    data.sample_rate_hz = config_.sample_rate_hz;
    data.measured_sample_rate_hz = measured_sample_rate_hz_ ? measured_sample_rate_hz_ : config_.sample_rate_hz;
    data.lifetime_sample_rate_hz = lifetime_sample_rate_hz_ ? lifetime_sample_rate_hz_ : data.measured_sample_rate_hz;
    data.latest_raw = latest_raw_;
    data.latest_volts = {{
        (static_cast<double>(latest_raw_[0]) * config_.vref) / 4095.0,
        (static_cast<double>(latest_raw_[1]) * config_.vref) / 4095.0
    }};
    data.total_frames = total_frames_;
    data.dropped_reads = dropped_reads_;
    data.overruns = overruns_;
    data.max_overrun_us = max_overrun_us_;
    data.avg_frame_read_us = avg_frame_read_us_;
    data.max_frame_read_us = max_frame_read_us_;
    data.avg_mutex_wait_us = avg_mutex_wait_us_;
    data.max_mutex_wait_us = max_mutex_wait_us_;
    data.avg_mutex_hold_us = avg_mutex_hold_us_;
    data.max_mutex_hold_us = max_mutex_hold_us_;
    data.snapshot_count = snapshot_count_;
    data.snapshot_samples_copied = snapshot_samples_copied_;
    data.realtime_requested = config_.realtime;
    data.realtime_active = realtime_active_;
    data.realtime_priority = config_.realtime_priority;
    data.cpu_affinity = config_.cpu_affinity;
    data.scheduler_status = scheduler_status_;
    data.valid_samples = valid_samples_;
    data.history_capacity_samples = ring_[0].size();
    data.last_error = last_error_;
}

AdcScopeData AdcSampler::status() const {
    std::lock_guard<std::mutex> lock(mtx_);
    AdcScopeData data;
    fillStatusLocked(data);
    return data;
}

AdcScopeData AdcSampler::snapshot(size_t max_points) const {
    std::lock_guard<std::mutex> lock(mtx_);
    snapshot_count_++;
    AdcScopeData data;
    fillStatusLocked(data);
    data.samples[0] = copyRecentDecimatedLocked(0, max_points);
    data.samples[1] = copyRecentDecimatedLocked(1, max_points);
    return data;
}

AdcScopeData AdcSampler::recent(size_t frames) const {
    std::lock_guard<std::mutex> lock(mtx_);
    snapshot_count_++;
    AdcScopeData data;
    fillStatusLocked(data);
    data.samples[0] = copyRecentExactLocked(0, frames);
    data.samples[1] = copyRecentExactLocked(1, frames);
    return data;
}
