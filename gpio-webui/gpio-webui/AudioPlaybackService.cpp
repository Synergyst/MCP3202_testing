#include "AudioPlaybackService.hpp"
#include "DacPlaybackFilters.hpp"
#include "WavFile.hpp"
#include "libs/audio_filters/AudioFilters.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <thread>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {
std::string nowIso() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{}; localtime_r(&t, &tm);
    std::ostringstream oss; oss << std::put_time(&tm, "%Y%m%dT%H%M%S");
    return oss.str();
}
std::string randomHex() {
    static std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<int> d(0, 15);
    std::string s; for (int i = 0; i < 8; ++i) s.push_back("0123456789abcdef"[d(rng)]); return s;
}
std::string safeName(std::string s) {
    for (char& c : s) if (!(std::isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-')) c = '_';
    if (s.empty()) s = "upload.wav";
    return s;
}
uint16_t pcmToRaw(int16_t s, double gain) {
    const double v = std::max(-32768.0, std::min(32767.0, static_cast<double>(s) * gain));
    int raw = 2048 + static_cast<int>(std::lrint(v / 16.0));
    return static_cast<uint16_t>(std::max(0, std::min(4095, raw)));
}
int16_t mix2(int16_t a, int16_t b) { return static_cast<int16_t>((static_cast<int32_t>(a) + static_cast<int32_t>(b)) / 2); }

bool pathInside(const fs::path& child, const fs::path& parent) {
    std::error_code ec;
    const fs::path c = fs::weakly_canonical(child, ec);
    if (ec) return false;
    const fs::path p = fs::weakly_canonical(parent, ec);
    if (ec) return false;
    auto ci = c.begin();
    for (auto pi = p.begin(); pi != p.end(); ++pi, ++ci) {
        if (ci == c.end() || *ci != *pi) return false;
    }
    return true;
}

std::string safeChannelMode(std::string mode) {
    if (mode == "ch0" || mode == "ch1" || mode == "mono_both" || mode == "stereo") return mode;
    return "ch0";
}
}

AudioPlaybackService::AudioPlaybackService(DacOutput* dac, FilterProfileManager* filters, std::shared_ptr<SystemContext> context)
    : dac_(dac), filters_(filters), context_(std::move(context)) {
    status_ = {{"active", false}, {"status", "idle"}, {"position_ms", 0}, {"last_error", ""}};
    std::lock_guard<std::mutex> lock(mtx_);
    loadUploadsLocked();
}

AudioPlaybackService::~AudioPlaybackService() { stop(); if (worker_.joinable()) worker_.join(); }

std::string AudioPlaybackService::uploadDir() const {
    if (!context_) return "uploads/audio";
    fs::path base = fs::absolute(fs::path(context_->config_path)).parent_path();
    return (base / "uploads" / "audio").string();
}

void AudioPlaybackService::loadUploadsLocked() {
    uploads_ = json::object();
    if (!context_) return;
    std::lock_guard<std::mutex> cfg_lock(context_->config_mutex);
    std::ifstream f(context_->config_path);
    if (!f.is_open()) return;
    try { json j; f >> j; if (j.contains("audio_uploads") && j["audio_uploads"].is_object()) uploads_ = j["audio_uploads"]; } catch (...) { uploads_ = json::object(); }
}

void AudioPlaybackService::saveUploadsLocked() const {
    if (!context_) return;
    std::lock_guard<std::mutex> cfg_lock(context_->config_mutex);
    json j; { std::ifstream f(context_->config_path); if (f.is_open()) { try { f >> j; } catch (...) { j = json::object(); } } }
    if (!j.is_object()) j = json::object();
    j["audio_uploads"] = uploads_;
    std::ofstream out(context_->config_path); if (out.is_open()) out << j.dump(2);
}

json AudioPlaybackService::saveUpload(const std::string& original_name, const std::string& bytes) {
    if (bytes.empty()) throw std::runtime_error("Upload is empty");
    if (bytes.size() > 50ull * 1024ull * 1024ull) throw std::runtime_error("Upload exceeds 50 MB limit");
    fs::create_directories(uploadDir());
    const std::string id = nowIso() + "_" + randomHex();
    const std::string name = safeName(original_name.empty() ? "upload.wav" : original_name);
    fs::path path = fs::path(uploadDir()) / (id + ".wav");
    { std::ofstream out(path, std::ios::binary); if (!out.is_open()) throw std::runtime_error("Unable to save upload"); out.write(bytes.data(), static_cast<std::streamsize>(bytes.size())); }
    WavInfo info;
    try { info = WavFile::probe(path.string()); }
    catch (...) { std::error_code ec; fs::remove(path, ec); throw; }
    if (info.sample_rate_hz > 100000) { std::error_code ec; fs::remove(path, ec); throw std::runtime_error("WAV sample rate exceeds 100 kHz playback limit"); }
    json meta = {{"id", id}, {"original_name", name}, {"path", path.string()}, {"size_bytes", bytes.size()}, {"created_at", nowIso()},
                 {"format", "wav"}, {"sample_rate_hz", info.sample_rate_hz}, {"channels", info.channels},
                 {"bits_per_sample", info.bits_per_sample}, {"duration_ms", info.duration_ms}};
    std::lock_guard<std::mutex> lock(mtx_);
    uploads_[id] = meta;
    saveUploadsLocked();
    return meta;
}

json AudioPlaybackService::listUploads() const {
    std::lock_guard<std::mutex> lock(mtx_);
    json arr = json::array();
    for (const auto& [id, meta] : uploads_.items()) arr.push_back(meta);
    return {{"status", "ok"}, {"uploads", arr}};
}

json AudioPlaybackService::uploadByIdLocked(const std::string& id) const {
    if (!uploads_.contains(id)) throw std::runtime_error("Unknown upload id");
    return uploads_.at(id);
}

json AudioPlaybackService::deleteUpload(const std::string& id) {
    stop();
    std::lock_guard<std::mutex> lock(mtx_);
    json meta = uploadByIdLocked(id);
    fs::path path = meta.value("path", "");
    if (!pathInside(path, uploadDir())) throw std::runtime_error("Refusing to delete path outside upload directory");
    std::error_code ec; fs::remove(path, ec);
    uploads_.erase(id);
    saveUploadsLocked();
    return {{"status", "ok"}, {"deleted", id}};
}

json AudioPlaybackService::play(const PlaybackRequest& req) {
    stop();
    if (worker_.joinable()) worker_.join();
    stop_requested_ = false;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        json meta = uploadByIdLocked(req.id);
        status_ = {{"active", true}, {"status", "starting"}, {"id", req.id}, {"original_name", meta.value("original_name", "")},
                   {"channel_mode", safeChannelMode(req.channel_mode)}, {"duration_ms", meta.value("duration_ms", 0)}, {"position_ms", 0},
                   {"frames_sent", 0}, {"last_error", ""}};
    }
    worker_ = std::thread(&AudioPlaybackService::worker, this, req);
    return status();
}

