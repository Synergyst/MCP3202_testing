#include "DacOutput.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <unistd.h>

namespace {
uint16_t clampRaw12(uint16_t v) { return static_cast<uint16_t>(v & 0x0FFFu); }

uint16_t voltsToRawFor(const MCP4922::ChannelConfig& cfg, double volts) {
    const double gain = cfg.gain_1x ? 1.0 : 2.0;
    const double full_scale = std::max(1e-12, cfg.vref * gain);
    const double clamped = std::max(0.0, std::min(volts, full_scale * 4095.0 / 4096.0));
    return static_cast<uint16_t>(std::lround((clamped * 4096.0) / full_scale)) & 0x0FFFu;
}

double rawToVoltsFor(const MCP4922::ChannelConfig& cfg, uint16_t raw) {
    const double gain = cfg.gain_1x ? 1.0 : 2.0;
    return (static_cast<double>(raw & 0x0FFFu) * cfg.vref * gain) / 4096.0;
}
}

DacOutput::DacOutput(Config config, AdcSampler* adc_sampler)
    : config_(std::move(config)), adc_sampler_(adc_sampler) {
    config_.native.enabled = config_.enabled && config_.transport == "native";
    native_ = std::make_unique<MCP4922>(config_.native);
}

DacOutput::~DacOutput() {
    std::string ignored;
    stop(ignored);
    if (native_) native_->close();
    if (direct_mcu_) direct_mcu_->close();
}

DacOutput::Config DacOutput::config() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return config_;
}

void DacOutput::updateConfig(Config config) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (native_) native_->close();
    if (direct_mcu_) direct_mcu_->close();
    direct_mcu_.reset();
    active_ = false;
    config_ = std::move(config);
    config_.native.enabled = config_.enabled && config_.transport == "native";
    native_ = std::make_unique<MCP4922>(config_.native);
    last_error_.clear();
}

DacOutput::Status DacOutput::status() const {
    std::lock_guard<std::mutex> lock(mtx_);
    Status s;
    s.enabled = config_.enabled;
    s.active = active_;
    s.transport = config_.transport;
    s.rp2040_dev = config_.rp2040_dev;
    s.sample_rate_hz = config_.sample_rate_hz;
    s.channel_count = config_.channel_count;
    s.sample_format = gw::sampleFormatName(config_.sample_format);
    s.raw_a = raw_a_;
    s.raw_b = raw_b_;
    s.volts_a = rawToVoltsA(raw_a_);
    s.volts_b = rawToVoltsB(raw_b_);
    s.frames_written = frames_written_;
    s.packets_sent = packets_sent_;
    s.errors = errors_;
    s.last_error = last_error_;
    s.native_open = native_ && native_->isOpen();
    s.mcu_connected = direct_mcu_ && direct_mcu_->isOpen();
    s.healthy = s.enabled && last_error_.empty() && (config_.transport == "native" ? true : (adc_sampler_ != nullptr || s.mcu_connected));
    return s;
}

bool DacOutput::start(std::string& error) {
    if (config_.enabled && config_.transport == "rp2040" && adc_sampler_) {
        if (!adc_sampler_->waitForGwpProtocol(1500, error)) {
            std::lock_guard<std::mutex> lock(mtx_);
            noteErrorLocked(error);
            return false;
        }
    }
    std::lock_guard<std::mutex> lock(mtx_);
    if (!config_.enabled) { error = "DAC is disabled"; noteErrorLocked(error); return false; }
    if (config_.transport == "native") {
        active_ = true;
        last_error_.clear();
        return true;
    }
    gw_format_payload_t f{};
    f.channel_count = config_.channel_count;
    f.sample_format = config_.sample_format;
    if (!sendMcuControl(GW_OP_DAC_SET_FORMAT, &f, sizeof(f), error)) { noteErrorLocked(error); return false; }
    gw_rate_payload_t r{};
    r.sample_rate_hz = config_.sample_rate_hz;
    if (!sendMcuControl(GW_OP_DAC_SET_RATE, &r, sizeof(r), error)) { noteErrorLocked(error); return false; }
    if (!sendMcuControl(GW_OP_DAC_STREAM_START, nullptr, 0, error)) { noteErrorLocked(error); return false; }
    active_ = true;
    last_error_.clear();
    return true;
}

