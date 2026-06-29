#pragma once

#include "MCP4922.hpp"
#include "AdcSampler.hpp"
#include "GwProtocol.hpp"
#include "GwTransport.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class DacOutput {
public:
    struct Config {
        bool enabled = false;
        // "native" = CM4 Linux spidev/bitbang MCP4922, "rp2040" = GWP1 over USB CDC.
        std::string transport = "native";
        std::string rp2040_dev = "/dev/ttyACM0";
        uint32_t sample_rate_hz = 48000;
        uint8_t channel_count = 2;
        uint8_t sample_format = GW_SAMPLE_U16_LE;
        MCP4922::Config native;
    };

    struct Status {
        bool enabled = false;
        bool active = false;
        bool healthy = false;
        bool native_open = false;
        bool mcu_connected = false;
        std::string transport;
        std::string rp2040_dev;
        uint32_t sample_rate_hz = 0;
        uint8_t channel_count = 2;
        std::string sample_format = "U16_LE";
        uint16_t raw_a = 0;
        uint16_t raw_b = 0;
        double volts_a = 0.0;
        double volts_b = 0.0;
        uint64_t frames_written = 0;
        uint64_t packets_sent = 0;
        uint64_t errors = 0;
        std::string last_error;
    };

    DacOutput(Config config, AdcSampler* adc_sampler = nullptr);
    ~DacOutput();

    DacOutput(const DacOutput&) = delete;
    DacOutput& operator=(const DacOutput&) = delete;

    Config config() const;
    void updateConfig(Config config);
    Status status() const;

    bool start(std::string& error);
    bool stop(std::string& error);
    bool flush(std::string& error);
    bool setRate(uint32_t rate_hz, std::string& error);
    bool setFormat(uint8_t channel_count, uint8_t sample_format, std::string& error);
    bool writeRawBoth(uint16_t raw_a, uint16_t raw_b, std::string& error);
    bool writeVoltsBoth(double volts_a, double volts_b, std::string& error);
    bool playDtmf(const std::string& digits, uint16_t tone_ms, uint16_t gap_ms, uint16_t amplitude, uint8_t channel_mask, std::string& error);
    bool stopDtmf(std::string& error);

private:
    bool sendMcuControl(uint16_t opcode, const void* args, uint16_t arg_len, std::string& error);
    bool sendMcuData(const std::vector<uint8_t>& payload, std::string& error);
    bool writeDirectMcu(const std::vector<uint8_t>& packet, std::string& error);
    bool ensureDirectMcuOpen(std::string& error);
    void noteErrorLocked(const std::string& error);
    double rawToVoltsA(uint16_t raw) const;
    double rawToVoltsB(uint16_t raw) const;
    uint16_t voltsToRawA(double volts) const;
    uint16_t voltsToRawB(double volts) const;

    mutable std::mutex mtx_;
    Config config_;
    AdcSampler* adc_sampler_ = nullptr;
    std::unique_ptr<MCP4922> native_;
    std::unique_ptr<gw::TtyTransport> direct_mcu_;
    bool active_ = false;
    uint16_t raw_a_ = 0;
    uint16_t raw_b_ = 0;
    uint64_t frames_written_ = 0;
    uint64_t packets_sent_ = 0;
    uint64_t errors_ = 0;
    std::string last_error_;
    uint32_t packet_seq_ = 1;
    uint16_t request_id_ = 1;
};
