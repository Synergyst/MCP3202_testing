#include "TelephonyCoordinator.hpp"

#include <iostream>
#include <stdexcept>

namespace {
void require(bool ok, const std::string& msg) { if (!ok) throw std::runtime_error(msg); }

TelephonyCoordinator::Inputs base() {
    TelephonyCoordinator::Inputs in;
    in.ch_available = true;
    in.ri_level = true;
    in.ri_active = false;
    in.line_available = true;
    in.line_state = "silence";
    in.line_confidence = 0.7;
    in.caller_available = true;
    return in;
}

void check(const char* name, const TelephonyCoordinator::Settings& s, const TelephonyCoordinator::Inputs& in,
           uint64_t rings, int first_ms, int last_ms, bool cid_timeout, TelephonyCoordinator::State expected) {
    auto d = TelephonyCoordinator::decide(s, in, rings, first_ms, last_ms, cid_timeout);
    std::cout << name << ": state=" << TelephonyCoordinator::stateText(d.state)
              << " safe=" << d.safe_to_answer << " answer=" << d.should_answer
              << " cid_wait=" << d.caller_id_waiting << " status=" << d.status << "\n";
    require(d.state == expected, std::string(name) + " expected " + TelephonyCoordinator::stateText(expected));
}
}

int main() {
    try {
        TelephonyCoordinator::Settings s;
        auto in = base();
        check("idle", s, in, 0, 0, 0, false, TelephonyCoordinator::State::OnHookIdle);

        in = base(); in.ri_level = false; in.ri_active = true;
        s.auto_answer_enabled = true; s.caller_id_before_auto_answer = false; s.min_rings_before_answer = 1;
        auto d = TelephonyCoordinator::decide(s, in, 1, 2000, 0, false);
        std::cout << "ri low blocks: state=" << TelephonyCoordinator::stateText(d.state) << " block=" << d.answer_block_reason << "\n";
        require(!d.safe_to_answer && !d.should_answer && d.state == TelephonyCoordinator::State::AutoAnswerArmed, "RI LOW must arm but block answering");

        in = base(); in.ri_ringing = true;
        d = TelephonyCoordinator::decide(s, in, 1, 2000, 100, false);
        require(d.safe_to_answer && d.should_answer && d.state == TelephonyCoordinator::State::ReadyToAnswer, "RI HIGH safe gap should permit auto-answer");

        s.caller_id_before_auto_answer = true;
        in = base(); in.ri_ringing = true; in.caller_detected = false;
        d = TelephonyCoordinator::decide(s, in, 1, 2000, 100, false);
        require(d.caller_id_waiting && !d.should_answer && d.state == TelephonyCoordinator::State::CallerIdPending, "Caller ID wait should delay answer");
        d = TelephonyCoordinator::decide(s, in, 1, 8000, 100, true);
        require(!d.caller_id_waiting && d.should_answer, "Caller ID timeout should permit answer in safe gap");

        in = base(); in.offhook = true; in.line_state = "dial_tone";
        check("offhook dial", s, in, 0, 0, 0, false, TelephonyCoordinator::State::DialTone);
        in.line_state = "busy"; in.line_state_duration_ms = 100;
        check("offhook busy armed", s, in, 0, 0, 0, false, TelephonyCoordinator::State::Busy);
        in.line_state = "reorder"; in.line_state_duration_ms = 100;
        check("offhook reorder armed", s, in, 0, 0, 0, false, TelephonyCoordinator::State::Reorder);
        in.line_state = "remote_hangup_tone"; in.line_state_duration_ms = s.auto_hangup_after_disconnect_ms + 10;
        d = TelephonyCoordinator::decide(s, in, 0, 0, 0, false);
        require(d.should_hangup && d.state == TelephonyCoordinator::State::AutoHangupPending, "remote hangup tone should trigger auto-hangup after threshold");
        in.line_state = "receiver_offhook_tone"; in.line_state_duration_ms = s.auto_hangup_after_warning_ms + 10;
        d = TelephonyCoordinator::decide(s, in, 0, 0, 0, false);
        require(d.should_hangup && d.state == TelephonyCoordinator::State::AutoHangupPending, "receiver-off-hook warning should trigger auto-hangup after threshold");
        in.line_state = "unknown_tone"; in.line_state_duration_ms = s.auto_hangup_after_unknown_tone_ms + 10;
        d = TelephonyCoordinator::decide(s, in, 0, 0, 0, false);
        require(d.should_hangup && d.state == TelephonyCoordinator::State::AutoHangupPending, "unknown tonal ATA warning should trigger auto-hangup after threshold");

        s.require_line_state_ringing = true; s.allow_ri_only_ring = true; s.caller_id_before_auto_answer = false;
        in = base(); in.ri_active = true; in.ri_level = false; in.line_state = "silence";
        d = TelephonyCoordinator::decide(s, in, 1, 1000, 0, false);
        require(!d.incoming_ring, "require_line_state_ringing should reject RI-only ring");
        in.line_state = "ringing"; in.line_confidence = 0.8;
        d = TelephonyCoordinator::decide(s, in, 1, 1000, 0, false);
        require(d.incoming_ring, "DSP ringing should satisfy required line-state ringing");

        std::cout << "Telephony coordinator self-test: PASS\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Telephony coordinator self-test: FAIL: " << e.what() << "\n";
        return 1;
    }
}
