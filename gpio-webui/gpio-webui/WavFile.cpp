#include "WavFile.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace {
uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8)); }
uint32_t rd32(const uint8_t* p) { return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24)); }

struct ParsedWav {
    WavInfo info;
    uint64_t data_offset = 0;
};

ParsedWav parseHeader(std::ifstream& f) {
    f.seekg(0, std::ios::end);
    const uint64_t file_size = static_cast<uint64_t>(f.tellg());
    f.seekg(0, std::ios::beg);
    if (file_size < 44) throw std::runtime_error("WAV file is too small");
    uint8_t hdr[12];
    f.read(reinterpret_cast<char*>(hdr), sizeof(hdr));
    if (!f || std::memcmp(hdr, "RIFF", 4) != 0 || std::memcmp(hdr + 8, "WAVE", 4) != 0) throw std::runtime_error("Not a RIFF/WAVE file");

    bool have_fmt = false, have_data = false;
    uint16_t audio_format = 0, channels = 0, bits = 0;
    uint32_t sample_rate = 0;
    uint64_t data_offset = 0, data_bytes = 0;

    while (f && static_cast<uint64_t>(f.tellg()) + 8 <= file_size) {
        uint8_t chdr[8];
        f.read(reinterpret_cast<char*>(chdr), sizeof(chdr));
        if (!f) break;
        const uint32_t chunk_size = rd32(chdr + 4);
        const uint64_t chunk_start = static_cast<uint64_t>(f.tellg());
        if (chunk_start + chunk_size > file_size) throw std::runtime_error("Malformed WAV chunk size");
        if (std::memcmp(chdr, "fmt ", 4) == 0) {
            if (chunk_size < 16) throw std::runtime_error("WAV fmt chunk too small");
            std::vector<uint8_t> fmt(chunk_size);
            f.read(reinterpret_cast<char*>(fmt.data()), chunk_size);
            audio_format = rd16(fmt.data());
            channels = rd16(fmt.data() + 2);
            sample_rate = rd32(fmt.data() + 4);
            bits = rd16(fmt.data() + 14);
            have_fmt = true;
        } else if (std::memcmp(chdr, "data", 4) == 0) {
            data_offset = chunk_start;
            data_bytes = chunk_size;
            have_data = true;
            f.seekg(static_cast<std::streamoff>(chunk_size), std::ios::cur);
        } else {
            f.seekg(static_cast<std::streamoff>(chunk_size), std::ios::cur);
        }
        if (chunk_size & 1u) f.seekg(1, std::ios::cur);
    }
    if (!have_fmt) throw std::runtime_error("WAV fmt chunk not found");
    if (!have_data) throw std::runtime_error("WAV data chunk not found");
    if (audio_format != 1) throw std::runtime_error("Only PCM WAV files are supported");
    if (channels < 1 || channels > 2) throw std::runtime_error("Only mono/stereo WAV files are supported");
    if (bits != 16) throw std::runtime_error("Only 16-bit PCM WAV files are supported");
    if (sample_rate == 0) throw std::runtime_error("Invalid WAV sample rate");
    const uint64_t bytes_per_frame = static_cast<uint64_t>(channels) * (bits / 8u);
    if (bytes_per_frame == 0 || data_bytes < bytes_per_frame) throw std::runtime_error("WAV data chunk is empty");
    ParsedWav p;
    p.info.sample_rate_hz = sample_rate;
    p.info.channels = channels;
    p.info.bits_per_sample = bits;
    p.info.data_bytes = data_bytes;
    p.info.file_bytes = file_size;
    p.info.duration_ms = static_cast<uint32_t>((data_bytes / bytes_per_frame) * 1000ull / sample_rate);
    p.data_offset = data_offset;
    return p;
}
}

WavInfo WavFile::probe(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) throw std::runtime_error("Unable to open WAV file");
    return parseHeader(f).info;
}

AudioBuffer WavFile::loadPcm16(const std::string& path, size_t max_bytes) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) throw std::runtime_error("Unable to open WAV file");
    ParsedWav p = parseHeader(f);
    if (p.info.data_bytes > max_bytes) throw std::runtime_error("WAV file exceeds configured size limit");
    const uint64_t samples_count = p.info.data_bytes / sizeof(int16_t);
    AudioBuffer b;
    b.sample_rate_hz = p.info.sample_rate_hz;
    b.channels = p.info.channels;
    b.samples.resize(static_cast<size_t>(samples_count));
    f.clear();
    f.seekg(static_cast<std::streamoff>(p.data_offset), std::ios::beg);
    f.read(reinterpret_cast<char*>(b.samples.data()), static_cast<std::streamsize>(samples_count * sizeof(int16_t)));
    if (!f) throw std::runtime_error("Failed reading WAV data");
    return b;
}
