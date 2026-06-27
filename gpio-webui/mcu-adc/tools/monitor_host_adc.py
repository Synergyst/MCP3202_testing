#!/usr/bin/env python3
"""Poll the gpio-webui ADC endpoint and report RP2040 stream health.

Intended for RP2040 ADC phase-3 sweep/soak/stress tests.
Exits non-zero if a hard failure is observed unless --no-fail is used.
"""

import argparse
import json
import sys
import time
import urllib.error
import urllib.request


def fetch_json(url, timeout):
    with urllib.request.urlopen(url, timeout=timeout) as r:
        return json.loads(r.read().decode("utf-8"))


def num(d, key, default=0):
    v = d.get(key, default)
    return default if v is None else v


def main():
    ap = argparse.ArgumentParser(description="Monitor /api/adc RP2040 health")
    ap.add_argument("--url", default="http://127.0.0.1:8080/api/adc", help="ADC endpoint URL")
    ap.add_argument("--seconds", type=float, default=60.0, help="monitor duration")
    ap.add_argument("--interval", type=float, default=1.0, help="poll interval")
    ap.add_argument("--timeout", type=float, default=2.0, help="HTTP timeout")
    ap.add_argument("--json-summary", action="store_true", help="print final summary as JSON")
    ap.add_argument("--no-fail", action="store_true", help="always exit 0")
    args = ap.parse_args()

    url = args.url
    sep = "&" if "?" in url else "?"
    if "points=" not in url:
        url = f"{url}{sep}points=0"

    start_wall = time.time()
    deadline = start_wall + max(0.0, args.seconds)
    first = None
    prev = None
    last = None
    failures = []
    samples = 0

    header = (
        "elapsed  conn ok  health req_hz fw_hz meas_hz life_hz "
        "frames/s pkts/s crc+ gap+ lost+ flags last_error"
    )
    print(header, flush=True)

    while time.time() <= deadline or samples == 0:
        now = time.time()
        try:
            j = fetch_json(url, args.timeout)
        except (urllib.error.URLError, TimeoutError, json.JSONDecodeError, OSError) as e:
            msg = f"fetch failed: {e}"
            failures.append(msg)
            print(f"{now-start_wall:7.1f}  ERR {msg}", flush=True)
            if time.time() >= deadline:
                break
            time.sleep(args.interval)
            continue

        if first is None:
            first = j
        if prev is None:
            prev = j
            prev_time = now

        dt = max(1e-9, now - prev_time)
        frames_delta = num(j, "total_frames") - num(prev, "total_frames")
        packets_delta = num(j, "rp2040_packets_ok") - num(prev, "rp2040_packets_ok")
        crc_delta = num(j, "rp2040_packets_crc_bad") - num(prev, "rp2040_packets_crc_bad")
        gap_delta = num(j, "rp2040_sequence_gaps") - num(prev, "rp2040_sequence_gaps")
        lost_delta = num(j, "rp2040_firmware_lost_frames") - num(prev, "rp2040_firmware_lost_frames")

        connected = bool(j.get("rp2040_connected", False))
        healthy = bool(j.get("healthy", False))
        last_error = str(j.get("last_error", "") or "")
        flags = num(j, "rp2040_firmware_flags")

        if not connected:
            failures.append("rp2040 disconnected")
        if not healthy:
            failures.append("adc unhealthy")
        if crc_delta > 0:
            failures.append(f"crc increased by {crc_delta}")
        if gap_delta > 0:
            failures.append(f"sequence gaps increased by {gap_delta}")
        if lost_delta > 0:
            failures.append(f"firmware lost frames increased by {lost_delta}")
        if flags != 0:
            failures.append(f"firmware flags nonzero: 0x{flags:x}")

        print(
            f"{now-start_wall:7.1f}  {int(connected):4d} {num(j,'rp2040_packets_ok'):3d} "
            f"{int(healthy):6d} {num(j,'sample_rate_hz'):6d} {num(j,'rp2040_declared_rate_hz'):5d} "
            f"{num(j,'measured_sample_rate_hz'):7d} {num(j,'lifetime_sample_rate_hz'):7d} "
            f"{frames_delta/dt:8.1f} {packets_delta/dt:6.1f} {crc_delta:4d} {gap_delta:4d} "
            f"{lost_delta:5d} 0x{flags:04x} {last_error[:100]}",
            flush=True,
        )

        last = j
        prev = j
        prev_time = now
        samples += 1
        if time.time() >= deadline:
            break
        time.sleep(args.interval)

    if first is None or last is None:
        summary = {"ok": False, "error": "no samples", "failures": failures}
    else:
        elapsed = max(1e-9, time.time() - start_wall)
        summary = {
            "ok": len(failures) == 0,
            "elapsed_s": elapsed,
            "samples": samples,
            "adc_source": last.get("adc_source"),
            "requested_rate_hz": last.get("sample_rate_hz"),
            "declared_rate_hz": last.get("rp2040_declared_rate_hz"),
            "measured_rate_hz": last.get("measured_sample_rate_hz"),
            "lifetime_rate_hz": last.get("lifetime_sample_rate_hz"),
            "total_frames_delta": num(last, "total_frames") - num(first, "total_frames"),
            "packets_ok_delta": num(last, "rp2040_packets_ok") - num(first, "rp2040_packets_ok"),
            "crc_bad_delta": num(last, "rp2040_packets_crc_bad") - num(first, "rp2040_packets_crc_bad"),
            "sequence_gaps_delta": num(last, "rp2040_sequence_gaps") - num(first, "rp2040_sequence_gaps"),
            "firmware_lost_frames_delta": num(last, "rp2040_firmware_lost_frames") - num(first, "rp2040_firmware_lost_frames"),
            "firmware_flags": num(last, "rp2040_firmware_flags"),
            "last_error": last.get("last_error", ""),
            "failures": sorted(set(failures)),
        }

    if args.json_summary:
        print(json.dumps(summary, indent=2, sort_keys=True), flush=True)
    else:
        print("SUMMARY " + json.dumps(summary, sort_keys=True), flush=True)

    return 0 if args.no_fail or summary.get("ok") else 1


if __name__ == "__main__":
    sys.exit(main())
