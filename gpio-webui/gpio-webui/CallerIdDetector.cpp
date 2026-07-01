#include "CallerIdDetector.hpp"
#include "libs/fsk/Fsk.hpp"
#include "libs/audio_filters/AudioFilters.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <utility>

using json = nlohmann::json;

namespace {
constexpr size_t MAX_RAW_BITS_SHOWN = 1200;
constexpr size_t MAX_ANALYZE_FRAMES = 64000;

std::string bytesHex(const std::vector<uint8_t>& bytes) {
    std::ostringstream oss;
    oss << std::hex << std::uppercase << std::setfill('0');
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (i) oss << ' ';
        oss << std::setw(2) << static_cast<int>(bytes[i]);
    }
    return oss.str();
}

std::string printableAscii(const std::vector<uint8_t>& b, size_t pos, size_t len) {
    std::string out;
    for (size_t i = 0; i < len && pos + i < b.size(); ++i) {
        const uint8_t c = b[pos + i];
        out.push_back((c >= 32 && c <= 126) ? static_cast<char>(c) : '.');
    }
    return out;
}

std::string formatDateTime(const std::string& raw) {
    if (raw.size() != 8) return raw;
    return raw.substr(0, 2) + "/" + raw.substr(2, 2) + " " + raw.substr(4, 2) + ":" + raw.substr(6, 2);
}

bool checksumOk(const std::vector<uint8_t>& b, size_t start, size_t total) {
    uint32_t sum = 0;
    for (size_t i = 0; i < total; ++i) sum += b[start + i];
    return (sum & 0xffu) == 0;
}

void parseCallerIdMessage(CallerIdDetector::DecodeCandidate& c) {
    const auto& b = c.bytes;
    for (size_t i = 0; i + 2 < b.size(); ++i) {
        const uint8_t type = b[i];
        if (type != 0x04 && type != 0x80) continue;
        const size_t len = b[i + 1];
        const size_t total = len + 3;
        if (len > 80 || i + total > b.size()) continue;

        c.valid = true;
        c.checksum_ok = checksumOk(b, i, total);
        c.message_type = (type == 0x80) ? "MDMF (0x80)" : "SDMF (0x04)";
        c.raw_bytes_hex = bytesHex(std::vector<uint8_t>(b.begin() + static_cast<std::ptrdiff_t>(i), b.begin() + static_cast<std::ptrdiff_t>(i + total)));

        if (type == 0x04) {
            if (len >= 8) {
                c.date_time_raw = printableAscii(b, i + 2, 8);
                c.date_time = formatDateTime(c.date_time_raw);
            }
            if (len > 8) c.number = printableAscii(b, i + 10, len - 8);
        } else {
            size_t p = i + 2;
            const size_t end = i + 2 + len;
            while (p + 2 <= end) {
                const uint8_t param_type = b[p++];
                const size_t plen = b[p++];
                if (p + plen > end) break;
                const std::string val = printableAscii(b, p, plen);
                if (param_type == 0x01) {
                    c.date_time_raw = val;
                    c.date_time = formatDateTime(val);
                } else if (param_type == 0x02 || param_type == 0x04) {
                    c.number = val;
                } else if (param_type == 0x07 || param_type == 0x08) {
                    c.name = val;
                }
                p += plen;
            }
        }
        c.status = c.checksum_ok ? "decoded" : "decoded with checksum mismatch";
        return;
    }
}

Fsk::DecoderSettings toFskSettings(const CallerIdSettings& cfg) {
    Fsk::DecoderSettings s;
    s.mark_hz = cfg.mark_hz;
    s.space_hz = cfg.space_hz;
    s.baud = cfg.baud;
    s.normalize = cfg.normalize;
    s.normalize_headroom_db = cfg.normalize_headroom_db;
    s.extra_gain_db = cfg.extra_gain_db;
    s.dc_block = cfg.dc_block;
    return s;
}

int clampInt(int v, int lo, int hi) { return std::max(lo, std::min(hi, v)); }
double clampDouble(double v, double lo, double hi) { return std::max(lo, std::min(hi, v)); }
}

CallerIdDetector::CallerIdDetector(AdcSampler* sampler, std::shared_ptr<SystemContext> context, FilterProfileManager* filter_profiles)
    : sampler_(sampler), context_(std::move(context)), filter_profiles_(filter_profiles) {
    state_.enabled = sampler_ != nullptr;
    loadLastLogged();
}

