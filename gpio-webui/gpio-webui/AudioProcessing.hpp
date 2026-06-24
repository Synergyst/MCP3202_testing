#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

struct AudioBuffer {
    uint32_t sample_rate_hz = 8000;
    uint16_t channels = 1;
    std::vector<int16_t> samples; // interleaved signed 16-bit PCM
};

struct AudioProcessOptions {
    std::vector<std::string> effects;
    std::string codec = "pcm16";
};

namespace AudioProcessing {
    nlohmann::json catalog();
    AudioProcessOptions parseOptions(const std::string& effects_csv, const std::string& codec);
    std::string sanitizeForFilename(const std::string& s);
    void applyEffects(AudioBuffer& buffer, const AudioProcessOptions& options);
    std::string encodeWav(const AudioBuffer& buffer, const AudioProcessOptions& options);
}