bool DacOutput::stop(std::string& error) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (config_.enabled && config_.transport == "rp2040") {
        std::string ignored;
        sendMcuControl(GW_OP_DAC_STREAM_STOP, nullptr, 0, ignored);
        (void)ignored;
    }
    active_ = false;
    error.clear();
    return true;
}

bool DacOutput::flush(std::string& error) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!config_.enabled) { error = "DAC is disabled"; noteErrorLocked(error); return false; }
    if (config_.transport == "native") {
        try {
            if (!native_) native_ = std::make_unique<MCP4922>(config_.native);
            native_->writeRawBoth(0, 0);
            raw_a_ = raw_b_ = 0;
            frames_written_++;
            last_error_.clear();
            return true;
        } catch (const std::exception& e) { error = e.what(); noteErrorLocked(error); return false; }
    }
    if (!sendMcuControl(GW_OP_DAC_FLUSH, nullptr, 0, error)) { noteErrorLocked(error); return false; }
    raw_a_ = raw_b_ = 0;
    last_error_.clear();
    return true;
}

bool DacOutput::setRate(uint32_t rate_hz, std::string& error) {
    if (rate_hz == 0 || rate_hz > 1000000) { error = "DAC rate must be 1..1000000"; return false; }
    std::lock_guard<std::mutex> lock(mtx_);
    config_.sample_rate_hz = rate_hz;
    if (config_.enabled && config_.transport == "rp2040") {
        gw_rate_payload_t r{}; r.sample_rate_hz = rate_hz;
        if (!sendMcuControl(GW_OP_DAC_SET_RATE, &r, sizeof(r), error)) { noteErrorLocked(error); return false; }
    }
    last_error_.clear();
    return true;
}

bool DacOutput::setFormat(uint8_t channel_count, uint8_t sample_format, std::string& error) {
    if (channel_count < 1 || channel_count > 2) { error = "DAC channel_count must be 1 or 2"; return false; }
    if (sample_format != GW_SAMPLE_U16_LE && sample_format != GW_SAMPLE_PACKED_U12_LE) { error = "Unsupported DAC sample format"; return false; }
    std::lock_guard<std::mutex> lock(mtx_);
    config_.channel_count = channel_count;
    config_.sample_format = sample_format;
    if (config_.enabled && config_.transport == "rp2040") {
        gw_format_payload_t f{}; f.channel_count = channel_count; f.sample_format = sample_format;
        if (!sendMcuControl(GW_OP_DAC_SET_FORMAT, &f, sizeof(f), error)) { noteErrorLocked(error); return false; }
    }
    last_error_.clear();
    return true;
}

bool DacOutput::writeRawBoth(uint16_t raw_a, uint16_t raw_b, std::string& error) {
    if (config_.enabled && config_.transport == "rp2040" && adc_sampler_) {
        if (!adc_sampler_->waitForGwpProtocol(1500, error)) {
            std::lock_guard<std::mutex> lock(mtx_);
            noteErrorLocked(error);
            return false;
        }
    }
    std::lock_guard<std::mutex> lock(mtx_);
    if (!config_.enabled) { error = "DAC is disabled"; noteErrorLocked(error); return false; }
    raw_a = clampRaw12(raw_a);
    raw_b = clampRaw12(raw_b);
    if (config_.transport == "native") {
        try {
            if (!native_) native_ = std::make_unique<MCP4922>(config_.native);
            native_->writeRawBoth(raw_a, raw_b);
        } catch (const std::exception& e) { error = e.what(); noteErrorLocked(error); return false; }
    } else {
        uint16_t args[2] = {raw_a, raw_b};
        if (!active_) {
            gw_format_payload_t f{}; f.channel_count = 2; f.sample_format = GW_SAMPLE_U16_LE;
            std::string ignored;
            sendMcuControl(GW_OP_DAC_SET_FORMAT, &f, sizeof(f), ignored);
        }
        if (!sendMcuControl(GW_OP_DAC_WRITE_FRAME, args, sizeof(args), error)) { noteErrorLocked(error); return false; }
    }
    raw_a_ = raw_a;
    raw_b_ = raw_b;
    frames_written_++;
    last_error_.clear();
    return true;
}

