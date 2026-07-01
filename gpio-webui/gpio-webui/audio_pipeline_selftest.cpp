#include "AudioProcessing.hpp"
#include "WavFile.hpp"
#include "libs/audio_filters/AudioFilters.hpp"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool ok, const std::string& msg) {
    if (!ok) throw std::runtime_error(msg);
}

std::string writeTempWav(const AudioBuffer& b) {
    AudioProcessOptions opts;
    opts.codec = "pcm16";
    std::string path = "/tmp/gpio_webui_audio_pipeline_selftest.wav";
    std::string wav = AudioProcessing::encodeWav(b, opts);
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) throw std::runtime_error("failed opening temp wav");
    out.write(wav.data(), static_cast<std::streamsize>(wav.size()));
    return path;
}
}

int main() {
    try {
        require(AudioFilters::rawAdcToPcm16(2048) == 0, "raw midpoint did not map to PCM zero");
        require(AudioFilters::pcm16ToRawAdc(0) == 2048, "PCM zero did not map to raw midpoint");
        require(AudioFilters::rawAdcToPcm16(0) <= -32760, "raw low rail did not map near PCM min");
        require(AudioFilters::rawAdcToPcm16(4095) >= 32750, "raw high rail did not map near PCM max");

        std::vector<uint16_t> raw = {2048, 2058, 2038, 4095, 0};
        auto mono = AudioFilters::rawAdcToMonoPcm16(raw, 8000);
        require(mono.channels == 1 && mono.sample_rate_hz == 8000 && mono.samples.size() == raw.size(), "mono raw->PCM shape mismatch");
        auto roundtrip = AudioFilters::monoPcm16ToRawAdc(mono);
        require(roundtrip.size() == raw.size(), "mono PCM->raw size mismatch");
        require(std::abs(static_cast<int>(roundtrip[0]) - 2048) <= 1, "roundtrip midpoint mismatch");

        AudioBuffer stereo;
        stereo.sample_rate_hz = 48000;
        stereo.channels = 2;
        stereo.samples = {0, 0, 1200, -1200, -1200, 1200, 0, 0};
        std::string path = writeTempWav(stereo);
        WavInfo info = WavFile::probe(path);
        require(info.sample_rate_hz == 48000, "WAV probe sample rate mismatch");
        require(info.channels == 2, "WAV probe channels mismatch");
        require(info.bits_per_sample == 16, "WAV probe bits mismatch");
        auto loaded = WavFile::loadPcm16(path);
        require(loaded.channels == stereo.channels && loaded.sample_rate_hz == stereo.sample_rate_hz, "WAV load metadata mismatch");
        require(loaded.samples == stereo.samples, "WAV loaded samples differ");
        std::remove(path.c_str());

        auto before = mono.samples;
        AudioFilters::applyEffects(mono, {});
        require(mono.samples == before, "empty filter chain changed samples");
        require(AudioFilters::isKnownEffect("dc_block"), "dc_block not known");
        require(!AudioFilters::isKnownEffect("definitely_not_a_filter"), "invalid filter reported known");

        std::cout << "Audio pipeline self-test: PASS\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Audio pipeline self-test: FAIL: " << e.what() << "\n";
        return 1;
    }
}
