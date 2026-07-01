#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct AudioBuffer {
    uint32_t sample_rate_hz = 8000;
    uint16_t channels = 1;
    std::vector<int16_t> samples; // interleaved signed 16-bit PCM
};

namespace AudioFilters {

class AudioFilter {
public:
    virtual ~AudioFilter() = default;
    virtual const char* id() const = 0;
    virtual void process(AudioBuffer& buffer) = 0;
};

class FilterChain {
public:
    FilterChain() = default;
    explicit FilterChain(const std::vector<std::string>& effect_ids);

    void add(std::unique_ptr<AudioFilter> filter);
    void addById(const std::string& effect_id);
    void clear();
    bool empty() const;
    void reset(const std::vector<std::string>& effect_ids);
    void process(AudioBuffer& buffer);

private:
    std::vector<std::unique_ptr<AudioFilter>> filters_;
};

bool isKnownEffect(const std::string& effect_id);
bool isRuntimeAvailable(const std::string& effect_id);
std::string unavailableReason(const std::string& effect_id);
std::unique_ptr<AudioFilter> createFilter(const std::string& effect_id);
void applyEffects(AudioBuffer& buffer, const std::vector<std::string>& effect_ids);

int16_t rawAdcToPcm16(uint16_t raw);
uint16_t pcm16ToRawAdc(int16_t sample);
AudioBuffer rawAdcToMonoPcm16(const std::vector<uint16_t>& raw, uint32_t sample_rate_hz);
AudioBuffer rawAdcToStereoPcm16(const std::vector<uint16_t>& ch0, const std::vector<uint16_t>& ch1, uint32_t sample_rate_hz);
std::vector<uint16_t> monoPcm16ToRawAdc(const AudioBuffer& buffer);
void stereoPcm16ToRawAdc(const AudioBuffer& buffer, std::vector<uint16_t>& ch0, std::vector<uint16_t>& ch1);
AudioBuffer floatMonoToPcm16(const std::vector<float>& samples, uint32_t sample_rate_hz);
std::vector<float> monoPcm16ToFloat(const AudioBuffer& buffer);
void applyEffectsToRawAdcMono(std::vector<uint16_t>& raw, uint32_t sample_rate_hz, const std::vector<std::string>& effect_ids);
void applyEffectsToFloatMono(std::vector<float>& samples, uint32_t sample_rate_hz, const std::vector<std::string>& effect_ids);
void applyEffectsToStereoChannels(std::vector<uint16_t>& ch0, std::vector<uint16_t>& ch1, uint32_t sample_rate_hz,
                                  const std::vector<std::string>& effects_ch0, const std::vector<std::string>& effects_ch1);

} // namespace AudioFilters
