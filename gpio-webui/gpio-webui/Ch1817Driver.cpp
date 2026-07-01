#include "Ch1817Driver.hpp"
#include "AdcSampler.hpp"
#include "HeaderPins.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>

using json = nlohmann::json;

namespace {
int clampInt(int v, int lo, int hi) { return std::max(lo, std::min(hi, v)); }
bool parseBoolish(const json& j, const char* key, bool def) {
    if (!j.contains(key)) return def;
    if (j[key].is_boolean()) return j[key].get<bool>();
    if (j[key].is_number_integer()) return j[key].get<int>() != 0;
    if (j[key].is_string()) {
        std::string s = j[key].get<std::string>();
        std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        return s == "1" || s == "true" || s == "yes" || s == "on" || s == "offhook";
    }
    return def;
}
}

Ch1817Driver::Ch1817Driver(std::shared_ptr<SystemContext> context, AdcSampler* adc_sampler)
    : context_(std::move(context)), adc_sampler_(adc_sampler) {
    if (adc_sampler_) mcu_periph_config_ = adc_sampler_->mcuPeripheralConfig();
    else if (context_) mcu_periph_config_ = loadMcuPeripheralConfigFromFile(context_->config_path, mcu_periph_config_);
    using_mcu_ri_ = mcu_periph_config_.ri.source == "mcu";
    using_mcu_oh_ = mcu_periph_config_.oh.source == "mcu";
    load();
}

Ch1817Driver::~Ch1817Driver() { stop(); }

void Ch1817Driver::validate(const Ch1817Settings& s) {
    if (!s.enabled) return;
    requireDistinctEnabledPins("CH1817", {{"OFFHK", s.offhook_phys}, {"RI", s.ri_phys}});
    if (s.auto_answer_delay_ms < 0 || s.auto_answer_delay_ms > 600000) {
        throw std::runtime_error("CH1817 illegal setting: auto-answer delay must be 0..600000 ms. " + gpioHelpText());
    }
}

Ch1817Settings Ch1817Driver::settingsFromJson(const json& j, const Ch1817Settings& defaults) {
    Ch1817Settings s = defaults;
    if (!j.is_object()) return s;
    s.enabled = parseBoolish(j, "enabled", s.enabled);
    s.offhook_phys = clampInt(j.value("offhook_phys", s.offhook_phys), 1, 40);
    s.ri_phys = clampInt(j.value("ri_phys", s.ri_phys), 1, 40);
    s.offhook = parseBoolish(j, "offhook", s.offhook);
    s.auto_answer_enabled = parseBoolish(j, "auto_answer_enabled", s.auto_answer_enabled);
    s.auto_answer_delay_ms = clampInt(j.value("auto_answer_delay_ms", s.auto_answer_delay_ms), 0, 600000);
    validate(s);
    return s;
}

json Ch1817Driver::settingsToJson(const Ch1817Settings& s) {
    return {
        {"enabled", s.enabled},
        {"offhook_phys", s.offhook_phys},
        {"offhook_bcm", s.enabled ? bcmForPhysicalPin(s.offhook_phys) : -1},
        {"ri_phys", s.ri_phys},
        {"ri_bcm", s.enabled ? bcmForPhysicalPin(s.ri_phys) : -1},
        {"offhook", s.offhook},
        {"auto_answer_enabled", s.auto_answer_enabled},
        {"auto_answer_delay_ms", s.auto_answer_delay_ms}
    };
}

void Ch1817Driver::load() {
    if (!context_) return;
    std::lock_guard<std::mutex> cfg_lock(context_->config_mutex);
    std::ifstream f(context_->config_path);
    if (!f.is_open()) return;
    try {
        json j; f >> j;
        if (j.contains("ch1817")) {
            std::lock_guard<std::mutex> lock(mtx_);
            settings_ = settingsFromJson(j["ch1817"], settings_);
        }
    } catch (const std::exception& e) {
        std::cerr << "[CH1817] Config load warning: " << e.what() << std::endl;
    }
}

void Ch1817Driver::saveLocked() {
    if (!context_) return;
    std::lock_guard<std::mutex> cfg_lock(context_->config_mutex);
    json j;
    { std::ifstream f(context_->config_path); if (f.is_open()) { try { f >> j; } catch (...) { j = json::object(); } } }
    if (!j.is_object()) j = json::object();
    j["ch1817"] = settingsToJson(settings_);
    std::ofstream out(context_->config_path);
    if (out.is_open()) out << j.dump(2);
}

void Ch1817Driver::start() {
    if (running_) return;
    validate(settings_);
    running_ = true;
    worker_ = std::thread(&Ch1817Driver::worker, this);
}

void Ch1817Driver::stop() {
    running_ = false;
    if (worker_.joinable()) worker_.join();
}

