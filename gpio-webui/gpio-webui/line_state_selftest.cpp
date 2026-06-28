#include "LineStateDetector.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
constexpr double kPi = 3.141592653589793238462643383279502884;

std::vector<float> tone(const std::vector<double>& hz, double sr, double seconds, double amp = 0.1) {
    const size_t n = static_cast<size_t>(std::llround(sr * seconds));
    std::vector<float> x(n, 0.0f);
    for (size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / sr;
        double v = 0.0;
        for (double f : hz) v += std::sin(2.0 * kPi * f * t);
        x[i] = static_cast<float>((amp * v) / std::max<size_t>(1, hz.size()));
    }
    return x;
}

void require(bool ok, const std::string& msg) {
    if (!ok) throw std::runtime_error(msg);
}

void checkState(const char* name, const std::vector<float>& x, LineStateDetector::State expected, bool ri = false) {
    LineStateDetector::Settings s;
    s.prefer_ri_for_ring_start = true;
    auto p = LineStateDetector::builtInProfile("nanp");
    auto r = LineStateDetector::analyzeSamplesForTest(x, 8000, s, p, ri);
    std::cout << name << ": got=" << LineStateDetector::stateText(r.state)
              << " confidence=" << r.confidence << " rms=" << r.rms
              << " status=" << r.status << "\n";
    require(r.state == expected, std::string(name) + " expected " + LineStateDetector::stateText(expected) + " got " + LineStateDetector::stateText(r.state));
}
}

int main() {
    try {
        constexpr double sr = 8000.0;
        checkState("silence", std::vector<float>(800, 0.0f), LineStateDetector::State::Silence);
        checkState("dial tone", tone({350.0, 440.0}, sr, 0.1, 0.12), LineStateDetector::State::DialTone);
        checkState("480/620 disconnect-family tone", tone({480.0, 620.0}, sr, 0.1, 0.12), LineStateDetector::State::RemoteHangupTone);
        checkState("ringback tone", tone({440.0, 480.0}, sr, 0.1, 0.12), LineStateDetector::State::Ringback);
        checkState("receiver off-hook warning", tone({1400.0, 2060.0, 2450.0, 2600.0}, sr, 0.1, 0.12), LineStateDetector::State::ReceiverOffHookTone);
        checkState("unknown high tone", tone({1800.0}, sr, 0.1, 0.12), LineStateDetector::State::UnknownTone);
        checkState("weak dial tone", tone({350.0, 440.0}, sr, 0.1, 0.006), LineStateDetector::State::DialTone);
        checkState("RI fast ring", std::vector<float>(800, 0.0f), LineStateDetector::State::Ringing, true);
        std::cout << "Line state self-test: PASS\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Line state self-test: FAIL: " << e.what() << "\n";
        return 1;
    }
}
