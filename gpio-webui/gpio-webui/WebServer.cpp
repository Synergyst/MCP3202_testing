#include "WebServer.hpp"
#include "AudioProcessing.hpp"
#include <nlohmann/json.hpp>
#include <iostream>
#include <sstream>
#include <map>
#include <cstdlib>
#include <string>
#include <utility>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cmath>
#include <fstream>
#include <vector>
#include <cctype>

using json = nlohmann::json;

namespace {
std::string readFskSourceSetting(const std::shared_ptr<SystemContext>& context) {
    json j;
    if (context) {
        std::lock_guard<std::mutex> lock(context->config_mutex);
        std::ifstream f(context->config_path);
        if (f.is_open()) { try { f >> j; } catch (...) { j = json::object(); } }
    }
    if (!j.is_object()) j = json::object();
    std::string src = j.value("fsk_source", "auto");
    if (src != "auto" && src != "software_adc") src = "auto";
    return src;
}

double callerIdScore(const json& j) {
    double score = j.value("confidence", 0.0);
    if (j.value("detected", false)) score += 1.0;
    if (j.value("checksum_ok", false)) score += 2.0;
    return score;
}

json disabledCallerIdJson(const std::string& source, const std::string& reason) {
    return {{"enabled", false}, {"running", false}, {"detected", false}, {"checksum_ok", false},
            {"source", source}, {"source_label", source}, {"status", reason}, {"last_error", reason},
            {"confidence", 0.0}, {"number", ""}, {"name", ""}, {"date_time", ""},
            {"message_type", ""}, {"raw_bits", ""}, {"raw_bytes_hex", ""}};
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

std::vector<std::string> splitEffectsCsv(const std::string& csv) {
    std::vector<std::string> out;
    std::stringstream ss(csv);
    std::string item;
    while (std::getline(ss, item, ',')) {
        item = lowerCopy(trimCopy(item));
        if (item.empty() || item == "none") continue;
        if (std::find(out.begin(), out.end(), item) == out.end()) out.push_back(item);
    }
    return out;
}

uint16_t clampAdc(double v) {
    if (v < 0.0) return 0;
    if (v > 4095.0) return 4095;
    return static_cast<uint16_t>(std::llround(v));
}

struct GraphBiquad {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    double z1 = 0.0, z2 = 0.0;
    double process(double x) {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

GraphBiquad makeGraphBandpass(double sample_rate, double center_hz, double q) {
    GraphBiquad f;
    if (sample_rate <= 0.0 || center_hz <= 0.0 || center_hz >= sample_rate * 0.45 || q <= 0.0) return f;
    const double w0 = 2.0 * M_PI * center_hz / sample_rate;
    const double alpha = std::sin(w0) / (2.0 * q);
    const double cosw0 = std::cos(w0);
    const double a0 = 1.0 + alpha;
    f.b0 = alpha / a0;
    f.b1 = 0.0;
    f.b2 = -alpha / a0;
    f.a1 = (-2.0 * cosw0) / a0;
    f.a2 = (1.0 - alpha) / a0;
    return f;
}

GraphBiquad makeGraphNotch(double sample_rate, double center_hz, double q) {
    GraphBiquad f;
    if (sample_rate <= 0.0 || center_hz <= 0.0 || center_hz >= sample_rate * 0.45 || q <= 0.0) return f;
    const double w0 = 2.0 * M_PI * center_hz / sample_rate;
    const double alpha = std::sin(w0) / (2.0 * q);
    const double cosw0 = std::cos(w0);
    const double a0 = 1.0 + alpha;
    f.b0 = 1.0 / a0;
    f.b1 = (-2.0 * cosw0) / a0;
    f.b2 = 1.0 / a0;
    f.a1 = (-2.0 * cosw0) / a0;
    f.a2 = (1.0 - alpha) / a0;
    return f;
}

std::vector<uint16_t> applyGraphEffectsToChannel(const std::vector<uint16_t>& raw, uint32_t sample_rate, const std::string& effects_csv) {
    if (raw.empty()) return {};
    std::vector<double> x;
    x.reserve(raw.size());
    for (uint16_t s : raw) x.push_back(static_cast<double>(s) - 2048.0);

    const auto effects = splitEffectsCsv(effects_csv);
    for (const auto& effect : effects) {
        if (effect == "dc_block") {
            double mean = 0.0;
            for (double v : x) mean += v;
            mean /= static_cast<double>(std::max<size_t>(1, x.size()));
            for (double& v : x) v -= mean;
        } else if (effect == "hum_notch_60" || effect == "hum_notch_120") {
            GraphBiquad notch = makeGraphNotch(std::max<double>(1.0, sample_rate), effect == "hum_notch_60" ? 60.0 : 120.0, 35.0);
            for (double& sample : x) sample = notch.process(sample);
        } else if (effect == "voice_agc") {
            double gain = 1.0;
            for (double& sample : x) {
                double desired = 9000.0 / std::max(1000.0, std::abs(sample * 16.0));
                desired = std::max(0.25, std::min(8.0, desired));
                const double coeff = desired < gain ? 0.10 : 0.003;
                gain += coeff * (desired - gain);
                sample *= gain;
            }
        } else if (effect == "pots_bandpass" || effect == "bell202_bandpass") {
            const double fs = std::max<double>(1.0, sample_rate);
            const double hp_cut = (effect == "bell202_bandpass") ? 700.0 : 300.0;
            const double lp_cut_req = (effect == "bell202_bandpass") ? 2700.0 : 3400.0;
            const int passes = (effect == "bell202_bandpass") ? 2 : 1;
            const double lp_cut = std::min(lp_cut_req, fs * 0.45);
            for (int pass = 0; pass < passes && lp_cut > hp_cut; ++pass) {
                const double hp_rc = 1.0 / (2.0 * M_PI * hp_cut);
                const double dt = 1.0 / fs;
                const double hp_alpha = hp_rc / (hp_rc + dt);
                const double lp_rc = 1.0 / (2.0 * M_PI * lp_cut);
                const double lp_alpha = dt / (lp_rc + dt);
                double hp_y = 0.0, hp_prev_x = 0.0, lp_y = 0.0;
                for (double& sample : x) {
                    const double in = sample;
                    const double yhp = hp_alpha * (hp_y + in - hp_prev_x);
                    hp_y = yhp;
                    hp_prev_x = in;
                    lp_y = lp_y + lp_alpha * (yhp - lp_y);
                    sample = lp_y;
                }
            }
        } else if (effect == "bell202_dual_tone") {
            GraphBiquad mark = makeGraphBandpass(std::max<double>(1.0, sample_rate), 1200.0, 8.0);
            GraphBiquad space = makeGraphBandpass(std::max<double>(1.0, sample_rate), 2200.0, 8.0);
            for (double& sample : x) sample = (mark.process(sample) + space.process(sample)) * 2.0;
        } else if (effect == "fsk_squelch") {
            if (x.size() >= 8) {
                const size_t win = std::max<size_t>(8, static_cast<size_t>(std::max<uint32_t>(1, sample_rate) / 200));
                std::vector<double> env(x.size(), 0.0);
                double sumsq = 0.0;
                for (size_t i = 0; i < x.size(); ++i) {
                    sumsq += x[i] * x[i];
                    if (i >= win) sumsq -= x[i - win] * x[i - win];
                    env[i] = std::sqrt(std::max(0.0, sumsq / static_cast<double>(std::min(win, i + 1))));
                }
                auto sorted = env;
                std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 3, sorted.end());
                const double floor = std::max(1.0, sorted[sorted.size() / 3]);
                const double open = floor * 2.2;
                const double full = floor * 5.0;
                for (size_t i = 0; i < x.size(); ++i) {
                    double gain = 0.18;
                    if (env[i] >= full) gain = 1.0;
                    else if (env[i] > open) gain = 0.18 + 0.82 * ((env[i] - open) / std::max(1.0, full - open));
                    x[i] *= gain;
                }
            }
        } else if (effect == "normalize") {
            double peak = 0.0;
            for (double v : x) peak = std::max(peak, std::abs(v));
            if (peak > 1.0) {
                const double target = 1800.0;
                const double gain = target / peak;
                if (gain > 1.01) for (double& v : x) v *= gain;
            }
        } else if (effect == "soft_clip") {
            constexpr double drive = 1.6;
            const double denom = std::tanh(drive);
            for (double& v : x) {
                const double xn = v / 2048.0;
                v = (std::tanh(xn * drive) / denom) * 2047.0;
            }
        } else if (effect == "rnnoise") {
            // RNNoise is intentionally skipped for the live graph. It is expensive
            // and speech-oriented; keep it available for WAV export only.
        }
    }

    std::vector<uint16_t> out;
    out.reserve(x.size());
    for (double v : x) out.push_back(clampAdc(2048.0 + v));
    return out;
}

json selectedCallerIdJson(const std::string& source, CallerIdDetector* cid) {
    auto software = [&]() -> json {
        if (!cid) return disabledCallerIdJson("software_adc", "Software ADC Caller ID detector disabled");
        json out = cid->snapshotJson();
        out["source"] = "software_adc";
        out["source_label"] = "Software ADC";
        return out;
    };

    if (source == "auto" || source == "software_adc") return software();
    return disabledCallerIdJson(source, "Unknown Caller ID source");
}
}

const char* HTML_UI = R"html(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>CM4 GPIO + MCP3202 Monitor</title>
    <style>
        body { background-color: #121212; color: #e0e0e0; font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; margin: 0; padding: 20px; display: flex; flex-direction: column; align-items: center; }
        h1 { color: #ffffff; margin-bottom: 5px; }
        .subtitle { color: #888; margin-bottom: 30px; font-size: 0.9rem; }
        .config-panel { background: #1e1e1e; padding: 15px 25px; border-radius: 10px; margin-bottom: 20px; box-shadow: 0 4px 10px rgba(0,0,0,0.3); display: flex; align-items: center; gap: 15px; flex-wrap: wrap; }
        .config-panel input { background: #2a2a2a; border: 1px solid #444; color: #fff; padding: 8px; border-radius: 5px; width: 80px; text-align: center; }
        .config-panel input.wide { width: 110px; }
        .config-panel button { background: #00adb5; color: white; border: none; padding: 8px 15px; border-radius: 5px; cursor: pointer; transition: 0.2s; }
        .config-panel button:hover { background: #00f5ff; }
        .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(280px, 1fr)); gap: 20px; width: 100%; max-width: 1200px; }
        .card { background: #1e1e1e; border-radius: 12px; padding: 20px; box-shadow: 0 4px 15px rgba(0,0,0,0.5); border: 1px solid #2d2d2d; display: flex; flex-direction: column; position: relative; }
        .card-header { display: flex; justify-content: space-between; align-items: center; border-bottom: 1px solid #333; padding-bottom: 10px; margin-bottom: 15px; }
        .pin-title { font-size: 1.3rem; font-weight: bold; }
        .bcm-tag { font-size: 0.8rem; color: #888; background: #2a2a2a; padding: 2px 6px; border-radius: 4px; }
        .status-container { display: flex; align-items: center; justify-content: space-between; margin-bottom: 20px; }
        .bulb { width: 40px; height: 40px; border-radius: 50%; background-color: #333; transition: all 0.2s ease-in-out; }
        .bulb.high { background-color: #00ff87; box-shadow: 0 0 15px #00ff87, 0 0 30px #00ff87; }
        .bulb.low { background-color: #ff4141; box-shadow: 0 0 15px #ff4141, 0 0 30px #ff4141; }
        .metrics { font-size: 0.95rem; line-height: 1.6; }
        .metrics span { font-weight: bold; color: #00adb5; }
        .controls { margin-top: auto; display: flex; flex-direction: column; gap: 10px; border-top: 1px solid #333; padding-top: 15px; }
        .control-row { display: flex; justify-content: space-between; align-items: center; }
        select, button.action-btn { background: #2a2a2a; color: #fff; border: 1px solid #444; padding: 6px 12px; border-radius: 5px; cursor: pointer; }
        .record-panel { justify-content: flex-start; margin-top: 14px; margin-bottom: 0; padding: 12px 14px; }
        .record-panel select { min-width: 170px; }
        .effects-panel { align-items: flex-start; }
        .effects-list { display: flex; gap: 10px; flex-wrap: wrap; max-width: 760px; }
        .effect-item { display: inline-flex; align-items: center; gap: 5px; color: #d5d5d5; font-size: .86rem; background: #20272a; border: 1px solid #334; padding: 5px 8px; border-radius: 6px; }
        .effect-item.disabled { opacity: .48; }
        .effect-item input { width: auto; }
        .record-help { color: #9aa; font-size: .82rem; }
        .record-status { color: #8df7ff; font-size: .85rem; min-width: 160px; }
        button.action-btn.active-high { background: #00ff87; color: #000; font-weight: bold; }
        button.action-btn.active-low { background: #ff4141; color: #fff; font-weight: bold; }
        .io-badge { padding: 4px 8px; border-radius: 5px; font-size: 0.8rem; font-weight: bold; }
        .io-in { background: rgba(0, 173, 181, 0.2); color: #00adb5; border: 1px solid #00adb5; }
        .io-out { background: rgba(240, 165, 0, 0.2); color: #f0a500; border: 1px solid #f0a500; }
        .scope-card { width: 100%; max-width: 1200px; box-sizing: border-box; margin-bottom: 20px; }
        body.gpio-only .scope-card { display: none; }
        .scope-top { display: flex; gap: 18px; align-items: center; flex-wrap: wrap; color: #bdbdbd; font-size: 0.9rem; }
        .scope-top span b { color: #00f5ff; }
        .scope-wrap { position: relative; margin-top: 12px; background: #070b0d; border: 1px solid #263238; border-radius: 8px; overflow: hidden; }
        #adcScope { display: block; width: 100%; height: 220px; image-rendering: pixelated; }
        .status-pill { padding: 4px 9px; border-radius: 999px; font-size: 0.8rem; border: 1px solid #555; }
        .cid-card { width: 100%; max-width: 1200px; box-sizing: border-box; margin-bottom: 20px; }
        body.gpio-only .cid-card { display: none; }
        .cid-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(180px, 1fr)); gap: 10px 18px; margin-top: 10px; }
        .cid-item { color: #bdbdbd; font-size: .9rem; }
        .cid-item b { color: #8df7ff; display: block; margin-top: 2px; word-break: break-word; }
        .cid-raw { margin-top: 12px; background: #080b0c; border: 1px solid #263238; border-radius: 8px; padding: 10px; font-family: monospace; color: #d7f7ff; white-space: pre-wrap; overflow-wrap: anywhere; max-height: 150px; overflow-y: auto; }
        .cid-tune { margin-top: 12px; align-items: center; }
        .cid-tune input[type="number"] { width: 76px; }
        .cid-tune select { min-width: 120px; }
        .cid-tune label { color: #cfcfcf; font-size: .86rem; }
        .status-ok { color: #00ff87; background: rgba(0,255,135,.12); border-color: #00ff87; }
        .status-bad { color: #ff6b6b; background: rgba(255,65,65,.12); border-color: #ff4141; }
        .tiny { color: #777; font-size: .78rem; margin-top: 8px; }
        .tabs { display:flex; gap:8px; flex-wrap:wrap; width:100%; max-width:1200px; margin: 0 0 16px 0; }
        .tab-btn { background:#1e1e1e; color:#d8d8d8; border:1px solid #333; border-radius:8px; padding:10px 14px; cursor:pointer; }
        .tab-btn.active { background:#00adb5; color:#fff; border-color:#00f5ff; }
        .tab-panel { display:none; width:100%; max-width:1200px; }
        .tab-panel.active { display:block; }
    </style>
</head>
<body class="__BODY_CLASS__">
    <h1>CM4 GPIO + MCP3202 Monitor Dashboard</h1>
    <div class="subtitle">Real-time physical header tracking, register control, and 8 kHz ADC audio scope</div>
    <div class="config-panel">
        <label for="timeoutInput">Steady-State Timeout (ms):</label>
        <input type="number" id="timeoutInput" value="5000" oninput="lockTimeoutInput()" onfocus="lockTimeoutInput()">
        <button onclick="updateTimeout()">Apply</button>
    </div>

    <div class="tabs">
        <button class="tab-btn active" data-tab="scope" onclick="selectTab('scope')">Scope / Audio</button>
        <button class="tab-btn" data-tab="caller" onclick="selectTab('caller')">Caller ID</button>
        <button class="tab-btn" data-tab="telephony" onclick="selectTab('telephony')">Telephony</button>
        <button class="tab-btn" data-tab="gpio" onclick="selectTab('gpio')">GPIO</button>
    </div>

    <div class="tab-panel active" id="tab-scope">
    <div class="card scope-card" id="adcScopeCard">
        <div class="card-header">
            <div>
                <span id="adcTitle" class="pin-title">MCP3202 Dual-Channel Historical Micro-Scope</span>
                <span id="adcTag" class="bcm-tag">CH0 + CH1, 12-bit @ 8 kHz frames</span>
            </div>
            <span class="status-pill status-bad" id="adcStatus">WAITING</span>
        </div>
        <div class="scope-top">
            <span>Mode: <b id="adcMode">-</b></span>
            <span>Frame Rate: <b id="adcRate">-</b></span>
            <span style="color:#00ff87">CH0: <b id="adcLatest0">-</b></span>
            <span style="color:#ffcf33">CH1: <b id="adcLatest1">-</b></span>
            <span>Frames: <b id="adcSamples">-</b></span>
            <span>Dropped: <b id="adcDropped">-</b></span>
            <span>History: <b id="adcHistory">-</b></span>
            <span id="adcErr" style="color:#ff8a80"></span>
        </div>
        <div class="scope-wrap"><canvas id="adcScope" width="1200" height="220"></canvas></div>
        <div class="config-panel record-panel">
            <label>Graph view:
                <select id="graphView">
                    <option value="raw">Raw ADC</option>
                    <option value="filtered">Filtered preview</option>
                    <option value="overlay">Raw + filtered overlay</option>
                </select>
            </label>
            <span class="record-help">Filtered/overlay graph uses the selected Effects below without downloading a WAV.</span>
        </div>
        <div class="config-panel record-panel">
            <label for="wavDuration">WAV capture:</label>
            <input class="wide" type="number" id="wavDuration" min="1" max="600000" step="100" value="1000">
            <span>ms</span>
            <select id="wavMode">
                <option value="ch0">Mono CH0</option>
                <option value="ch1">Mono CH1</option>
                <option value="stereo">Stereo CH0 + CH1</option>
                <option value="mix">Mono mix CH0 + CH1</option>
            </select>
            <select id="audioCodec">
                <option value="pcm16">WAV PCM 16-bit</option>
            </select>
            <button onclick="downloadWav()">Download Audio</button>
            <button onclick="previewWav()">Preview Audio</button>
            <span class="record-status" id="wavStatus"></span>
            <span class="record-help">Downloads or previews the newest n ms already held in the ADC ring buffer as 16-bit PCM.</span>
            <audio id="audioPreview" controls style="width:100%; max-width:520px; display:none;"></audio>
        </div>
        <div class="config-panel record-panel effects-panel">
            <label>Effects:</label>
            <div class="effects-list" id="effectsList">Loading audio modules...</div>
        </div>
        <div class="tiny"><span style="color:#00ff87">CH0 is green</span>, <span style="color:#ffcf33">CH1 is amber</span>. The scope endpoint returns recent ring-buffer history with min/max decimation so audio peaks remain visible while the browser polls at UI speed.</div>
    </div>
    </div>

    <div class="tab-panel" id="tab-caller">
    <div class="card cid-card" id="callerIdCard">
        <div class="card-header">
            <div>
                <span class="pin-title">Caller ID FSK Detector</span>
                <span class="bcm-tag">Bell 202: 1200 Hz mark / 2200 Hz space @ 1200 baud</span>
            </div>
            <span class="status-pill status-bad" id="cidStatus">LISTENING</span>
        </div>
        <div class="cid-grid">
            <div class="cid-item">Number<b id="cidNumber">-</b></div>
            <div class="cid-item">Name<b id="cidName">-</b></div>
            <div class="cid-item">Date/Time<b id="cidDate">-</b></div>
            <div class="cid-item">Message Type<b id="cidType">-</b></div>
            <div class="cid-item">Checksum<b id="cidChecksum">-</b></div>
            <div class="cid-item">Current Confidence<b id="cidConfidence">-</b></div>
            <div class="cid-item">Best Confidence<b id="cidBestConfidence">-</b></div>
            <div class="cid-item">Channel<b id="cidChannel">-</b></div>
            <div class="cid-item">Best Channel<b id="cidBestChannel">-</b></div>
            <div class="cid-item">Last Update<b id="cidUpdate">-</b></div>
        </div>
        <div class="config-panel record-panel cid-tune">
            <label>FSK channel <select id="cidTuneChannel"><option value="0">CH0</option><option value="1">CH1</option><option value="2">Mix</option><option value="-1">Auto</option></select></label>
            <label>Mark <input type="number" id="cidTuneMark" value="1200" step="5"> Hz</label>
            <label>Space <input type="number" id="cidTuneSpace" value="2200" step="5"> Hz</label>
            <label>Baud <input type="number" id="cidTuneBaud" value="1200" step="1"></label>
            <label>Window <input type="number" id="cidTuneWindow" value="5000" step="250"> ms</label>
            <label><input type="checkbox" id="cidTuneNormalize" checked> normalize</label>
            <label>Headroom <input type="number" id="cidTuneHeadroom" value="6" step="1"> dB</label>
            <label>Extra gain <input type="number" id="cidTuneGain" value="12" step="1"> dB</label>
            <label><input type="checkbox" id="cidTuneDc" checked> DC block</label>
            <button onclick="applyCallerIdSettings()">Apply FSK Tune</button>
            <button onclick="reloadCallerIdSettings()">Reload Settings</button>
            <span class="record-status" id="cidTuneStatus"></span>
        </div>
        <div class="tiny" id="cidError"></div>
        <div class="cid-raw" id="cidRaw">Raw FSK bits/bytes will appear here when detected.</div>
        <div class="cid-raw" id="cidBestRaw">Best confidence FSK bits/bytes will appear here.</div>
    </div>
    </div>

    <div class="tab-panel" id="tab-telephony">
    <div class="card cid-card" id="telephonyCard">
        <div class="card-header"><div><span class="pin-title">CH1817 DAA</span><span class="bcm-tag">OFFHK + RI</span></div><span class="status-pill" id="chStatus">-</span></div>
        <div class="cid-grid">
            <div class="cid-item">RI level<b id="chRiLevel">-</b></div><div class="cid-item">Ringing<b id="chRinging">-</b></div><div class="cid-item">RI frequency<b id="chFreq">-</b></div><div class="cid-item">Hook state<b id="chHook">-</b></div>
        </div>
        <div class="config-panel record-panel cid-tune">
            <button onclick="setChOffhook(false)">Go On-Hook</button><button onclick="setChOffhook(true)">Go Off-Hook</button>
            <label><input type="checkbox" id="chAutoAnswer"> auto-answer</label><label>Delay <input type="number" id="chAutoDelay" min="0" max="600000" step="100" value="0"> ms</label><button onclick="applyCh1817Settings()">Apply CH1817</button><span class="record-status" id="chApplyStatus"></span>
        </div>
        <div class="tiny" id="chHelp"></div>
    </div>
    </div>

    <div class="tab-panel" id="tab-gpio">
    <div class="grid" id="pinGrid"></div>
    </div>
    <script>
        let timeoutInputLockExpiration = 0;

        function lockTimeoutInput() {
            timeoutInputLockExpiration = Date.now() + 5000;
        }

        function selectTab(name) {
            document.querySelectorAll('.tab-panel').forEach(p => p.classList.remove('active'));
            document.querySelectorAll('.tab-btn').forEach(b => b.classList.remove('active'));
            const panel = document.getElementById(`tab-${name}`);
            const btn = document.querySelector(`.tab-btn[data-tab="${name}"]`);
            if (panel) panel.classList.add('active');
            if (btn) btn.classList.add('active');
            saveUiPref('activeTab', name);
            if (name === 'scope') fetchAdcScope();
        }

        function ensurePinCard(pinId, pin) {
            const grid = document.getElementById('pinGrid');
            let card = document.getElementById(`pin-card-${pinId}`);

            if (card) return card;

            card = document.createElement('div');
            card.id = `pin-card-${pinId}`;
            card.className = 'card';
            card.innerHTML = `
                <div class="card-header">
                    <div>
                        <span class="pin-title">Physical Pin ${pin.physical_pin}</span>
                        <span class="bcm-tag">BCM ${pin.bcm_pin}</span>
                    </div>
                    <span class="io-badge" id="io-badge-${pinId}"></span>
                </div>
                <div class="status-container">
                    <div class="bulb" id="bulb-${pinId}"></div>
                    <div class="metrics">
                        <div>State: <span id="state-${pinId}"></span></div>
                        <div>Transitioning: <span id="transitioning-${pinId}"></span></div>
                        <div>Frequency: <span id="frequency-${pinId}"></span></div>
                    </div>
                </div>
                <div class="controls">
                    <div class="control-row">
                        <label>Direction Mode:</label>
                        <select id="mode-${pinId}" onchange="updateConfig(${pinId}, 'mode', this.value)">
                            <option value="in">Read-Only (Input)</option>
                            <option value="out">Output Override</option>
                        </select>
                    </div>
                    <div id="output-row-${pinId}"></div>
                </div>`;

            grid.appendChild(card);
            return card;
        }

        async function fetchStatus() {
            try {
                const res = await fetch('/api/status');
                const data = await res.json();

                const timeoutInput = document.getElementById('timeoutInput');
                if (Date.now() > timeoutInputLockExpiration && document.activeElement !== timeoutInput) {
                    timeoutInput.value = data.timeout_ms;
                }

                for (const pinId in data.pins) {
                    const pin = data.pins[pinId];
                    ensurePinCard(pinId, pin);

                    const badge = document.getElementById(`io-badge-${pinId}`);
                    badge.textContent = pin.is_output ? 'OUTPUT' : 'INPUT';
                    badge.className = `io-badge ${pin.is_output ? 'io-out' : 'io-in'}`;

                    const bulb = document.getElementById(`bulb-${pinId}`);
                    bulb.className = `bulb ${pin.state ? 'high' : 'low'}`;

                    document.getElementById(`state-${pinId}`).textContent = pin.state ? 'HIGH (1)' : 'LOW (0)';
                    document.getElementById(`transitioning-${pinId}`).textContent = pin.is_transitioning ? 'YES' : 'NO';
                    document.getElementById(`frequency-${pinId}`).textContent = `${pin.frequency.toFixed(2)} Hz`;

                    const modeSelect = document.getElementById(`mode-${pinId}`);
                    if (document.activeElement !== modeSelect) {
                        modeSelect.value = pin.is_output ? 'out' : 'in';
                    }

                    const outputRow = document.getElementById(`output-row-${pinId}`);
                    const desiredOutputHtml = pin.is_output ? `
                        <div class="control-row">
                            <label>Set Output State:</label>
                            <button class="action-btn ${pin.state ? 'active-high' : 'active-low'}"
                                    onclick="updateConfig(${pinId}, 'state', ${pin.state ? 'false' : 'true'})">
                                Drive ${pin.state ? 'LOW' : 'HIGH'}
                            </button>
                        </div>` : '';

                    if (outputRow.dataset.lastHtml !== desiredOutputHtml) {
                        outputRow.innerHTML = desiredOutputHtml;
                        outputRow.dataset.lastHtml = desiredOutputHtml;
                    }
                }
            } catch (err) {
                console.error("Error updates:", err);
            }
        }

        function channelLabel(ch) {
            return ch === 0 ? 'CH0' : (ch === 1 ? 'CH1' : (ch === 2 ? 'CH0+CH1 mix' : (ch === -1 ? 'Auto' : '-')));
        }

        let cidTuneDirty = false;
        function markCidTuneDirty() { cidTuneDirty = true; document.getElementById('cidTuneStatus').textContent = 'Unsaved changes'; }
        function cidTuneFocused() { return ['cidTuneChannel','cidTuneMark','cidTuneSpace','cidTuneBaud','cidTuneWindow','cidTuneNormalize','cidTuneHeadroom','cidTuneGain','cidTuneDc'].includes(document.activeElement && document.activeElement.id); }
        function fillCallerIdSettings(settings, force=false) {
            if (!settings || (!force && (cidTuneDirty || cidTuneFocused()))) return;
            document.getElementById('cidTuneChannel').value = settings.channel ?? 0;
            document.getElementById('cidTuneMark').value = settings.mark_hz ?? 1200;
            document.getElementById('cidTuneSpace').value = settings.space_hz ?? 2200;
            document.getElementById('cidTuneBaud').value = settings.baud ?? 1200;
            document.getElementById('cidTuneWindow').value = settings.analysis_ms ?? 5000;
            document.getElementById('cidTuneNormalize').checked = settings.normalize !== false;
            document.getElementById('cidTuneHeadroom').value = settings.normalize_headroom_db ?? 6;
            document.getElementById('cidTuneGain').value = settings.extra_gain_db ?? 12;
            document.getElementById('cidTuneDc').checked = settings.dc_block !== false;
            cidTuneDirty = false;
        }

        function wireCallerIdTuneDirty() {
            ['cidTuneChannel','cidTuneMark','cidTuneSpace','cidTuneBaud','cidTuneWindow','cidTuneNormalize','cidTuneHeadroom','cidTuneGain','cidTuneDc'].forEach(id => {
                const el = document.getElementById(id); if (el) { el.addEventListener('input', markCidTuneDirty); el.addEventListener('change', markCidTuneDirty); }
            });
            const graphView = document.getElementById('graphView');
            if (graphView) graphView.addEventListener('change', fetchAdcScope);
        }

        async function reloadCallerIdSettings() {
            const status = document.getElementById('cidTuneStatus'); status.textContent = 'Reloading...';
            try { const res = await fetch('/api/caller-id/settings'); const data = await res.json(); if(!res.ok) throw new Error(data.error || `HTTP ${res.status}`); fillCallerIdSettings(data, true); status.textContent = 'Reloaded'; }
            catch(err) { status.textContent = `Reload failed: ${err.message || err}`; }
        }

        async function applyCallerIdSettings() {
            cidTuneDirty = true;
            const settings = {
                channel: parseInt(document.getElementById('cidTuneChannel').value, 10),
                mark_hz: parseFloat(document.getElementById('cidTuneMark').value),
                space_hz: parseFloat(document.getElementById('cidTuneSpace').value),
                baud: parseFloat(document.getElementById('cidTuneBaud').value),
                analysis_ms: parseInt(document.getElementById('cidTuneWindow').value, 10),
                normalize: document.getElementById('cidTuneNormalize').checked,
                normalize_headroom_db: parseFloat(document.getElementById('cidTuneHeadroom').value),
                extra_gain_db: parseFloat(document.getElementById('cidTuneGain').value),
                dc_block: document.getElementById('cidTuneDc').checked
            };
            const status = document.getElementById('cidTuneStatus');
            status.textContent = 'Applying...';
            try {
                const res = await fetch('/api/caller-id/settings', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify(settings)
                });
                const data = await res.json();
                if (!res.ok) throw new Error(data.error || `HTTP ${res.status}`);
                status.textContent = 'Applied';
                cidTuneDirty = false;
                fillCallerIdSettings(data.settings || data, true);
                fetchCallerId();
            } catch (err) {
                status.textContent = `Failed: ${err.message || err}`;
            }
        }

        async function fetchCallerId() {
            try {
                const res = await fetch('/api/caller-id');
                const data = await res.json();
                const status = document.getElementById('cidStatus');
                status.textContent = data.detected ? (data.checksum_ok ? 'DECODED' : 'DECODED?') : (data.status || 'LISTENING').toUpperCase();
                status.className = `status-pill ${data.detected ? 'status-ok' : 'status-bad'}`;
                document.getElementById('cidNumber').textContent = data.number || '-';
                document.getElementById('cidName').textContent = data.name || '-';
                document.getElementById('cidDate').textContent = data.date_time || data.date_time_raw || '-';
                document.getElementById('cidType').textContent = data.message_type || '-';
                document.getElementById('cidChecksum').textContent = data.detected ? (data.checksum_ok ? 'OK' : 'BAD/unknown') : '-';
                document.getElementById('cidConfidence').textContent = data.confidence ? data.confidence.toFixed(3) : '-';
                document.getElementById('cidBestConfidence').textContent = data.best_confidence ? data.best_confidence.toFixed(3) : '-';
                const ch = data.selected_channel;
                document.getElementById('cidChannel').textContent = channelLabel(ch);
                document.getElementById('cidBestChannel').textContent = channelLabel(data.best_selected_channel);
                document.getElementById('cidUpdate').textContent = data.last_update || '-';
                document.getElementById('cidError').textContent = data.last_error || data.status || '';
                document.getElementById('cidRaw').textContent = `Current raw bytes: ${data.raw_bytes_hex || '-'}\nCurrent raw bits: ${data.raw_bits || '-'}`;
                document.getElementById('cidBestRaw').textContent = `Best status: ${data.best_status || '-'}\nBest raw bytes: ${data.best_raw_bytes_hex || '-'}\nBest raw bits: ${data.best_raw_bits || '-'}`;
                fillCallerIdSettings(data.settings);
            } catch (err) {
                document.getElementById('cidError').textContent = `Caller ID error: ${err}`;
            }
        }

        async function fetchTelephony() {
            try {
                const res = await fetch('/api/telephony/ch1817'); const data = await res.json();
                const st = document.getElementById('chStatus'); st.textContent = data.ringing ? 'RINGING' : (data.offhook ? 'OFF-HOOK' : 'IDLE'); st.className = `status-pill ${data.ringing ? 'status-ok' : 'status-bad'}`;
                document.getElementById('chRiLevel').textContent = data.ri_level_text || '-'; document.getElementById('chRinging').textContent = data.ringing ? 'YES' : 'NO'; document.getElementById('chFreq').textContent = `${(data.ri_frequency_hz || 0).toFixed(2)} Hz`; document.getElementById('chHook').textContent = data.offhook ? 'off-hook' : 'on-hook';
                if (document.activeElement?.id !== 'chAutoDelay' && document.activeElement?.id !== 'chAutoAnswer') { document.getElementById('chAutoAnswer').checked = data.settings?.auto_answer_enabled || false; document.getElementById('chAutoDelay').value = data.settings?.auto_answer_delay_ms ?? 0; }
                document.getElementById('chHelp').textContent = data.last_error || data.help || '';
            } catch(err) { document.getElementById('chHelp').textContent = `CH1817 error: ${err.message || err}`; }
        }

        async function setChOffhook(offhook) { await fetch('/api/telephony/ch1817/offhook',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({offhook})}); fetchTelephony(); }
        async function applyCh1817Settings() {
            const st=document.getElementById('chApplyStatus'); st.textContent='Applying...';
            try { const res=await fetch('/api/telephony/ch1817/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({auto_answer_enabled:document.getElementById('chAutoAnswer').checked,auto_answer_delay_ms:parseInt(document.getElementById('chAutoDelay').value||'0',10)})}); const data=await res.json(); if(!res.ok) throw new Error(data.error||'failed'); st.textContent='Applied'; fetchTelephony(); }
            catch(err){ st.textContent=`Failed: ${err.message||err}`; alert(`Illegal CH1817 configuration:\n${err.message||err}`); }
        }


        async function fetchAdcScope() {
            try {
                const canvas = document.getElementById('adcScope');
                const pointBudget = Math.max(300, Math.min(2400, canvas.clientWidth * 2));
                const view = document.getElementById('graphView')?.value || 'raw';
                const effects = selectedEffectsCsv();
                const res = await fetch(`/api/adc?points=${pointBudget}&view=${encodeURIComponent(view)}&effects=${encodeURIComponent(effects)}`);
                const data = await res.json();
                updateAdcHeader(data);
                drawScope(data);
            } catch (err) {
                const status = document.getElementById('adcStatus');
                status.textContent = 'ADC ERROR';
                status.className = 'status-pill status-bad';
                document.getElementById('adcErr').textContent = String(err);
            }
        }

        function updateAdcHeader(data) {
            const sourceNames = {
                'mcp3202-spidev': 'MCP3202 Dual-Channel Historical Micro-Scope',
                'rp2040': 'RP2040 Dual-Channel Historical Micro-Scope'
            };
            const titleEl = document.getElementById('adcTitle');
            if (titleEl) titleEl.textContent = sourceNames[data.adc_source] || 'ADC Dual-Channel Scope';

            const tagEl = document.getElementById('adcTag');
            if (tagEl) {
                const res = (data.adc_source === 'rp2040' || data.adc_source === 'mcp3202-spidev') ? '12-bit' : 'unknown-bit';
                const rateKhz = (data.measured_sample_rate_hz / 1000).toFixed(1);
                const interfaceType = data.adc_source === 'rp2040' ? 'USB CDC' : 'SPI';
                tagEl.textContent = `CH0 + CH1, ${res} @ ${rateKhz} kHz frames (${interfaceType})`;
            }

            const status = document.getElementById('adcStatus');
            status.textContent = data.enabled ? (data.healthy ? 'ADC OK' : 'ADC FAULT') : 'ADC DISABLED';
            status.className = `status-pill ${data.healthy ? 'status-ok' : 'status-bad'}`;
            
            let modeText = 'Unknown';
            if (data.adc_source === 'rp2040') {
                modeText = 'RP2040 (USB)';
            } else if (data.adc_source === 'mcp3202-spidev') {
                modeText = data.bitbang ? 'bitbang GPIO' : 'hardware SPI';
            }
            document.getElementById('adcMode').textContent = modeText;
            const nominalRate = data.sample_rate_hz || 0;
            const measuredRate = data.measured_sample_rate_hz || nominalRate;
            document.getElementById('adcRate').textContent = nominalRate ? `${measuredRate} Hz actual (${nominalRate} Hz requested)` : '-';
            const raw = data.latest_raw || [0, 0];
            const volts = data.latest_volts || [0, 0];
            document.getElementById('adcLatest0').textContent = `${raw[0] ?? 0} (${(volts[0] ?? 0).toFixed(3)} V)`;
            document.getElementById('adcLatest1').textContent = `${raw[1] ?? 0} (${(volts[1] ?? 0).toFixed(3)} V)`;
            document.getElementById('adcSamples').textContent = data.total_frames ?? 0;
            document.getElementById('adcDropped').textContent = data.dropped_reads ?? 0;
            const availMs = data.available_history_ms || 0;
            const capMs = data.history_capacity_ms || 0;
            document.getElementById('adcHistory').textContent = capMs ? `${(availMs/1000).toFixed(1)}s / ${(capMs/1000).toFixed(1)}s` : '-';
            document.getElementById('adcErr').textContent = data.last_error || '';
        }

        function drawScope(data) {
            const samples0 = (data.samples && data.samples.ch0) || [];
            const samples1 = (data.samples && data.samples.ch1) || [];
            const filt0 = (data.filtered_samples && data.filtered_samples.ch0) || [];
            const filt1 = (data.filtered_samples && data.filtered_samples.ch1) || [];
            const view = data.graph_view || document.getElementById('graphView')?.value || 'raw';

            const canvas = document.getElementById('adcScope');
            const dpr = window.devicePixelRatio || 1;
            const w = Math.max(300, canvas.clientWidth || 1200);
            const h = Math.max(160, canvas.clientHeight || 220);
            if (canvas.width !== Math.floor(w * dpr) || canvas.height !== Math.floor(h * dpr)) {
                canvas.width = Math.floor(w * dpr);
                canvas.height = Math.floor(h * dpr);
            }
            const ctx = canvas.getContext('2d');
            ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
            ctx.clearRect(0, 0, w, h);
            ctx.fillStyle = '#070b0d';
            ctx.fillRect(0, 0, w, h);

            ctx.strokeStyle = 'rgba(0, 173, 181, 0.18)';
            ctx.lineWidth = 1;
            for (let x = 0; x < w; x += w / 12) { ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, h); ctx.stroke(); }
            for (let y = 0; y < h; y += h / 8) { ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(w, y); ctx.stroke(); }

            const midY = h / 2;
            ctx.strokeStyle = 'rgba(255,255,255,0.25)';
            ctx.beginPath(); ctx.moveTo(0, midY); ctx.lineTo(w, midY); ctx.stroke();

            if (!samples0.length && !samples1.length && !filt0.length && !filt1.length) {
                ctx.fillStyle = '#777';
                ctx.fillText('Waiting for MCP3202 samples...', 14, 24);
                return;
            }

            if (view === 'overlay' && (filt0.length || filt1.length)) {
                drawTrace(ctx, samples0, w, h, 'rgba(0,255,135,0.32)', 1.0);
                drawTrace(ctx, samples1, w, h, 'rgba(255,207,51,0.32)', 1.0);
                drawTrace(ctx, filt0, w, h, '#00ff87', 1.8);
                drawTrace(ctx, filt1, w, h, '#ffcf33', 1.6);
            } else {
                drawTrace(ctx, samples0, w, h, '#00ff87', 1.45);
                drawTrace(ctx, samples1, w, h, '#ffcf33', 1.25);
            }

            ctx.fillStyle = '#8df7ff';
            ctx.font = '12px monospace';
            ctx.fillText(`0`, 6, h - 6);
            ctx.fillText(`2048`, 6, midY - 6);
            ctx.fillText(`4095`, 6, 14);
            let label = view === 'overlay' ? 'overlay: dim=raw bright=filtered' : (view === 'filtered' ? 'filtered preview' : 'raw ADC');
            if ((data.graph_skipped_effects || []).length) label += ` / skipped live: ${(data.graph_skipped_effects || []).join(',')}`;
            ctx.fillStyle = '#aaa'; ctx.fillText(label, 80, 16);
            ctx.fillStyle = '#00ff87'; ctx.fillText('CH0', w - 76, 16);
            ctx.fillStyle = '#ffcf33'; ctx.fillText('CH1', w - 38, 16);
        }

        function drawTrace(ctx, samples, w, h, color, width) {
            if (!samples || !samples.length) return;
            ctx.strokeStyle = color;
            ctx.lineWidth = width;
            ctx.beginPath();
            for (let i = 0; i < samples.length; i++) {
                const x = samples.length === 1 ? 0 : (i / (samples.length - 1)) * (w - 1);
                const y = h - (Math.max(0, Math.min(4095, samples[i])) / 4095) * h;
                if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
            }
            ctx.stroke();
        }

        async function updateConfig(pin, action, value) {
            const params = new URLSearchParams();
            params.append('pin', pin);
            params.append('action', action);
            params.append('value', value);

            await fetch('/api/config', {
                method: 'POST',
                headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                body: params
            });
            fetchStatus();
        }

        async function loadAudioModules() {
            if (!ADC_ENABLED) return;
            try {
                const res = await fetch('/api/audio/modules');
                const data = await res.json();
                const effectsList = document.getElementById('effectsList');
                effectsList.innerHTML = '';
                const savedEffects = loadUiPref('effects', 'dc_block').split(',').filter(Boolean);
                for (const effect of (data.effects || [])) {
                    const label = document.createElement('label');
                    label.className = `effect-item ${effect.available ? '' : 'disabled'}`;
                    label.title = effect.description || '';
                    const input = document.createElement('input');
                    input.type = 'checkbox';
                    input.value = effect.id;
                    input.disabled = !effect.available;
                    input.checked = savedEffects.includes(effect.id);
                    input.addEventListener('change', () => { saveAudioUiPrefs(); fetchAdcScope(); });
                    label.appendChild(input);
                    label.appendChild(document.createTextNode(effect.label || effect.id));
                    effectsList.appendChild(label);
                }
                const codecSelect = document.getElementById('audioCodec');
                codecSelect.innerHTML = '';
                for (const codec of (data.codecs || [])) {
                    const opt = document.createElement('option');
                    opt.value = codec.id;
                    opt.textContent = codec.label || codec.id;
                    opt.disabled = !codec.available;
                    if (codec.id === loadUiPref('audioCodec', 'pcm16')) opt.selected = true;
                    codecSelect.appendChild(opt);
                }
                codecSelect.addEventListener('change', saveAudioUiPrefs);
                saveAudioUiPrefs();
            } catch (err) {
                document.getElementById('effectsList').textContent = `Unable to load audio modules: ${err}`;
            }
        }

        const UI_PREF_PREFIX = 'cm4gpio.';
        function saveUiPref(key, value) { try { localStorage.setItem(UI_PREF_PREFIX + key, value); } catch (_) {} }
        function loadUiPref(key, fallback='') { try { return localStorage.getItem(UI_PREF_PREFIX + key) ?? fallback; } catch (_) { return fallback; } }

        function saveAudioUiPrefs() {
            const dur = document.getElementById('wavDuration');
            const mode = document.getElementById('wavMode');
            const codec = document.getElementById('audioCodec');
            const graph = document.getElementById('graphView');
            if (dur) saveUiPref('wavDuration', dur.value);
            if (mode) saveUiPref('wavMode', mode.value);
            if (codec) saveUiPref('audioCodec', codec.value);
            if (graph) saveUiPref('graphView', graph.value);
            saveUiPref('effects', selectedEffectsCsv());
        }

        function restoreSimpleUiPrefs() {
            const dur = document.getElementById('wavDuration');
            const mode = document.getElementById('wavMode');
            const graph = document.getElementById('graphView');
            if (dur) dur.value = loadUiPref('wavDuration', dur.value || '1000');
            if (mode) mode.value = loadUiPref('wavMode', mode.value || 'ch0');
            if (graph) graph.value = loadUiPref('graphView', graph.value || 'raw');
            [dur, mode, graph].forEach(el => { if (el) el.addEventListener('change', saveAudioUiPrefs); });
            if (dur) dur.addEventListener('input', saveAudioUiPrefs);
        }

        function selectedEffectsCsv() {
            return Array.from(document.querySelectorAll('#effectsList input[type="checkbox"]:checked'))
                .map(input => input.value)
                .join(',');
        }

        function currentWavUrl() {
            const durInput = document.getElementById('wavDuration');
            const modeSelect = document.getElementById('wavMode');
            const codecSelect = document.getElementById('audioCodec');
            const ms = Math.max(1, Math.min(600000, parseInt(durInput.value || '1000', 10)));
            durInput.value = ms;
            const mode = modeSelect.value;
            const codec = codecSelect.value || 'pcm16';
            const effects = selectedEffectsCsv();
            saveAudioUiPrefs();
            return `/api/adc/wav?ms=${encodeURIComponent(ms)}&mode=${encodeURIComponent(mode)}&codec=${encodeURIComponent(codec)}&effects=${encodeURIComponent(effects)}`;
        }

        async function downloadWav() {
            const status = document.getElementById('wavStatus');
            status.textContent = 'Preparing download...';
            try {
                const res = await fetch(currentWavUrl());
                if (!res.ok) {
                    let msg = `HTTP ${res.status}`;
                    try { const j = await res.json(); msg = j.error || msg; } catch (_) {}
                    throw new Error(msg);
                }
                const blob = await res.blob();
                const requestedMs = parseInt(res.headers.get('X-Requested-Duration-Ms') || '0', 10);
                const actualMs = parseInt(res.headers.get('X-Actual-Duration-Ms') || '0', 10);
                const availableMs = parseInt(res.headers.get('X-Available-Duration-Ms') || '0', 10);
                const truncated = (res.headers.get('X-Wav-Truncated') || 'false') === 'true';
                const cd = res.headers.get('Content-Disposition') || '';
                const m = cd.match(/filename="?([^";]+)"?/i);
                const filename = m ? m[1] : `mcp3202_${mode}_${ms}ms.wav`;
                const url = URL.createObjectURL(blob);
                const a = document.createElement('a');
                a.href = url;
                a.download = filename;
                document.body.appendChild(a);
                a.click();
                a.remove();
                URL.revokeObjectURL(url);
                status.textContent = truncated
                    ? `Downloaded ${filename} — truncated to ${actualMs} ms; only ${availableMs} ms available.`
                    : `Downloaded ${filename}${actualMs ? ` (${actualMs} ms)` : ''}`;
            } catch (err) {
                status.textContent = `Download failed: ${err.message || err}`;
            }
        }

        async function previewWav() {
            const status = document.getElementById('wavStatus');
            const player = document.getElementById('audioPreview');
            status.textContent = 'Preparing preview...';
            try {
                const res = await fetch(currentWavUrl());
                if (!res.ok) {
                    let msg = `HTTP ${res.status}`;
                    try { const j = await res.json(); msg = j.error || msg; } catch (_) {}
                    throw new Error(msg);
                }
                const actualMs = parseInt(res.headers.get('X-Actual-Duration-Ms') || '0', 10);
                const availableMs = parseInt(res.headers.get('X-Available-Duration-Ms') || '0', 10);
                const truncated = (res.headers.get('X-Wav-Truncated') || 'false') === 'true';
                const blob = await res.blob();
                if (player.dataset.objectUrl) URL.revokeObjectURL(player.dataset.objectUrl);
                const url = URL.createObjectURL(blob);
                player.dataset.objectUrl = url;
                player.src = url;
                player.style.display = 'block';
                await player.play().catch(() => {});
                status.textContent = truncated
                    ? `Preview ready — truncated to ${actualMs} ms; only ${availableMs} ms available.`
                    : `Preview ready${actualMs ? ` (${actualMs} ms)` : ''}`;
            } catch (err) {
                status.textContent = `Preview failed: ${err.message || err}`;
            }
        }

        async function updateTimeout() {
            const val = document.getElementById('timeoutInput').value;
            const params = new URLSearchParams();
            params.append('action', 'timeout');
            params.append('value', val);

            timeoutInputLockExpiration = 0;

            await fetch('/api/config', {
                method: 'POST',
                headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                body: params
            });
            fetchStatus();
        }

        const ADC_ENABLED = __ADC_ENABLED__;
        setInterval(fetchStatus, 250);
        if (ADC_ENABLED) { setInterval(fetchAdcScope, 120); setInterval(fetchCallerId, 1000); }
        setInterval(fetchTelephony, 500);
        window.onload = () => { restoreSimpleUiPrefs(); selectTab(loadUiPref('activeTab', 'scope')); fetchStatus(); wireCallerIdTuneDirty(); if (ADC_ENABLED) { loadAudioModules().then(() => fetchAdcScope()); fetchCallerId(); } fetchTelephony(); };
    </script>
</body>
</html>
)html";

WebServer::WebServer(std::map<int, std::shared_ptr<PinState>>& reg, ConfigManager& cfg, GpioManager& gpio, std::shared_ptr<SystemContext> ctx, AdcSampler* adc, CallerIdDetector* cid, Ch1817Driver* ch1817, std::set<int> reserved) 
    : registry(reg), config_mgr(cfg), gpio_mgr(gpio), context(ctx), adc_sampler(adc), caller_id_detector(cid), ch1817_driver(ch1817), reserved_bcm_pins(std::move(reserved)) {
    setup_routes();
}

void WebServer::setup_routes() {
    svr.Get("/", [this](const httplib::Request&, httplib::Response& res) {
        std::string html = HTML_UI;
        const bool adc_enabled = (adc_sampler != nullptr && adc_sampler->isEnabled());
        auto replace_all = [](std::string& s, const std::string& from, const std::string& to) {
            size_t pos = 0;
            while ((pos = s.find(from, pos)) != std::string::npos) {
                s.replace(pos, from.length(), to);
                pos += to.length();
            }
        };
        replace_all(html, "__ADC_ENABLED__", adc_enabled ? "true" : "false");
        replace_all(html, "__BODY_CLASS__", adc_enabled ? "adc-enabled" : "gpio-only");
        res.set_content(html, "text/html");
    });

    svr.Get("/api/status", [this](const httplib::Request&, httplib::Response& res) {
        res.set_content(serialize_state(), "application/json");
    });

    svr.Get("/api/system/settings", [this](const httplib::Request&, httplib::Response& res) {
        json j;
        std::string src = readFskSourceSetting(context);
        res.set_content(json{{"fsk_source", src}}.dump(), "application/json");
    });

    svr.Post("/api/adc/config", [this](const httplib::Request& req, httplib::Response& res) {
        if (!adc_sampler) {
            res.status = 404;
            res.set_content("{\"error\":\"ADC sampler not configured\"}", "application/json");
            return;
        }
        try {
            json j = json::parse(req.body.empty() ? "{}" : req.body);
            
            AdcSampler::Config cfg; 
            cfg.adc_source = j.value("adc_source", "mcp3202-spidev");
            cfg.rp2040_dev = j.value("rp2040_dev", "/dev/ttyACM0");
            
            adc_sampler->updateConfig(cfg);
            
            // Update GPIO reservations based on the new source
            std::set<int> new_reserved;
            if (cfg.adc_source == "mcp3202-spidev") {
                // Use defaults for SPI pins as per main.cpp logic
                new_reserved.insert(11); // CLK
                new_reserved.insert(10); // MOSI
                new_reserved.insert(9);  // MISO
                new_reserved.insert(8);   // CS
            }
            
            // Merge with telephony pins
            if (ch1817_driver && ch1817_driver->settings().enabled) {
                new_reserved.insert(ch1817_driver->offhookBcm());
                new_reserved.insert(ch1817_driver->riBcm());
            }
            
            reserved_bcm_pins = std::move(new_reserved);
            
            // Persist to config.json
            config_mgr.setSetting("adc_source", cfg.adc_source);
            config_mgr.setSetting("rp2040_dev", cfg.rp2040_dev);
            
            res.set_content("{\"status\":\"ok\"}", "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"status", "error"}, {"error", e.what()}}.dump(), "application/json");
        }
    });

    svr.Post("/api/system/settings", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = json::parse(req.body.empty() ? "{}" : req.body);
            std::string src = body.value("fsk_source", "auto");
            if (src != "auto" && src != "software_adc") throw std::runtime_error("Illegal FSK source; use auto or software_adc.");
            std::lock_guard<std::mutex> lock(context->config_mutex); json j; { std::ifstream f(context->config_path); if (f.is_open()) { try { f >> j; } catch (...) { j = json::object(); } } } if (!j.is_object()) j = json::object(); j["fsk_source"] = src; std::ofstream out(context->config_path); if (out.is_open()) out << j.dump(2);
            res.set_content(json{{"status","ok"},{"fsk_source",src}}.dump(), "application/json");
        } catch (const std::exception& e) { res.status = 400; res.set_content(json{{"status","error"},{"error",e.what()}}.dump(), "application/json"); }
    });

    svr.Get("/api/adc", [this](const httplib::Request& req, httplib::Response& res) {
        if (!adc_sampler || !adc_sampler->isEnabled()) {
            res.status = 404;
            res.set_content("{\"enabled\":false,\"error\":\"ADC/graph disabled in full GPIO mode\"}", "application/json");
            return;
        }
        size_t points = 1600;
        if (req.has_param("points")) {
            const size_t requested_points = std::strtoul(req.get_param_value("points").c_str(), nullptr, 10);
            points = requested_points == 0 ? 0 : std::max<size_t>(100, std::min<size_t>(5000, requested_points));
        }
        std::string view = req.has_param("view") ? req.get_param_value("view") : "raw";
        std::string effects = req.has_param("effects") ? req.get_param_value("effects") : "";
        res.set_content(serialize_adc_scope(points, view, effects), "application/json");
    });

    svr.Get("/api/caller-id", [this](const httplib::Request&, httplib::Response& res) {
        const std::string source = readFskSourceSetting(context);
        json out = selectedCallerIdJson(source, caller_id_detector);
        out["configured_source"] = source;
        res.set_content(out.dump(), "application/json");
    });

    svr.Get("/api/caller-id/settings", [this](const httplib::Request&, httplib::Response& res) {
        if (!caller_id_detector) {
            res.status = 404;
            res.set_content("{\"enabled\":false,\"error\":\"Caller ID detector disabled\"}", "application/json");
            return;
        }
        res.set_content(caller_id_detector->settingsJson().dump(), "application/json");
    });

    svr.Post("/api/caller-id/settings", [this](const httplib::Request& req, httplib::Response& res) {
        if (!caller_id_detector) {
            res.status = 404;
            res.set_content("{\"status\":\"error\",\"error\":\"Caller ID detector disabled\"}", "application/json");
            return;
        }
        try {
            json j = json::parse(req.body.empty() ? "{}" : req.body);
            caller_id_detector->updateSettingsFromJson(j);
            json out = {{"status", "ok"}, {"settings", caller_id_detector->settingsJson()}};
            res.set_content(out.dump(), "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            json out = {{"status", "error"}, {"error", e.what()}};
            res.set_content(out.dump(), "application/json");
        }
    });

    svr.Get("/api/telephony/ch1817", [this](const httplib::Request&, httplib::Response& res) {
        if (!ch1817_driver) { res.status = 404; res.set_content("{\"enabled\":false,\"error\":\"CH1817 driver disabled\"}", "application/json"); return; }
        res.set_content(ch1817_driver->snapshotJson().dump(), "application/json");
    });

    svr.Post("/api/telephony/ch1817/settings", [this](const httplib::Request& req, httplib::Response& res) {
        if (!ch1817_driver) { res.status = 404; res.set_content("{\"status\":\"error\",\"error\":\"CH1817 driver disabled\"}", "application/json"); return; }
        try { json j = json::parse(req.body.empty() ? "{}" : req.body); ch1817_driver->updateFromJson(j); res.set_content(json{{"status","ok"},{"settings",ch1817_driver->settingsJson()}}.dump(), "application/json"); }
        catch (const std::exception& e) { res.status = 400; res.set_content(json{{"status","error"},{"error",e.what()},{"help",ch1817_driver->validationHelp()}}.dump(), "application/json"); }
    });

    svr.Post("/api/telephony/ch1817/offhook", [this](const httplib::Request& req, httplib::Response& res) {
        if (!ch1817_driver) { res.status = 404; res.set_content("{\"status\":\"error\",\"error\":\"CH1817 driver disabled\"}", "application/json"); return; }
        try { json j = json::parse(req.body.empty() ? "{}" : req.body); ch1817_driver->setOffhook(j.value("offhook", false)); res.set_content(json{{"status","ok"},{"state",ch1817_driver->snapshotJson()}}.dump(), "application/json"); }
        catch (const std::exception& e) { res.status = 400; res.set_content(json{{"status","error"},{"error",e.what()},{"help",ch1817_driver->validationHelp()}}.dump(), "application/json"); }
    });

    svr.Get("/api/audio/modules", [this](const httplib::Request&, httplib::Response& res) {
        res.set_content(AudioProcessing::catalog().dump(), "application/json");
    });

    svr.Get("/api/adc/wav", [this](const httplib::Request& req, httplib::Response& res) {
        if (!adc_sampler || !adc_sampler->isEnabled()) {
            res.status = 404;
            res.set_content("{\"status\":\"error\",\"error\":\"ADC/graph disabled in GPIO-only mode\"}", "application/json");
            return;
        }

        size_t duration_ms = 1000;
        if (req.has_param("ms")) {
            duration_ms = std::strtoul(req.get_param_value("ms").c_str(), nullptr, 10);
        } else if (req.has_param("duration_ms")) {
            duration_ms = std::strtoul(req.get_param_value("duration_ms").c_str(), nullptr, 10);
        }
        duration_ms = std::max<size_t>(1, std::min<size_t>(600000, duration_ms));

        std::string mode = req.has_param("mode") ? req.get_param_value("mode") : "ch0";
        std::string effects = req.has_param("effects") ? req.get_param_value("effects") : "";
        std::string codec = req.has_param("codec") ? req.get_param_value("codec") : "pcm16";
        std::string filename;
        std::string content_type;
        try {
            size_t requested_ms = 0, actual_ms = 0, available_ms = 0;
            bool truncated = false;
            std::string payload = build_adc_wav(mode, duration_ms, effects, codec, filename, content_type,
                                                requested_ms, actual_ms, available_ms, truncated);
            res.set_header("Content-Disposition", "attachment; filename=\"" + filename + "\"");
            res.set_header("Cache-Control", "no-store");
            res.set_header("X-Requested-Duration-Ms", std::to_string(requested_ms));
            res.set_header("X-Actual-Duration-Ms", std::to_string(actual_ms));
            res.set_header("X-Available-Duration-Ms", std::to_string(available_ms));
            res.set_header("X-Wav-Truncated", truncated ? "true" : "false");
            res.set_content(payload.data(), payload.size(), content_type.c_str());
        } catch (const std::exception& e) {
            res.status = 400;
            json j = {{"status", "error"}, {"error", e.what()}};
            res.set_content(j.dump(), "application/json");
        }
    });

    svr.Post("/api/config", [this](const httplib::Request& req, httplib::Response& res) {
        bool changed = false;
        std::string action = req.get_param_value("action");
        std::string val = req.get_param_value("value");

        if (action == "timeout") {
            if (!val.empty()) {
                int new_t = std::stoi(val);
                context->timeout_ms = new_t;
                gpio_mgr.updateTimeout(new_t);
                changed = true;
            }
        } else {
            std::string pin_str = req.get_param_value("pin");
            if (!pin_str.empty()) {
                int phys = std::stoi(pin_str);
                if (registry.count(phys)) {
                    auto pin = registry[phys];
                    if (reserved_bcm_pins.count(pin->bcm_pin)) {
                        res.status = 403;
                        res.set_content("{\"status\":\"error\",\"error\":\"GPIO is reserved for ADC/SPI\"}", "application/json");
                        return;
                    }
                    std::lock_guard<std::mutex> lock(pin->mtx);
                    if (action == "mode") {
                        pin->is_output = (val == "out");
                        changed = true;
                    } else if (action == "state") {
                        pin->target_output_state = (val == "true");
                        changed = true;
                    }
                }
            }
        }
        
        if (changed) {
            std::lock_guard<std::mutex> lock(context->config_mutex);
            config_mgr.save(registry, context->timeout_ms);
        }
        res.set_content("{\"status\":\"ok\"}", "application/json");
    });
}

std::string WebServer::serialize_state() {
    json j;
    j["timeout_ms"] = context->timeout_ms.load();
    j["pins"] = json::object();

    for (const auto& [phys, pin] : registry) {
        std::lock_guard<std::mutex> lock(pin->mtx);
        j["pins"][std::to_string(phys)] = {
            {"physical_pin", pin->physical_pin},
            {"bcm_pin", pin->bcm_pin},
            {"is_output", pin->is_output},
            {"state", pin->current_state},
            {"is_transitioning", pin->is_transitioning},
            {"frequency", pin->average_frequency}
        };
    }
    return j.dump();
}

std::string WebServer::serialize_adc_scope(size_t max_points, const std::string& view, const std::string& effects_csv) {
    json j;
    if (!adc_sampler) {
        j = {
            {"enabled", false}, {"running", false}, {"healthy", false}, {"bitbang", false},
            {"sample_rate_hz", 0}, {"measured_sample_rate_hz", 0}, {"latest_raw", {0, 0}},
            {"latest_volts", {0.0, 0.0}}, {"total_frames", 0}, {"dropped_reads", 0},
            {"last_error", "ADC sampler not configured"}, {"graph_view", "raw"}, {"graph_effects", ""},
            {"samples", {{"ch0", json::array()}, {"ch1", json::array()}}}
        };
        return j.dump();
    }

    const std::string safe_view = (view == "filtered" || view == "overlay") ? view : "raw";
    auto s = (max_points == 0) ? adc_sampler->status() : adc_sampler->snapshot(max_points);
    const uint32_t sr = s.measured_sample_rate_hz ? s.measured_sample_rate_hz : s.sample_rate_hz;

    j["enabled"] = s.enabled;
    j["running"] = s.running;
    j["healthy"] = s.healthy;
    j["bitbang"] = s.bitbang;
    j["sample_rate_hz"] = s.sample_rate_hz;
    j["measured_sample_rate_hz"] = s.measured_sample_rate_hz;
    j["lifetime_sample_rate_hz"] = s.lifetime_sample_rate_hz;
    j["latest_raw"] = {s.latest_raw[0], s.latest_raw[1]};
    j["latest_volts"] = {s.latest_volts[0], s.latest_volts[1]};
    j["total_frames"] = s.total_frames;
    j["dropped_reads"] = s.dropped_reads;
    j["overruns"] = s.overruns;
    j["max_overrun_us"] = s.max_overrun_us;
    j["avg_frame_read_us"] = s.avg_frame_read_us;
    j["max_frame_read_us"] = s.max_frame_read_us;
    j["avg_mutex_wait_us"] = s.avg_mutex_wait_us;
    j["max_mutex_wait_us"] = s.max_mutex_wait_us;
    j["avg_mutex_hold_us"] = s.avg_mutex_hold_us;
    j["max_mutex_hold_us"] = s.max_mutex_hold_us;
    j["snapshot_count"] = s.snapshot_count;
    j["snapshot_samples_copied"] = s.snapshot_samples_copied;
    j["realtime_requested"] = s.realtime_requested;
    j["realtime_active"] = s.realtime_active;
    j["realtime_priority"] = s.realtime_priority;
    j["cpu_affinity"] = s.cpu_affinity;
    j["scheduler_status"] = s.scheduler_status;
    j["valid_samples"] = s.valid_samples;
    j["adc_source"] = s.adc_source;
    j["rp2040_connected"] = s.rp2040_connected;
    j["rp2040_packets_ok"] = s.rp2040_packets_ok;
    j["rp2040_packets_crc_bad"] = s.rp2040_packets_crc_bad;
    j["rp2040_sequence_gaps"] = s.rp2040_sequence_gaps;
    j["rp2040_firmware_lost_frames"] = s.rp2040_firmware_lost_frames;
    j["rp2040_firmware_flags"] = s.rp2040_firmware_flags;
    j["rp2040_dev"] = s.rp2040_dev;
    j["rp2040_declared_rate_hz"] = s.rp2040_declared_rate_hz;
    j["history_capacity_samples"] = s.history_capacity_samples;
    j["available_history_ms"] = (static_cast<uint64_t>(s.valid_samples) * 1000ull) / std::max<uint32_t>(1, sr);
    j["history_capacity_ms"] = (static_cast<uint64_t>(s.history_capacity_samples) * 1000ull) / std::max<uint32_t>(1, sr);
    j["last_error"] = s.last_error;
    j["graph_view"] = safe_view;
    j["graph_effects"] = effects_csv;
    json skipped = json::array();
    for (const auto& effect : splitEffectsCsv(effects_csv)) {
        if (effect == "rnnoise") skipped.push_back("rnnoise");
    }
    j["graph_skipped_effects"] = skipped;

    if (safe_view == "raw") {
        j["samples"] = {{"ch0", s.samples[0]}, {"ch1", s.samples[1]}};
    } else {
        auto f0 = applyGraphEffectsToChannel(s.samples[0], sr, effects_csv);
        auto f1 = applyGraphEffectsToChannel(s.samples[1], sr, effects_csv);
        if (safe_view == "filtered") {
            j["samples"] = {{"ch0", f0}, {"ch1", f1}};
        } else {
            j["samples"] = {{"ch0", s.samples[0]}, {"ch1", s.samples[1]}};
            j["filtered_samples"] = {{"ch0", f0}, {"ch1", f1}};
        }
    }
    return j.dump();
}

std::string WebServer::build_adc_wav(const std::string& mode, size_t duration_ms, const std::string& effects_csv, const std::string& codec,
                                    std::string& filename, std::string& content_type, size_t& requested_ms, size_t& actual_ms,
                                    size_t& available_ms, bool& truncated) {
    if (!adc_sampler) throw std::runtime_error("ADC sampler not configured");

    const auto options = AudioProcessing::parseOptions(effects_csv, codec);
    const auto meta = adc_sampler->status();
    const uint32_t sample_rate = std::max<uint32_t>(1, meta.measured_sample_rate_hz ? meta.measured_sample_rate_hz : meta.sample_rate_hz);
    // Duration selects how much ring-buffer history to copy. Use the measured rate so the downloaded WAV
    // has both the requested duration and the correct playback pitch even if Linux can't hit the requested rate exactly.
    requested_ms = duration_ms;
    const size_t requested_frames = std::max<size_t>(1, (static_cast<uint64_t>(sample_rate) * duration_ms + 999) / 1000);
    const auto data = adc_sampler->recent(requested_frames);
    const size_t frames = std::min(data.samples[0].size(), data.samples[1].size());
    if (frames == 0) throw std::runtime_error("No ADC samples available yet");
    available_ms = (static_cast<uint64_t>(data.valid_samples) * 1000ull) / std::max<uint32_t>(1, sample_rate);
    actual_ms = (static_cast<uint64_t>(frames) * 1000ull) / std::max<uint32_t>(1, sample_rate);
    truncated = frames < requested_frames;

    enum class WavMode { Ch0, Ch1, Stereo, Mix };
    WavMode wav_mode;
    std::string safe_mode;
    if (mode == "ch0" || mode == "mono-ch0") { wav_mode = WavMode::Ch0; safe_mode = "ch0"; }
    else if (mode == "ch1" || mode == "mono-ch1") { wav_mode = WavMode::Ch1; safe_mode = "ch1"; }
    else if (mode == "stereo" || mode == "ch0ch1") { wav_mode = WavMode::Stereo; safe_mode = "stereo"; }
    else if (mode == "mix" || mode == "mono-mix") { wav_mode = WavMode::Mix; safe_mode = "mix"; }
    else throw std::runtime_error("Invalid WAV mode; use ch0, ch1, stereo, or mix");

    auto pcm16 = [](uint16_t raw) -> int16_t {
        const int32_t centered = static_cast<int32_t>(std::max<uint16_t>(0, std::min<uint16_t>(4095, raw))) - 2048;
        return static_cast<int16_t>(std::max<int32_t>(-32768, std::min<int32_t>(32767, centered * 16)));
    };

    AudioBuffer buffer;
    buffer.sample_rate_hz = sample_rate;
    buffer.channels = (wav_mode == WavMode::Stereo) ? 2 : 1;
    buffer.samples.reserve(frames * buffer.channels);

    for (size_t i = 0; i < frames; ++i) {
        const int16_t ch0 = pcm16(data.samples[0][i]);
        const int16_t ch1 = pcm16(data.samples[1][i]);
        switch (wav_mode) {
            case WavMode::Ch0:
                buffer.samples.push_back(ch0);
                break;
            case WavMode::Ch1:
                buffer.samples.push_back(ch1);
                break;
            case WavMode::Stereo:
                buffer.samples.push_back(ch0);
                buffer.samples.push_back(ch1);
                break;
            case WavMode::Mix:
                buffer.samples.push_back(static_cast<int16_t>((static_cast<int32_t>(ch0) + static_cast<int32_t>(ch1)) / 2));
                break;
        }
    }

    AudioProcessing::applyEffects(buffer, options);
    std::string payload = AudioProcessing::encodeWav(buffer, options);

    std::string effects_part = options.effects.empty() ? "dry" : AudioProcessing::sanitizeForFilename(effects_csv);
    filename = "mcp3202_" + safe_mode + "_" + effects_part + "_" + std::to_string(actual_ms) + "ms_" + std::to_string(sample_rate) + "hz.wav";
    content_type = "audio/wav";
    return payload;
}

void WebServer::listen(const std::string& host, int port) {
    svr.listen(host.c_str(), port);
}
