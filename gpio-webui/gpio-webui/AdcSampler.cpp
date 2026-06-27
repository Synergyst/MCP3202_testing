#include "AdcSampler.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <pthread.h>
#include <sched.h>
#include <sstream>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <utility>
#include <vector>

// ─── helpers for absolute-time sleeping ────────────────────────────────────

namespace {

int64_t monotonicNs() {
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000ll + static_cast<int64_t>(ts.tv_nsec);
}

timespec nsToTimespec(int64_t ns) {
    timespec ts{};
    ts.tv_sec  = static_cast<time_t>(ns / 1000000000ll);
    ts.tv_nsec = static_cast<long>(ns % 1000000000ll);
    if (ts.tv_nsec < 0) { ts.tv_sec--; ts.tv_nsec += 1000000000l; }
    return ts;
}

void cpuRelax() {
#if defined(__aarch64__)
    __asm__ __volatile__("yield" ::: "memory");
#else
    __asm__ __volatile__("" ::: "memory");
#endif
}

void sleepUntilMonotonicNs(int64_t target_ns, int64_t spin_ns,
                            const std::atomic<bool>& running) {
    const int64_t sleep_target = target_ns - std::max<int64_t>(0, spin_ns);
    while (running.load(std::memory_order_relaxed)) {
        int64_t now = monotonicNs();
        if (now >= sleep_target) break;
        timespec ts = nsToTimespec(sleep_target);
        int rc;
        do { rc = ::clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr); }
        while (rc == EINTR && running.load(std::memory_order_relaxed));
        if (rc != 0 && rc != EINTR) break;
    }
    while (running.load(std::memory_order_relaxed) && monotonicNs() < target_ns)
        cpuRelax();
}

// ─── RP2040 binary protocol helpers ────────────────────────────────────────

// Packet magic: 'ADC2' little-endian = 0x32434441
constexpr uint32_t RP2040_MAGIC   = 0x32434441u;
constexpr uint16_t RP2040_VERSION = 1u;

struct __attribute__((packed)) Rp2040Header {
    uint32_t magic;
    uint16_t version;
    uint16_t header_bytes;
    uint32_t sample_rate_hz;
    uint32_t frame_count;
    uint32_t sequence_start;
    uint32_t flags;
    uint32_t lost_frames;
    uint32_t reserved;
};

struct __attribute__((packed)) Rp2040Frame {
    uint32_t seq;
    uint16_t ch0;
    uint16_t ch1;
};

static_assert(sizeof(Rp2040Header) == 32, "header size");
static_assert(sizeof(Rp2040Frame)  ==  8, "frame size");

uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t len) {
    crc = ~crc;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b)
            crc = (crc >> 1) ^ (0xEDB88320u & (-(int32_t)(crc & 1u)));
    }
    return ~crc;
}

bool readExact(int fd, uint8_t* out, size_t len, const std::atomic<bool>& running) {
    size_t got = 0;
    while (got < len && running.load(std::memory_order_relaxed)) {
        ssize_t rc = ::read(fd, out + got, len - got);
        if (rc > 0)  { got += static_cast<size_t>(rc); continue; }
        if (rc == 0) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); continue; }
        if (errno == EINTR)              continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2)); continue;
        }
        return false;
    }
    return got == len;
}

bool configureTtyRaw(int fd) {
    termios tio{};
    if (::tcgetattr(fd, &tio) != 0) return false;
    ::cfmakeraw(&tio);
    ::cfsetispeed(&tio, B115200);
    ::cfsetospeed(&tio, B115200);
    tio.c_cflag |= (CLOCAL | CREAD);
    tio.c_cflag &= ~CRTSCTS;
    tio.c_cc[VMIN]  = 0;
    tio.c_cc[VTIME] = 1;
    return ::tcsetattr(fd, TCSANOW, &tio) == 0;
}

} // namespace

// ─── AdcSampler implementation ─────────────────────────────────────────────

AdcSampler::AdcSampler() : AdcSampler(Config()) {}

