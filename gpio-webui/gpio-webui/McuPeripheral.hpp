#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <deque>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>

struct McuDtmfDecoderConfig {
    bool enabled = true;
    std::string source = "mcu_mt8870";
    int stq_gpio = 12;
    int q1_gpio = 27;
    int q2_gpio = 26;
    int q3_gpio = 10;
    int q4_gpio = 11;
    bool stq_active_high = true;
    bool q_active_high = true;
    int debounce_ms = 2;
    int event_holdoff_ms = 25;
    size_t history_limit = 64;
    bool validation_enabled = true;
    int raw_poll_hz = 20;
};

struct McuCh1817SignalConfig {
    std::string source = "cm4"; // cm4, mcu, disabled
    int gpio = -1;
    bool active_high = true;
};

struct McuPeripheralConfig {
    bool enabled = true;
    McuDtmfDecoderConfig dtmf_decoder;
    McuCh1817SignalConfig ri{"cm4", 8, false}; // CH1817 RI is active-low by default.
    McuCh1817SignalConfig oh{"cm4", 7, true};  // CH1817 OH/OFFHK is active-high by default.
};

struct DtmfDecodedEvent {
    uint64_t server_timestamp_ms = 0;
    uint32_t mcu_timestamp_ms = 0;
    uint32_t sequence = 0;
    std::string source = "mcu_mt8870";
    char digit = 0;
    uint8_t raw_q_bits = 0;
    bool stq_active = false;
};

struct McuPeripheralSnapshot {
    McuPeripheralConfig config;
    bool connected = false;
    bool status_seen = false;
    bool config_dirty = true;
    bool config_sent = false;
    uint64_t last_config_send_server_ms = 0;
    uint32_t config_apply_count = 0;
    std::string last_config_error;
    uint64_t last_status_server_ms = 0;
    uint32_t uptime_ms = 0;
    bool enabled = false;
    bool dtmf_enabled = false;
    bool dtmf_active = false;
    bool stq_raw = false;
    bool q1_raw = false;
    bool q2_raw = false;
    bool q3_raw = false;
    bool q4_raw = false;
    uint8_t raw_q_bits = 0;
    char decoded_digit = 0;
    uint32_t dtmf_sequence = 0;
    uint32_t dtmf_event_ms = 0;
    bool ri_raw = false;
    bool ri_logical = false;
    uint32_t ri_transition_count = 0;
    bool oh_raw = false;
    bool oh_logical = false;
    bool oh_drive = false;
    uint32_t oh_transition_count = 0;
    std::deque<DtmfDecodedEvent> history;
};

inline int mcuClampInt(int v, int lo, int hi) { return std::max(lo, std::min(hi, v)); }

inline bool mcuJsonBoolish(const nlohmann::json& j, const char* key, bool def) {
    if (!j.is_object() || !j.contains(key)) return def;
    const auto& v = j[key];
    if (v.is_boolean()) return v.get<bool>();
    if (v.is_number_integer()) return v.get<int>() != 0;
    if (v.is_string()) {
        std::string s = v.get<std::string>();
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s == "1" || s == "true" || s == "yes" || s == "on" || s == "enabled" || s == "mcu" || s == "mcu_mt8870";
    }
    return def;
}

inline std::string mcuValidSource(std::string s, const std::string& def) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (s == "cm4" || s == "mcu" || s == "disabled" || s == "mcu_mt8870" || s == "software_adc") return s;
    return def;
}

inline McuCh1817SignalConfig mcuSignalConfigFromJson(const nlohmann::json& j, const McuCh1817SignalConfig& def) {
    McuCh1817SignalConfig out = def;
    if (!j.is_object()) return out;
    out.source = mcuValidSource(j.value("source", out.source), out.source);
    out.gpio = mcuClampInt(j.value("gpio", out.gpio), -1, 29);
    out.active_high = mcuJsonBoolish(j, "active_high", out.active_high);
    return out;
}

