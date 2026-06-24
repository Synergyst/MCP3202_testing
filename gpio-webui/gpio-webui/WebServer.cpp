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
            <span class="record-status" id="wavStatus"></span>
            <span class="record-help">Downloads the newest n ms already held in the ADC ring buffer as 16-bit PCM.</span>
        </div>
        <div class="config-panel record-panel effects-panel">
            <label>Effects:</label>
            <div class="effects-list" id="effectsList">Loading audio modules...</div>
        </div>
        <div class="tiny"><span style="color:#00ff87">CH0 is green</span>, <span style="color:#ffcf33">CH1 is amber</span>. The scope endpoint returns recent ring-buffer history with min/max decimation so audio peaks remain visible while the browser polls at UI speed.</div>
    </div>

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

    <div class="card cid-card" id="htCard">
        <div class="card-header"><div><span class="pin-title">HT9032C Caller ID Receiver</span><span class="bcm-tag">CDET + RDET + DOUT + DOUTC</span></div><span class="status-pill" id="htStatus">-</span></div>
        <div class="cid-grid">
            <div class="cid-item">Carrier CDET<b id="htCarrier">-</b></div><div class="cid-item">Ring RDET<b id="htRing">-</b></div><div class="cid-item">PDWN/Power<b id="htPower">-</b></div><div class="cid-item">RDET level<b id="htRdetLevel">-</b></div>
            <div class="cid-item">DOUT level<b id="htDoutLevel">-</b></div><div class="cid-item">DOUTC level<b id="htDoutcLevel">-</b></div><div class="cid-item">DOUT decoded<b id="htDoutDecoded">-</b></div><div class="cid-item">DOUTC decoded<b id="htDoutcDecoded">-</b></div>
        </div>
        <div class="config-panel record-panel cid-tune">
            <label>FSK result source <select id="fskSource"><option value="auto">Auto best</option><option value="software_adc">Software ADC</option><option value="ht9032_dout">HT9032 DOUT</option><option value="ht9032_doutc">HT9032 DOUTC</option></select></label><button onclick="applyFskSource()">Apply Source</button>
            <label>Monitor <select id="htMonitor"><option value="both">DOUT + DOUTC</option><option value="dout">DOUT only</option><option value="doutc">DOUTC only</option></select></label>
            <label><input type="checkbox" id="htPowered" checked> powered</label><button onclick="applyHt9032Settings()">Apply HT9032</button><span class="record-status" id="htApplyStatus"></span>
        </div>
        <div class="cid-raw" id="htRaw">HT9032 raw bits/bytes will appear here.</div><div class="tiny" id="htHelp"></div>
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

        async function fetchHt9032() {
            try {
                const res = await fetch('/api/ht9032'); const data = await res.json();
                const st = document.getElementById('htStatus'); st.textContent = data.carrier ? 'CARRIER' : (data.status || '-').toUpperCase(); st.className = `status-pill ${data.carrier ? 'status-ok' : 'status-bad'}`;
                const rdetWired = (data.settings?.rdet_phys || 0) > 0;
                document.getElementById('htCarrier').textContent = data.carrier ? 'present' : 'absent'; document.getElementById('htRing').textContent = rdetWired ? (data.ring_detect ? 'detected' : 'absent') : 'not wired'; document.getElementById('htPower').textContent = data.settings?.pdwn_control ? (data.powered ? 'powered' : 'power-down') : 'PDWN not controlled'; document.getElementById('htRdetLevel').textContent = rdetWired ? (data.rdet_level ? 'HIGH' : 'LOW') : '-'; document.getElementById('htDoutLevel').textContent = data.dout_level ? 'HIGH' : 'LOW'; document.getElementById('htDoutcLevel').textContent = data.doutc_level ? 'HIGH' : 'LOW';
                document.getElementById('htDoutDecoded').textContent = data.dout?.decoded ? `${data.dout.number||''} ${data.dout.checksum_ok?'OK':'BAD'}` : (data.dout?.status || '-'); document.getElementById('htDoutcDecoded').textContent = data.doutc?.decoded ? `${data.doutc.number||''} ${data.doutc.checksum_ok?'OK':'BAD'}` : (data.doutc?.status || '-');
                if (document.activeElement?.id !== 'htMonitor' && document.activeElement?.id !== 'htPowered') { document.getElementById('htMonitor').value = data.settings?.monitor_mode || 'both'; document.getElementById('htPowered').checked = data.settings?.powered !== false; }
                document.getElementById('htRaw').textContent = `RDET: ${rdetWired ? (data.ring_detect ? 'detected' : 'absent') : 'not wired'} / level ${rdetWired ? (data.rdet_level ? 'HIGH' : 'LOW') : '-'} / PHYS ${data.settings?.rdet_phys ?? 0} / BCM ${data.settings?.rdet_bcm ?? -1}\nCDET: ${data.carrier ? 'carrier present' : 'carrier absent'} / level ${data.cdet_level ? 'HIGH' : 'LOW'}\n\nDOUT bytes: ${data.dout?.bytes_hex || '-'}\nDOUT bits: ${data.dout?.bits || '-'}\n\nDOUTC bytes: ${data.doutc?.bytes_hex || '-'}\nDOUTC bits: ${data.doutc?.bits || '-'}`; document.getElementById('htHelp').textContent = data.last_error || data.help || '';
            } catch(err) { document.getElementById('htHelp').textContent = `HT9032 error: ${err.message || err}`; }
        }

        async function applyHt9032Settings() {
            const st=document.getElementById('htApplyStatus'); st.textContent='Applying...';
            try { const res=await fetch('/api/ht9032/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({monitor_mode:document.getElementById('htMonitor').value,powered:document.getElementById('htPowered').checked})}); const data=await res.json(); if(!res.ok) throw new Error(data.error||'failed'); st.textContent='Applied'; fetchHt9032(); }
            catch(err){ st.textContent=`Failed: ${err.message||err}`; alert(`Illegal HT9032 configuration:\n${err.message||err}`); }
        }

        async function applyFskSource() {
            try { const res=await fetch('/api/system/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({fsk_source:document.getElementById('fskSource').value})}); const data=await res.json(); if(!res.ok) throw new Error(data.error||'failed'); }
            catch(err){ alert(`Unable to save FSK source: ${err.message||err}`); }
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
            const nominalRate = data.sample_rate_hz || 0;
            const measuredRate = data.measured_sample_rate_hz || nominalRate;
            document.getElementById('adcRate').textContent = nominalRate ? `${measuredRate} Hz actual (${nominalRate} Hz requested)` : '-';
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

        async function loadAudioModules() {
            if (!ADC_ENABLED) return;
            try {
                const res = await fetch('/api/audio/modules');
                const data = await res.json();
                const effectsList = document.getElementById('effectsList');
                effectsList.innerHTML = '';
                for (const effect of (data.effects || [])) {
                    const label = document.createElement('label');
                    label.className = `effect-item ${effect.available ? '' : 'disabled'}`;
                    label.title = effect.description || '';
                    const input = document.createElement('input');
                    input.type = 'checkbox';
                    input.value = effect.id;
                    input.disabled = !effect.available;
                    if (effect.id === 'dc_block') input.checked = true;
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
                    if (codec.id === 'pcm16') opt.selected = true;
                    codecSelect.appendChild(opt);
                }
            } catch (err) {
                document.getElementById('effectsList').textContent = `Unable to load audio modules: ${err}`;
            }
        }

        function selectedEffectsCsv() {
            return Array.from(document.querySelectorAll('#effectsList input[type="checkbox"]:checked'))
                .map(input => input.value)
                .join(',');
        }

        async function downloadWav() {
            const durInput = document.getElementById('wavDuration');
            const modeSelect = document.getElementById('wavMode');
            const codecSelect = document.getElementById('audioCodec');
            const status = document.getElementById('wavStatus');
            const ms = Math.max(1, Math.min(600000, parseInt(durInput.value || '1000', 10)));
            durInput.value = ms;
            const mode = modeSelect.value;
            const codec = codecSelect.value || 'pcm16';
            const effects = selectedEffectsCsv();
            status.textContent = 'Preparing download...';
            try {
                const res = await fetch(`/api/adc/wav?ms=${encodeURIComponent(ms)}&mode=${encodeURIComponent(mode)}&codec=${encodeURIComponent(codec)}&effects=${encodeURIComponent(effects)}`);
                if (!res.ok) {
                    let msg = `HTTP ${res.status}`;
                    try { const j = await res.json(); msg = j.error || msg; } catch (_) {}
                    throw new Error(msg);
                }
                const blob = await res.blob();
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
                status.textContent = `Downloaded ${filename}`;
            } catch (err) {
                status.textContent = `Download failed: ${err.message || err}`;
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
        setInterval(fetchTelephony, 500); setInterval(fetchHt9032, 500);
        window.onload = () => { fetchStatus(); wireCallerIdTuneDirty(); if (ADC_ENABLED) { fetchAdcScope(); fetchCallerId(); loadAudioModules(); } fetchTelephony(); fetchHt9032(); fetch('/api/system/settings').then(r=>r.json()).then(d=>{ if(d.fsk_source) document.getElementById('fskSource').value=d.fsk_source; }).catch(()=>{}); };
    </script>
</body>
</html>
)html";

WebServer::WebServer(std::map<int, std::shared_ptr<PinState>>& reg, ConfigManager& cfg, GpioManager& gpio, std::shared_ptr<SystemContext> ctx, AdcSampler* adc, CallerIdDetector* cid, Ch1817Driver* ch1817, Ht9032Driver* ht9032, std::set<int> reserved) 
    : registry(reg), config_mgr(cfg), gpio_mgr(gpio), context(ctx), adc_sampler(adc), caller_id_detector(cid), ch1817_driver(ch1817), ht9032_driver(ht9032), reserved_bcm_pins(std::move(reserved)) {
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
        { std::lock_guard<std::mutex> lock(context->config_mutex); std::ifstream f(context->config_path); if (f.is_open()) { try { f >> j; } catch (...) { j = json::object(); } } }
        if (!j.is_object()) j = json::object();
        res.set_content(json{{"fsk_source", j.value("fsk_source", "auto")}}.dump(), "application/json");
    });

    svr.Post("/api/system/settings", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = json::parse(req.body.empty() ? "{}" : req.body);
            std::string src = body.value("fsk_source", "auto");
            if (src != "auto" && src != "software_adc" && src != "ht9032_dout" && src != "ht9032_doutc") throw std::runtime_error("Illegal FSK source; use auto, software_adc, ht9032_dout, or ht9032_doutc.");
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
            points = std::max<size_t>(100, std::min<size_t>(5000, std::strtoul(req.get_param_value("points").c_str(), nullptr, 10)));
        }
        res.set_content(serialize_adc_scope(points), "application/json");
    });

    svr.Get("/api/caller-id", [this](const httplib::Request&, httplib::Response& res) {
        if (!caller_id_detector) {
            res.status = 404;
            res.set_content("{\"enabled\":false,\"error\":\"Caller ID detector disabled\"}", "application/json");
            return;
        }
        res.set_content(caller_id_detector->snapshotJson().dump(), "application/json");
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

    svr.Get("/api/ht9032", [this](const httplib::Request&, httplib::Response& res) {
        if (!ht9032_driver) { res.status = 404; res.set_content("{\"enabled\":false,\"error\":\"HT9032 driver disabled\"}", "application/json"); return; }
        res.set_content(ht9032_driver->snapshotJson().dump(), "application/json");
    });

    svr.Post("/api/ht9032/settings", [this](const httplib::Request& req, httplib::Response& res) {
        if (!ht9032_driver) { res.status = 404; res.set_content("{\"status\":\"error\",\"error\":\"HT9032 driver disabled\"}", "application/json"); return; }
        try { json j = json::parse(req.body.empty() ? "{}" : req.body); ht9032_driver->updateFromJson(j); res.set_content(json{{"status","ok"},{"settings",ht9032_driver->settingsJson()}}.dump(), "application/json"); }
        catch (const std::exception& e) { res.status = 400; res.set_content(json{{"status","error"},{"error",e.what()},{"help",ht9032_driver->validationHelp()}}.dump(), "application/json"); }
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
            std::string payload = build_adc_wav(mode, duration_ms, effects, codec, filename, content_type);
            res.set_header("Content-Disposition", "attachment; filename=\"" + filename + "\"");
            res.set_header("Cache-Control", "no-store");
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

std::string WebServer::serialize_adc_scope(size_t max_points) {
    json j;
    if (!adc_sampler) {
        j = {
            {"enabled", false}, {"running", false}, {"healthy", false}, {"bitbang", false},
            {"sample_rate_hz", 0}, {"measured_sample_rate_hz", 0}, {"latest_raw", {0, 0}},
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
    j["measured_sample_rate_hz"] = s.measured_sample_rate_hz;
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

std::string WebServer::build_adc_wav(const std::string& mode, size_t duration_ms, const std::string& effects_csv, const std::string& codec, std::string& filename, std::string& content_type) {
    if (!adc_sampler) throw std::runtime_error("ADC sampler not configured");

    const auto options = AudioProcessing::parseOptions(effects_csv, codec);
    const auto meta = adc_sampler->snapshot(1);
    const uint32_t sample_rate = std::max<uint32_t>(1, meta.measured_sample_rate_hz ? meta.measured_sample_rate_hz : meta.sample_rate_hz);
    // Duration selects how much ring-buffer history to copy. Use the measured rate so the downloaded WAV
    // has both the requested duration and the correct playback pitch even if Linux can't hit the requested rate exactly.
    const size_t requested_frames = std::max<size_t>(1, (static_cast<uint64_t>(sample_rate) * duration_ms + 999) / 1000);
    const auto data = adc_sampler->recent(requested_frames);
    const size_t frames = std::min(data.samples[0].size(), data.samples[1].size());
    if (frames == 0) throw std::runtime_error("No ADC samples available yet");

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

    const size_t actual_ms = (frames * 1000ull) / sample_rate;
    std::string effects_part = options.effects.empty() ? "dry" : AudioProcessing::sanitizeForFilename(effects_csv);
    filename = "mcp3202_" + safe_mode + "_" + effects_part + "_" + std::to_string(actual_ms) + "ms_" + std::to_string(sample_rate) + "hz.wav";
    content_type = "audio/wav";
    return payload;
}

void WebServer::listen(const std::string& host, int port) {
    svr.listen(host.c_str(), port);
}