AdcSampler::AdcSampler(Config config)
    : config_(std::move(config)) {
    const size_t n = std::max<size_t>(1, config_.history_samples);
    ring_[0].assign(n, 0);
    ring_[1].assign(n, 0);
}

AdcSampler::~AdcSampler() { stop(); }

void AdcSampler::stop() {
    running_ = false;
    if (worker_.joinable()) worker_.join();
    if (adc_) adc_->close();
}

bool AdcSampler::sendRp2040RateCommandLocked(int fd, uint32_t rate, std::string& error) {
    if (fd < 0) {
        error = "RP2040 rate update deferred: device is not connected";
        return false;
    }
    const std::string cmd = "S" + std::to_string(rate) + "\n";
    size_t sent = 0;
    while (sent < cmd.size()) {
        ssize_t rc = ::write(fd, cmd.data() + sent, cmd.size() - sent);
        if (rc > 0) {
            sent += static_cast<size_t>(rc);
            continue;
        }
        if (rc < 0 && errno == EINTR) continue;
        error = "Failed to write rate command to RP2040: " + std::string(std::strerror(errno));
        return false;
    }
    return true;
}

void AdcSampler::setSampleRate(uint32_t rate) {
    if (rate == 0 || rate > 100000) {
        std::lock_guard<std::mutex> lock(mtx_);
        last_error_ = "Invalid RP2040 sample rate " + std::to_string(rate) + " Hz; valid range is 1..100000";
        return;
    }

    std::lock_guard<std::mutex> lock(mtx_);
    config_.sample_rate_hz = rate;
    if (config_.adc_source != "rp2040") return;

    rp2040_pending_rate_hz_ = rate;
    std::string error;
    if (sendRp2040RateCommandLocked(rp2040_fd_, rate, error)) {
        rp2040_pending_rate_hz_ = 0;
        if (!last_error_.empty() && last_error_.find("rate") != std::string::npos) last_error_.clear();
    } else {
        last_error_ = error;
    }
}

void AdcSampler::start() {
    if (!config_.enabled || running_) return;
    running_ = true;
    if (config_.adc_source == "rp2040")
        worker_ = std::thread(&AdcSampler::workerRp2040, this);
    else
        worker_ = std::thread(&AdcSampler::workerSpidev, this);
}

void AdcSampler::updateConfig(Config new_config) {
    stop();
    {
        std::lock_guard<std::mutex> lock(mtx_);
        config_ = std::move(new_config);
        // Reset buffers for the new source
        ring_[0].assign(std::max<size_t>(1, config_.history_samples), 0);
        ring_[1].assign(std::max<size_t>(1, config_.history_samples), 0);
        write_index_ = 0;
        valid_samples_ = 0;
        total_frames_ = 0;
        dropped_reads_ = 0;
        overruns_ = 0;
        // Reset RP2040 stats
        rp2040_connected_ = false;
        rp2040_packets_ok_ = 0;
        rp2040_packets_crc_bad_ = 0;
        rp2040_sequence_gaps_ = 0;
        rp2040_firmware_lost_frames_ = 0;
        rp2040_firmware_flags_ = 0;
        rp2040_declared_rate_hz_ = 0;
        rp2040_have_last_seq_ = false;
        rp2040_last_seq_ = 0;
        rp2040_fd_ = -1;
        rp2040_pending_rate_hz_ = (config_.adc_source == "rp2040") ? config_.sample_rate_hz : 0;
    }
    start();
    if (config_.adc_source == "rp2040") {
        setSampleRate(config_.sample_rate_hz);
    }
}

bool AdcSampler::isEnabled() const { return config_.enabled; }

AdcSampler::Config AdcSampler::config() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return config_;
}

uint64_t AdcSampler::ema(uint64_t old_value, uint64_t sample, uint32_t weight) {
    if (old_value == 0) return sample;
    return (old_value * weight + sample) / (weight + 1);
}

// ─── shared ring-push helper ────────────────────────────────────────────────

