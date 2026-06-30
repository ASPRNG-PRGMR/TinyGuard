#!/usr/bin/env python3
"""
validate.py — TinyGuard Validation (Phase 1 + Phase 3)
=========================================================
Sends crafted UDP heartbeat packets to the monitor to trigger each
detection scenario without needing to physically manipulate hardware.

Run this from your laptop while the monitor is running and has
completed its learning phase (5 minutes after first real heartbeat).

Monitor is located via mDNS (tinyguard-monitor.local).

Usage:
    python3 validate.py <scenario>
    python3 validate.py all              # Phase 1 scenarios only
    python3 validate.py all --phase3     # Phase 1 + Phase 3 scenarios

Phase 1 scenarios:
    normal          — 30 normal packets (baseline verification)
    rssi            — RSSI spike: 10 normal then 5 anomalous (-95 dBm)
    reconnect       — Reconnect spike: rapidly incrementing reconnect counter
    stream          — Stream anomaly: viewer count spike to 50
    heartbeat_stop  — Stop sending for 35s to trigger CRITICAL timeout

Phase 3 scenarios (require monitor warmup — see each docstring):
    stream_hijack   — stream_active=1, viewer_count=0 sustained (attack A3)
    cadence         — irregular 2s/18s heartbeat timing (attack A4)
    slow_drift      — RSSI drifts -42 to -80 over 60 packets (attack A5)
    combined        — simultaneous RSSI + reconnect anomaly (attack A6)

NOTE on RSSI<>Reconnect correlation pair:
    This pair will never fire against a camera with reconnects=0 always.
    Constant reconnect_rate has zero variance, making Pearson r undefined
    for that pair (logged once as a degenerate-pair warning on the monitor).
    This is expected behavior, not a bug — the pair needs natural variation
    in reconnect events to have a meaningful correlation to track.

The monitor's dashboard at http://tinyguard-monitor.local/ should show
alerts in real-time as each scenario runs.

Requirements: Python 3, no external packages needed.
"""

import socket
import json
import time
import sys
import argparse

import socket

def resolve_monitor():
    try:
        return socket.gethostbyname("tinyguard-monitor.local")
    except socket.gaierror:
        raise RuntimeError(
            "Could not resolve tinyguard-monitor.local.\n"
            "Ensure the monitor is connected and mDNS is working."
        )

MONITOR_IP = resolve_monitor()
INTERVAL     = 10.0   # match camera heartbeat interval

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

def send(uptime_ms, rssi, stream_active, viewer_count, reconnects):
    packet = json.dumps({
        "device":       "esp32cam",
        "uptime":       uptime_ms,
        "rssi":         rssi,
        "stream_active": stream_active,
        "viewer_count": viewer_count,
        "reconnects":   reconnects,
        "timestamp":    uptime_ms // 1000,
    })
    sock.sendto(packet.encode(), (MONITOR_IP, 5000))
    print(f"  → sent | uptime={uptime_ms}ms rssi={rssi} viewers={viewer_count} "
          f"reconnects={reconnects} stream={stream_active}")


def scenario_normal(start_uptime=10000, count=30):
    """30 normal packets. Confirms baseline is stable and no false positives."""
    print(f"\n[NORMAL] Sending {count} normal packets...")
    uptime = start_uptime
    for i in range(count):
        send(uptime, rssi=-42, stream_active=0, viewer_count=0, reconnects=0)
        uptime += 10000
        time.sleep(INTERVAL)
    return uptime


def scenario_rssi(start_uptime=10000):
    """
    10 normal packets to stabilise, then 5 packets with RSSI = -95 dBm.
    Expected: WARNING RSSI_ANOMALY after 3 consecutive anomalous packets.
    z = (-95 - (-42)) / ~1.2 ≈ -44 >> threshold of 3.0
    """
    print("\n[RSSI] 10 normal, then 5 anomalous RSSI packets...")
    uptime = start_uptime
    for i in range(10):
        send(uptime, rssi=-42, stream_active=0, viewer_count=0, reconnects=0)
        uptime += 10000
        time.sleep(INTERVAL)
    print("  >> Injecting anomalous RSSI (-95 dBm) ...")
    for i in range(5):
        send(uptime, rssi=-95, stream_active=0, viewer_count=0, reconnects=0)
        uptime += 10000
        time.sleep(INTERVAL)
    return uptime


