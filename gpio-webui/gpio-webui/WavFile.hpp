#pragma once

#include "AudioProcessing.hpp"
#include <cstdint>
#include <string>

struct WavInfo {
    uint32_t sample_rate_hz = 0;
    uint16_t channels = 0;
    uint16_t bits_per_sample = 0;
    uint32_t duration_ms = 0;
    uint64_t data_bytes = 0;
    uint64_t file_bytes = 0;
};

class WavFile {
public:
    static WavInfo probe(const std::string& path);
    static AudioBuffer loadPcm16(const std::string& path, size_t max_bytes = 50ull * 1024ull * 1024ull);
};
