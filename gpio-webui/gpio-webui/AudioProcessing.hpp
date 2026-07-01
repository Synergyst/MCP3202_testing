#pragma once

#include "libs/audio_filters/AudioFilters.hpp"

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

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
