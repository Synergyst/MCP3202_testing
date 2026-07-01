#pragma once

#include "SystemContext.hpp"
#include <nlohmann/json.hpp>

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class FilterProfileManager {
public:
    struct Profile {
        bool enabled = true;
        std::vector<std::string> effects;
    };

    explicit FilterProfileManager(std::shared_ptr<SystemContext> context);

    bool isKnownContext(const std::string& context_id) const;
    std::vector<std::string> contextIds() const;
    Profile defaultProfile(const std::string& context_id) const;
    Profile getProfile(const std::string& context_id) const;
    Profile updateProfile(const std::string& context_id, const Profile& profile);
    Profile updateProfileFromJson(const nlohmann::json& j);
    Profile restoreDefault(const std::string& context_id);
    std::vector<std::string> effectiveEffects(const std::string& context_id) const;

    nlohmann::json profileJson(const std::string& context_id) const;
    nlohmann::json allProfilesJson() const;
    nlohmann::json defaultsJson() const;
    nlohmann::json contextsJson() const;

    static nlohmann::json profileToJson(const Profile& p);
    static Profile profileFromJson(const nlohmann::json& j);
    static Profile profileFromJson(const nlohmann::json& j, const Profile& fallback);

private:
    Profile sanitizeProfile(const std::string& context_id, const Profile& profile, bool reject_invalid) const;
    void loadLocked();
    void saveLocked() const;
    void ensureDefaultsLocked();

    std::shared_ptr<SystemContext> context_;
    mutable std::mutex mtx_;
    std::map<std::string, Profile> profiles_;
};