bool DacOutput::writeRawBlock(const std::vector<std::pair<uint16_t, uint16_t>>& frames, std::string& error) {
    if (frames.empty()) { error.clear(); return true; }
    if (config_.enabled && config_.transport == "rp2040" && adc_sampler_) {
        if (!adc_sampler_->waitForGwpProtocol(1500, error)) {
            std::lock_guard<std::mutex> lock(mtx_);
            noteErrorLocked(error);
            return false;
        }
    }
    std::lock_guard<std::mutex> lock(mtx_);
    if (!config_.enabled) { error = "DAC is disabled"; noteErrorLocked(error); return false; }
    if (config_.transport == "native") {
        try {
            if (!native_) native_ = std::make_unique<MCP4922>(config_.native);
            for (const auto& frame : frames) {
                native_->writeRawBoth(clampRaw12(frame.first), clampRaw12(frame.second));
                raw_a_ = clampRaw12(frame.first);
                raw_b_ = clampRaw12(frame.second);
                frames_written_++;
            }
        } catch (const std::exception& e) { error = e.what(); noteErrorLocked(error); return false; }
    } else {
        std::vector<uint8_t> payload;
        const uint16_t frame_count = static_cast<uint16_t>(std::min<size_t>(frames.size(), 512));
        payload.resize(sizeof(gw_dac_data_payload_t) + frame_count * 4u);
        gw_dac_data_payload_t meta{};
        meta.frame_count = frame_count;
        meta.channel_count = 2;
        meta.sample_format = GW_SAMPLE_U16_LE;
        std::memcpy(payload.data(), &meta, sizeof(meta));
        uint8_t* p = payload.data() + sizeof(meta);
        for (uint16_t i = 0; i < frame_count; ++i) {
            uint16_t a = clampRaw12(frames[i].first);
            uint16_t b = clampRaw12(frames[i].second);
            std::memcpy(p + i * 4u, &a, 2);
            std::memcpy(p + i * 4u + 2u, &b, 2);
            raw_a_ = a; raw_b_ = b;
        }
        if (!active_) {
            std::string ignored;
            gw_format_payload_t f{}; f.channel_count = 2; f.sample_format = GW_SAMPLE_U16_LE;
            sendMcuControl(GW_OP_DAC_SET_FORMAT, &f, sizeof(f), ignored);
            sendMcuControl(GW_OP_DAC_STREAM_START, nullptr, 0, ignored);
            active_ = true;
        }
        if (!sendMcuData(payload, error)) { noteErrorLocked(error); return false; }
        frames_written_ += frame_count;
    }
    last_error_.clear();
    return true;
}

bool DacOutput::writeVoltsBoth(double volts_a, double volts_b, std::string& error) {
    return writeRawBoth(voltsToRawA(volts_a), voltsToRawB(volts_b), error);
}

