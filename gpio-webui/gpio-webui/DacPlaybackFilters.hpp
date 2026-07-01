#pragma once

#include "AudioProcessing.hpp"
#include "FilterProfileManager.hpp"

#include <string>

namespace DacPlaybackFilters {

void applyPlaybackProfiles(AudioBuffer& buffer, const std::string& mode, FilterProfileManager* profiles);

} // namespace DacPlaybackFilters
