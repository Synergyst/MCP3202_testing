#include "GpioManager.hpp"
#include <iostream>
#include <iomanip>
#include <ctime>

GpioManager::GpioManager(std::map<int, std::shared_ptr<PinState>>& reg, std::set<int> reserved)
    : registry(reg), reserved_bcm_pins(std::move(reserved)) {}

GpioManager::~GpioManager() {
    stop();
}

void GpioManager::start(int t_ms) {
    timeout_ms = t_ms;
    running = true;
    for (auto const& [phys, state] : registry) {
        if (reserved_bcm_pins.count(state->bcm_pin)) {
            std::cout << "[GPIO] Skipping physical pin " << phys << " / BCM" << state->bcm_pin
                      << " because it is reserved for another driver." << std::endl;
            continue;
        }
        workers.emplace_back(&GpioManager::worker_thread, this, phys, state->bcm_pin, state);
    }
}

void GpioManager::stop() {
    running = false;
    for (auto& t : workers) {
        if (t.joinable()) t.join();
    }
    workers.clear();
}

void GpioManager::updateTimeout(int new_timeout) {
    timeout_ms = new_timeout;
}

void GpioManager::worker_thread(int phys, int bcm, std::shared_ptr<PinState> state) {
    gpiod::chip chip("0");
    gpiod::line line = chip.get_line(bcm);
    bool requested = false;
    bool configured_output = false;

    while (running) {
        bool want_output;
        bool target_val;
        {
            std::lock_guard<std::mutex> lock(state->mtx);
            want_output = state->is_output;
            target_val = state->target_output_state;
        }

        try {
            if (!requested || configured_output != want_output) {
                if (requested) {
                    line.release();
                    requested = false;
                }

                gpiod::line_request req;
                req.consumer = "cm4_manager";
                req.flags.reset();
                req.request_type = want_output
                    ? gpiod::line_request::DIRECTION_OUTPUT
                    : gpiod::line_request::EVENT_BOTH_EDGES;

                line.request(req, target_val ? 1 : 0);
                requested = true;
                configured_output = want_output;
            }

            if (want_output) {
                line.set_value(target_val ? 1 : 0);
                bool actual = line.get_value();
                {
                    std::lock_guard<std::mutex> lock(state->mtx);
                    state->current_state = actual;
                    state->is_transitioning = false;
                    state->average_frequency = 0.0;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            } else {
                if (line.event_wait(std::chrono::milliseconds(100))) {
                    line.event_read(); // drain the event
                    auto now = std::chrono::steady_clock::now();
                    bool sampled = line.get_value();

                    std::lock_guard<std::mutex> lock(state->mtx);
                    if (sampled != state->current_state) {
                        state->current_state = sampled;

                        auto time_now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
                        auto local_time = std::localtime(&time_now);
                        std::cout << std::put_time(local_time, "[%H:%M:%S]")
                                  << " [TRANSITION] Phys Pin " << phys
                                  << " -> " << (sampled ? "HIGH" : "LOW") << std::endl;

                        if (!state->is_transitioning) {
                            state->is_transitioning = true;
                            state->transition_start_time = now;
                            state->transition_count = 0;
                        }
                        state->transition_count++;
                        state->last_toggle_time = now;
                    }
                }

                {
                    std::lock_guard<std::mutex> lock(state->mtx);
                    if (state->is_transitioning) {
                        auto quiet = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - state->last_toggle_time).count();

                        if (quiet >= timeout_ms) {
                            auto window = std::chrono::duration_cast<std::chrono::duration<double>>(
                                state->last_toggle_time - state->transition_start_time).count();

                            if (window > 0.001 && state->transition_count > 1) {
                                state->average_frequency = (state->transition_count / 2.0) / window;
                            } else {
                                state->average_frequency = 0.0;
                            }
                            state->is_transitioning = false;

                            auto time_now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
                            auto local_time = std::localtime(&time_now);
                            std::cout << std::put_time(local_time, "[%H:%M:%S]")
                                      << " [STABILIZED] Phys Pin " << phys
                                      << " Freq: " << std::fixed << std::setprecision(2)
                                      << state->average_frequency << " Hz" << std::endl;
                        }
                    }
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "[CRITICAL] Pin " << phys << " error: " << e.what() << std::endl;
            if (requested) {
                try { line.release(); } catch (...) {}
                requested = false;
            }
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    if (requested) {
        try { line.release(); } catch (...) {}
    }
}
