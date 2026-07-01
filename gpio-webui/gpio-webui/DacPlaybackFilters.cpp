#include "DacPlaybackFilters.hpp"
#include "libs/audio_filters/AudioFilters.hpp"

#include <algorithm>

namespace {

AudioBuffer extractMono(const AudioBuffer& in, uint16_t channel) {
    AudioBuffer out;
    out.sample_rate_hz = in.sample_rate_hz;
    out.channels = 1;
    if (in.channels == 0) return out;
    const size_t frames = in.samples.size() / in.channels;
    out.samples.reserve(frames);
    const uint16_t ch = std::min<uint16_t>(channel, in.channels - 1);
    for (size_t i = 0; i < frames; ++i) out.samples.push_back(in.samples[i * in.channels + ch]);
    return out;
}

void writeMono(AudioBuffer& out, const AudioBuffer& mono, uint16_t channel) {
    if (out.channels == 0 || mono.channels == 0) return;
    const size_t frames = std::min(out.samples.size() / out.channels, mono.samples.size() / mono.channels);
    const uint16_t ch = std::min<uint16_t>(channel, out.channels - 1);
    for (size_t i = 0; i < frames; ++i) out.samples[i * out.channels + ch] = mono.samples[i * mono.channels];
}

} // namespace

namespace DacPlaybackFilters {

void applyPlaybackProfiles(AudioBuffer& buffer, const std::string& mode, FilterProfileManager* profiles) {
    if (!profiles || buffer.samples.empty() || buffer.channels == 0) return;
    if (buffer.channels == 1) {
        std::string ctx = (mode == "ch1" || mode == "mono-ch1") ? "dac.playback.ch1" : "dac.playback.ch0";
        auto effects = profiles->effectiveEffects(ctx);
        if (!effects.empty()) AudioFilters::applyEffects(buffer, effects);
        return;
    }

    auto stereo = profiles->effectiveEffects("dac.playback.stereo");
    if (!stereo.empty()) AudioFilters::applyEffects(buffer, stereo);

    auto left = extractMono(buffer, 0);
    auto right = extractMono(buffer, 1);
    auto e0 = profiles->effectiveEffects("dac.playback.ch0");
    auto e1 = profiles->effectiveEffects("dac.playback.ch1");
    if (!e0.empty()) AudioFilters::applyEffects(left, e0);
    if (!e1.empty()) AudioFilters::applyEffects(right, e1);
    writeMono(buffer, left, 0);
    writeMono(buffer, right, 1);
}

} // namespace DacPlaybackFilters
