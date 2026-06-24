#include "AudioProcessing.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <memory>
#include <sstream>
#include <stdexcept>

#ifdef HAVE_RNNOISE
#include <rnnoise.h>
#endif

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

int16_t clamp16(int32_t v) {
    return static_cast<int16_t>(std::max<int32_t>(-32768, std::min<int32_t>(32767, v)));
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

void effectNormalize(AudioBuffer& b) {
    int32_t peak = 0;
    for (int16_t s : b.samples) peak = std::max<int32_t>(peak, std::abs(static_cast<int32_t>(s)));
    if (peak <= 0) return;
    const double target = 30000.0;
    const double gain = target / static_cast<double>(peak);
    if (gain <= 1.01) return;
    for (auto& s : b.samples) s = clamp16(static_cast<int32_t>(std::lrint(static_cast<double>(s) * gain)));
}

void effectSoftClip(AudioBuffer& b) {
    // Gentle tanh soft limiter. Useful for telephone recordings with occasional hot peaks.
    constexpr double drive = 1.6;
    const double denom = std::tanh(drive);
    for (auto& s : b.samples) {
        const double x = static_cast<double>(s) / 32768.0;
        const double y = std::tanh(x * drive) / denom;
        s = clamp16(static_cast<int32_t>(std::lrint(y * 32767.0)));
    }
}

void effectDcBlock(AudioBuffer& b) {
    if (b.samples.empty() || b.channels == 0) return;
    const uint16_t chs = b.channels;
    std::vector<double> mean(chs, 0.0);
    std::vector<size_t> count(chs, 0);
    for (size_t i = 0; i < b.samples.size(); ++i) {
        const size_t ch = i % chs;
        mean[ch] += b.samples[i];
        count[ch]++;
    }
    for (size_t ch = 0; ch < chs; ++ch) {
        if (count[ch]) mean[ch] /= static_cast<double>(count[ch]);
    }
    for (size_t i = 0; i < b.samples.size(); ++i) {
        b.samples[i] = clamp16(static_cast<int32_t>(std::lrint(static_cast<double>(b.samples[i]) - mean[i % chs])));
    }
}

void effectRnNoise(AudioBuffer& b) {
#ifndef HAVE_RNNOISE
    (void)b;
    throw std::runtime_error("RNNoise is not linked in this build");
#else
    if (b.samples.empty() || b.channels == 0 || b.sample_rate_hz == 0) return;

    const int rn_frame = rnnoise_get_frame_size(); // normally 480 samples at 48 kHz = 10 ms
    if (rn_frame <= 0) throw std::runtime_error("RNNoise reported invalid frame size");
    constexpr uint32_t rn_rate = 48000;
    const uint16_t chs = b.channels;
    const size_t input_frames = b.samples.size() / chs;
    if (input_frames == 0) return;

    // RNNoise is trained and specified around 48 kHz / 480-sample frames. Resample each channel
    // to 48 kHz with linear interpolation, process complete padded RNNoise frames, then resample
    // back to the capture rate. This keeps the WAV export sample rate unchanged.
    const size_t rn_frames_count = std::max<size_t>(1, (static_cast<uint64_t>(input_frames) * rn_rate + b.sample_rate_hz - 1) / b.sample_rate_hz);
    std::vector<std::vector<float>> channel_in(chs, std::vector<float>(rn_frames_count, 0.0f));
    std::vector<std::vector<float>> channel_out(chs, std::vector<float>(rn_frames_count, 0.0f));

    for (uint16_t ch = 0; ch < chs; ++ch) {
        for (size_t i = 0; i < rn_frames_count; ++i) {
            const double src_pos = static_cast<double>(i) * static_cast<double>(b.sample_rate_hz) / static_cast<double>(rn_rate);
            const size_t i0 = std::min<size_t>(static_cast<size_t>(src_pos), input_frames - 1);
            const size_t i1 = std::min<size_t>(i0 + 1, input_frames - 1);
            const double frac = src_pos - static_cast<double>(i0);
            const double s0 = static_cast<double>(b.samples[i0 * chs + ch]);
            const double s1 = static_cast<double>(b.samples[i1 * chs + ch]);
            channel_in[ch][i] = static_cast<float>(s0 + (s1 - s0) * frac);
        }
    }

    const size_t padded = ((rn_frames_count + static_cast<size_t>(rn_frame) - 1) / static_cast<size_t>(rn_frame)) * static_cast<size_t>(rn_frame);
    std::vector<float> frame_in(padded, 0.0f), frame_out(padded, 0.0f);

    for (uint16_t ch = 0; ch < chs; ++ch) {
        std::unique_ptr<DenoiseState, decltype(&rnnoise_destroy)> st(rnnoise_create(nullptr), rnnoise_destroy);
        if (!st) throw std::runtime_error("rnnoise_create failed");

        std::fill(frame_in.begin(), frame_in.end(), 0.0f);
        std::copy(channel_in[ch].begin(), channel_in[ch].end(), frame_in.begin());
        for (size_t offset = 0; offset < padded; offset += static_cast<size_t>(rn_frame)) {
            rnnoise_process_frame(st.get(), frame_out.data() + offset, frame_in.data() + offset);
        }
        std::copy(frame_out.begin(), frame_out.begin() + static_cast<std::ptrdiff_t>(rn_frames_count), channel_out[ch].begin());
    }

    for (size_t i = 0; i < input_frames; ++i) {
        const double src_pos = static_cast<double>(i) * static_cast<double>(rn_rate) / static_cast<double>(b.sample_rate_hz);
        const size_t i0 = std::min<size_t>(static_cast<size_t>(src_pos), rn_frames_count - 1);
        const size_t i1 = std::min<size_t>(i0 + 1, rn_frames_count - 1);
        const double frac = src_pos - static_cast<double>(i0);
        for (uint16_t ch = 0; ch < chs; ++ch) {
            const double s0 = static_cast<double>(channel_out[ch][i0]);
            const double s1 = static_cast<double>(channel_out[ch][i1]);
            b.samples[i * chs + ch] = clamp16(static_cast<int32_t>(std::lrint(s0 + (s1 - s0) * frac)));
        }
    }
#endif
}

void effectPotsBandpass(AudioBuffer& b) {
    // Lightweight first-order voice band approximation for POTS captures:
    // high-pass near 300 Hz + low-pass near 3400 Hz. This is intentionally simple
    // and dependency-free; future modules can replace it with libsamplerate/sox/ffmpeg filters.
    if (b.samples.empty() || b.channels == 0 || b.sample_rate_hz == 0) return;
    const double fs = static_cast<double>(b.sample_rate_hz);
    const double hp_cut = 300.0;
    const double lp_cut = std::min(3400.0, fs * 0.45);
    if (lp_cut <= 0.0) return;

    const double hp_rc = 1.0 / (2.0 * M_PI * hp_cut);
    const double dt = 1.0 / fs;
    const double hp_alpha = hp_rc / (hp_rc + dt);
    const double lp_rc = 1.0 / (2.0 * M_PI * lp_cut);
    const double lp_alpha = dt / (lp_rc + dt);

    const uint16_t chs = b.channels;
    std::vector<double> hp_y(chs, 0.0), hp_prev_x(chs, 0.0), lp_y(chs, 0.0);
    for (size_t i = 0; i < b.samples.size(); ++i) {
        const size_t ch = i % chs;
        const double x = static_cast<double>(b.samples[i]);
        const double yhp = hp_alpha * (hp_y[ch] + x - hp_prev_x[ch]);
        hp_y[ch] = yhp;
        hp_prev_x[ch] = x;
        lp_y[ch] = lp_y[ch] + lp_alpha * (yhp - lp_y[ch]);
        b.samples[i] = clamp16(static_cast<int32_t>(std::lrint(lp_y[ch])));
    }
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
}

namespace AudioProcessing {

nlohmann::json catalog() {
    return {
        {"effects", nlohmann::json::array({
            {{"id", "dc_block"}, {"label", "DC block / center"}, {"available", true}, {"description", "Removes per-channel DC offset from the exported buffer."}},
            {{"id", "pots_bandpass"}, {"label", "POTS voice band"}, {"available", true}, {"description", "Simple dependency-free 300 Hz high-pass plus ~3.4 kHz low-pass."}},
            {{"id", "normalize"}, {"label", "Normalize"}, {"available", true}, {"description", "Raises gain so the peak reaches about -0.8 dBFS."}},
            {{"id", "soft_clip"}, {"label", "Soft clip / limiter"}, {"available", true}, {"description", "Gentle tanh limiter for hot analog peaks."}},
#ifdef HAVE_RNNOISE
            {{"id", "rnnoise"}, {"label", "RNNoise denoise"}, {"available", true}, {"description", "RNNoise neural speech denoiser. Audio is internally resampled to 48 kHz / 480-sample frames for processing."}}
#else
            {{"id", "rnnoise"}, {"label", "RNNoise denoise"}, {"available", false}, {"description", "RNNoise denoise. Not linked in this build."}}
#endif
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
        if (item != "dc_block" && item != "pots_bandpass" && item != "normalize" && item != "soft_clip" && item != "rnnoise") {
            throw std::runtime_error("Unknown or unavailable audio effect: " + item);
        }
#ifndef HAVE_RNNOISE
        if (item == "rnnoise") throw std::runtime_error("RNNoise is not linked in this build");
#endif
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
    for (const auto& effect : options.effects) {
        if (effect == "dc_block") effectDcBlock(buffer);
        else if (effect == "pots_bandpass") effectPotsBandpass(buffer);
        else if (effect == "normalize") effectNormalize(buffer);
        else if (effect == "soft_clip") effectSoftClip(buffer);
        else if (effect == "rnnoise") effectRnNoise(buffer);
        else throw std::runtime_error("Unavailable audio effect: " + effect);
    }
}

std::string encodeWav(const AudioBuffer& buffer, const AudioProcessOptions& options) {
    if (options.codec != "pcm16") {
        throw std::runtime_error("Codec '" + options.codec + "' is not available in this build; use pcm16");
    }
    return encodePcmWav(buffer);
}

}
