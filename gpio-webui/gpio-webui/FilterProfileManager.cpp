#include "FilterProfileManager.hpp"
#include "libs/audio_filters/AudioFilters.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <stdexcept>
#include <utility>

using json = nlohmann::json;

namespace {

const std::map<std::string, FilterProfileManager::Profile>& defaultProfiles() {
    using P = FilterProfileManager::Profile;
    static const std::map<std::string, P> defaults = {
        {"adc.graph.ch0", {true, {"dc_block"}}},
        {"adc.graph.ch1", {true, {"dc_block"}}},
        {"adc.wav.ch0", {true, {}}},
        {"adc.wav.ch1", {true, {}}},
        {"adc.wav.mix", {true, {}}},
        {"adc.wav.stereo", {true, {}}},
        {"fsk.decoder", {true, {"dc_block", "bell202_bandpass"}}},
        {"line_state.detector", {true, {"dc_block"}}},
        {"telephony.diagnostics", {true, {}}},
        {"dac.playback.ch0", {true, {}}},
        {"dac.playback.ch1", {true, {}}},
        {"dac.playback.stereo", {true, {}}},
        {"dac.dtmf", {true, {}}}
    };
    return defaults;
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

} // namespace

FilterProfileManager::FilterProfileManager(std::shared_ptr<SystemContext> context) : context_(std::move(context)) {
    std::lock_guard<std::mutex> lock(mtx_);
    loadLocked();
    ensureDefaultsLocked();
}

bool FilterProfileManager::isKnownContext(const std::string& context_id) const {
    return defaultProfiles().count(context_id) > 0;
}

std::vector<std::string> FilterProfileManager::contextIds() const {
    std::vector<std::string> ids;
    for (const auto& [id, _] : defaultProfiles()) ids.push_back(id);
    return ids;
}

FilterProfileManager::Profile FilterProfileManager::defaultProfile(const std::string& context_id) const {
    auto it = defaultProfiles().find(context_id);
    if (it == defaultProfiles().end()) throw std::runtime_error("Unknown filter profile context: " + context_id);
    return it->second;
}

FilterProfileManager::Profile FilterProfileManager::getProfile(const std::string& context_id) const {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!isKnownContext(context_id)) throw std::runtime_error("Unknown filter profile context: " + context_id);
    auto it = profiles_.find(context_id);
    return it == profiles_.end() ? defaultProfile(context_id) : it->second;
}

FilterProfileManager::Profile FilterProfileManager::updateProfile(const std::string& context_id, const Profile& profile) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!isKnownContext(context_id)) throw std::runtime_error("Unknown filter profile context: " + context_id);
    Profile clean = sanitizeProfile(context_id, profile, true);
    profiles_[context_id] = clean;
    saveLocked();
    return clean;
}

FilterProfileManager::Profile FilterProfileManager::updateProfileFromJson(const json& j) {
    if (!j.is_object()) throw std::runtime_error("Filter profile payload must be a JSON object");
    std::string context_id = j.value("context", "");
    if (context_id.empty()) throw std::runtime_error("Missing filter profile context");
    return updateProfile(context_id, profileFromJson(j, defaultProfile(context_id)));
}

FilterProfileManager::Profile FilterProfileManager::restoreDefault(const std::string& context_id) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!isKnownContext(context_id)) throw std::runtime_error("Unknown filter profile context: " + context_id);
    Profile p = defaultProfile(context_id);
    profiles_[context_id] = p;
    saveLocked();
    return p;
}

std::vector<std::string> FilterProfileManager::effectiveEffects(const std::string& context_id) const {
    Profile p = getProfile(context_id);
    return p.enabled ? p.effects : std::vector<std::string>{};
}

json FilterProfileManager::profileJson(const std::string& context_id) const {
    json out = profileToJson(getProfile(context_id));
    out["context"] = context_id;
    out["default"] = profileToJson(defaultProfile(context_id));
    return out;
}

json FilterProfileManager::allProfilesJson() const {
    std::lock_guard<std::mutex> lock(mtx_);
    json profiles = json::object();
    for (const auto& [id, def] : defaultProfiles()) {
        auto it = profiles_.find(id);
        profiles[id] = profileToJson(it == profiles_.end() ? def : it->second);
    }
    return profiles;
}