inline nlohmann::json mcuSignalConfigToJson(const McuCh1817SignalConfig& c) {
    return {{"source", c.source}, {"gpio", c.gpio}, {"active_high", c.active_high}};
}

inline McuPeripheralConfig mcuPeripheralConfigFromJson(const nlohmann::json& root, const McuPeripheralConfig& def = McuPeripheralConfig()) {
    McuPeripheralConfig out = def;
    const nlohmann::json& j = root.contains("mcu_peripherals") ? root["mcu_peripherals"] : root;
    if (!j.is_object()) return out;
    out.enabled = mcuJsonBoolish(j, "enabled", out.enabled);

    if (j.contains("dtmf_decoder") && j["dtmf_decoder"].is_object()) {
        const auto& d = j["dtmf_decoder"];
        out.dtmf_decoder.enabled = mcuJsonBoolish(d, "enabled", out.dtmf_decoder.enabled);
        out.dtmf_decoder.source = mcuValidSource(d.value("source", out.dtmf_decoder.source), out.dtmf_decoder.source);
        if (d.contains("pins") && d["pins"].is_object()) {
            const auto& p = d["pins"];
            out.dtmf_decoder.stq_gpio = mcuClampInt(p.value("stq", out.dtmf_decoder.stq_gpio), 0, 29);
            out.dtmf_decoder.q1_gpio = mcuClampInt(p.value("q1", out.dtmf_decoder.q1_gpio), 0, 29);
            out.dtmf_decoder.q2_gpio = mcuClampInt(p.value("q2", out.dtmf_decoder.q2_gpio), 0, 29);
            out.dtmf_decoder.q3_gpio = mcuClampInt(p.value("q3", out.dtmf_decoder.q3_gpio), 0, 29);
            out.dtmf_decoder.q4_gpio = mcuClampInt(p.value("q4", out.dtmf_decoder.q4_gpio), 0, 29);
        } else {
            out.dtmf_decoder.stq_gpio = mcuClampInt(d.value("stq_gpio", out.dtmf_decoder.stq_gpio), 0, 29);
            out.dtmf_decoder.q1_gpio = mcuClampInt(d.value("q1_gpio", out.dtmf_decoder.q1_gpio), 0, 29);
            out.dtmf_decoder.q2_gpio = mcuClampInt(d.value("q2_gpio", out.dtmf_decoder.q2_gpio), 0, 29);
            out.dtmf_decoder.q3_gpio = mcuClampInt(d.value("q3_gpio", out.dtmf_decoder.q3_gpio), 0, 29);
            out.dtmf_decoder.q4_gpio = mcuClampInt(d.value("q4_gpio", out.dtmf_decoder.q4_gpio), 0, 29);
        }
        if (d.contains("polarity") && d["polarity"].is_object()) {
            const auto& p = d["polarity"];
            out.dtmf_decoder.stq_active_high = mcuJsonBoolish(p, "stq_active_high", out.dtmf_decoder.stq_active_high);
            out.dtmf_decoder.q_active_high = mcuJsonBoolish(p, "q_active_high", out.dtmf_decoder.q_active_high);
        } else {
            out.dtmf_decoder.stq_active_high = mcuJsonBoolish(d, "stq_active_high", out.dtmf_decoder.stq_active_high);
            out.dtmf_decoder.q_active_high = mcuJsonBoolish(d, "q_active_high", out.dtmf_decoder.q_active_high);
        }
        out.dtmf_decoder.debounce_ms = mcuClampInt(d.value("debounce_ms", out.dtmf_decoder.debounce_ms), 0, 1000);
        out.dtmf_decoder.event_holdoff_ms = mcuClampInt(d.value("event_holdoff_ms", out.dtmf_decoder.event_holdoff_ms), 0, 10000);
        out.dtmf_decoder.history_limit = static_cast<size_t>(mcuClampInt(d.value("history_limit", static_cast<int>(out.dtmf_decoder.history_limit)), 1, 512));
        if (d.contains("validation") && d["validation"].is_object()) {
            out.dtmf_decoder.validation_enabled = mcuJsonBoolish(d["validation"], "enabled", out.dtmf_decoder.validation_enabled);
            out.dtmf_decoder.raw_poll_hz = mcuClampInt(d["validation"].value("raw_poll_hz", out.dtmf_decoder.raw_poll_hz), 1, 100);
        }
    }

    if (j.contains("ch1817_signals") && j["ch1817_signals"].is_object()) {
        const auto& c = j["ch1817_signals"];
        McuCh1817SignalConfig ri_def = out.ri;
        ri_def.gpio = 8;
        ri_def.active_high = false;
        McuCh1817SignalConfig oh_def = out.oh;
        oh_def.gpio = 7;
        oh_def.active_high = true;
        out.ri = mcuSignalConfigFromJson(c.value("ri", nlohmann::json::object()), ri_def);
        out.oh = mcuSignalConfigFromJson(c.value("oh", nlohmann::json::object()), oh_def);
        if (c.contains("enabled") && !mcuJsonBoolish(c, "enabled", true)) {
            if (out.ri.source == "mcu") out.ri.source = "disabled";
            if (out.oh.source == "mcu") out.oh.source = "disabled";
        }
    }
    return out;
}