void AdcSampler::pushFrameLocked(uint16_t ch0, uint16_t ch1,
                                  const std::chrono::steady_clock::time_point& sample_time,
                                  uint64_t read_us, uint64_t wait_us) {
    if (total_frames_ == 0) {
        first_sample_time_ = sample_time;
        last_sample_time_  = sample_time;
        rate_window_start_ = sample_time;
        rate_window_frames_ = 0;
        measured_sample_rate_hz_ = config_.sample_rate_hz;
        lifetime_sample_rate_hz_ = config_.sample_rate_hz;
    } else {
        last_sample_time_ = sample_time;
    }

    ring_[0][write_index_] = ch0;
    ring_[1][write_index_] = ch1;
    write_index_ = (write_index_ + 1) % ring_[0].size();
    valid_samples_ = std::min(valid_samples_ + 1, ring_[0].size());
    latest_raw_[0] = ch0;
    latest_raw_[1] = ch1;
    total_frames_++;

    avg_frame_read_us_ = ema(avg_frame_read_us_, read_us);
    max_frame_read_us_ = std::max(max_frame_read_us_, read_us);
    avg_mutex_wait_us_ = ema(avg_mutex_wait_us_, wait_us);
    max_mutex_wait_us_ = std::max(max_mutex_wait_us_, wait_us);

    if (total_frames_ > 1) {
        const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     last_sample_time_ - first_sample_time_).count();
        if (elapsed_ns > 0) {
            const uint64_t measured =
                ((total_frames_ - 1) * 1000000000ull + static_cast<uint64_t>(elapsed_ns / 2))
                / static_cast<uint64_t>(elapsed_ns);
            lifetime_sample_rate_hz_ = static_cast<uint32_t>(std::max<uint64_t>(1, measured));
        }
    }

    const auto win_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            sample_time - rate_window_start_).count();
    if (win_ns >= 1000000000ll) {
        const uint64_t frames  = total_frames_ - rate_window_frames_;
        const uint64_t recent  = (frames * 1000000000ull + static_cast<uint64_t>(win_ns / 2))
                                 / static_cast<uint64_t>(win_ns);
        measured_sample_rate_hz_ = static_cast<uint32_t>(std::max<uint64_t>(1, recent));
        rate_window_start_ = sample_time;
        rate_window_frames_ = total_frames_;
    }

    healthy_ = true;
    last_error_.clear();
}

// ─── MCP3202 spidev worker ──────────────────────────────────────────────────

void AdcSampler::configureWorkerScheduling() {
    std::ostringstream status;
    bool ok = true;

    if (config_.cpu_affinity >= 0) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(config_.cpu_affinity, &cpuset);
        int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
        if (rc == 0) status << "affinity=cpu" << config_.cpu_affinity << " ";
        else { ok = false; status << "affinity failed: " << std::strerror(rc) << " "; }
    }

    if (config_.realtime) {
        sched_param sp{};
        sp.sched_priority = std::max(1, std::min(99, config_.realtime_priority));
        int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
        if (rc == 0) { realtime_active_ = true;  status << "SCHED_FIFO priority " << sp.sched_priority; }
        else         { ok = false; realtime_active_ = false; status << "SCHED_FIFO failed: " << std::strerror(rc); }
    } else if (status.str().empty()) {
        status << "normal scheduler";
    }

    std::lock_guard<std::mutex> lock(mtx_);
    scheduler_status_ = status.str();
    if (!ok && !last_error_.empty()) scheduler_status_ += "; last_error=" + last_error_;
}

