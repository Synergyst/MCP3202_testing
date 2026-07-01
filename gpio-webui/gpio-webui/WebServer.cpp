#include "WebServer.hpp"
#include "AudioProcessing.hpp"
#include "libs/audio_filters/AudioFilters.hpp"
#include <nlohmann/json.hpp>
#include <iostream>
#include <sstream>
#include <map>
#include <cstdlib>
#include <string>
#include <utility>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cmath>
#include <fstream>
#include <vector>
#include <cctype>

using json = nlohmann::json;

namespace {
std::string readFskSourceSetting(const std::shared_ptr<SystemContext>& context) {
    json j;
    if (context) {
        std::lock_guard<std::mutex> lock(context->config_mutex);
        std::ifstream f(context->config_path);
        if (f.is_open()) { try { f >> j; } catch (...) { j = json::object(); } }
    }
    if (!j.is_object()) j = json::object();
    std::string src = j.value("fsk_source", "auto");
    if (src != "auto" && src != "software_adc") src = "auto";
    return src;
}

double callerIdScore(const json& j) {
    double score = j.value("confidence", 0.0);
    if (j.value("detected", false)) score += 1.0;
    if (j.value("checksum_ok", false)) score += 2.0;
    return score;
}

json disabledCallerIdJson(const std::string& source, const std::string& reason) {
    return {{"enabled", false}, {"running", false}, {"detected", false}, {"checksum_ok", false},
            {"source", source}, {"source_label", source}, {"status", reason}, {"last_error", reason},
            {"confidence", 0.0}, {"number", ""}, {"name", ""}, {"date_time", ""},
            {"message_type", ""}, {"raw_bits", ""}, {"raw_bytes_hex", ""}};
}

std::string trimCopy(const std::string& s) {
    const auto first = std::find_if_not(s.begin(), s.end(), [](unsigned char c) { return std::isspace(c); });
    const auto last = std::find_if_not(s.rbegin(), s.rend(), [](unsigned char c) { return std::isspace(c); }).base();
    if (first >= last) return "";
    return std::string(first, last);
}

std::string lowerCopy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::set<int> adcReservedPinsForConfig(const AdcSampler::Config& cfg) {
    std::set<int> pins;
    if (!cfg.enabled || cfg.adc_source == "rp2040") return pins;
    pins.insert(cfg.adc.clk_bcm);
    pins.insert(cfg.adc.mosi_bcm);
    pins.insert(cfg.adc.miso_bcm);
    if (cfg.adc.cs_bcm >= 0) pins.insert(cfg.adc.cs_bcm);
    if (!cfg.adc.bitbang) {
        if (cfg.adc.device.find("spidev0.1") != std::string::npos) pins.insert(7);
        else if (cfg.adc.device.find("spidev0.0") != std::string::npos) pins.insert(8);
    }
    return pins;
}


std::set<int> dacReservedPinsForConfig(const DacOutput::Config& cfg) {
    std::set<int> pins;
    if (!cfg.enabled || cfg.transport != "native") return pins;
    const auto& dac = cfg.native;
    pins.insert(dac.clk_bcm);
    pins.insert(dac.mosi_bcm);
    if (dac.cs_bcm >= 0) pins.insert(dac.cs_bcm);
    if (dac.ldac_bcm >= 0) pins.insert(dac.ldac_bcm);
    if (dac.shdn_bcm >= 0) pins.insert(dac.shdn_bcm);
    if (!dac.bitbang) {
        if (dac.device.find("spidev0.1") != std::string::npos) pins.insert(7);
        else if (dac.device.find("spidev0.0") != std::string::npos) pins.insert(8);
    }
    return pins;
}

void persistDacSettings(ConfigManager& cfg, const DacOutput::Config& out) {
    const auto& dac = out.native;
    cfg.setSetting("dac_enabled", out.enabled ? "true" : "false");
    cfg.setSetting("dac_transport", out.transport);
    cfg.setSetting("dac_rp2040_dev", out.rp2040_dev);
    cfg.setSetting("dac_sample_rate_hz", std::to_string(out.sample_rate_hz));
    cfg.setSetting("dac_channel_count", std::to_string(out.channel_count));
    cfg.setSetting("dac_sample_format", std::to_string(out.sample_format));
    cfg.setSetting("dac_bitbang", dac.bitbang ? "true" : "false");
    cfg.setSetting("dac_spi_dev", dac.device);
    cfg.setSetting("dac_spi_speed_hz", std::to_string(dac.speed_hz));
    cfg.setSetting("dac_cs_bcm", std::to_string(dac.cs_bcm));
    cfg.setSetting("dac_clk_bcm", std::to_string(dac.clk_bcm));
    cfg.setSetting("dac_mosi_bcm", std::to_string(dac.mosi_bcm));
    cfg.setSetting("dac_ldac_bcm", std::to_string(dac.ldac_bcm));
    cfg.setSetting("dac_shdn_bcm", std::to_string(dac.shdn_bcm));
    cfg.setSetting("dac_vref_a", std::to_string(dac.channel[0].vref));
    cfg.setSetting("dac_vref_b", std::to_string(dac.channel[1].vref));
    cfg.setSetting("dac_gain_a", dac.channel[0].gain_1x ? "1" : "2");
    cfg.setSetting("dac_gain_b", dac.channel[1].gain_1x ? "1" : "2");
    cfg.setSetting("dac_buffered_a", dac.channel[0].buffered_vref ? "true" : "false");
    cfg.setSetting("dac_buffered_b", dac.channel[1].buffered_vref ? "true" : "false");
}

json dacStatusJson(const DacOutput::Status& s) {
    return {
        {"enabled", s.enabled}, {"active", s.active}, {"healthy", s.healthy},
        {"native_open", s.native_open}, {"mcu_connected", s.mcu_connected},
        {"transport", s.transport}, {"rp2040_dev", s.rp2040_dev},
        {"sample_rate_hz", s.sample_rate_hz}, {"channel_count", s.channel_count},
        {"sample_format", s.sample_format}, {"raw_a", s.raw_a}, {"raw_b", s.raw_b},
        {"volts_a", s.volts_a}, {"volts_b", s.volts_b},
        {"frames_written", s.frames_written}, {"packets_sent", s.packets_sent},
        {"errors", s.errors}, {"last_error", s.last_error}
    };
}

json dacConfigJson(const DacOutput::Config& c) {
    const auto& d = c.native;
    return {
        {"enabled", c.enabled}, {"transport", c.transport}, {"rp2040_dev", c.rp2040_dev},
        {"sample_rate_hz", c.sample_rate_hz}, {"channel_count", c.channel_count},
        {"sample_format", c.sample_format}, {"bitbang", d.bitbang}, {"spi_dev", d.device},
        {"spi_speed_hz", d.speed_hz}, {"cs_bcm", d.cs_bcm}, {"clk_bcm", d.clk_bcm},
        {"mosi_bcm", d.mosi_bcm}, {"ldac_bcm", d.ldac_bcm}, {"shdn_bcm", d.shdn_bcm},
        {"vref_a", d.channel[0].vref}, {"vref_b", d.channel[1].vref},
        {"gain_a", d.channel[0].gain_1x ? 1 : 2}, {"gain_b", d.channel[1].gain_1x ? 1 : 2},
        {"buffered_a", d.channel[0].buffered_vref}, {"buffered_b", d.channel[1].buffered_vref}
    };
}


std::string qBitsBinary(uint8_t bits) {
    std::string out;
    for (int i = 3; i >= 0; --i) out.push_back((bits & (1u << i)) ? '1' : '0');
    return out;
}

json mcuDtmfEventJson(const DtmfDecodedEvent& e) {
    return {
        {"server_timestamp_ms", e.server_timestamp_ms},
        {"mcu_timestamp_ms", e.mcu_timestamp_ms},
        {"sequence", e.sequence},
        {"source", e.source},
        {"digit", e.digit ? std::string(1, e.digit) : std::string("")},
        {"raw_q_bits", e.raw_q_bits},
        {"raw_q_bits_binary", qBitsBinary(e.raw_q_bits)},
        {"stq_active", e.stq_active}
    };
}

json mcuPeripheralConfigJson(const McuPeripheralConfig& c) {
    return mcuPeripheralConfigToJson(c);
}

json mcuPeripheralStatusJson(const McuPeripheralSnapshot& s) {
    json hist = json::array();
    for (const auto& e : s.history) hist.push_back(mcuDtmfEventJson(e));
    return {
        {"connected", s.connected},
        {"status_seen", s.status_seen},
        {"last_status_server_ms", s.last_status_server_ms},
        {"uptime_ms", s.uptime_ms},
        {"config", mcuPeripheralConfigJson(s.config)},
        {"dtmf_decoder", {
            {"enabled", s.config.dtmf_decoder.enabled},
            {"source", s.config.dtmf_decoder.source},
            {"active", s.dtmf_active},
            {"current_digit", s.decoded_digit ? std::string(1, s.decoded_digit) : std::string("")},
            {"last_digit", s.decoded_digit ? std::string(1, s.decoded_digit) : std::string("")},
            {"last_sequence", s.dtmf_sequence},
            {"mcu_timestamp_ms", s.dtmf_event_ms},
            {"history_limit", s.config.dtmf_decoder.history_limit},
            {"raw", {
                {"stq", s.stq_raw}, {"q1", s.q1_raw}, {"q2", s.q2_raw}, {"q3", s.q3_raw}, {"q4", s.q4_raw},
                {"q_bits", s.raw_q_bits}, {"q_bits_binary", qBitsBinary(s.raw_q_bits)}
            }},
            {"history", hist}
        }},
        {"ch1817_signals", {
            {"ri", {{"source", s.config.ri.source}, {"raw", s.ri_raw}, {"logical", s.ri_logical}, {"transition_count", s.ri_transition_count}}},
            {"oh", {{"source", s.config.oh.source}, {"raw", s.oh_raw}, {"logical", s.oh_logical}, {"transition_count", s.oh_transition_count}}}
        }}
    };
}

DacOutput::Config dacConfigFromJson(const json& j, DacOutput::Config cfg) {
    auto& d = cfg.native;
    if (j.contains("enabled")) cfg.enabled = j.at("enabled").get<bool>();
    if (j.contains("transport")) cfg.transport = j.at("transport").get<std::string>();
    if (cfg.transport != "native" && cfg.transport != "rp2040") throw std::runtime_error("dac transport must be native or rp2040");
    if (j.contains("rp2040_dev")) cfg.rp2040_dev = j.at("rp2040_dev").get<std::string>();
    if (j.contains("sample_rate_hz")) cfg.sample_rate_hz = j.at("sample_rate_hz").get<uint32_t>();
    if (j.contains("channel_count")) cfg.channel_count = j.at("channel_count").get<uint8_t>();
    if (j.contains("sample_format")) {
        if (j.at("sample_format").is_string()) {
            std::string sf = j.at("sample_format").get<std::string>();
            cfg.sample_format = (sf == "PACKED_U12_LE" || sf == "packed_u12") ? GW_SAMPLE_PACKED_U12_LE : GW_SAMPLE_U16_LE;
        } else cfg.sample_format = j.at("sample_format").get<uint8_t>();
    }
    if (j.contains("bitbang")) d.bitbang = j.at("bitbang").get<bool>();
    if (j.contains("spi_dev")) d.device = j.at("spi_dev").get<std::string>();
    if (j.contains("spi_speed_hz")) d.speed_hz = j.at("spi_speed_hz").get<uint32_t>();
    if (j.contains("cs_bcm")) d.cs_bcm = j.at("cs_bcm").get<int>();
    if (j.contains("clk_bcm")) d.clk_bcm = j.at("clk_bcm").get<int>();
    if (j.contains("mosi_bcm")) d.mosi_bcm = j.at("mosi_bcm").get<int>();
    if (j.contains("ldac_bcm")) d.ldac_bcm = j.at("ldac_bcm").get<int>();
    if (j.contains("shdn_bcm")) d.shdn_bcm = j.at("shdn_bcm").get<int>();
    if (j.contains("vref")) { double v = j.at("vref").get<double>(); d.channel[0].vref = v; d.channel[1].vref = v; }
    if (j.contains("vref_a")) d.channel[0].vref = j.at("vref_a").get<double>();
    if (j.contains("vref_b")) d.channel[1].vref = j.at("vref_b").get<double>();
    if (j.contains("gain_a")) d.channel[0].gain_1x = j.at("gain_a").get<int>() == 1;
    if (j.contains("gain_b")) d.channel[1].gain_1x = j.at("gain_b").get<int>() == 1;
    if (j.contains("buffered_a")) d.channel[0].buffered_vref = j.at("buffered_a").get<bool>();
    if (j.contains("buffered_b")) d.channel[1].buffered_vref = j.at("buffered_b").get<bool>();
    if (cfg.sample_rate_hz == 0 || cfg.sample_rate_hz > 1000000) throw std::runtime_error("dac sample_rate_hz must be 1..1000000");
    if (cfg.channel_count < 1 || cfg.channel_count > 2) throw std::runtime_error("dac channel_count must be 1 or 2");
    if (cfg.sample_format != GW_SAMPLE_U16_LE && cfg.sample_format != GW_SAMPLE_PACKED_U12_LE) throw std::runtime_error("unsupported DAC sample_format");
    if (d.channel[0].vref <= 0.0 || d.channel[1].vref <= 0.0) throw std::runtime_error("dac vref values must be positive");
    d.enabled = cfg.enabled && cfg.transport == "native";
    return cfg;
}

std::vector<std::string> splitEffectsCsv(const std::string& csv) {
    std::vector<std::string> out;
    std::stringstream ss(csv);
    std::string item;
    while (std::getline(ss, item, ',')) {
        item = lowerCopy(trimCopy(item));
        if (item.empty() || item == "none") continue;
        if (std::find(out.begin(), out.end(), item) == out.end()) out.push_back(item);
    }
    return out;
}

std::string joinEffectsCsv(const std::vector<std::string>& effects) {
    std::string out;
    for (const auto& e : effects) {
        if (!out.empty()) out += ",";
        out += e;
    }
    return out;
}

std::string wavProfileContextForMode(const std::string& mode) {
    if (mode == "ch0" || mode == "mono-ch0") return "adc.wav.ch0";
    if (mode == "ch1" || mode == "mono-ch1") return "adc.wav.ch1";
    if (mode == "mix" || mode == "mono-mix") return "adc.wav.mix";
    if (mode == "stereo" || mode == "ch0ch1") return "adc.wav.stereo";
    return "adc.wav.ch0";
}

uint16_t clampAdc(double v) {
    if (v < 0.0) return 0;
    if (v > 4095.0) return 4095;
    return static_cast<uint16_t>(std::llround(v));
}

struct GraphBiquad {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    double z1 = 0.0, z2 = 0.0;
    double process(double x) {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

GraphBiquad makeGraphBandpass(double sample_rate, double center_hz, double q) {
    GraphBiquad f;
    if (sample_rate <= 0.0 || center_hz <= 0.0 || center_hz >= sample_rate * 0.45 || q <= 0.0) return f;
    const double w0 = 2.0 * M_PI * center_hz / sample_rate;
    const double alpha = std::sin(w0) / (2.0 * q);
    const double cosw0 = std::cos(w0);
    const double a0 = 1.0 + alpha;
    f.b0 = alpha / a0;
    f.b1 = 0.0;
    f.b2 = -alpha / a0;
    f.a1 = (-2.0 * cosw0) / a0;
    f.a2 = (1.0 - alpha) / a0;
    return f;
}

GraphBiquad makeGraphNotch(double sample_rate, double center_hz, double q) {
    GraphBiquad f;
    if (sample_rate <= 0.0 || center_hz <= 0.0 || center_hz >= sample_rate * 0.45 || q <= 0.0) return f;
    const double w0 = 2.0 * M_PI * center_hz / sample_rate;
    const double alpha = std::sin(w0) / (2.0 * q);
    const double cosw0 = std::cos(w0);
    const double a0 = 1.0 + alpha;
    f.b0 = 1.0 / a0;
    f.b1 = (-2.0 * cosw0) / a0;
    f.b2 = 1.0 / a0;
    f.a1 = (-2.0 * cosw0) / a0;
    f.a2 = (1.0 - alpha) / a0;
    return f;
}

std::vector<uint16_t> applyGraphEffectsToChannel(const std::vector<uint16_t>& raw, uint32_t sample_rate, const std::string& effects_csv) {
    auto out = raw;
    AudioFilters::applyEffectsToRawAdcMono(out, std::max<uint32_t>(1, sample_rate), splitEffectsCsv(effects_csv));
    return out;
}

std::vector<uint16_t> applyGraphEffectsToChannelLegacy(const std::vector<uint16_t>& raw, uint32_t sample_rate, const std::string& effects_csv) {
    if (raw.empty()) return {};
    std::vector<double> x;
    x.reserve(raw.size());
    for (uint16_t s : raw) x.push_back(static_cast<double>(s) - 2048.0);

    const auto effects = splitEffectsCsv(effects_csv);
    for (const auto& effect : effects) {
        if (effect == "dc_block") {
            double mean = 0.0;
            for (double v : x) mean += v;
            mean /= static_cast<double>(std::max<size_t>(1, x.size()));
            for (double& v : x) v -= mean;
        } else if (effect == "hum_notch_60" || effect == "hum_notch_120") {
            GraphBiquad notch = makeGraphNotch(std::max<double>(1.0, sample_rate), effect == "hum_notch_60" ? 60.0 : 120.0, 35.0);
            for (double& sample : x) sample = notch.process(sample);
        } else if (effect == "voice_agc") {
            double gain = 1.0;
            for (double& sample : x) {
                double desired = 9000.0 / std::max(1000.0, std::abs(sample * 16.0));
                desired = std::max(0.25, std::min(8.0, desired));
                const double coeff = desired < gain ? 0.10 : 0.003;
                gain += coeff * (desired - gain);
                sample *= gain;
            }
        } else if (effect == "pots_bandpass" || effect == "bell202_bandpass") {
            const double fs = std::max<double>(1.0, sample_rate);
            const double hp_cut = (effect == "bell202_bandpass") ? 700.0 : 300.0;
            const double lp_cut_req = (effect == "bell202_bandpass") ? 2700.0 : 3400.0;
            const int passes = (effect == "bell202_bandpass") ? 2 : 1;
            const double lp_cut = std::min(lp_cut_req, fs * 0.45);
            for (int pass = 0; pass < passes && lp_cut > hp_cut; ++pass) {
                const double hp_rc = 1.0 / (2.0 * M_PI * hp_cut);
                const double dt = 1.0 / fs;
                const double hp_alpha = hp_rc / (hp_rc + dt);
                const double lp_rc = 1.0 / (2.0 * M_PI * lp_cut);
                const double lp_alpha = dt / (lp_rc + dt);
                double hp_y = 0.0, hp_prev_x = 0.0, lp_y = 0.0;
                for (double& sample : x) {
                    const double in = sample;
                    const double yhp = hp_alpha * (hp_y + in - hp_prev_x);
                    hp_y = yhp;
                    hp_prev_x = in;
                    lp_y = lp_y + lp_alpha * (yhp - lp_y);
                    sample = lp_y;
                }
            }
        } else if (effect == "bell202_dual_tone") {
            GraphBiquad mark = makeGraphBandpass(std::max<double>(1.0, sample_rate), 1200.0, 8.0);
            GraphBiquad space = makeGraphBandpass(std::max<double>(1.0, sample_rate), 2200.0, 8.0);
            for (double& sample : x) sample = (mark.process(sample) + space.process(sample)) * 2.0;
        } else if (effect == "fsk_squelch") {
            if (x.size() >= 8) {
                const size_t win = std::max<size_t>(8, static_cast<size_t>(std::max<uint32_t>(1, sample_rate) / 200));
                std::vector<double> env(x.size(), 0.0);
                double sumsq = 0.0;
                for (size_t i = 0; i < x.size(); ++i) {
                    sumsq += x[i] * x[i];
                    if (i >= win) sumsq -= x[i - win] * x[i - win];
                    env[i] = std::sqrt(std::max(0.0, sumsq / static_cast<double>(std::min(win, i + 1))));
                }
                auto sorted = env;
                std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 3, sorted.end());
                const double floor = std::max(1.0, sorted[sorted.size() / 3]);
                const double open = floor * 2.2;
                const double full = floor * 5.0;
                for (size_t i = 0; i < x.size(); ++i) {
                    double gain = 0.18;
                    if (env[i] >= full) gain = 1.0;
                    else if (env[i] > open) gain = 0.18 + 0.82 * ((env[i] - open) / std::max(1.0, full - open));
                    x[i] *= gain;
                }
            }
        } else if (effect == "normalize") {
            double peak = 0.0;
            for (double v : x) peak = std::max(peak, std::abs(v));
            if (peak > 1.0) {
                const double target = 1800.0;
                const double gain = target / peak;
                if (gain > 1.01) for (double& v : x) v *= gain;
            }
        } else if (effect == "soft_clip") {
            constexpr double drive = 1.6;
            const double denom = std::tanh(drive);
            for (double& v : x) {
                const double xn = v / 2048.0;
                v = (std::tanh(xn * drive) / denom) * 2047.0;
            }
        } else if (effect == "rnnoise") {
            // RNNoise is intentionally skipped for the live graph. It is expensive
            // and speech-oriented; keep it available for WAV export only.
        }
    }

    std::vector<uint16_t> out;
    out.reserve(x.size());
    for (double v : x) out.push_back(clampAdc(2048.0 + v));
    return out;
}

json selectedCallerIdJson(const std::string& source, CallerIdDetector* cid) {
    auto software = [&]() -> json {
        if (!cid) return disabledCallerIdJson("software_adc", "Software ADC Caller ID detector disabled");
        json out = cid->snapshotJson();
        out["source"] = "software_adc";
        out["source_label"] = "Software ADC";
        return out;
    };

    if (source == "auto" || source == "software_adc") return software();
    return disabledCallerIdJson(source, "Unknown Caller ID source");
}
}

const char* HTML_UI = R"html(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>CM4 GPIO + ADC Monitor</title>
    <style>
        body { background-color: #121212; color: #e0e0e0; font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; margin: 0; padding: 20px; display: flex; flex-direction: column; align-items: center; }
        h1 { color: #ffffff; margin-bottom: 5px; }
        .subtitle { color: #888; margin-bottom: 30px; font-size: 0.9rem; }
        .config-panel { background: #1e1e1e; padding: 15px 25px; border-radius: 10px; margin-bottom: 20px; box-shadow: 0 4px 10px rgba(0,0,0,0.3); display: flex; align-items: center; gap: 15px; flex-wrap: wrap; }
        .config-panel input { background: #2a2a2a; border: 1px solid #444; color: #fff; padding: 8px; border-radius: 5px; width: 80px; text-align: center; }
        .config-panel input.wide { width: 110px; }
        .config-panel button { background: #00adb5; color: white; border: none; padding: 8px 15px; border-radius: 5px; cursor: pointer; transition: 0.2s; }
        .config-panel button:hover { background: #00f5ff; }
        .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(280px, 1fr)); gap: 20px; width: 100%; max-width: 1200px; }
        .card { background: #1e1e1e; border-radius: 12px; padding: 20px; box-shadow: 0 4px 15px rgba(0,0,0,0.5); border: 1px solid #2d2d2d; display: flex; flex-direction: column; position: relative; }
        .card-header { display: flex; justify-content: space-between; align-items: center; border-bottom: 1px solid #333; padding-bottom: 10px; margin-bottom: 15px; }
        .pin-title { font-size: 1.3rem; font-weight: bold; }
        .bcm-tag { font-size: 0.8rem; color: #888; background: #2a2a2a; padding: 2px 6px; border-radius: 4px; }
        .status-container { display: flex; align-items: center; justify-content: space-between; margin-bottom: 20px; }
        .bulb { width: 40px; height: 40px; border-radius: 50%; background-color: #333; transition: all 0.2s ease-in-out; }
        .bulb.high { background-color: #00ff87; box-shadow: 0 0 15px #00ff87, 0 0 30px #00ff87; }
        .bulb.low { background-color: #ff4141; box-shadow: 0 0 15px #ff4141, 0 0 30px #ff4141; }
        .metrics { font-size: 0.95rem; line-height: 1.6; }
        .metrics span { font-weight: bold; color: #00adb5; }
        .controls { margin-top: auto; display: flex; flex-direction: column; gap: 10px; border-top: 1px solid #333; padding-top: 15px; }
        .control-row { display: flex; justify-content: space-between; align-items: center; }
        select, button.action-btn { background: #2a2a2a; color: #fff; border: 1px solid #444; padding: 6px 12px; border-radius: 5px; cursor: pointer; }
        .record-panel { justify-content: flex-start; margin-top: 14px; margin-bottom: 0; padding: 12px 14px; }
        .record-panel select { min-width: 170px; }
        .effects-panel { align-items: flex-start; }
        .effects-list { display: flex; gap: 10px; flex-wrap: wrap; max-width: 760px; }
        .effect-item { display: inline-flex; align-items: center; gap: 5px; color: #d5d5d5; font-size: .86rem; background: #20272a; border: 1px solid #334; padding: 5px 8px; border-radius: 6px; }
        .effect-item.disabled { opacity: .48; }
        .effect-item input { width: auto; }
        .record-help { color: #9aa; font-size: .82rem; }
        .record-status { color: #8df7ff; font-size: .85rem; min-width: 160px; }
        .adc-config-panel input.wide { width: 360px; max-width: 80vw; text-align: left; }
        .adc-diag { margin-top: 10px; padding: 9px 11px; border-radius: 7px; background: #101719; border: 1px solid #263238; color: #b9d7dc; font-size: .86rem; line-height: 1.45; }
        .adc-diag.ok { border-color: rgba(0,255,135,.55); color: #d6fff0; }
        .adc-diag.warn { border-color: #f0a500; color: #ffe4a8; }
        .adc-diag.bad { border-color: #ff4141; color: #ffb7b7; }
        .rate-warn { color: #ffcf33; font-weight: bold; }
        button.action-btn.active-high { background: #00ff87; color: #000; font-weight: bold; }
        button.action-btn.active-low { background: #ff4141; color: #fff; font-weight: bold; }
        .io-badge { padding: 4px 8px; border-radius: 5px; font-size: 0.8rem; font-weight: bold; }
        .io-in { background: rgba(0, 173, 181, 0.2); color: #00adb5; border: 1px solid #00adb5; }
        .io-out { background: rgba(240, 165, 0, 0.2); color: #f0a500; border: 1px solid #f0a500; }
        .scope-card { width: 100%; max-width: 1200px; box-sizing: border-box; margin-bottom: 20px; }
        body.gpio-only .scope-card { display: none; }
        .scope-top { display: flex; gap: 18px; align-items: center; flex-wrap: wrap; color: #bdbdbd; font-size: 0.9rem; }
        .scope-top span b { color: #00f5ff; }
        .scope-wrap { position: relative; margin-top: 12px; background: #070b0d; border: 1px solid #263238; border-radius: 8px; overflow: hidden; }
        #adcScope { display: block; width: 100%; height: 220px; image-rendering: pixelated; }
        .status-pill { padding: 4px 9px; border-radius: 999px; font-size: 0.8rem; border: 1px solid #555; }
        .cid-card { width: 100%; max-width: 1200px; box-sizing: border-box; margin-bottom: 20px; }
        body.gpio-only .cid-card { display: none; }
        .cid-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(180px, 1fr)); gap: 10px 18px; margin-top: 10px; }
        .cid-item { color: #bdbdbd; font-size: .9rem; }
        .cid-item b { color: #8df7ff; display: block; margin-top: 2px; word-break: break-word; }
        .cid-raw { margin-top: 12px; background: #080b0c; border: 1px solid #263238; border-radius: 8px; padding: 10px; font-family: monospace; color: #d7f7ff; white-space: pre-wrap; overflow-wrap: anywhere; max-height: 150px; overflow-y: auto; }
        .cid-tune { margin-top: 12px; align-items: center; }
        .cid-tune input[type="number"] { width: 76px; }
        .cid-tune select { min-width: 120px; }
        .cid-tune label { color: #cfcfcf; font-size: .86rem; }
        .status-ok { color: #00ff87; background: rgba(0,255,135,.12); border-color: #00ff87; }
        .status-bad { color: #ff6b6b; background: rgba(255,65,65,.12); border-color: #ff4141; }
        .tiny { color: #777; font-size: .78rem; margin-top: 8px; }
        .tabs { display:flex; gap:8px; flex-wrap:wrap; width:100%; max-width:1200px; margin: 0 0 16px 0; }
        .tab-btn { background:#1e1e1e; color:#d8d8d8; border:1px solid #333; border-radius:8px; padding:10px 14px; cursor:pointer; }
        .tab-btn.active { background:#00adb5; color:#fff; border-color:#00f5ff; }
        .tab-panel { display:none; width:100%; max-width:1200px; }
        .tab-panel.active { display:block; }
        .dtmf-pad { display:grid; grid-template-columns: repeat(4, 56px); gap:8px; }
        .dtmf-pad button { padding:12px 0; font-size:1.05rem; font-weight:bold; border-radius:8px; background:#202a2d; border:1px solid #3d555b; color:#e9feff; cursor:pointer; }
        .dtmf-pad button:hover { background:#00adb5; color:#fff; }
        .dtmf-sequence { width: 300px !important; max-width: 80vw; text-align:left !important; font-family: monospace; }
    </style>
</head>
<body class="__BODY_CLASS__">
    <h1>CM4 GPIO + ADC Monitor Dashboard</h1>
    <div class="subtitle">Real-time physical header tracking, telephony control, and dual-channel ADC scope</div>
    <div class="config-panel">
        <label for="timeoutInput">Steady-State Timeout (ms):</label>
        <input type="number" id="timeoutInput" value="5000" oninput="lockTimeoutInput()" onfocus="lockTimeoutInput()">
        <button onclick="updateTimeout()">Apply</button>
    </div>

    <div class="tabs">
        <button class="tab-btn active" data-tab="scope" onclick="selectTab('scope')">Scope / Audio</button>
        <button class="tab-btn" data-tab="caller" onclick="selectTab('caller')">Caller ID</button>
        <button class="tab-btn" data-tab="telephony" onclick="selectTab('telephony')">Telephony</button>
        <button class="tab-btn" data-tab="dac" onclick="selectTab('dac')">DAC</button>
        <button class="tab-btn" data-tab="gpio" onclick="selectTab('gpio')">GPIO</button>
    </div>

    <div class="tab-panel active" id="tab-scope">
    <div class="card scope-card" id="adcScopeCard">
        <div class="card-header">
            <div>
                <span id="adcTitle" class="pin-title">Dual-Channel ADC Scope</span>
                <span id="adcTag" class="bcm-tag">CH0 + CH1, 12-bit @ 8 kHz frames</span>
            </div>
            <span class="status-pill status-bad" id="adcStatus">WAITING</span>
        </div>
        <div class="scope-top">
            <span>Mode: <b id="adcMode">-</b></span>
            <span>Frame Rate: <b id="adcRate">-</b></span>
            <span id="adcRateWarn" class="rate-warn"></span>
            <span style="color:#00ff87">CH0: <b id="adcLatest0">-</b></span>
            <span style="color:#ffcf33">CH1: <b id="adcLatest1">-</b></span>
            <span>Frames: <b id="adcSamples">-</b></span>
            <span>Dropped: <b id="adcDropped">-</b></span>
            <span>History: <b id="adcHistory">-</b></span>
            <span id="adcErr" style="color:#ff8a80"></span>
        </div>
        <div class="adc-diag" id="adcDiagnostics">ADC diagnostics will appear here.</div>
        <div class="config-panel record-panel adc-config-panel">
            <label>ADC source:
                <select id="adcConfigSource">
                    <option value="rp2040">RP2040 USB CDC</option>
                    <option value="mcp3202-spidev">MCP3202 Linux SPI</option>
                </select>
            </label>
            <label>RP2040 device:
                <input class="wide" type="text" id="adcConfigRp2040Dev" value="/dev/ttyACM0">
            </label>
            <label>Sample rate:
                <input type="number" id="adcConfigRate" min="1" max="100000" step="1000" list="adcRatePresets" value="16000">
                <datalist id="adcRatePresets"><option value="8000"><option value="16000"><option value="24000"></datalist>
            </label>
            <button onclick="applyAdcSettings()">Apply ADC Settings</button>
            <span class="record-status" id="adcConfigStatus"></span>
            <span class="record-help">Recommended RP2040 rate: 16000 Hz. Tested practical max: 24000 Hz; higher requested rates may not be achieved.</span>
        </div>
        <div class="scope-wrap"><canvas id="adcScope" width="1200" height="220"></canvas></div>
        <div class="config-panel record-panel">
            <label>Graph view:
                <select id="graphView">
                    <option value="raw">Raw ADC</option>
                    <option value="filtered">Filtered preview</option>
                    <option value="overlay">Raw + filtered overlay</option>
                </select>
            </label>
            <label><input type="checkbox" id="graphUseProfiles" checked> use saved CH0/CH1 graph profiles</label>
            <span class="record-help">Filtered/overlay graph can use persisted per-channel profiles, or uncheck this to use the temporary Effects override below.</span>
        </div>
        <div class="config-panel record-panel">
            <label for="wavDuration">WAV capture:</label>
            <input class="wide" type="number" id="wavDuration" min="1" max="600000" step="100" value="1000">
            <span>ms</span>
            <select id="wavMode">
                <option value="ch0">Mono CH0</option>
                <option value="ch1">Mono CH1</option>
                <option value="stereo">Stereo CH0 + CH1</option>
                <option value="mix">Mono mix CH0 + CH1</option>
            </select>
            <select id="audioCodec">
                <option value="pcm16">WAV PCM 16-bit</option>
            </select>
            <label><input type="checkbox" id="wavUseProfiles" checked> use saved WAV profile</label>
            <button onclick="downloadWav()">Download Audio</button>
            <button onclick="previewWav()">Preview Audio</button>
            <span class="record-status" id="wavStatus"></span>
            <span class="record-help">Downloads or previews the newest n ms already held in the ADC ring buffer as 16-bit PCM.</span>
            <audio id="audioPreview" controls style="width:100%; max-width:520px; display:none;"></audio>
        </div>
        <div class="config-panel record-panel effects-panel">
            <label>Temporary Effects Override:</label>
            <div class="effects-list" id="effectsList">Loading audio modules...</div>
        </div>
        <div class="config-panel record-panel effects-panel" id="filterProfileEditor">
            <div style="display:flex; flex-direction:column; gap:10px; min-width:260px;">
                <label>Saved filter profile:
                    <select id="filterProfileContext"></select>
                </label>
                <label><input type="checkbox" id="filterProfileEnabled" checked> enabled</label>
                <div style="display:flex; gap:8px; flex-wrap:wrap;">
                    <button onclick="applyFilterProfile()">Apply Profile</button>
                    <button onclick="reloadFilterProfiles(true)">Reload</button>
                    <button onclick="clearFilterProfile()">Clear</button>
                    <button onclick="restoreFilterProfileDefault()">Restore Default</button>
                </div>
                <span class="record-status" id="filterProfileStatus"></span>
                <span class="record-help">Server-persisted profiles are kept per channel/task. Active edits are guarded so status refreshes do not overwrite user changes.</span>
            </div>
            <div class="effects-list" id="filterProfileEffects">Loading saved profiles...</div>
        </div>
        <div class="tiny"><span style="color:#00ff87">CH0 is green</span>, <span style="color:#ffcf33">CH1 is amber</span>. The scope endpoint returns recent ring-buffer history with min/max decimation so audio peaks remain visible while the browser polls at UI speed.</div>
    </div>
    </div>

    <div class="tab-panel" id="tab-caller">
    <div class="card cid-card" id="callerIdCard">
        <div class="card-header">
            <div>
                <span class="pin-title">Caller ID FSK Detector</span>
                <span class="bcm-tag">Bell 202: 1200 Hz mark / 2200 Hz space @ 1200 baud</span>
            </div>
            <span class="status-pill status-bad" id="cidStatus">LISTENING</span>
        </div>
        <div class="cid-grid">
            <div class="cid-item">Number<b id="cidNumber">-</b></div>
            <div class="cid-item">Name<b id="cidName">-</b></div>
            <div class="cid-item">Date/Time<b id="cidDate">-</b></div>
            <div class="cid-item">Message Type<b id="cidType">-</b></div>
            <div class="cid-item">Checksum<b id="cidChecksum">-</b></div>
            <div class="cid-item">Current Confidence<b id="cidConfidence">-</b></div>
            <div class="cid-item">Best Confidence<b id="cidBestConfidence">-</b></div>
            <div class="cid-item">Channel<b id="cidChannel">-</b></div>
            <div class="cid-item">Best Channel<b id="cidBestChannel">-</b></div>
            <div class="cid-item">Last Update<b id="cidUpdate">-</b></div>
        </div>
        <div class="config-panel record-panel cid-tune">
            <label>FSK channel <select id="cidTuneChannel"><option value="0">CH0</option><option value="1">CH1</option><option value="2">Mix</option><option value="-1">Auto</option></select></label>
            <label>Mark <input type="number" id="cidTuneMark" value="1200" step="5"> Hz</label>
            <label>Space <input type="number" id="cidTuneSpace" value="2200" step="5"> Hz</label>
            <label>Baud <input type="number" id="cidTuneBaud" value="1200" step="1"></label>
            <label>Window <input type="number" id="cidTuneWindow" value="5000" step="250"> ms</label>
            <label><input type="checkbox" id="cidTuneNormalize" checked> normalize</label>
            <label>Headroom <input type="number" id="cidTuneHeadroom" value="6" step="1"> dB</label>
            <label>Extra gain <input type="number" id="cidTuneGain" value="12" step="1"> dB</label>
            <label><input type="checkbox" id="cidTuneDc" checked> DC block</label>
            <button onclick="applyCallerIdSettings()">Apply FSK Tune</button>
            <button onclick="reloadCallerIdSettings()">Reload Settings</button>
            <span class="record-status" id="cidTuneStatus"></span>
        </div>
        <div class="tiny" id="cidError"></div>
        <div class="cid-raw" id="cidRaw">Raw FSK bits/bytes will appear here when detected.</div>
        <div class="cid-raw" id="cidBestRaw">Best confidence FSK bits/bytes will appear here.</div>
    </div>
    </div>

    <div class="tab-panel" id="tab-telephony">
    <div class="card cid-card" id="telephonyUnifiedCard">
        <div class="card-header"><div><span class="pin-title">Unified Telephony State</span><span class="bcm-tag">CH1817 + line DSP + Caller ID</span></div><span class="status-pill" id="tcStatus">-</span></div>
        <div class="cid-grid">
            <div class="cid-item">State<b id="tcState">-</b></div><div class="cid-item">Hook<b id="tcHook">-</b></div><div class="cid-item">Safe to answer<b id="tcSafe">-</b></div><div class="cid-item">Ring count<b id="tcRings">-</b></div>
            <div class="cid-item">Auto-answer<b id="tcAuto">-</b></div><div class="cid-item">Auto-hangup<b id="tcHangup">-</b></div><div class="cid-item">Caller ID wait<b id="tcCidWait">-</b></div><div class="cid-item">Block reason<b id="tcBlock">-</b></div><div class="cid-item">Hangup reason<b id="tcHangupReason">-</b></div><div class="cid-item">Transition<b id="tcTransition">-</b></div>
        </div>
        <div class="config-panel record-panel cid-tune">
            <label><input type="checkbox" id="tcEnabled" checked> coordinator</label>
            <label><input type="checkbox" id="tcAutoAnswer"> auto-answer</label>
            <label><input type="checkbox" id="tcWaitCid" checked> wait CID</label>
            <label>Min rings <input type="number" id="tcMinRings" min="0" max="20" step="1" value="1"></label>
            <label>Delay <input type="number" id="tcDelay" min="0" max="600000" step="100" value="0"> ms</label>
            <label><input type="checkbox" id="tcAutoHangup" checked> auto-hangup</label>
            <label>Disconnect <input type="number" id="tcHangupDisconnect" min="100" max="600000" step="100" value="1500"> ms</label>
            <label>Warning <input type="number" id="tcHangupWarning" min="100" max="600000" step="100" value="500"> ms</label>
            <button onclick="applyTelephonyCoordinatorSettings()">Apply Coordinator</button><span class="record-status" id="tcApplyStatus"></span>
        </div>
        <div class="cid-raw" id="tcEvents">Coordinator events will appear here.</div>
        <div class="tiny" id="tcHelp"></div>
    </div>
    <div class="card cid-card" id="telephonyCard">
        <div class="card-header"><div><span class="pin-title">CH1817 DAA</span><span class="bcm-tag">OFFHK + RI</span></div><span class="status-pill" id="chStatus">-</span></div>
        <div class="cid-grid">
            <div class="cid-item">RI level<b id="chRiLevel">-</b></div><div class="cid-item">Ringing<b id="chRinging">-</b></div><div class="cid-item">RI frequency<b id="chFreq">-</b></div><div class="cid-item">Hook state<b id="chHook">-</b></div>
        </div>
        <div class="config-panel record-panel cid-tune">
            <button onclick="setChOffhook(false)">Go On-Hook</button><button onclick="setChOffhook(true)">Go Off-Hook</button>
            <label><input type="checkbox" id="chAutoAnswer"> auto-answer</label><label>Delay <input type="number" id="chAutoDelay" min="0" max="600000" step="100" value="0"> ms</label><button onclick="applyCh1817Settings()">Apply CH1817</button><span class="record-status" id="chApplyStatus"></span>
        </div>
        <div class="tiny" id="chHelp"></div>
    </div>
    <div class="card cid-card" id="lineStateCard">
        <div class="card-header"><div><span class="pin-title">Line State Detector</span><span class="bcm-tag">RCV/CH0 DSP + RI corroboration</span></div><span class="status-pill" id="lsStatus">-</span></div>
        <div class="cid-grid">
            <div class="cid-item">State<b id="lsState">-</b></div><div class="cid-item">Confidence<b id="lsConfidence">-</b></div><div class="cid-item">RMS<b id="lsRms">-</b></div><div class="cid-item">Peak<b id="lsPeak">-</b></div>
            <div class="cid-item">Zero-cross estimate<b id="lsZcr">-</b></div><div class="cid-item">Best tone<b id="lsTone">-</b></div><div class="cid-item">Region<b id="lsRegion">-</b></div><div class="cid-item">Window<b id="lsWindow">-</b></div>
        </div>
        <div class="config-panel record-panel cid-tune">
            <label>Region <select id="lsRegionSetting"><option value="nanp">NANP / North America</option></select></label>
            <label>Window <input type="number" id="lsWindowSetting" min="20" max="2000" step="10" value="100"> ms</label>
            <label>Min RMS <input type="number" id="lsMinRms" min="0" max="1" step="0.0005" value="0.002"></label>
            <label>Silence RMS <input type="number" id="lsSilenceRms" min="0" max="1" step="0.0001" value="0.0008"></label>
            <label><input type="checkbox" id="lsUseRi" checked> use RI</label>
            <button onclick="applyLineStateSettings()">Apply Line State</button><span class="record-status" id="lsApplyStatus"></span>
        </div>
        <div class="cid-raw" id="lsTones">Tone diagnostics will appear here.</div>
        <div class="tiny" id="lsHelp"></div>
    </div>
    <div class="card cid-card" id="telephonyDiagnosticsCard">
        <div class="card-header"><div><span class="pin-title">Calibration / Hardware Diagnostics</span><span class="bcm-tag">RCV noise + tone scan + safety warnings</span></div><span class="status-pill" id="tdStatus">-</span></div>
        <div class="cid-grid">
            <div class="cid-item">ADC health<b id="tdAdc">-</b></div><div class="cid-item">RCV RMS<b id="tdRms">-</b></div><div class="cid-item">RCV peak<b id="tdPeak">-</b></div><div class="cid-item">Warnings<b id="tdWarnCount">-</b></div>
            <div class="cid-item">RI mode<b id="tdRiMode">-</b></div><div class="cid-item">Silence RMS rec<b id="tdSilenceRec">-</b></div><div class="cid-item">Min RMS rec<b id="tdMinRec">-</b></div><div class="cid-item">Tone threshold<b id="tdToneThresh">-</b></div>
        </div>
        <div class="config-panel record-panel cid-tune">
            <button onclick="runRcvCalibration()">Measure Idle Noise</button>
            <button onclick="runToneScan()">Scan Current Tone</button>
            <button onclick="captureDisconnectProfile()">Capture Disconnect Profile</button>
            <button onclick="applyRecommendedThresholds()">Apply Thresholds</button>
            <button onclick="exportDiagnostics()">Export Diagnostics</button>
            <label>RI mode <select id="tdRiModeSetting"><option value="auto">auto</option><option value="pulsed">pulsed</option><option value="envelope">envelope</option></select></label>
            <button onclick="applyCalibrationSettings()">Apply Calibration</button><span class="record-status" id="tdApplyStatus"></span>
        </div>
        <div class="cid-raw" id="tdOutput">Diagnostics will appear here.</div>
        <div class="tiny" id="tdHelp"></div>
    </div>
    </div>


    <div class="tab-panel" id="tab-dac">
    <div class="card cid-card" id="dacCard">
        <div class="card-header"><div><span class="pin-title">MCP4922 DAC Output</span><span class="bcm-tag">native SPI or RP2040/GWP1</span></div><span class="status-pill" id="dacStatusPill">-</span></div>
        <div class="cid-grid">
            <div class="cid-item">Enabled<b id="dacEnabled">-</b></div>
            <div class="cid-item">Transport<b id="dacTransportStatus">-</b></div>
            <div class="cid-item">Active<b id="dacActive">-</b></div>
            <div class="cid-item">Raw A/B<b id="dacRaw">-</b></div>
            <div class="cid-item">Volts A/B<b id="dacVolts">-</b></div>
            <div class="cid-item">Frames Written<b id="dacFrames">-</b></div>
            <div class="cid-item">Errors<b id="dacErrors">-</b></div>
            <div class="cid-item">Last Error<b id="dacLastError">-</b></div>
        </div>
        <div class="config-panel record-panel">
            <label><input type="checkbox" id="dacConfigEnabled"> enabled</label>
            <label>Transport <select id="dacConfigTransport"><option value="native">Native Pi SPI</option><option value="rp2040">RP2040 USB/GWP1</option></select></label>
            <label>RP2040 dev <input class="wide" type="text" id="dacConfigRp2040Dev" value="/dev/ttyACM0"></label>
            <label>Rate <input type="number" id="dacConfigRate" min="1" max="1000000" value="48000"></label>
            <button onclick="applyDacConfig()">Apply DAC Config</button>
            <button onclick="dacControl('start')">Start</button>
            <button onclick="dacControl('stop')">Stop</button>
            <button onclick="dacControl('flush')">Flush/Zero</button>
            <span class="record-status" id="dacConfigStatus"></span>
        </div>
        <div class="config-panel record-panel">
            <label>Raw A <input type="number" id="dacRawA" min="0" max="4095" value="0"></label>
            <label>Raw B <input type="number" id="dacRawB" min="0" max="4095" value="0"></label>
            <button onclick="writeDacRaw()">Write Raw</button>
            <label>Volts A <input type="number" id="dacVoltsA" min="0" step="0.001" value="0"></label>
            <label>Volts B <input type="number" id="dacVoltsB" min="0" step="0.001" value="0"></label>
            <button onclick="writeDacVolts()">Write Volts</button>
            <span class="record-status" id="dacWriteStatus"></span>
        </div>
        <div class="config-panel record-panel effects-panel">
            <div style="display:flex; flex-direction:column; gap:10px; min-width:320px;">
                <div class="pin-title" style="font-size:1.05rem;">Uploaded File Playback</div>
                <label>Upload WAV <input type="file" id="playbackFile" accept="audio/wav,.wav"></label>
                <div style="display:flex; gap:8px; flex-wrap:wrap;">
                    <button onclick="uploadPlaybackFile()">Upload</button>
                    <select id="playbackUploadSelect" style="min-width:220px;"></select>
                    <button onclick="deletePlaybackUpload()">Delete</button>
                </div>
                <div style="display:flex; gap:10px; flex-wrap:wrap;">
                    <label>Output <select id="playbackChannel"><option value="ch0">CH0/A</option><option value="ch1">CH1/B</option><option value="mono_both">CH0+CH1 mono</option><option value="stereo">CH0+CH1 stereo</option></select></label>
                    <label><input type="checkbox" id="playbackUseProfiles" checked> use DAC playback profiles</label>
                    <label>Gain <input type="number" id="playbackGain" min="0" max="4" step="0.05" value="1.0"></label>
                    <label><input type="checkbox" id="playbackLoop"> loop</label>
                </div>
                <div style="display:flex; gap:8px; flex-wrap:wrap;">
                    <button onclick="playUploadedAudio()">Play Upload</button>
                    <button onclick="stopUploadedAudio()">Stop Playback</button>
                    <span class="record-status" id="playbackStatus">Idle</span>
                </div>
                <span class="record-help">Supports PCM16 WAV. Silence on inactive DAC channels is written as midscale 2048. Saved DAC playback filter profiles are applied when enabled.</span>
            </div>
        </div>
        <div class="config-panel record-panel effects-panel">
            <div>
                <div class="pin-title" style="font-size:1.05rem;margin-bottom:8px;">Advanced DTMF Dialer</div>
                <div class="dtmf-pad" id="dtmfPad">
                    <button data-dtmf="1">1</button><button data-dtmf="2">2</button><button data-dtmf="3">3</button><button data-dtmf="A">A</button>
                    <button data-dtmf="4">4</button><button data-dtmf="5">5</button><button data-dtmf="6">6</button><button data-dtmf="B">B</button>
                    <button data-dtmf="7">7</button><button data-dtmf="8">8</button><button data-dtmf="9">9</button><button data-dtmf="C">C</button>
                    <button data-dtmf="*">*</button><button data-dtmf="0">0</button><button data-dtmf="#">#</button><button data-dtmf="D">D</button>
                </div>
            </div>
            <div style="display:flex; flex-direction:column; gap:10px; min-width:320px;">
                <label>Sequence <input class="dtmf-sequence" type="text" id="dtmfSequence" value="" placeholder="18005551212"></label>
                <div style="display:flex; gap:10px; flex-wrap:wrap;">
                    <label>Tone <input type="number" id="dtmfToneMs" min="40" max="2000" step="10" value="100"> ms</label>
                    <label>Gap <input type="number" id="dtmfGapMs" min="0" max="2000" step="10" value="50"> ms</label>
                    <label>Amplitude <input type="number" id="dtmfAmplitude" min="1" max="2047" step="10" value="1200"></label>
                    <label>Output <select id="dtmfChannel"><option value="1">CH0/A</option><option value="2">CH1/B</option><option value="3">CH0+CH1</option></select></label>
                </div>
                <div style="display:flex; gap:8px; flex-wrap:wrap;">
                    <button onclick="playDtmf()">Dial Sequence</button>
                    <button onclick="stopDtmf()">Stop</button>
                    <button onclick="clearDtmf()">Clear</button>
                    <button onclick="backspaceDtmf()">Backspace</button>
                    <span class="record-status" id="dtmfStatus"></span>
                </div>
                <span class="record-help">DTMF tones are synthesized on the RP2040 firmware using fixed 8 kHz DTMF synthesis with midpoint-biased silence and click-reducing ramps; this UI preserves unsaved edits while status polling runs.</span>
            </div>
        </div>
        <div class="config-panel record-panel effects-panel">
            <div style="display:flex; flex-direction:column; gap:10px; min-width:360px; flex:1;">
                <div class="pin-title" style="font-size:1.05rem;">MT8870 DTMF Decoder / MCU Peripheral Validation</div>
                <div class="stat-row"><b>Decoder:</b> <span id="dtmfDecEnabled">-</span> <b>Source:</b> <span id="dtmfDecSource">-</span> <b>Active/StQ:</b> <span id="dtmfDecActive">-</span> <b>Current:</b> <span id="dtmfDecDigit">-</span></div>
                <div class="stat-row">Raw pins: StQ=<span id="mtStqRaw">-</span> Q1=<span id="mtQ1Raw">-</span> Q2=<span id="mtQ2Raw">-</span> Q3=<span id="mtQ3Raw">-</span> Q4=<span id="mtQ4Raw">-</span> bits=<span id="mtBits">----</span> seq=<span id="mtSeq">0</span></div>
                <div class="stat-row">RI source=<span id="mcuRiSource">-</span> raw=<span id="mcuRiRaw">-</span> logical=<span id="mcuRiLogical">-</span> transitions=<span id="mcuRiTransitions">0</span> · OH source=<span id="mcuOhSource">-</span> raw=<span id="mcuOhRaw">-</span> logical=<span id="mcuOhLogical">-</span> transitions=<span id="mcuOhTransitions">0</span></div>
                <span class="record-help">The MT8870 analog DTMF input is not assumed by software. Generate validation tones from any properly conditioned source; DAC CH1 is only bench context if you wire it that way.</span>
                <div style="display:flex; gap:8px; flex-wrap:wrap; align-items:center;">
                    <label>Expected sequence <input class="dtmf-sequence" type="text" id="dtmfValidationExpected" placeholder="1234567890*#ABCD"></label>
                    <button onclick="startDtmfValidation()">Start Validation</button>
                    <button onclick="clearDtmfDecoderHistory()">Clear History</button>
                    <button onclick="copyDtmfDecoderHistory()">Copy History</button>
                    <span class="record-status" id="dtmfDecoderStatus">Idle</span>
                </div>
                <div style="max-height:220px; overflow:auto; border:1px solid #294247; border-radius:8px;">
                    <table style="width:100%; border-collapse:collapse; font-size:0.88rem;">
                        <thead><tr><th>Seq</th><th>Digit</th><th>Raw bits</th><th>Expected</th><th>Match</th><th>Source</th></tr></thead>
                        <tbody id="dtmfDecoderHistory"><tr><td colspan="6">No decoded tones yet.</td></tr></tbody>
                    </table>
                </div>
                <div id="dtmfMappingHelp" class="record-help"></div>
            </div>
        </div>
        <div class="tiny">MCP4922 is a dual 12-bit voltage-output DAC. Writes use 16 SPI clocks: four command bits followed by 12 data bits; SPI is unidirectional and supports modes 0,0 and 1,1. VOUT = VREF * code / 4096 * gain.</div>
    </div>
    </div>

    <div class="tab-panel" id="tab-gpio">
    <div class="grid" id="pinGrid"></div>
    </div>
    <script>
        let timeoutInputLockExpiration = 0;

        function lockTimeoutInput() {
            timeoutInputLockExpiration = Date.now() + 5000;
        }

        function selectTab(name) {
            document.querySelectorAll('.tab-panel').forEach(p => p.classList.remove('active'));
            document.querySelectorAll('.tab-btn').forEach(b => b.classList.remove('active'));
            const panel = document.getElementById(`tab-${name}`);
            const btn = document.querySelector(`.tab-btn[data-tab="${name}"]`);
            if (panel) panel.classList.add('active');
            if (btn) btn.classList.add('active');
            saveUiPref('activeTab', name);
            if (name === 'scope') fetchAdcScope();
        }

        function ensurePinCard(pinId, pin) {
            const grid = document.getElementById('pinGrid');
            let card = document.getElementById(`pin-card-${pinId}`);

            if (card) return card;

            card = document.createElement('div');
            card.id = `pin-card-${pinId}`;
            card.className = 'card';
            card.innerHTML = `
                <div class="card-header">
                    <div>
                        <span class="pin-title">Physical Pin ${pin.physical_pin}</span>
                        <span class="bcm-tag">BCM ${pin.bcm_pin}</span>
                    </div>
                    <span class="io-badge" id="io-badge-${pinId}"></span>
                </div>
                <div class="status-container">
                    <div class="bulb" id="bulb-${pinId}"></div>
                    <div class="metrics">
                        <div>State: <span id="state-${pinId}"></span></div>
                        <div>Transitioning: <span id="transitioning-${pinId}"></span></div>
                        <div>Frequency: <span id="frequency-${pinId}"></span></div>
                    </div>
                </div>
                <div class="controls">
                    <div class="control-row">
                        <label>Direction Mode:</label>
                        <select id="mode-${pinId}" onchange="updateConfig(${pinId}, 'mode', this.value)">
                            <option value="in">Read-Only (Input)</option>
                            <option value="out">Output Override</option>
                        </select>
                    </div>
                    <div id="output-row-${pinId}"></div>
                </div>`;

            grid.appendChild(card);
            return card;
        }

        async function fetchStatus() {
            try {
                const res = await fetch('/api/status');
                const data = await res.json();

                const timeoutInput = document.getElementById('timeoutInput');
                if (Date.now() > timeoutInputLockExpiration && document.activeElement !== timeoutInput) {
                    timeoutInput.value = data.timeout_ms;
                }
                updateDacUi(data);
                updateMcuPeripheralUi(data);

                for (const pinId in data.pins) {
                    const pin = data.pins[pinId];
                    ensurePinCard(pinId, pin);

                    const badge = document.getElementById(`io-badge-${pinId}`);
                    badge.textContent = pin.is_output ? 'OUTPUT' : 'INPUT';
                    badge.className = `io-badge ${pin.is_output ? 'io-out' : 'io-in'}`;

                    const bulb = document.getElementById(`bulb-${pinId}`);
                    bulb.className = `bulb ${pin.state ? 'high' : 'low'}`;

                    document.getElementById(`state-${pinId}`).textContent = pin.state ? 'HIGH (1)' : 'LOW (0)';
                    document.getElementById(`transitioning-${pinId}`).textContent = pin.is_transitioning ? 'YES' : 'NO';
                    document.getElementById(`frequency-${pinId}`).textContent = `${pin.frequency.toFixed(2)} Hz`;

                    const modeSelect = document.getElementById(`mode-${pinId}`);
                    if (document.activeElement !== modeSelect) {
                        modeSelect.value = pin.is_output ? 'out' : 'in';
                    }

                    const outputRow = document.getElementById(`output-row-${pinId}`);
                    const desiredOutputHtml = pin.is_output ? `
                        <div class="control-row">
                            <label>Set Output State:</label>
                            <button class="action-btn ${pin.state ? 'active-high' : 'active-low'}"
                                    onclick="updateConfig(${pinId}, 'state', ${pin.state ? 'false' : 'true'})">
                                Drive ${pin.state ? 'LOW' : 'HIGH'}
                            </button>
                        </div>` : '';

                    if (outputRow.dataset.lastHtml !== desiredOutputHtml) {
                        outputRow.innerHTML = desiredOutputHtml;
                        outputRow.dataset.lastHtml = desiredOutputHtml;
                    }
                }
            } catch (err) {
                console.error("Error updates:", err);
            }
        }

        function channelLabel(ch) {
            return ch === 0 ? 'CH0' : (ch === 1 ? 'CH1' : (ch === 2 ? 'CH0+CH1 mix' : (ch === -1 ? 'Auto' : '-')));
        }

        function makeSettingsGuard(ids, statusId) {
            let dirty = false;
            let applying = false;
            function statusEl() { return statusId ? document.getElementById(statusId) : null; }
            function setStatus(text) { const el = statusEl(); if (el) el.textContent = text; }
            function markDirty() { if (applying) return; dirty = true; setStatus('Unsaved changes'); }
            function focused() { const activeId = document.activeElement && document.activeElement.id; return ids.includes(activeId); }
            function shouldFill(force=false) { return !!force || (!dirty && !focused() && !applying); }
            function beginApply() { applying = true; setStatus('Applying...'); }
            function finishApply(text='Applied') { applying = false; dirty = false; setStatus(text); }
            function failApply(text) { applying = false; setStatus(text || 'Failed'); }
            function clearDirty(text='') { dirty = false; applying = false; setStatus(text); }
            function wire() {
                ids.forEach(id => {
                    const el = document.getElementById(id);
                    if (el) { el.addEventListener('input', markDirty); el.addEventListener('change', markDirty); }
                });
            }
            return { markDirty, focused, shouldFill, beginApply, finishApply, failApply, clearDirty, wire };
        }

        const chGuard = makeSettingsGuard(['chAutoAnswer','chAutoDelay'], 'chApplyStatus');
        const tcGuard = makeSettingsGuard(['tcEnabled','tcAutoAnswer','tcWaitCid','tcMinRings','tcDelay','tcAutoHangup','tcHangupDisconnect','tcHangupWarning'], 'tcApplyStatus');
        const lsGuard = makeSettingsGuard(['lsRegionSetting','lsWindowSetting','lsMinRms','lsSilenceRms','lsUseRi'], 'lsApplyStatus');
        const tdGuard = makeSettingsGuard(['tdRiModeSetting'], 'tdApplyStatus');
        function wireSettingsGuards() { chGuard.wire(); tcGuard.wire(); lsGuard.wire(); tdGuard.wire(); window.__settingsGuards = { chGuard, tcGuard, lsGuard, tdGuard }; }

        function fillCh1817Settings(settings, force=false) {
            if (!settings || !chGuard.shouldFill(force)) return;
            document.getElementById('chAutoAnswer').checked = settings.auto_answer_enabled || false;
            document.getElementById('chAutoDelay').value = settings.auto_answer_delay_ms ?? 0;
            if (force) chGuard.clearDirty('');
        }

        function fillTelephonyCoordinatorSettings(settings, force=false) {
            if (!settings || !tcGuard.shouldFill(force)) return;
            document.getElementById('tcEnabled').checked = settings.enabled !== false;
            document.getElementById('tcAutoAnswer').checked = settings.auto_answer_enabled || false;
            document.getElementById('tcWaitCid').checked = settings.caller_id_before_auto_answer !== false;
            document.getElementById('tcMinRings').value = settings.min_rings_before_answer ?? 1;
            document.getElementById('tcDelay').value = settings.auto_answer_delay_ms ?? 0;
            document.getElementById('tcAutoHangup').checked = settings.auto_hangup_enabled !== false;
            document.getElementById('tcHangupDisconnect').value = settings.auto_hangup_after_disconnect_ms ?? 1500;
            document.getElementById('tcHangupWarning').value = settings.auto_hangup_after_warning_ms ?? 500;
            if (force) tcGuard.clearDirty('');
        }

        function fillLineStateSettings(settings, force=false) {
            if (!settings || !lsGuard.shouldFill(force)) return;
            document.getElementById('lsRegionSetting').value = settings.region || 'nanp';
            document.getElementById('lsWindowSetting').value = settings.analysis_window_ms ?? 100;
            document.getElementById('lsMinRms').value = settings.min_rms ?? 0.002;
            document.getElementById('lsSilenceRms').value = settings.silence_rms ?? 0.0008;
            document.getElementById('lsUseRi').checked = settings.use_ri_corroboration !== false;
            if (force) lsGuard.clearDirty('');
        }

        function fillCalibrationSettings(settings, force=false) {
            if (!settings || !tdGuard.shouldFill(force)) return;
            document.getElementById('tdRiModeSetting').value = settings.ri_mode || 'auto';
            if (force) tdGuard.clearDirty('');
        }

        let cidTuneDirty = false;
        function markCidTuneDirty() { cidTuneDirty = true; document.getElementById('cidTuneStatus').textContent = 'Unsaved changes'; }
        function cidTuneFocused() { return ['cidTuneChannel','cidTuneMark','cidTuneSpace','cidTuneBaud','cidTuneWindow','cidTuneNormalize','cidTuneHeadroom','cidTuneGain','cidTuneDc'].includes(document.activeElement && document.activeElement.id); }
        function fillCallerIdSettings(settings, force=false) {
            if (!settings || (!force && (cidTuneDirty || cidTuneFocused()))) return;
            document.getElementById('cidTuneChannel').value = settings.channel ?? 0;
            document.getElementById('cidTuneMark').value = settings.mark_hz ?? 1200;
            document.getElementById('cidTuneSpace').value = settings.space_hz ?? 2200;
            document.getElementById('cidTuneBaud').value = settings.baud ?? 1200;
            document.getElementById('cidTuneWindow').value = settings.analysis_ms ?? 5000;
            document.getElementById('cidTuneNormalize').checked = settings.normalize !== false;
            document.getElementById('cidTuneHeadroom').value = settings.normalize_headroom_db ?? 6;
            document.getElementById('cidTuneGain').value = settings.extra_gain_db ?? 12;
            document.getElementById('cidTuneDc').checked = settings.dc_block !== false;
            cidTuneDirty = false;
        }

        function wireCallerIdTuneDirty() {
            ['cidTuneChannel','cidTuneMark','cidTuneSpace','cidTuneBaud','cidTuneWindow','cidTuneNormalize','cidTuneHeadroom','cidTuneGain','cidTuneDc'].forEach(id => {
                const el = document.getElementById(id); if (el) { el.addEventListener('input', markCidTuneDirty); el.addEventListener('change', markCidTuneDirty); }
            });
            const graphView = document.getElementById('graphView');
            if (graphView) graphView.addEventListener('change', fetchAdcScope);
        }

        async function reloadCallerIdSettings() {
            const status = document.getElementById('cidTuneStatus'); status.textContent = 'Reloading...';
            try { const res = await fetch('/api/caller-id/settings'); const data = await res.json(); if(!res.ok) throw new Error(data.error || `HTTP ${res.status}`); fillCallerIdSettings(data, true); status.textContent = 'Reloaded'; }
            catch(err) { status.textContent = `Reload failed: ${err.message || err}`; }
        }

        async function applyCallerIdSettings() {
            cidTuneDirty = true;
            const settings = {
                channel: parseInt(document.getElementById('cidTuneChannel').value, 10),
                mark_hz: parseFloat(document.getElementById('cidTuneMark').value),
                space_hz: parseFloat(document.getElementById('cidTuneSpace').value),
                baud: parseFloat(document.getElementById('cidTuneBaud').value),
                analysis_ms: parseInt(document.getElementById('cidTuneWindow').value, 10),
                normalize: document.getElementById('cidTuneNormalize').checked,
                normalize_headroom_db: parseFloat(document.getElementById('cidTuneHeadroom').value),
                extra_gain_db: parseFloat(document.getElementById('cidTuneGain').value),
                dc_block: document.getElementById('cidTuneDc').checked
            };
            const status = document.getElementById('cidTuneStatus');
            status.textContent = 'Applying...';
            try {
                const res = await fetch('/api/caller-id/settings', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify(settings)
                });
                const data = await res.json();
                if (!res.ok) throw new Error(data.error || `HTTP ${res.status}`);
                status.textContent = 'Applied';
                cidTuneDirty = false;
                fillCallerIdSettings(data.settings || data, true);
                fetchCallerId();
            } catch (err) {
                status.textContent = `Failed: ${err.message || err}`;
            }
        }

        async function fetchCallerId() {
            try {
                const res = await fetch('/api/caller-id');
                const data = await res.json();
                const status = document.getElementById('cidStatus');
                status.textContent = data.detected ? (data.checksum_ok ? 'DECODED' : 'DECODED?') : (data.status || 'LISTENING').toUpperCase();
                status.className = `status-pill ${data.detected ? 'status-ok' : 'status-bad'}`;
                document.getElementById('cidNumber').textContent = data.number || '-';
                document.getElementById('cidName').textContent = data.name || '-';
                document.getElementById('cidDate').textContent = data.date_time || data.date_time_raw || '-';
                document.getElementById('cidType').textContent = data.message_type || '-';
                document.getElementById('cidChecksum').textContent = data.detected ? (data.checksum_ok ? 'OK' : 'BAD/unknown') : '-';
                document.getElementById('cidConfidence').textContent = data.confidence ? data.confidence.toFixed(3) : '-';
                document.getElementById('cidBestConfidence').textContent = data.best_confidence ? data.best_confidence.toFixed(3) : '-';
                const ch = data.selected_channel;
                document.getElementById('cidChannel').textContent = channelLabel(ch);
                document.getElementById('cidBestChannel').textContent = channelLabel(data.best_selected_channel);
                document.getElementById('cidUpdate').textContent = data.last_update || '-';
                document.getElementById('cidError').textContent = data.last_error || data.status || '';
                document.getElementById('cidRaw').textContent = `Current raw bytes: ${data.raw_bytes_hex || '-'}\nCurrent raw bits: ${data.raw_bits || '-'}`;
                document.getElementById('cidBestRaw').textContent = `Best status: ${data.best_status || '-'}\nBest raw bytes: ${data.best_raw_bytes_hex || '-'}\nBest raw bits: ${data.best_raw_bits || '-'}`;
                fillCallerIdSettings(data.settings);
            } catch (err) {
                document.getElementById('cidError').textContent = `Caller ID error: ${err}`;
            }
        }

        async function fetchTelephony() {
            try {
                const res = await fetch('/api/telephony/ch1817'); const data = await res.json();
                const st = document.getElementById('chStatus'); st.textContent = data.ringing ? 'RINGING' : (data.offhook ? 'OFF-HOOK' : 'IDLE'); st.className = `status-pill ${data.ringing ? 'status-ok' : 'status-bad'}`;
                document.getElementById('chRiLevel').textContent = data.ri_level_text || '-'; document.getElementById('chRinging').textContent = data.ringing ? 'YES' : 'NO'; document.getElementById('chFreq').textContent = `${(data.ri_frequency_hz || 0).toFixed(2)} Hz`; document.getElementById('chHook').textContent = data.offhook ? 'off-hook' : 'on-hook';
                fillCh1817Settings(data.settings);
                document.getElementById('chHelp').textContent = data.last_error || data.help || '';
            } catch(err) { document.getElementById('chHelp').textContent = `CH1817 error: ${err.message || err}`; }
        }

        let lastCalibrationRecommendations = null;
        async function fetchTelephonyDiagnostics() {
            try {
                const res = await fetch('/api/telephony/hardware-check'); const data = await res.json();
                const warnings = data.warnings || [];
                document.getElementById('tdStatus').textContent = warnings.length ? 'WARN' : 'OK';
                document.getElementById('tdStatus').className = `status-pill ${warnings.length ? 'status-bad' : 'status-ok'}`;
                document.getElementById('tdAdc').textContent = data.adc?.healthy ? 'healthy' : 'unhealthy';
                document.getElementById('tdRms').textContent = (data.line_state?.rms || 0).toFixed(6);
                document.getElementById('tdPeak').textContent = (data.line_state?.peak || 0).toFixed(6);
                document.getElementById('tdWarnCount').textContent = warnings.length;
                document.getElementById('tdRiMode').textContent = data.calibration?.ri_mode || 'auto';
                document.getElementById('tdToneThresh').textContent = data.line_state?.settings?.tone_detect_threshold ?? '-';
                fillCalibrationSettings(data.calibration);
                document.getElementById('tdOutput').textContent = `Warnings:\n${warnings.join('\n') || 'none'}\n\nValidation reminders available at /api/telephony/validation-checklist`;
            } catch(err) { const el=document.getElementById('tdHelp'); if(el) el.textContent = `Diagnostics error: ${err.message || err}`; }
        }
        async function runRcvCalibration() {
            const st=document.getElementById('tdApplyStatus'); st.textContent='Measuring...';
            try { const res=await fetch('/api/telephony/calibration/rcv-noise-floor',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({duration_ms:3000,window_ms:100})}); const data=await res.json(); if(!res.ok || data.status==='error') throw new Error(data.error||'failed'); lastCalibrationRecommendations=data.recommendations; document.getElementById('tdSilenceRec').textContent=Number(data.recommendations.recommended_silence_rms||0).toFixed(6); document.getElementById('tdMinRec').textContent=Number(data.recommendations.recommended_min_rms||0).toFixed(6); document.getElementById('tdOutput').textContent=JSON.stringify(data,null,2); st.textContent='Measured'; }
            catch(err){ st.textContent=`Failed: ${err.message||err}`; }
        }
        async function runToneScan() {
            const st=document.getElementById('tdApplyStatus'); st.textContent='Scanning...';
            try { const res=await fetch('/api/telephony/calibration/tone-scan',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({duration_ms:1000,region:'nanp'})}); const data=await res.json(); if(!res.ok || data.status==='error') throw new Error(data.error||'failed'); document.getElementById('tdOutput').textContent=JSON.stringify(data,null,2); st.textContent='Scanned'; }
            catch(err){ st.textContent=`Failed: ${err.message||err}`; }
        }
        async function captureDisconnectProfile() {
            const st=document.getElementById('tdApplyStatus'); st.textContent='Capturing disconnect profile...';
            try { const res=await fetch('/api/telephony/calibration/capture-disconnect-profile',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({duration_ms:3000,label:'ata_custom'})}); const data=await res.json(); if(!res.ok || data.status==='error') throw new Error(data.error||'failed'); document.getElementById('tdOutput').textContent=JSON.stringify(data,null,2); st.textContent='Captured'; }
            catch(err){ st.textContent=`Failed: ${err.message||err}`; }
        }
        async function applyRecommendedThresholds() {
            if (!lastCalibrationRecommendations) { alert('Run idle noise calibration first.'); return; }
            const st=document.getElementById('tdApplyStatus'); st.textContent='Applying thresholds...';
            try { const res=await fetch('/api/telephony/calibration/apply-line-thresholds',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({silence_rms:lastCalibrationRecommendations.recommended_silence_rms,min_rms:lastCalibrationRecommendations.recommended_min_rms})}); const data=await res.json(); if(!res.ok) throw new Error(data.error||'failed'); st.textContent='Thresholds applied'; if (data.line_state_settings) { lsGuard.clearDirty(''); fillLineStateSettings(data.line_state_settings, true); } fetchLineState(); }
            catch(err){ st.textContent=`Failed: ${err.message||err}`; }
        }
        async function applyCalibrationSettings() {
            const body = { ri_mode: document.getElementById('tdRiModeSetting').value };
            tdGuard.beginApply();
            try { const res=await fetch('/api/telephony/calibration/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)}); const data=await res.json(); if(!res.ok) throw new Error(data.error||'failed'); tdGuard.finishApply('Calibration applied'); fillCalibrationSettings(data.settings || body, true); fetchTelephonyDiagnostics(); }
            catch(err){ tdGuard.failApply(`Failed: ${err.message||err}`); }
        }
        async function exportDiagnostics() {
            try { const res=await fetch('/api/telephony/diagnostics/export'); const data=await res.json(); document.getElementById('tdOutput').textContent=JSON.stringify(data,null,2); }
            catch(err){ document.getElementById('tdHelp').textContent=`Export failed: ${err.message||err}`; }
        }

        async function fetchTelephonyCoordinator() {
            try {
                const res = await fetch('/api/telephony/state'); const data = await res.json();
                const st = document.getElementById('tcStatus'); const state = data.state || 'unknown';
                st.textContent = state.toUpperCase(); st.className = `status-pill ${data.safe_to_answer ? 'status-ok' : 'status-bad'}`;
                document.getElementById('tcState').textContent = state;
                document.getElementById('tcHook').textContent = data.hook_offhook ? 'off-hook' : 'on-hook';
                document.getElementById('tcSafe').textContent = data.safe_to_answer ? 'YES' : 'NO';
                document.getElementById('tcRings').textContent = data.ring_count ?? 0;
                document.getElementById('tcAuto').textContent = data.auto_answer_armed ? 'armed' : (data.settings?.auto_answer_enabled ? 'enabled' : 'disabled');
                document.getElementById('tcHangup').textContent = data.auto_hangup_armed ? 'armed' : (data.settings?.auto_hangup_enabled ? 'enabled' : 'disabled');
                document.getElementById('tcCidWait').textContent = data.caller_id_waiting ? 'YES' : 'NO';
                document.getElementById('tcBlock').textContent = data.answer_block_reason || '-';
                document.getElementById('tcHangupReason').textContent = data.hangup_reason || data.last_auto_hangup_reason || '-';
                document.getElementById('tcTransition').textContent = data.last_transition || '-';
                fillTelephonyCoordinatorSettings(data.settings);
                document.getElementById('tcEvents').textContent = `Status: ${data.status || '-'}\nLine: ${data.inputs?.line_state || '-'} (${Number(data.inputs?.line_confidence||0).toFixed(3)})  CID: ${data.inputs?.caller_detected ? 'detected' : (data.inputs?.caller_status || '-')}\n\n${(data.events || []).slice(-12).join('\n')}`;
                document.getElementById('tcHelp').textContent = data.last_error || '';
            } catch(err) { const el=document.getElementById('tcHelp'); if(el) el.textContent = `Telephony coordinator error: ${err.message || err}`; }
        }

        async function applyTelephonyCoordinatorSettings() {
            const body = {enabled:document.getElementById('tcEnabled').checked,auto_answer_enabled:document.getElementById('tcAutoAnswer').checked,caller_id_before_auto_answer:document.getElementById('tcWaitCid').checked,min_rings_before_answer:parseInt(document.getElementById('tcMinRings').value||'1',10),auto_answer_delay_ms:parseInt(document.getElementById('tcDelay').value||'0',10),auto_hangup_enabled:document.getElementById('tcAutoHangup').checked,auto_hangup_after_disconnect_ms:parseInt(document.getElementById('tcHangupDisconnect').value||'1500',10),auto_hangup_after_warning_ms:parseInt(document.getElementById('tcHangupWarning').value||'500',10)};
            tcGuard.beginApply();
            try { const res=await fetch('/api/telephony/state/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)}); const data=await res.json(); if(!res.ok) throw new Error(data.error||'failed'); tcGuard.finishApply('Applied'); fillTelephonyCoordinatorSettings(data.settings || body, true); fetchTelephonyCoordinator(); }
            catch(err){ tcGuard.failApply(`Failed: ${err.message||err}`); }
        }

        async function fetchLineState() {
            try {
                const res = await fetch('/api/telephony/line-state'); const data = await res.json();
                const st = document.getElementById('lsStatus'); const state = data.state || 'unknown';
                st.textContent = state.toUpperCase(); st.className = `status-pill ${['ringing','dial_tone','busy','reorder','ringback'].includes(state) ? 'status-ok' : 'status-bad'}`;
                document.getElementById('lsState').textContent = state;
                document.getElementById('lsConfidence').textContent = (data.confidence || 0).toFixed(3);
                document.getElementById('lsRms').textContent = (data.rms || 0).toFixed(6);
                document.getElementById('lsPeak').textContent = (data.peak || 0).toFixed(6);
                document.getElementById('lsZcr').textContent = `${(data.zero_crossing_hz || 0).toFixed(1)} Hz`;
                document.getElementById('lsTone').textContent = data.best_tone || '-';
                document.getElementById('lsRegion').textContent = data.region_label || data.region || '-';
                document.getElementById('lsWindow').textContent = `${data.window_samples || 0} @ ${data.sample_rate_hz || 0} Hz`;
                fillLineStateSettings(data.settings);
                const tones = (data.tones || []).map(t => `${Number(t.frequency_hz).toFixed(1)} Hz  norm=${Number(t.normalized_power||0).toFixed(4)}  dB=${Number(t.relative_db||-120).toFixed(1)}`).join('\n');
                document.getElementById('lsTones').textContent = `Status: ${data.status || '-'}\nRI active: ${data.ri_active ? 'YES' : 'NO'}  RI ringing: ${data.ri_ringing ? 'YES' : 'NO'}  RI freq: ${(data.ri_frequency_hz || 0).toFixed(2)} Hz\n${tones || 'No tone bins'}`;
                document.getElementById('lsHelp').textContent = data.last_error || '';
            } catch(err) { const el=document.getElementById('lsHelp'); if(el) el.textContent = `Line state error: ${err.message || err}`; }
        }

        async function applyLineStateSettings() {
            const body = {region:document.getElementById('lsRegionSetting').value,analysis_window_ms:parseInt(document.getElementById('lsWindowSetting').value||'100',10),min_rms:parseFloat(document.getElementById('lsMinRms').value||'0.002'),silence_rms:parseFloat(document.getElementById('lsSilenceRms').value||'0.0008'),use_ri_corroboration:document.getElementById('lsUseRi').checked};
            lsGuard.beginApply();
            try { const res=await fetch('/api/telephony/line-state/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)}); const data=await res.json(); if(!res.ok) throw new Error(data.error||'failed'); lsGuard.finishApply('Applied'); fillLineStateSettings(data.settings || body, true); fetchLineState(); }
            catch(err){ lsGuard.failApply(`Failed: ${err.message||err}`); }
        }

        async function setChOffhook(offhook) { await fetch('/api/telephony/ch1817/offhook',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({offhook})}); fetchTelephony(); fetchLineState(); }
        async function applyCh1817Settings() {
            const st=document.getElementById('chApplyStatus'); st.textContent='Applying...';
            try { const res=await fetch('/api/telephony/ch1817/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({auto_answer_enabled:document.getElementById('chAutoAnswer').checked,auto_answer_delay_ms:parseInt(document.getElementById('chAutoDelay').value||'0',10)})}); const data=await res.json(); if(!res.ok) throw new Error(data.error||'failed'); st.textContent='Applied'; fetchTelephony(); }
            catch(err){ st.textContent=`Failed: ${err.message||err}`; alert(`Illegal CH1817 configuration:\n${err.message||err}`); }
        }


        async function fetchAdcScope() {
            try {
                const canvas = document.getElementById('adcScope');
                const pointBudget = Math.max(300, Math.min(2400, canvas.clientWidth * 2));
                const view = document.getElementById('graphView')?.value || 'raw';
                const effects = selectedEffectsCsv();
                const useProfiles = document.getElementById('graphUseProfiles')?.checked !== false;
                const url = useProfiles
                    ? `/api/adc?points=${pointBudget}&view=${encodeURIComponent(view)}`
                    : `/api/adc?points=${pointBudget}&view=${encodeURIComponent(view)}&effects=${encodeURIComponent(effects)}`;
                const res = await fetch(url);
                const data = await res.json();
                window.lastAdcData = data;
                updateAdcHeader(data);
                fillAdcSettings(data);
                drawScope(data);
            } catch (err) {
                const status = document.getElementById('adcStatus');
                status.textContent = 'ADC ERROR';
                status.className = 'status-pill status-bad';
                document.getElementById('adcErr').textContent = String(err);
            }
        }

        let adcConfigDirty = false;
        function markAdcConfigDirty() { adcConfigDirty = true; const s = document.getElementById('adcConfigStatus'); if (s) s.textContent = 'Unsaved ADC changes'; }
        function adcConfigFocused() { return ['adcConfigSource','adcConfigRp2040Dev','adcConfigRate'].includes(document.activeElement && document.activeElement.id); }
        function fillAdcSettings(data, force=false) {
            if (!data || (!force && (adcConfigDirty || adcConfigFocused()))) return;
            const src = document.getElementById('adcConfigSource');
            const dev = document.getElementById('adcConfigRp2040Dev');
            const rate = document.getElementById('adcConfigRate');
            if (src) src.value = data.adc_source || 'rp2040';
            if (dev) dev.value = data.rp2040_dev || '/dev/ttyACM0';
            if (rate) rate.value = data.sample_rate_hz || 16000;
            adcConfigDirty = false;
        }
        function wireAdcConfigDirty() {
            ['adcConfigSource','adcConfigRp2040Dev','adcConfigRate'].forEach(id => {
                const el = document.getElementById(id);
                if (el) { el.addEventListener('input', markAdcConfigDirty); el.addEventListener('change', markAdcConfigDirty); }
            });
        }
        async function applyAdcSettings() {
            const status = document.getElementById('adcConfigStatus');
            const body = {
                adc_source: document.getElementById('adcConfigSource').value,
                rp2040_dev: document.getElementById('adcConfigRp2040Dev').value.trim() || '/dev/ttyACM0',
                sample_rate_hz: Math.max(1, Math.min(100000, parseInt(document.getElementById('adcConfigRate').value || '16000', 10)))
            };
            document.getElementById('adcConfigRate').value = body.sample_rate_hz;
            status.textContent = 'Applying ADC settings...';
            try {
                const res = await fetch('/api/adc/config', { method: 'POST', headers: {'Content-Type':'application/json'}, body: JSON.stringify(body) });
                const data = await res.json();
                if (!res.ok) throw new Error(data.error || `HTTP ${res.status}`);
                status.textContent = `Applied ${data.adc_source || body.adc_source} @ ${data.sample_rate_hz || body.sample_rate_hz} Hz`;
                adcConfigDirty = false;
                setTimeout(fetchAdcScope, 700);
            } catch (err) {
                status.textContent = `ADC apply failed: ${err.message || err}`;
            }
        }

        function formatHexFlags(v) { return '0x' + ((v || 0) >>> 0).toString(16).padStart(8, '0'); }
        function updateAdcDiagnostics(data) {
            const el = document.getElementById('adcDiagnostics');
            if (!el) return;
            if (data.adc_source === 'rp2040') {
                const crc = data.rp2040_packets_crc_bad || 0;
                const gaps = data.rp2040_sequence_gaps || 0;
                const lost = data.rp2040_firmware_lost_frames || 0;
                const flags = data.rp2040_firmware_flags || 0;
                const ok = data.healthy && data.rp2040_connected && crc === 0 && gaps === 0 && lost === 0 && flags === 0;
                const warn = data.rp2040_connected && !ok;
                el.className = `adc-diag ${ok ? 'ok' : (warn ? 'warn' : 'bad')}`;
                el.textContent = `Device: ${data.rp2040_connected ? 'connected' : 'disconnected'} | protocol ${data.device_protocol || 'ADC2'} | transport ${data.device_transport || data.rp2040_dev || '-'} | stream ${data.device_adc_stream_id || 1} | format ${data.device_sample_format || 'U16_LE'} x${data.device_channel_count || 2} | packets ${data.rp2040_packets_ok || 0} | crc ${crc} | gaps ${gaps} | lost ${lost} | flags ${formatHexFlags(flags)} | firmware ${data.rp2040_declared_rate_hz || 0} Hz`;
            } else {
                el.className = `adc-diag ${data.healthy ? 'ok' : 'bad'}`;
                el.textContent = `MCP3202/Linux SPI: ${data.healthy ? 'streaming' : 'not healthy'} | scheduler ${data.scheduler_status || 'normal'} | overruns ${data.overruns || 0} | max overrun ${data.max_overrun_us || 0} us`;
            }
        }

        function updateAdcHeader(data) {
            const sourceNames = {
                'mcp3202-spidev': 'MCP3202 Dual-Channel ADC Scope',
                'rp2040': 'RP2040 USB Dual-Channel ADC Scope'
            };
            const titleEl = document.getElementById('adcTitle');
            if (titleEl) titleEl.textContent = sourceNames[data.adc_source] || 'ADC Dual-Channel Scope';

            const tagEl = document.getElementById('adcTag');
            if (tagEl) {
                const res = (data.adc_source === 'rp2040' || data.adc_source === 'mcp3202-spidev') ? '12-bit' : 'unknown-bit';
                const rateKhz = (data.measured_sample_rate_hz / 1000).toFixed(1);
                const interfaceType = data.adc_source === 'rp2040' ? 'USB CDC' : 'SPI';
                tagEl.textContent = `CH0 + CH1, ${res} @ ${rateKhz} kHz frames (${interfaceType})`;
            }

            const status = document.getElementById('adcStatus');
            status.textContent = data.enabled ? (data.healthy ? 'ADC OK' : 'ADC FAULT') : 'ADC DISABLED';
            status.className = `status-pill ${data.healthy ? 'status-ok' : 'status-bad'}`;
            
            let modeText = 'Unknown';
            if (data.adc_source === 'rp2040') {
                modeText = 'RP2040 (USB)';
            } else if (data.adc_source === 'mcp3202-spidev') {
                modeText = data.bitbang ? 'bitbang GPIO' : 'hardware SPI';
            }
            document.getElementById('adcMode').textContent = modeText;
            const nominalRate = data.sample_rate_hz || 0;
            const measuredRate = data.measured_sample_rate_hz || nominalRate;
            const firmwareRate = data.rp2040_declared_rate_hz || 0;
            document.getElementById('adcRate').textContent = nominalRate ? (data.adc_source === 'rp2040' ? `${measuredRate} Hz measured | ${nominalRate} Hz requested | ${firmwareRate || '-'} Hz firmware` : `${measuredRate} Hz measured | ${nominalRate} Hz requested`) : '-';
            const mismatch = nominalRate && measuredRate && Math.abs(measuredRate - nominalRate) / nominalRate > 0.05;
            const warnEl = document.getElementById('adcRateWarn');
            if (warnEl) warnEl.textContent = mismatch ? 'Measured rate differs from requested; use 16000 or 24000 Hz for tested RP2040 operation.' : '';
            const raw = data.latest_raw || [0, 0];
            const volts = data.latest_volts || [0, 0];
            document.getElementById('adcLatest0').textContent = `${raw[0] ?? 0} (${(volts[0] ?? 0).toFixed(3)} V)`;
            document.getElementById('adcLatest1').textContent = `${raw[1] ?? 0} (${(volts[1] ?? 0).toFixed(3)} V)`;
            document.getElementById('adcSamples').textContent = data.total_frames ?? 0;
            document.getElementById('adcDropped').textContent = data.dropped_reads ?? 0;
            const availMs = data.available_history_ms || 0;
            const capMs = data.history_capacity_ms || 0;
            document.getElementById('adcHistory').textContent = capMs ? `${(availMs/1000).toFixed(1)}s / ${(capMs/1000).toFixed(1)}s` : '-';
            document.getElementById('adcErr').textContent = data.last_error || '';
            updateAdcDiagnostics(data);
        }

        function drawScope(data) {
            const samples0 = (data.samples && data.samples.ch0) || [];
            const samples1 = (data.samples && data.samples.ch1) || [];
            const filt0 = (data.filtered_samples && data.filtered_samples.ch0) || [];
            const filt1 = (data.filtered_samples && data.filtered_samples.ch1) || [];
            const view = data.graph_view || document.getElementById('graphView')?.value || 'raw';

            const canvas = document.getElementById('adcScope');
            const dpr = window.devicePixelRatio || 1;
            const w = Math.max(300, canvas.clientWidth || 1200);
            const h = Math.max(160, canvas.clientHeight || 220);
            if (canvas.width !== Math.floor(w * dpr) || canvas.height !== Math.floor(h * dpr)) {
                canvas.width = Math.floor(w * dpr);
                canvas.height = Math.floor(h * dpr);
            }
            const ctx = canvas.getContext('2d');
            ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
            ctx.clearRect(0, 0, w, h);
            ctx.fillStyle = '#070b0d';
            ctx.fillRect(0, 0, w, h);

            ctx.strokeStyle = 'rgba(0, 173, 181, 0.18)';
            ctx.lineWidth = 1;
            for (let x = 0; x < w; x += w / 12) { ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, h); ctx.stroke(); }
            for (let y = 0; y < h; y += h / 8) { ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(w, y); ctx.stroke(); }

            const midY = h / 2;
            ctx.strokeStyle = 'rgba(255,255,255,0.25)';
            ctx.beginPath(); ctx.moveTo(0, midY); ctx.lineTo(w, midY); ctx.stroke();

            if (!samples0.length && !samples1.length && !filt0.length && !filt1.length) {
                ctx.fillStyle = '#777';
                ctx.fillText('Waiting for ADC samples...', 14, 24);
                return;
            }

            if (view === 'overlay' && (filt0.length || filt1.length)) {
                drawTrace(ctx, samples0, w, h, 'rgba(0,255,135,0.32)', 1.0);
                drawTrace(ctx, samples1, w, h, 'rgba(255,207,51,0.32)', 1.0);
                drawTrace(ctx, filt0, w, h, '#00ff87', 1.8);
                drawTrace(ctx, filt1, w, h, '#ffcf33', 1.6);
            } else {
                drawTrace(ctx, samples0, w, h, '#00ff87', 1.45);
                drawTrace(ctx, samples1, w, h, '#ffcf33', 1.25);
            }

            ctx.fillStyle = '#8df7ff';
            ctx.font = '12px monospace';
            ctx.fillText(`0`, 6, h - 6);
            ctx.fillText(`2048`, 6, midY - 6);
            ctx.fillText(`4095`, 6, 14);
            let label = view === 'overlay' ? 'overlay: dim=raw bright=filtered' : (view === 'filtered' ? 'filtered preview' : 'raw ADC');
            if ((data.graph_skipped_effects || []).length) label += ` / skipped live: ${(data.graph_skipped_effects || []).join(',')}`;
            ctx.fillStyle = '#aaa'; ctx.fillText(label, 80, 16);
            ctx.fillStyle = '#00ff87'; ctx.fillText('CH0', w - 76, 16);
            ctx.fillStyle = '#ffcf33'; ctx.fillText('CH1', w - 38, 16);
        }

        function drawTrace(ctx, samples, w, h, color, width) {
            if (!samples || !samples.length) return;
            ctx.strokeStyle = color;
            ctx.lineWidth = width;
            ctx.beginPath();
            for (let i = 0; i < samples.length; i++) {
                const x = samples.length === 1 ? 0 : (i / (samples.length - 1)) * (w - 1);
                const y = h - (Math.max(0, Math.min(4095, samples[i])) / 4095) * h;
                if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
            }
            ctx.stroke();
        }

        async function updateConfig(pin, action, value) {
            const params = new URLSearchParams();
            params.append('pin', pin);
            params.append('action', action);
            params.append('value', value);

            await fetch('/api/config', {
                method: 'POST',
                headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                body: params
            });
            fetchStatus();
        }

        let filterProfileData = null;
        const filterProfileGuard = makeSettingsGuard(['filterProfileContext','filterProfileEnabled'], 'filterProfileStatus');

        function selectedProfileEffectsCsv() {
            return Array.from(document.querySelectorAll('#filterProfileEffects input[type="checkbox"]:checked'))
                .map(input => input.value)
                .join(',');
        }

        function currentFilterProfileContext() {
            return document.getElementById('filterProfileContext')?.value || loadUiPref('filterProfileContext', 'adc.graph.ch0');
        }

        function fillFilterProfileEditor(force=false) {
            if (!filterProfileData || !filterProfileGuard.shouldFill(force)) return;
            const ctxSelect = document.getElementById('filterProfileContext');
            const enabled = document.getElementById('filterProfileEnabled');
            const effectsBox = document.getElementById('filterProfileEffects');
            if (!ctxSelect || !enabled || !effectsBox) return;

            if (!ctxSelect.options.length) {
                for (const ctx of (filterProfileData.contexts || [])) {
                    const opt = document.createElement('option');
                    opt.value = ctx.id;
                    opt.textContent = ctx.label || ctx.id;
                    ctxSelect.appendChild(opt);
                }
                const savedCtx = loadUiPref('filterProfileContext', 'adc.graph.ch0');
                if (filterProfileData.profiles?.[savedCtx]) ctxSelect.value = savedCtx;
                ctxSelect.addEventListener('change', () => {
                    saveUiPref('filterProfileContext', ctxSelect.value);
                    filterProfileGuard.clearDirty('');
                    fillFilterProfileEditor(true);
                });
                enabled.addEventListener('change', filterProfileGuard.markDirty);
            }

            const desiredCtx = filterProfileData.profiles?.[ctxSelect.value] ? ctxSelect.value : loadUiPref('filterProfileContext', 'adc.graph.ch0');
            if (filterProfileData.profiles?.[desiredCtx]) ctxSelect.value = desiredCtx;
            const ctx = ctxSelect.value || 'adc.graph.ch0';
            const profile = filterProfileData.profiles?.[ctx] || {enabled:true, effects:[]};
            enabled.checked = profile.enabled !== false;
            effectsBox.innerHTML = '';
            const selected = new Set(profile.effects || []);
            for (const effect of (filterProfileData.effects || [])) {
                const label = document.createElement('label');
                label.className = `effect-item ${effect.available ? '' : 'disabled'}`;
                label.title = effect.description || '';
                const input = document.createElement('input');
                input.type = 'checkbox';
                input.value = effect.id;
                input.disabled = !effect.available;
                input.checked = selected.has(effect.id);
                input.addEventListener('change', filterProfileGuard.markDirty);
                label.appendChild(input);
                label.appendChild(document.createTextNode(effect.label || effect.id));
                effectsBox.appendChild(label);
            }
            if (force) filterProfileGuard.clearDirty('');
        }

        async function reloadFilterProfiles(force=false) {
            const status = document.getElementById('filterProfileStatus');
            if (status && force) status.textContent = 'Reloading profiles...';
            try {
                const res = await fetch('/api/audio/filter-profiles');
                const data = await res.json();
                if (!res.ok) throw new Error(data.error || `HTTP ${res.status}`);
                filterProfileData = data;
                fillFilterProfileEditor(force);
                if (status && force) status.textContent = 'Profiles reloaded';
            } catch (err) {
                if (status) status.textContent = `Profile load failed: ${err.message || err}`;
            }
        }

        async function applyFilterProfile() {
            const ctx = currentFilterProfileContext();
            filterProfileGuard.beginApply();
            try {
                const body = {
                    context: ctx,
                    enabled: document.getElementById('filterProfileEnabled').checked,
                    effects: selectedProfileEffectsCsv().split(',').filter(Boolean)
                };
                const res = await fetch('/api/audio/filter-profile', {method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify(body)});
                const data = await res.json();
                if (!res.ok) throw new Error(data.error || `HTTP ${res.status}`);
                if (!filterProfileData) filterProfileData = {profiles:{}};
                if (!filterProfileData.profiles) filterProfileData.profiles = {};
                filterProfileData.profiles[ctx] = data.profile;
                filterProfileGuard.finishApply('Profile applied');
                fillFilterProfileEditor(true);
                fetchAdcScope();
            } catch (err) { filterProfileGuard.failApply(`Failed: ${err.message || err}`); }
        }

        function clearFilterProfile() {
            document.querySelectorAll('#filterProfileEffects input[type="checkbox"]').forEach(input => { input.checked = false; });
            filterProfileGuard.markDirty();
        }

        async function restoreFilterProfileDefault() {
            const ctx = currentFilterProfileContext();
            filterProfileGuard.beginApply();
            try {
                const res = await fetch('/api/audio/filter-profile', {method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify({context:ctx, action:'restore_default'})});
                const data = await res.json();
                if (!res.ok) throw new Error(data.error || `HTTP ${res.status}`);
                if (!filterProfileData) filterProfileData = {profiles:{}};
                if (!filterProfileData.profiles) filterProfileData.profiles = {};
                filterProfileData.profiles[ctx] = data.profile;
                filterProfileGuard.finishApply('Default restored');
                fillFilterProfileEditor(true);
                fetchAdcScope();
            } catch (err) { filterProfileGuard.failApply(`Failed: ${err.message || err}`); }
        }

        async function loadAudioModules() {
            if (!ADC_ENABLED) return;
            try {
                const res = await fetch('/api/audio/modules');
                const data = await res.json();
                const effectsList = document.getElementById('effectsList');
                effectsList.innerHTML = '';
                const savedEffects = loadUiPref('effects', 'dc_block').split(',').filter(Boolean);
                for (const effect of (data.effects || [])) {
                    const label = document.createElement('label');
                    label.className = `effect-item ${effect.available ? '' : 'disabled'}`;
                    label.title = effect.description || '';
                    const input = document.createElement('input');
                    input.type = 'checkbox';
                    input.value = effect.id;
                    input.disabled = !effect.available;
                    input.checked = savedEffects.includes(effect.id);
                    input.addEventListener('change', () => { saveAudioUiPrefs(); fetchAdcScope(); });
                    label.appendChild(input);
                    label.appendChild(document.createTextNode(effect.label || effect.id));
                    effectsList.appendChild(label);
                }
                const codecSelect = document.getElementById('audioCodec');
                codecSelect.innerHTML = '';
                for (const codec of (data.codecs || [])) {
                    const opt = document.createElement('option');
                    opt.value = codec.id;
                    opt.textContent = codec.label || codec.id;
                    opt.disabled = !codec.available;
                    if (codec.id === loadUiPref('audioCodec', 'pcm16')) opt.selected = true;
                    codecSelect.appendChild(opt);
                }
                codecSelect.addEventListener('change', saveAudioUiPrefs);
                saveAudioUiPrefs();
            } catch (err) {
                document.getElementById('effectsList').textContent = `Unable to load audio modules: ${err}`;
            }
        }

        const UI_PREF_PREFIX = 'cm4gpio.';
        function saveUiPref(key, value) { try { localStorage.setItem(UI_PREF_PREFIX + key, value); } catch (_) {} }
        function loadUiPref(key, fallback='') { try { return localStorage.getItem(UI_PREF_PREFIX + key) ?? fallback; } catch (_) { return fallback; } }

        function saveAudioUiPrefs() {
            const dur = document.getElementById('wavDuration');
            const mode = document.getElementById('wavMode');
            const codec = document.getElementById('audioCodec');
            const graph = document.getElementById('graphView');
            const graphProfiles = document.getElementById('graphUseProfiles');
            const wavProfiles = document.getElementById('wavUseProfiles');
            if (dur) saveUiPref('wavDuration', dur.value);
            if (mode) saveUiPref('wavMode', mode.value);
            if (codec) saveUiPref('audioCodec', codec.value);
            if (graph) saveUiPref('graphView', graph.value);
            if (graphProfiles) saveUiPref('graphUseProfiles', graphProfiles.checked ? '1' : '0');
            if (wavProfiles) saveUiPref('wavUseProfiles', wavProfiles.checked ? '1' : '0');
            saveUiPref('effects', selectedEffectsCsv());
        }

        function restoreSimpleUiPrefs() {
            const dur = document.getElementById('wavDuration');
            const mode = document.getElementById('wavMode');
            const graph = document.getElementById('graphView');
            const graphProfiles = document.getElementById('graphUseProfiles');
            const wavProfiles = document.getElementById('wavUseProfiles');
            if (dur) dur.value = loadUiPref('wavDuration', dur.value || '1000');
            if (mode) mode.value = loadUiPref('wavMode', mode.value || 'ch0');
            if (graph) graph.value = loadUiPref('graphView', graph.value || 'raw');
            if (graphProfiles) graphProfiles.checked = loadUiPref('graphUseProfiles', '1') !== '0';
            if (wavProfiles) wavProfiles.checked = loadUiPref('wavUseProfiles', '1') !== '0';
            [dur, mode, graph, graphProfiles, wavProfiles].forEach(el => { if (el) el.addEventListener('change', () => { saveAudioUiPrefs(); if (el === graphProfiles) fetchAdcScope(); }); });
            if (dur) dur.addEventListener('input', saveAudioUiPrefs);
        }

        function selectedEffectsCsv() {
            return Array.from(document.querySelectorAll('#effectsList input[type="checkbox"]:checked'))
                .map(input => input.value)
                .join(',');
        }

        function currentWavUrl() {
            const durInput = document.getElementById('wavDuration');
            const modeSelect = document.getElementById('wavMode');
            const codecSelect = document.getElementById('audioCodec');
            const ms = Math.max(1, Math.min(600000, parseInt(durInput.value || '1000', 10)));
            durInput.value = ms;
            const mode = modeSelect.value;
            const codec = codecSelect.value || 'pcm16';
            const effects = selectedEffectsCsv();
            const useProfiles = document.getElementById('wavUseProfiles')?.checked !== false;
            saveAudioUiPrefs();
            const base = `/api/adc/wav?ms=${encodeURIComponent(ms)}&mode=${encodeURIComponent(mode)}&codec=${encodeURIComponent(codec)}`;
            return useProfiles ? base : `${base}&effects=${encodeURIComponent(effects)}`;
        }

        async function downloadWav() {
            const status = document.getElementById('wavStatus');
            status.textContent = 'Preparing download...';
            try {
                const res = await fetch(currentWavUrl());
                if (!res.ok) {
                    let msg = `HTTP ${res.status}`;
                    try { const j = await res.json(); msg = j.error || msg; } catch (_) {}
                    throw new Error(msg);
                }
                const blob = await res.blob();
                const requestedMs = parseInt(res.headers.get('X-Requested-Duration-Ms') || '0', 10);
                const actualMs = parseInt(res.headers.get('X-Actual-Duration-Ms') || '0', 10);
                const availableMs = parseInt(res.headers.get('X-Available-Duration-Ms') || '0', 10);
                const truncated = (res.headers.get('X-Wav-Truncated') || 'false') === 'true';
                const cd = res.headers.get('Content-Disposition') || '';
                const m = cd.match(/filename="?([^";]+)"?/i);
                const sourcePrefix = (window.lastAdcData && window.lastAdcData.adc_source === 'rp2040') ? 'rp2040' : ((window.lastAdcData && window.lastAdcData.adc_source === 'mcp3202-spidev') ? 'mcp3202' : 'adc');
                const filename = m ? m[1] : `${sourcePrefix}_${mode}_${ms}ms.wav`;
                const url = URL.createObjectURL(blob);
                const a = document.createElement('a');
                a.href = url;
                a.download = filename;
                document.body.appendChild(a);
                a.click();
                a.remove();
                URL.revokeObjectURL(url);
                status.textContent = truncated
                    ? `Downloaded ${filename} — truncated to ${actualMs} ms; only ${availableMs} ms available.`
                    : `Downloaded ${filename}${actualMs ? ` (${actualMs} ms)` : ''}`;
            } catch (err) {
                status.textContent = `Download failed: ${err.message || err}`;
            }
        }

        async function previewWav() {
            const status = document.getElementById('wavStatus');
            const player = document.getElementById('audioPreview');
            status.textContent = 'Preparing preview...';
            try {
                const res = await fetch(currentWavUrl());
                if (!res.ok) {
                    let msg = `HTTP ${res.status}`;
                    try { const j = await res.json(); msg = j.error || msg; } catch (_) {}
                    throw new Error(msg);
                }
                const actualMs = parseInt(res.headers.get('X-Actual-Duration-Ms') || '0', 10);
                const availableMs = parseInt(res.headers.get('X-Available-Duration-Ms') || '0', 10);
                const truncated = (res.headers.get('X-Wav-Truncated') || 'false') === 'true';
                const blob = await res.blob();
                if (player.dataset.objectUrl) URL.revokeObjectURL(player.dataset.objectUrl);
                const url = URL.createObjectURL(blob);
                player.dataset.objectUrl = url;
                player.src = url;
                player.style.display = 'block';
                await player.play().catch(() => {});
                status.textContent = truncated
                    ? `Preview ready — truncated to ${actualMs} ms; only ${availableMs} ms available.`
                    : `Preview ready${actualMs ? ` (${actualMs} ms)` : ''}`;
            } catch (err) {
                status.textContent = `Preview failed: ${err.message || err}`;
            }
        }


        const dacGuard = makeSettingsGuard(['dacConfigEnabled','dacConfigTransport','dacConfigRp2040Dev','dacConfigRate'], 'dacConfigStatus');
        const playbackGuard = makeSettingsGuard(['playbackFile','playbackUploadSelect','playbackChannel','playbackUseProfiles','playbackGain','playbackLoop'], 'playbackStatus');
        const dtmfGuard = makeSettingsGuard(['dtmfSequence','dtmfToneMs','dtmfGapMs','dtmfAmplitude','dtmfChannel'], 'dtmfStatus');
        function wireDtmfPad() {
            dtmfGuard.wire();
            document.querySelectorAll('#dtmfPad button[data-dtmf]').forEach(btn => {
                btn.addEventListener('click', () => {
                    const seq = document.getElementById('dtmfSequence');
                    seq.value = (seq.value || '') + btn.dataset.dtmf;
                    dtmfGuard.markDirty();
                    seq.focus();
                });
            });
        }
        async function refreshPlaybackUploads(force=false) {
            try {
                const res = await fetch('/api/audio/uploads'); const data = await res.json(); if(!res.ok) throw new Error(data.error || 'failed');
                const sel = document.getElementById('playbackUploadSelect'); if (!sel || (!force && !playbackGuard.shouldFill(false))) return;
                const old = sel.value; sel.innerHTML = '';
                for (const u of (data.uploads || [])) { const opt=document.createElement('option'); opt.value=u.id; opt.textContent=`${u.original_name} (${u.duration_ms||0} ms, ${u.sample_rate_hz||0} Hz, ${u.channels||0}ch)`; sel.appendChild(opt); }
                if (old) sel.value = old;
            } catch(err) { const st=document.getElementById('playbackStatus'); if(st) st.textContent=`Uploads error: ${err.message||err}`; }
        }
        async function uploadPlaybackFile() {
            const file = document.getElementById('playbackFile').files[0]; const st=document.getElementById('playbackStatus');
            if (!file) { st.textContent='Choose a WAV file first'; return; }
            st.textContent='Uploading...';
            try { const form=new FormData(); form.append('file', file); const res=await fetch('/api/audio/upload',{method:'POST',body:form}); const data=await res.json(); if(!res.ok) throw new Error(data.error||'upload failed'); playbackGuard.clearDirty('Uploaded'); await refreshPlaybackUploads(true); document.getElementById('playbackUploadSelect').value=data.upload.id; }
            catch(err){ playbackGuard.failApply(`Upload failed: ${err.message||err}`); }
        }
        async function deletePlaybackUpload() {
            const id=document.getElementById('playbackUploadSelect').value; if(!id) return; const st=document.getElementById('playbackStatus'); st.textContent='Deleting...';
            try { const res=await fetch('/api/audio/upload/delete',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({id})}); const data=await res.json(); if(!res.ok) throw new Error(data.error||'delete failed'); playbackGuard.clearDirty('Deleted'); refreshPlaybackUploads(true); }
            catch(err){ playbackGuard.failApply(`Delete failed: ${err.message||err}`); }
        }
        async function playUploadedAudio() {
            const id=document.getElementById('playbackUploadSelect').value; if(!id) { document.getElementById('playbackStatus').textContent='No upload selected'; return; }
            playbackGuard.beginApply();
            try { const body={id,channel_mode:document.getElementById('playbackChannel').value,use_profiles:document.getElementById('playbackUseProfiles').checked,gain:parseFloat(document.getElementById('playbackGain').value||'1'),loop:document.getElementById('playbackLoop').checked}; const res=await fetch('/api/audio/playback/play',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)}); const data=await res.json(); if(!res.ok) throw new Error(data.error||'play failed'); playbackGuard.finishApply('Playing'); }
            catch(err){ playbackGuard.failApply(`Play failed: ${err.message||err}`); }
        }
        async function stopUploadedAudio() { const st=document.getElementById('playbackStatus'); st.textContent='Stopping...'; try { const res=await fetch('/api/audio/playback/stop',{method:'POST'}); const data=await res.json(); if(!res.ok) throw new Error(data.error||'stop failed'); st.textContent='Stopped'; } catch(err){ st.textContent=`Stop failed: ${err.message||err}`; } }
        async function fetchPlaybackStatus() {
            try { const res=await fetch('/api/audio/playback/status'); const data=await res.json(); if(!res.ok) return; const p=data.playback||{}; const st=document.getElementById('playbackStatus'); if(st && !playbackGuard.focused()) st.textContent = `${p.status||'idle'} ${p.position_ms||0}/${p.duration_ms||0} ms ${p.last_error||''}`; }
            catch(_) {}
        }

        function dtmfBody() {
            return {
                digits: document.getElementById('dtmfSequence').value || '',
                tone_ms: Math.max(40, Math.min(2000, parseInt(document.getElementById('dtmfToneMs').value || '100', 10))),
                gap_ms: Math.max(0, Math.min(2000, parseInt(document.getElementById('dtmfGapMs').value || '50', 10))),
                amplitude: Math.max(1, Math.min(2047, parseInt(document.getElementById('dtmfAmplitude').value || '1200', 10))),
                channel_mask: parseInt(document.getElementById('dtmfChannel').value || '1', 10)
            };
        }
        async function playDtmf() {
            dtmfGuard.beginApply();
            try {
                const res = await fetch('/api/dac/dtmf/play', {method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify(dtmfBody())});
                const data = await res.json(); if (!res.ok) throw new Error(data.error || 'failed');
                dtmfGuard.finishApply('Dialing');
            } catch (err) { dtmfGuard.failApply(`Failed: ${err.message || err}`); }
        }
        async function stopDtmf() {
            const st = document.getElementById('dtmfStatus'); if (st) st.textContent = 'Stopping...';
            try { const res = await fetch('/api/dac/dtmf/stop', {method:'POST'}); const data = await res.json(); if(!res.ok) throw new Error(data.error || 'failed'); if (st) st.textContent = 'Stopped'; }
            catch(err) { if (st) st.textContent = `Failed: ${err.message || err}`; }
        }
        function clearDtmf() { document.getElementById('dtmfSequence').value = ''; dtmfGuard.markDirty(); }
        function backspaceDtmf() { const el=document.getElementById('dtmfSequence'); el.value=(el.value||'').slice(0,-1); dtmfGuard.markDirty(); }

        function updateDacUi(data) {
            const dac = data && data.dac;
            const cfg = data && data.dac_config;
            if (!dac) return;
            const pill = document.getElementById('dacStatusPill');
            if (pill) { pill.textContent = dac.enabled ? (dac.healthy ? 'OK' : 'WARN') : 'DISABLED'; pill.className = `status-pill ${dac.enabled && dac.healthy ? 'status-ok' : 'status-bad'}`; }
            const set = (id, v) => { const el = document.getElementById(id); if (el) el.textContent = v; };
            set('dacEnabled', dac.enabled ? 'yes' : 'no');
            set('dacTransportStatus', dac.transport || '-');
            set('dacActive', dac.active ? 'yes' : 'no');
            set('dacRaw', `${dac.raw_a ?? 0} / ${dac.raw_b ?? 0}`);
            set('dacVolts', `${Number(dac.volts_a || 0).toFixed(4)} / ${Number(dac.volts_b || 0).toFixed(4)} V`);
            set('dacFrames', dac.frames_written ?? 0);
            set('dacErrors', dac.errors ?? 0);
            set('dacLastError', dac.last_error || '-');
            if (cfg && dacGuard.shouldFill(false)) {
                const en = document.getElementById('dacConfigEnabled'); if (en && document.activeElement !== en) en.checked = !!cfg.enabled;
                const tr = document.getElementById('dacConfigTransport'); if (tr && document.activeElement !== tr) tr.value = cfg.transport || 'native';
                const dev = document.getElementById('dacConfigRp2040Dev'); if (dev && document.activeElement !== dev) dev.value = cfg.rp2040_dev || '/dev/ttyACM0';
                const rate = document.getElementById('dacConfigRate'); if (rate && document.activeElement !== rate) rate.value = cfg.sample_rate_hz || 48000;
            }
        }

        async function applyDacConfig() {
            const st = document.getElementById('dacConfigStatus');
            dacGuard.beginApply();
            try {
                const body = {
                    enabled: document.getElementById('dacConfigEnabled').checked,
                    transport: document.getElementById('dacConfigTransport').value,
                    rp2040_dev: document.getElementById('dacConfigRp2040Dev').value,
                    sample_rate_hz: parseInt(document.getElementById('dacConfigRate').value || '48000', 10)
                };
                const res = await fetch('/api/dac/config', {method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify(body)});
                const data = await res.json();
                if (!res.ok) throw new Error(data.error || 'failed');
                dacGuard.finishApply('Applied');
                fetchStatus();
                setTimeout(() => { if (st.textContent === 'Applied') st.textContent = ''; }, 1500);
            } catch (err) { dacGuard.failApply(`Failed: ${err.message || err}`); }
        }

        async function dacControl(action) {
            const st = document.getElementById('dacConfigStatus');
            st.textContent = `${action}...`;
            try {
                const res = await fetch('/api/dac/control', {method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify({action})});
                const data = await res.json();
                if (!res.ok) throw new Error(data.error || 'failed');
                st.textContent = 'OK';
                fetchStatus();
                setTimeout(() => { if (st.textContent === 'OK') st.textContent = ''; }, 1500);
            } catch (err) { st.textContent = `Failed: ${err.message || err}`; }
        }

        async function writeDacRaw() {
            const st = document.getElementById('dacWriteStatus');
            st.textContent = 'Writing...';
            try {
                const body = {raw_a: parseInt(document.getElementById('dacRawA').value || '0', 10), raw_b: parseInt(document.getElementById('dacRawB').value || '0', 10)};
                const res = await fetch('/api/dac/write', {method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify(body)});
                const data = await res.json();
                if (!res.ok) throw new Error(data.error || 'failed');
                st.textContent = 'Written';
                fetchStatus();
            } catch (err) { st.textContent = `Failed: ${err.message || err}`; }
        }

        async function writeDacVolts() {
            const st = document.getElementById('dacWriteStatus');
            st.textContent = 'Writing...';
            try {
                const body = {volts_a: parseFloat(document.getElementById('dacVoltsA').value || '0'), volts_b: parseFloat(document.getElementById('dacVoltsB').value || '0')};
                const res = await fetch('/api/dac/write', {method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify(body)});
                const data = await res.json();
                if (!res.ok) throw new Error(data.error || 'failed');
                st.textContent = 'Written';
                fetchStatus();
            } catch (err) { st.textContent = `Failed: ${err.message || err}`; }
        }


        let dtmfValidation = { active:false, expected:'', startSeq:0 };
        const normalBitsByDigit = {'1':'0001','2':'0010','3':'0011','4':'0100','5':'0101','6':'0110','7':'0111','8':'1000','9':'1001','0':'1010','*':'1011','#':'1100','A':'1101','B':'1110','C':'1111','D':'0000'};
        function setText(id, value) { const el = document.getElementById(id); if (el) el.textContent = value; }
        function updateMcuPeripheralUi(data) {
            const mp = data && data.mcu_peripherals; if (!mp) return;
            const dec = mp.dtmf_decoder || {}; const raw = dec.raw || {}; const sig = mp.ch1817_signals || {};
            setText('dtmfDecEnabled', dec.enabled ? 'yes' : 'no');
            setText('dtmfDecSource', dec.source || '-');
            setText('dtmfDecActive', dec.active ? 'ACTIVE' : 'idle');
            setText('dtmfDecDigit', dec.current_digit || '-');
            setText('mtStqRaw', raw.stq ? '1' : '0'); setText('mtQ1Raw', raw.q1 ? '1' : '0'); setText('mtQ2Raw', raw.q2 ? '1' : '0'); setText('mtQ3Raw', raw.q3 ? '1' : '0'); setText('mtQ4Raw', raw.q4 ? '1' : '0');
            setText('mtBits', raw.q_bits_binary || '----'); setText('mtSeq', dec.last_sequence || 0);
            const ri = sig.ri || {}; const oh = sig.oh || {};
            setText('mcuRiSource', ri.source || '-'); setText('mcuRiRaw', ri.raw ? '1' : '0'); setText('mcuRiLogical', ri.logical ? '1' : '0'); setText('mcuRiTransitions', ri.transition_count || 0);
            setText('mcuOhSource', oh.source || '-'); setText('mcuOhRaw', oh.raw ? '1' : '0'); setText('mcuOhLogical', oh.logical ? '1' : '0'); setText('mcuOhTransitions', oh.transition_count || 0);
            renderDtmfDecoderHistory(dec.history || []);
        }
        function renderDtmfDecoderHistory(history) {
            const tbody = document.getElementById('dtmfDecoderHistory'); if (!tbody) return;
            if (!history.length) { tbody.innerHTML = '<tr><td colspan="6">No decoded tones yet.</td></tr>'; return; }
            const rows = [];
            const mismatches = [];
            const events = history.filter(e => !dtmfValidation.active || (e.sequence || 0) > dtmfValidation.startSeq);
            const expected = (dtmfValidation.expected || '').toUpperCase();
            history.slice().reverse().forEach((e) => {
                const evIndex = events.findIndex(x => x.sequence === e.sequence);
                const exp = (dtmfValidation.active && evIndex >= 0 && evIndex < expected.length) ? expected[evIndex] : '';
                const obs = (e.digit || '').toUpperCase();
                const match = exp ? (obs === exp ? 'yes' : 'NO') : '';
                if (exp && obs !== exp) mismatches.push({expected:exp, observed:obs, raw:e.raw_q_bits_binary || ''});
                rows.push(`<tr><td>${e.sequence ?? ''}</td><td><b>${e.digit || ''}</b></td><td>${e.raw_q_bits_binary || ''}</td><td>${exp}</td><td>${match}</td><td>${e.source || ''}</td></tr>`);
            });
            tbody.innerHTML = rows.join('');
            const help = document.getElementById('dtmfMappingHelp');
            if (help) {
                help.textContent = mismatches.length ? mismatches.slice(0,5).map(m => `Expected ${m.expected} normally bits ${normalBitsByDigit[m.expected] || '????'}, observed ${m.observed || '?'} bits ${m.raw}.`).join(' ') + ' This may indicate Q pins are swapped or inverted.' : '';
            }
            const st = document.getElementById('dtmfDecoderStatus');
            if (st && dtmfValidation.active) st.textContent = `Validation: ${Math.min(events.length, expected.length)}/${expected.length} captured`;
        }
        function startDtmfValidation() {
            const el = document.getElementById('dtmfValidationExpected');
            dtmfValidation.expected = ((el && el.value) || '').toUpperCase().replace(/[^0-9*#A-D]/g, '');
            const seq = parseInt(document.getElementById('mtSeq')?.textContent || '0', 10) || 0;
            dtmfValidation.startSeq = seq;
            dtmfValidation.active = dtmfValidation.expected.length > 0;
            const st = document.getElementById('dtmfDecoderStatus'); if (st) st.textContent = dtmfValidation.active ? `Validation started for ${dtmfValidation.expected}` : 'Enter expected sequence first';
        }
        async function clearDtmfDecoderHistory() {
            const st = document.getElementById('dtmfDecoderStatus'); if (st) st.textContent = 'Clearing...';
            try { const res = await fetch('/api/dac/dtmf/decoder/history/clear', {method:'POST'}); const data = await res.json(); if(!res.ok) throw new Error(data.error || 'failed'); dtmfValidation.active=false; if (st) st.textContent='History cleared'; fetchStatus(); }
            catch(err) { if (st) st.textContent = `Clear failed: ${err.message || err}`; }
        }
        async function copyDtmfDecoderHistory() {
            try {
                const res = await fetch('/api/dac/dtmf/decoder/status');
                const data = await res.json();
                if (!res.ok) throw new Error(data.error || 'failed');
                const txt = (data.history || []).map(e => `${e.sequence}\t${e.digit}\t${e.raw_q_bits_binary}\t${e.source}`).join('\n');
                if (navigator.clipboard && navigator.clipboard.writeText) {
                    await navigator.clipboard.writeText(txt);
                } else {
                    const ta = document.createElement('textarea');
                    ta.value = txt;
                    ta.style.position = 'fixed';
                    ta.style.left = '-9999px';
                    document.body.appendChild(ta);
                    ta.focus();
                    ta.select();
                    const ok = document.execCommand('copy');
                    ta.remove();
                    if (!ok) throw new Error('clipboard unavailable');
                }
                const st=document.getElementById('dtmfDecoderStatus'); if(st) st.textContent='History copied';
            }
            catch(err) { const st=document.getElementById('dtmfDecoderStatus'); if(st) st.textContent=`Copy failed: ${err.message || err}`; }
        }

        async function updateTimeout() {
            const val = document.getElementById('timeoutInput').value;
            const params = new URLSearchParams();
            params.append('action', 'timeout');
            params.append('value', val);

            timeoutInputLockExpiration = 0;

            await fetch('/api/config', {
                method: 'POST',
                headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                body: params
            });
            fetchStatus();
        }

        const ADC_ENABLED = __ADC_ENABLED__;
        setInterval(fetchStatus, 250);
        if (ADC_ENABLED) { setInterval(fetchAdcScope, 120); setInterval(fetchCallerId, 1000); setInterval(fetchLineState, 500); }
        setInterval(fetchTelephony, 500); setInterval(fetchTelephonyCoordinator, 500); setInterval(fetchTelephonyDiagnostics, 1500);
        setInterval(fetchPlaybackStatus, 500);
        window.onload = () => { restoreSimpleUiPrefs(); selectTab(loadUiPref('activeTab', 'scope')); fetchStatus(); wireCallerIdTuneDirty(); wireSettingsGuards(); wireAdcConfigDirty(); dacGuard.wire(); playbackGuard.wire(); filterProfileGuard.wire(); wireDtmfPad(); refreshPlaybackUploads(true); if (ADC_ENABLED) { reloadFilterProfiles(true); loadAudioModules().then(() => fetchAdcScope()); fetchCallerId(); fetchLineState(); } fetchTelephony(); fetchTelephonyCoordinator(); fetchTelephonyDiagnostics(); };
    </script>
</body>
</html>
)html";

WebServer::WebServer(std::map<int, std::shared_ptr<PinState>>& reg, ConfigManager& cfg, GpioManager& gpio, std::shared_ptr<SystemContext> ctx, AdcSampler* adc, DacOutput* dac, CallerIdDetector* cid, Ch1817Driver* ch1817, LineStateDetector* line_state, TelephonyCoordinator* telco, TelephonyDiagnostics* tel_diag, FilterProfileManager* profiles, AudioPlaybackService* playback, std::set<int> reserved) 
    : registry(reg), config_mgr(cfg), gpio_mgr(gpio), context(ctx), adc_sampler(adc), dac_output(dac), caller_id_detector(cid), ch1817_driver(ch1817), line_state_detector(line_state), telephony_coordinator(telco), telephony_diagnostics(tel_diag), filter_profiles(profiles), playback_service(playback), reserved_bcm_pins(std::move(reserved)) {
    setup_routes();
}

void WebServer::setup_routes() {
    svr.Get("/", [this](const httplib::Request&, httplib::Response& res) {
        std::string html = HTML_UI;
        const bool adc_enabled = (adc_sampler != nullptr && adc_sampler->isEnabled());
        auto replace_all = [](std::string& s, const std::string& from, const std::string& to) {
            size_t pos = 0;
            while ((pos = s.find(from, pos)) != std::string::npos) {
                s.replace(pos, from.length(), to);
                pos += to.length();
            }
        };
        replace_all(html, "__ADC_ENABLED__", adc_enabled ? "true" : "false");
        replace_all(html, "__BODY_CLASS__", adc_enabled ? "adc-enabled" : "gpio-only");
        res.set_content(html, "text/html");
    });

    svr.Get("/api/status", [this](const httplib::Request&, httplib::Response& res) {
        res.set_content(serialize_state(), "application/json");
    });

    svr.Get("/api/system/settings", [this](const httplib::Request&, httplib::Response& res) {
        json j;
        std::string src = readFskSourceSetting(context);
        res.set_content(json{{"fsk_source", src}}.dump(), "application/json");
    });

    svr.Post("/api/adc/config", [this](const httplib::Request& req, httplib::Response& res) {
        if (!adc_sampler) {
            res.status = 404;
            res.set_content("{\"error\":\"ADC sampler not configured\"}", "application/json");
            return;
        }
        try {
            json j = json::parse(req.body.empty() ? "{}" : req.body);

            AdcSampler::Config cfg = adc_sampler->config();
            cfg.adc_source = j.value("adc_source", cfg.adc_source);
            cfg.rp2040_dev = j.value("rp2040_dev", cfg.rp2040_dev);
            cfg.enabled = j.value("enabled", cfg.enabled);
            if (j.contains("sample_rate_hz")) cfg.sample_rate_hz = j.at("sample_rate_hz").get<uint32_t>();
            if (j.contains("sample_rate")) cfg.sample_rate_hz = j.at("sample_rate").get<uint32_t>();
            if (j.contains("history_samples")) cfg.history_samples = j.at("history_samples").get<size_t>();
            if (j.contains("vref")) cfg.vref = j.at("vref").get<double>();

            if (cfg.adc_source != "mcp3202-spidev" && cfg.adc_source != "rp2040") {
                throw std::runtime_error("adc_source must be 'mcp3202-spidev' or 'rp2040'");
            }
            if (cfg.sample_rate_hz == 0 || cfg.sample_rate_hz > 100000) {
                throw std::runtime_error("sample_rate_hz must be in range 1..100000");
            }
            if (cfg.history_samples == 0) {
                throw std::runtime_error("history_samples must be positive");
            }
            if (cfg.vref <= 0.0) {
                throw std::runtime_error("vref must be positive");
            }

            adc_sampler->updateConfig(cfg);

            std::set<int> new_reserved = adcReservedPinsForConfig(cfg);
            if (dac_output) {
                std::set<int> dac_pins = dacReservedPinsForConfig(dac_output->config());
                new_reserved.insert(dac_pins.begin(), dac_pins.end());
            }
            if (ch1817_driver && ch1817_driver->settings().enabled) {
                new_reserved.insert(ch1817_driver->offhookBcm());
                new_reserved.insert(ch1817_driver->riBcm());
            }
            reserved_bcm_pins = std::move(new_reserved);

            config_mgr.setSetting("adc_source", cfg.adc_source);
            config_mgr.setSetting("rp2040_dev", cfg.rp2040_dev);
            config_mgr.setSetting("adc_sample_rate_hz", std::to_string(cfg.sample_rate_hz));

            res.set_content(json{{"status", "ok"}, {"adc_source", cfg.adc_source}, {"rp2040_dev", cfg.rp2040_dev}, {"sample_rate_hz", cfg.sample_rate_hz}}.dump(), "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"status", "error"}, {"error", e.what()}}.dump(), "application/json");
        }
    });

    svr.Post("/api/system/settings", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = json::parse(req.body.empty() ? "{}" : req.body);
            std::string src = body.value("fsk_source", "auto");
            if (src != "auto" && src != "software_adc") throw std::runtime_error("Illegal FSK source; use auto or software_adc.");
            std::lock_guard<std::mutex> lock(context->config_mutex); json j; { std::ifstream f(context->config_path); if (f.is_open()) { try { f >> j; } catch (...) { j = json::object(); } } } if (!j.is_object()) j = json::object(); j["fsk_source"] = src; std::ofstream out(context->config_path); if (out.is_open()) out << j.dump(2);
            res.set_content(json{{"status","ok"},{"fsk_source",src}}.dump(), "application/json");
        } catch (const std::exception& e) { res.status = 400; res.set_content(json{{"status","error"},{"error",e.what()}}.dump(), "application/json"); }
    });

    svr.Get("/api/adc", [this](const httplib::Request& req, httplib::Response& res) {
        if (!adc_sampler || !adc_sampler->isEnabled()) {
            res.status = 404;
            res.set_content("{\"enabled\":false,\"error\":\"ADC/graph disabled in full GPIO mode\"}", "application/json");
            return;
        }
        size_t points = 1600;
        if (req.has_param("points")) {
            const size_t requested_points = std::strtoul(req.get_param_value("points").c_str(), nullptr, 10);
            points = requested_points == 0 ? 0 : std::max<size_t>(100, std::min<size_t>(5000, requested_points));
        }
        std::string view = req.has_param("view") ? req.get_param_value("view") : "raw";
        const bool effects_override = req.has_param("effects");
        std::string effects = effects_override ? req.get_param_value("effects") : "";
        res.set_content(serialize_adc_scope(points, view, effects, effects_override), "application/json");
    });


    svr.Get("/api/dac", [this](const httplib::Request&, httplib::Response& res) {
        if (!dac_output) {
            res.status = 404;
            res.set_content(json{{"enabled", false}, {"error", "DAC output not configured"}}.dump(), "application/json");
            return;
        }
        res.set_content(json{{"config", dacConfigJson(dac_output->config())}, {"status", dacStatusJson(dac_output->status())}}.dump(), "application/json");
    });

    svr.Get("/api/dac/status", [this](const httplib::Request&, httplib::Response& res) {
        if (!dac_output) {
            res.status = 404;
            res.set_content(json{{"enabled", false}, {"error", "DAC output not configured"}}.dump(), "application/json");
            return;
        }
        res.set_content(dacStatusJson(dac_output->status()).dump(), "application/json");
    });

    svr.Get("/api/audio/uploads", [this](const httplib::Request&, httplib::Response& res) {
        if (!playback_service) { res.status = 404; res.set_content(json{{"status","error"},{"error","Playback service not configured"}}.dump(), "application/json"); return; }
        res.set_content(playback_service->listUploads().dump(), "application/json");
    });

    svr.Post("/api/audio/upload", [this](const httplib::Request& req, httplib::Response& res) {
        if (!playback_service) { res.status = 404; res.set_content(json{{"status","error"},{"error","Playback service not configured"}}.dump(), "application/json"); return; }
        try {
            std::string filename = req.has_param("filename") ? req.get_param_value("filename") : "upload.wav";
            std::string content;
            if (req.is_multipart_form_data() && req.form.has_file("file")) {
                auto file = req.form.get_file("file");
                filename = file.filename.empty() ? filename : file.filename;
                content = file.content;
            } else content = req.body;
            auto meta = playback_service->saveUpload(filename, content);
            res.set_content(json{{"status", "ok"}, {"upload", meta}}.dump(), "application/json");
        } catch (const std::exception& e) { res.status = 400; res.set_content(json{{"status","error"},{"error",e.what()}}.dump(), "application/json"); }
    });

    svr.Post("/api/audio/upload/delete", [this](const httplib::Request& req, httplib::Response& res) {
        if (!playback_service) { res.status = 404; res.set_content(json{{"status","error"},{"error","Playback service not configured"}}.dump(), "application/json"); return; }
        try { json j = json::parse(req.body.empty() ? "{}" : req.body); res.set_content(playback_service->deleteUpload(j.value("id", "")).dump(), "application/json"); }
        catch (const std::exception& e) { res.status = 400; res.set_content(json{{"status","error"},{"error",e.what()}}.dump(), "application/json"); }
    });

    svr.Get("/api/audio/playback/status", [this](const httplib::Request&, httplib::Response& res) {
        if (!playback_service) { res.status = 404; res.set_content(json{{"status","error"},{"error","Playback service not configured"}}.dump(), "application/json"); return; }
        res.set_content(playback_service->status().dump(), "application/json");
    });

    svr.Post("/api/audio/playback/play", [this](const httplib::Request& req, httplib::Response& res) {
        if (!playback_service) { res.status = 404; res.set_content(json{{"status","error"},{"error","Playback service not configured"}}.dump(), "application/json"); return; }
        try {
            json j = json::parse(req.body.empty() ? "{}" : req.body);
            AudioPlaybackService::PlaybackRequest pr;
            pr.id = j.value("id", "");
            pr.channel_mode = j.value("channel_mode", std::string("ch0"));
            pr.use_profiles = j.value("use_profiles", true);
            pr.loop = j.value("loop", false);
            pr.gain = j.value("gain", 1.0);
            if (j.contains("effects") && j["effects"].is_array()) for (const auto& e : j["effects"]) if (e.is_string()) pr.effects.push_back(e.get<std::string>());
            res.set_content(playback_service->play(pr).dump(), "application/json");
        } catch (const std::exception& e) { res.status = 400; res.set_content(json{{"status","error"},{"error",e.what()}}.dump(), "application/json"); }
    });

    svr.Post("/api/audio/playback/stop", [this](const httplib::Request&, httplib::Response& res) {
        if (!playback_service) { res.status = 404; res.set_content(json{{"status","error"},{"error","Playback service not configured"}}.dump(), "application/json"); return; }
        res.set_content(playback_service->stop().dump(), "application/json");
    });

    svr.Post("/api/dac/config", [this](const httplib::Request& req, httplib::Response& res) {
        if (!dac_output) {
            res.status = 404;
            res.set_content(json{{"status", "error"}, {"error", "DAC output not configured"}}.dump(), "application/json");
            return;
        }
        try {
            json j = json::parse(req.body.empty() ? "{}" : req.body);
            DacOutput::Config cfg = dacConfigFromJson(j, dac_output->config());
            dac_output->updateConfig(cfg);
            persistDacSettings(config_mgr, cfg);
            std::set<int> new_reserved;
            if (adc_sampler) new_reserved = adcReservedPinsForConfig(adc_sampler->config());
            std::set<int> dac_pins = dacReservedPinsForConfig(cfg);
            new_reserved.insert(dac_pins.begin(), dac_pins.end());
            if (ch1817_driver && ch1817_driver->settings().enabled) {
                new_reserved.insert(ch1817_driver->offhookBcm());
                new_reserved.insert(ch1817_driver->riBcm());
            }
            reserved_bcm_pins = std::move(new_reserved);
            res.set_content(json{{"status", "ok"}, {"config", dacConfigJson(cfg)}, {"dac_status", dacStatusJson(dac_output->status())}}.dump(), "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"status", "error"}, {"error", e.what()}}.dump(), "application/json");
        }
    });

    svr.Post("/api/dac/write", [this](const httplib::Request& req, httplib::Response& res) {
        if (!dac_output) {
            res.status = 404;
            res.set_content(json{{"status", "error"}, {"error", "DAC output not configured"}}.dump(), "application/json");
            return;
        }
        try {
            json j = json::parse(req.body.empty() ? "{}" : req.body);
            std::string error;
            bool ok = false;
            if (j.contains("volts_a") || j.contains("volts_b")) {
                double va = j.value("volts_a", 0.0);
                double vb = j.value("volts_b", va);
                ok = dac_output->writeVoltsBoth(va, vb, error);
            } else {
                uint16_t ra = static_cast<uint16_t>(j.value("raw_a", j.value("raw", 0)));
                uint16_t rb = static_cast<uint16_t>(j.value("raw_b", j.value("raw", static_cast<int>(ra))));
                ok = dac_output->writeRawBoth(ra, rb, error);
            }
            if (!ok) throw std::runtime_error(error.empty() ? "DAC write failed" : error);
            res.set_content(json{{"status", "ok"}, {"dac_status", dacStatusJson(dac_output->status())}}.dump(), "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"status", "error"}, {"error", e.what()}, {"dac_status", dac_output ? dacStatusJson(dac_output->status()) : json::object()}}.dump(), "application/json");
        }
    });

    svr.Post("/api/dac/control", [this](const httplib::Request& req, httplib::Response& res) {
        if (!dac_output) {
            res.status = 404;
            res.set_content(json{{"status", "error"}, {"error", "DAC output not configured"}}.dump(), "application/json");
            return;
        }
        try {
            json j = json::parse(req.body.empty() ? "{}" : req.body);
            std::string action = j.value("action", "");
            std::string error;
            bool ok = false;
            if (action == "start") ok = dac_output->start(error);
            else if (action == "stop") ok = dac_output->stop(error);
            else if (action == "flush") ok = dac_output->flush(error);
            else if (action == "set_rate") ok = dac_output->setRate(j.at("sample_rate_hz").get<uint32_t>(), error);
            else throw std::runtime_error("Unsupported DAC control action");
            if (!ok) throw std::runtime_error(error.empty() ? "DAC control failed" : error);
            DacOutput::Config cfg = dac_output->config();
            persistDacSettings(config_mgr, cfg);
            res.set_content(json{{"status", "ok"}, {"dac_status", dacStatusJson(dac_output->status())}}.dump(), "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"status", "error"}, {"error", e.what()}, {"dac_status", dac_output ? dacStatusJson(dac_output->status()) : json::object()}}.dump(), "application/json");
        }
    });

    svr.Post("/api/dac/dtmf/play", [this](const httplib::Request& req, httplib::Response& res) {
        if (!dac_output) {
            res.status = 404;
            res.set_content(json{{"status", "error"}, {"error", "DAC output not configured"}}.dump(), "application/json");
            return;
        }
        try {
            json j = json::parse(req.body.empty() ? "{}" : req.body);
            std::string digits = j.value("digits", "");
            uint16_t tone_ms = static_cast<uint16_t>(std::max(40, std::min(2000, j.value("tone_ms", 100))));
            uint16_t gap_ms = static_cast<uint16_t>(std::max(0, std::min(2000, j.value("gap_ms", 50))));
            uint16_t amplitude = static_cast<uint16_t>(std::max(1, std::min(2047, j.value("amplitude", 1200))));
            uint8_t channel_mask = static_cast<uint8_t>(j.value("channel_mask", static_cast<int>(GW_DAC_CHANNEL_A)));
            std::string error;
            if (!dac_output->playDtmf(digits, tone_ms, gap_ms, amplitude, channel_mask, error)) throw std::runtime_error(error.empty() ? "DTMF play failed" : error);
            res.set_content(json{{"status", "ok"}, {"dac_status", dacStatusJson(dac_output->status())}}.dump(), "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"status", "error"}, {"error", e.what()}, {"dac_status", dac_output ? dacStatusJson(dac_output->status()) : json::object()}}.dump(), "application/json");
        }
    });

    svr.Post("/api/dac/dtmf/stop", [this](const httplib::Request&, httplib::Response& res) {
        if (!dac_output) {
            res.status = 404;
            res.set_content(json{{"status", "error"}, {"error", "DAC output not configured"}}.dump(), "application/json");
            return;
        }
        try {
            std::string error;
            if (!dac_output->stopDtmf(error)) throw std::runtime_error(error.empty() ? "DTMF stop failed" : error);
            res.set_content(json{{"status", "ok"}, {"dac_status", dacStatusJson(dac_output->status())}}.dump(), "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"status", "error"}, {"error", e.what()}, {"dac_status", dac_output ? dacStatusJson(dac_output->status()) : json::object()}}.dump(), "application/json");
        }
    });


    svr.Get("/api/dac/dtmf/decoder/status", [this](const httplib::Request&, httplib::Response& res) {
        if (!adc_sampler) {
            res.status = 404;
            res.set_content(json{{"status", "error"}, {"error", "RP2040/ADC sampler not configured"}}.dump(), "application/json");
            return;
        }
        res.set_content(mcuPeripheralStatusJson(adc_sampler->mcuPeripheralSnapshot())["dtmf_decoder"].dump(), "application/json");
    });

    svr.Post("/api/dac/dtmf/decoder/history/clear", [this](const httplib::Request&, httplib::Response& res) {
        if (!adc_sampler) {
            res.status = 404;
            res.set_content(json{{"status", "error"}, {"error", "RP2040/ADC sampler not configured"}}.dump(), "application/json");
            return;
        }
        adc_sampler->clearMcuDtmfHistory();
        res.set_content(json{{"ok", true}, {"cleared", true}}.dump(), "application/json");
    });

    svr.Get("/api/mcu/peripherals/status", [this](const httplib::Request&, httplib::Response& res) {
        if (!adc_sampler) {
            res.status = 404;
            res.set_content(json{{"status", "error"}, {"error", "RP2040/ADC sampler not configured"}}.dump(), "application/json");
            return;
        }
        res.set_content(mcuPeripheralStatusJson(adc_sampler->mcuPeripheralSnapshot()).dump(), "application/json");
    });

    svr.Post("/api/mcu/peripherals/config", [this](const httplib::Request& req, httplib::Response& res) {
        if (!adc_sampler) {
            res.status = 404;
            res.set_content(json{{"status", "error"}, {"error", "RP2040/ADC sampler not configured"}}.dump(), "application/json");
            return;
        }
        try {
            json body = json::parse(req.body.empty() ? "{}" : req.body);
            McuPeripheralConfig cfg = mcuPeripheralConfigFromJson(body.contains("mcu_peripherals") ? body : json{{"mcu_peripherals", body}}, adc_sampler->mcuPeripheralConfig());
            {
                std::lock_guard<std::mutex> cfg_lock(context->config_mutex);
                json root;
                { std::ifstream f(context->config_path); if (f.is_open()) { try { f >> root; } catch (...) { root = json::object(); } } }
                if (!root.is_object()) root = json::object();
                root["mcu_peripherals"] = mcuPeripheralConfigToJson(cfg);
                std::ofstream out(context->config_path);
                if (out.is_open()) out << root.dump(2);
            }
            adc_sampler->updateMcuPeripheralConfig(cfg);
            res.set_content(json{{"status", "ok"}, {"mcu_peripherals", mcuPeripheralStatusJson(adc_sampler->mcuPeripheralSnapshot())}}.dump(), "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"status", "error"}, {"error", e.what()}}.dump(), "application/json");
        }
    });

    svr.Get("/api/caller-id", [this](const httplib::Request&, httplib::Response& res) {
        const std::string source = readFskSourceSetting(context);
        json out = selectedCallerIdJson(source, caller_id_detector);
        out["configured_source"] = source;
        res.set_content(out.dump(), "application/json");
    });

    svr.Get("/api/caller-id/settings", [this](const httplib::Request&, httplib::Response& res) {
        if (!caller_id_detector) {
            res.status = 404;
            res.set_content("{\"enabled\":false,\"error\":\"Caller ID detector disabled\"}", "application/json");
            return;
        }
        res.set_content(caller_id_detector->settingsJson().dump(), "application/json");
    });

    svr.Post("/api/caller-id/settings", [this](const httplib::Request& req, httplib::Response& res) {
        if (!caller_id_detector) {
            res.status = 404;
            res.set_content("{\"status\":\"error\",\"error\":\"Caller ID detector disabled\"}", "application/json");
            return;
        }
        try {
            json j = json::parse(req.body.empty() ? "{}" : req.body);
            caller_id_detector->updateSettingsFromJson(j);
            json out = {{"status", "ok"}, {"settings", caller_id_detector->settingsJson()}};
            res.set_content(out.dump(), "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            json out = {{"status", "error"}, {"error", e.what()}};
            res.set_content(out.dump(), "application/json");
        }
    });

    svr.Get("/api/telephony/hardware-check", [this](const httplib::Request&, httplib::Response& res) {
        if (!telephony_diagnostics) { res.status = 404; res.set_content("{\"status\":\"error\",\"error\":\"Telephony diagnostics disabled\"}", "application/json"); return; }
        res.set_content(telephony_diagnostics->hardwareCheck().dump(), "application/json");
    });

    svr.Get("/api/telephony/diagnostics/export", [this](const httplib::Request&, httplib::Response& res) {
        if (!telephony_diagnostics) { res.status = 404; res.set_content("{\"status\":\"error\",\"error\":\"Telephony diagnostics disabled\"}", "application/json"); return; }
        res.set_content(telephony_diagnostics->diagnosticsExport().dump(), "application/json");
    });

    svr.Get("/api/telephony/events", [this](const httplib::Request&, httplib::Response& res) {
        if (!telephony_diagnostics) { res.status = 404; res.set_content("{\"events\":[]}", "application/json"); return; }
        res.set_content(telephony_diagnostics->eventsJson().dump(), "application/json");
    });

    svr.Get("/api/telephony/validation-checklist", [this](const httplib::Request&, httplib::Response& res) {
        if (!telephony_diagnostics) { res.status = 404; res.set_content("{\"items\":[]}", "application/json"); return; }
        res.set_content(telephony_diagnostics->validationChecklist().dump(), "application/json");
    });

    svr.Get("/api/telephony/calibration/settings", [this](const httplib::Request&, httplib::Response& res) {
        if (!telephony_diagnostics) { res.status = 404; res.set_content("{\"status\":\"error\",\"error\":\"Telephony diagnostics disabled\"}", "application/json"); return; }
        res.set_content(telephony_diagnostics->calibrationSettingsJson().dump(), "application/json");
    });

    svr.Post("/api/telephony/calibration/settings", [this](const httplib::Request& req, httplib::Response& res) {
        if (!telephony_diagnostics) { res.status = 404; res.set_content("{\"status\":\"error\",\"error\":\"Telephony diagnostics disabled\"}", "application/json"); return; }
        try { json j = json::parse(req.body.empty() ? "{}" : req.body); telephony_diagnostics->updateCalibrationSettingsFromJson(j); res.set_content(json{{"status","ok"},{"settings",telephony_diagnostics->calibrationSettingsJson()}}.dump(), "application/json"); }
        catch (const std::exception& e) { res.status = 400; res.set_content(json{{"status","error"},{"error",e.what()}}.dump(), "application/json"); }
    });

    svr.Post("/api/telephony/calibration/rcv-noise-floor", [this](const httplib::Request& req, httplib::Response& res) {
        if (!telephony_diagnostics) { res.status = 404; res.set_content("{\"status\":\"error\",\"error\":\"Telephony diagnostics disabled\"}", "application/json"); return; }
        try { json j = json::parse(req.body.empty() ? "{}" : req.body); res.set_content(telephony_diagnostics->calibrateRcvNoiseFloor(j.value("duration_ms", 3000), j.value("window_ms", 100)).dump(), "application/json"); }
        catch (const std::exception& e) { res.status = 400; res.set_content(json{{"status","error"},{"error",e.what()}}.dump(), "application/json"); }
    });

    svr.Post("/api/telephony/calibration/tone-scan", [this](const httplib::Request& req, httplib::Response& res) {
        if (!telephony_diagnostics) { res.status = 404; res.set_content("{\"status\":\"error\",\"error\":\"Telephony diagnostics disabled\"}", "application/json"); return; }
        try { json j = json::parse(req.body.empty() ? "{}" : req.body); res.set_content(telephony_diagnostics->toneScan(j.value("duration_ms", 1000), j.value("region", std::string("nanp"))).dump(), "application/json"); }
        catch (const std::exception& e) { res.status = 400; res.set_content(json{{"status","error"},{"error",e.what()}}.dump(), "application/json"); }
    });

    svr.Post("/api/telephony/calibration/capture-disconnect-profile", [this](const httplib::Request& req, httplib::Response& res) {
        if (!telephony_diagnostics) { res.status = 404; res.set_content("{\"status\":\"error\",\"error\":\"Telephony diagnostics disabled\"}", "application/json"); return; }
        try { json j = json::parse(req.body.empty() ? "{}" : req.body); res.set_content(telephony_diagnostics->captureDisconnectProfile(j.value("duration_ms", 3000), j.value("label", std::string("ata_custom"))).dump(), "application/json"); }
        catch (const std::exception& e) { res.status = 400; res.set_content(json{{"status","error"},{"error",e.what()}}.dump(), "application/json"); }
    });

    svr.Post("/api/telephony/calibration/apply-line-thresholds", [this](const httplib::Request& req, httplib::Response& res) {
        if (!telephony_diagnostics || !line_state_detector) { res.status = 404; res.set_content("{\"status\":\"error\",\"error\":\"Diagnostics or line state disabled\"}", "application/json"); return; }
        try { json j = json::parse(req.body.empty() ? "{}" : req.body); auto s = line_state_detector->settings(); s.silence_rms = j.value("silence_rms", s.silence_rms); s.min_rms = j.value("min_rms", s.min_rms); line_state_detector->updateSettings(s); res.set_content(json{{"status","ok"},{"line_state_settings",line_state_detector->settingsJson()}}.dump(), "application/json"); }
        catch (const std::exception& e) { res.status = 400; res.set_content(json{{"status","error"},{"error",e.what()}}.dump(), "application/json"); }
    });

    svr.Get("/api/telephony/state", [this](const httplib::Request&, httplib::Response& res) {
        if (!telephony_coordinator) { res.status = 404; res.set_content("{\"enabled\":false,\"error\":\"Telephony coordinator disabled\"}", "application/json"); return; }
        res.set_content(telephony_coordinator->snapshotJson().dump(), "application/json");
    });

    svr.Get("/api/telephony/state/settings", [this](const httplib::Request&, httplib::Response& res) {
        if (!telephony_coordinator) { res.status = 404; res.set_content("{\"enabled\":false,\"error\":\"Telephony coordinator disabled\"}", "application/json"); return; }
        res.set_content(telephony_coordinator->settingsJson().dump(), "application/json");
    });

    svr.Post("/api/telephony/state/settings", [this](const httplib::Request& req, httplib::Response& res) {
        if (!telephony_coordinator) { res.status = 404; res.set_content("{\"status\":\"error\",\"error\":\"Telephony coordinator disabled\"}", "application/json"); return; }
        try { json j = json::parse(req.body.empty() ? "{}" : req.body); telephony_coordinator->updateSettingsFromJson(j); res.set_content(json{{"status","ok"},{"settings",telephony_coordinator->settingsJson()}}.dump(), "application/json"); }
        catch (const std::exception& e) { res.status = 400; res.set_content(json{{"status","error"},{"error",e.what()}}.dump(), "application/json"); }
    });

    svr.Get("/api/telephony/ch1817", [this](const httplib::Request&, httplib::Response& res) {
        if (!ch1817_driver) { res.status = 404; res.set_content("{\"enabled\":false,\"error\":\"CH1817 driver disabled\"}", "application/json"); return; }
        res.set_content(ch1817_driver->snapshotJson().dump(), "application/json");
    });

    svr.Get("/api/telephony/line-state", [this](const httplib::Request&, httplib::Response& res) {
        if (!line_state_detector) { res.status = 404; res.set_content("{\"enabled\":false,\"error\":\"Line state detector disabled\"}", "application/json"); return; }
        res.set_content(line_state_detector->snapshotJson().dump(), "application/json");
    });

    svr.Get("/api/telephony/line-state/settings", [this](const httplib::Request&, httplib::Response& res) {
        if (!line_state_detector) { res.status = 404; res.set_content("{\"enabled\":false,\"error\":\"Line state detector disabled\"}", "application/json"); return; }
        res.set_content(line_state_detector->settingsJson().dump(), "application/json");
    });

    svr.Post("/api/telephony/line-state/settings", [this](const httplib::Request& req, httplib::Response& res) {
        if (!line_state_detector) { res.status = 404; res.set_content("{\"status\":\"error\",\"error\":\"Line state detector disabled\"}", "application/json"); return; }
        try { json j = json::parse(req.body.empty() ? "{}" : req.body); line_state_detector->updateSettingsFromJson(j); res.set_content(json{{"status","ok"},{"settings",line_state_detector->settingsJson()}}.dump(), "application/json"); }
        catch (const std::exception& e) { res.status = 400; res.set_content(json{{"status","error"},{"error",e.what()}}.dump(), "application/json"); }
    });

    svr.Post("/api/telephony/ch1817/settings", [this](const httplib::Request& req, httplib::Response& res) {
        if (!ch1817_driver) { res.status = 404; res.set_content("{\"status\":\"error\",\"error\":\"CH1817 driver disabled\"}", "application/json"); return; }
        try { json j = json::parse(req.body.empty() ? "{}" : req.body); ch1817_driver->updateFromJson(j); res.set_content(json{{"status","ok"},{"settings",ch1817_driver->settingsJson()}}.dump(), "application/json"); }
        catch (const std::exception& e) { res.status = 400; res.set_content(json{{"status","error"},{"error",e.what()},{"help",ch1817_driver->validationHelp()}}.dump(), "application/json"); }
    });

    svr.Post("/api/telephony/ch1817/offhook", [this](const httplib::Request& req, httplib::Response& res) {
        if (!ch1817_driver) { res.status = 404; res.set_content("{\"status\":\"error\",\"error\":\"CH1817 driver disabled\"}", "application/json"); return; }
        try { json j = json::parse(req.body.empty() ? "{}" : req.body); ch1817_driver->setOffhook(j.value("offhook", false)); res.set_content(json{{"status","ok"},{"state",ch1817_driver->snapshotJson()}}.dump(), "application/json"); }
        catch (const std::exception& e) { res.status = 400; res.set_content(json{{"status","error"},{"error",e.what()},{"help",ch1817_driver->validationHelp()}}.dump(), "application/json"); }
    });

    svr.Get("/api/audio/modules", [this](const httplib::Request&, httplib::Response& res) {
        res.set_content(AudioProcessing::catalog().dump(), "application/json");
    });

    svr.Get("/api/audio/filter-profiles", [this](const httplib::Request&, httplib::Response& res) {
        if (!filter_profiles) {
            res.status = 404;
            res.set_content(json{{"status", "error"}, {"error", "Filter profile manager not configured"}}.dump(), "application/json");
            return;
        }
        json catalog = AudioProcessing::catalog();
        json out = {{"status", "ok"},
                    {"effects", catalog.value("effects", json::array())},
                    {"codecs", catalog.value("codecs", json::array())},
                    {"contexts", filter_profiles->contextsJson()},
                    {"defaults", filter_profiles->defaultsJson()},
                    {"profiles", filter_profiles->allProfilesJson()}};
        res.set_content(out.dump(), "application/json");
    });

    svr.Get("/api/audio/filter-profile", [this](const httplib::Request& req, httplib::Response& res) {
        if (!filter_profiles) {
            res.status = 404;
            res.set_content(json{{"status", "error"}, {"error", "Filter profile manager not configured"}}.dump(), "application/json");
            return;
        }
        try {
            std::string ctx = req.has_param("context") ? req.get_param_value("context") : "";
            if (ctx.empty()) throw std::runtime_error("Missing context query parameter");
            res.set_content(json{{"status", "ok"}, {"profile", filter_profiles->profileJson(ctx)}}.dump(), "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"status", "error"}, {"error", e.what()}}.dump(), "application/json");
        }
    });

    svr.Post("/api/audio/filter-profile", [this](const httplib::Request& req, httplib::Response& res) {
        if (!filter_profiles) {
            res.status = 404;
            res.set_content(json{{"status", "error"}, {"error", "Filter profile manager not configured"}}.dump(), "application/json");
            return;
        }
        try {
            json j = json::parse(req.body.empty() ? "{}" : req.body);
            std::string action = j.value("action", "update");
            std::string ctx = j.value("context", "");
            if (ctx.empty()) throw std::runtime_error("Missing filter profile context");
            FilterProfileManager::Profile p;
            if (action == "restore_default") p = filter_profiles->restoreDefault(ctx);
            else p = filter_profiles->updateProfileFromJson(j);
            res.set_content(json{{"status", "ok"}, {"context", ctx}, {"profile", FilterProfileManager::profileToJson(p)}}.dump(), "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"status", "error"}, {"error", e.what()}}.dump(), "application/json");
        }
    });

    svr.Get("/api/adc/wav", [this](const httplib::Request& req, httplib::Response& res) {
        if (!adc_sampler || !adc_sampler->isEnabled()) {
            res.status = 404;
            res.set_content("{\"status\":\"error\",\"error\":\"ADC/graph disabled in GPIO-only mode\"}", "application/json");
            return;
        }

        size_t duration_ms = 1000;
        if (req.has_param("ms")) {
            duration_ms = std::strtoul(req.get_param_value("ms").c_str(), nullptr, 10);
        } else if (req.has_param("duration_ms")) {
            duration_ms = std::strtoul(req.get_param_value("duration_ms").c_str(), nullptr, 10);
        }
        duration_ms = std::max<size_t>(1, std::min<size_t>(600000, duration_ms));

        std::string mode = req.has_param("mode") ? req.get_param_value("mode") : "ch0";
        std::string effects = req.has_param("effects") ? req.get_param_value("effects") : "";
        if (!req.has_param("effects") && filter_profiles) {
            std::string profile = req.has_param("profile") ? req.get_param_value("profile") : wavProfileContextForMode(mode);
            effects = joinEffectsCsv(filter_profiles->effectiveEffects(profile));
        }
        std::string codec = req.has_param("codec") ? req.get_param_value("codec") : "pcm16";
        std::string filename;
        std::string content_type;
        try {
            size_t requested_ms = 0, actual_ms = 0, available_ms = 0;
            bool truncated = false;
            std::string payload = build_adc_wav(mode, duration_ms, effects, codec, filename, content_type,
                                                requested_ms, actual_ms, available_ms, truncated);
            res.set_header("Content-Disposition", "attachment; filename=\"" + filename + "\"");
            res.set_header("Cache-Control", "no-store");
            res.set_header("X-Requested-Duration-Ms", std::to_string(requested_ms));
            res.set_header("X-Actual-Duration-Ms", std::to_string(actual_ms));
            res.set_header("X-Available-Duration-Ms", std::to_string(available_ms));
            res.set_header("X-Wav-Truncated", truncated ? "true" : "false");
            res.set_content(payload.data(), payload.size(), content_type.c_str());
        } catch (const std::exception& e) {
            res.status = 400;
            json j = {{"status", "error"}, {"error", e.what()}};
            res.set_content(j.dump(), "application/json");
        }
    });

    svr.Post("/api/config", [this](const httplib::Request& req, httplib::Response& res) {
        bool changed = false;
        std::string action = req.get_param_value("action");
        std::string val = req.get_param_value("value");

        if (action == "timeout") {
            if (!val.empty()) {
                int new_t = std::stoi(val);
                context->timeout_ms = new_t;
                gpio_mgr.updateTimeout(new_t);
                changed = true;
            }
        } else {
            std::string pin_str = req.get_param_value("pin");
            if (!pin_str.empty()) {
                int phys = std::stoi(pin_str);
                if (registry.count(phys)) {
                    auto pin = registry[phys];
                    if (reserved_bcm_pins.count(pin->bcm_pin)) {
                        res.status = 403;
                        res.set_content("{\"status\":\"error\",\"error\":\"GPIO is reserved for ADC/SPI\"}", "application/json");
                        return;
                    }
                    std::lock_guard<std::mutex> lock(pin->mtx);
                    if (action == "mode") {
                        pin->is_output = (val == "out");
                        changed = true;
                    } else if (action == "state") {
                        pin->target_output_state = (val == "true");
                        changed = true;
                    }
                }
            }
        }
        
        if (changed) {
            std::lock_guard<std::mutex> lock(context->config_mutex);
            config_mgr.save(registry, context->timeout_ms);
        }
        res.set_content("{\"status\":\"ok\"}", "application/json");
    });
}

std::string WebServer::serialize_state() {
    json j;
    j["timeout_ms"] = context->timeout_ms.load();
    if (dac_output) {
        j["dac"] = dacStatusJson(dac_output->status());
        j["dac_config"] = dacConfigJson(dac_output->config());
    } else {
        j["dac"] = {{"enabled", false}, {"healthy", false}, {"last_error", "DAC output not configured"}};
    }
    if (adc_sampler) j["mcu_peripherals"] = mcuPeripheralStatusJson(adc_sampler->mcuPeripheralSnapshot());
    else j["mcu_peripherals"] = mcuPeripheralStatusJson(McuPeripheralSnapshot());
    j["pins"] = json::object();

    for (const auto& [phys, pin] : registry) {
        std::lock_guard<std::mutex> lock(pin->mtx);
        j["pins"][std::to_string(phys)] = {
            {"physical_pin", pin->physical_pin},
            {"bcm_pin", pin->bcm_pin},
            {"is_output", pin->is_output},
            {"state", pin->current_state},
            {"is_transitioning", pin->is_transitioning},
            {"frequency", pin->average_frequency}
        };
    }
    return j.dump();
}

std::string WebServer::serialize_adc_scope(size_t max_points, const std::string& view, const std::string& effects_csv, bool effects_override) {
    json j;
    if (!adc_sampler) {
        j = {
            {"enabled", false}, {"running", false}, {"healthy", false}, {"bitbang", false},
            {"sample_rate_hz", 0}, {"measured_sample_rate_hz", 0}, {"latest_raw", {0, 0}},
            {"latest_volts", {0.0, 0.0}}, {"total_frames", 0}, {"dropped_reads", 0},
            {"last_error", "ADC sampler not configured"}, {"graph_view", "raw"}, {"graph_effects", ""},
            {"samples", {{"ch0", json::array()}, {"ch1", json::array()}}}
        };
        return j.dump();
    }

    const std::string safe_view = (view == "filtered" || view == "overlay") ? view : "raw";
    auto s = (max_points == 0) ? adc_sampler->status() : adc_sampler->snapshot(max_points);
    const uint32_t sr = s.measured_sample_rate_hz ? s.measured_sample_rate_hz : s.sample_rate_hz;

    j["enabled"] = s.enabled;
    j["running"] = s.running;
    j["healthy"] = s.healthy;
    j["bitbang"] = s.bitbang;
    j["sample_rate_hz"] = s.sample_rate_hz;
    j["measured_sample_rate_hz"] = s.measured_sample_rate_hz;
    j["lifetime_sample_rate_hz"] = s.lifetime_sample_rate_hz;
    j["latest_raw"] = {s.latest_raw[0], s.latest_raw[1]};
    j["latest_volts"] = {s.latest_volts[0], s.latest_volts[1]};
    j["total_frames"] = s.total_frames;
    j["dropped_reads"] = s.dropped_reads;
    j["overruns"] = s.overruns;
    j["max_overrun_us"] = s.max_overrun_us;
    j["avg_frame_read_us"] = s.avg_frame_read_us;
    j["max_frame_read_us"] = s.max_frame_read_us;
    j["avg_mutex_wait_us"] = s.avg_mutex_wait_us;
    j["max_mutex_wait_us"] = s.max_mutex_wait_us;
    j["avg_mutex_hold_us"] = s.avg_mutex_hold_us;
    j["max_mutex_hold_us"] = s.max_mutex_hold_us;
    j["snapshot_count"] = s.snapshot_count;
    j["snapshot_samples_copied"] = s.snapshot_samples_copied;
    j["realtime_requested"] = s.realtime_requested;
    j["realtime_active"] = s.realtime_active;
    j["realtime_priority"] = s.realtime_priority;
    j["cpu_affinity"] = s.cpu_affinity;
    j["scheduler_status"] = s.scheduler_status;
    j["valid_samples"] = s.valid_samples;
    j["adc_source"] = s.adc_source;
    j["rp2040_connected"] = s.rp2040_connected;
    j["rp2040_packets_ok"] = s.rp2040_packets_ok;
    j["rp2040_packets_crc_bad"] = s.rp2040_packets_crc_bad;
    j["rp2040_sequence_gaps"] = s.rp2040_sequence_gaps;
    j["rp2040_firmware_lost_frames"] = s.rp2040_firmware_lost_frames;
    j["rp2040_firmware_flags"] = s.rp2040_firmware_flags;
    j["rp2040_dev"] = s.rp2040_dev;
    j["rp2040_declared_rate_hz"] = s.rp2040_declared_rate_hz;
    j["device_protocol_active"] = s.device_protocol_active;
    j["device_protocol"] = s.device_protocol;
    j["device_transport"] = s.device_transport;
    j["device_adc_stream_id"] = s.device_adc_stream_id;
    j["device_channel_count"] = s.device_channel_count;
    j["device_sample_format"] = s.device_sample_format;
    j["device_caps_max_rate_hz"] = s.device_caps_max_rate_hz;
    j["device_caps_formats"] = s.device_caps_formats;
    j["history_capacity_samples"] = s.history_capacity_samples;
    j["available_history_ms"] = (static_cast<uint64_t>(s.valid_samples) * 1000ull) / std::max<uint32_t>(1, sr);
    j["history_capacity_ms"] = (static_cast<uint64_t>(s.history_capacity_samples) * 1000ull) / std::max<uint32_t>(1, sr);
    j["last_error"] = s.last_error;
    std::string effects0 = effects_csv;
    std::string effects1 = effects_csv;
    if (!effects_override && filter_profiles) {
        effects0 = joinEffectsCsv(filter_profiles->effectiveEffects("adc.graph.ch0"));
        effects1 = joinEffectsCsv(filter_profiles->effectiveEffects("adc.graph.ch1"));
    }
    const std::string graph_effects = (effects0 == effects1) ? effects0 : ("ch0:" + effects0 + "|ch1:" + effects1);
    j["graph_view"] = safe_view;
    j["graph_effects"] = graph_effects;
    j["graph_effects_override"] = effects_override;
    j["graph_filter_profiles"] = {{"ch0", "adc.graph.ch0"}, {"ch1", "adc.graph.ch1"}};
    json skipped = json::array();
    for (const auto& effect : splitEffectsCsv(effects0 + "," + effects1)) {
        if (effect == "rnnoise") skipped.push_back("rnnoise");
    }
    j["graph_skipped_effects"] = skipped;

    if (safe_view == "raw") {
        j["samples"] = {{"ch0", s.samples[0]}, {"ch1", s.samples[1]}};
    } else {
        auto f0 = applyGraphEffectsToChannel(s.samples[0], sr, effects0);
        auto f1 = applyGraphEffectsToChannel(s.samples[1], sr, effects1);
        if (safe_view == "filtered") {
            j["samples"] = {{"ch0", f0}, {"ch1", f1}};
        } else {
            j["samples"] = {{"ch0", s.samples[0]}, {"ch1", s.samples[1]}};
            j["filtered_samples"] = {{"ch0", f0}, {"ch1", f1}};
        }
    }
    return j.dump();
}

std::string WebServer::build_adc_wav(const std::string& mode, size_t duration_ms, const std::string& effects_csv, const std::string& codec,
                                    std::string& filename, std::string& content_type, size_t& requested_ms, size_t& actual_ms,
                                    size_t& available_ms, bool& truncated) {
    if (!adc_sampler) throw std::runtime_error("ADC sampler not configured");

    const auto options = AudioProcessing::parseOptions(effects_csv, codec);
    const auto meta = adc_sampler->status();
    const uint32_t sample_rate = std::max<uint32_t>(1, meta.measured_sample_rate_hz ? meta.measured_sample_rate_hz : meta.sample_rate_hz);
    // Duration selects how much ring-buffer history to copy. Use the measured rate so the downloaded WAV
    // has both the requested duration and the correct playback pitch even if Linux can't hit the requested rate exactly.
    requested_ms = duration_ms;
    const size_t requested_frames = std::max<size_t>(1, (static_cast<uint64_t>(sample_rate) * duration_ms + 999) / 1000);
    const auto data = adc_sampler->recent(requested_frames);
    const size_t frames = std::min(data.samples[0].size(), data.samples[1].size());
    if (frames == 0) throw std::runtime_error("No ADC samples available yet");
    available_ms = (static_cast<uint64_t>(data.valid_samples) * 1000ull) / std::max<uint32_t>(1, sample_rate);
    actual_ms = (static_cast<uint64_t>(frames) * 1000ull) / std::max<uint32_t>(1, sample_rate);
    truncated = frames < requested_frames;

    enum class WavMode { Ch0, Ch1, Stereo, Mix };
    WavMode wav_mode;
    std::string safe_mode;
    if (mode == "ch0" || mode == "mono-ch0") { wav_mode = WavMode::Ch0; safe_mode = "ch0"; }
    else if (mode == "ch1" || mode == "mono-ch1") { wav_mode = WavMode::Ch1; safe_mode = "ch1"; }
    else if (mode == "stereo" || mode == "ch0ch1") { wav_mode = WavMode::Stereo; safe_mode = "stereo"; }
    else if (mode == "mix" || mode == "mono-mix") { wav_mode = WavMode::Mix; safe_mode = "mix"; }
    else throw std::runtime_error("Invalid WAV mode; use ch0, ch1, stereo, or mix");

    auto pcm16 = [](uint16_t raw) -> int16_t {
        const int32_t centered = static_cast<int32_t>(std::max<uint16_t>(0, std::min<uint16_t>(4095, raw))) - 2048;
        return static_cast<int16_t>(std::max<int32_t>(-32768, std::min<int32_t>(32767, centered * 16)));
    };

    AudioBuffer buffer;
    buffer.sample_rate_hz = sample_rate;
    buffer.channels = (wav_mode == WavMode::Stereo) ? 2 : 1;
    buffer.samples.reserve(frames * buffer.channels);

    for (size_t i = 0; i < frames; ++i) {
        const int16_t ch0 = pcm16(data.samples[0][i]);
        const int16_t ch1 = pcm16(data.samples[1][i]);
        switch (wav_mode) {
            case WavMode::Ch0:
                buffer.samples.push_back(ch0);
                break;
            case WavMode::Ch1:
                buffer.samples.push_back(ch1);
                break;
            case WavMode::Stereo:
                buffer.samples.push_back(ch0);
                buffer.samples.push_back(ch1);
                break;
            case WavMode::Mix:
                buffer.samples.push_back(static_cast<int16_t>((static_cast<int32_t>(ch0) + static_cast<int32_t>(ch1)) / 2));
                break;
        }
    }

    AudioProcessing::applyEffects(buffer, options);
    std::string payload = AudioProcessing::encodeWav(buffer, options);

    std::string effects_part = options.effects.empty() ? "dry" : AudioProcessing::sanitizeForFilename(effects_csv);
    std::string source_prefix = "adc";
    if (meta.adc_source == "rp2040") source_prefix = "rp2040";
    else if (meta.adc_source == "mcp3202-spidev") source_prefix = "mcp3202";
    filename = source_prefix + "_" + safe_mode + "_" + effects_part + "_" + std::to_string(actual_ms) + "ms_" + std::to_string(sample_rate) + "hz.wav";
    content_type = "audio/wav";
    return payload;
}

void WebServer::listen(const std::string& host, int port) {
    svr.listen(host.c_str(), port);
}
