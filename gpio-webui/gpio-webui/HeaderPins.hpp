#pragma once

#include <map>
#include <set>
#include <string>
#include <sstream>
#include <stdexcept>

inline const std::map<int, int>& raspberryPiHeaderPhysicalToBcm() {
    static const std::map<int, int> pins = {
        {3, 2}, {5, 3}, {7, 4}, {8, 14}, {10, 15},
        {11, 17}, {12, 18}, {13, 27}, {15, 22}, {16, 23}, {18, 24},
        {19, 10}, {21, 9}, {22, 25}, {23, 11}, {24, 8}, {26, 7},
        {27, 0}, {28, 1}, {29, 5}, {31, 6}, {32, 12}, {33, 13},
        {35, 19}, {36, 16}, {37, 26}, {38, 20}, {40, 21}
    };
    return pins;
}

inline int bcmForPhysicalPin(int phys) {
    auto it = raspberryPiHeaderPhysicalToBcm().find(phys);
    if (it == raspberryPiHeaderPhysicalToBcm().end()) {
        throw std::runtime_error("Physical pin " + std::to_string(phys) + " is not a GPIO-capable Raspberry Pi 40-pin-header pin.");
    }
    return it->second;
}

inline bool isGpioPhysicalPin(int phys) {
    return raspberryPiHeaderPhysicalToBcm().count(phys) != 0;
}

inline void requireDistinctEnabledPins(const std::string& owner, const std::map<std::string, int>& pins) {
    std::map<int, std::string> used;
    for (const auto& [name, phys] : pins) {
        if (phys <= 0) continue;
        bcmForPhysicalPin(phys);
        auto it = used.find(phys);
        if (it != used.end()) {
            throw std::runtime_error(owner + " illegal pin configuration: " + name + " and " + it->second + " both use physical pin " + std::to_string(phys) + ". Each enabled signal must use a distinct GPIO-capable physical pin.");
        }
        used[phys] = name;
    }
}

inline std::string gpioHelpText() {
    return "Use only GPIO-capable 40-pin-header physical pins: 3,5,7,8,10,11,12,13,15,16,18,19,21,22,23,24,26,27,28,29,31,32,33,35,36,37,38,40. Do not assign the same physical pin to more than one enabled chip signal. Reserved chip pins are hidden from generic GPIO control.";
}