CallerIdDetector::~CallerIdDetector() { stop(); }

void CallerIdDetector::start() {
    if (!sampler_ || running_) return;
    running_ = true;
    worker_ = std::thread(&CallerIdDetector::worker, this);
}

void CallerIdDetector::stop() {
    running_ = false;
    if (worker_.joinable()) worker_.join();
}

CallerIdSnapshot CallerIdDetector::snapshot() const {
    std::lock_guard<std::mutex> lock(mtx_);
    auto s = state_;
    s.settings = settings_;
    return s;
}

json CallerIdDetector::snapshotJson() const { return toJson(snapshot()); }

CallerIdSettings CallerIdDetector::settings() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return settings_;
}

json CallerIdDetector::settingsJson() const { return settingsToJson(settings()); }

void CallerIdDetector::updateSettings(const CallerIdSettings& settings) {
    std::lock_guard<std::mutex> lock(mtx_);
    settings_ = settings;
    state_.settings = settings_;
    state_.status = "settings updated";
    saveSettingsLocked();
}

void CallerIdDetector::updateSettingsFromJson(const json& j) {
    CallerIdSettings defaults;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        defaults = settings_;
    }
    updateSettings(settingsFromJson(j, defaults));
}

void CallerIdDetector::worker() {
    while (running_) {
        try {
            const auto adc = sampler_->status();
            if (!adc.healthy || adc.total_frames < 1000) {
                std::lock_guard<std::mutex> lock(mtx_);
                state_.enabled = true;
                state_.running = true;
                state_.status = adc.healthy ? "waiting for samples" : "waiting for healthy ADC";
                state_.last_error = adc.last_error;
                state_.settings = settings_;
            } else {
                auto c = analyzeRecent();
                if (c.valid) publishCandidate(c, adc.measured_sample_rate_hz, adc.total_frames);
                else {
                    std::lock_guard<std::mutex> lock(mtx_);
                    state_.enabled = true;
                    state_.running = true;
                    state_.status = c.status.empty() ? "listening" : c.status;
                    state_.confidence = c.confidence;
                    state_.raw_bits = c.raw_bits;
                    state_.raw_bytes_hex = c.raw_bytes_hex;
                    state_.sample_rate_hz = adc.measured_sample_rate_hz;
                    state_.last_total_frames_seen = adc.total_frames;
                    state_.selected_channel = c.selected_channel;
                    state_.last_error.clear();
                    state_.filter_profile = c.filter_profile;
                    state_.filter_effects = c.filter_effects;
                    state_.settings = settings_;
                    if (c.confidence > state_.best_confidence) {
                        state_.best_confidence = c.confidence;
                        state_.best_selected_channel = c.selected_channel;
                        state_.best_raw_bits = c.raw_bits;
                        state_.best_raw_bytes_hex = c.raw_bytes_hex;
                        state_.best_status = c.status;
                        state_.best_update = nowIso8601();
                        saveLastLoggedLocked();
                    }
                }
            }
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lock(mtx_);
            state_.last_error = e.what();
            state_.status = "error";
        }
        for (int i = 0; i < 10 && running_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

CallerIdDetector::DecodeCandidate CallerIdDetector::analyzeRecent() {
    DecodeCandidate best;
    CallerIdSettings cfg;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        cfg = settings_;
    }

    const auto meta = sampler_->status();
    const uint32_t sr = std::max<uint32_t>(1, meta.measured_sample_rate_hz ? meta.measured_sample_rate_hz : meta.sample_rate_hz);
    const size_t frames = std::min<size_t>((static_cast<size_t>(sr) * std::max<size_t>(250, cfg.analysis_ms) + 999) / 1000, MAX_ANALYZE_FRAMES);
    const auto recent = sampler_->recent(frames);
    if (recent.samples[0].size() < 1000 || recent.samples[1].size() < 1000) {
        best.status = "not enough recent samples";
        return best;
    }

    std::vector<int> sources;
    if (cfg.channel >= 0 && cfg.channel <= 2) sources.push_back(cfg.channel);
    else sources = {0, 1, 2};

    std::vector<uint16_t> ch0 = recent.samples[0];
    std::vector<uint16_t> ch1 = recent.samples[1];
    std::vector<std::string> fsk_effects;
    if (filter_profiles_) {
        fsk_effects = filter_profiles_->effectiveEffects("fsk.decoder");
        if (!fsk_effects.empty()) AudioFilters::applyEffectsToStereoChannels(ch0, ch1, sr, fsk_effects, fsk_effects);
    }
    auto fsk_cfg = toFskSettings(cfg);
    if (std::find(fsk_effects.begin(), fsk_effects.end(), "dc_block") != fsk_effects.end()) fsk_cfg.dc_block = false;
    if (std::find(fsk_effects.begin(), fsk_effects.end(), "normalize") != fsk_effects.end() ||
        std::find(fsk_effects.begin(), fsk_effects.end(), "voice_agc") != fsk_effects.end()) fsk_cfg.normalize = false;
    Fsk::FskDecoder decoder;
    auto fsk = decoder.decodeBest(ch0, ch1, sources, sr, fsk_cfg, MAX_RAW_BITS_SHOWN);
    best.confidence = fsk.confidence;
    best.selected_channel = fsk.selected_channel;
    best.bytes = std::move(fsk.bytes);
    best.raw_bytes_hex = std::move(fsk.raw_bytes_hex);
    best.raw_bits = std::move(fsk.raw_bits);
    best.status = "listening";
    best.filter_profile = "fsk.decoder";
    best.filter_effects = fsk_effects;
    parseCallerIdMessage(best);
    if (!best.valid && best.bytes.size() >= 4) best.status = "FSK-like frames seen";
    return best;
}

void CallerIdDetector::publishCandidate(const DecodeCandidate& c, uint32_t sample_rate, uint64_t frames_seen) {
    std::lock_guard<std::mutex> lock(mtx_);
    const std::string old_hex = state_.raw_bytes_hex;
    state_.enabled = true;
    state_.running = true;
    state_.detected = c.valid;
    state_.checksum_ok = c.checksum_ok;
    state_.total_decodes += (c.raw_bytes_hex != old_hex) ? 1 : 0;
    state_.last_total_frames_seen = frames_seen;
    state_.sample_rate_hz = sample_rate;
    state_.selected_channel = c.selected_channel;
    state_.confidence = c.confidence;
    state_.status = c.status;
    state_.message_type = c.message_type;
    state_.date_time = c.date_time;
    state_.date_time_raw = c.date_time_raw;
    state_.number = c.number;
    state_.name = c.name;
    state_.raw_bits = c.raw_bits;
    state_.raw_bytes_hex = c.raw_bytes_hex;
    state_.filter_profile = c.filter_profile;
    state_.filter_effects = c.filter_effects;
    state_.last_update = nowIso8601();
    state_.last_error.clear();
    state_.settings = settings_;
    if (c.confidence >= state_.best_confidence) {
        state_.best_confidence = c.confidence;
        state_.best_selected_channel = c.selected_channel;
        state_.best_raw_bits = c.raw_bits;
        state_.best_raw_bytes_hex = c.raw_bytes_hex;
        state_.best_status = c.status;
        state_.best_update = state_.last_update;
    }
    saveLastLoggedLocked();
}

json CallerIdDetector::toJson(const CallerIdSnapshot& s) {
    return {
        {"enabled", s.enabled}, {"running", s.running}, {"detected", s.detected},
        {"checksum_ok", s.checksum_ok}, {"total_decodes", s.total_decodes},
        {"last_total_frames_seen", s.last_total_frames_seen}, {"sample_rate_hz", s.sample_rate_hz},
        {"selected_channel", s.selected_channel}, {"confidence", s.confidence},
        {"best_confidence", s.best_confidence}, {"best_selected_channel", s.best_selected_channel},
        {"status", s.status}, {"message_type", s.message_type}, {"date_time", s.date_time},
        {"date_time_raw", s.date_time_raw}, {"number", s.number}, {"name", s.name},
        {"raw_bits", s.raw_bits}, {"raw_bytes_hex", s.raw_bytes_hex},
        {"best_raw_bits", s.best_raw_bits}, {"best_raw_bytes_hex", s.best_raw_bytes_hex},
        {"best_status", s.best_status}, {"best_update", s.best_update},
        {"last_update", s.last_update}, {"last_error", s.last_error},
        {"filter_profile", s.filter_profile}, {"filter_effects", s.filter_effects},
        {"settings", settingsToJson(s.settings)}
    };
}

json CallerIdDetector::settingsToJson(const CallerIdSettings& s) {
    return {
        {"channel", s.channel}, {"mark_hz", s.mark_hz}, {"space_hz", s.space_hz},
        {"baud", s.baud}, {"analysis_ms", s.analysis_ms}, {"normalize", s.normalize},
        {"normalize_headroom_db", s.normalize_headroom_db}, {"extra_gain_db", s.extra_gain_db},
        {"dc_block", s.dc_block}
    };
}

CallerIdSettings CallerIdDetector::settingsFromJson(const json& j, const CallerIdSettings& defaults) {
    CallerIdSettings s = defaults;
    if (!j.is_object()) return s;
    s.channel = clampInt(j.value("channel", s.channel), -1, 2);
    s.mark_hz = clampDouble(j.value("mark_hz", s.mark_hz), 500.0, 3000.0);
    s.space_hz = clampDouble(j.value("space_hz", s.space_hz), 500.0, 3500.0);
    s.baud = clampDouble(j.value("baud", s.baud), 300.0, 2400.0);
    s.analysis_ms = static_cast<size_t>(clampInt(static_cast<int>(j.value("analysis_ms", s.analysis_ms)), 500, 10000));
    s.normalize = j.value("normalize", s.normalize);
    s.normalize_headroom_db = clampDouble(j.value("normalize_headroom_db", s.normalize_headroom_db), 0.0, 30.0);
    s.extra_gain_db = clampDouble(j.value("extra_gain_db", s.extra_gain_db), -24.0, 60.0);
    s.dc_block = j.value("dc_block", s.dc_block);
    return s;
}

std::string CallerIdDetector::nowIso8601() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&t, &tm);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S%z");
    return oss.str();
}