Ch1817Settings Ch1817Driver::settings() const { std::lock_guard<std::mutex> lock(mtx_); return settings_; }
json Ch1817Driver::settingsJson() const { return settingsToJson(settings()); }
int Ch1817Driver::offhookBcm() const { auto s = settings(); return s.enabled ? bcmForPhysicalPin(s.offhook_phys) : -1; }
int Ch1817Driver::riBcm() const { auto s = settings(); return s.enabled ? bcmForPhysicalPin(s.ri_phys) : -1; }
std::string Ch1817Driver::validationHelp() const { return gpioHelpText(); }

void Ch1817Driver::updateFromJson(const json& j) {
    std::lock_guard<std::mutex> lock(mtx_);
    Ch1817Settings next = settingsFromJson(j, settings_);
    const bool pin_change = next.enabled != settings_.enabled || next.offhook_phys != settings_.offhook_phys || next.ri_phys != settings_.ri_phys;
    if (pin_change && running_) throw std::runtime_error("CH1817 pin mapping changes require a server restart so GPIO lines can be safely re-requested. " + gpioHelpText());
    settings_ = next;
    status_ = "settings updated";
    saveLocked();
}

void Ch1817Driver::setOffhook(bool offhook) {
    std::lock_guard<std::mutex> lock(mtx_);
    if ((using_mcu_ri_ || using_mcu_oh_) && adc_sampler_) updateMcuTelephonyStateLocked(adc_sampler_->mcuPeripheralSnapshot(), std::chrono::steady_clock::now());
    if (offhook && ringing_ && !ri_level_) {
        throw std::runtime_error("CH1817 refuses to go off-hook while RI is active/LOW. The CH1817 datasheet warns that setting OFFHK HIGH before RI returns HIGH during ring indication may degrade internal relay contacts; wait for RI HIGH or the between-ring silent interval.");
    }
    settings_.offhook = offhook;
    if (using_mcu_oh_ && adc_sampler_) {
        adc_sampler_->setMcuOhDrive(offhook);
        mcu_oh_logical_ = offhook;
        mcu_oh_raw_ = mcu_periph_config_.oh.active_high ? offhook : !offhook;
    }
    status_ = offhook ? "off-hook requested" : "on-hook requested";
    saveLocked();
}

void Ch1817Driver::setMcuPeripheralConfig(const McuPeripheralConfig& config) {
    std::lock_guard<std::mutex> lock(mtx_);
    mcu_periph_config_ = config;
    using_mcu_ri_ = mcu_periph_config_.ri.source == "mcu";
    using_mcu_oh_ = mcu_periph_config_.oh.source == "mcu";
}

void Ch1817Driver::setAdcSampler(AdcSampler* adc_sampler) {
    std::lock_guard<std::mutex> lock(mtx_);
    adc_sampler_ = adc_sampler;
    if (adc_sampler_) {
        mcu_periph_config_ = adc_sampler_->mcuPeripheralConfig();
        using_mcu_ri_ = mcu_periph_config_.ri.source == "mcu";
        using_mcu_oh_ = mcu_periph_config_.oh.source == "mcu";
    }
}

void Ch1817Driver::updateMcuTelephonyStateLocked(const McuPeripheralSnapshot& snap, const std::chrono::steady_clock::time_point& now) {
    mcu_periph_config_ = snap.config;
    using_mcu_ri_ = mcu_periph_config_.ri.source == "mcu";
    using_mcu_oh_ = mcu_periph_config_.oh.source == "mcu";
    mcu_ri_raw_ = snap.ri_raw;
    mcu_ri_logical_ = snap.ri_logical;
    mcu_oh_raw_ = snap.oh_raw;
    mcu_oh_logical_ = snap.oh_logical;
    mcu_ri_transitions_ = snap.ri_transition_count;
    mcu_oh_transitions_ = snap.oh_transition_count;

    if (using_mcu_ri_) {
        const bool new_ri_level = !snap.ri_logical; // CH1817 legacy API: true=idle/high, false=active/ring.
        if (new_ri_level != ri_level_) {
            ri_level_ = new_ri_level;
            ri_edges_++;
            freq_edges_++;
            last_edge_ = now;
            if (!ringing_ && !ri_level_) { ringing_ = true; ring_start_ = now; freq_window_start_ = now; freq_edges_ = 1; }
        }
        const auto since_edge = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_edge_).count();
        if (ringing_ && since_edge > 1200 && ri_level_) {
            ringing_ = false;
            ri_frequency_hz_ = 0.0;
            freq_edges_ = 0;
            status_ = settings_.offhook ? "off-hook" : "idle";
        } else if (ringing_) {
            const double secs = std::chrono::duration<double>(std::max(now, freq_window_start_) - freq_window_start_).count();
            if (secs > 0.25) ri_frequency_hz_ = (static_cast<double>(freq_edges_) / 2.0) / secs;
            status_ = "ringing (MCU RI)";
        } else if (status_ == "running" || status_.empty()) {
            status_ = settings_.offhook ? "off-hook" : "idle";
        }
    }
}

