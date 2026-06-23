#include "WebServer.hpp"
#include <nlohmann/json.hpp>
#include <iostream>
#include <sstream>
#include <map>
#include <cstdlib>
#include <string>
#include <utility>

using json = nlohmann::json;

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
        .status-ok { color: #00ff87; background: rgba(0,255,135,.12); border-color: #00ff87; }
        .status-bad { color: #ff6b6b; background: rgba(255,65,65,.12); border-color: #ff4141; }
        .tiny { color: #777; font-size: .78rem; margin-top: 8px; }
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

    <div class="card scope-card" id="adcScopeCard">
        <div class="card-header">
            <div>
                <span class="pin-title">MCP3202 Dual-Channel Historical Micro-Scope</span>
                <span class="bcm-tag">CH0 + CH1, 12-bit @ 8 kHz frames</span>
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
            <span id="adcErr" style="color:#ff8a80"></span>
        </div>
        <div class="scope-wrap"><canvas id="adcScope" width="1200" height="220"></canvas></div>
        <div class="tiny"><span style="color:#00ff87">CH0 is green</span>, <span style="color:#ffcf33">CH1 is amber</span>. The scope endpoint returns recent ring-buffer history with min/max decimation so audio peaks remain visible while the browser polls at UI speed.</div>
    </div>

    <div class="grid" id="pinGrid"></div>
    <script>
        let timeoutInputLockExpiration = 0;

        function lockTimeoutInput() {
            timeoutInputLockExpiration = Date.now() + 5000;
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

        async function fetchAdcScope() {
            try {
                const canvas = document.getElementById('adcScope');
                const pointBudget = Math.max(300, Math.min(2400, canvas.clientWidth * 2));
                const res = await fetch(`/api/adc?points=${pointBudget}`);
                const data = await res.json();
                updateAdcHeader(data);
                drawScope((data.samples && data.samples.ch0) || [], (data.samples && data.samples.ch1) || []);
            } catch (err) {
                const status = document.getElementById('adcStatus');
                status.textContent = 'ERROR';
                status.className = 'status-pill status-bad';
                document.getElementById('adcErr').textContent = err;
            }
        }

        function updateAdcHeader(data) {
            const status = document.getElementById('adcStatus');
            status.textContent = data.enabled ? (data.healthy ? 'ADC OK' : 'ADC FAULT') : 'ADC DISABLED';
            status.className = `status-pill ${data.healthy ? 'status-ok' : 'status-bad'}`;
            document.getElementById('adcMode').textContent = data.bitbang ? 'bitbang GPIO' : 'hardware SPI';
            document.getElementById('adcRate').textContent = data.sample_rate_hz ? `${data.sample_rate_hz} Hz` : '-';
            const raw = data.latest_raw || [0, 0];
            const volts = data.latest_volts || [0, 0];
            document.getElementById('adcLatest0').textContent = `${raw[0] ?? 0} (${(volts[0] ?? 0).toFixed(3)} V)`;
            document.getElementById('adcLatest1').textContent = `${raw[1] ?? 0} (${(volts[1] ?? 0).toFixed(3)} V)`;
            document.getElementById('adcSamples').textContent = data.total_frames ?? 0;
            document.getElementById('adcDropped').textContent = data.dropped_reads ?? 0;
            document.getElementById('adcErr').textContent = data.last_error || '';
        }

        function drawScope(samples0, samples1) {
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

            if (!samples0.length && !samples1.length) {
                ctx.fillStyle = '#777';
                ctx.fillText('Waiting for MCP3202 samples...', 14, 24);
                return;
            }

            drawTrace(ctx, samples0, w, h, '#00ff87', 1.45);
            drawTrace(ctx, samples1, w, h, '#ffcf33', 1.25);

            ctx.fillStyle = '#8df7ff';
            ctx.font = '12px monospace';
            ctx.fillText(`0`, 6, h - 6);
            ctx.fillText(`2048`, 6, midY - 6);
            ctx.fillText(`4095`, 6, 14);
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
        if (ADC_ENABLED) setInterval(fetchAdcScope, 120);
        window.onload = () => { fetchStatus(); if (ADC_ENABLED) fetchAdcScope(); };
    </script>
</body>
</html>
)html";

WebServer::WebServer(std::map<int, std::shared_ptr<PinState>>& reg, ConfigManager& cfg, GpioManager& gpio, std::shared_ptr<SystemContext> ctx, AdcSampler* adc, std::set<int> reserved) 
    : registry(reg), config_mgr(cfg), gpio_mgr(gpio), context(ctx), adc_sampler(adc), reserved_bcm_pins(std::move(reserved)) {
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

    svr.Get("/api/adc", [this](const httplib::Request& req, httplib::Response& res) {
        if (!adc_sampler || !adc_sampler->isEnabled()) {
            res.status = 404;
            res.set_content("{\"enabled\":false,\"error\":\"ADC/graph disabled in full GPIO mode\"}", "application/json");
            return;
        }
        size_t points = 1600;
        if (req.has_param("points")) {
            points = std::max<size_t>(100, std::min<size_t>(5000, std::strtoul(req.get_param_value("points").c_str(), nullptr, 10)));
        }
        res.set_content(serialize_adc_scope(points), "application/json");
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

std::string WebServer::serialize_adc_scope(size_t max_points) {
    json j;
    if (!adc_sampler) {
        j = {
            {"enabled", false}, {"running", false}, {"healthy", false}, {"bitbang", false},
            {"sample_rate_hz", 0}, {"latest_raw", {0, 0}},
            {"latest_volts", {0.0, 0.0}}, {"total_frames", 0}, {"dropped_reads", 0},
            {"last_error", "ADC sampler not configured"},
            {"samples", {{"ch0", json::array()}, {"ch1", json::array()}}}
        };
        return j.dump();
    }

    auto s = adc_sampler->snapshot(max_points);
    j["enabled"] = s.enabled;
    j["running"] = s.running;
    j["healthy"] = s.healthy;
    j["bitbang"] = s.bitbang;
    j["sample_rate_hz"] = s.sample_rate_hz;
    j["latest_raw"] = {s.latest_raw[0], s.latest_raw[1]};
    j["latest_volts"] = {s.latest_volts[0], s.latest_volts[1]};
    j["total_frames"] = s.total_frames;
    j["dropped_reads"] = s.dropped_reads;
    j["last_error"] = s.last_error;
    j["samples"] = {
        {"ch0", s.samples[0]},
        {"ch1", s.samples[1]}
    };
    return j.dump();
}

void WebServer::listen(const std::string& host, int port) {
    svr.listen(host.c_str(), port);
}