def scenario_reconnect(start_uptime=10000):
    """
    10 normal packets, then rapidly increment reconnect counter by 20 per packet.
    Expected: WARNING RECONNECT_ANOMALY after 3 consecutive high-rate packets.
    Normal rate ≈ 0 reconnects/hr. Injected rate ≈ 7200 reconnects/hr.
    """
    print("\n[RECONNECT] 10 normal, then 5 high-reconnect packets...")
    uptime = start_uptime
    reconnects = 0
    for i in range(10):
        send(uptime, rssi=-42, stream_active=0, viewer_count=0, reconnects=reconnects)
        uptime += 10000
        time.sleep(INTERVAL)
    print("  >> Injecting rapid reconnects (+20 per packet) ...")
    for i in range(5):
        reconnects += 20
        send(uptime, rssi=-42, stream_active=0, viewer_count=0, reconnects=reconnects)
        uptime += 10000
        time.sleep(INTERVAL)
    return uptime


def scenario_stream(start_uptime=10000):
    """
    10 normal packets (0 viewers), then 5 packets with 50 viewers.
    Expected: WARNING STREAM_ANOMALY after 3 consecutive viewer spikes.
    """
    print("\n[STREAM] 10 normal (0 viewers), then 5 packets with 50 viewers...")
    uptime = start_uptime
    for i in range(10):
        send(uptime, rssi=-42, stream_active=0, viewer_count=0, reconnects=0)
        uptime += 10000
        time.sleep(INTERVAL)
    print("  >> Injecting viewer spike (50 viewers) ...")
    for i in range(5):
        send(uptime, rssi=-42, stream_active=1, viewer_count=50, reconnects=0)
        uptime += 10000
        time.sleep(INTERVAL)
    return uptime


def scenario_heartbeat_stop(start_uptime=10000):
    """
    5 normal packets then stop sending for 35 seconds.
    Expected: CRITICAL HEARTBEAT_MISSING within 30s of last packet.
    Resumes after timeout to verify INFO DEVICE_CONNECTED recovery.
    """
    print("\n[HEARTBEAT STOP] 5 normal, then 35s silence, then resume...")
    uptime = start_uptime
    for i in range(5):
        send(uptime, rssi=-42, stream_active=0, viewer_count=0, reconnects=0)
        uptime += 10000
        time.sleep(INTERVAL)
    print("  >> Stopping. Waiting 35s for CRITICAL alert ...")
    time.sleep(35)
    print("  >> Resuming heartbeats ...")
    for i in range(3):
        send(uptime, rssi=-42, stream_active=0, viewer_count=0, reconnects=0)
        uptime += 10000
        time.sleep(INTERVAL)
    return uptime




def scenario_stream_hijack(start_uptime=10000):
    """
    Attack A3 — Stream hijack simulation.
    stream_active=1 (stream running) but viewer_count=0 (viewers suppressed).
    Breaks the Stream<>Viewers correlation pair.
    Requires correlation_tracker warmed up (~23 min post-learning).
    Expected: CORRELATION_ANOMALY on Stream<>Viewers pair.
    """
    print("\n[STREAM_HIJACK] 10 normal, then 15 hijack packets (stream on, 0 viewers)...")
    print("  Requires: correlation_tracker warmed up (~23 min post-learning)")
    uptime = start_uptime
    for i in range(10):
        send(uptime, rssi=-42, stream_active=0, viewer_count=0, reconnects=0)
        uptime += 10000
        time.sleep(INTERVAL)
    print("  >> Injecting stream hijack (stream_active=1, viewer_count=0) ...")
    for i in range(15):
        send(uptime, rssi=-42, stream_active=1, viewer_count=0, reconnects=0)
        uptime += 10000
        time.sleep(INTERVAL)
    return uptime


def scenario_cadence(start_uptime=10000):
    """
    Attack A4 — Irregular heartbeat cadence.
    Alternates 2s / 18s delays against a baseline of 10s.
    Expected: HB_INTERVAL_ANOMALY WARNING.
    """
    print("\n[CADENCE] 10 normal, then 20 irregular-cadence packets (2s/18s alternating)...")
    uptime = start_uptime
    for i in range(10):
        send(uptime, rssi=-42, stream_active=0, viewer_count=0, reconnects=0)
        uptime += 10000
        time.sleep(INTERVAL)
    print("  >> Switching to irregular cadence ...")
    for i in range(20):
        delay = 2.0 if i % 2 == 0 else 18.0
        send(uptime, rssi=-42, stream_active=0, viewer_count=0, reconnects=0)
        uptime += int(delay * 1000)
        time.sleep(delay)
    return uptime


def scenario_slow_drift(start_uptime=10000):
    """
    Attack A5 — Slow RSSI drift.
    RSSI drifts from -42 to -80 over 60 packets (~0.63 dBm/packet).
    Phase 1 z-score should NOT fire (window mean follows the signal).
    behavior_profile drift detection should fire DRIFT_ANOMALY instead.
    Requires behavior_profile warmed up (~20 min post-learning).
    """
    print("\n[SLOW_DRIFT] RSSI drifting -42 -> -80 over 60 packets...")
    print("  Expected: DRIFT_ANOMALY WARNING (NOT RSSI_ANOMALY)")
    print("  Requires: behavior_profile warmed up (~20 min post-learning)")
    uptime = start_uptime
    start_rssi, end_rssi, steps = -42, -80, 60
    for i in range(steps):
        rssi = int(start_rssi + (end_rssi - start_rssi) * (i / steps))
        send(uptime, rssi=rssi, stream_active=0, viewer_count=0, reconnects=0)
        uptime += 10000
        time.sleep(INTERVAL)
    return uptime