json Ch1817Driver::snapshotJson() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return {
        {"enabled", settings_.enabled},
        {"settings", settingsToJson(settings_)},
        {"running", running_.load()},
        {"offhook", using_mcu_oh_ ? mcu_oh_logical_ : settings_.offhook},
        {"offhook_requested", settings_.offhook},
        {"ri_level", ri_level_},
        {"ri_level_text", ri_level_ ? "HIGH / idle" : "LOW / active"},
        {"ringing", ringing_},
        {"ri_frequency_hz", ri_frequency_hz_},
        {"ri_edges", ri_edges_},
        {"ri_source", using_mcu_ri_ ? "mcu" : "cm4"},
        {"oh_source", using_mcu_oh_ ? "mcu" : "cm4"},
        {"mcu_ri_raw", mcu_ri_raw_},
        {"mcu_ri_logical", mcu_ri_logical_},
        {"mcu_ri_transition_count", mcu_ri_transitions_},
        {"mcu_oh_raw", mcu_oh_raw_},
        {"mcu_oh_logical", mcu_oh_logical_},
        {"mcu_oh_transition_count", mcu_oh_transitions_},
        {"status", status_},
        {"last_error", last_error_},
        {"help", gpioHelpText()}
    };
}

void Ch1817Driver::worker() {
    Ch1817Settings cfg;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        cfg = settings_;
        status_ = cfg.enabled ? "running" : "disabled";
    }
    if (!cfg.enabled) return;

    gpiod::chip chip("0");
    gpiod::line off_line = chip.get_line(bcmForPhysicalPin(cfg.offhook_phys));
    gpiod::line ri_line;
    bool off_req = false;
    bool ri_req = false;
    try {
        off_line.request({"ch1817_offhk", gpiod::line_request::DIRECTION_OUTPUT, 0}, cfg.offhook ? 1 : 0);
        off_req = true;

        if (!using_mcu_ri_) {
            ri_line = chip.get_line(bcmForPhysicalPin(cfg.ri_phys));
            ri_line.request({"ch1817_ri", gpiod::line_request::EVENT_BOTH_EDGES, 0});
            ri_req = true;
            bool last_ri = ri_line.get_value();
            auto now = std::chrono::steady_clock::now();
            { std::lock_guard<std::mutex> lock(mtx_); ri_level_ = last_ri; last_edge_ = now; freq_window_start_ = now; ring_start_ = now; }
        }

        while (running_) {
            Ch1817Settings s;
            bool use_mcu_ri = false;
            bool use_mcu_oh = false;
            { std::lock_guard<std::mutex> lock(mtx_); s = settings_; use_mcu_ri = using_mcu_ri_; use_mcu_oh = using_mcu_oh_; }
            off_line.set_value(s.offhook ? 1 : 0);

            auto now = std::chrono::steady_clock::now();
            if ((use_mcu_ri || use_mcu_oh) && adc_sampler_) {
                std::lock_guard<std::mutex> lock(mtx_);
                updateMcuTelephonyStateLocked(adc_sampler_->mcuPeripheralSnapshot(), now);
            }
            if (!use_mcu_ri && ri_req) {
                if (ri_line.event_wait(std::chrono::milliseconds(25))) {
                    ri_line.event_read();
                    bool v = ri_line.get_value();
                    now = std::chrono::steady_clock::now();
                    std::lock_guard<std::mutex> lock(mtx_);
                    if (v != ri_level_) {
                        ri_level_ = v;
                        ri_edges_++;
                        freq_edges_++;
                        last_edge_ = now;
                        if (!ringing_) { ringing_ = true; ring_start_ = now; freq_window_start_ = now; freq_edges_ = 1; }
                    }
                }

                now = std::chrono::steady_clock::now();
                bool do_save = false;
                {
                    std::lock_guard<std::mutex> lock(mtx_);
                    const auto since_edge = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_edge_).count();
                    if (ringing_ && since_edge > 1200 && ri_level_) {
                        ringing_ = false;
                        ri_frequency_hz_ = 0.0;
                        freq_edges_ = 0;
                        status_ = settings_.offhook ? "off-hook" : "idle";
                    } else if (ringing_) {
                        const double secs = std::chrono::duration<double>(std::max(now, freq_window_start_) - freq_window_start_).count();
                        if (secs > 0.25) ri_frequency_hz_ = (static_cast<double>(freq_edges_) / 2.0) / secs;
                        status_ = "ringing";
                        if (settings_.auto_answer_enabled && !settings_.offhook) {
                            const auto ring_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - ring_start_).count();
                            if (ring_ms >= settings_.auto_answer_delay_ms) {
                                if (ri_level_) {
                                    settings_.offhook = true;
                                    status_ = "auto-answered";
                                    do_save = true;
                                } else {
                                    status_ = "auto-answer armed; waiting for RI HIGH";
                                }
                            }
                        }
                    }
                }
                if (do_save) { std::lock_guard<std::mutex> lock(mtx_); saveLocked(); }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            }
        }
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(mtx_);
        last_error_ = e.what();
        status_ = "error";
    }
    if (ri_req) { try { ri_line.release(); } catch (...) {} }
    if (off_req) { try { off_line.release(); } catch (...) {} }
}