bool DacOutput::playDtmf(const std::string& digits, uint16_t tone_ms, uint16_t gap_ms, uint16_t amplitude, uint8_t channel_mask, std::string& error) {
    if (config_.enabled && config_.transport == "rp2040" && adc_sampler_) {
        if (!adc_sampler_->waitForGwpProtocol(1500, error)) {
            std::lock_guard<std::mutex> lock(mtx_);
            noteErrorLocked(error);
            return false;
        }
    }
    std::lock_guard<std::mutex> lock(mtx_);
    if (!config_.enabled) { error = "DAC is disabled"; noteErrorLocked(error); return false; }
    if (config_.transport != "rp2040") { error = "DTMF generation is implemented on RP2040 firmware only"; noteErrorLocked(error); return false; }
    gw_dtmf_play_payload_t p{};
    p.tone_ms = tone_ms ? tone_ms : 100;
    p.gap_ms = gap_ms;
    p.amplitude = std::min<uint16_t>(amplitude, 2047);
    p.channel_mask = (channel_mask & GW_DAC_CHANNEL_STEREO) ? (channel_mask & GW_DAC_CHANNEL_STEREO) : GW_DAC_CHANNEL_A;
    for (char c : digits) {
        if (p.digit_count >= sizeof(p.digits)) break;
        if ((c >= '0' && c <= '9') || c == '*' || c == '#' || (c >= 'A' && c <= 'D') || (c >= 'a' && c <= 'd')) {
            p.digits[p.digit_count++] = c;
        }
    }
    if (p.digit_count == 0) { error = "DTMF digit sequence is empty"; noteErrorLocked(error); return false; }
    if (!sendMcuControl(GW_OP_DAC_DTMF_PLAY, &p, sizeof(p), error)) { noteErrorLocked(error); return false; }
    active_ = true;
    last_error_.clear();
    return true;
}

bool DacOutput::stopDtmf(std::string& error) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!config_.enabled) { error = "DAC is disabled"; noteErrorLocked(error); return false; }
    if (config_.transport != "rp2040") { error = "DTMF generation is implemented on RP2040 firmware only"; noteErrorLocked(error); return false; }
    if (!sendMcuControl(GW_OP_DAC_DTMF_STOP, nullptr, 0, error)) { noteErrorLocked(error); return false; }
    last_error_.clear();
    return true;
}

bool DacOutput::sendMcuControl(uint16_t opcode, const void* args, uint16_t arg_len, std::string& error) {
    const auto pkt = gw::encodeControlRequest(opcode, request_id_++, args, arg_len, packet_seq_++);
    return writeDirectMcu(pkt, error);
}

bool DacOutput::sendMcuData(const std::vector<uint8_t>& payload, std::string& error) {
    const auto pkt = gw::encodePacket(GW_MSG_DATA, GW_STREAM_DAC0, packet_seq_++, payload);
    return writeDirectMcu(pkt, error);
}

bool DacOutput::writeDirectMcu(const std::vector<uint8_t>& packet, std::string& error) {
    if (adc_sampler_) {
        if (!adc_sampler_->sendGwPacketToRp2040(packet, error)) return false;
        packets_sent_++;
        return true;
    }
    if (!ensureDirectMcuOpen(error)) return false;
    size_t sent = 0;
    while (sent < packet.size()) {
        ssize_t rc = direct_mcu_->write(packet.data() + sent, packet.size() - sent);
        if (rc > 0) { sent += static_cast<size_t>(rc); continue; }
        if (rc < 0 && errno == EINTR) continue;
        if (rc < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { usleep(1000); continue; }
        error = "write " + direct_mcu_->description() + " failed";
        direct_mcu_->close();
        return false;
    }
    packets_sent_++;
    return true;
}

bool DacOutput::ensureDirectMcuOpen(std::string& error) {
    if (direct_mcu_ && direct_mcu_->isOpen()) return true;
    direct_mcu_ = std::make_unique<gw::TtyTransport>(config_.rp2040_dev);
    if (!direct_mcu_->open(error)) return false;
    return true;
}

void DacOutput::noteErrorLocked(const std::string& error) {
    last_error_ = error;
    errors_++;
}

double DacOutput::rawToVoltsA(uint16_t raw) const { return rawToVoltsFor(config_.native.channel[0], raw); }
double DacOutput::rawToVoltsB(uint16_t raw) const { return rawToVoltsFor(config_.native.channel[1], raw); }
uint16_t DacOutput::voltsToRawA(double volts) const { return voltsToRawFor(config_.native.channel[0], volts); }
uint16_t DacOutput::voltsToRawB(double volts) const { return voltsToRawFor(config_.native.channel[1], volts); }