json AudioPlaybackService::stop() {
    stop_requested_ = true;
    if (worker_.joinable()) worker_.join();
    std::lock_guard<std::mutex> lock(mtx_);
    if (status_.value("active", false)) status_["status"] = "stopped";
    status_["active"] = false;
    return {{"status", "ok"}, {"playback", status_}};
}

json AudioPlaybackService::status() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return {{"status", "ok"}, {"playback", status_}};
}

void AudioPlaybackService::setStatusLocked(const std::string& status, const std::string& error) {
    status_["status"] = status;
    status_["last_error"] = error;
    if (!error.empty()) status_["active"] = false;
}

void AudioPlaybackService::worker(PlaybackRequest req) {
    try {
        req.channel_mode = safeChannelMode(req.channel_mode);
        if (!std::isfinite(req.gain)) req.gain = 1.0;
        req.gain = std::max(0.0, std::min(4.0, req.gain));
        if (!dac_) throw std::runtime_error("DAC output is not configured");
        json meta;
        { std::lock_guard<std::mutex> lock(mtx_); meta = uploadByIdLocked(req.id); }
        AudioBuffer buffer = WavFile::loadPcm16(meta.value("path", ""));
        if (buffer.sample_rate_hz > 100000) throw std::runtime_error("WAV sample rate exceeds 100 kHz playback limit");
        if (req.use_profiles) DacPlaybackFilters::applyPlaybackProfiles(buffer, req.channel_mode, filters_);
        else if (!req.effects.empty()) AudioFilters::applyEffects(buffer, req.effects);
        std::string err;
        if (!dac_->setRate(buffer.sample_rate_hz, err)) throw std::runtime_error(err);
        if (!dac_->setFormat(2, GW_SAMPLE_U16_LE, err)) throw std::runtime_error(err);
        if (!dac_->start(err)) throw std::runtime_error(err);
        const size_t frames = buffer.samples.size() / std::max<uint16_t>(1, buffer.channels);
        auto makeFrame = [&](size_t i) -> std::pair<uint16_t,uint16_t> {
            int16_t l = buffer.samples[i * buffer.channels];
            int16_t r = buffer.samples[i * buffer.channels + (buffer.channels > 1 ? 1 : 0)];
            int16_t mono = buffer.channels > 1 ? mix2(l, r) : l;
            uint16_t silence = 2048;
            uint16_t a = silence, b = silence;
            if (req.channel_mode == "ch0") a = pcmToRaw(mono, req.gain);
            else if (req.channel_mode == "ch1") b = pcmToRaw(mono, req.gain);
            else if (req.channel_mode == "stereo" && buffer.channels > 1) { a = pcmToRaw(l, req.gain); b = pcmToRaw(r, req.gain); }
            else { a = pcmToRaw(mono, req.gain); b = pcmToRaw(mono, req.gain); }
            return {a,b};
        };
        const bool native = dac_->config().transport == "native";
        const size_t chunk = 128;
        do {
            if (native) {
                auto next = std::chrono::steady_clock::now();
                const auto step = std::chrono::duration<double>(1.0 / std::max<uint32_t>(1, buffer.sample_rate_hz));
                for (size_t i = 0; i < frames && !stop_requested_; ++i) {
                    auto fr = makeFrame(i);
                    if (!dac_->writeRawBoth(fr.first, fr.second, err)) throw std::runtime_error(err);
                    if ((i & 0x3f) == 0) {
                        std::lock_guard<std::mutex> lock(mtx_);
                        status_["status"] = "playing"; status_["frames_sent"] = i + 1; status_["frames_total"] = frames;
                        status_["position_ms"] = static_cast<uint64_t>(i + 1) * 1000ull / std::max<uint32_t>(1, buffer.sample_rate_hz);
                        status_["sample_rate_hz"] = buffer.sample_rate_hz;
                    }
                    next += std::chrono::duration_cast<std::chrono::steady_clock::duration>(step);
                    std::this_thread::sleep_until(next);
                }
            } else {
                for (size_t pos = 0; pos < frames && !stop_requested_; pos += chunk) {
                    std::vector<std::pair<uint16_t,uint16_t>> out;
                    const size_t end = std::min(frames, pos + chunk);
                    out.reserve(end - pos);
                    for (size_t i = pos; i < end; ++i) out.push_back(makeFrame(i));
                    if (!dac_->writeRawBlock(out, err)) throw std::runtime_error(err);
                    std::lock_guard<std::mutex> lock(mtx_);
                    status_["status"] = "playing"; status_["frames_sent"] = end; status_["frames_total"] = frames;
                    status_["position_ms"] = static_cast<uint64_t>(end) * 1000ull / std::max<uint32_t>(1, buffer.sample_rate_hz);
                    status_["sample_rate_hz"] = buffer.sample_rate_hz;
                }
            }
        } while (req.loop && !stop_requested_);
        std::string ignored; dac_->writeRawBoth(2048, 2048, ignored);
        std::lock_guard<std::mutex> lock(mtx_);
        status_["active"] = false;
        status_["status"] = stop_requested_ ? "stopped" : "completed";
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(mtx_);
        setStatusLocked("error", e.what());
    }
}