inline nlohmann::json mcuPeripheralConfigToJson(const McuPeripheralConfig& c) {
    return {
        {"enabled", c.enabled},
        {"dtmf_decoder", {
            {"enabled", c.dtmf_decoder.enabled},
            {"source", c.dtmf_decoder.source},
            {"pins", {
                {"stq", c.dtmf_decoder.stq_gpio}, {"q1", c.dtmf_decoder.q1_gpio},
                {"q2", c.dtmf_decoder.q2_gpio}, {"q3", c.dtmf_decoder.q3_gpio}, {"q4", c.dtmf_decoder.q4_gpio}
            }},
            {"polarity", {{"stq_active_high", c.dtmf_decoder.stq_active_high}, {"q_active_high", c.dtmf_decoder.q_active_high}}},
            {"debounce_ms", c.dtmf_decoder.debounce_ms},
            {"event_holdoff_ms", c.dtmf_decoder.event_holdoff_ms},
            {"history_limit", c.dtmf_decoder.history_limit},
            {"validation", {{"enabled", c.dtmf_decoder.validation_enabled}, {"raw_poll_hz", c.dtmf_decoder.raw_poll_hz}}}
        }},
        {"ch1817_signals", {
            {"enabled", c.enabled},
            {"ri", mcuSignalConfigToJson(c.ri)},
            {"oh", mcuSignalConfigToJson(c.oh)}
        }}
    };
}

inline McuPeripheralConfig loadMcuPeripheralConfigFromFile(const std::string& path, const McuPeripheralConfig& def = McuPeripheralConfig()) {
    std::ifstream f(path);
    if (!f.is_open()) return def;
    try {
        nlohmann::json j;
        f >> j;
        return mcuPeripheralConfigFromJson(j, def);
    } catch (...) {
        return def;
    }
}

inline bool configFileHasMcuPeripheralSection(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return false;
    try {
        nlohmann::json j;
        f >> j;
        return j.is_object() && j.contains("mcu_peripherals") && j["mcu_peripherals"].is_object();
    } catch (...) {
        return false;
    }
}

inline void persistMcuPeripheralConfigToFile(const std::string& path, const McuPeripheralConfig& cfg) {
    nlohmann::json root;
    {
        std::ifstream f(path);
        if (f.is_open()) {
            try { f >> root; } catch (...) { root = nlohmann::json::object(); }
        }
    }
    if (!root.is_object()) root = nlohmann::json::object();
    root["mcu_peripherals"] = mcuPeripheralConfigToJson(cfg);
    std::ofstream out(path);
    if (out.is_open()) out << root.dump(2);
}