def scenario_combined(start_uptime=10000):
    """
    Attack A6 — Combined simultaneous attack.
    RSSI drops to -95 AND reconnect rate spikes simultaneously.
    Expected: RSSI_ANOMALY + RECONNECT_ANOMALY + MULTI_METRIC_ANOMALY CRITICAL.
    """
    print("\n[COMBINED] 10 normal, then 10 dual-anomaly packets...")
    uptime = start_uptime
    reconnects = 0
    for i in range(10):
        send(uptime, rssi=-42, stream_active=0, viewer_count=0, reconnects=reconnects)
        uptime += 10000
        time.sleep(INTERVAL)
    print("  >> Injecting RSSI=-95 AND reconnect spike simultaneously ...")
    for i in range(10):
        reconnects += 20
        send(uptime, rssi=-95, stream_active=0, viewer_count=0, reconnects=reconnects)
        uptime += 10000
        time.sleep(INTERVAL)
    return uptime


def scenario_all(phase3=False):
    """
    Run all Phase 1 scenarios in sequence.
    If phase3=True, also runs Phase 3 scenarios after a warmup pause.
    Phase 3 scenarios (correlation, drift) require ~20-23 min of prior
    monitor uptime to be meaningful — running them immediately after
    Phase 1 scenarios on a freshly booted monitor will show them as
    "warming up" rather than producing real alerts.
    """
    print("=" * 60)
    print("TinyGuard Validation — Full Suite" + (" (Phase 1 + Phase 3)" if phase3 else " (Phase 1 only)"))
    print("Monitor dashboard: http://tinyguard-monitor.local")
    print("=" * 60)
    print("\nNOTE: Run this AFTER the monitor has completed learning (5 min).")
    print("Watch http://tinyguard-monitor.local/ during the run.\n")

    uptime = 10000
    uptime = scenario_normal(uptime, count=10)
    print("\n  [pause 10s between scenarios]"); time.sleep(10)

    uptime = scenario_rssi(uptime)
    print("\n  [pause 10s]"); time.sleep(10)

    uptime = scenario_reconnect(uptime)
    print("\n  [pause 10s]"); time.sleep(10)

    uptime = scenario_stream(uptime)
    print("\n  [pause 10s]"); time.sleep(10)

    uptime = scenario_heartbeat_stop(uptime)

    if phase3:
        print("\n  [pause 15s before Phase 3 scenarios]"); time.sleep(15)
        print("\n  NOTE: correlation_tracker and behavior_profile may still be")
        print("  warming up depending on total monitor uptime. Phase 3 scenarios")
        print("  are most meaningful after 20-23 min of continuous monitor operation.\n")

        uptime = scenario_stream_hijack(uptime)
        print("\n  [pause 15s]"); time.sleep(15)

        uptime = scenario_cadence(uptime)
        print("\n  [pause 15s]"); time.sleep(15)

        uptime = scenario_slow_drift(uptime)
        print("\n  [pause 15s]"); time.sleep(15)

        uptime = scenario_combined(uptime)

    print("\n" + "=" * 60)
    print("Validation complete. Check dashboard for alert history.")
    print("=" * 60)


def main():
    parser = argparse.ArgumentParser(description="TinyGuard validation injector")
    parser.add_argument("scenario",
                        choices=["normal","rssi","reconnect","stream",
                                 "heartbeat_stop","stream_hijack","cadence",
                                 "slow_drift","combined","all"],
                        help="Scenario to run")
    parser.add_argument("--phase3", action="store_true",
                        help="When used with 'all', also run Phase 3 attack "
                             "scenarios (stream_hijack, cadence, slow_drift, "
                             "combined) after the Phase 1 suite.")
    args = parser.parse_args()

    scenarios = {
        "normal":          scenario_normal,
        "rssi":            scenario_rssi,
        "reconnect":       scenario_reconnect,
        "stream":          scenario_stream,
        "heartbeat_stop":  scenario_heartbeat_stop,
        "stream_hijack":   scenario_stream_hijack,
        "cadence":         scenario_cadence,
        "slow_drift":      scenario_slow_drift,
        "combined":        scenario_combined,
    }

    try:
        if args.scenario == "all":
            scenario_all(phase3=args.phase3)
        else:
            scenarios[args.scenario]()
    except KeyboardInterrupt:
        print("\nAborted.")
    finally:
        sock.close()


if __name__ == "__main__":
    main()
