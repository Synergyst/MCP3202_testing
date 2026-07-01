#include "AudioProcessing.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <sstream>
#include <stdexcept>

namespace {

std::string trim(const std::string& s) {
    const auto begin = std::find_if_not(s.begin(), s.end(), [](unsigned char c) { return std::isspace(c); });
    const auto end = std::find_if_not(s.rbegin(), s.rend(), [](unsigned char c) { return std::isspace(c); }).base();
    if (begin >= end) return "";
    return std::string(begin, end);
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

void append_u16(std::string& s, uint16_t v) {
    s.push_back(static_cast<char>(v & 0xff));
    s.push_back(static_cast<char>((v >> 8) & 0xff));
}

void append_u32(std::string& s, uint32_t v) {
    s.push_back(static_cast<char>(v & 0xff));
    s.push_back(static_cast<char>((v >> 8) & 0xff));
    s.push_back(static_cast<char>((v >> 16) & 0xff));
    s.push_back(static_cast<char>((v >> 24) & 0xff));
}

std::string encodePcmWav(const AudioBuffer& b) {
    const uint16_t bits_per_sample = 16;
    const uint16_t block_align = b.channels * (bits_per_sample / 8);
    const uint32_t byte_rate = b.sample_rate_hz * block_align;
    const uint32_t data_bytes = static_cast<uint32_t>(b.samples.size() * sizeof(int16_t));
    const uint32_t riff_size = 36 + data_bytes;

    std::string wav;
    wav.reserve(44 + data_bytes);
    wav.append("RIFF", 4);
    append_u32(wav, riff_size);
    wav.append("WAVE", 4);
    wav.append("fmt ", 4);
    append_u32(wav, 16);
    append_u16(wav, 1); // PCM
    append_u16(wav, b.channels);
    append_u32(wav, b.sample_rate_hz);
    append_u32(wav, byte_rate);
    append_u16(wav, block_align);
    append_u16(wav, bits_per_sample);
    wav.append("data", 4);
    append_u32(wav, data_bytes);
    for (int16_t sample : b.samples) append_u16(wav, static_cast<uint16_t>(sample));
    return wav;
}

} // namespace

namespace AudioProcessing {

nlohmann::json catalog() {
    return {
        {"effects", nlohmann::json::array({
            {{"id", "dc_block"}, {"label", "DC block / center"}, {"available", AudioFilters::isRuntimeAvailable("dc_block")}, {"description", "Removes per-channel DC offset from the exported buffer."}},
            {{"id", "pots_bandpass"}, {"label", "POTS voice band"}, {"available", AudioFilters::isRuntimeAvailable("pots_bandpass")}, {"description", "Simple dependency-free 300 Hz high-pass plus ~3.4 kHz low-pass."}},
            {{"id", "hum_notch_60"}, {"label", "60 Hz hum notch"}, {"available", AudioFilters::isRuntimeAvailable("hum_notch_60")}, {"description", "Narrow notch to reduce mains hum."}},
            {{"id", "hum_notch_120"}, {"label", "120 Hz hum notch"}, {"available", AudioFilters::isRuntimeAvailable("hum_notch_120")}, {"description", "Narrow notch to reduce second-harmonic mains hum."}},
            {{"id", "voice_agc"}, {"label", "Voice AGC"}, {"available", AudioFilters::isRuntimeAvailable("voice_agc")}, {"description", "Simple automatic gain control for speech/listening previews."}},
            {{"id", "bell202_bandpass"}, {"label", "Bell 202 FSK band"}, {"available", AudioFilters::isRuntimeAvailable("bell202_bandpass")}, {"description", "FSK-focused broad bandpass around Caller ID/Bell 202 tones. Good first-stage cleanup."}},
            {{"id", "bell202_dual_tone"}, {"label", "Bell 202 dual-tone isolate"}, {"available", AudioFilters::isRuntimeAvailable("bell202_dual_tone")}, {"description", "Narrowly isolates 1200 Hz and 2200 Hz and rejects in-band static/interference between the tones."}},
            {{"id", "fsk_squelch"}, {"label", "FSK static squelch"}, {"available", AudioFilters::isRuntimeAvailable("fsk_squelch")}, {"description", "Soft-gates low-level line static using a short-window noise floor estimate. Best after Bell 202 bandpass."}},
            {{"id", "normalize"}, {"label", "Normalize"}, {"available", AudioFilters::isRuntimeAvailable("normalize")}, {"description", "Raises gain so the peak reaches about -0.8 dBFS."}},
            {{"id", "soft_clip"}, {"label", "Soft clip / limiter"}, {"available", AudioFilters::isRuntimeAvailable("soft_clip")}, {"description", "Gentle tanh limiter for hot analog peaks."}},
            {{"id", "rnnoise"}, {"label", "RNNoise denoise"}, {"available", AudioFilters::isRuntimeAvailable("rnnoise")}, {"description", AudioFilters::isRuntimeAvailable("rnnoise") ? "RNNoise neural speech denoiser. Audio is internally resampled to 48 kHz / 480-sample frames for processing." : "RNNoise denoise. Not linked in this build."}}
        })},
        {"codecs", nlohmann::json::array({
            {{"id", "pcm16"}, {"label", "WAV PCM 16-bit"}, {"container", "wav"}, {"available", true}, {"description", "Uncompressed 16-bit PCM WAV."}},
            {{"id", "mulaw"}, {"label", "G.711 µ-law"}, {"container", "wav"}, {"available", false}, {"description", "Reserved for POTS-oriented compressed exports. Not linked in this build."}},
            {{"id", "alaw"}, {"label", "G.711 A-law"}, {"container", "wav"}, {"available", false}, {"description", "Reserved for telephony compressed exports. Not linked in this build."}},
            {{"id", "opus"}, {"label", "Opus"}, {"container", "ogg/opus"}, {"available", false}, {"description", "Reserved optional module hook. Not linked in this build."}},
            {{"id", "flac"}, {"label", "FLAC"}, {"container", "flac"}, {"available", false}, {"description", "Reserved optional module hook. Not linked in this build."}}
        })}
    };
}

AudioProcessOptions parseOptions(const std::string& effects_csv, const std::string& codec) {
    AudioProcessOptions opts;
    opts.codec = lower(trim(codec.empty() ? "pcm16" : codec));
    if (opts.codec != "pcm16") {
        throw std::runtime_error("Codec '" + opts.codec + "' is not available in this build; use pcm16");
    }

    std::stringstream ss(effects_csv);
    std::string item;
    while (std::getline(ss, item, ',')) {
        item = lower(trim(item));
        if (item.empty() || item == "none") continue;
        if (!AudioFilters::isKnownEffect(item)) {
            throw std::runtime_error("Unknown audio effect: " + item);
        }
        if (!AudioFilters::isRuntimeAvailable(item)) {
            throw std::runtime_error(AudioFilters::unavailableReason(item));
        }
        if (std::find(opts.effects.begin(), opts.effects.end(), item) == opts.effects.end()) opts.effects.push_back(item);
    }
    return opts;
}

std::string sanitizeForFilename(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') out.push_back(c);
    }
    return out.empty() ? "none" : out;
}

void applyEffects(AudioBuffer& buffer, const AudioProcessOptions& options) {
    AudioFilters::applyEffects(buffer, options.effects);
}

std::string encodeWav(const AudioBuffer& buffer, const AudioProcessOptions& options) {
    if (options.codec != "pcm16") {
        throw std::runtime_error("Codec '" + options.codec + "' is not available in this build; use pcm16");
    }
    return encodePcmWav(buffer);
}

} // namespace AudioProcessing