json FilterProfileManager::defaultsJson() const {
    json defaults = json::object();
    for (const auto& [id, p] : defaultProfiles()) defaults[id] = profileToJson(p);
    return defaults;
}

json FilterProfileManager::contextsJson() const {
    json arr = json::array();
    for (const auto& [id, _] : defaultProfiles()) {
        std::string label = id;
        std::replace(label.begin(), label.end(), '.', ' ');
        arr.push_back({{"id", id}, {"label", label}});
    }
    return arr;
}

json FilterProfileManager::profileToJson(const Profile& p) {
    return {{"enabled", p.enabled}, {"effects", p.effects}};
}

FilterProfileManager::Profile FilterProfileManager::profileFromJson(const json& j) {
    return profileFromJson(j, Profile());
}

FilterProfileManager::Profile FilterProfileManager::profileFromJson(const json& j, const Profile& fallback) {
    Profile p = fallback;
    if (!j.is_object()) return p;
    p.enabled = j.value("enabled", p.enabled);
    if (j.contains("effects")) {
        p.effects.clear();
        if (j["effects"].is_array()) {
            for (const auto& v : j["effects"]) {
                if (v.is_string()) p.effects.push_back(v.get<std::string>());
            }
        } else if (j["effects"].is_string()) {
            std::string csv = j["effects"].get<std::string>();
            size_t start = 0;
            while (start <= csv.size()) {
                const size_t comma = csv.find(',', start);
                p.effects.push_back(csv.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
        }
    }
    return p;
}

FilterProfileManager::Profile FilterProfileManager::sanitizeProfile(const std::string& context_id, const Profile& profile, bool reject_invalid) const {
    if (!isKnownContext(context_id)) throw std::runtime_error("Unknown filter profile context: " + context_id);
    Profile clean;
    clean.enabled = profile.enabled;
    for (std::string id : profile.effects) {
        id = lowerCopy(trimCopy(id));
        if (id.empty() || id == "none") continue;
        if (!AudioFilters::isKnownEffect(id)) {
            if (reject_invalid) throw std::runtime_error("Unknown audio effect for " + context_id + ": " + id);
            continue;
        }
        if (!AudioFilters::isRuntimeAvailable(id)) {
            if (reject_invalid) throw std::runtime_error(AudioFilters::unavailableReason(id));
            continue;
        }
        if (std::find(clean.effects.begin(), clean.effects.end(), id) == clean.effects.end()) clean.effects.push_back(id);
    }
    return clean;
}

void FilterProfileManager::loadLocked() {
    profiles_.clear();
    if (!context_) return;
    std::lock_guard<std::mutex> cfg_lock(context_->config_mutex);
    std::ifstream f(context_->config_path);
    if (!f.is_open()) return;
    try {
        json j;
        f >> j;
        if (!j.is_object() || !j.contains("filter_profiles") || !j["filter_profiles"].is_object()) return;
        for (const auto& [id, val] : j["filter_profiles"].items()) {
            if (!isKnownContext(id)) continue;
            profiles_[id] = sanitizeProfile(id, profileFromJson(val, defaultProfile(id)), false);
        }
    } catch (...) {
        profiles_.clear();
    }
}

void FilterProfileManager::saveLocked() const {
    if (!context_) return;
    std::lock_guard<std::mutex> cfg_lock(context_->config_mutex);
    json j;
    {
        std::ifstream f(context_->config_path);
        if (f.is_open()) { try { f >> j; } catch (...) { j = json::object(); } }
    }
    if (!j.is_object()) j = json::object();
    j["filter_profiles"] = json::object();
    for (const auto& [id, def] : defaultProfiles()) {
        auto it = profiles_.find(id);
        j["filter_profiles"][id] = profileToJson(it == profiles_.end() ? def : it->second);
    }
    std::ofstream out(context_->config_path);
    if (out.is_open()) out << j.dump(2);
}

void FilterProfileManager::ensureDefaultsLocked() {
    bool changed = false;
    for (const auto& [id, def] : defaultProfiles()) {
        if (!profiles_.count(id)) {
            profiles_[id] = def;
            changed = true;
        }
    }
    if (changed) saveLocked();
}
