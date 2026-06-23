#include "AdcSampler.hpp"

#include <algorithm>
#include <iostream>
#include <utility>

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

void AdcSampler::worker() {
    const auto period = std::chrono::nanoseconds(1000000000ull / std::max<uint32_t>(1, config_.sample_rate_hz));
    auto next_sample = std::chrono::steady_clock::now();

    while (running_) {
        try {
            if (!adc_) adc_ = std::make_unique<MCP3202>(config_.adc);
            if (!adc_->isOpen()) adc_->open();

            std::array<uint16_t, 2> raw{{adc_->readChannel(0), adc_->readChannel(1)}};
            const auto sample_time = std::chrono::steady_clock::now();
            {
                std::lock_guard<std::mutex> lock(mtx_);
                if (total_frames_ == 0) {
                    first_sample_time_ = sample_time;
                    last_sample_time_ = sample_time;
                    measured_sample_rate_hz_ = config_.sample_rate_hz;
                } else {
                    last_sample_time_ = sample_time;
                }
                ring_[0][write_index_] = raw[0];
                ring_[1][write_index_] = raw[1];
                write_index_ = (write_index_ + 1) % ring_[0].size();
                valid_samples_ = std::min(valid_samples_ + 1, ring_[0].size());
                latest_raw_ = raw;
                total_frames_++;
                if (total_frames_ > 1) {
                    const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(last_sample_time_ - first_sample_time_).count();
                    if (elapsed_ns > 0) {
                        const uint64_t measured = ((total_frames_ - 1) * 1000000000ull + static_cast<uint64_t>(elapsed_ns / 2)) / static_cast<uint64_t>(elapsed_ns);
                        measured_sample_rate_hz_ = static_cast<uint32_t>(std::max<uint64_t>(1, measured));
                    }
                }
                healthy_ = true;
                last_error_.clear();
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
            next_sample = std::chrono::steady_clock::now();
        }

        next_sample += period;
        auto now = std::chrono::steady_clock::now();
        if (next_sample > now) {
            std::this_thread::sleep_until(next_sample);
        } else {
            // If Linux scheduling falls behind, do not spin trying to catch up forever.
            next_sample = now;
        }
    }
}

std::vector<uint16_t> AdcSampler::copyRecentLocked(int channel, size_t max_points) const {
    const auto& ring = ring_[channel];
    const size_t count = std::min(valid_samples_, ring.size());
    if (count == 0) return {};

    std::vector<uint16_t> ordered;
    ordered.reserve(count);
    const size_t start = (write_index_ + ring.size() - count) % ring.size();
    for (size_t i = 0; i < count; ++i) {
        ordered.push_back(ring[(start + i) % ring.size()]);
    }

    if (max_points == 0 || ordered.size() <= max_points) return ordered;

    // Min/max decimation keeps short transients visible in the browser micro-scope.
    std::vector<uint16_t> decimated;
    decimated.reserve(max_points);
    const size_t buckets = std::max<size_t>(1, max_points / 2);
    const double step = static_cast<double>(ordered.size()) / static_cast<double>(buckets);
    for (size_t b = 0; b < buckets; ++b) {
        size_t begin = static_cast<size_t>(b * step);
        size_t end = static_cast<size_t>((b + 1) * step);
        end = std::min(end, ordered.size());
        if (begin >= end) continue;
        auto mm = std::minmax_element(ordered.begin() + begin, ordered.begin() + end);
        decimated.push_back(*mm.first);
        if (decimated.size() < max_points) decimated.push_back(*mm.second);
    }
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
    return ordered;
}

AdcScopeData AdcSampler::snapshot(size_t max_points) const {
    std::lock_guard<std::mutex> lock(mtx_);
    AdcScopeData data;
    data.enabled = config_.enabled;
    data.running = running_;
    data.healthy = healthy_;
    data.bitbang = config_.adc.bitbang;
    data.sample_rate_hz = config_.sample_rate_hz;
    data.measured_sample_rate_hz = measured_sample_rate_hz_ ? measured_sample_rate_hz_ : config_.sample_rate_hz;
    data.latest_raw = latest_raw_;
    data.latest_volts = {{
        (static_cast<double>(latest_raw_[0]) * config_.vref) / 4095.0,
        (static_cast<double>(latest_raw_[1]) * config_.vref) / 4095.0
    }};
    data.total_frames = total_frames_;
    data.dropped_reads = dropped_reads_;
    data.last_error = last_error_;
    data.samples[0] = copyRecentLocked(0, max_points);
    data.samples[1] = copyRecentLocked(1, max_points);
    return data;
}

AdcScopeData AdcSampler::recent(size_t frames) const {
    std::lock_guard<std::mutex> lock(mtx_);
    AdcScopeData data;
    data.enabled = config_.enabled;
    data.running = running_;
    data.healthy = healthy_;
    data.bitbang = config_.adc.bitbang;
    data.sample_rate_hz = config_.sample_rate_hz;
    data.measured_sample_rate_hz = measured_sample_rate_hz_ ? measured_sample_rate_hz_ : config_.sample_rate_hz;
    data.latest_raw = latest_raw_;
    data.latest_volts = {{
        (static_cast<double>(latest_raw_[0]) * config_.vref) / 4095.0,
        (static_cast<double>(latest_raw_[1]) * config_.vref) / 4095.0
    }};
    data.total_frames = total_frames_;
    data.dropped_reads = dropped_reads_;
    data.last_error = last_error_;
    data.samples[0] = copyRecentExactLocked(0, frames);
    data.samples[1] = copyRecentExactLocked(1, frames);
    return data;
}
