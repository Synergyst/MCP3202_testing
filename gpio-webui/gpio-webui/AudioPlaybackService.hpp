#pragma once

#include "DacOutput.hpp"
#include "FilterProfileManager.hpp"
#include "SystemContext.hpp"
#include <nlohmann/json.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class AudioPlaybackService {
public:
    struct PlaybackRequest {
        std::string id;
        std::string channel_mode = "ch0"; // ch0, ch1, mono_both, stereo
        bool use_profiles = true;
        std::vector<std::string> effects;
        bool loop = false;
        double gain = 1.0;
    };

    AudioPlaybackService(DacOutput* dac, FilterProfileManager* filters, std::shared_ptr<SystemContext> context);
    ~AudioPlaybackService();

    nlohmann::json saveUpload(const std::string& original_name, const std::string& bytes);
    nlohmann::json listUploads() const;
    nlohmann::json deleteUpload(const std::string& id);
    nlohmann::json play(const PlaybackRequest& req);
    nlohmann::json stop();
    nlohmann::json status() const;

private:
    void loadUploadsLocked();
    void saveUploadsLocked() const;
    void worker(PlaybackRequest req);
    std::string uploadDir() const;
    nlohmann::json uploadByIdLocked(const std::string& id) const;
    void setStatusLocked(const std::string& status, const std::string& error = "");

    DacOutput* dac_ = nullptr;
    FilterProfileManager* filters_ = nullptr;
    std::shared_ptr<SystemContext> context_;
    mutable std::mutex mtx_;
    nlohmann::json uploads_ = nlohmann::json::object();
    nlohmann::json status_;
    std::atomic<bool> stop_requested_{false};
    std::thread worker_;
};