void CallerIdDetector::loadLastLogged() {
    if (!context_) return;
    std::lock_guard<std::mutex> cfg_lock(context_->config_mutex);
    std::ifstream f(context_->config_path);
    if (!f.is_open()) return;
    try {
        json j;
        f >> j;
        if (j.contains("caller_id_settings")) settings_ = settingsFromJson(j["caller_id_settings"], settings_);
        std::lock_guard<std::mutex> lock(mtx_);
        state_.settings = settings_;
        if (!j.contains("caller_id_last")) return;
        auto c = j["caller_id_last"];
        state_.detected = c.value("detected", false);
        state_.checksum_ok = c.value("checksum_ok", false);
        state_.total_decodes = c.value("total_decodes", 0ull);
        state_.sample_rate_hz = c.value("sample_rate_hz", 0u);
        state_.selected_channel = c.value("selected_channel", -1);
        state_.confidence = c.value("confidence", 0.0);
        state_.best_confidence = c.value("best_confidence", state_.confidence);
        state_.best_selected_channel = c.value("best_selected_channel", state_.selected_channel);
        state_.status = c.value("status", "loaded previous caller ID");
        state_.message_type = c.value("message_type", "");
        state_.date_time = c.value("date_time", "");
        state_.date_time_raw = c.value("date_time_raw", "");
        state_.number = c.value("number", "");
        state_.name = c.value("name", "");
        state_.raw_bits = c.value("raw_bits", "");
        state_.raw_bytes_hex = c.value("raw_bytes_hex", "");
        state_.best_raw_bits = c.value("best_raw_bits", state_.raw_bits);
        state_.best_raw_bytes_hex = c.value("best_raw_bytes_hex", state_.raw_bytes_hex);
        state_.best_status = c.value("best_status", state_.status);
        state_.best_update = c.value("best_update", "");
        state_.last_update = c.value("last_update", "");
    } catch (...) {}
}

void CallerIdDetector::saveSettingsLocked() {
    if (!context_) return;
    std::lock_guard<std::mutex> cfg_lock(context_->config_mutex);
    json j;
    {
        std::ifstream f(context_->config_path);
        if (f.is_open()) { try { f >> j; } catch (...) { j = json::object(); } }
    }
    if (!j.is_object()) j = json::object();
    j["caller_id_settings"] = settingsToJson(settings_);
    std::ofstream out(context_->config_path);
    if (out.is_open()) out << j.dump(2);
}

void CallerIdDetector::saveLastLoggedLocked() {
    if (!context_) return;
    std::lock_guard<std::mutex> cfg_lock(context_->config_mutex);
    json j;
    {
        std::ifstream f(context_->config_path);
        if (f.is_open()) { try { f >> j; } catch (...) { j = json::object(); } }
    }
    if (!j.is_object()) j = json::object();
    j["caller_id_last"] = toJson(state_);
    j["caller_id_settings"] = settingsToJson(settings_);
    std::ofstream out(context_->config_path);
    if (out.is_open()) out << j.dump(2);
}
