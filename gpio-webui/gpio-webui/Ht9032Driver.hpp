#pragma once

#include "SystemContext.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <set>
#include <vector>
#include <gpiod.hpp>

struct Ht9032Settings {
    bool enabled = true;
    int pdwn_phys = 36;
    int cdet_phys = 37;
    int dout_phys = 15;
    int doutc_phys = 38;
    int rdet_phys = 0; // 0 = not wired/unused
    bool pdwn_control = true;
    bool powered = true; // true drives PDWN low when pdwn_control is enabled
    bool active_low_cdet = true;
    bool active_low_rdet = true;
    std::string monitor_mode = "both"; // both, dout, doutc
    int baud = 1200;
};

class Ht9032Driver {
public:
    explicit Ht9032Driver(std::shared_ptr<SystemContext> context);
    ~Ht9032Driver();

    void start();
    void stop();

    Ht9032Settings settings() const;
    nlohmann::json settingsJson() const;
    nlohmann::json snapshotJson() const;
    void updateFromJson(const nlohmann::json& j);

    std::set<int> reservedBcms() const;
    std::string validationHelp() const;

    static Ht9032Settings settingsFromJson(const nlohmann::json& j, const Ht9032Settings& defaults = Ht9032Settings());
    static nlohmann::json settingsToJson(const Ht9032Settings& s);
    static void validate(const Ht9032Settings& s);

private:
    struct LineDecoder {
        bool enabled = false;
        bool last_level = true;
        bool receiving = false;
        int bit_index = 0;
        uint8_t byte = 0;
        std::chrono::steady_clock::time_point next_sample{};
        std::string bits;
        std::vector<uint8_t> bytes;
        std::string status;
        double confidence = 0.0;
        bool decoded = false;
        bool checksum_ok = false;
        std::string message_type;
        std::string date_time;
        std::string number;
        std::string name;
    };

    void worker();
    void load();
    void saveLocked();
    void resetDecodeLocked(LineDecoder& d);
    void feedSerialLocked(LineDecoder& d, bool level, std::chrono::steady_clock::time_point now, int baud);
    static void parseCallerId(LineDecoder& d);
    static std::string bytesHex(const std::vector<uint8_t>& bytes);

    std::shared_ptr<SystemContext> context_;
    mutable std::mutex mtx_;
    Ht9032Settings settings_;
    bool pdwn_level_ = true;
    bool cdet_level_ = true;
    bool carrier_ = false;
    bool rdet_level_ = true;
    bool ring_detect_ = false;
    bool dout_level_ = true;
    bool doutc_level_ = true;
    std::chrono::steady_clock::time_point last_carrier_seen_{};
    LineDecoder dout_;
    LineDecoder doutc_;
    std::string status_ = "stopped";
    std::string last_error_;
    uint64_t samples_ = 0;

    std::atomic<bool> running_{false};
    std::thread worker_;
};