void AdcSampler::workerSpidev() {
    configureWorkerScheduling();

    const int64_t period_ns = static_cast<int64_t>(
        1000000000ull / std::max<uint32_t>(1, config_.sample_rate_hz));
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
            const uint64_t read_us = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    sample_time - read_start).count());

            const int64_t after_read_ns  = monotonicNs();
            const int64_t late_ns        = after_read_ns - next_sample_ns;
            if (late_ns > period_ns) {
                std::lock_guard<std::mutex> lock(mtx_);
                overruns_++;
                max_overrun_us_ = std::max<uint64_t>(max_overrun_us_,
                                                     static_cast<uint64_t>(late_ns / 1000));
            }

            const auto lock_wait_start = std::chrono::steady_clock::now();
            mtx_.lock();
            const auto lock_acquired = std::chrono::steady_clock::now();
            const uint64_t wait_us = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    lock_acquired - lock_wait_start).count());

            pushFrameLocked(raw[0], raw[1], sample_time, read_us, wait_us);

            const auto unlock_time = std::chrono::steady_clock::now();
            const uint64_t hold_us = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    unlock_time - lock_acquired).count());
            avg_mutex_hold_us_ = ema(avg_mutex_hold_us_, hold_us);
            max_mutex_hold_us_ = std::max(max_mutex_hold_us_, hold_us);
            mtx_.unlock();

            const int64_t done_ns = monotonicNs();
            if (done_ns - next_sample_ns > period_ns * 4) {
                const int64_t missed = (done_ns - next_sample_ns) / period_ns;
                next_sample_ns += missed * period_ns;
            }
        } catch (const std::exception& e) {
            {
                std::lock_guard<std::mutex> lock(mtx_);
                dropped_reads_++;
                healthy_    = false;
                last_error_ = e.what();
            }
            if (adc_) adc_->close();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            next_sample_ns = monotonicNs();
        }
    }
}

// ─── RP2040 USB CDC worker ──────────────────────────────────────────────────

