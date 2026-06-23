#pragma once
#include <chrono>
#include <string>
#include <mutex>

struct PinState {
    int physical_pin;
    int bcm_pin;
    bool is_output = false;
    bool current_state = false;
    bool target_output_state = false;

    // Frequency & Transition Tracker
    std::chrono::steady_clock::time_point last_toggle_time;
    std::chrono::steady_clock::time_point transition_start_time;
    uint64_t transition_count = 0;
    bool is_transitioning = false;
    double average_frequency = 0.0;

    std::mutex mtx;

    // Constructor for in-place construction via std::make_shared
    PinState(int phys, int bcm) : physical_pin(phys), bcm_pin(bcm) {}
};
