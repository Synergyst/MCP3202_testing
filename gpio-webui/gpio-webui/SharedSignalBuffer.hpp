#pragma once

#include <vector>
#include <mutex>
#include <memory>
#include <algorithm>

/**
 * SharedSignalBuffer provides a thread-safe, circular buffer of normalized
 * audio samples. It is designed for a single producer (AdcSampler) and
 * multiple consumers (FSK, DTMF, LineState, etc.).
 */
class SharedSignalBuffer {
public:
    explicit SharedSignalBuffer(size_t capacity = 8000 * 10) // Default 10 seconds at 8kHz
        : buffer_(capacity), capacity_(capacity) {}

    /**
     * Push a new normalized sample into the buffer.
     * @param sample Normalized sample in range [-1.0, 1.0].
     */
    void push(float sample) {
        std::lock_guard<std::mutex> lock(mtx_);
        buffer_[write_idx_] = sample;
        write_idx_ = (write_idx_ + 1) % capacity_;
        if (valid_samples_ < capacity_) {
            valid_samples_++;
        }
    }

    /**
     * Retrieve the most recent N samples.
     * If fewer than N samples are available, it returns all available samples.
     * @param length Number of samples to retrieve.
     * @return A vector containing the requested samples in chronological order.
     */
    std::vector<float> getLatestWindow(size_t length) const {
        std::lock_guard<std::mutex> lock(mtx_);
        
        size_t available = valid_samples_;
        size_t to_copy = std::min(length, available);
        std::vector<float> result;
        result.reserve(to_copy);

        if (to_copy == 0) return result;

        // Calculate the start index
        // write_idx_ is the position for the NEXT write.
        // The most recent sample is at (write_idx_ - 1 + capacity_) % capacity_
        size_t read_idx = (write_idx_ + capacity_ - to_copy) % capacity_;

        for (size_t i = 0; i < to_copy; ++i) {
            result.push_back(buffer_[(read_idx + i) % capacity_]);
        }

        return result;
    }

    size_t getCapacity() const { return capacity_; }
    size_t getValidSamples() const { 
        std::lock_guard<std::mutex> lock(mtx_); 
        return valid_samples_; 
    }

private:
    mutable std::mutex mtx_;
    std::vector<float> buffer_;
    size_t capacity_;
    size_t write_idx_ = 0;
    size_t valid_samples_ = 0;
};