void AdcSampler::workerRp2040() {
    const std::string device = config_.rp2040_dev;

    auto set_error = [&](const std::string& msg, bool connected = false) {
        std::lock_guard<std::mutex> lock(mtx_);
        healthy_          = false;
        rp2040_connected_ = connected;
        last_error_       = msg;
    };

    auto reconnect_with_error = [&](int fd, const std::string& msg) {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            healthy_          = false;
            rp2040_connected_ = false;
            if (rp2040_fd_ == fd) rp2040_fd_ = -1;
            if (!msg.empty()) last_error_ = msg;
        }
        ::close(fd);
    };

    while (running_) {
        int fd = ::open(device.c_str(), O_RDWR | O_NOCTTY | O_CLOEXEC | O_NONBLOCK);
        if (fd < 0) {
            set_error(std::string("open ") + device + ": " + std::strerror(errno));
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        if (!configureTtyRaw(fd)) {
            reconnect_with_error(fd, std::string("configure raw TTY ") + device + ": " + std::strerror(errno));
            if (running_) std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(mtx_);
            rp2040_fd_        = fd;
            rp2040_connected_ = true;
            healthy_          = false; // true after first valid packet
            last_error_.clear();
            if (rp2040_pending_rate_hz_ == 0) rp2040_pending_rate_hz_ = config_.sample_rate_hz;
            std::string error;
            if (rp2040_pending_rate_hz_ != 0 && sendRp2040RateCommandLocked(fd, rp2040_pending_rate_hz_, error)) {
                rp2040_pending_rate_hz_ = 0;
            } else if (!error.empty()) {
                last_error_ = error;
            }
        }

        std::string reconnect_reason;
        std::array<uint8_t, 4> window{};
        size_t window_fill = 0;

        while (running_) {
            // Sync to magic 'ADC2'. The port is non-blocking so stop() can exit promptly.
            while (running_) {
                uint8_t byte = 0;
                ssize_t rc = ::read(fd, &byte, 1);
                if (rc == 1) {
                    if (window_fill < window.size()) {
                        window[window_fill++] = byte;
                    } else {
                        window[0] = window[1];
                        window[1] = window[2];
                        window[2] = window[3];
                        window[3] = byte;
                    }
                    if (window_fill < window.size()) continue;
                    uint32_t magic = static_cast<uint32_t>(window[0])
                                   | (static_cast<uint32_t>(window[1]) << 8)
                                   | (static_cast<uint32_t>(window[2]) << 16)
                                   | (static_cast<uint32_t>(window[3]) << 24);
                    if (magic == RP2040_MAGIC) break;
                    continue;
                }
                if (rc == 0 || errno == EAGAIN || errno == EWOULDBLOCK) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    continue;
                }
                if (errno == EINTR) continue;
                reconnect_reason = std::string("RP2040 read while syncing: ") + std::strerror(errno);
                goto reconnect;
            }
            if (!running_) break;

            Rp2040Header hdr{};
            std::memcpy(&hdr, window.data(), 4);
            if (!readExact(fd, reinterpret_cast<uint8_t*>(&hdr) + 4, sizeof(hdr) - 4, running_)) {
                reconnect_reason = "RP2040 read failed while receiving packet header";
                goto reconnect;
            }

            if (hdr.magic != RP2040_MAGIC
             || hdr.version != RP2040_VERSION
             || hdr.header_bytes != sizeof(Rp2040Header)
             || hdr.frame_count == 0
             || hdr.frame_count > 4096) {
                std::ostringstream oss;
                oss << "RP2040 invalid header: magic=0x" << std::hex << hdr.magic << std::dec
                    << " version=" << hdr.version
                    << " header_bytes=" << hdr.header_bytes
                    << " frame_count=" << hdr.frame_count;
                {
                    std::lock_guard<std::mutex> lock(mtx_);
                    healthy_ = false;
                    last_error_ = oss.str();
                }
                window_fill = 0;
                continue;
            }

            std::vector<Rp2040Frame> frames(hdr.frame_count);
            if (!readExact(fd, reinterpret_cast<uint8_t*>(frames.data()), frames.size() * sizeof(Rp2040Frame), running_)) {
                reconnect_reason = "RP2040 read failed while receiving packet frames";
                goto reconnect;
            }

            uint32_t pkt_crc = 0;
            if (!readExact(fd, reinterpret_cast<uint8_t*>(&pkt_crc), sizeof(pkt_crc), running_)) {
                reconnect_reason = "RP2040 read failed while receiving packet CRC";
                goto reconnect;
            }

            uint32_t calc = 0;
            calc = crc32Update(calc, reinterpret_cast<const uint8_t*>(&hdr), sizeof(hdr));
            calc = crc32Update(calc, reinterpret_cast<const uint8_t*>(frames.data()), frames.size() * sizeof(Rp2040Frame));

            {
                std::lock_guard<std::mutex> lock(mtx_);
                if (calc != pkt_crc) {
                    rp2040_packets_crc_bad_++;
                    healthy_ = false;
                    std::ostringstream oss;
                    oss << "RP2040 packet CRC mismatch: expected 0x" << std::hex << pkt_crc
                        << " calculated 0x" << calc << std::dec
                        << " frames=" << hdr.frame_count;
                    last_error_ = oss.str();
                    window_fill = 0;
                    continue;
                }

                rp2040_packets_ok_++;
                rp2040_declared_rate_hz_       = hdr.sample_rate_hz;
                rp2040_firmware_flags_         = hdr.flags;
                rp2040_firmware_lost_frames_   = hdr.lost_frames;

                bool sequence_gap = false;
                uint32_t expected_seq = 0;
                if (rp2040_have_last_seq_ && hdr.sequence_start != rp2040_last_seq_ + 1u) {
                    sequence_gap = true;
                    expected_seq = rp2040_last_seq_ + 1u;
                    rp2040_sequence_gaps_++;
                }
                rp2040_have_last_seq_ = true;
                rp2040_last_seq_      = frames.back().seq;

                const auto now = std::chrono::steady_clock::now();
                for (const auto& f : frames) {
                    pushFrameLocked(f.ch0, f.ch1, now, 0, 0);
                }

                if (sequence_gap) {
                    std::ostringstream oss;
                    oss << "RP2040 sequence gap: expected " << expected_seq
                        << " got " << hdr.sequence_start;
                    if (hdr.lost_frames) oss << "; firmware lost_frames=" << hdr.lost_frames;
                    last_error_ = oss.str();
                } else if (hdr.flags != 0) {
                    std::ostringstream oss;
                    oss << "RP2040 firmware flags=0x" << std::hex << hdr.flags << std::dec
                        << " lost_frames=" << hdr.lost_frames;
                    last_error_ = oss.str();
                } else {
                    last_error_.clear();
                }
            }
            window_fill = 0;
        }

        reconnect:
        reconnect_with_error(fd, reconnect_reason);
        if (running_) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

// ─── ring helpers ───────────────────────────────────────────────────────────

std::vector<uint16_t> AdcSampler::copyRecentDecimatedLocked(int channel,
                                                              size_t max_points) const {
    const auto& ring  = ring_[channel];
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
    const auto& ring  = ring_[channel];
    const size_t count = std::min({frames, valid_samples_, ring.size()});
    if (count == 0) return {};

    std::vector<uint16_t> ordered;
    ordered.reserve(count);
    const size_t start = (write_index_ + ring.size() - count) % ring.size();
    for (size_t i = 0; i < count; ++i)
        ordered.push_back(ring[(start + i) % ring.size()]);
    snapshot_samples_copied_ += ordered.size();
    return ordered;
}

// ─── status / snapshot ──────────────────────────────────────────────────────

void AdcSampler::fillStatusLocked(AdcScopeData& data) const {
    data.enabled          = config_.enabled;
    data.running          = running_;
    data.healthy          = healthy_;
    data.bitbang          = config_.adc.bitbang;
    data.adc_source       = config_.adc_source;
    data.sample_rate_hz   = config_.sample_rate_hz;
    data.measured_sample_rate_hz =
        measured_sample_rate_hz_ ? measured_sample_rate_hz_ : config_.sample_rate_hz;
    data.lifetime_sample_rate_hz =
        lifetime_sample_rate_hz_ ? lifetime_sample_rate_hz_ : data.measured_sample_rate_hz;
    data.latest_raw   = latest_raw_;
    data.latest_volts = {{
        (static_cast<double>(latest_raw_[0]) * config_.vref) / 4095.0,
        (static_cast<double>(latest_raw_[1]) * config_.vref) / 4095.0
    }};
    data.total_frames          = total_frames_;
    data.dropped_reads         = dropped_reads_;
    data.overruns              = overruns_;
    data.max_overrun_us        = max_overrun_us_;
    data.avg_frame_read_us     = avg_frame_read_us_;
    data.max_frame_read_us     = max_frame_read_us_;
    data.avg_mutex_wait_us     = avg_mutex_wait_us_;
    data.max_mutex_wait_us     = max_mutex_wait_us_;
    data.avg_mutex_hold_us     = avg_mutex_hold_us_;
    data.max_mutex_hold_us     = max_mutex_hold_us_;
    data.snapshot_count        = snapshot_count_;
    data.snapshot_samples_copied = snapshot_samples_copied_;
    data.realtime_requested    = config_.realtime;
    data.realtime_active       = realtime_active_;
    data.realtime_priority     = config_.realtime_priority;
    data.cpu_affinity          = config_.cpu_affinity;
    data.scheduler_status      = scheduler_status_;
    // RP2040
    data.rp2040_dev                = config_.rp2040_dev;
    data.rp2040_connected          = rp2040_connected_;
    data.rp2040_packets_ok         = rp2040_packets_ok_;
    data.rp2040_packets_crc_bad    = rp2040_packets_crc_bad_;
    data.rp2040_sequence_gaps      = rp2040_sequence_gaps_;
    data.rp2040_firmware_lost_frames = rp2040_firmware_lost_frames_;
    data.rp2040_firmware_flags     = rp2040_firmware_flags_;
    data.rp2040_declared_rate_hz   = rp2040_declared_rate_hz_;
    data.valid_samples             = valid_samples_;
    data.history_capacity_samples  = ring_[0].size();
    data.last_error                = last_error_;
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
